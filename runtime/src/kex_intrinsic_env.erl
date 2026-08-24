%% Kex.Intrinsic.Env — BEAM primitive backend for ENV namespace functions.
%%
%% ENV is an immutable Map<String, String> snapshot of the process environment.
%% Each function calls kex_io:env_map() to retrieve the snapshot and applies
%% the corresponding map operation.
-module(kex_intrinsic_env).
-export(['get'/1, 'getWithDefault'/2, 'has?'/1, 'keys'/0, 'values'/0,
         'count'/0, 'each'/1, 'entries'/0, 'set'/2, 'unset'/1,
         mockSet/2, mockUnset/1, mockClear/0, mockVars/1]).

'get'(K) ->
    M = kex_io:env_map(),
    case maps:find(K, M) of
        {ok, V} -> {'Just', V};
        error   -> 'None'
    end.

'getWithDefault'(K, Default) ->
    maps:get(K, kex_io:env_map(), Default).

%% ENV.set(Name, Value) — set a variable in THIS process and in the children
%% it starts. `os:putenv/2` changes the emulator's own environment, which is
%% what `open_port({spawn_executable, ...})` hands to a child.
%%
%% The reason this exists: a program that shells out sometimes has to decide
%% what the child sees. Tey knows which compiler it selected and has to hand
%% that to `Kex.AST`, which reads `$KEX` — without a setter the only channel
%% between them is whatever PATH happens to hold, and a manifest was being
%% parsed by an unrelated `kex` or by none at all.
'set'(Name, Value) ->
    N = unicode:characters_to_list(Name),
    case N of
        "" -> 'Kex.Unit';
        _ ->
            %% An `=` in the NAME would write a variable nobody can read back.
            case lists:member($=, N) of
                true -> 'Kex.Unit';
                false ->
                    os:putenv(N, unicode:characters_to_list(Value)),
                    'Kex.Unit'
            end
    end.

%% ENV.unset(Name) — remove a variable from this process and its children.
'unset'(Name) ->
    case unicode:characters_to_list(Name) of
        "" -> 'Kex.Unit';
        N -> os:unsetenv(N), 'Kex.Unit'
    end.

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
%% The whole overlay in one call, the same shape `Mock.Env { vars: ... }`
%% takes (kexhq/kex#143).
mockVars(Entries) when is_map(Entries) ->
    maps:foreach(fun(Name, Value) -> mockSet(Name, Value) end, Entries),
    'Kex.Unit';
mockVars(_) -> 'Kex.Unit'.

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
