%% Kex.Intrinsic.Fun — function-application primitives.
%%
%% applyItem/2 is the auto-splat used by Enumerable's default HOFs: a block may
%% be written `{ |x| }` over elements (List/Range) or `{ |k, v| }` over pairs
%% (Map). When the item is a 2-tuple and the block takes 2 args, spread it.
-module(kex_intrinsic_fun).
-export([applyItem/2, convertTo/2, items/1]).

convertTo(V, 'String') -> kex_io:to_string_optional(V);
convertTo(V, 'Integer') -> kex_intrinsic_number:to_integer(V);
convertTo(V, 'Int') -> kex_intrinsic_number:to_integer(V);
convertTo(V, 'Float') -> kex_intrinsic_number:to_float(V);
convertTo(V, 'List') when is_list(V) -> {'Just', V};
convertTo(_, _) -> 'None'.

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
