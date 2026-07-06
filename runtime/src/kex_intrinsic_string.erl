%% Kex.Intrinsic.String — BEAM primitive backend for the string intrinsics.
%% Thin wrappers over Core Erlang's `string` BIFs; the typed string stdlib lives
%% in the Kex prelude (src/prelude/string.kex). Receiver is the first argument.
-module(kex_intrinsic_string).
-export([upperCase/1, lowerCase/1, trim/1, split/2,
         'startsWith?'/2, 'endsWith?'/2]).

upperCase(S) -> string:to_upper(S).
lowerCase(S) -> string:to_lower(S).
trim(S)      -> string:trim(S).
split(S, Sep) -> string:split(S, Sep, all).
%% Kex strings are charlists, so lists:prefix/suffix are the natural primitives
%% (closes a real BEAM gap — these worked on the walker via native builtins but
%% errored under --ir with "not yet ported").
'startsWith?'(S, Pre) -> lists:prefix(Pre, S).
'endsWith?'(S, Suf)   -> lists:suffix(Suf, S).
