-module(kex_intrinsic_kex).
-export([backend/0, 'featureHas?'/1, 'featureList'/0, inspect/1, inspect/2, show/1, kind/1,
         version/0]).

%% Defined by erlc from CMake (see CMakeLists.txt) so this reports the same
%% version the native binary does. The fallbacks keep a bare
%% `erlc runtime/src/*.erl` working outside the build system.
-ifndef(KEX_VERSION_MAJOR).
-define(KEX_VERSION_MAJOR, 0).
-endif.
-ifndef(KEX_VERSION_MINOR).
-define(KEX_VERSION_MINOR, 0).
-endif.
-ifndef(KEX_VERSION_PATCH).
-define(KEX_VERSION_PATCH, 0).
-endif.
-ifndef(KEX_GIT_REVISION).
-define(KEX_GIT_REVISION, "").
-endif.

%% {Major, Minor, Patch, Revision} where Revision is a Kex Optional: a build
%% from a source archive has no commit to name.
version() ->
    Revision = case ?KEX_GIT_REVISION of
                   "" -> 'None';
                   Hash -> {'Just', list_to_binary(Hash)}
               end,
    {?KEX_VERSION_MAJOR, ?KEX_VERSION_MINOR, ?KEX_VERSION_PATCH, Revision}.

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

inspect(Value, true) -> kex_io:inspect_value(Value);
inspect(Value, false) -> inspect(Value).

show(Value) -> unicode:characters_to_binary(kex_io:to_string(Value)).

%% Broad language-level categories for source-owned libraries handling Any.
kind('None') -> none;
kind(true) -> bool;
kind(false) -> bool;
kind(Value) when is_integer(Value) -> integer;
kind(Value) when is_float(Value) -> float;
kind(Value) when is_binary(Value) -> string;
kind(Value) when is_list(Value) -> list;
kind(Value) when is_map(Value) -> map;
kind({'Char', _}) -> char;
kind(Value) when is_atom(Value) -> atom;
kind(Value) when is_tuple(Value) -> tuple;
kind(_) -> other.
