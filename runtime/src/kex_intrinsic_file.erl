%% Kex.Intrinsic.File — BEAM primitive backend for File.* namespace functions.
-module(kex_intrinsic_file).
-export([open/2, open/3, read/1, write/2, append/2,
         'exists?'/1, 'file?'/1, 'directory?'/1,
         delete/1, copy/2, rename/2, lines/1, feed/1, size/1,
         basename/1, dirname/1, extension/1, join/2, absolute/1]).

open(Path, Mode) -> kex_file:open(Path, Mode).
open(Path, Mode, Fun) -> kex_file:open(Path, Mode, Fun).
read(Path) -> kex_file:read(Path).
write(Path, Content) -> kex_file:write(Path, Content).
append(Path, Content) -> kex_file:append(Path, Content).
'exists?'(Path) -> kex_file:exists(Path).
'file?'(Path) -> kex_file:'file?'(Path).
'directory?'(Path) -> kex_file:'directory?'(Path).
delete(Path) -> kex_file:delete(Path).
copy(Src, Dst) -> kex_file:copy(Src, Dst).
rename(Src, Dst) -> kex_file:rename(Src, Dst).
lines(Path) -> kex_file:lines(Path).
feed(Path) -> kex_file:feed(Path).
size(Path) -> kex_file:size(Path).
basename(Path) -> kex_file:basename(Path).
dirname(Path) -> kex_file:dirname(Path).
extension(Path) -> kex_file:extension(Path).
join(A, B) -> kex_file:join(A, B).
absolute(Path) -> kex_file:absolute(Path).
