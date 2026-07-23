%% Kex.Intrinsic.FileHandle — BEAM primitive backend for FileHandle methods.
-module(kex_intrinsic_filehandle).
-export([getLine/1, get/1, printLine/2, print/2,
         readLine/1, writeLine/2, read/1, write/2,
         'eof?'/1, 'atEnd?'/1, feed/1, close/1]).

getLine(Handle) -> kex_file:handle_getLine(Handle).
get(Handle) -> kex_file:handle_get(Handle).
printLine(Handle, Content) -> kex_file:handle_printLine(Handle, Content).
print(Handle, Content) -> kex_file:handle_print(Handle, Content).
readLine(Handle) -> kex_file:handle_getLine(Handle).
writeLine(Handle, Content) -> kex_file:handle_printLine(Handle, Content).
read(Handle) -> kex_file:handle_read(Handle).
write(Handle, Content) -> kex_file:handle_write(Handle, Content).
'eof?'(Handle) -> kex_file:'handle_atEnd?'(Handle).
'atEnd?'(Handle) -> kex_file:'handle_atEnd?'(Handle).
feed({'FileHandle', _, Path}) -> kex_file:feed(Path);
feed({'MockFileHandle', Path}) -> kex_file:feed(Path);
feed(_) -> 'None'.
close(Handle) -> kex_file:handle_close(Handle).
