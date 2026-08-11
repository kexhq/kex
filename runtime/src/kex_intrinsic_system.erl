%% Kex.Intrinsic.System — BEAM primitive backend for System.* functions.
-module(kex_intrinsic_system).
-export([exit/1, die/1]).

exit(Code) -> erlang:halt(Code).

%% die(Msg) — the walker's `die` (src/interpreter/stdlib/io.cxx): the message
%% goes to STDERR prefixed with "fatal: ", then the process exits with 1. Not
%% an exception, so `trying`/`rescue` cannot catch it on either backend.
die(Msg) ->
    io:format(standard_error, "fatal: ~ts~n", [kex_io:to_string_bin(Msg)]),
    erlang:halt(1).
