%% Kex.Intrinsic.Env — BEAM primitive backend for ENV namespace functions.
%%
%% ENV is an immutable Map<String, String> snapshot of the process environment.
%% Each function calls kex_io:env_map() to retrieve the snapshot and applies
%% the corresponding map operation.
-module(kex_intrinsic_env).
-export(['get'/1, 'getWithDefault'/2, 'has?'/1, 'keys'/0, 'values'/0,
         'count'/0, 'each'/1, 'entries'/0,
         mockSet/2, mockUnset/1, mockClear/0]).

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

%% Mock.ENV — a test's overlay on the process environment, held in the
%% process dictionary like the file and IO mocks. kex_io:env_map/0 applies it,
%% so every ENV function sees the same answer. Test-only like every Mock.*
%% intrinsic (issue #144).
mockSet(Name, Value) ->
    kex_test:require_mocks_allowed(<<"Mock.ENV.set">>),
    Overlay = case erlang:get(kex_mock_env) of undefined -> #{}; M -> M end,
    erlang:put(kex_mock_env, maps:put(Name, Value, Overlay)),
    erlang:put(kex_mock_env_unset,
        lists:delete(Name, case erlang:get(kex_mock_env_unset) of undefined -> []; L -> L end)),
    'Kex.Unit'.

mockUnset(Name) ->
    kex_test:require_mocks_allowed(<<"Mock.ENV.unset">>),
    Overlay = case erlang:get(kex_mock_env) of undefined -> #{}; M -> M end,
    erlang:put(kex_mock_env, maps:remove(Name, Overlay)),
    Unset = case erlang:get(kex_mock_env_unset) of undefined -> []; L -> L end,
    erlang:put(kex_mock_env_unset, [Name | lists:delete(Name, Unset)]),
    'Kex.Unit'.

mockClear() ->
    kex_test:require_mocks_allowed(<<"Mock.ENV.clear">>),
    erlang:erase(kex_mock_env),
    erlang:erase(kex_mock_env_unset),
    'Kex.Unit'.
