-module(kex_intrinsic_kex).
-export([backend/0, 'featureHas?'/1, 'featureList'/0, inspect/1]).

backend() -> 'Beam'.

'featureHas?'(Feature) ->
    lists:member(Feature, 'featureList'()).

'featureList'() ->
    ['Http', 'FS', 'Process', 'WebServer'].

%% inspect(Value) -> String — pretty-printed representation.
%%
%% Delegates to kex_io's formatter so there is exactly one notion of how a Kex
%% value renders on BEAM (including its variant-vs-tuple metadata lookup), then
%% strips the ANSI it embeds: `.inspect` returns a String the program may store
%% or compare, and the interpreter's equivalent is colourless under
%% --no-colors, which is the mode golden specs run in.
inspect(Value) ->
    unicode:characters_to_binary(kex_io:inspect_plain(Value)).
