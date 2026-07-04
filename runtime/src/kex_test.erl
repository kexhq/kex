-module(kex_test).
-export([describe/2, it/2, maybe_print_summary/0]).

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
    io:format("~s~s~n", [indent(Depth), kex_io:to_string(Name)]),
    put(kex_test_depth, Depth + 1),
    try
        Fun()
    after
        put(kex_test_depth, Depth)
    end,
    'none'.

it(Name, Fun) ->
    Depth = get_depth(),
    Indent = indent(Depth),
    case run_case(Fun) of
        ok ->
            inc(kex_test_passed),
            io:format("~s~ts ~s~n", [Indent, [16#2713], kex_io:to_string(Name)]);
        {failed, Msg} ->
            inc(kex_test_failed),
            io:format("~s~ts ~s: ~s~n", [Indent, [16#2717], kex_io:to_string(Name), Msg])
    end,
    'none'.

%% Only assert's own erlang:error(String) reason is reproduced verbatim
%% (matching the tree-walker's e.what() for a failed assertion exactly);
%% any other runtime error escaping the block is still caught (so the
%% suite doesn't abort) but formatted generically, since exact tree-
%% walker error text (with source location) genuinely can't be
%% reproduced under BEAM's own runtime.
run_case(Fun) ->
    try
        Fun(),
        ok
    catch
        error:Reason when is_list(Reason) -> {failed, Reason};
        error:Reason -> {failed, lists:flatten(io_lib:format("~p", [Reason]))};
        throw:Reason -> {failed, lists:flatten(io_lib:format("~p", [Reason]))}
    end.

maybe_print_summary() ->
    Passed = counter(kex_test_passed),
    Failed = counter(kex_test_failed),
    case Passed + Failed of
        0 -> ok;
        _ -> io:format("~n~b passed, ~b failed~n", [Passed, Failed])
    end.

get_depth() -> counter(kex_test_depth).

counter(Key) ->
    case get(Key) of
        undefined -> 0;
        V -> V
    end.

inc(Key) -> put(Key, counter(Key) + 1).

indent(Depth) -> lists:duplicate(Depth * 2, $\s).
