%% Kex.Intrinsic.Number — BEAM primitive backend for numeric intrinsics shared
%% by Integer and Float. Receiver is the first argument.
-module(kex_intrinsic_number).
-export([abs/1, sqrt/1, add/2, divide/2, eq/2, neq/2,
          floor/1, ceil/1, round/1, toInteger/1,
          float_parse/1, float_parse_prefix/1, number_parse/1,
          parse/1, to_integer/1, to_float/1]).

abs(N)  -> erlang:abs(N).
sqrt(N) -> math:sqrt(N).

%% add/2 — polymorphic + : string concat, Char+String splicing, list append,
%% numeric add. A Kex String is a UTF-8 binary; a Char is a tagged tuple
%% {'Char', Codepoint}; a [Char] list counts as a String too.
add({'Char', A}, {'Char', B}) -> <<A/utf8, B/utf8>>;
add(A, {'Char', B}) when is_binary(A) -> <<A/binary, B/utf8>>;
add({'Char', A}, B) when is_binary(B) -> <<A/utf8, B/binary>>;
add(A, B = {'Char', _}) when is_list(A) -> A ++ [B];
add(A = {'Char', _}, B) when is_list(B) -> [A | B];
add(A, B) when is_binary(A), is_binary(B) -> <<A/binary, B/binary>>;
add(A, B) when is_binary(A), is_list(B) -> <<A/binary, (unicode:characters_to_binary(charlist(B)))/binary>>;
add(A, B) when is_list(A), is_binary(B) -> <<(unicode:characters_to_binary(charlist(A)))/binary, B/binary>>;
add(A, B) when is_list(A), is_integer(B) -> A ++ [B];
add(A, B) when is_integer(A), is_list(B) -> [A | B];
add(A, B) when is_list(A) -> A ++ B;
add(A, B) -> A + B.

%% eq/neq — Kex ==. Strict equality except the one representation split the
%% language defines away: [Char] IS String, so a list of tagged Chars and a
%% binary holding the same text are equal. Nothing else coerces (an [Int]
%% list is NOT a String, and a [String] list is NOT the concatenated string).
eq(A, B) when is_list(A), is_binary(B) -> charlist_eq(A, B);
eq(A, B) when is_binary(A), is_list(B) -> charlist_eq(B, A);
eq(A, B) -> A =:= B.

neq(A, B) -> not eq(A, B).

charlist_eq(L, Bin) ->
    case charlist_opt(L, []) of
        {ok, Cs} ->
            case unicode:characters_to_binary(Cs) of
                B2 when is_binary(B2) -> B2 =:= Bin;
                _ -> false
            end;
        error -> false
    end.

%% The codepoint list of a [Char] (tagged) — errors on anything else.
charlist(L) ->
    case charlist_opt(L, []) of
        {ok, Cs} -> Cs;
        error -> L
    end.

charlist_opt([], Acc) -> {ok, lists:reverse(Acc)};
charlist_opt([{'Char', C} | T], Acc) when is_integer(C) -> charlist_opt(T, [C | Acc]);
charlist_opt(_, _) -> error.

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

%% toInteger/1 — truncate toward zero (no-op on integers).
toInteger(N)  -> erlang:trunc(N).

%% ParseError is the tagged tuple {'ParseError', Input, Position, Value, Message, Rest}
%% (record lowered by src/ir/lower.cxx). Matches src/interpreter/stdlib/number.cxx.
parse_error(S, Position, Value, Message, Rest) ->
    {'ParseError', S, Position, Value, Message, Rest}.

%% Float.parse(s) -> Ok(Float) | Error(ParseError). Full match returns bare
%% Ok(Flt). Partial (trailing) returns Error(ParseError{value, rest, ...}).
%% A bare integer like "5" is accepted (to_integer fallback).
float_parse(S) when is_binary(S) ->
    case string:to_float(S) of
        {Flt, <<>>} -> {'Ok', Flt};
        {Flt, Rest} when is_float(Flt) ->
            {'Error', parse_error(S, byte_size(S) - byte_size(Rest), Flt,
                                  <<"trailing characters after float">>, Rest)};
        _ ->
            case string:to_integer(S) of
                {Int, <<>>} -> {'Ok', float(Int)};
                {Int, Rest} when is_integer(Int) ->
                    {'Error', parse_error(S, byte_size(S) - byte_size(Rest), float(Int),
                                          <<"trailing characters after float">>, Rest)};
                _ -> {'Error', parse_error(S, 0, 'None', <<"invalid float">>, S)}
            end
    end;
