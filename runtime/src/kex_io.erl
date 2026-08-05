-module(kex_io).
-export([print_line/1, print/1, print_error/1, read_line/0, read_char/0,
           inspect/1, inspect_rendered/2, inspect_repl/1, inspect_value/1, inspect_plain/1, inspect_typed/2, to_string/1, to_string_optional/1,
           to_string_bin/1, env_map/0, register_display/2,
           mock_start/0, mock_input/1, mock_output/0, mock_clear/0,
           mock_stop/0, value_type_name/1]).

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
    case mock_active() of
        true -> mock_append(X, <<"\n">>);
        false -> io:format("~ts~n", [to_string(X)]), 'Kex.Unit'
    end.

%% IO.print(x) — print x without a trailing newline.
print(X) ->
    case mock_active() of
        true -> mock_append(X, <<>>);
        false -> io:format("~ts", [to_string(X)]), 'Kex.Unit'
    end.

%% IO.printError / IO.warn / IO.warning — print to stderr.
print_error(X) ->
    case mock_active() of
        true -> mock_append(X, <<"\n">>);
        false -> io:format(standard_error, "~ts~n", [to_string(X)]), 'Kex.Unit'
    end.

%% IO.readLine — read a line from stdin, returns a String (UTF-8 binary).
read_line() ->
    case mock_active() of
        true -> mock_take_line();
        false ->
            case io:get_line("") of
                eof -> 'None';
                {error, _} -> 'None';
                Line -> unicode:characters_to_binary(string:trim(Line, trailing, "\n"))
            end
    end.

%% IO.get — read one Unicode character, sharing the mock input queue used by
%% read_line/0. The terminal fallback asks the IO server for one character.
read_char() ->
    case mock_active() of
        true -> mock_take_char();
        false ->
            case io:get_chars("", 1) of
                eof -> 'None';
                {error, _} -> 'None';
                Chars -> unicode:characters_to_binary(Chars)
            end
    end.

mock_start() ->
    put(kex_mock_io_active, true),
    mock_clear().

mock_input(Lines) ->
    Existing = case get(kex_mock_io_input) of undefined -> []; V -> V end,
    put(kex_mock_io_input, Existing ++ [to_string_bin(L) || L <- Lines]),
    'Kex.Unit'.

mock_output() ->
    case get(kex_mock_io_output) of undefined -> <<>>; V -> V end.

mock_clear() ->
    put(kex_mock_io_output, <<>>),
    put(kex_mock_io_input, []),
    'Kex.Unit'.

mock_stop() ->
    erase(kex_mock_io_active),
    erase(kex_mock_io_output),
    erase(kex_mock_io_input),
    'Kex.Unit'.

mock_active() -> get(kex_mock_io_active) =:= true.

mock_append(X, Suffix) ->
    Current = mock_output(),
    Text = to_string_bin(X),
    put(kex_mock_io_output, <<Current/binary, Text/binary, Suffix/binary>>),
    'Kex.Unit'.

mock_take_line() ->
    case get(kex_mock_io_input) of
        [Line | Rest] -> put(kex_mock_io_input, Rest), Line;
        _ -> 'None'
    end.

mock_take_char() ->
    case get(kex_mock_io_input) of
        [Line | Rest] ->
            case unicode:characters_to_list(Line) of
                [Char | More] ->
                    Tail = unicode:characters_to_binary(More),
                    Next = case More of [] -> Rest; _ -> [Tail | Rest] end,
                    put(kex_mock_io_input, Next),
                    unicode:characters_to_binary([Char]);
                [] ->
                    put(kex_mock_io_input, Rest),
                    mock_take_char()
            end;
        _ -> 'None'
    end.

%% IO.inspect — print "<value> : <Type>" with ANSI colours, returns value.
-define(GRAY,  color("\e[90m")).
-define(RESET, color("\e[0m")).
-define(CYAN,  color("\e[36m")).
-define(YELL,  color("\e[33m")).
-define(GREEN, color("\e[32m")).
-define(WHITE, color("\e[97m")).

color(Code) ->
    case os:getenv("KEX_COLORS", "1") of
        "0" -> "";
        _ -> Code
    end.

