-module(kex_io).
-export([print_line/1, print/1, print_error/1, read_line/0, inspect/1, to_string/1, add/2,
         list_get/2, list_get/3, env_map/0, integer_parse/1, float_parse/1,
         math_e/0, math_log/1, math_log/2, math_hypot/2, math_cbrt/1,
         assert/1, assert/2, index_of/2, list_product/1,
         is_digit/1, is_alpha/1, is_space/1, divide/2]).

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
% An empty Kex list and an empty Kex string are both just Erlang's [] —
% genuinely indistinguishable at this point by value alone
% (io_lib:printable_unicode_list([]) is vacuously true). Tried special-
% casing to_string([]) -> "[]" to fix spec/env.kex's `args: ${args}`
% (an empty [String] prints "args: " instead of "args: []"), but that
% directly regressed spec/my_capitalize.kex (an empty STRING result needs
% to print blank, not "[]") — a genuine, irreconcilable-by-value ambiguity
% between these two specs. Left as a known limitation (blank wins, since
% it's the more common case) rather than fixing one at the other's expense.
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
to_string(X) when is_list(X) ->
    case io_lib:printable_unicode_list(X) of
        true  -> X;
        false ->
            "[" ++ lists:flatten(lists:join(", ", lists:map(fun to_string/1, X))) ++ "]"
    end;
to_string(X) when is_binary(X)  -> binary_to_list(X);
% Kex's None is capitalized at the source level (an UpperIdentifier, like
% the interpreter's NoneValue prints as "None") — every other atom (Kex's
% own lowercase :atoms, true/false) prints as its literal name, so this
% needs its own clause before the general is_atom one below. A real,
% reproduced bug otherwise: IO.printLine(None) printed "none" under BEAM
% but "None" under the tree-walker.
to_string('none')               -> "None";
to_string(X) when is_atom(X)    -> atom_to_list(X);
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

%% divide/2 — polymorphic /: integer division (truncating, like Erlang's
%% own div) when both operands are integers, float division otherwise.
%% Matches src/interpreter/evaluator.cxx's BinaryOp::Slash case exactly —
%% used by curried `~(/)` references (see core_erlang.cxx's CurryExpr
%% handling); ordinary `/` expressions inline this same check directly
%% rather than calling out here.
divide(A, B) when is_integer(A), is_integer(B) -> A div B;
divide(A, B) -> A / B.

%% add/2 — polymorphic + for strings, numbers, and Char+String/String+Char.
%% A Kex Char and a Kex Integer are both just plain Erlang integers (no
%% distinct runtime tag), so `x.upperCase + rest` (Char + String, e.g.
%% spec/my_capitalize.kex) can't be told apart from actual integer
%% arithmetic by value alone — bias toward treating an integer mixed with a
%% list as a char code to splice in, matching Kex's Char+String convention
%% (an integer never legitimately adds to a list otherwise: that's always
%% a type error in Kex, so this heuristic has no real downside).
add(A, B) when is_list(A), is_integer(B) -> A ++ [B];
add(A, B) when is_integer(A), is_list(B) -> [A | B];
add(A, B) when is_list(A) -> A ++ B;
add(A, B) -> A + B.

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

%% list_get/2,3 — `list[i]` / `list.get(i[, default])`. Returns the raw
%% element (or none/Default if out of range) — NOT Just(value)-wrapped,
%% unlike Map.get's 2-arg form. Matches
%% src/interpreter/stdlib/map.cxx's `get` builtin exactly for ListValue
%% receivers (see that file's comment on why list indexing and Map.get
%% differ in wrapping behavior despite sharing one Kex-level name).
list_get(List, Idx) -> list_get(List, Idx, 'none').
list_get(List, Idx, _Default) when is_integer(Idx), Idx >= 0, Idx < length(List) ->
    lists:nth(Idx + 1, List);
list_get(_List, _Idx, Default) ->
    Default.

