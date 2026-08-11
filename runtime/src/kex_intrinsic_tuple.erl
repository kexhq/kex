%% Kex.Intrinsic.Tuple — BEAM primitive backend for the Tuple intrinsics.
%% A Kex tuple is an Erlang tuple; the typed surface lives in the prelude
%% (src/stdlib/string.kex's `make Tuple`).
-module(kex_intrinsic_tuple).
-export([items/1]).

%% items/1 — the tuple's elements as a list. A tuple is not a list in Kex
%% (its arity is part of its type), so this is the explicit conversion.
items(T) when is_tuple(T) -> tuple_to_list(T);
items(L) when is_list(L) -> L;
items(_) -> [].
