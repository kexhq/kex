%% Kex.Intrinsic.Number — BEAM primitive backend for numeric intrinsics shared
%% by Integer and Float. Receiver is the first argument.
-module(kex_intrinsic_number).
-export([abs/1, sqrt/1]).

abs(N)  -> erlang:abs(N).
sqrt(N) -> math:sqrt(N).
