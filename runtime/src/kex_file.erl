%% kex_file — File / Directory / FileHandle / Mock.FS runtime.
%%
%% Mirrors src/interpreter/stdlib/file.cxx. Paths and contents are Kex
%% Strings (UTF-8 binaries). Mock.FS registers an in-memory filesystem in
%% the process dictionary (tests run single-process), consulted by the
%% read-side ops before touching the real filesystem — same layering as the
%% walker's m_mockFiles/m_mockDirs.
-module(kex_file).
-export([exists/1, lines/1, read/1, write/2, append/2, size/1, delete/1, feed/1,
         open/2, open/3,
         basename/1, dirname/1, extension/1, join/2, absolute/1,
         'file?'/1, 'directory?'/1, copy/2, rename/2,
         handle_getLine/1, handle_get/1,
         handle_printLine/2, handle_print/2,
         handle_read/1, handle_write/2,
         'handle_atEnd?'/1, handle_close/1,
         dir_current/0, dir_home/0, dir_create/1, dir_delete/1, dir_delete_all/1,
         dir_list/1, dir_files/1, dir_directories/1, 'dir_exists?'/1, 'dir_file?'/1,
         mock_file/2, mock_dir/1, mock_clear/0]).

%% ── Mock.FS registry ────────────────────────────────────────────────────
mock_file(Path, Content) ->
    P = pth(Path),
    put({kex_mock_file, P}, kex_io:to_string_bin(Content)),
    put(kex_mock_files, lists:usort([P | plist(kex_mock_files)])),
    ok.

mock_dir(Path) ->
    put(kex_mock_dirs, lists:usort([pth(Path) | plist(kex_mock_dirs)])),
    ok.

mock_clear() ->
    [erase({kex_mock_file, P}) || P <- plist(kex_mock_files)],
    erase(kex_mock_files),
    erase(kex_mock_dirs),
    ok.

plist(K) -> case get(K) of undefined -> []; L -> L end.
mock_content(P) -> get({kex_mock_file, pth(P)}).
mocked_dir(P) -> lists:member(pth(P), plist(kex_mock_dirs)).

%% Normalize any Kex String shape (binary or [Char]) to a binary path.
pth(P) -> kex_io:to_string_bin(P).

%% ── File ────────────────────────────────────────────────────────────────
exists(Path) ->
    case mock_content(Path) of
        undefined -> filelib:is_regular(pth(Path));
        _ -> true
    end.

'file?'(Path) -> exists(Path).
'directory?'(Path) ->
    mocked_dir(Path) orelse filelib:is_dir(pth(Path)).

%% File.lines(path) → [String] | 'None'
lines(Path) ->
    case mock_content(Path) of
        undefined ->
            case file:read_file(pth(Path)) of
                {ok, Bin} -> split_lines(Bin);
                _ -> 'None'
            end;
        C -> split_lines(C)
    end.

split_lines(Bin) ->
    Lines = string:split(Bin, <<"\n">>, all),
    % Drop trailing empty line from final newline
    case lists:reverse(Lines) of
        [<<>>|Rest] -> lists:reverse(Rest);
        _           -> Lines
    end.

%% File.read(path) → String | 'None'
read(Path) ->
    case mock_content(Path) of
        undefined ->
            case file:read_file(pth(Path)) of
                {ok, Bin} -> Bin;
                _         -> 'None'
            end;
        C -> C
    end.

%% File.write(path, content) → true | false
write(Path, Content) ->
    case mock_content(Path) of
        undefined ->
            case file:write_file(pth(Path), kex_io:to_string_bin(Content)) of
                ok -> true;
                _  -> false
            end;
        _ ->
            put({kex_mock_file, pth(Path)}, kex_io:to_string_bin(Content)),
            true
    end.

