%% Kex.Intrinsic.Fun — function-application primitives.
%%
%% applyItem/2 is the auto-splat used by Enumerable's default HOFs: a block may
%% be written `{ |x| }` over elements (List/Range) or `{ |k, v| }` over pairs
%% (Map). When the item is a 2-tuple and the block takes 2 args, spread it.
-module(kex_intrinsic_fun).
-export([applyItem/2,
         each2/2, filter2/2, map2/2, count2/2, any2/2, all2/2, reject2/2,
         find2/2, or_else/2]).

%% or_else/2 — `.or(default)` is universal: Just/Ok unwrap, None/Error give
%% the default, and any other value returns itself (matching the walker).
or_else({'Just', X}, _) -> X;
or_else('None', D) -> D;
or_else({'Ok', X}, _) -> X;
or_else({'Error', _}, D) -> D;
or_else(V, _) -> V.

applyItem(F, Item) ->
    case {erlang:fun_info(F, arity), Item} of
        {{arity, 2}, {K, V}} -> F(K, V);
        _                    -> F(Item)
    end.

%% *2/2 — the `{ |k, v| }` (2-param block) HOF forms. The receiver is either
%% a Map (iterate its pairs; filter/reject return a Map) or a LIST of pairs
%% (e.g. `m.entries.map { |k, v| ... }`) — dispatch at runtime, since the
%% lowerer only sees the block's arity, not the receiver's type.

each2(M, F) when is_map(M) -> maps:foreach(F, M);
each2(L, F) -> lists:foreach(fun(I) -> applyItem(F, I) end, as_list(L)).

filter2(M, F) when is_map(M) -> maps:filter(F, M);
filter2(L, F) -> [I || I <- as_list(L), applyItem(F, I)].

map2(M, F) when is_map(M) ->
    lists:reverse(maps:fold(fun(K, V, A) -> [F(K, V) | A] end, [], M));
map2(L, F) -> [applyItem(F, I) || I <- as_list(L)].

count2(M, F) when is_map(M) -> maps:size(maps:filter(F, M));
count2(L, F) -> erlang:length(filter2(L, F)).

any2(M, F) when is_map(M) -> count2(M, F) > 0;
any2(L, F) -> lists:any(fun(I) -> applyItem(F, I) end, as_list(L)).

all2(M, F) when is_map(M) -> count2(M, F) =:= maps:size(M);
all2(L, F) -> lists:all(fun(I) -> applyItem(F, I) end, as_list(L)).

reject2(M, F) when is_map(M) ->
    maps:filter(fun(K, V) -> not F(K, V) end, M);
reject2(L, F) -> [I || I <- as_list(L), not applyItem(F, I)].

%% find2 — first matching pair as Just({K, V}) (map) / Just(Item) (list),
%% else 'None'.
find2(M, F) when is_map(M) ->
    maps:fold(fun(K, V, 'None') ->
                      case F(K, V) of
                          true -> {'Just', {K, V}};
                          false -> 'None'
                      end;
                 (_K, _V, Acc) -> Acc
              end, 'None', M);
find2(L, F) ->
    case lists:search(fun(I) -> applyItem(F, I) end, as_list(L)) of
        {value, I} -> {'Just', I};
        false -> 'None'
    end.

as_list(L) -> kex_intrinsic_list:as_list(L).
