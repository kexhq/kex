%% Kex.Intrinsic.Directory — BEAM primitive backend for Directory.* namespace functions.
-module(kex_intrinsic_directory).
-export(['exists?'/1, 'directory?'/1, 'file?'/1,
         create/1, delete/1, deleteAll/1,
         list/1, files/1, directories/1,
         current/0, home/0]).

'exists?'(Path) -> kex_file:'dir_exists?'(Path).
'directory?'(Path) -> kex_file:'dir_exists?'(Path).
'file?'(Path) -> kex_file:'dir_file?'(Path).
create(Path) -> kex_file:dir_create(Path).
delete(Path) -> kex_file:dir_delete(Path).
deleteAll(Path) -> kex_file:dir_delete_all(Path).
list(Path) -> kex_file:dir_list(Path).
files(Path) -> kex_file:dir_files(Path).
directories(Path) -> kex_file:dir_directories(Path).
current() -> kex_file:dir_current().
home() -> kex_file:dir_home().