%% Math.e / Math.E, Math.log(x[, base]), Math.hypot(a,b), Math.cbrt(x) —
%% not plain 1:1 BIF forwards (math:exp/1 needs an argument so can't back a
%% bare 0-arg constant; Erlang's math module has no log/2, hypot/2, or
%% cbrt/1 at all), matching
%% src/interpreter/stdlib/math.cxx's Math::E/log/hypot/cbrt exactly.
math_e() -> math:exp(1.0).
math_log(X) -> math:log(X).
math_log(X, Base) -> math:log(X) / math:log(Base).
math_hypot(A, B) -> math:sqrt(A * A + B * B).
math_cbrt(X) when X < 0 -> -math:pow(-X, 1.0 / 3.0);
math_cbrt(X) -> math:pow(X, 1.0 / 3.0).

%% assert(cond[, msg]) — matches src/interpreter/stdlib/test.cxx's assert
%% exactly: throws (here, erlang:error/1, caught the same way any other
%% Kex runtime error is) when cond isn't truthy.
assert(Cond) -> assert(Cond, "assertion failed").
assert(Cond, Msg) ->
    case is_truthy(Cond) of
        true -> true;
        false -> erlang:error(lists:flatten("assertion failed: " ++ to_string(Msg)))
    end.

%% Same truthiness rule as `if`/`while`/`&&`/`||` throughout this runtime:
%% only false/none/'ok' (Kex's Unit) are falsy — everything else (0, "",
%% [], any record/variant) is truthy.
is_truthy(false) -> false;
is_truthy('none') -> false;
is_truthy('ok') -> false;
is_truthy(_) -> true.

%% list.indexOf(value) -> Just(index) | None (0-based) — matches
%% src/interpreter/stdlib/list.cxx's indexOf exactly. No lists:indexOf/2
%% BIF exists in standard Erlang/OTP.
index_of(Value, List) -> index_of(Value, List, 0).
index_of(_Value, [], _I) -> 'none';
index_of(Value, [Value | _], I) -> {'Just', I};
index_of(Value, [_ | Rest], I) -> index_of(Value, Rest, I + 1).

%% list.product — no lists:product/1 BIF.
list_product(List) -> lists:foldl(fun(E, A) -> A * E end, 1, List).

%% Char predicates — matches src/interpreter/stdlib/string.cxx's
%% digit?/alpha?/space? exactly. Real top-level functions (not inlined at
%% the call site) so `&digit?`/`&alpha?`/`&space?` (a first-class function
%% reference — see core_erlang.cxx's ShorthandLambda handling) can call the
%% exact same implementation, not just the direct `.digit?` method form.
is_digit(C) -> C >= $0 andalso C =< $9.
is_alpha(C) -> (C >= $A andalso C =< $Z) orelse (C >= $a andalso C =< $z).
is_space(C) -> lists:member(C, [$\s, $\t, $\n, $\r, $\v, $\f]).

%% ENV — a real Map<String,String> value (see
%% src/interpreter/stdlib/env.cxx), a snapshot of the process environment.
%% Built fresh on every reference (matches how ENV is compiled — see
%% core_erlang.cxx's UpperIdentifier handling) rather than once at startup;
%% cheap enough, and avoids needing a global to seed.
env_map() ->
    maps:from_list([split_env_entry(E) || E <- os:getenv()]).

split_env_entry(E) ->
    case string:split(E, "=") of
        [K, V] -> {K, V};
        [K]    -> {K, ""}
    end.

%% Integer.parse(s) / Float.parse(s) -> Ok(v) | Error(reason) — matches
%% src/interpreter/stdlib/integer.cxx's Integer::parse/Float::parse exactly
%% (arbitrary-precision integers included, via the same big-integer path
%% Kex's own Value::bigInteger uses elsewhere in this runtime).
integer_parse(S) ->
    case string:to_integer(S) of
        {Int, ""} -> {'Ok', Int};
        _ -> {'Error', "invalid integer: " ++ S}
    end.

float_parse(S) ->
    case string:to_float(S) of
        {Flt, ""} -> {'Ok', Flt};
        _ ->
            % string:to_float/1 requires a decimal point (rejects "42");
            % Kex's Float.parse accepts plain integers too.
            case string:to_integer(S) of
                {Int, ""} -> {'Ok', float(Int)};
                _ -> {'Error', "invalid float: " ++ S}
            end
    end.
