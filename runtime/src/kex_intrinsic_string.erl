%% Kex.Intrinsic.String — BEAM primitive backend for the string intrinsics.
%% Thin wrappers over Core Erlang's `string` BIFs; the typed string stdlib lives
%% in the Kex prelude (src/prelude/string.kex). Receiver is the first argument.
-module(kex_intrinsic_string).
-export([upperCase/1, lowerCase/1, trim/1, split/2]).

upperCase(S) -> string:to_upper(S).
lowerCase(S) -> string:to_lower(S).
trim(S)      -> string:trim(S).
split(S, Sep) -> string:split(S, Sep, all).
