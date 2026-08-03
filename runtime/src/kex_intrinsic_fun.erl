%% Kex.Intrinsic.Fun — function-application primitives.
%%
%% applyItem/2 is the auto-splat used by Enumerable's default HOFs: a block may
%% be written `{ |x| }` over elements (List/Range) or `{ |k, v| }` over pairs
%% (Map). When the item is a 2-tuple and the block takes 2 args, spread it.
%%
%% applyIndexed/3 is the same idea for the indexed HOFs (eachIndexed /
%% mapIndexed). The index is always the LAST argument, so `|entry, I|` gets the
%% whole item and `|K, V, I|` gets a Map entry spread. A 1-arity block ignores
%% the index.
-module(kex_intrinsic_fun).
-export([applyItem/2, applyIndexed/3, convertTo/2, convertTo/3, items/1]).

%% A `Type` VALUE names its target too: `x.to(Type.of(y))`. Its name is a
%% binary, so it routes back through the atom-keyed clauses below.
convertTo(V, {'Type', Name, _Args, _Pure}) -> convertTo(V, binary_to_atom(Name, utf8));
convertTo(V, 'String') -> kex_io:to_string_optional(V);
convertTo(V, 'Integer') -> kex_intrinsic_number:to_integer(V);
convertTo(V, 'Float') -> kex_intrinsic_number:to_float(V);
convertTo(V, 'List') when is_list(V) -> {'Just', V};
convertTo(_, _) -> 'None'.

%% Base-aware Integer <-> String, `.to(String, radix: 16)`. Only that pair is
%% base dependent; anything else with a radix is a conversion that was not
%% meant, so it fails rather than silently ignoring the base.
convertTo(V, 'String', Radix)
  when is_integer(V), is_integer(Radix), Radix >= 2, Radix =< 36 ->
    %% integer_to_list/2 uppercases digits above 9; the walker's GMP get_str
    %% lowercases them, and the two have to agree.
    {'Just', unicode:characters_to_binary(string:lowercase(integer_to_list(V, Radix)))};
convertTo(V, 'Integer', Radix) when is_binary(V) ->
    case kex_intrinsic_integer:parse_in_base(V, Radix) of
        {ok, N} -> {'Just', N};
        error   -> 'None'
    end;
convertTo(_, _, _) -> 'None'.

applyItem(F, Item) ->
    {arity, N} = erlang:fun_info(F, arity),
    case spreadable(Item, N) of
        true  -> erlang:apply(F, tuple_to_list(Item));
        false -> F(Item)
    end.

applyIndexed(F, Item, I) ->
    {arity, N} = erlang:fun_info(F, arity),
    case spreadable(Item, N - 1) of
        true -> erlang:apply(F, tuple_to_list(Item) ++ [I]);
        false when N =:= 1 -> F(Item);
        false -> F(Item, I)
    end.

%% Should `Item` be spread across a block of `N` parameters?
%%
%% Only a genuine tuple of exactly N elements, and only for N > 1 — a
%% single-parameter block always receives the whole item.
%%
%% RECORDS ARE EXCLUDED, and that is the whole subtlety: a record is also a
%% tuple here (`{'Point', 1, 2}`), so without the check a two-parameter block
%% over a list of one-field records got the tag and the field rather than an
%% error. The walker has RecordValue and TupleValue as separate types and
%% rejects it, so this was a silent backend divergence. The registry
%% kex_io:register_display/2 fills is what makes the two distinguishable at
%% all — a bare `{a, 1}` and a record are otherwise the same term.
spreadable(Item, N) when is_tuple(Item), N > 1, tuple_size(Item) =:= N ->
    not is_record_term(Item);
spreadable(_, _) ->
    false.

is_record_term(Item) ->
    case element(1, Item) of
        Tag when is_atom(Tag) ->
            maps:is_key(Tag, persistent_term:get(kex_display_records, #{}));
        _ ->
            false
    end.

%% Normalize an Enumerable receiver to the item representation used by Kex
%% callbacks. Maps yield {Key, Value} pairs; lists and strings retain the same
%% coercion used by the HOF helpers above.
items(M) when is_map(M) -> maps:to_list(M);
items(L) -> kex_intrinsic_list:as_list(L).
