-module(kex_test).
-export([describe/2, describe/3, it/2, it/3, before/1, before/2,
          'after'/1, 'after'/2,
          maybe_print_summary/0, configure/2,
          assert/1, assert/2, assert_at/2, assert_at/3, is_truthy/1,
          allow_mocks/0, mocks_allowed/0, require_mocks_allowed/1]).

%% Mock.* is test-only (issue #144): a mock lets one part of a program lie
%% to another about the filesystem, environment, platform, network or
%% console, so the intrinsic backends (kex_intrinsic_fs/env/system/io/http)
%% call require_mocks_allowed/1 before touching mock state. The flag is
%% process-local like the mock state itself, and it is the RUNNER that grants
%% it, never the emitted module: `kex -R` prepends kex_test:allow_mocks() to
%% its -eval for a *.spec.kex entry or --allow-mocks, and the BEAM REPL does
%% the same before kex_repl_driver:loop() (both in src/main.cxx). So the very
%% same .beam started by `erl -pa ebin` begins with mocks denied. Mirrors
%% Evaluator::setMocksAllowed in the tree-walker, error line included — the
%% -R parity suites diff the two backends' output.
allow_mocks() -> put(kex_mocks_allowed, true), 'Kex.Unit'.

mocks_allowed() -> get(kex_mocks_allowed) =:= true.

require_mocks_allowed(Api) ->
    case mocks_allowed() of
        true -> ok;
        %% `/utf8` on the literal, not just on the file: a plain <<"…">>
        %% truncates each character to a byte, which turned the em dash into
        %% a lone \x14 and made the BEAM message differ from the walker's.
        false -> erlang:error(<<Api/binary,
            " is test-only — Mock.* runs in spec files (*.spec.kex), the "
            "REPL, or with --allow-mocks"/utf8>>)
    end.

%% Minimal RSpec-style describe/it DSL, mirroring
%% src/interpreter/stdlib/test.cxx exactly: describe is purely
%% organizational (prints a header, tracks nesting depth via indentation);
%% it runs its block and reports pass/fail; assert (kex_io:assert/1,2) is
%% what actually throws on failure. State (nesting depth, pass/fail
%% counts) lives in the process dictionary since — like the tree-walker's
%% m_testDepth/m_testsPassed/m_testsFailed members — it's inherently
%% sequential, single-process bookkeeping, not real concurrent state.

%% How this run reports itself, and which cases it runs (kexhq/kex#199).
%% Like allow_mocks/0 this is granted by the RUNNER (src/main.cxx builds the
%% -eval that calls it), never baked into an emitted module: a .beam handed
%% straight to `erl` reports the ✓/✗ prose a person reads.
%%
%% Mode is pretty | json | list; Filters is a list of binaries, each naming a
%% case by its full ` > `-joined path, by an ancestor describe's path, or by a
%% bare label. Mirrors Evaluator::setTestReportMode/setTestFilters.
configure(Mode, Filters) when Mode =:= pretty; Mode =:= json; Mode =:= list ->
    put(kex_test_mode, Mode),
    put(kex_test_filters, Filters),
    'Kex.Unit'.

mode() ->
    case get(kex_test_mode) of
        undefined -> pretty;
        Mode -> Mode
    end.

name_path() ->
    case get(kex_test_name_path) of
        undefined -> [];
        Path -> Path
    end.

%% A filter names a case by its full path, an ancestor `describe` by its own
%% path, or any case by its bare label. Mirrors Evaluator::testCaseSelected,
%% `IsGroup` included: a describe also runs when a filter names something
%% INSIDE it — a bare label could belong to a case at any depth, so a describe
%% cannot rule it out without running.
selected(Path) -> selected(Path, false).

selected(Path, IsGroup) ->
    case get(kex_test_filters) of
        undefined -> true;
        [] -> true;
        Filters ->
            Joined = join_path(Path),
            Prefix = <<Joined/binary, " > ">>,
            Leaf = lists:last(Path),
            lists:any(fun(Filter) ->
                Filter =:= Joined
                    orelse has_prefix(Joined, <<Filter/binary, " > ">>)
                    orelse case IsGroup of
                        true -> has_prefix(Filter, Prefix)
                                    orelse binary:match(Filter, <<" > ">>) =:= nomatch;
                        false -> Filter =:= Leaf
                    end
            end, Filters)
    end.

has_prefix(Binary, Prefix) ->
    Size = byte_size(Prefix),
    byte_size(Binary) >= Size andalso binary:part(Binary, 0, Size) =:= Prefix.

join_path([]) -> <<>>;
join_path([First | Rest]) ->
    lists:foldl(fun(Part, Acc) -> <<Acc/binary, " > ", Part/binary>> end,
                First, Rest).

%% The location `assert` last reported, when it is in the same file as the
%% `it` — a failure inside a stdlib helper would otherwise decorate a line of
%% the stdlib, which is no use to anyone reading the spec.
failure_location(Where) ->
    case {get(kex_test_last_location), Where} of
        {{File, _, _} = Last, {File, _, _}} -> Last;
        _ -> Where
    end.

%% ---- JSON records ------------------------------------------------------
%% One record per line on stdout, byte-for-byte the shape the tree-walker
%% emits (src/interpreter/stdlib/test.cxx) — spec/test_json.spec.kex diffs the
%% two backends, because an editor cannot care which one ran the suite.

emit_case(Path, Status, DurationMs, Failure, Where) ->
    Head = [<<"{\"kexTest\":\"case\",\"path\":">>, json_path(Path),
            <<",\"name\":">>, json_string(lists:last(Path)),
            <<",\"status\":">>, json_string(Status),
            <<",\"durationMs\":">>, json_number(DurationMs),
            json_location(Where)],
    Tail = case Failure of
        none -> [];
        {Message, FailWhere} ->
            [<<",\"failure\":{\"message\":">>,
             json_string(kex_io:to_string_bin(Message)),
             json_location(FailWhere), <<"}">>]
    end,
    emit_record([Head, Tail]).

emit_item(Kind, Path, Where) ->
    emit_record([<<"{\"kexTest\":\"item\",\"kind\":">>, json_string(Kind),
                 <<",\"path\":">>, json_path(Path),
                 <<",\"name\":">>, json_string(lists:last(Path)),
                 json_location(Where)]).

emit_record(Parts) ->
    io:format("~ts}~n", [iolist_to_binary(Parts)]).

json_path(Path) ->
    Items = lists:join(<<",">>, [json_string(Part) || Part <- Path]),
    iolist_to_binary([<<"[">>, Items, <<"]">>]).

%% Omitted entirely when the location is unknown, so a consumer can tell "no
%% location" from "line 0".
json_location(none) -> <<>>;
json_location({File, Line, Column}) ->
    iolist_to_binary([<<",\"file\":">>, json_string(File),
                      <<",\"line\":">>, integer_to_binary(Line),
                      <<",\"column\":">>, integer_to_binary(Column)]).

json_number(Value) ->
    iolist_to_binary(io_lib:format("~.3f", [Value * 1.0])).

json_string(Value) when not is_binary(Value) ->
    json_string(kex_io:to_string_bin(Value));
json_string(Binary) ->
    iolist_to_binary([<<"\"">>, [json_char(C) || <<C>> <= Binary], <<"\"">>]).

%% Bytes, not characters: UTF-8 continuation bytes pass through untouched,
%% which is what makes the result a valid UTF-8 JSON string.
json_char($") -> <<"\\\"">>;
json_char($\\) -> <<"\\\\">>;
json_char($\n) -> <<"\\n">>;
json_char($\r) -> <<"\\r">>;
json_char($\t) -> <<"\\t">>;
json_char(C) when C < 16#20 ->
    iolist_to_binary(io_lib:format("\\u~4.16.0b", [C]));
json_char(C) -> <<C>>.

describe(Name, Fun) -> describe(Name, Fun, none).

describe(Name, Fun, Where) ->
    Depth = get_depth(),
    PreviousScopes = hook_scopes(),
    PreviousPath = name_path(),
    Path = PreviousPath ++ [kex_io:to_string_bin(Name)],
    %% A `describe` runs whenever anything under it might: it is what
    %% REGISTERS the cases, so skipping it would skip them too.
    case selected(Path, true) of
        false -> 'None';
        true ->
            case mode() of
                pretty -> io:format("~s~ts~n", [indent(Depth), kex_io:to_string(Name)]);
                list -> emit_item(<<"describe">>, Path, Where);
                json -> ok
            end,
            put(kex_test_name_path, Path),
            put(kex_test_depth, Depth + 1),
            put(kex_test_hook_scopes,
                [#{before => [], 'after' => [], after_all => []} | PreviousScopes]),
            BodyResult = capture_exception(Fun),
            [Current | _] = hook_scopes(),
            AfterResult = run_all_hooks(lists:reverse(maps:get(after_all, Current))),
            put(kex_test_hook_scopes, PreviousScopes),
            put(kex_test_depth, Depth),
            put(kex_test_name_path, PreviousPath),
            case BodyResult of
                {raised, Class, Reason, Stack} -> erlang:raise(Class, Reason, Stack);
                ok ->
                    case AfterResult of
                        ok -> ok;
                        {failed, Msg} -> erlang:error(Msg)
                    end
            end,
            'None'
    end.

it(Name, Fun) -> it(Name, Fun, none).

it(Name, Fun, Where) ->
    Path = name_path() ++ [kex_io:to_string_bin(Name)],
    case mode() of
        %% Discovery runs no bodies: the tree an editor draws must be cheap and
        %% must not have side effects (kexhq/kex#199).
        list -> emit_item(<<"it">>, Path, Where), 'None';
        Mode ->
            case selected(Path) of
                false -> 'None';
                true -> run_it(Name, Fun, Where, Path, Mode)
            end
    end.

run_it(Name, Fun, Where, Path, Mode) ->
    Depth = get_depth(),
    Indent = indent(Depth),
    Started = erlang:monotonic_time(microsecond),
    %% Mock state set during this test — by a `before` hook or by the body —
    %% is discarded when the test ends, so a forgotten `Mock.FS.clear()` cannot
    %% leak into the next one. Captured BEFORE the hooks run, so per-test setup
    %% is undone too while anything a `before(:all)` established outside this
    %% block survives (kexhq/kex#143).
    SavedMocks = capture_mocks(),
    %% Where in the SPEC FILE we got to. Only `assert` reports a location (see
    %% assert_at/2,3), so a failure inside a stdlib helper leaves this at the
    %% `it` itself rather than pointing into another file.
    put(kex_test_last_location, Where),
    Result = run_case(Fun),
    restore_mocks(SavedMocks),
    FailWhere = failure_location(Where),
    Elapsed = (erlang:monotonic_time(microsecond) - Started) / 1000,
    case {Result, Mode} of
        {ok, json} ->
            inc(kex_test_passed),
            emit_case(Path, <<"passed">>, Elapsed, none, Where);
        {ok, _} ->
            inc(kex_test_passed),
            io:format("~s~ts~ts~ts ~ts~n", [Indent, kex_intrinsic_console:'Green'(),
                      [16#2713], kex_intrinsic_console:'Reset'(), kex_io:to_string(Name)]);
        {{failed, Msg}, json} ->
            inc(kex_test_failed),
            emit_case(Path, <<"failed">>, Elapsed, {Msg, FailWhere}, Where);
        {{failed, Msg}, _} ->
            inc(kex_test_failed),
            io:format("~s~ts~ts~ts ~ts: ~ts~n", [Indent, kex_intrinsic_console:'Red'(),
                      [16#2717], kex_intrinsic_console:'Reset'(), kex_io:to_string(Name), Msg])
    end,
    'None'.

%% Every mock keeps its state in the process dictionary, under an atom key
%% beginning `kex_mock_` or a tuple whose first element is one (the per-path
%% `{kex_mock_file, Path}` entries).
is_mock_key(Key) when is_atom(Key) ->
    lists:prefix("kex_mock_", atom_to_list(Key));
is_mock_key(Key) when is_tuple(Key), tuple_size(Key) > 0 ->
    is_mock_key(element(1, Key));
is_mock_key(_) -> false.

capture_mocks() ->
    [Entry || {Key, _} = Entry <- erlang:get(), is_mock_key(Key)].

restore_mocks(Saved) ->
    [erase(Key) || {Key, _} <- erlang:get(), is_mock_key(Key)],
    [put(Key, Value) || {Key, Value} <- Saved],
    ok.

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
    %% An unrescued `.try` failure inside a block does not unwind — a lambda IS
    %% a function, so it RETURNS `{'Error', E}` (the same rule HOFs such as
    %% `map` depend on). A test body is a block, so `assert(f().try == x)` on a
    %% failing `f()` left the assert unevaluated and the case reported green.
    %% Every spec assertion whose expression propagated was silently passing.
    %% Mirrors Evaluator's `it` builtin in src/interpreter/stdlib/test.cxx.
    try
        case Fun() of
            {'Error', Propagated} ->
                {failed, iolist_to_binary(
                    [<<".try propagated out of the test body: ">>,
                     kex_io:to_string_bin(
                         kex_intrinsic_kex:show(Propagated))])};
            _ -> ok
        end
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

%% Prints the tally and, if anything failed, ENDS THE RUN NON-ZERO.
%%
%% A failing spec used to exit 0. Every `tey test` and every CI job built on
%% one was therefore green whatever the specs said, and this repository's own
%% Makefile worked around it by grepping "N failed" back out of the output —
%% a workaround no downstream project knew to copy.
%%
%% halt/1 here rather than a return value the caller checks: this call is
%% already the last thing a lowered `main` does (see withTestSummary in
%% src/ir/lower.cxx), and threading a status back out through main's return
%% would change what every non-test program returns.
maybe_print_summary() ->
    Passed = counter(kex_test_passed),
    Failed = counter(kex_test_failed),
    case mode() of
        %% Discovery ran no cases, so there is nothing to tally.
        list -> ok;
        %% In JSON the summary is emitted even for a run with no cases at all:
        %% it is how a consumer tells "this file has no tests" from "the
        %% process died before finishing".
        json ->
            io:format("{\"kexTest\":\"summary\",\"passed\":~b,\"failed\":~b}~n",
                      [Passed, Failed]),
            halt_if_failed(Failed);
        pretty ->
            case Passed + Failed of
                0 -> ok;
                _ ->
                    io:format("~n~b passed, ~b failed~n", [Passed, Failed]),
                    halt_if_failed(Failed)
            end
    end.

halt_if_failed(0) -> ok;
halt_if_failed(_) ->
    %% Flush before halting: halt/1 does not wait for the group leader, and the
    %% tally above is the one line a failing run must not lose.
    ok = io:format(""),
    erlang:halt(1).

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
%% The message is a BINARY (a Kex String), never a charlist: a charlist is an
%% ordinary Kex list of integers here, so `kex_io:to_string/1` renders it as
%% `[97, 115, …]` — which is exactly what a bare `assert(false)` used to
%% report, where the walker says just "assertion failed".
%% Located forms, emitted by the IR lowerer for every `assert` in Kex source
%% (kexhq/kex#199). They exist so a failure can name the LINE it happened on:
%% the walker recovers that from evaluation, and BEAM has nothing equivalent
%% once the spec is a fun in a .beam. The location is recorded whether or not
%% the assertion holds — the last one recorded before a failure is the one
%% that failed.
assert_at(Cond, Where) ->
    put(kex_test_last_location, Where),
    assert(Cond).
assert_at(Cond, Msg, Where) ->
    put(kex_test_last_location, Where),
    assert(Cond, Msg).

assert(Cond) ->
    case is_truthy(Cond) of
        true -> true;
        false -> erlang:error(<<"assertion failed">>)
    end.
assert(Cond, Msg) ->
    case is_truthy(Cond) of
        true -> true;
        false -> erlang:error(<<"assertion failed: ",
                                (kex_io:to_string_bin(Msg))/binary>>)
    end.

%% Same truthiness rule as `if`/`while`/`&&`/`||` throughout this runtime:
%% only false/none/'ok' (Kex's Unit) are falsy — everything else (0, "",
%% [], any record/variant) is truthy.
is_truthy(false) -> false;
is_truthy('None') -> false;
is_truthy('ok') -> false;
is_truthy(_) -> true.
