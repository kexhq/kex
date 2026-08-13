%% Kex.Intrinsic.Interface — reading a compiled module's KexI chunk.
%%
%% KexI is Kex's module-interface format: a custom BEAM chunk holding the
%% typed public surface of a module, stored as an Erlang external term. Both
%% halves of that — locating a named chunk and decoding a term — are Erlang
%% library calls, so a Kex program that wanted its own interface data had to
%% reach through `Erlang.Beam_lib.chunks` and `Erlang.Erlang.binary_to_term`.
%% This is the one intentional entry point instead.
%%
%% Deliberately NOT a general `binary_to_term`: decoding an arbitrary term
%% from untrusted bytes is a known hazard, and nothing in the language needs
%% that. Narrowing it to "the KexI chunk of a BEAM file the caller named"
%% keeps the capability without handing out the hazard.
%%
%% The `safe` option is NOT usable here, despite being the obvious guard: a
%% KexI chunk is mostly atoms the running VM has never seen — type tags,
%% module and function names — which is precisely what the chunk is for, and
%% `safe` refuses to create them. It fails on every real chunk.
-module(kex_intrinsic_interface).
-export([read/1]).

read(Path) when is_binary(Path) -> read(binary_to_list(Path));
read(Path) when is_list(Path) ->
    case beam_lib:chunks(Path, ["KexI"]) of
        {ok, {_Module, [{"KexI", Bin}]}} ->
            try {'Just', binary_to_term(Bin)}
            catch _:_ -> 'None'
            end;
        _ -> 'None'
    end;
read(_) -> 'None'.
