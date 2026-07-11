-module(kex_repl_driver).
-export([loop/0]).

%% Drives a persistent BEAM VM for the interactive `kex -R` REPL. The C++
%% side (src/main.cxx's beam-repl mode) keeps ONE `erl -noshell -eval
%% 'kex_repl_driver:loop()'` process alive for the whole session and talks
%% to it over stdin/stdout with a nonce-delimited protocol:
%%
%%   C++ -> driver:  "load <nonce> <Module> <BeamFile>\n"
%%   C++ -> driver:  "eval <nonce> <Module>\n"
%%   C++ -> driver:  "quit\n"
%%
%%   driver -> C++:  "KEX_REPL_DONE <nonce> ok\n" | "... error\n"
%%
%% Program IO (IO.printLine / IO.inspect via kex_io) goes to the SAME stdout
%% BEFORE the sentinel; C++ reads until it sees the matching sentinel line and
%% treats everything above it as the command's output. The nonce (chosen fresh
%% per command by C++) makes the delimiter unforgeable by program output.
%%
%% Why this exists instead of shelling out to a fresh `erl` per line: a
%% persistent VM is what makes `p = spawn { ... }` on one line reachable from
%% `p.send(m)` on the next — the spawned process, any registered name, and any
%% ETS table survive across REPL inputs. Hot-loading the recompiled session
%% module via code:load_binary is BEAM's native code-upgrade path, not a re-run
%% from a cold VM each line.

loop() ->
    case io:get_line("") of
        eof ->
            halt(0);
        {error, _} ->
            halt(1);
        Line ->
            handle(string:trim(Line, both, "\r\n")),
            loop()
    end.

handle("quit") ->
    halt(0);
handle(Cmd) ->
    case string:tokens(Cmd, " ") of
        ["load", Nonce, Mod, BeamFile] ->
            load_module(Nonce, Mod, BeamFile);
        ["eval", Nonce, Mod] ->
            eval_module(Nonce, Mod);
        _ ->
            io:format("bad driver command: ~ts~n", [Cmd]),
            halt(1)
    end.

load_module(Nonce, Mod, BeamFile) ->
    case catch begin
        {ok, Bin} = file:read_file(BeamFile),
        code:load_binary(list_to_atom(Mod), BeamFile, Bin)
    end of
        {module, _} ->
            sentinel(Nonce, ok);
        Else ->
            io:format("load failed: ~p~n", [Else]),
            sentinel(Nonce, error)
    end.

%% The session module's main/0 wraps the entered expression in IO.inspect, so
%% its side effect (printing the value) is the result. A Kex runtime error
%% surfaces as error/exit with a String reason (printed verbatim); raw BEAM
%% terms fall back to ~p — mirrors the -R run path in main.cxx.
eval_module(Nonce, Mod) ->
    try apply(list_to_atom(Mod), main, []) of
        _ -> sentinel(Nonce, ok)
    catch
        error:Reason -> report(Nonce, Reason);
        exit:Reason  -> report(Nonce, Reason)
    end.

report(Nonce, Reason) ->
    Msg = case Reason of
        B when is_binary(B); is_list(B) -> B;
        _ -> io_lib:format("~p", [Reason])
    end,
    io:format("~ts~n", [Msg]),
    sentinel(Nonce, error).

sentinel(Nonce, Status) ->
    io:format("KEX_REPL_DONE ~s ~s~n", [Nonce, Status]),
    ok.
