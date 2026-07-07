%% Kex.Intrinsic.Integer — BEAM primitive backend for integer intrinsics.
%% The typed Integer stdlib lives in src/prelude/integer.kex; `even?`/`odd?`
%% are expressed there in Kex on top of `modulo`. Receiver is the first arg.
-module(kex_intrinsic_integer).
-export([modulo/2, times/2, integer_parse/1]).

modulo(A, B) -> A rem B.
%% n.times { |i| block(i) } — call block with 0..n-1.
times(N, Fun) -> lists:foreach(Fun, lists:seq(0, N - 1)).

%% Integer.parse(s) — parse a string to integer, returning Ok(Int) or
%% Error(reason). Matches src/interpreter/stdlib/number.cxx exactly.
%% Moved from kex_io where parsing didn't belong.
integer_parse(S) ->
    case string:to_integer(S) of
        {Int, ""} -> {'Ok', Int};
        _ -> {'Error', "invalid integer: " ++ S}
    end.
