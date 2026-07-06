%% Kex.Intrinsic.List — BEAM primitive backend for the list intrinsics.
%%
%% The ONLY place Core Erlang list BIFs are named for the stdlib. The typed
%% list stdlib lives in the Kex prelude (src/prelude/list.kex), which calls
%% `Kex.Intrinsic.List.<fn>`; the compiler maps that module path to a plain
%% cross-module call here. Adding a primitive is a one-line function.
-module(kex_intrinsic_list).
-export([reverse/1, sort/1, uniq/1, flatten/1, take/2, drop/2, zip/2, push/2,
         sum/1, product/1, indexOf/2, at/2,
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

%% list_get/2,3 — `list[i]` / `list.get(i[, default])`. Returns the raw element
%% (or none/Default if out of range) — NOT Just(value)-wrapped, unlike Map.get's
%% 2-arg form.
list_get(List, Idx) -> list_get(List, Idx, 'none').
list_get(List, Idx, _Default) when is_integer(Idx), Idx >= 0, Idx < length(List) ->
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
