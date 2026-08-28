%% Kex.Intrinsic.Task — BEAM primitive backend for Task.* namespace functions.
-module(kex_intrinsic_task).
-export([start/1, awaitAll/1, sleep/1]).

start(Fun)   -> kex_task:start(Fun).
awaitAll(Tasks) -> kex_task:await_all(Tasks).
sleep({'Duration', Seconds}) when is_number(Seconds) ->
    timer:sleep(max(0, round(Seconds * 1000))),
    ok.
