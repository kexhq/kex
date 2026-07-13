-module(kex_test).
-export([describe/2, it/2, before/1, before/2, 'after'/1, 'after'/2,
          maybe_print_summary/0,
          assert/1, assert/2, is_truthy/1]).

%% Minimal RSpec-style describe/it DSL, mirroring
%% src/interpreter/stdlib/test.cxx exactly: describe is purely
%% organizational (prints a header, tracks nesting depth via indentation);
%% it runs its block and reports pass/fail; assert (kex_io:assert/1,2) is
%% what actually throws on failure. State (nesting depth, pass/fail
%% counts) lives in the process dictionary since — like the tree-walker's
%% m_testDepth/m_testsPassed/m_testsFailed members — it's inherently
%% sequential, single-process bookkeeping, not real concurrent state.

describe(Name, Fun) ->
    Depth = get_depth(),
    PreviousScopes = hook_scopes(),
    io:format("~s~ts~n", [indent(Depth), kex_io:to_string(Name)]),
    put(kex_test_depth, Depth + 1),
    put(kex_test_hook_scopes,
        [#{before => [], 'after' => [], after_all => []} | PreviousScopes]),
    BodyResult = capture_exception(Fun),
    [Current | _] = hook_scopes(),
    AfterResult = run_all_hooks(lists:reverse(maps:get(after_all, Current))),
    put(kex_test_hook_scopes, PreviousScopes),
    put(kex_test_depth, Depth),
    case BodyResult of
        {raised, Class, Reason, Stack} -> erlang:raise(Class, Reason, Stack);
        ok ->
            case AfterResult of
                ok -> ok;
                {failed, Msg} -> erlang:error(Msg)
            end
    end,
    'None'.

it(Name, Fun) ->
    Depth = get_depth(),
    Indent = indent(Depth),
    case run_case(Fun) of
        ok ->
            inc(kex_test_passed),
            io:format("~s~ts~ts~ts ~ts~n", [Indent, kex_intrinsic_console:'Green'(),
                      [16#2713], kex_intrinsic_console:'Reset'(), kex_io:to_string(Name)]);
        {failed, Msg} ->
            inc(kex_test_failed),
            io:format("~s~ts~ts~ts ~ts: ~ts~n", [Indent, kex_intrinsic_console:'Red'(),
                      [16#2717], kex_intrinsic_console:'Reset'(), kex_io:to_string(Name), Msg])
    end,
    'None'.

before(Fun) -> register_hook(before, each, Fun).
before(Scope, Fun) -> register_hook(before, Scope, Fun).
'after'(Fun) -> register_hook('after', each, Fun).
'after'(Scope, Fun) -> register_hook('after', Scope, Fun).

register_hook(Key, Scope, Fun) when is_function(Fun, 0),
                                    (Scope =:= each orelse Scope =:= all) ->
    case hook_scopes() of
        [] -> erlang:error(atom_to_list(Key) ++ " must be declared inside describe");
        [Current | Parents] ->
            case {Key, Scope} of
                {before, all} -> Fun(), 'Kex.Unit';
                {'after', all} ->
                    Hooks = maps:get(after_all, Current),
                    put(kex_test_hook_scopes,
                        [Current#{after_all := Hooks ++ [Fun]} | Parents]),
                    'Kex.Unit';
                _ ->
                    Hooks = maps:get(Key, Current),
                    put(kex_test_hook_scopes, [Current#{Key := Hooks ++ [Fun]} | Parents]),
                    'Kex.Unit'
            end
    end;
register_hook(_, Scope, _) when Scope =/= each, Scope =/= all ->
    erlang:error("test hook scope must be :each or :all");
register_hook(Key, _, _) ->
    erlang:error(atom_to_list(Key) ++ " requires a block").

%% Only assert's own erlang:error(String) reason is reproduced verbatim
%% (matching the tree-walker's e.what() for a failed assertion exactly);
%% any other runtime error escaping the block is still caught (so the
%% suite doesn't abort) but formatted generically, since exact tree-
%% walker error text (with source location) genuinely can't be
%% reproduced under BEAM's own runtime.
run_case(Fun) ->
    Scopes = hook_scopes(),
    Before = lists:append([maps:get(before, Scope) || Scope <- lists:reverse(Scopes)]),
    After = lists:append([lists:reverse(maps:get('after', Scope)) || Scope <- Scopes]),
    BodyResult = case run_hooks(Before) of
        ok -> capture(Fun);
        BeforeFailure -> BeforeFailure
    end,
    AfterResult = run_all_hooks(After),
    case BodyResult of
        ok -> AfterResult;
        BodyFailure -> BodyFailure
    end.

run_hooks([]) -> ok;
run_hooks([Hook | Rest]) ->
    case capture(Hook) of
        ok -> run_hooks(Rest);
        Failed -> Failed
    end.

%% Teardown is best-effort: every hook runs, while the first failure remains
%% the one reported for the test case.
run_all_hooks(Hooks) -> run_all_hooks(Hooks, ok).
run_all_hooks([], Result) -> Result;
run_all_hooks([Hook | Rest], Result) ->
    HookResult = capture(Hook),
    Next = case Result of
        ok -> HookResult;
        Failed -> Failed
    end,
    run_all_hooks(Rest, Next).

capture(Fun) ->
    try Fun(), ok
    catch
        error:Reason when is_binary(Reason) -> {failed, Reason};
        error:Reason when is_list(Reason) -> {failed, Reason};
        error:Reason -> {failed, lists:flatten(io_lib:format("~p", [Reason]))};
        throw:Reason -> {failed, lists:flatten(io_lib:format("~p", [Reason]))};
        exit:Reason -> {failed, lists:flatten(io_lib:format("~p", [Reason]))}
    end.

capture_exception(Fun) ->
    try Fun(), ok
    catch Class:Reason:Stack -> {raised, Class, Reason, Stack}
    end.

maybe_print_summary() ->
    Passed = counter(kex_test_passed),
    Failed = counter(kex_test_failed),
    case Passed + Failed of
        0 -> ok;
        _ -> io:format("~n~b passed, ~b failed~n", [Passed, Failed])
    end.

get_depth() -> counter(kex_test_depth).
hook_scopes() ->
    case get(kex_test_hook_scopes) of
        undefined -> [];
        Scopes -> Scopes
    end.

counter(Key) ->
    case get(Key) of
        undefined -> 0;
        V -> V
    end.

inc(Key) -> put(Key, counter(Key) + 1).

indent(Depth) -> lists:duplicate(Depth * 2, $\s).

%% assert(cond[, msg]) — matches src/interpreter/stdlib/test.cxx's assert
%% exactly: throws (here, erlang:error/1, caught the same way any other
%% Kex runtime error is) when cond isn't truthy.
%% Moved from kex_io where testing logic didn't belong.
assert(Cond) -> assert(Cond, "assertion failed").
assert(Cond, Msg) ->
    case is_truthy(Cond) of
        true -> true;
        false -> erlang:error(lists:flatten("assertion failed: " ++ kex_io:to_string(Msg)))
    end.

%% Same truthiness rule as `if`/`while`/`&&`/`||` throughout this runtime:
%% only false/none/'ok' (Kex's Unit) are falsy — everything else (0, "",
%% [], any record/variant) is truthy.
is_truthy(false) -> false;
is_truthy('None') -> false;
is_truthy('ok') -> false;
is_truthy(_) -> true.
