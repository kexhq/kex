-module(kex_task).
-export([start/1, await/2, await_all/1]).

%% Task.start { expr } — spawn a monitored process running a 0-arity fun.
%% Returns {Ref, Pid} as the task handle.
start(Fun) ->
    Ref = make_ref(),
    Parent = self(),
    Pid = spawn(fun() ->
        Result = Fun(),
        Parent ! {'kex_task_done', Ref, {ok, Result}}
    end),
    erlang:monitor(process, Pid),
    {Ref, Pid}.

%% task.await(timeout: T) — wait up to T ms for the task result.
%% Returns {ok, Value} | {error, timeout} | {error, {exit, Reason}}.
await({Ref, Pid}, Timeout) ->
    receive
        {'kex_task_done', Ref, Result} ->
            erlang:demonitor(Ref, [flush]),
            Result;
        {'DOWN', _MonRef, process, Pid, Reason} ->
            {error, {exit, Reason}}
    after Timeout ->
        erlang:demonitor(Ref, [flush]),
        {error, timeout}
    end.

%% await_all([Task]) — await a list of tasks, return list of results.
await_all(Tasks) ->
    lists:map(fun(T) -> await(T, infinity) end, Tasks).
