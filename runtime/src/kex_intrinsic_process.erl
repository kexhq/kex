%% Kex.Intrinsic.Process — BEAM primitive backend for process/concurrency
%% intrinsics. Thin wrappers over Erlang BIFs with Kex's message format.
%% Receiver is the first argument.
-module(kex_intrinsic_process).
-export(['send'/2, 'link'/1, 'unlink'/1, 'monitor'/1, 'alive?'/1, 'await'/2,
          'demonitor'/1,
          self/0, exit/2, register/2, whereis/1, run/2]).

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
    case os:find_executable(unicode:characters_to_list(Command)) of
        false -> {'Error', <<"executable not found">>};
        Executable ->
            Arguments = [unicode:characters_to_list(A) || A <- Args],
            case {os:find_executable("sh"), os:find_executable("mkfifo")} of
                {false, _} -> run_merged(Executable, Arguments);
                {_, false} -> run_merged(Executable, Arguments);
                {Shell, Mkfifo} ->
                    case make_fifo(Mkfifo) of
                        error -> run_merged(Executable, Arguments);
                        {ok, Fifo} ->
                            Result = run_split(Shell, Executable, Arguments, Fifo),
                            file:delete(Fifo),
                            Result
                    end
            end
    end.

run_split(Shell, Executable, Arguments, Fifo) ->
    %% The reader blocks opening the fifo until a writer appears, and the
    %% shell's `2>fifo` blocks until a reader appears: they release each
    %% other. Reading happens in its own process so the port's stdout is
    %% still being drained meanwhile — otherwise a child that filled one
    %% pipe while we waited on the other would stall.
    Collector = erlang:self(),
    Reader = spawn(fun() -> Collector ! {kex_stderr, erlang:self(), read_fifo(Fifo)} end),
    Script = "exec \"$0\" \"$@\" 2>'" ++ Fifo ++ "'",
    Port = open_port({spawn_executable, Shell},
                     [binary, exit_status, use_stdio, hide,
                      {args, ["-c", Script, Executable | Arguments]}]),
    {Status, Out} = collect_port(Port, []),
    Err = await_stderr(Reader, Fifo),
    {'Ok', {'ProcessResult', Status, Out, Err}}.

run_merged(Executable, Arguments) ->
    Port = open_port({spawn_executable, Executable},
                     [binary, exit_status, use_stdio, stderr_to_stdout,
                      hide, {args, Arguments}]),
    {Status, Out} = collect_port(Port, []),
    {'Ok', {'ProcessResult', Status, Out, <<>>}}.

make_fifo(Mkfifo) ->
    Path = fifo_path(),
    Port = open_port({spawn_executable, Mkfifo},
                     [binary, exit_status, use_stdio, hide, {args, [Path]}]),
    case collect_port(Port, []) of
        {0, _} -> {ok, Path};
        _      -> error
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
        _ = spawn(fun() ->
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
    receive
        {Port, {data, Data}} -> collect_port(Port, [Data | Chunks]);
        {Port, {exit_status, Status}} ->
            {Status, iolist_to_binary(lists:reverse(Chunks))}
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
