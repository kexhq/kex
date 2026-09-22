%% Kex.Intrinsic.Float — BEAM primitive backend for Float namespace functions.
-module(kex_intrinsic_float).
-export([parse/1, parsePrefix/1, scaleUnit/3]).

parse(S) -> kex_intrinsic_number:float_parse(S).
parsePrefix(S) -> kex_intrinsic_number:float_parse_prefix(S).

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
