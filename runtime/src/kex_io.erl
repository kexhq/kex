-module(kex_io).
-export([print_line/1, print/1, print_error/1, read_line/0, inspect/1, to_string/1,
           to_string_bin/1, env_map/0]).

%% IO.printLine(x) — print x followed by a newline to stdout.
print_line(X) ->
    io:format("~ts~n", [to_string(X)]).

%% IO.print(x) — print x without a trailing newline.
print(X) ->
    io:format("~ts", [to_string(X)]).

%% IO.printError / IO.warn / IO.warning — print to stderr.
print_error(X) ->
    io:format(standard_error, "~ts~n", [to_string(X)]).

%% IO.readLine — read a line from stdin, returns a String (UTF-8 binary).
read_line() ->
    case io:get_line("") of
        eof -> none;
        {error, _} -> none;
        Line -> unicode:characters_to_binary(string:trim(Line, trailing, "\n"))
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
inspect(X) when is_binary(X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ ?GREEN ++ "\"~ts\"" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "String" ++ ?RESET ++ "~n", [X]), X;
inspect([{'Char', _} | _] = X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ ?GREEN ++ "\"~ts\"" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "String" ++ ?RESET ++ "~n", [to_string(X)]), X;
inspect(X) when is_list(X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ "~p"
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "List" ++ ?RESET ++ "~n", [X]), X;
inspect({'Char', C}) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ ?GREEN ++ "'~ts'" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Char" ++ ?RESET ++ "~n", [[C]]), {'Char', C};
inspect({'Just', {'Char', C}} = X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ ?GREEN ++ "Just("
              ++ [C] ++ ")" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Char" ++ ?RESET ++ "~n"), X;
inspect(X) when is_tuple(X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ "~p"
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Tuple" ++ ?RESET ++ "~n", [X]), X;
inspect(X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ "~p~n", [X]), X.

%% Any Kex value as a Kex String VALUE (UTF-8 binary) — what `.to(String)`
%% and toString-style conversions return. to_string/1 below stays a charlist
%% because its output feeds io:format/iolists, not user code.
to_string_bin(X) when is_binary(X) -> X;
to_string_bin(X) -> unicode:characters_to_binary(to_string(X)).

%% Internal: convert any Kex value to a printable charlist.
% Nested elements (inside a List/Tuple/Map) use exactly the same to_string
% recursively — NO quoting of nested strings — matching
% src/interpreter/value.cxx's ListValue/TupleValue toString exactly (both
% call element->toString(), the same unquoted convention, not a separate
% "inspect"-style quoted rendering). A real, reproduced bug otherwise: this
% used to have a separate format_value/1 with its own quoting logic for
% nested strings, printing `["abcd"]` instead of the expected `[abcd]`
% (spec/io_ops.kex), and `{Underscore,"wooo"}` (Erlang tuple syntax, quoted,
% no spaces) instead of `(Underscore, wooo)` (Kex's own tuple syntax:
% parens, comma-space, unquoted) — spec/my_starts_with.kex.
% A Kex String is a UTF-8 binary and a Char is {'Char', N}, so [] is
% unambiguously an empty LIST ("[]"), an [Int] is unambiguously a list of
% numbers, and a [Char] — which IS String in Kex — displays as text. No
% printable-list heuristic remains.
to_string(X) when is_binary(X)  -> unicode:characters_to_list(X);
to_string({'Char', C})          -> [C];
to_string([{'Char', _} | _] = L) ->
    [C || {'Char', C} <- L];
to_string(X) when is_list(X) ->
    "[" ++ lists:flatten(lists:join(", ", lists:map(fun to_string/1, X))) ++ "]";
% Kex's None is capitalized at the source level (an UpperIdentifier, like
% the interpreter's NoneValue prints as "None") — every other atom (Kex's
% own lowercase :atoms, true/false) prints as its literal name, so this
% needs its own clause before the general is_atom one below. A real,
% reproduced bug otherwise: IO.printLine(None) printed "none" under BEAM
% but "None" under the tree-walker.
to_string('none')               -> "None";
to_string('true')               -> "true";
to_string('false')              -> "false";
% A Kex `:atom` literal (always lowercase-first — see the lexer) prints
% WITH its leading colon, matching src/interpreter/value.cxx's AtomValue
% toString (`":" + name`). Capitalized atoms are instead ADT nullary
% constructors (Just/Ok/Less/JsonNull/...) or type names, which print bare
% — so first-letter case is what distinguishes the two here. true/false
% (Kex booleans, handled just above) are the only lowercase atoms that are
% NOT `:atoms` and so are excluded. A real, reproduced bug otherwise: an
% atom argument interpolated into a string printed as `database` instead
% of `:database` (spec/optional_parens_do.kex).
to_string(X) when is_atom(X) ->
    S = atom_to_list(X),
    case S of
        [C | _] when C >= $a, C =< $z -> [$: | S];
        _ -> S
    end;
