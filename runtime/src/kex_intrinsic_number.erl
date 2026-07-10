%% Kex.Intrinsic.Number — BEAM primitive backend for numeric intrinsics shared
%% by Integer and Float. Receiver is the first argument.
-module(kex_intrinsic_number).
-export([abs/1, sqrt/1, add/2, divide/2, eq/2, neq/2,
          floor/1, ceil/1, round/1, toFloat/1, toInteger/1,
          'toString'/1, float_parse/1,
          to_integer/1, to_float/1]).

abs(N)  -> erlang:abs(N).
sqrt(N) -> math:sqrt(N).

%% add/2 — polymorphic + : string concat, Char+String splicing, list append,
%% numeric add. A Kex String is a UTF-8 binary; a Char is a codepoint integer;
%% a [Char] charlist counts as a String too, so mixed operands coerce.
add(A, B) when is_binary(A), is_binary(B) -> <<A/binary, B/binary>>;
add(A, B) when is_binary(A), is_integer(B) -> <<A/binary, B/utf8>>;
add(A, B) when is_integer(A), is_binary(B) -> <<A/utf8, B/binary>>;
add(A, B) when is_binary(A), is_list(B) -> <<A/binary, (unicode:characters_to_binary(B))/binary>>;
add(A, B) when is_list(A), is_binary(B) -> <<(unicode:characters_to_binary(A))/binary, B/binary>>;
add(A, B) when is_list(A), is_integer(B) -> A ++ [B];
add(A, B) when is_integer(A), is_list(B) -> [A | B];
add(A, B) when is_list(A) -> A ++ B;
add(A, B) -> A + B.

%% eq/neq — Kex ==. Strict equality except the one representation split the
%% language defines away: [Char] IS String, so a flat charlist and a binary
%% holding the same text are equal. Nothing else coerces (a [String] list is
%% NOT equal to the concatenated string, hence the flat-integer check).
eq(A, B) when is_list(A), is_binary(B) -> charlist_eq(A, B);
eq(A, B) when is_binary(A), is_list(B) -> charlist_eq(B, A);
eq(A, B) -> A =:= B.

neq(A, B) -> not eq(A, B).

charlist_eq(L, Bin) ->
    case lists:all(fun erlang:is_integer/1, L) of
        true ->
            case unicode:characters_to_binary(L) of
                B2 when is_binary(B2) -> B2 =:= Bin;
                _ -> false
            end;
        false -> false
    end.

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

%% toString/1 — convert number to string, matching Kex's display formatting
%% (6 decimal places for floats, matching kex_io:to_string / interpreter).
'toString'(N) -> unicode:characters_to_binary(kex_io:to_string(N)).

%% Float.parse(s) — parse a string to float, returning Ok(Float) or
%% Error(reason). Matches src/interpreter/stdlib/number.cxx exactly.
%% Moved from kex_io where parsing didn't belong.
float_parse(S) when is_binary(S) ->
    case string:to_float(S) of
        {Flt, <<>>} -> {'Ok', Flt};
        _ ->
            case string:to_integer(S) of
                {Int, <<>>} -> {'Ok', float(Int)};
                _ -> {'Error', <<"invalid float: ", S/binary>>}
            end
    end;
float_parse(S) ->
    case string:to_float(S) of
        {Flt, ""} -> {'Ok', Flt};
        _ ->
            case string:to_integer(S) of
                {Int, ""} -> {'Ok', float(Int)};
                _ -> {'Error', unicode:characters_to_binary(["invalid float: ", S])}
            end
    end.

%% x.to(Integer) / x.to(Float) — universal numeric conversion, mirroring
%% src/interpreter/stdlib/list.cxx's `to` builtin exactly: passthrough for
%% an already-matching type, TRUNCATE (not round) a Float down to Integer,
%% parse a String, and 'none' on anything else/unparseable.
%% Moved from kex_io where type conversion didn't belong.
to_integer(X) when is_integer(X) -> X;
to_integer(X) when is_float(X) -> erlang:trunc(X);
to_integer(X) when is_binary(X) ->
    case string:to_integer(X) of
        {Int, <<>>} -> Int;
        _ -> 'none'
    end;
to_integer(X) when is_list(X) ->
    case string:to_integer(X) of
        {Int, ""} -> Int;
        _ -> 'none'
    end;
to_integer(_) -> 'none'.

to_float(X) when is_float(X) -> X;
to_float(X) when is_integer(X) -> float(X);
to_float(X) when is_binary(X) ->
    case string:to_float(X) of
        {Flt, <<>>} -> Flt;
        _ ->
            case string:to_integer(X) of
                {Int, <<>>} -> float(Int);
                _ -> 'none'
            end
    end;
to_float(X) when is_list(X) ->
    case string:to_float(X) of
        {Flt, ""} -> Flt;
        _ ->
            case string:to_integer(X) of
                {Int, ""} -> float(Int);
                _ -> 'none'
            end
    end;
to_float(_) -> 'none'.
