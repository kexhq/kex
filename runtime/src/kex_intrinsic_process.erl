%% Kex.Intrinsic.Process — BEAM primitive backend for process/concurrency
%% intrinsics. Thin wrappers over Erlang BIFs with Kex's message format.
%% Receiver is the first argument.
-module(kex_intrinsic_process).
-behaviour(gen_server).
-export(['send'/2, 'sendFrom'/2, 'link'/1, 'unlink'/1, 'monitor'/1, 'alive?'/1, 'await'/2,
          'demonitor'/1,
          self/0, exit/2, register/2, whereis/1, run/2, run/3, stream/2,
          spawn/1, 'spawnServing'/3, server_call/4, server_cast/3, reply/1, cast/0,
          replyFrom/2, fromPid/1]).
-export([init/1, handle_call/3, handle_cast/2, handle_info/2]).

%% Execute a program and capture its output with the streams KEPT APART, the
%% same result the tree walker produces.
%%
%% The walker reads two ordinary pipes. A BEAM port has no equivalent: it
%% carries ONE stream, so stderr either folds into stdout or escapes to the
%% emulator's own stderr, uncaptured. The nearest thing the VM can address is
%% a named pipe — nothing is written to disk, the bytes travel through the
%% kernel exactly as with any other pipe, and only the NAME lives in the
%% filesystem so that both sides can find it.
%%
%% The program and its arguments reach the shell as $0 and "$@", passed as
%% separate argv entries rather than interpolated into the script, so no
%% argument can be re-read as shell syntax.
%%
%% Without `mkfifo` or `sh` (Windows) the old merged capture stands in: one
%% combined stream in `stdout` beats losing stderr.
run(Command, Args) ->
    run_with(Command, Args, infinity).

%% Time-budgeted run: the child is killed when `TimeoutMs` milliseconds pass
%% with it still running, and the call answers a timeout Error rather than
%% the child's output. The budget bounds the whole call.
run(Command, Args, TimeoutMs) ->
    case normalize_timeout(TimeoutMs) of
        {ok, Timeout} -> run_with(Command, Args, Timeout);
        {error, Message} -> {'Error', Message}
    end.

normalize_timeout(N) when is_integer(N), N >= 0 -> {ok, N};
normalize_timeout(N) when is_integer(N) -> {ok, 0};
normalize_timeout(_) ->
    {error, <<"Process.run timeout must be an integer number of milliseconds">>}.

run_with(Command, Args, Timeout) ->
    case os:find_executable(unicode:characters_to_list(Command)) of
        false -> {'Error', <<"executable not found">>};
        Executable ->
            Arguments = [unicode:characters_to_list(A) || A <- Args],
            case {os:find_executable("sh"), os:find_executable("mkfifo")} of
                {false, _} -> run_merged(Executable, Arguments, Timeout);
                {_, false} -> run_merged(Executable, Arguments, Timeout);
                {Shell, Mkfifo} ->
                    case make_fifo(Mkfifo) of
                        error -> run_merged(Executable, Arguments, Timeout);
                        {ok, Fifo} ->
                            run_split(Shell, Executable, Arguments, Fifo, Timeout)
                    end
            end
    end.

run_split(Shell, Executable, Arguments, Fifo, Timeout) ->
    %% The reader blocks opening the fifo until a writer appears, and the
    %% shell's `2>fifo` blocks until a reader appears: they release each
    %% other. Reading happens in its own process so the port's stdout is
    %% still being drained meanwhile — otherwise a child that filled one
    %% pipe while we waited on the other would stall.
    Collector = erlang:self(),
    Reader = erlang:spawn(fun() -> Collector ! {kex_stderr, erlang:self(), read_fifo(Fifo)} end),
    Script = "exec \"$0\" \"$@\" 2>'" ++ Fifo ++ "'",
    Port = open_port({spawn_executable, Shell},
                     [binary, exit_status, use_stdio, hide,
                      {args, ["-c", Script, Executable | Arguments]}]),
    Guard = kex_child_guard:protect(Port),
    Result = case collect_port(Port, [], deadline(Timeout)) of
        {ok, {Status, Out}} ->
            kex_child_guard:release(Guard),
            Err = await_stderr(Reader, Fifo),
            file:delete(Fifo),
            {'Ok', {'ProcessResult', Status, Out, Err}};
        timeout ->
            kill_runaway(Port, Reader, Fifo, Guard),
            {'Error', timeout_message(Timeout)}
    end,
    Result.

