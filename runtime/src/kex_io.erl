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

%% IO.inspect — print "=> <value> : <Type>" with ANSI colours, returns value.
-define(GRAY,  "\e[90m").
-define(RESET, "\e[0m").
-define(CYAN,  "\e[36m").
-define(YELL,  "\e[33m").
-define(GREEN, "\e[32m").
-define(WHITE, "\e[97m").

inspect(X) when is_integer(X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ ?YELL ++ "~p" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Int" ++ ?RESET ++ "~n", [X]), X;
inspect(X) when is_float(X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ ?YELL ++ "~g" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Float" ++ ?RESET ++ "~n", [X]), X;
inspect(true) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ ?YELL ++ "true" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Bool" ++ ?RESET ++ "~n"), true;
inspect(false) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ ?YELL ++ "false" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Bool" ++ ?RESET ++ "~n"), false;
inspect(none) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ ?WHITE ++ "None" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Option" ++ ?RESET ++ "~n"), none;
inspect(X) when is_atom(X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ ":" ++ atom_to_list(X)
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Atom" ++ ?RESET ++ "~n"), X;
inspect(X) when is_list(X) ->
    case io_lib:printable_list(X) of
        true ->
            io:format(?GRAY ++ "=> " ++ ?RESET ++ ?GREEN ++ "\"~s\"" ++ ?RESET
                      ++ " " ++ ?GRAY ++ ":" ++ ?RESET
                      ++ " " ++ ?CYAN ++ "String" ++ ?RESET ++ "~n", [X]);
        false ->
            io:format(?GRAY ++ "=> " ++ ?RESET ++ "~p"
                      ++ " " ++ ?GRAY ++ ":" ++ ?RESET
                      ++ " " ++ ?CYAN ++ "List" ++ ?RESET ++ "~n", [X])
    end, X;
inspect(X) when is_tuple(X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ "~p"
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Tuple" ++ ?RESET ++ "~n", [X]), X;
inspect(X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ "~p~n", [X]), X.

%% Internal: convert any Kex value to a printable charlist.
to_string(X) when is_list(X)    -> X;
to_string(X) when is_binary(X)  -> binary_to_list(X);
to_string(X) when is_atom(X)    -> atom_to_list(X);
to_string(X) when is_integer(X) -> integer_to_list(X);
to_string(X) when is_float(X)   -> io_lib:format("~g", [X]);
to_string(X) when is_tuple(X)   -> io_lib:format("~p", [X]);
to_string(X)                    -> io_lib:format("~p", [X]).
