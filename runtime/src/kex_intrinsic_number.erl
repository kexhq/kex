%% Kex.Intrinsic.Number — BEAM primitive backend for numeric intrinsics shared
%% by Integer and Float. Receiver is the first argument.
-module(kex_intrinsic_number).
-export([abs/1, sqrt/1, add/2, divide/2,
          floor/1, ceil/1, round/1, toFloat/1, toInteger/1]).

abs(N)  -> erlang:abs(N).
sqrt(N) -> math:sqrt(N).

%% add/2 — polymorphic + : string concat, Char+String splicing, numeric add.
%% Moved from kex_io where it didn't belong (it's a language operator, not I/O).
add(A, B) when is_list(A), is_integer(B) -> A ++ [B];
add(A, B) when is_integer(A), is_list(B) -> [A | B];
add(A, B) when is_list(A) -> A ++ B;
add(A, B) -> A + B.

%% divide/2 — polymorphic / : integer division when both integers, float
%% otherwise. Division-by-zero is a runtime error (caught in the emitter).
divide(A, B) when is_integer(A), is_integer(B), B =:= 0 -> erlang:error("runtime error: Division by zero");
divide(A, B) when is_integer(A), is_integer(B) -> A div B;
divide(A, B) -> A / B.

%% floor/ceil/round — rounding operations on numbers. erlang:floor/1 and
%% erlang:ceil/1 are OTP 27+; on integer input they return the integer itself.
floor(N)      -> erlang:floor(N).
ceil(N)       -> erlang:ceil(N).
round(N)      -> erlang:round(N).       %% works on both int and float

%% toFloat/1 — convert integer to float (no-op on floats).
toFloat(N)    -> erlang:float(N).

%% toInteger/1 — truncate toward zero (no-op on integers).
toInteger(N)  -> erlang:trunc(N).
