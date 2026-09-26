%% Kex.Intrinsic.Code — Kex.load and Kex.LoadedModule: loading compiled Kex modules into the
%% running VM and calling into them (kexhq/kex#399), without `BEAM.code`,
%% `BEAM.rpc` and inspecting raw tuples to tell success from failure.
-module(kex_intrinsic_code).
-export([load/1, 'loaded?'/1, call/3]).

%% load(Path) -> Ok(ModuleAtom) | Error(Reason). The module's name comes from
%% the file itself, so the caller does not have to know how it was named.
load(Path) ->
    File = kex_io:to_string_bin(Path),
    case file:read_file(File) of
        {ok, Bin} ->
            case beam_lib:chunks(Bin, []) of
                {ok, {Module, _}} ->
                    %% Old code still running in some process is kept; the
                    %% version before it has to go for the load to succeed.
                    _ = code:soft_purge(Module),
                    case code:load_binary(Module, unicode:characters_to_list(File), Bin) of
                        {module, Module} -> {'Ok', Module};
                        {error, Reason} -> failure("cannot load ~ts: ~p", [File, Reason])
                    end;
                {error, beam_lib, Reason} ->
                    failure("~ts is not a compiled module: ~p", [File, Reason])
            end;
        {error, Reason} ->
            failure("cannot read ~ts: ~ts", [File, file:format_error(Reason)])
    end.

'loaded?'(Module) -> code:is_loaded(Module) =/= false.

%% call(Module, Function, Args) -> Ok(Value) | Error(Reason). Everything the
%% call raises is caught here, so a broken module cannot take the caller down.
call(Module, Function, Args) when is_atom(Module), is_atom(Function), is_list(Args) ->
    Arity = length(Args),
    case code:ensure_loaded(Module) of
        {module, Module} ->
            case erlang:function_exported(Module, Function, Arity) of
                true ->
                    try apply(Module, Function, Args) of
                        Value -> {'Ok', Value}
                    catch
                        Class:Reason when is_binary(Reason) ->
                            failure("~ts:~ts/~b raised ~p: ~ts",
                                    [Module, Function, Arity, Class, Reason]);
                        Class:Reason ->
                            failure("~ts:~ts/~b raised ~p: ~p",
                                    [Module, Function, Arity, Class, Reason])
                    end;
                false ->
                    failure("~ts does not export ~ts/~b", [Module, Function, Arity])
            end;
        {error, Reason} ->
            failure("~ts is not loaded: ~p", [Module, Reason])
    end.

failure(Format, Args) ->
    {'Error', unicode:characters_to_binary(io_lib:format(Format, Args))}.
