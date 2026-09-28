-module(kex_intrinsic_console).
-export(['Reset'/0, 'Bold'/0, 'Dim'/0, 'Italic'/0, 'Underline'/0,
         'Blink'/0, 'Reverse'/0, 'Hidden'/0, 'Strikethrough'/0,
         'Red'/0, 'Green'/0, 'Yellow'/0,
         'Blue'/0, 'Magenta'/0, 'Cyan'/0, 'White'/0, 'Gray'/0,
         'Purple'/0, 'Clear'/0, 'Home'/0, 'ClearLine'/0,
         'enabled?'/0, colorize/2]).

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

%% Cursor/screen control, gated on the same switch as the colors: a clear
%% sequence written into a pipe is noise, not a cleared display.
'Clear'() -> code(<<27, "[2J", 27, "[H">>).
'Home'() -> code(<<27, "[H">>).
'ClearLine'() -> code(<<27, "[2K", "\r">>).

%% Whether styling is on, in this order:
%%   * NO_COLOR set to anything non-empty turns it off (https://no-color.org);
%%   * KEX_COLORS, which the `kex` launcher always sets from its own decision
%%     (a terminal, `--no-colors`) — authoritative, because the BEAM REPL runs
%%     behind pipes and could not tell a terminal from here;
%%   * otherwise — Tey, an escript, `erl` by hand — FORCE_COLOR (set, and
%%     not "0") turns it on, and failing that, whether stdout is a terminal,
%%     so escape codes never land in a pipe or a log file by accident.
%% The environment is read on every call (tests and programs change it);
%% only the terminal probe, which cannot change, is remembered.
enabled() ->
    case os:getenv("NO_COLOR", "") of
        "" ->
            case os:getenv("KEX_COLORS") of
                "0" -> false;
                "1" -> true;
                _ ->
                    case os:getenv("FORCE_COLOR", "") of
                        "" -> stdout_terminal();
                        "0" -> stdout_terminal();
                        _ -> true
                    end
            end;
        _ -> false
    end.

stdout_terminal() ->
    case persistent_term:get({?MODULE, stdout_terminal}, undefined) of
        undefined ->
            %% OTP 27+ reports it in the standard_io options; without the
            %% key there is no way to tell, and the old default (on) stays.
            Terminal = try proplists:get_value(stdout, io:getopts(standard_io), true)
                       catch _:_ -> true
                       end,
            persistent_term:put({?MODULE, stdout_terminal}, Terminal =:= true),
            Terminal =:= true;
        Known -> Known
    end.

'enabled?'() -> enabled().

code(Value) ->
    case enabled() of true -> Value; false -> <<>> end.

colorize(Text, Color) ->
    case enabled() of
        true -> <<Color/binary, Text/binary, (('Reset')())/binary>>;
        false -> Text
    end.
