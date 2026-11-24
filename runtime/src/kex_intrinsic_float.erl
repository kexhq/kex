%% Kex.Intrinsic.Float — BEAM primitive backend for Float namespace functions.
-module(kex_intrinsic_float).
-export([fromDigits/1, scaleUnit/3, formatFixed/2]).

%% Float.fromDigits(Text): the native half of Float.parse and Number.parse.
%% It reads canonical text only, an optional "-", digits, then optionally
%% ".digits" and an exponent, and answers None for anything else, or for a
%% value no Float holds (1e999): Kex has no infinities. The parses try it on
%% the text itself first and leave the rest to the scanner in
%% src/stdlib/culture.kex. binary_to_float wants a fraction, so a missing one
%% is written as ".0".
fromDigits(Text) when is_binary(Text) ->
    case canonical(Text) of
        {ok, Whole, Fraction, Exponent} ->
            try {'Just', binary_to_float(<<Whole/binary, ".", Fraction/binary, Exponent/binary>>)}
            catch error:badarg -> 'None'
            end;
        error -> 'None'
    end;
fromDigits(_) -> 'None'.

%% {ok, Whole, Fraction, Exponent} for canonical float text, the fraction
%% "0" when it has none.
canonical(Text) ->
    Start = case Text of <<$-, _/binary>> -> 1; _ -> 0 end,
    case digit_run(Text, Start) of
        0 -> error;
        N ->
            WholeSize = Start + N,
            <<Whole:WholeSize/binary, AfterWhole/binary>> = Text,
            case fraction(AfterWhole) of
                {ok, Fraction, AfterFraction} ->
                    case exponent(AfterFraction) of
                        {ok, Exponent} -> {ok, Whole, Fraction, Exponent};
                        error -> error
                    end;
                error -> error
            end
    end.

fraction(<<$., Rest/binary>>) ->
    case digit_run(Rest, 0) of
        0 -> error;
        N -> <<Fraction:N/binary, After/binary>> = Rest, {ok, Fraction, After}
    end;
fraction(Rest) -> {ok, <<"0">>, Rest}.

exponent(<<>>) -> {ok, <<>>};
exponent(<<E, Rest/binary>> = Exponent) when E =:= $e; E =:= $E ->
    Start = case Rest of <<S, _/binary>> when S =:= $+; S =:= $- -> 1; _ -> 0 end,
    case digit_run(Rest, Start) of
        N when N > 0, Start + N =:= byte_size(Rest) -> {ok, Exponent};
        _ -> error
    end;
exponent(_) -> error.

%% How many ASCII digits Bin holds from byte At.
digit_run(Bin, At) when At < byte_size(Bin) ->
    case binary:at(Bin, At) of
        C when C >= $0, C =< $9 -> 1 + digit_run(Bin, At + 1);
        _ -> 0
    end;
digit_run(_, _) -> 0.

formatFixed(Value, Places)
  when is_float(Value), is_integer(Places), Places >= 0, Places =< 15 ->
    Text = float_to_binary(Value, [{decimals, Places}]),
    %% A rounded zero has no sign, including a negative zero input.
    case Text of
        <<"-", Rest/binary>> ->
            case lists:all(fun(C) -> C =:= $0 orelse C =:= $. end,
                           binary_to_list(Rest)) of
                true -> Rest;
                false -> Text
            end;
        _ -> Text
    end;
formatFixed(_, _) ->
    erlang:error({kex_error, <<"decimal places must be between 0 and 15">>}).

%% Clamp rounding at the excluded upper endpoint to its predecessor.
scaleUnit(Low, High, Unit) when is_float(Low), is_float(High), Low < High ->
    Value = Low * (1.0 - Unit) + High * Unit,
    if Value >= High -> previous(High);
       Value < Low -> Low;
       true -> Value
    end;
scaleUnit(_, _, _) ->
    erlang:error({kex_error, <<"Random.float: bounds must be finite and strictly ascending">>}).
previous(Value) when Value == 0.0 -> -5.0e-324;
previous(Value) ->
    <<Bits:64/unsigned>> = <<Value:64/float>>,
    Previous = if Value > 0.0 -> Bits - 1; true -> Bits + 1 end,
    <<Result:64/float>> = <<Previous:64/unsigned>>,
    Result.
