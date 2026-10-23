%% Kex.Intrinsic.Float — BEAM primitive backend for Float namespace functions.
-module(kex_intrinsic_float).
-export([parse/1, parsePrefix/1, scaleUnit/3, formatFixed/2]).

parse(S) -> kex_intrinsic_number:float_parse(S).
parsePrefix(S) -> kex_intrinsic_number:float_parse_prefix(S).

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