%% The budget is spent with the child still running: stop it, reclaim the
%% port, and leave no stray process behind. TERM first so a server gets to
%% close up; KILL after a short grace for anything ignoring TERM. There is
%% no BIF for signalling an arbitrary OS process (see kex_child_guard), so
%% this goes through `kill` the same way.
%%
%% The signal goes to the whole PROCESS GROUP, not just the direct child:
%% `erl_child_setup` puts every spawned child in its own group, and a child
%% that already forked (a shell running `sleep`, say) leaves the grandchild
%% holding the port's stdout open — which holds back EOF, which holds back
%% the exit_status this waits for. Killing only the direct child would stall
%% here until the grandchild exits on its own.
kill_runaway(Port, Reader, Fifo, Guard) ->
    OsPid = case erlang:port_info(Port, os_pid) of
        {os_pid, P} when is_integer(P) -> P;
        _ -> undefined
    end,
    signal_group(OsPid, "-TERM"),
    case wait_port_exit(Port, 100) of
        exited -> ok;
        running ->
            signal_group(OsPid, "-KILL"),
            wait_port_exit(Port, 2000)
    end,
    close_port(Port),
    flush_port(Port),
    case Reader of
        undefined -> ok;
        _ -> erlang:exit(Reader, kill)
    end,
    flush_stderr(),
    case Fifo of
        undefined -> ok;
        _ -> file:delete(Fifo)
    end,
    kex_child_guard:release(Guard),
    ok.

%% Signal the child's process group: `-Pid` names the group led by `Pid`.
%% The `--` keeps a negative id from being read as an option.
signal_group(OsPid, Signal) when is_integer(OsPid), OsPid > 0 ->
    try os:cmd("kill " ++ Signal ++ " -- -" ++ erlang:integer_to_list(OsPid)) of
        _ -> ok
    catch
        _:_ -> ok
    end;
signal_group(_, _) -> ok.

wait_port_exit(Port, Ms) ->
    receive
        {Port, {exit_status, _}} -> exited
    after Ms -> running
    end.

%% A port whose child has exited may already have closed itself — closing
%% twice raises badarg — so only close what is still open.
close_port(Port) ->
    case erlang:port_info(Port) of
        undefined -> ok;
        _ ->
            try erlang:port_close(Port) of
                _ -> ok
            catch
                _:_ -> ok
            end
    end.

%% The port is closed: nothing new can arrive, so drain what is left without
%% waiting. Stale port and stderr messages must not leak into this process's
%% mailbox and confuse a later receive.
flush_port(Port) ->
    receive
        {Port, _} -> flush_port(Port)
    after 0 -> ok
    end.

flush_stderr() ->
    receive
        {kex_stderr, _, _} -> flush_stderr()
    after 0 -> ok
    end.

deadline(infinity) -> infinity;
deadline(Ms) -> erlang:monotonic_time(millisecond) + Ms.

timeout_message(Ms) ->
    list_to_binary(["timed out after ", integer_to_list(Ms), "ms"]).

