%% Kex.Intrinsic.Fun — function-application primitives.
%%
%% applyItem/2 is the auto-splat used by Enumerable's default HOFs: a block may
%% be written `{ |x| }` over elements (List/Range) or `{ |k, v| }` over pairs
%% (Map). When the item is a 2-tuple and the block takes 2 args, spread it.
-module(kex_intrinsic_fun).
-export([applyItem/2]).

applyItem(F, Item) ->
    case {erlang:fun_info(F, arity), Item} of
        {{arity, 2}, {K, V}} -> F(K, V);
        _                    -> F(Item)
    end.
