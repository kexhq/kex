%% Kex.Intrinsic.Range — BEAM primitive backend for the range intrinsics.
%%
%% On BEAM a range is already a real list (the compiler lowers `a..b` to
%% lists:seq/2 before any method call), so materializing is the identity.
-module(kex_intrinsic_range).
-export([items/1]).

items(L) -> L.
