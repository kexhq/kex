-module(kex_io).
-export([print_line/1, print/1, print_error/1, read_line/0, inspect/1, inspect_value/1, to_string/1,
           to_string_optional/1,
           to_string_bin/1, env_map/0, register_display/2]).

%% register_display/2 — called once at main start with the compiling module's
%% record layouts (#{Tag => [field, …]} in declaration order) and ADT payload
%% variant arities (#{Tag => Arity}). Only the compiler knows these; without
%% them a record and a plain tuple are the same term, so to_string falls back
%% to tuple rendering.
register_display(Records, Variants) ->
    Old_R = persistent_term:get(kex_display_records, #{}),
    Old_V = persistent_term:get(kex_display_variants, #{}),
    persistent_term:put(kex_display_records, maps:merge(Old_R, Records)),
    persistent_term:put(kex_display_variants, maps:merge(Old_V, Variants)),
    ok.

%% IO.printLine(x) — print x followed by a newline to stdout.
print_line(X) ->
    io:format("~ts~n", [to_string(X)]),
    'Kex.Unit'.

%% IO.print(x) — print x without a trailing newline.
print(X) ->
    io:format("~ts", [to_string(X)]),
    'Kex.Unit'.

%% IO.printError / IO.warn / IO.warning — print to stderr.
print_error(X) ->
    io:format(standard_error, "~ts~n", [to_string(X)]),
    'Kex.Unit'.

%% IO.readLine — read a line from stdin, returns a String (UTF-8 binary).
read_line() ->
    case io:get_line("") of
        eof -> 'None';
        {error, _} -> 'None';
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
inspect('Kex.Unit') ->
    'Kex.Unit';
inspect(ok) ->
    ok;
inspect('None') ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ ?WHITE ++ "None" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Option" ++ ?RESET ++ "~n"), 'None';
inspect(X) when is_atom(X) ->
    Name = atom_to_list(X),
    case Name of
        [C | _] when C >= $A, C =< $Z ->
            io:format(?GRAY ++ "=> " ++ ?RESET ++ "~ts"
                      ++ " " ++ ?GRAY ++ ":" ++ ?RESET
                      ++ " " ++ ?CYAN ++ "~ts" ++ ?RESET ++ "~n",
                      [Name, nullary_type_name(X)]);
        _ ->
            io:format(?GRAY ++ "=> " ++ ?RESET ++ ":~ts"
                      ++ " " ++ ?GRAY ++ ":" ++ ?RESET
                      ++ " " ++ ?CYAN ++ "Atom" ++ ?RESET ++ "~n", [Name])
    end,
    X;
inspect(X) when is_binary(X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ ?GREEN ++ "\"~ts\"" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "String" ++ ?RESET ++ "~n", [X]), X;
inspect([{'Char', _} | _] = X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ ?GREEN ++ "\"~ts\"" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "String" ++ ?RESET ++ "~n", [to_string(X)]), X;
inspect(X) when is_list(X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ "~ts"
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "~ts" ++ ?RESET ++ "~n",
              [inspect_string(X), list_type_name(X)]), X;
inspect({'Char', C}) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ ?GREEN ++ "'~ts'" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Char" ++ ?RESET ++ "~n", [[C]]), {'Char', C};
inspect({'Just', {'Char', C}} = X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ ?GREEN ++ "Just("
              ++ [C] ++ ")" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Char" ++ ?RESET ++ "~n"), X;
inspect(X) when is_tuple(X), tuple_size(X) >= 1,
                   (element(1, X) =:= 'Just' orelse element(1, X) =:= 'Ok' orelse
                    element(1, X) =:= 'Error' orelse element(1, X) =:= 'Some') ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ "~ts"
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "~ts" ++ ?RESET ++ "~n",
              [inspect_string(X), value_type_name(X)]), X;
inspect(X) when is_tuple(X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ "~ts"
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "~ts" ++ ?RESET ++ "~n",
              [inspect_string(X), value_type_name(X)]), X;
inspect(X) when is_map(X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ "~ts"
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Map" ++ ?RESET ++ "~n",
              [inspect_string(X)]), X;
inspect(X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ "~p~n", [X]), X.

%% UFCS value.inspect() — return the colored representation as a String.
inspect_value(X) -> unicode:characters_to_binary(inspect_string(X)).

inspect_string(X) when is_binary(X) ->
    ?GREEN ++ "\"" ++ unicode:characters_to_list(X) ++ "\"" ++ ?RESET;
inspect_string(X) when is_integer(X) -> ?YELL ++ integer_to_list(X) ++ ?RESET;
inspect_string(true) -> ?YELL ++ "true" ++ ?RESET;
inspect_string(false) -> ?YELL ++ "false" ++ ?RESET;
inspect_string('None') -> ?GRAY ++ "None" ++ ?RESET;
inspect_string(X) when is_list(X) ->
    "[" ++ lists:flatten(lists:join(", ", [inspect_string(E) || E <- X])) ++ "]";
inspect_string(X) when is_map(X) ->
    Pairs = [inspect_string(K) ++ ": " ++ inspect_string(V)
             || {K, V} <- lists:sort(maps:to_list(X))],
    "{ " ++ lists:flatten(lists:join(", ", Pairs)) ++ " }";
inspect_string(X) when is_atom(X) ->
    case variant_metadata(X) of
        {0, _Owner} -> atom_to_list(X);
        _ -> ?GREEN ++ ":" ++ atom_to_list(X) ++ ?RESET
    end;
inspect_string(X) when is_tuple(X), tuple_size(X) >= 1,
                       (element(1, X) =:= 'Just' orelse element(1, X) =:= 'Ok' orelse
                        element(1, X) =:= 'Error' orelse element(1, X) =:= 'Some') ->
    [Tag | Args] = tuple_to_list(X),
    atom_to_list(Tag) ++ "(" ++
        lists:flatten(lists:join(", ", [inspect_string(A) || A <- Args])) ++ ")";
inspect_string(X) when is_tuple(X) ->
    Tag = element(1, X),
    Arity = tuple_size(X) - 1,
    case variant_metadata(Tag) of
        {Arity, _Owner} -> inspect_variant_string(X);
        _ ->
            "(" ++ lists:flatten(lists:join(", ",
                [inspect_string(E) || E <- tuple_to_list(X)])) ++ ")"
    end;
inspect_string(X) -> unicode:characters_to_list(to_string(X)).

nullary_type_name(X) ->
    case variant_metadata(X) of
        {0, Owner} -> atom_to_list(Owner);
        _ ->
            case X of
                'Less' -> "Ordering";
                'Equal' -> "Ordering";
                'Greater' -> "Ordering";
                _ -> "Variant"
            end
    end.

variant_metadata(Tag) when is_atom(Tag) ->
    maps:get(Tag, persistent_term:get(kex_display_variants, #{}), undefined);
variant_metadata(_) -> undefined.

inspect_variant_string(X) ->
    [Tag | Args] = tuple_to_list(X),
    atom_to_list(Tag) ++ "(" ++ lists:flatten(lists:join(", ",
        [inspect_string(A) || A <- Args])) ++ ")".

list_type_name([]) -> "[?]";
list_type_name([H | T]) ->
    Type = value_type_name(H),
    case lists:all(fun(E) -> value_type_name(E) =:= Type end, T) of
        true -> "[" ++ Type ++ "]";
        false -> "[Any]"
    end.

value_type_name(X) when is_binary(X) -> "String";
value_type_name(X) when is_integer(X) -> "Int";
value_type_name(X) when is_float(X) -> "Float";
value_type_name(true) -> "Bool";
value_type_name(false) -> "Bool";
value_type_name({'Char', _}) -> "Char";
value_type_name(X) when is_list(X) -> list_type_name(X);
value_type_name(X) when is_map(X) -> "Map";
value_type_name({'Just', V}) -> "Option<" ++ value_type_name(V) ++ ">";
value_type_name({'Some', V}) -> "Option<" ++ value_type_name(V) ++ ">";
value_type_name({'Ok', V}) -> "Result<" ++ value_type_name(V) ++ ", ?>";
value_type_name({'Error', V}) -> "Result<?, " ++ value_type_name(V) ++ ">";
value_type_name(X) when is_tuple(X) ->
    Tag = element(1, X),
    Arity = tuple_size(X) - 1,
    case variant_metadata(Tag) of
        {Arity, Owner} -> atom_to_list(Owner);
        _ -> "Tuple"
    end;
value_type_name(X) when is_atom(X) -> "Atom";
value_type_name(_) -> "Any".

%% Any Kex value as a Kex String VALUE (UTF-8 binary) — what `.to(String)`
%% and toString-style conversions return. to_string/1 below stays a charlist
%% because its output feeds io:format/iolists, not user code.
to_string_bin(X) when is_binary(X) -> X;
to_string_bin(X) -> unicode:characters_to_binary(to_string(X)).

%% Universal `value.to(String)` conversion. Keep the Optional construction
%% behind a runtime call so Core Erlang does not fold a subsequent
%% Just/None match and warn that the failure branch is unreachable.
to_string_optional(X) -> {'Just', to_string_bin(X)}.

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
to_string('None')               -> "None";
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
% Kex map syntax (matching the walker's MapValue::toString): `{ :k: v, … }`,
% and `{  }` when empty. Note key ORDER can still differ from the walker —
% Erlang maps don't preserve insertion order after put/delete.
to_string(X) when is_map(X)     ->
    Pairs = [to_string(K) ++ ": " ++ to_string(V) || {K, V} <- lists:sort(maps:to_list(X))],
    "{ " ++ lists:flatten(lists:join(", ", Pairs)) ++ " }";
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
    variant_string(X);
% A registered record renders as `Name { field: value, … }` (fields sorted,
% matching the walker); a registered payload-variant tag as `Tag(args)`;
% anything else stays a plain Kex tuple `(a, b)`. The tag AND arity must
% both match — `(Underscore, "wooo")`, a tuple whose head happens to be a
% nullary variant VALUE, still renders as a tuple.
to_string(X) when is_tuple(X), tuple_size(X) >= 1 ->
    Tag = element(1, X),
    Arity = tuple_size(X) - 1,
    Recs = persistent_term:get(kex_display_records, #{}),
    case is_atom(Tag) andalso maps:get(Tag, Recs, undefined) of
        Fields when is_list(Fields), length(Fields) =:= Arity ->
            Pairs = lists:sort(lists:zip([atom_to_list(F) || F <- Fields],
                                         lists:seq(2, tuple_size(X)))),
            Body = [F ++ ": " ++ to_string(element(I, X)) || {F, I} <- Pairs],
            atom_to_list(Tag) ++ " { " ++ lists:flatten(lists:join(", ", Body)) ++ " }";
        _ ->
            Vars = persistent_term:get(kex_display_variants, #{}),
            case is_atom(Tag) andalso maps:get(Tag, Vars, undefined) of
                {Arity, _Owner} -> variant_string(X);
                Arity -> variant_string(X);
                _ ->
                    Parts = [to_string(E) || E <- tuple_to_list(X)],
                    "(" ++ lists:flatten(lists:join(", ", Parts)) ++ ")"
            end
    end;
to_string(X) when is_tuple(X)   ->
    Parts = [to_string(E) || E <- tuple_to_list(X)],
    "(" ++ lists:flatten(lists:join(", ", Parts)) ++ ")";
to_string(X)                    -> lists:flatten(io_lib:format("~p", [X])).

variant_string(X) ->
    [Tag | Args] = tuple_to_list(X),
    Parts = [to_string(E) || E <- Args],
    atom_to_list(Tag) ++ "(" ++ lists:flatten(lists:join(", ", Parts)) ++ ")".

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