%% File.append(path, content) → true | false
append(Path, Content) ->
    case mock_content(Path) of
        undefined ->
            case file:write_file(pth(Path), kex_io:to_string_bin(Content), [append]) of
                ok -> true;
                _  -> false
            end;
        C ->
            put({kex_mock_file, pth(Path)},
                <<C/binary, (kex_io:to_string_bin(Content))/binary>>),
            true
    end.

%% File.size(path) → Int | 'None'
size(Path) ->
    case mock_content(Path) of
        undefined ->
            case file:read_file_info(pth(Path)) of
                {ok, Info} -> element(2, Info);  %% #file_info.size is index 2
                _          -> 'None'
            end;
        C -> byte_size(C)
    end.

%% File.delete(path) → true | false
delete(Path) ->
    case mock_content(Path) of
        undefined ->
            case file:delete(pth(Path)) of
                ok -> true;
                _  -> false
            end;
        _ ->
            erase({kex_mock_file, pth(Path)}),
            put(kex_mock_files, lists:delete(pth(Path), plist(kex_mock_files))),
            true
    end.

%% File.copy / File.rename → true | false
copy(From, To) ->
    case file:copy(pth(From), pth(To)) of
        {ok, _} -> true;
        _       -> false
    end.

rename(From, To) ->
    case file:rename(pth(From), pth(To)) of
        ok -> true;
        _  -> false
    end.

%% File.feed(path) → [String] | 'None' — lazy-stream placeholder; returns the
%% same eager list as lines/1.
feed(Path) -> lines(Path).

%% File.open(path, mode) -> FileHandle (crashes on failure)
open(Path, Mode) ->
    case file:open(pth(Path), mode_flags(Mode)) of
        {ok, Dev} -> {'FileHandle', Dev, pth(Path)};
        {error, Reason} ->
            error({file_open_error, pth(Path), Reason})
    end.