%% Process.stream — run a program and let its output through AS IT ARRIVES,
%% answering only the exit code.
%%
%% `run/2` above captures both streams and can only answer once the child has
%% exited, so anything long-running shows nothing and then dumps everything at
%% the end — `tey test` and a package's own commands looked stalled for their
%% whole run (kexhq/kex#187).
%%
%% stderr is folded into stdout (`stderr_to_stdout`) rather than separated the
%% way `run/2` does it: the two are being written straight through to the same
%% terminal, so keeping them apart would only reorder them against each other.
%%
%% Unlike the tree walker — which forks and lets the child inherit the real
%% descriptors — a port is a pipe, so a child that asks whether it is talking
%% to a terminal is told no, and may drop its own colours. Live output is
%% worth that; a child whose colours matter can be told explicitly.
stream(Command, Args) ->
    case os:find_executable(unicode:characters_to_list(Command)) of
        false -> {'Error', <<"executable not found">>};
        Executable ->
            Arguments = [unicode:characters_to_list(A) || A <- Args],
            Port = open_port({spawn_executable, Executable},
                             [binary, exit_status, use_stdio, stderr_to_stdout,
                              hide, {args, Arguments}]),
            %% A child that READS is as common as one that writes — `tey repl`
            %% and `tey run` on a program that asks a question both do — and a
            %% port's stdin is written by the parent rather than inherited, so
            %% without this the child prints its prompt and then waits for
            %% input that can never arrive. The feeder reads this process's
            %% own stdin and forwards it, in its own process so that waiting
            %% for a line never stops the output loop below.
            %% A port's child is in its own process group and nothing
            %% reaps it, so it outlives this VM with its socket still bound:
            %% `tey run` exited and left the server it started serving. See
            %% kex_child_guard.
            Guard = kex_child_guard:protect(Port),
            Feeder = erlang:spawn(fun() -> feed_port(Port) end),
            Status = pump_port(Port),
            %% The child is gone; whatever the feeder is waiting for is no
            %% longer wanted, and leaving it blocked on stdin would eat the
            %% next thing typed at Tey itself.
            erlang:exit(Feeder, kill),
            kex_child_guard:release(Guard),
            {'Ok', Status}
    end.

feed_port(Port) ->
    case io:get_line('') of
        eof -> ok;
        {error, _} -> ok;
        Line ->
            %% `port_command` raises once the child has exited, which is a
            %% race this cannot avoid: the line was typed before the exit
            %% arrived. Losing it is correct — there is nobody to read it.
            try erlang:port_command(Port, unicode:characters_to_binary(Line)) of
                true -> feed_port(Port)
            catch
                _:_ -> ok
            end
    end.

%% Every chunk the child writes goes straight out, and the loop ends with the
%% exit status.
%%
%% A port hands over BYTES, and a chunk boundary can fall inside a multi-byte
%% character: the spec runner's `✓` is three bytes, and `io:put_chars/1` on a
%% truncated sequence raises `badarg` — which surfaced as `tey: badarg` from
%% the launcher, on Alpine, in the middle of `tey test`. So an incomplete tail
%% is held back and prepended to the next chunk.
pump_port(Port) -> pump_port(Port, <<>>).

pump_port(Port, Buffered) ->
    receive
        {Port, {data, Bin}} ->
            pump_port(Port, emit(<<Buffered/binary, Bin/binary>>));
        {Port, {exit_status, Status}} ->
            %% Whatever is left cannot become valid — the child is gone — but
            %% it is output the child produced, so it goes out as raw bytes
            %% rather than being dropped.
            case Buffered of
                <<>> -> ok;
                Rest -> file:write(standard_io, Rest)
            end,
            Status
    end.

%% Writes every COMPLETE character in `Chunk` and answers the incomplete tail
%% to carry forward. Invalid bytes (not merely incomplete ones) are written
%% raw: they came from the child, and a terminal showing them wrongly beats
%% output that silently disappears.
emit(Chunk) ->
    case unicode:characters_to_binary(Chunk, unicode, unicode) of
        {incomplete, Encoded, Rest} ->
            io:put_chars(Encoded),
            Rest;
        {error, Encoded, _Rest} ->
            io:put_chars(Encoded),
            <<>>;
        Encoded when is_binary(Encoded) ->
            io:put_chars(Encoded),
            <<>>
    end.

run_merged(Executable, Arguments, Timeout) ->
    Port = open_port({spawn_executable, Executable},
                     [binary, exit_status, use_stdio, stderr_to_stdout,
                      hide, {args, Arguments}]),
    Guard = kex_child_guard:protect(Port),
    Result = case collect_port(Port, [], deadline(Timeout)) of
        {ok, {Status, Out}} ->
            kex_child_guard:release(Guard),
            {'Ok', {'ProcessResult', Status, Out, <<>>}};
        timeout ->
            kill_runaway(Port, undefined, undefined, Guard),
            {'Error', timeout_message(Timeout)}
    end,
    Result.

make_fifo(Mkfifo) ->
    Path = fifo_path(),
    Port = open_port({spawn_executable, Mkfifo},
                     [binary, exit_status, use_stdio, hide, {args, [Path]}]),
    case collect_port(Port, []) of
        {ok, {0, _}} -> {ok, Path};
        _            -> error
    end.

%% Unique per call: two runs at once must not read each other's stderr. No
%% quote character can reach the script, because the name is built here.
fifo_path() ->
    Directory = case os:getenv("TMPDIR") of
                    false -> "/tmp";
                    ""    -> "/tmp";
                    Value -> Value
                end,
    Name = "kex_process_stderr_" ++ os:getpid() ++ "_" ++
           integer_to_list(erlang:unique_integer([positive])),
    filename:join(Directory, Name).

read_fifo(Fifo) ->
    case file:open(Fifo, [read, binary, raw]) of
        {ok, Handle} ->
            Data = read_fifo(Handle, []),
            file:close(Handle),
            Data;
        _ ->
            <<>>
    end.

read_fifo(Handle, Chunks) ->
    case file:read(Handle, 65536) of
        {ok, Chunk} -> read_fifo(Handle, [Chunk | Chunks]);
        _           -> iolist_to_binary(lists:reverse(Chunks))
    end.

%% The command has already exited here, so its stderr is complete. A reader
%% still parked in open/2 never saw a writer — the shell died before the
%% redirect — so open the write end briefly to hand it EOF rather than wait
%% on a process that cannot finish.
await_stderr(Reader, Fifo) ->
    receive
        {kex_stderr, Reader, Data} -> Data
    after 100 ->
        _ = erlang:spawn(fun() ->
                      case file:open(Fifo, [write, binary, raw]) of
                          {ok, Handle} -> file:close(Handle);
                          _            -> ok
                      end
                  end),
        receive
            {kex_stderr, Reader, Data} -> Data
        after 5000 ->
            erlang:exit(Reader, kill),
            <<>>
        end
    end.

collect_port(Port, Chunks) ->
    collect_port(Port, Chunks, infinity).

%% Deadline-bounded collect: `infinity` waits for the child the way the
%% untimed run always has; an integer deadline answers `timeout` instead.
%% The deadline is absolute — each loop waits only for what is left of the
%% budget, so a child that keeps trickling output still runs out of time.
collect_port(Port, Chunks, infinity) ->
    receive
        {Port, {data, Data}} -> collect_port(Port, [Data | Chunks], infinity);
        {Port, {exit_status, Status}} ->
            {ok, {Status, iolist_to_binary(lists:reverse(Chunks))}}
    end;
collect_port(Port, Chunks, Deadline) ->
    Timeout = max(0, Deadline - erlang:monotonic_time(millisecond)),
    receive
        {Port, {data, Data}} ->
            collect_port(Port, [Data | Chunks], Deadline);
        {Port, {exit_status, Status}} ->
            {ok, {Status, iolist_to_binary(lists:reverse(Chunks))}}
    after Timeout ->
        timeout
    end.

%% Raw BEAM messaging, plus the conventional sender-bearing {Pid, Payload} form.
'send'(Pid, Msg) -> erlang:send(Pid, Msg).
'sendFrom'(Pid, Msg) -> erlang:send(Pid, {erlang:self(), Msg}).

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

%% A typed Kex Server<X> is represented by the ordinary Server record tuple.
%% The process itself is a real OTP gen_server; slot-specific code arrives as
%% a closure so this generic callback module can serve every Kex state type.
spawn(State) ->
    'spawnServing'(State, undefined, []).

'spawnServing'(State, Module, Slots) ->
    case gen_server:start_link(?MODULE, {State, Module, Slots}, []) of
        {ok, Pid} -> {'Server', Pid, 5000};
        {error, Reason} -> erlang:error(Reason)
    end.

server_call({'Server', Pid, DefaultTimeout}, Method, Args, Timeout) ->
    Effective = case Timeout of default -> DefaultTimeout; _ -> Timeout end,
    Request = list_to_tuple([Method | Args]),
    try gen_server:call(Pid, Request, Effective) of
        Reply -> {'Ok', Reply}
    catch
        exit:{timeout, _} -> {'Error', 'Timeout'};
        exit:{noproc, _} -> {'Error', 'NoProcess'};
        exit:_ -> {'Error', 'CallFailed'}
    end.

server_cast({'Server', Pid, _DefaultTimeout}, Method, Args) ->
    gen_server:cast(Pid, list_to_tuple([Method | Args])).

reply(Value) -> {'Reply', Value}.
cast() -> ok.
'replyFrom'({'From', Pid, Tag}, Value) -> gen_server:reply({Pid, Tag}, Value).
'fromPid'({'From', Pid, _Tag}) -> Pid.

init({State, Module, Slots}) ->
    erlang:put(kex_serving_module, Module),
    erlang:put(kex_serving_slots, Slots),
    {ok, State}.

handle_call(Request, {Pid, Tag}, State) ->
    erlang:put(kex_serving_from, {'From', Pid, Tag}),
    Result = invoke_slot(Request, State),
    erlang:erase(kex_serving_from),
    case Result of
        {'Reply', Reply} -> {reply, Reply, State};
        {'Transition', Next, Reply} -> {reply, Reply, Next};
        #{stop := Reason, new := Next, reply := Reply} -> {stop, Reason, Reply, Next};
        #{stop := Reason, state := Next, reply := Reply} -> {stop, Reason, Reply, Next};
        #{stop := Reason, reply := Reply} -> {stop, Reason, Reply, State};
        #{new := Next, reply := Reply} -> {reply, Reply, Next};
        #{state := Next, reply := Reply} -> {reply, Reply, Next};
        #{reply := Reply} -> {reply, Reply, State};
        #{new := Next} -> {noreply, Next};
        #{state := Next} -> {noreply, Next};
        Other -> erlang:error({invalid_serving_reply, Other})
    end.

handle_cast(Request, State) ->
    case invoke_slot(Request, State) of
        {'Transition', Next, _} -> {noreply, Next};
        #{stop := Reason, new := Next} -> {stop, Reason, Next};
        #{stop := Reason, state := Next} -> {stop, Reason, Next};
        #{stop := Reason} -> {stop, Reason, State};
        #{new := Next} -> {noreply, Next};
        #{state := Next} -> {noreply, Next};
        Next when is_tuple(Next) -> {noreply, Next};
        _ -> {noreply, State}
    end.

%% Task.start monitors its worker and reports completion to the process that
%% started it. Deferred slots intentionally do not await that task: the task's
%% useful result is delivered through From.reply, so consume its bookkeeping.
handle_info({kex_task_done, _Ref, _Result}, State) -> {noreply, State};
handle_info({'DOWN', _Ref, process, _Pid, normal}, State) -> {noreply, State};
handle_info(_Message, State) -> {noreply, State}.

invoke_slot(Request, State) when is_tuple(Request), tuple_size(Request) >= 1 ->
    [Method | Args] = tuple_to_list(Request),
    Module = erlang:get(kex_serving_module),
    Slots = erlang:get(kex_serving_slots),
    PlainArgs = [State | Args],
    case lists:member(Method, Slots) of
        false -> erlang:error({unknown_serving_slot, Method});
        true ->
            case erlang:function_exported(Module, Method, length(PlainArgs)) of
                true -> erlang:apply(Module, Method, PlainArgs);
                false -> erlang:apply(Module, Method, PlainArgs ++ [#{}])
            end
    end;
invoke_slot(Method, State) when is_atom(Method) ->
    invoke_slot({Method}, State);
invoke_slot(Request, _State) -> erlang:error({unknown_serving_request, Request}).
