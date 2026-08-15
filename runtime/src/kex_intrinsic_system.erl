%% Kex.Intrinsic.System — BEAM primitive backend for System.* functions.
-module(kex_intrinsic_system).
-export([exit/1, die/1, os/0, bitWidth/0,
         mockOS/1, mockBitWidth/1, mockClear/0]).

exit(Code) -> erlang:halt(Code).

%% die(Msg) — the walker's `die` (src/interpreter/stdlib/io.cxx): the message
%% goes to STDERR prefixed with "fatal: ", then the process exits with 1. Not
%% an exception, so `trying`/`rescue` cannot catch it on either backend.
die(Msg) ->
    io:format(standard_error, "fatal: ~ts~n", [kex_io:to_string_bin(Msg)]),
    erlang:halt(1).

%% os() — the operating system family this program runs on, as one of the
%% atoms in the OS union (src/stdlib/system.kex); the tree walker answers with
%% the same atom for the same machine. Anything unmodelled is 'unknown', not
%% its own name: the union is closed, so a caller can match it exhaustively.
os() ->
    case get(kex_mock_os) of
        undefined -> real_os();
        Mocked    -> Mocked
    end.

real_os() ->
    case os:type() of
        {win32, _}      -> 'windows';
        {unix, darwin}  -> 'macos';
        {unix, linux}   -> 'linux';
        {unix, freebsd} -> 'freebsd';
        {unix, openbsd} -> 'openbsd';
        {unix, netbsd}  -> 'netbsd';
        {_Family, _}    -> 'unknown'
    end.

%% bitWidth() — the machine's pointer width in bits, from the emulator's word
%% size. The tree walker answers with sizeof(void*) * 8 for the same machine.
bitWidth() ->
    case get(kex_mock_bit_width) of
        undefined -> erlang:system_info(wordsize) * 8;
        Mocked    -> Mocked
    end.

%% Mock.System — makes the machine claim to be something else, so a test for
%% Windows behaviour can run anywhere. Process-dictionary state, like the
%% file and IO mocks in kex_file/kex_io.
mockOS(Name) -> put(kex_mock_os, Name), 'Kex.Unit'.

mockBitWidth(Bits) -> put(kex_mock_bit_width, Bits), 'Kex.Unit'.

mockClear() ->
    erase(kex_mock_os),
    erase(kex_mock_bit_width),
    'Kex.Unit'.
