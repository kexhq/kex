-module(kex_file).
-export([exists/1, lines/1, read/1, write/2, append/2, size/1, delete/1]).

%% File.exists?(path) → true | false
exists(Path) ->
    filelib:is_regular(Path).

%% File.lines(path) → [String] | 'none'
lines(Path) ->
    case file:read_file(Path) of
        {ok, Bin} ->
            Content = binary_to_list(Bin),
            Lines = string:split(Content, "\n", all),
            % Drop trailing empty line from final newline
            case lists:reverse(Lines) of
                [""|Rest] -> lists:reverse(Rest);
                _         -> Lines
            end;
        _ -> none
    end.

%% File.read(path) → String | 'none'
read(Path) ->
    case file:read_file(Path) of
        {ok, Bin} -> binary_to_list(Bin);
        _         -> none
    end.

%% File.write(path, content) → true | false
write(Path, Content) ->
    case file:write_file(Path, Content) of
        ok    -> true;
        _     -> false
    end.

%% File.append(path, content) → true | false
append(Path, Content) ->
    case file:write_file(Path, Content, [append]) of
        ok    -> true;
        _     -> false
    end.

%% File.size(path) → Int | 'none'
size(Path) ->
    case file:read_file_info(Path) of
        {ok, Info} -> element(2, Info);  %% #file_info.size is index 2
        _          -> none
    end.

%% File.delete(path) → true | false
delete(Path) ->
    case file:delete(Path) of
        ok -> true;
        _  -> false
    end.
