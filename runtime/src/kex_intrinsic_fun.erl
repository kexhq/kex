%% Kex.Intrinsic.Fun — function-application primitives.
%%
%% applyItem/2 is the auto-splat used by Enumerable's default HOFs: a block may
%% be written `{ |x| }` over elements (List/Range) or `{ |k, v| }` over pairs
%% (Map). When the item is a 2-tuple and the block takes 2 args, spread it.
-module(kex_intrinsic_fun).
-export([applyItem/2, convertTo/2, convertTo/3, items/1]).

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
    case {erlang:fun_info(F, arity), Item} of
        {{arity, 2}, {K, V}} -> F(K, V);
        _                    -> F(Item)
    end.

%% Normalize an Enumerable receiver to the item representation used by Kex
%% callbacks. Maps yield {Key, Value} pairs; lists and strings retain the same
%% coercion used by the HOF helpers above.
items(M) when is_map(M) -> maps:to_list(M);
items(L) -> kex_intrinsic_list:as_list(L).
