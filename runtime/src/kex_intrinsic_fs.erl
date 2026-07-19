%% Kex.Intrinsic.FS — BEAM primitive backend for Mock.FS.
-module(kex_intrinsic_fs).
-export([file/2, directory/1, clear/0]).

file(Path, Content) -> kex_file:mock_file(Path, Content).
directory(Path) -> kex_file:mock_dir(Path).
clear() -> kex_file:mock_clear().
