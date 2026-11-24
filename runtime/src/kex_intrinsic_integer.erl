%% Kex.Intrinsic.Integer — BEAM primitive backend for integer intrinsics.
%% The typed Integer stdlib lives in src/stdlib/number.kex; `even?`/`odd?`
%% are expressed there in Kex on top of `modulo`. Receiver is the first arg.
-module(kex_intrinsic_integer).
-export([modulo/2, times/2, fromDigits/2, parse_in_base/2]).

%% Mathematical modulo: the result has the divisor's sign, matching the Kex
%% interpreter rather than Erlang's dividend-signed rem/2.
modulo(A, B) when is_integer(A), is_integer(B) -> ((A rem B) + B) rem B;
%% `modulo` belongs to `make Integer`, so a non-integer receiver reaches here
%% only because the call had a single owner and needed no dispatcher. Report it
%% the way the walker does instead of failing with `badarith`.
modulo(A, _B) ->
    erlang:error(iolist_to_binary(["runtime error: undefined method: modulo for ",
                                   kex_io:value_type_name(A)])).
%% n.times { |i| block(i) } — call block with 0..n-1. A parameterless
%% `do ... end` block (a `Block<Void>` overload) ignores the index rather
%% than raising badarity.
times(N, Fun) ->
    {arity, A} = erlang:fun_info(Fun, arity),
    G = case A of
        0 -> fun(_) -> Fun() end;
        _ -> Fun
    end,
    lists:foreach(G, lists:seq(0, N - 1)).

%% Integer.fromDigits(Text, Radix): the native half of Integer.parse and
%% Number.parse. It reads canonical text only, an optional "-" then digits in
%% the radix, and answers None for anything else: the parses try it on the
%% text itself first and leave the rest to the scanner in
%% src/stdlib/culture.kex. The walker's twin is in
%% src/interpreter/stdlib/number.cxx.
fromDigits(Text, Radix)
  when is_binary(Text), is_integer(Radix), Radix >= 2, Radix =< 36 ->
    Start = case Text of <<$-, _/binary>> -> 1; _ -> 0 end,
    <<_:Start/binary, Digits/binary>> = Text,
    case Digits =/= <<>> andalso all_digits(Digits, Radix) of
        true -> {'Just', binary_to_integer(Text, Radix)};
        false -> 'None'
    end;
fromDigits(_, _) -> 'None'.

all_digits(<<C, Rest/binary>>, Radix) ->
    digit_value(C) < Radix andalso all_digits(Rest, Radix);
all_digits(<<>>, _) -> true.

digit_value(C) when C >= $0, C =< $9 -> C - $0;
digit_value(C) when C >= $a, C =< $z -> C - $a + 10;
digit_value(C) when C >= $A, C =< $Z -> C - $A + 10;
digit_value(_) -> 99.

%% Whole-string parse in base 2..36, mirroring value.cxx's parseIntegerInBase:
%% an optional sign, then digits valid for that base, and nothing else.
%% list_to_integer/2 enforces exactly that and, unlike string:to_integer/1,
%% refuses a partial parse. Digits above 9 are accepted in either case.
parse_in_base(S, Radix) when is_binary(S) ->
    parse_in_base(unicode:characters_to_list(S), Radix);
parse_in_base(S, Radix) when is_integer(Radix), Radix >= 2, Radix =< 36 ->
    try {ok, list_to_integer(S, Radix)}
    catch error:badarg -> error
    end;
parse_in_base(_, _) -> error.
