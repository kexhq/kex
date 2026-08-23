%% Kex.Intrinsic.FS — BEAM primitive backend for Mock.FS.
-module(kex_intrinsic_fs).
-export([file/2, directory/1, clear/0, files/1, onRead/1]).

file(Path, Content) ->
    kex_test:require_mocks_allowed(<<"Mock.FS.File">>),
    kex_file:mock_file(Path, Content).
directory(Path) ->
    kex_test:require_mocks_allowed(<<"Mock.FS.Directory">>),
    kex_file:mock_dir(Path).
files(Entries) ->
    kex_test:require_mocks_allowed(<<"Mock.FS.files">>),
    kex_file:mock_files(Entries).

onRead(Fun) ->
    kex_test:require_mocks_allowed(<<"Mock.FS.onRead">>),
    kex_file:mock_on_read(Fun).

clear() ->
    kex_test:require_mocks_allowed(<<"Mock.FS.clear">>),
    kex_file:mock_clear().