inspect(X) ->
    case mock_active() of
        true -> mock_append(inspect_value(X), <<"\n">>), X;
        false ->
            put(kex_inspect_stderr, true),
            try inspect_real(X)
            after erase(kex_inspect_stderr)
            end
    end.

%% The Inspectable implementation owns value rendering; IO only adds the
%% established diagnostic type suffix and chooses the output stream.
inspect_rendered(X, Rendered) ->
    Type = value_type_name(X),
    case mock_active() of
        true ->
            mock_append(Rendered,
                        unicode:characters_to_binary([" : ", Type, "\n"])),
            X;
        false ->
            put(kex_inspect_stderr, true),
            try
                inspect_format("~ts" ++ ?GRAY ++ " : " ++ ?RESET ++
                               ?CYAN ++ "~ts" ++ ?RESET ++ "~n",
                               [Rendered, Type]),
                X
            after erase(kex_inspect_stderr)
            end
    end.

%% The BEAM REPL historically prefixes evaluated values with `=>`, while
%% IO.inspect in a program matches the tree walker and prints only the value.
%% Keep that presentation concern at the REPL boundary instead of baking it
%% into the public IO operation.
inspect_repl('Kex.Unit') -> 'Kex.Unit';
inspect_repl(ok) -> ok;
inspect_repl(X) ->
    io:format(?GRAY ++ "=> " ++ ?RESET),
    inspect_real(X).

inspect_format(Format) -> inspect_format(Format, []).
inspect_format(Format, Args) ->
    case get(kex_inspect_stderr) of
        true -> io:format(standard_error, Format, Args);
        _ -> io:format(Format, Args)
    end.

%% REPL-only inspection with the compiler's semantic type. Runtime tuples
%% erase phantom parameters, so value_type_name/1 cannot recover typestate.
inspect_typed('Kex.Unit', _Type) -> 'Kex.Unit';
inspect_typed(ok, _Type) -> ok;
inspect_typed(X, Type) ->
    io:format(?GRAY ++ "=> " ++ ?RESET ++ "~ts"
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "~ts" ++ ?RESET ++ "~n",
              [inspect_string(X), Type]),
    X.

