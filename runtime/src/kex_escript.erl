%% The entry point of an escript built from a Kex program (`tey build`'s
%% targets).
%%
%% `escript` calls `Module:main(Args)` with each argument as a charlist, while
%% Kex's own runner hands `main(args)` binaries — so the same program saw
%% `[String]` under `kex --run` and lists of code points once installed
%% (kexhq/kex#406). Starting the escript here instead gives it the runner's
%% contract: UTF-8 standard streams, `String` arguments, `main/0` when the
%% program declares no parameters, and a runtime error reported the same way
%% with exit status 1.
%%
%% The program's own module is named by the `-kex_entry` emulator flag the
%% escript header carries, since every entry module is named after its file
%% (`kex_<stem>`). An escript written before that flag existed names no entry,
%% and gets `kex_main`, the module `src/main.kex` compiles to.
-module(kex_escript).

-export([main/1]).

main(Arguments) ->
    io:setopts(standard_io, [{encoding, unicode}]),
    io:setopts(standard_error, [{encoding, unicode}]),
    Entry = entry(),
    try
        case lists:member({main, 1}, Entry:module_info(exports)) of
            true -> Entry:main([unicode:characters_to_binary(A) || A <- Arguments]);
            false -> Entry:main()
        end
    of
        _ -> halt(0)
    catch
        _:Reason when is_binary(Reason); is_list(Reason) ->
            io:format(standard_error, "Internal error: ~ts~n", [Reason]),
            halt(1);
        _:Reason ->
            io:format(standard_error, "Internal error: ~p~n", [Reason]),
            halt(1)
    end.

entry() ->
    case init:get_argument(kex_entry) of
        {ok, [[Name | _] | _]} -> list_to_atom(Name);
        _ -> kex_main
    end.
