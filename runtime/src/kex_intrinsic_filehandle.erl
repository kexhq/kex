%% Kex.Intrinsic.FileHandle — BEAM primitive backend for FileHandle functions.
-module(kex_intrinsic_filehandle).
-export([readLine/1, getChar/1, getChunk/2, put/2, printLine/2, print/2,
         read/1, readBytes/1, 'eof?'/1, feed/1, close/1, seek/2, reset/1]).

readLine(Handle) -> kex_file:handle_getLine(Handle).
getChar(Handle) -> kex_file:handle_get(Handle).
getChunk(Handle, Limit) -> kex_file:handle_get_chunk(Handle, Limit).
put(Handle, Content) -> kex_file:handle_put(Handle, Content).
printLine(Handle, Content) -> kex_file:handle_printLine(Handle, Content).
print(Handle, Content) -> kex_file:handle_print(Handle, Content).
read(Handle) -> kex_file:handle_read(Handle).
readBytes(Handle) -> kex_file:handle_read_bytes(Handle).
'eof?'(Handle) -> kex_file:'handle_atEnd?'(Handle).
feed({'FileHandle', Std, _}) when Std =:= stdout; Std =:= stderr; Std =:= stdin -> 'None';
feed({'FileHandle', Dev, _}) -> {'Just', kex_intrinsic_feed:of_handle(Dev)};
feed(_) -> 'None'.
close(Handle) -> kex_file:handle_close(Handle).
seek(Handle, Offset) -> kex_file:handle_seek(Handle, Offset).
reset(Handle) -> kex_file:handle_reset(Handle).