float_parse(S) ->
    case string:to_float(S) of
        {Flt, ""} -> {'Ok', Flt};
        {Flt, Rest} when is_float(Flt) ->
            {'Error', parse_error(unicode:characters_to_binary(S),
                                  length(S) - length(Rest), Flt,
                                  <<"trailing characters after float">>,
                                  unicode:characters_to_binary(Rest))};
        _ ->
            case string:to_integer(S) of
                {Int, ""} -> {'Ok', float(Int)};
                {Int, Rest} when is_integer(Int) ->
                    {'Error', parse_error(unicode:characters_to_binary(S),
                                          length(S) - length(Rest), float(Int),
                                          <<"trailing characters after float">>,
                                          unicode:characters_to_binary(Rest))};
                _ -> {'Error', parse_error(unicode:characters_to_binary(S), 0, 'None',
                                           <<"invalid float">>,
                                           unicode:characters_to_binary(S))}
            end
    end.

%% Float.parsePrefix(s) -> Just({Float, Rest}) | none. Parses a leading float
%% (or bare integer promoted to float), returning a proper Optional tuple.
float_parse_prefix(S) when is_binary(S) ->
    case string:to_float(S) of
        {Flt, Rest} when is_float(Flt) -> {'Just', {Flt, Rest}};
        _ ->
            case string:to_integer(S) of
                {Int, Rest} when is_integer(Int) -> {'Just', {float(Int), Rest}};
                _ -> 'None'
            end
    end;
float_parse_prefix(S) ->
    case string:to_float(S) of
        {Flt, Rest} when is_float(Flt) -> {'Just', {Flt, Rest}};
        _ ->
            case string:to_integer(S) of
                {Int, Rest} when is_integer(Int) -> {'Just', {float(Int), Rest}};
                _ -> 'None'
            end
    end.

%% Number.parse(s) -> Ok(Int|Float) | Error(ParseError). Tries Integer first,
%% then Float (so "42" -> Integer, "3.14" -> Float, "5" -> Integer). When both
%% fail, returns a generic "invalid number" error (matching the interpreter).
number_parse(S) when is_binary(S) ->
    %% Only accept integer full-match; fall through to float otherwise.
    case kex_intrinsic_integer:integer_parse(S) of
        {'Ok', {_Int, <<>>}} = Ok -> Ok;
        _ ->
            case float_parse(S) of
                {'Ok', _} = Ok -> Ok;
                {'Error', _} -> {'Error', parse_error(S, 0, 'None',
                                                       <<"invalid number">>, S)}
            end
    end;
number_parse(S) -> number_parse(unicode:characters_to_binary(S)).

parse(S) -> number_parse(S).

%% x.to(Integer) / x.to(Float) — universal numeric conversion, mirroring
%% src/interpreter/stdlib/list.cxx's `to` builtin exactly: passthrough for
%% an already-matching type, TRUNCATE (not round) a Float down to Integer,
%% parse a String, and return {'Just', Value} or 'None'.
%% Moved from kex_io where type conversion didn't belong.
to_integer({'Char', C}) -> {'Just', C};
to_integer(X) when is_integer(X) -> {'Just', X};
to_integer(X) when is_float(X) -> {'Just', erlang:trunc(X)};
to_integer(X) when is_binary(X) ->
    case string:to_integer(X) of
        {Int, <<>>} -> {'Just', Int};
        _ -> 'None'
    end;
to_integer(X) when is_list(X) ->
    case string:to_integer(X) of
        {Int, ""} -> {'Just', Int};
        _ -> 'None'
    end;
to_integer(_) -> 'None'.

to_float(X) when is_float(X) -> {'Just', X};
to_float(X) when is_integer(X) -> {'Just', float(X)};
to_float(X) when is_binary(X) ->
    case string:to_float(X) of
        {Flt, <<>>} -> {'Just', Flt};
        _ ->
            case string:to_integer(X) of
                {Int, <<>>} -> {'Just', float(Int)};
                _ -> 'None'
            end
    end;
to_float(X) when is_list(X) ->
    case string:to_float(X) of
        {Flt, ""} -> {'Just', Flt};
        _ ->
            case string:to_integer(X) of
                {Int, ""} -> {'Just', float(Int)};
                _ -> 'None'
            end
    end;
to_float(_) -> 'None'.
