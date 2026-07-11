%% Kex.Intrinsic.Range — BEAM primitive backend for the range intrinsics.
%%
%% On BEAM a range is a real list, materialized by make/2 at the `a..b`
%% site. Char bounds ({'Char', N}) produce a [Char] — a list of tagged
%% chars — not an [Int].
-module(kex_intrinsic_range).
-export([make/2, items/1]).

make({'Char', A}, {'Char', B}) -> [{'Char', C} || C <- lists:seq(A, B)];
make(A, B) -> lists:seq(A, B).

%% Materializing an already-materialized range is the identity.
items(L) -> L.
