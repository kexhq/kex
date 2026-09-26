%% Kex.Intrinsic.Shared — Process.Shared<X> over persistent_term
%% (kexhq/kex#402): reading copies nothing, while replacing a value costs a
%% scan of every process for references to the old one.
%%
%% A handle is its persistent_term key, tagged with the type's name the way
%% an opaque value is, so method dispatch finds `Process.Shared`.
-module(kex_intrinsic_shared).
-export([named/1, put/2, get/1, delete/1]).

named(Name) -> {'Process.Shared', kex_io:to_string_bin(Name)}.

put(Key, Value) ->
    persistent_term:put(Key, Value),
    ok.

get(Key) ->
    case persistent_term:get(Key, '$kex_shared_empty') of
        '$kex_shared_empty' -> 'None';
        Value -> {'Just', Value}
    end.

delete(Key) -> persistent_term:erase(Key).
