%% Kex.Evaluator — run Kex source at run time, in a sandbox. BEAM has no
%% evaluator of its own, so a call goes to the compiler's tree-walk one, as
%% kex_intrinsic_ast goes to its parser: the request is written as ETF to a
%% temporary file, `kex --eval-etf` evaluates it under the same limits the
%% walker's Evaluator.run applies, and writes back the `Ok(value)` or
%% `Error(message)` in the value representation BEAM programs use.
%%
%% What the evaluated code prints reaches this program's standard output, as
%% it would under the walker. EvaluatorOptions.modules cannot come along: its
%% functions are closures of this VM, and the evaluator runs in another
%% process, so a call that lends any answers an Error instead.
-module(kex_intrinsic_evaluator).
-export([run/1, run/2, runExpression/1, runExpression/2]).

-define(OPTIONS, 'Evaluator.EvaluatorOptions').

run(Source) -> evaluate(Source, false, defaults()).
run(Source, Options) -> evaluate(Source, false, Options).

runExpression(Source) -> evaluate(Source, true, defaults()).
runExpression(Source, Options) -> evaluate(Source, true, Options).

%% EvaluatorOptions' defaults (src/stdlib/evaluator.kex).
defaults() ->
    {?OPTIONS, ['Math', 'List', 'String', 'Integer', 'Map', 'Stream'], #{}, 1000000, 256}.

evaluate(Source, Expression, {?OPTIONS, Allow, Modules, MaxSteps, MaxDepth})
  when is_binary(Source) ->
    {?OPTIONS, DefaultAllow, _, DefaultSteps, DefaultDepth} = defaults(),
    case lends(Modules) of
        true ->
            {'Error', <<"EvaluatorOptions.modules cannot be lent to the evaluator on BEAM: "
                        "its functions cannot leave this program">>};
        false ->
            Request = #{source => Source,
                        expression => Expression,
                        allow => given(Allow, DefaultAllow),
                        maxSteps => given(MaxSteps, DefaultSteps),
                        maxDepth => given(MaxDepth, DefaultDepth)},
            request(Request)
    end;
evaluate(Source, _, _) when is_binary(Source) ->
    {'Error', <<"Evaluator.run requires EvaluatorOptions">>};
evaluate(_, _, _) ->
    {'Error', <<"Evaluator.run requires a source string">>}.

lends(Modules) when is_map(Modules) -> map_size(Modules) > 0;
lends(_) -> false.

%% A field the record left unset keeps its default.
given('None', Default) -> Default;
given(Value, _) -> Value.

request(Request) ->
    Input = temporary_path("in"),
    Output = temporary_path("out"),
    Result =
        case file:write_file(Input, term_to_binary(Request#{output => list_to_binary(Output)})) of
            ok -> run_kex(Input, Output);
            {error, Reason} ->
                {'Error', iolist_to_binary(
                    io_lib:format("cannot create evaluator input: ~p", [Reason]))}
        end,
    file:delete(Input),
    file:delete(Output),
    Result.

run_kex(Input, Output) ->
    case executable() of
        {error, Message} -> {'Error', Message};
        {ok, Executable} ->
            Port = open_port({spawn_executable, Executable},
                             [binary, exit_status, use_stdio, stderr_to_stdout, hide,
                              {args, ["--eval-etf", Input]}]),
            {Status, Printed} = collect(Port, []),
            %% What the evaluated code printed, where the walker prints it.
            Printed =/= <<>> andalso io:put_chars(Printed),
            case {Status, file:read_file(Output)} of
                {0, {ok, Bytes}} ->
                    try binary_to_term(Bytes)
                    catch _:_ -> {'Error', <<"kex returned an invalid evaluator result">>}
                    end;
                _ ->
                    {'Error', iolist_to_binary(
                        io_lib:format("evaluator command failed (~B, ~ts)", [Status, Executable]))}
            end
    end.

executable() ->
    case os:getenv("KEX") of
        Path when is_list(Path), Path =/= "" -> {ok, Path};
        _ ->
            case os:find_executable("kex") of
                false -> {error, <<"kex executable not found; set $KEX or add kex to PATH">>};
                Path -> {ok, Path}
            end
    end.

collect(Port, Chunks) ->
    receive
        {Port, {data, Data}} -> collect(Port, [Data | Chunks]);
        {Port, {exit_status, Status}} ->
            {Status, iolist_to_binary(lists:reverse(Chunks))}
    end.

temporary_path(Suffix) ->
    Directory = case os:getenv("TMPDIR") of
                    false -> "/tmp";
                    "" -> "/tmp";
                    Value -> Value
                end,
    Name = "kex_eval_" ++ os:getpid() ++ "_" ++
           integer_to_list(erlang:unique_integer([positive])) ++ "." ++ Suffix,
    filename:join(Directory, Name).
