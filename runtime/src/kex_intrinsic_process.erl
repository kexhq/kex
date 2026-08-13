%% Kex.Intrinsic.Process — BEAM primitive backend for process/concurrency
%% intrinsics. Thin wrappers over Erlang BIFs with Kex's message format.
%% Receiver is the first argument.
-module(kex_intrinsic_process).
-export(['send'/2, 'link'/1, 'unlink'/1, 'monitor'/1, 'alive?'/1, 'await'/2,
          'demonitor'/1,
          self/0, exit/2, register/2, whereis/1, run/2]).

%% Execute directly (never through a shell) and capture output. Erlang ports
%% combine stderr into stdout portably; the native walker can keep the streams
%% separate, but callers must be prepared for BEAM's stderr field to be empty.
run(Command, Args) ->
    Executable = os:find_executable(unicode:characters_to_list(Command)),
    case Executable of
        false -> {'Error', <<"executable not found">>};
        _ ->
            Port = open_port({spawn_executable, Executable},
                             [binary, exit_status, use_stdio, stderr_to_stdout,
                              hide, {args, [unicode:characters_to_list(A) || A <- Args]}]),
            collect_port(Port, [])
    end.

collect_port(Port, Chunks) ->
    receive
        {Port, {data, Data}} -> collect_port(Port, [Data | Chunks]);
        {Port, {exit_status, Status}} ->
            {'Ok', {'ProcessResult', Status,
                    iolist_to_binary(lists:reverse(Chunks)), <<>>}}
    end.

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

%% Process.self() — current process identifier.
self() -> erlang:self().

%% Process.exit(pid, reason) — send an exit signal.
exit(Pid, Reason) -> erlang:exit(Pid, Reason).

%% Process.register(pid, name) — register a process under an atom name.
register(Pid, Name) -> erlang:register(Name, Pid).

%% Process.whereis(name) — look up a registered process by name.
whereis(Name) -> erlang:whereis(Name).
