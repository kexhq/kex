%% Kex.Intrinsic.Retry — secure production randomness for retry jitter.
-module(kex_intrinsic_retry).
-export([randomUnit/0]).

randomUnit() ->
    <<Value:53/unsigned-integer, _/bitstring>> = crypto:strong_rand_bytes(8),
    Value / 9007199254740992.
