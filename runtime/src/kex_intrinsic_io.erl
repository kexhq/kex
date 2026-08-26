%% Kex.Intrinsic.IO — BEAM primitive backend for IO.* and System.* functions.
-module(kex_intrinsic_io).
-export([printLine/1, print/1, putLine/1, put/1, inspect/1, inspectRendered/2, inspectRepl/1, inspectTyped/2,
         printError/1, printErrorRaw/1, warn/1, warning/1, getLine/0, get/0,
         stdout/0, stderr/0, stdin/0,
         ioMockStart/0, ioMockStop/0, ioMockOutput/0, ioMockClear/0, ioMockInput/1]).

printLine(Msg)  -> kex_io:print_line(Msg).
print(Msg)      -> kex_io:print(Msg).
putLine(Msg)    -> kex_io:print_line(Msg).
put(Msg)        -> kex_io:print(Msg).
inspect(Val)    -> kex_io:inspect(Val).
inspectRendered(Val, Rendered) -> kex_io:inspect_rendered(Val, Rendered).
inspectRepl(Val) -> kex_io:inspect_repl(Val).
inspectTyped(Val, Type) -> kex_io:inspect_typed(Val, Type).
printError(Msg) -> kex_io:print_error(Msg).
printErrorRaw(Msg) -> kex_io:print_error_raw(Msg).
warn(Msg)       -> kex_io:print_error(Msg).
warning(Msg)    -> kex_io:print_error(Msg).
%% IO.out / IO.error / IO.in — the standard streams as ordinary
%% FileHandle values (kexhq/kex#139). The device slot holds an atom rather
%% than an open file; kex_file's handle_* clauses recognise it and forward
%% to the kex_io functions above.
stdout() -> {'FileHandle', stdout, <<"<stdout>">>}.
stderr() -> {'FileHandle', stderr, <<"<stderr>">>}.
stdin()  -> {'FileHandle', stdin, <<"<stdin>">>}.

getLine()       -> kex_io:read_line().
get()           -> kex_io:read_char().
%% Mock.IO — capturing the console is as much a lie as faking a file, so
%% the whole family is test-only like every other Mock.* intrinsic
%% (issue #144, kex_test:require_mocks_allowed/1).
ioMockStart() ->
    kex_test:require_mocks_allowed(<<"Mock.IO.start">>),
    kex_io:mock_start().
ioMockStop() ->
    kex_test:require_mocks_allowed(<<"Mock.IO.stop">>),
    kex_io:mock_stop().
ioMockOutput() ->
    kex_test:require_mocks_allowed(<<"Mock.IO.output">>),
    kex_io:mock_output().
ioMockClear() ->
    kex_test:require_mocks_allowed(<<"Mock.IO.clear">>),
    kex_io:mock_clear().
ioMockInput(Lines) when is_list(Lines) ->
    kex_test:require_mocks_allowed(<<"Mock.IO.input">>),
    kex_io:mock_input(Lines);
ioMockInput(Line) ->
    kex_test:require_mocks_allowed(<<"Mock.IO.input">>),
    kex_io:mock_input([Line]).
