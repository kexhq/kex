%% Ties a port's OS child to the lifetime of this VM.
%%
%% `erl_child_setup` gives every spawned child its OWN process group, so a
%% terminal's Ctrl+C reaches the VM and never the child — and nothing reaps it
%% afterwards. A port's child outlives the owning VM whether that VM halts
%% cleanly or is killed, with whatever it held still held: `tey run` exited and
%% left the server it had started answering on its port, so the next run met
%% `eaddrinuse` (docs/rodolfo-findings.md). Closing the port only gives the
%% child EOF on stdin, which a server never reads.
%%
%% So the child is claimed explicitly, from both directions:
%%
%%   * a SIGTERM to the VM runs `erl_signal_server`, and this module is added
%%     there as a gen_event handler that kills the child before shutdown gets
%%     any further. It deliberately does NOT call `init:stop/0` — the server's
%%     own default handler does that, and gen_event notifies every handler
%%     before returning, so both run.
%%   * the process owning the port dying — an exception, a `kill` — is seen by
%%     a guardian monitoring it.
%%
%% SIGINT is not reachable this way: it belongs to the VM's break handler, and
%% `os:set_signal(sigint, handle)` is a `badarg`. A VM that survives SIGINT
%% keeps its children, which is consistent if not always what a person wants.
-module(kex_child_guard).
-behaviour(gen_event).

-export([protect/1, release/1]).
-export([init/1, handle_event/2, handle_call/2, handle_info/2, terminate/2]).

-record(guard, {handler :: term(), guardian :: pid()}).

%% Claim `Port`'s OS child for as long as the calling process holds it.
%% Answers a token for `release/1`, or `undefined` when there is nothing to
%% claim (no OS child, or the signal server is unavailable).
-spec protect(port()) -> #guard{} | undefined.
protect(Port) ->
    case erlang:port_info(Port, os_pid) of
        {os_pid, OsPid} when is_integer(OsPid) ->
            Owner = erlang:self(),
            Guardian = erlang:spawn(fun() -> guard(Owner, OsPid) end),
            Handler = {?MODULE, erlang:make_ref()},
            case add_signal_handler(Handler, OsPid) of
                ok -> #guard{handler = Handler, guardian = Guardian};
                error -> #guard{handler = undefined, guardian = Guardian}
            end;
        _ ->
            undefined
    end.

%% The child has exited on its own; stop watching for it. Called on the
%% ordinary path, so it must not raise whatever state the guard is in.
-spec release(#guard{} | undefined) -> ok.
release(undefined) -> ok;
release(#guard{handler = Handler, guardian = Guardian}) ->
    case is_pid(Guardian) of
        true -> erlang:exit(Guardian, kill);
        false -> ok
    end,
    case Handler of
        undefined -> ok;
        _ ->
            try gen_event:delete_handler(erl_signal_server, Handler, [])
            catch _:_ -> ok
            end,
            ok
    end,
    ok.

add_signal_handler(Handler, OsPid) ->
    try gen_event:add_handler(erl_signal_server, Handler, [OsPid]) of
        ok -> ok;
        _ -> error
    catch
        _:_ -> error
    end.

%% The owner is gone and the child is not: nobody is reading its output and
%% nobody will reap it.
guard(Owner, OsPid) ->
    erlang:monitor(process, Owner),
    receive
        {'DOWN', _Reference, process, Owner, _Reason} -> terminate_os_process(OsPid)
    end.

%% There is no BIF for signalling an arbitrary OS process, so this goes
%% through `kill`. SIGTERM rather than SIGKILL: a server gets to close its
%% listener, and anything ignoring it was going to need SIGKILL from a person
%% either way.
terminate_os_process(OsPid) when is_integer(OsPid), OsPid > 0 ->
    try os:cmd("kill -TERM " ++ erlang:integer_to_list(OsPid))
    catch _:_ -> ok
    end,
    ok;
terminate_os_process(_) ->
    ok.

%% ---- gen_event: the VM is being asked to stop -------------------------

init([OsPid]) -> {ok, OsPid}.

handle_event(Signal, OsPid) when Signal =:= sigterm; Signal =:= sighup ->
    terminate_os_process(OsPid),
    {ok, OsPid};
handle_event(_Signal, OsPid) ->
    {ok, OsPid}.

handle_call(_Request, OsPid) -> {ok, ok, OsPid}.

handle_info(_Info, OsPid) -> {ok, OsPid}.

terminate(_Reason, _OsPid) -> ok.
