-module(kex_intrinsic_console).
-export(['Reset'/0, 'Bold'/0, 'Dim'/0, 'Italic'/0, 'Underline'/0,
         'Blink'/0, 'Reverse'/0, 'Hidden'/0, 'Strikethrough'/0,
         'Red'/0, 'Green'/0, 'Yellow'/0,
         'Blue'/0, 'Magenta'/0, 'Cyan'/0, 'White'/0, 'Gray'/0,
         'Purple'/0, 'enabled?'/0, colorize/2]).

'Reset'() -> code(<<27, "[0m">>).
'Bold'() -> code(<<27, "[1m">>).
'Dim'() -> code(<<27, "[2m">>).
'Italic'() -> code(<<27, "[3m">>).
'Underline'() -> code(<<27, "[4m">>).
'Blink'() -> code(<<27, "[5m">>).
'Reverse'() -> code(<<27, "[7m">>).
'Hidden'() -> code(<<27, "[8m">>).
'Strikethrough'() -> code(<<27, "[9m">>).
'Red'() -> code(<<27, "[31m">>).
'Green'() -> code(<<27, "[32m">>).
'Yellow'() -> code(<<27, "[33m">>).
'Blue'() -> code(<<27, "[34m">>).
'Magenta'() -> code(<<27, "[35m">>).
'Cyan'() -> code(<<27, "[36m">>).
'White'() -> code(<<27, "[37m">>).
'Gray'() -> code(<<27, "[90m">>).
'Purple'() -> code(<<27, "[95m">>).

enabled() ->
    os:getenv("KEX_COLORS", "1") =/= "0".

'enabled?'() -> enabled().

code(Value) ->
    case enabled() of true -> Value; false -> <<>> end.

colorize(Text, Color) ->
    case enabled() of
        true -> <<Color/binary, Text/binary, (('Reset')())/binary>>;
        false -> Text
    end.
