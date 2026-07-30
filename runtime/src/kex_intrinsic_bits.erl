%% Kex.Intrinsic.Bits — BEAM primitive backend for the Bits module.
%% The typed surface lives in src/stdlib/bits.kex; the walker twin is
%% src/interpreter/stdlib/bits.cxx.
%%
%% Erlang integers are arbitrary precision and its bitwise operators model a
%% negative as infinite-precision two's complement — the same model GMP uses
%% on the walker side, so both backends agree on negatives and bignums with no
%% special casing.
-module(kex_intrinsic_bits).
-export(['and'/2, 'or'/2, 'xor'/2, 'not'/1,
         shiftLeft/2, shiftRight/2,
         'test?'/2, set/2, clear/2, toggle/2,
         count/1, width/1]).

'and'(A, B) -> A band B.
'or'(A, B)  -> A bor B.
'xor'(A, B) -> A bxor B.
'not'(A)    -> bnot A.

%% Shift distances and bit indices address a position, so they must be
%% non-negative. Erlang would silently reverse direction for a negative `bsl`,
%% which only hides the mistake.
shiftLeft(N, By)  -> N bsl check_position(By, <<"Bits.shiftLeft">>).
%% Arithmetic (sign-propagating): -8 bsr 1 is -4.
shiftRight(N, By) -> N bsr check_position(By, <<"Bits.shiftRight">>).

'test?'(N, Index) -> ((N bsr check_position(Index, <<"Bits.test?">>)) band 1) =:= 1.
set(N, Index)     -> N bor (1 bsl check_position(Index, <<"Bits.set">>)).
clear(N, Index)   -> N band bnot (1 bsl check_position(Index, <<"Bits.clear">>)).
toggle(N, Index)  -> N bxor (1 bsl check_position(Index, <<"Bits.toggle">>)).

check_position(By, _What) when is_integer(By), By >= 0 -> By;
check_position(_, What) ->
    erlang:error(<<What/binary, ": bit position cannot be negative">>).

%% A negative value has infinitely many set bits under two's complement, so
%% both of these are defined only for non-negative input.
count(N) when is_integer(N), N >= 0 -> count_bits(N, 0);
count(_) -> erlang:error(<<"Bits.count is undefined for a negative Integer">>).

count_bits(0, Acc) -> Acc;
count_bits(N, Acc) -> count_bits(N bsr 1, Acc + (N band 1)).

width(N) when is_integer(N), N >= 0 -> width_bits(N, 0);
width(_) -> erlang:error(<<"Bits.width is undefined for a negative Integer">>).

width_bits(0, Acc) -> Acc;
width_bits(N, Acc) -> width_bits(N bsr 1, Acc + 1).