%% File.open(path, mode, block) -> T? (None if can't open)
open(Path, Mode, Fun) ->
    case file:open(pth(Path), mode_flags(Mode)) of
        {ok, Dev} ->
            try Fun({'FileHandle', Dev, pth(Path)})
            after file:close(Dev)
            end;
        _ -> 'None'
    end.

mode_flags('Read')      -> [read, binary];
mode_flags('Write')     -> [write, binary];
mode_flags('Append')    -> [append, binary];
mode_flags('ReadWrite') -> [read, write, binary];
mode_flags(_)           -> [read, binary].

%% ── FileHandle methods (receiver-first UFCS) ────────────────────────────
%% Text I/O — mirrors IO.getLine / IO.get / IO.printLine / IO.print.

%% getLine → String? (newline stripped, 'None' at EOF).
handle_getLine({'FileHandle', Dev, _}) ->
    case file:read_line(Dev) of
        {ok, Line} -> string:trim(Line, trailing, "\n");
        _          -> 'None'
    end;
handle_getLine(_) -> 'None'.

%% get → String? (single character, 'None' at EOF).
handle_get({'FileHandle', Dev, _}) ->
    case file:read(Dev, 1) of
        {ok, <<C>>} -> <<C>>;
        _           -> 'None'
    end;
handle_get(_) -> 'None'.

%% printLine(content) → Bool (write + newline).
handle_printLine({'FileHandle', Dev, _}, Content) ->
    file:write(Dev, [kex_io:to_string_bin(Content), $\n]) =:= ok;
handle_printLine(_, _) -> false.

%% print(content) → Bool (write, no newline).
handle_print({'FileHandle', Dev, _}, Content) ->
    file:write(Dev, kex_io:to_string_bin(Content)) =:= ok;
handle_print(_, _) -> false.

%% Binary/raw I/O.

%% read → String? (remaining content).
handle_read({'FileHandle', Dev, _}) ->
    read_rest(Dev, <<>>);
handle_read(_) -> 'None'.

read_rest(Dev, Acc) ->
    case file:read(Dev, 65536) of
        {ok, Data} -> read_rest(Dev, <<Acc/binary, Data/binary>>);
        eof        -> Acc;
        _          -> Acc
    end.

%% write(data) → Bool (raw write).
handle_write({'FileHandle', Dev, _}, Content) ->
    file:write(Dev, kex_io:to_string_bin(Content)) =:= ok;
handle_write(_, _) -> false.

%% Lifecycle.

%% atEnd? → Bool.
'handle_atEnd?'({'FileHandle', Dev, _}) ->
    {ok, Pos} = file:position(Dev, cur),
    {ok, End} = file:position(Dev, eof),
    {ok, _} = file:position(Dev, {bof, Pos}),
    Pos >= End;
'handle_atEnd?'(_) -> true.

%% close → Unit.
handle_close({'FileHandle', Dev, _}) ->
    file:close(Dev),
    ok;
handle_close(_) -> ok.

%% ── Path utilities ──────────────────────────────────────────────────────
basename(P)  -> to_bin(filename:basename(pth(P))).
dirname(P)   -> to_bin(filename:dirname(pth(P))).
extension(P) -> to_bin(filename:extension(pth(P))).
join(A, B)   -> to_bin(filename:join(pth(A), pth(B))).
absolute(P)  -> to_bin(filename:absname(pth(P))).

to_bin(X) when is_binary(X) -> X;
to_bin(X) -> unicode:characters_to_binary(X).

%% ── Directory ───────────────────────────────────────────────────────────
dir_current() ->
    case file:get_cwd() of
        {ok, Cwd} -> to_bin(Cwd);
        _         -> <<>>
    end.

dir_home() ->
    case os:getenv("HOME") of
        false -> 'None';
        Home  -> to_bin(Home)
    end.

'dir_exists?'(P) -> mocked_dir(P) orelse filelib:is_dir(pth(P)).
'dir_file?'(P)   -> exists(P).

dir_create(P) ->
    case file:make_dir(pth(P)) of
        ok -> true;
        _  -> false
    end.

dir_delete(P) ->
    case file:del_dir(pth(P)) of
        ok -> true;
        _  -> false
    end.

dir_delete_all(P) ->
    case file:del_dir_r(pth(P)) of
        ok -> true;
        _  -> false
    end.

%% list/files/directories → Just([String]) | 'None'. Mocked directories list
%% their registered direct children.
dir_list(P) ->
    case mocked_dir(P) of
        true ->
            {'Just', mock_children(P, plist(kex_mock_files)) ++
                     mock_children(P, plist(kex_mock_dirs))};
        false ->
            case file:list_dir(pth(P)) of
                {ok, Names} -> {'Just', lists:sort([to_bin(N) || N <- Names])};
                _           -> 'None'
            end
    end.

dir_files(P) ->
    case mocked_dir(P) of
        true -> {'Just', mock_children(P, plist(kex_mock_files))};
        false ->
            Dir = pth(P),
            case file:list_dir(Dir) of
                {ok, Names} ->
                    {'Just', lists:sort([to_bin(N) || N <- Names,
                        filelib:is_regular(filename:join(Dir, N))])};
                _ -> 'None'
            end
    end.

dir_directories(P) ->
    case mocked_dir(P) of
        true -> {'Just', mock_children(P, plist(kex_mock_dirs))};
        false ->
            Dir = pth(P),
            case file:list_dir(Dir) of
                {ok, Names} ->
                    {'Just', lists:sort([to_bin(N) || N <- Names,
                        filelib:is_dir(filename:join(Dir, N))])};
                _ -> 'None'
            end
    end.

%% Direct children of a mocked dir among the registered paths, as basenames.
mock_children(Dir, Paths) ->
    D = pth(Dir),
    Prefix = <<D/binary, "/">>,
    Sz = byte_size(Prefix),
    lists:sort([basename(Pth) || Pth <- Paths,
        Pth =/= D,
        byte_size(Pth) > Sz,
        binary:part(Pth, 0, Sz) =:= Prefix,
        binary:match(Pth, <<"/">>, [{scope, {Sz, byte_size(Pth) - Sz}}]) =:= nomatch]).
