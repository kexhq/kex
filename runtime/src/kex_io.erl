-module(kex_io).
-export([print_line/1, print/1, print_error/1, read_line/0, inspect/1, to_string/1]).

%% IO.printLine(x) — print x followed by a newline to stdout.
print_line(X) ->
    io:format("~s~n", [to_string(X)]).

%% IO.print(x) — print x without a trailing newline.
print(X) ->
    io:format("~s", [to_string(X)]).

%% IO.printError / IO.warn / IO.warning — print to stderr.
print_error(X) ->
    io:format(standard_error, "~s~n", [to_string(X)]).

%% IO.readLine — read a line from stdin, returns a charlist.
read_line() ->
    case io:get_line("") of
        eof -> none;
        {error, _} -> none;
        Line -> string:trim(Line, trailing, "\n")
    end.

%% IO.inspect — print a debug representation.
inspect(X) ->
    io:format("~p~n", [X]),
    X.

%% Internal: convert any Kex value to a printable charlist.
to_string(X) when is_list(X)    -> X;
to_string(X) when is_binary(X)  -> binary_to_list(X);
to_string(X) when is_atom(X)    -> atom_to_list(X);
to_string(X) when is_integer(X) -> integer_to_list(X);
to_string(X) when is_float(X)   -> io_lib:format("~g", [X]);
to_string(X) when is_tuple(X)   -> io_lib:format("~p", [X]);
to_string(X)                    -> io_lib:format("~p", [X]).
