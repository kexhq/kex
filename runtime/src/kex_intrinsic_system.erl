%% Kex.Intrinsic.System — BEAM primitive backend for System.* functions.
-module(kex_intrinsic_system).
-export([exit/1]).

exit(Code) -> erlang:halt(Code).
