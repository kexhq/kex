%% Kex.Intrinsic.Map — BEAM primitive backend for the map intrinsics.
%%
%% Maps are unordered; keys/values/entries expose a deterministic CANONICAL
%% order (sorted by key) because plain Erlang map order is non-deterministic
%% across VM invocations. The tree-walker sorts identically (src/interpreter/
%% stdlib/map.cxx). Receiver is the first argument.
-module(kex_intrinsic_map).
-export(['keys'/1, 'values'/1, 'entries'/1, 'merge'/2, 'has?'/2, 'put'/3, 'delete'/2,
         fromEntries/1]).

%% Build a map from a list of {K, V} pairs (used by Map's filter/reject/… which
%% enumerate entries then rebuild a map).
fromEntries(Pairs) -> maps:from_list(Pairs).

keys(M)      -> lists:sort(maps:keys(M)).
values(M)    -> [V || {_, V} <- lists:sort(maps:to_list(M))].
entries(M)   -> lists:sort(maps:to_list(M)).
merge(M, O)  -> maps:merge(M, O).
'has?'(M, K) -> maps:is_key(K, M).
put(M, K, V) -> maps:put(K, V, M).
delete(M, K) -> maps:remove(K, M).
