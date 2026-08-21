%% Kex.AST — parse through the compiler so BEAM programs consume the same AST
%% as the tree-walk interpreter. The compiler emits Kex's native BEAM value
%% representation as ETF; no second schema decoder lives here.
-module(kex_intrinsic_ast).
-export([parse/1, parse/2, 'parseFile'/1, 'parseType'/1,
         'parseExpression'/1]).

parse(Source) -> parse(Source, <<"<string>">>).

parse(Source, Filename) when is_binary(Source), is_binary(Filename) ->
    Path = temporary_path(),
    case file:write_file(Path, Source) of
        ok ->
            Result = run_kex(Path, Filename),
            file:delete(Path),
            Result;
        {error, Reason} ->
            parse_error(iolist_to_binary(
                io_lib:format("cannot create parser input: ~p", [Reason])))
    end;
parse(_, _) -> parse_error(<<"parse requires a source string">>).

'parseFile'(Path) when is_binary(Path) -> run_kex(binary_to_list(Path), none);
'parseFile'(Path) when is_list(Path) -> run_kex(Path, none);
'parseFile'(_) -> parse_error(<<"parseFile requires a file path">>).

'parseType'(Source) when is_binary(Source) ->
    Wrapped = <<"module T do\nvalue : ", Source/binary, "\nend">>,
    case parse(Wrapped, <<"<type>">>) of
        {'Ok', {'Kex.AST.Program', _, [
            {'ModuleDef', {'Kex.AST.ModuleInfo', _, _, [
                {'TypeAnnotation', {'Kex.AST.AnnotationInfo', _, Type, _, _}}
            ], _}}
        ]}} ->
            {'Ok', Type};
        {'Error', _} = Error -> Error;
        _ -> parse_error(<<"failed to parse type expression">>)
    end;
'parseType'(_) -> parse_error(<<"parseType requires a type string">>).

'parseExpression'(Source) when is_binary(Source) ->
    case parse(Source, <<"<expression>">>) of
        {'Ok', {'Kex.AST.Program', _,
            [{'MainDef', {'Kex.AST.MainInfo', _, _, [Expression | _], _}}]}} ->
            {'Ok', Expression};
        {'Error', _} = Error -> Error;
        _ -> parse_error(<<"failed to parse expression">>)
    end;
'parseExpression'(_) ->
    parse_error(<<"parseExpression requires a source string">>).

run_kex(Path, Filename) ->
    case executable() of
        {error, Message} -> parse_error(Message);
        {ok, Executable} ->
            Args = case Filename of
                       none -> ["--emit-ast", "--no-colors", Path];
                       _ -> ["--emit-ast", "--no-colors", "--ast-filename",
                             binary_to_list(Filename), Path]
                   end,
            Port = open_port({spawn_executable, Executable},
                             [binary, exit_status, use_stdio, stderr_to_stdout,
                              hide,
                              {args, Args}]),
            case collect(Port, []) of
                {0, Bytes} ->
                    try case binary_to_term(Bytes) of
                            {'Error', _} = Error -> Error;
                            Term -> {'Ok', Term}
                        end
                    catch _:_ -> parse_error(<<"kex returned invalid AST data">>)
                    end;
                {Status, <<>>} ->
                    parse_error(iolist_to_binary(
                        io_lib:format("kex exited with status ~B", [Status])));
                {Status, Bytes} -> compiler_error(Status, Bytes, Executable)
            end
    end.

executable() ->
    case os:getenv("KEX") of
        false ->
            case os:find_executable("kex") of
                false -> {error, <<"kex executable not found; set $KEX or add kex to PATH">>};
                Path -> {ok, Path}
            end;
        "" ->
            case os:find_executable("kex") of
                false -> {error, <<"kex executable not found; set $KEX or add kex to PATH">>};
                Path -> {ok, Path}
            end;
        Path -> {ok, Path}
    end.

collect(Port, Chunks) ->
    receive
        {Port, {data, Data}} -> collect(Port, [Data | Chunks]);
        {Port, {exit_status, Status}} ->
            {Status, iolist_to_binary(lists:reverse(Chunks))}
    end.

temporary_path() ->
    Directory = case os:getenv("TMPDIR") of
                    false -> "/tmp";
                    "" -> "/tmp";
                    Value -> Value
                end,
    Name = "kex_ast_" ++ os:getpid() ++ "_" ++
           integer_to_list(erlang:unique_integer([positive])) ++ ".kex",
    filename:join(Directory, Name).

compiler_error(Status, Bytes, Executable) ->
    FirstLine = hd(binary:split(Bytes, <<"\n">>, [global])),
    Message = iolist_to_binary(io_lib:format(
        "AST parser command failed (~B, ~ts): ~ts",
        [Status, Executable, FirstLine])),
    parse_error(Message).

parse_error(Message) ->
    {'Error', {'Kex.AST.ParseError', Message, 'None'}}.