inspect_real(X) when is_integer(X) ->
    inspect_format(?YELL ++ "~p" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Int" ++ ?RESET ++ "~n", [X]), X;
%% format_float/1, not ~g: ~g rendered 3.0e11 as "3.00000e+11" in the BEAM
%% REPL while the walker REPL and every non-REPL path printed
%% "300000000000.0" for the same value.
inspect_real(X) when is_float(X) ->
    inspect_format(?YELL ++ "~ts" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Float" ++ ?RESET ++ "~n", [format_float(X)]), X;
inspect_real(true) ->
    inspect_format(?YELL ++ "true" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Bool" ++ ?RESET ++ "~n"), true;
inspect_real(false) ->
    inspect_format(?YELL ++ "false" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Bool" ++ ?RESET ++ "~n"), false;
inspect_real('Kex.Unit') ->
    'Kex.Unit';
inspect_real(ok) ->
    ok;
inspect_real('None') ->
    inspect_format(?WHITE ++ "None" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Option" ++ ?RESET ++ "~n"), 'None';
inspect_real(X) when is_atom(X) ->
    Name = atom_to_list(X),
    case Name of
        [C | _] when C >= $A, C =< $Z ->
            inspect_format("~ts"
                      ++ " " ++ ?GRAY ++ ":" ++ ?RESET
                      ++ " " ++ ?CYAN ++ "~ts" ++ ?RESET ++ "~n",
                      [Name, nullary_type_name(X)]);
        _ ->
            inspect_format(":~ts"
                      ++ " " ++ ?GRAY ++ ":" ++ ?RESET
                      ++ " " ++ ?CYAN ++ "Atom" ++ ?RESET ++ "~n", [Name])
    end,
    X;
inspect_real(X) when is_binary(X) ->
    inspect_format(?GREEN ++ "\"~ts\"" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "String" ++ ?RESET ++ "~n", [X]), X;
inspect_real([{'Char', _} | _] = X) ->
    inspect_format(?GREEN ++ "\"~ts\"" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "String" ++ ?RESET ++ "~n", [to_string(X)]), X;
inspect_real(X) when is_list(X) ->
    inspect_format("~ts"
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "~ts" ++ ?RESET ++ "~n",
              [inspect_string(X), list_type_name(X)]), X;
inspect_real({'Char', C}) ->
    inspect_format(?GREEN ++ "'~ts'" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Char" ++ ?RESET ++ "~n", [[C]]), {'Char', C};
inspect_real({'Just', {'Char', C}} = X) ->
    inspect_format(?GREEN ++ "Just("
              ++ [C] ++ ")" ++ ?RESET
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Char" ++ ?RESET ++ "~n"), X;
inspect_real(X) when is_tuple(X), tuple_size(X) >= 1,
                   (element(1, X) =:= 'Just' orelse element(1, X) =:= 'Ok' orelse
                    element(1, X) =:= 'Error' orelse element(1, X) =:= 'Some') ->
    inspect_format("~ts"
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "~ts" ++ ?RESET ++ "~n",
              [inspect_string(X), value_type_name(X)]), X;
inspect_real(X) when is_tuple(X) ->
    inspect_format("~ts"
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "~ts" ++ ?RESET ++ "~n",
              [inspect_string(X), value_type_name(X)]), X;
inspect_real(X) when is_map(X) ->
    inspect_format("~ts"
              ++ " " ++ ?GRAY ++ ":" ++ ?RESET
              ++ " " ++ ?CYAN ++ "Map" ++ ?RESET ++ "~n",
              [inspect_string(X)]), X;
inspect_real(X) ->
    inspect_format("~p~n", [X]), X.

%% UFCS value.inspect() — return the colored representation as a String.
inspect_value(X) -> unicode:characters_to_binary(inspect_string(X)).

%% The same rendering as inspect_string/1 with the ANSI escapes removed, so a
%% value can be turned into a plain String (Kex's `.inspect`) without keeping a
%% second, drifting copy of the formatting rules.
inspect_plain(X) -> strip_ansi(lists:flatten(inspect_string(X))).

strip_ansi([]) -> [];
strip_ansi([$\e, $[ | Rest]) -> strip_ansi(drop_until_m(Rest));
strip_ansi([C | Rest]) -> [C | strip_ansi(Rest)].

drop_until_m([]) -> [];
drop_until_m([$m | Rest]) -> Rest;
drop_until_m([_ | Rest]) -> drop_until_m(Rest).

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
inspect_string({'FileHandle', _Device, Path}) ->
    "<FileHandle: " ++ inspect_string(Path) ++ ">";
inspect_string({'MockFileHandle', Path}) ->
    "<FileHandle: " ++ inspect_string(Path) ++ ">";
inspect_string(X) when is_tuple(X), tuple_size(X) >= 1,
                       (element(1, X) =:= 'Just' orelse element(1, X) =:= 'Ok' orelse
                        element(1, X) =:= 'Error' orelse element(1, X) =:= 'Some') ->
    [Tag | Args] = tuple_to_list(X),
    atom_to_list(Tag) ++ "(" ++
        lists:flatten(lists:join(", ", [inspect_string(A) || A <- Args])) ++ ")";
%% A record renders as `Tag { field: value, … }`, the same shape to_string/1
%% already produced — inspect_string/1 consulted only the VARIANT table, so
%% every record (prelude ones included, e.g. ParseError) fell through to the
%% raw tuple form showing its type tag as an atom.
inspect_string(X) when is_tuple(X), tuple_size(X) >= 2,
                       is_atom(element(1, X)) ->
    Tag = element(1, X),
    Arity = tuple_size(X) - 1,
    Records = persistent_term:get(kex_display_records, #{}),
    case maps:get(Tag, Records, undefined) of
        Fields when is_list(Fields), length(Fields) =:= Arity ->
            Pairs = lists:sort(lists:zip([atom_to_list(F) || F <- Fields],
                                         lists:seq(2, tuple_size(X)))),
            Body = [F ++ ": " ++ inspect_string(element(I, X)) || {F, I} <- Pairs],
            ?CYAN ++ atom_to_list(Tag) ++ ?RESET ++
                " { " ++ lists:flatten(lists:join(", ", Body)) ++ " }";
        _ -> inspect_tuple_string(X)
    end;
inspect_string(X) when is_tuple(X) -> inspect_tuple_string(X);
inspect_string(X) -> unicode:characters_to_list(to_string(X)).

inspect_tuple_string(X) ->
    Tag = element(1, X),
    Arity = tuple_size(X) - 1,
    case variant_metadata(Tag) of
        {Arity, _Owner} -> inspect_variant_string(X);
        _ ->
            "(" ++ lists:flatten(lists:join(", ",
                [inspect_string(E) || E <- tuple_to_list(X)])) ++ ")"
    end.

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
        _ ->
            %% A record is a tagged tuple whose tag and field count match a
            %% registered layout; its type is the tag. Without this the REPL
            %% reported the lowered representation ("Tuple", or
            %% "Result<?, Tuple>") for every record.
            Records = persistent_term:get(kex_display_records, #{}),
            case is_atom(Tag) andalso maps:get(Tag, Records, undefined) of
                Fields when is_list(Fields), length(Fields) =:= Arity ->
                    atom_to_list(Tag);
                _ -> "Tuple"
            end
    end;
%% A nullary constructor lowers to a bare atom, so its type has to come from
%% the variant registry — otherwise every one of them reported "Atom", and
%% containers holding them reported "[Atom]" / "Option<Atom>".
value_type_name(X) when is_atom(X) -> nullary_type_name(X);
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

%% Float display — the same algorithm as src/interpreter/value.cxx's
%% formatFloat, digit for digit, since the spec suite diffs walker output
%% against BEAM output. Both render the shortest digit string that reads back
%% as the same double; neither can use its platform default, because
%% `float_to_list(X, [short])` spells 2000000.0 as "2.0e6" while C++'s
%% shortest `to_chars` spells it "2e+06".
%% Signed zero, without arithmetic — `1.0 / 0.0` is a badarith on BEAM.
format_float(X) when X == 0.0 ->
    case float_to_list(X, [short]) of
        [$- | _] -> "-0.0";
        _        -> "0.0"
    end;
format_float(X) ->
    S = float_to_list(X, [short]),
    {Sign, Rest} = case S of
        [$- | R] -> {"-", R};
        _        -> {"", S}
    end,
    %% Reduce to `Digits * 10^Power`, the one shape both backends agree on
    %% regardless of how their shortest-round-trip primitive spelled it.
    {MantS, Exp0} = case string:split(Rest, "e") of
        [M, E] -> {M, list_to_integer(E)};
        [M]    -> {M, 0}
    end,
    {Whole, Frac} = case string:split(MantS, ".") of
        [W, F] -> {W, F};
        [W]    -> {W, ""}
    end,
    %% Canonicalize, so "20 * 10^5" and "2 * 10^6" render identically.
    {Digits, Power} = strip_trailing_zeros(Whole ++ Frac, Exp0 - length(Frac)),
    Mag = abs(X),
    case Mag < 1.0e-4 orelse Mag >= 1.0e16 of
        true  -> Sign ++ sci_float(Digits, Power);
        false -> Sign ++ plain_float(Digits, Power)
    end.

strip_trailing_zeros(Digits, Power) when length(Digits) > 1 ->
    case lists:last(Digits) of
        $0 -> strip_trailing_zeros(lists:droplast(Digits), Power + 1);
        _  -> {Digits, Power}
    end;
strip_trailing_zeros(Digits, Power) ->
    {Digits, Power}.

sci_float([D | Rest], Power) ->
    Frac = case Rest of
        [] -> "0";
        _  -> Rest
    end,
    [D] ++ "." ++ Frac ++ "e" ++ integer_to_list(Power + length(Rest)).

plain_float(Digits, Power) when Power >= 0 ->
    %% Whole number — keep it visibly a float ("2" -> "2000000.0").
    Digits ++ lists:duplicate(Power, $0) ++ ".0";
plain_float(Digits, Power) ->
    Shift = -Power,
    case length(Digits) > Shift of
        true ->
            {W, F} = lists:split(length(Digits) - Shift, Digits),
            W ++ "." ++ F;
        false ->
            "0." ++ lists:duplicate(Shift - length(Digits), $0) ++ Digits
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