to_string(X) when is_integer(X) -> integer_to_list(X);
to_string(X) when is_float(X)   -> format_float(X);
to_string(X) when is_map(X)     ->
    Pairs = [to_string(K) ++ " => " ++ to_string(V) || {K, V} <- maps:to_list(X)],
    "#{" ++ lists:flatten(lists:join(", ", Pairs)) ++ "}";
% A plain Kex Tuple (structural pairing, e.g. `(Underscore, rest)`) and a
% prelude ADT variant with a payload (e.g. Just(1)) are BOTH just an
% Erlang {Atom, Args...} tuple — genuinely indistinguishable by value alone
% (same class of ambiguity as the Char/Integer and empty-list/empty-string
% cases elsewhere in this file). Reserved prelude tags get "Tag(args)"
% rendering (matching src/interpreter/value.cxx's VariantValue toString);
% anything else falls to plain "(a, b, ...)" tuple rendering (matching
% TupleValue toString) — this doesn't generalize to arbitrary user-defined
% ADT tags used as a tuple's first element, but covers the tags that
% actually appear from Kex's own prelude constructors.
to_string(X) when is_tuple(X), tuple_size(X) >= 1,
                   (element(1, X) =:= 'Just' orelse element(1, X) =:= 'Ok' orelse
                    element(1, X) =:= 'Error' orelse element(1, X) =:= 'Some') ->
    [Tag | Args] = tuple_to_list(X),
    Parts = [to_string(E) || E <- Args],
    atom_to_list(Tag) ++ "(" ++ lists:flatten(lists:join(", ", Parts)) ++ ")";
to_string(X) when is_tuple(X)   ->
    Parts = [to_string(E) || E <- tuple_to_list(X)],
    "(" ++ lists:flatten(lists:join(", ", Parts)) ++ ")";
to_string(X)                    -> lists:flatten(io_lib:format("~p", [X])).

%% Float display — matches src/interpreter/value.cxx's toString exactly:
%% format with 6 decimal places (not Erlang's ~g, which picks a variable,
%% shorter number of significant digits and produced real mismatches —
%% "3.14159" vs "3.141593", "6.28000"/"4.00000" vs "6.28"/"4.0"), then trim
%% trailing zeros, keeping at least one digit after the decimal point.
format_float(X) ->
    S = lists:flatten(io_lib:format("~.6f", [X])),
    case string:split(S, ".") of
        [Whole, Frac] ->
            Trimmed = string:trim(Frac, trailing, "0"),
            case Trimmed of
                "" -> Whole ++ ".0";
                _  -> Whole ++ "." ++ Trimmed
            end;
        _ -> S
    end.


%% ENV — a real Map<String,String> value (see
%% src/interpreter/stdlib/env.cxx), a snapshot of the process environment.
%% Built fresh on every reference (matches how ENV is compiled — see
%% core_erlang.cxx's UpperIdentifier handling) rather than once at startup;
%% cheap enough, and avoids needing a global to seed.
env_map() ->
    maps:from_list([split_env_entry(E) || E <- os:getenv()]).

split_env_entry(E) ->
    case string:split(E, "=") of
        [K, V] -> {unicode:characters_to_binary(K), unicode:characters_to_binary(V)};
        [K]    -> {unicode:characters_to_binary(K), <<>>}
    end.
