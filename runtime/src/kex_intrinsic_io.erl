%% Kex.Intrinsic.IO — BEAM primitive backend for IO.* and System.* functions.
-module(kex_intrinsic_io).
-export([printLine/1, print/1, putLine/1, put/1, inspect/1,
         printError/1, warn/1, warning/1, getLine/0, get/0]).

printLine(Msg)  -> kex_io:print_line(Msg).
print(Msg)      -> kex_io:print(Msg).
putLine(Msg)    -> kex_io:print_line(Msg).
put(Msg)        -> kex_io:print(Msg).
inspect(Val)    -> kex_io:inspect(Val).
printError(Msg) -> kex_io:print_error(Msg).
warn(Msg)       -> kex_io:print_error(Msg).
warning(Msg)    -> kex_io:print_error(Msg).
getLine()       -> kex_io:read_line().
get()           -> kex_io:read_char().
