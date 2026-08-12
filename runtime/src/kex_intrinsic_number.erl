%% Kex.Intrinsic.Number — BEAM primitive backend for numeric intrinsics shared
%% by Integer and Float. Receiver is the first argument.
-module(kex_intrinsic_number).
-export([sub/3, mul/3, divide/3, pow/3, lt/3, gt/3, lte/3, gte/3, abs/1, sqrt/1, add/2, divide/2, pow/2, eq/2, neq/2,
          floor/1, ceil/1, round/1, toInteger/1,
          float_parse/1, float_parse_prefix/1,
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
add(A, B) when is_integer(A), is_integer(B) -> A + B;
add(A, B) -> checked(fun() -> A + B end, add, A, B, <<>>, <<"Float addition">>, <<"add">>).

%% `-` and `*` reach here whenever lowering cannot prove both operands are
%% Integers. Integer arithmetic answers immediately (bignums cannot overflow);
%% only float/unknown operands take the checked path, so the hot integer loops
%% pay two guard tests and nothing else.
%% `Loc` is the call site ("file:line:col: ", empty when unknown), passed in by
%% lowering so the message matches the walker's down to the location.
sub(A, B, _Loc) when is_integer(A), is_integer(B) -> A - B;
sub(A, B, Loc) -> checked(fun() -> A - B end, sub, A, B, Loc, <<"Float subtraction">>, <<"subtract">>).

mul(A, B, _Loc) when is_integer(A), is_integer(B) -> A * B;
mul(A, B, Loc) -> checked(fun() -> A * B end, mul, A, B, Loc, <<"Float multiplication">>, <<"multiply">>).

divide(A, B, Loc) -> divide_at(A, B, Loc).
pow(A, B, Loc) -> pow_at(A, B, Loc).

%% A float result that overflows raises `badarith` here and a typed message in
%% the walker ("Float multiplication: result overflowed (Infinity)"); a
%% non-number operand raises `badarith` too, where the walker says "Cannot
%% multiply X and Y". Report what the walker reports, including the sign the
%% result would have had — `badarith` itself carries neither.
checked(Fun, Op, A, B, Loc, FloatWhat, Verb) ->
    case is_number(A) andalso is_number(B) of
        false ->
            erlang:error(iolist_to_binary([Loc, "runtime error: Cannot ", Verb,
                                           " ", kex_io:value_type_name(A),
                                           " and ", kex_io:value_type_name(B)]));
        true ->
            try Fun()
            catch error:badarith ->
                erlang:error(iolist_to_binary(
                    [Loc, "runtime error: ", FloatWhat, ": result overflowed (",
                     overflow_sign(Op, A, B), "Infinity)"]))
            end
    end.

%% `math:pow` overflows to +Infinity except for a negative base with an odd
%% integral exponent.
overflow_sign(pow, A, B) ->
    Odd = is_integer(B) andalso (B rem 2) =/= 0,
    case A < 0 andalso Odd of true -> "-"; false -> "" end;
overflow_sign(divide, A, B) ->
    case (A < 0) =/= (B < 0) of true -> "-"; false -> "" end;
overflow_sign(mul, A, B) ->
    case (A < 0) =/= (B < 0) of true -> "-"; false -> "" end;
overflow_sign(sub, A, B) ->
    case erlang:abs(A) >= erlang:abs(B) of
        true -> sign_text(A);
        false -> case sign_text(B) of "-" -> ""; _ -> "-" end
    end;
overflow_sign(_Add, A, B) ->
    case erlang:abs(A) >= erlang:abs(B) of true -> sign_text(A); false -> sign_text(B) end.

sign_text(X) when X < 0 -> "-";
sign_text(_) -> "".

%% lt/gt/lte/gte — Kex's TYPED ordering, used wherever lowering cannot prove
%% both operands are the same comparable kind. Erlang's `<` orders any two
%% terms; Kex compares numbers with numbers (promoting Integer/Float), strings
%% with strings and chars with chars, and raises otherwise — the walker's rule
%% (src/interpreter/evaluator.cxx), reproduced here down to the message.
%% `Loc` is the call site ("file:line:col: ", empty when unknown), passed in
%% by lowering so the raised message matches the walker's exactly.
lt(A, B, Loc) -> ordering(A, B, Loc, fun(X, Y) -> X < Y end).
gt(A, B, Loc) -> ordering(A, B, Loc, fun(X, Y) -> X > Y end).
lte(A, B, Loc) -> ordering(A, B, Loc, fun(X, Y) -> X =< Y end).
gte(A, B, Loc) -> ordering(A, B, Loc, fun(X, Y) -> X >= Y end).

ordering(A, B, _Loc, Op) when is_number(A), is_number(B) -> Op(A, B);
ordering(A, B, _Loc, Op) when is_binary(A), is_binary(B) -> Op(A, B);
ordering({'Char', A}, {'Char', B}, _Loc, Op) -> Op(A, B);
ordering(A, B, Loc, _Op) ->
    erlang:error(iolist_to_binary([Loc, "runtime error: Cannot compare ",
                                   kex_io:value_type_name(A), " and ",
                                   kex_io:value_type_name(B)])).

%% eq/neq — Kex ==. Strict: a String is its own type, NOT a [Char], so a
%% binary and a list of tagged Chars holding the same text are NOT equal.
%% `chars` converts one to the other and `join("")` converts back.
%%
%% Numbers are the one deliberate exception: an Integer and a Float compare
%% numerically (`0 == 0.0`), matching the promotion `+`, `<` and friends
%% already do, so `1 =< 1.0` and `1 >= 1.0` cannot both hold while `1 == 1.0`
%% is false. Erlang's `==` is exactly that coercing comparison.
eq(A, B) when is_number(A), is_number(B) -> A == B;
eq(A, B) -> A =:= B.

neq(A, B) -> not eq(A, B).

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
divide(A, B) -> divide_at(A, B, <<>>).

divide_at(A, B, Loc) when is_integer(A), is_integer(B), B =:= 0 ->
    erlang:error(iolist_to_binary([Loc, "runtime error: Division by zero"]));
divide_at(A, B, _Loc) when is_integer(A), is_integer(B) -> A div B;
%% A float zero divisor is the same error as an integer one — `badarith` said
%% nothing about which, while the walker names it.
divide_at(_A, B, Loc) when B == 0 ->
    erlang:error(iolist_to_binary([Loc, "runtime error: Division by zero"]));
divide_at(A, B, Loc) -> checked(fun() -> A / B end, divide, A, B, Loc,
                                <<"Float division">>, <<"divide">>).

%% pow/2 keeps integral bases raised to non-negative integral exponents exact,
%% matching the interpreter's arbitrary-precision Integer result. Other powers
%% use Erlang's floating-point math semantics.
pow(A, B) -> pow_at(A, B, <<>>).

pow_at(A, B, _Loc) when is_integer(A), is_integer(B), B >= 0 -> int_pow(A, B, 1);
pow_at(A, B, Loc) -> checked(fun() -> math:pow(A, B) end, pow, A, B, Loc,
                             <<"Exponentiation">>, <<"raise">>).

int_pow(_, 0, Acc) -> Acc;
int_pow(Base, Exponent, Acc) when Exponent rem 2 =:= 1 ->
    int_pow(Base * Base, Exponent div 2, Acc * Base);
int_pow(Base, Exponent, Acc) -> int_pow(Base * Base, Exponent div 2, Acc).

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

%% string:to_float/1 requires a fraction part, so it rejects "1e10" — text the
%% Kex lexer accepts as a float literal, and which the walker's std::stod
%% parses fine. Splice in the ".0" so both backends accept exactly the float
%% texts the language's own grammar defines. The rewrite only ever inserts
%% characters *before* the exponent, so a Rest returned by the normalized
%% parse is still a suffix of the original and the trailing-character
%% positions below stay correct.
exp_normalized(S) when is_binary(S) ->
    unicode:characters_to_binary(exp_normalized(unicode:characters_to_list(S)));
exp_normalized(S) ->
    case bare_mantissa_exponent(S, []) of
        {Mantissa, Exponent} -> Mantissa ++ ".0" ++ Exponent;
        none -> S
    end.

%% {Mantissa, "e" ++ Rest} when S is digits (with an optional sign) followed
%% by a well-formed exponent and no ".", else none. Mirrors the lexer's rule
%% for when `e` starts an exponent at all.
bare_mantissa_exponent([C | T], Acc) when C >= $0, C =< $9 ->
    bare_mantissa_exponent(T, [C | Acc]);
bare_mantissa_exponent([C | T], []) when C =:= $+; C =:= $- ->
    bare_mantissa_exponent(T, [C]);
bare_mantissa_exponent([E | T], Acc) when E =:= $e; E =:= $E ->
    case has_digit(Acc) andalso starts_exponent(T) of
        true -> {lists:reverse(Acc), [E | T]};
        false -> none
    end;
bare_mantissa_exponent(_, _) -> none.

starts_exponent([C | T]) when C =:= $+; C =:= $- -> starts_exponent(T);
starts_exponent([D | _]) when D >= $0, D =< $9 -> true;
starts_exponent(_) -> false.

has_digit(L) -> lists:any(fun(C) -> C >= $0 andalso C =< $9 end, L).

%% Float.parse(s) -> Ok(Float) | Error(ParseError). Full match returns bare
%% Ok(Flt). Partial (trailing) returns Error(ParseError{value, rest, ...}).
%% A bare integer like "5" is accepted (to_integer fallback).
float_parse(S) when is_binary(S) ->
    case string:to_float(exp_normalized(S)) of
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
    case string:to_float(exp_normalized(S)) of
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
    case string:to_float(exp_normalized(S)) of
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
        {'Ok', Int} = Ok when is_integer(Int) -> Ok;
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
