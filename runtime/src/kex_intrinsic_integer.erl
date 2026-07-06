%% Kex.Intrinsic.Integer — BEAM primitive backend for integer intrinsics.
%% The typed Integer stdlib lives in src/prelude/integer.kex; `even?`/`odd?`
%% are expressed there in Kex on top of `modulo`. Receiver is the first arg.
-module(kex_intrinsic_integer).
-export([modulo/2]).

modulo(A, B) -> A rem B.
