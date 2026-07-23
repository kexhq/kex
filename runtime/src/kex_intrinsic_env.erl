%% Kex.Intrinsic.Env — BEAM primitive backend for ENV namespace functions.
%%
%% ENV is an immutable Map<String, String> snapshot of the process environment.
%% Each function calls kex_io:env_map() to retrieve the snapshot and applies
%% the corresponding map operation.
-module(kex_intrinsic_env).
-export(['get'/1, 'getWithDefault'/2, 'has?'/1, 'keys'/0, 'values'/0,
         'count'/0, 'each'/1, 'entries'/0]).

'get'(K) ->
    M = kex_io:env_map(),
    case maps:find(K, M) of
        {ok, V} -> {'Just', V};
        error   -> 'None'
    end.

'getWithDefault'(K, Default) ->
    maps:get(K, kex_io:env_map(), Default).

'has?'(K) ->
    maps:is_key(K, kex_io:env_map()).

'keys'() ->
    lists:sort(maps:keys(kex_io:env_map())).

'values'() ->
    M = kex_io:env_map(),
    [V || {_, V} <- lists:sort(maps:to_list(M))].

'count'() ->
    maps:size(kex_io:env_map()).

'each'(F) ->
    maps:foreach(F, kex_io:env_map()).

'entries'() ->
    lists:sort(maps:to_list(kex_io:env_map())).
