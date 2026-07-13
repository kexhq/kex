%% Kex.Intrinsic.Process — BEAM primitive backend for process/concurrency
%% intrinsics. Thin wrappers over Erlang BIFs with Kex's message format.
%% Receiver is the first argument.
-module(kex_intrinsic_process).
-export(['send'/2, 'link'/1, 'unlink'/1, 'monitor'/1, 'alive?'/1, 'await'/2,
          'demonitor'/1]).

%% pid.send(msg) — send a Kex-formatted message.
'send'(Pid, Msg) -> erlang:send(Pid, {'kex_msg', Msg, erlang:self()}).

%% pid.link() — bidirectional exit propagation.
'link'(Pid) -> erlang:link(Pid).

%% pid.unlink() — remove the link.
'unlink'(Pid) -> erlang:unlink(Pid).

%% pid.monitor() — start monitoring, returns a reference for demonitor.
'monitor'(Pid) -> erlang:monitor(process, Pid).

%% pid.alive?() — check if the process is currently alive.
'alive?'(Pid) -> erlang:is_process_alive(Pid).

%% task.await(timeout) — await a task's result. The timeout is in milliseconds;
%% defaults to 'infinity' in Core Erlang's receive-after construct. Returns
%% {'Just', Value} on the normal 'kex_result' message, or 'None' on timeout.
'await'(Task, Timeout) -> kex_task:await(Task, Timeout).

%% ref.demonitor() — stop monitoring. The receiver is the reference returned
%% by pid.monitor(). Returns 'true'.
'demonitor'(Ref) -> erlang:demonitor(Ref).
