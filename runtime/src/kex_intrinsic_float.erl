%% Kex.Intrinsic.Float — BEAM primitive backend for Float namespace functions.
-module(kex_intrinsic_float).
-export([parse/1, parsePrefix/1]).

parse(S) -> kex_intrinsic_number:float_parse(S).
parsePrefix(S) -> kex_intrinsic_number:float_parse_prefix(S).
