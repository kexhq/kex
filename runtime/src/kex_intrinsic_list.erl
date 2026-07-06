%% Kex.Intrinsic.List — BEAM primitive backend for the list intrinsics.
%%
%% The ONLY place Core Erlang list BIFs are named for the stdlib. The typed
%% list stdlib lives in the Kex prelude (src/prelude/list.kex), which calls
%% `Kex.Intrinsic.List.<fn>`; the compiler maps that module path to a plain
%% cross-module call here. Adding a primitive is a one-line function.
-module(kex_intrinsic_list).
-export([reverse/1, sort/1, uniq/1, flatten/1, take/2, drop/2, zip/2, push/2,
         sum/1, product/1, indexOf/2, at/2, foldl/3, min/1, max/1, length/1,
         join/1, join/2,
         %% Lower-level list ops used directly by the emitters (moved here from
         %% kex_io, where list operations didn't belong).
         list_get/2, list_get/3, index_of/2, list_product/1]).

%% Receiver is always the first argument (Kex UFCS convention).
reverse(L) -> lists:reverse(L).
sort(L)    -> lists:sort(L).
uniq(L)    -> lists:usort(L).
flatten(L) -> lists:flatten(L).
take(L, N) -> lists:sublist(L, N).
drop(L, N) -> lists:nthtail(N, L).
zip(L, R)  -> lists:zip(L, R).
push(L, X) -> L ++ [X].
sum(L)     -> lists:sum(L).
product(L) -> list_product(L).
indexOf(L, X) -> index_of(X, L).
at(L, I)   -> list_get(L, I).
%% foldl/3 — the universal left fold backing Enumerable.reduce (and so every
%% HOF derived from it). Kex's reducer takes (acc, elem); Erlang's lists:foldl
%% takes fun(elem, acc), so swap the argument order at the boundary.
foldl(L, Acc, Fun) -> lists:foldl(fun(Elem, A) -> Fun(A, Elem) end, Acc, L).

%% min/1, max/1 — the smallest/largest element wrapped in Just, or None for [].
%% lists:min/max crash on the empty list, so guard here (the prelude's old
%% recursive impl was non-tail and returned this same Just/None form).
min([]) -> 'none';
min(L)  -> {'Just', lists:min(L)}.
max([]) -> 'none';
max(L)  -> {'Just', lists:max(L)}.

%% length/1 — element count, backing List.count (was a non-tail `1 + xs.count`
%% recursion in the prelude). Also covers charlist strings on BEAM.
length(L) -> erlang:length(L).

%% join/1,2 — string join for [String|Char] lists. The old prelude form was
%% non-tail recursive (`x + sep + xs.join(sep)` — the ++ wraps the call, so it
%% nests O(n) deep on both backends). join/1 concatenates directly; join/2
%% inserts Sep between each pair.
join(L)      -> lists:flatten(L).
join(L, Sep) -> lists:flatten(lists:join(Sep, L)).

%% list_get/2,3 — `list[i]` / `list.get(i[, default])`. Returns the raw element
%% (or none/Default if out of range) — NOT Just(value)-wrapped, unlike Map.get's
%% 2-arg form.
list_get(List, Idx) -> list_get(List, Idx, 'none').
list_get(List, Idx, _Default) when is_integer(Idx), Idx >= 0, Idx < erlang:length(List) ->
    lists:nth(Idx + 1, List);
list_get(_List, _Idx, Default) ->
    Default.

%% list.indexOf(value) -> Just(index) | None (0-based). No lists:indexOf/2 BIF.
index_of(Value, List) -> index_of(Value, List, 0).
index_of(_Value, [], _I) -> 'none';
index_of(Value, [Value | _], I) -> {'Just', I};
index_of(Value, [_ | Rest], I) -> index_of(Value, Rest, I + 1).

%% list.product — no lists:product/1 BIF.
list_product(List) -> lists:foldl(fun(E, A) -> A * E end, 1, List).
