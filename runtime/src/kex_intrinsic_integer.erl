%% Kex.Intrinsic.Integer — BEAM primitive backend for integer intrinsics.
%% The typed Integer stdlib lives in src/stdlib/number.kex; `even?`/`odd?`
%% are expressed there in Kex on top of `modulo`. Receiver is the first arg.
-module(kex_intrinsic_integer).
-export([modulo/2, times/2, integer_parse/1,
         parse/1, parse/2, parsePrefix/1, parse_in_base/2]).

%% Mathematical modulo: the result has the divisor's sign, matching the Kex
%% interpreter rather than Erlang's dividend-signed rem/2.
modulo(A, B) when is_integer(A), is_integer(B) -> ((A rem B) + B) rem B;
%% `modulo` belongs to `make Integer`, so a non-integer receiver reaches here
%% only because the call had a single owner and needed no dispatcher. Report it
%% the way the walker does instead of failing with `badarith`.
modulo(A, _B) ->
    erlang:error(iolist_to_binary(["runtime error: Undefined method: modulo for ",
                                   kex_io:value_type_name(A)])).
%% n.times { |i| block(i) } — call block with 0..n-1.
times(N, Fun) -> lists:foreach(Fun, lists:seq(0, N - 1)).

%% ParseError is the tagged tuple {'ParseError', Input, Position, Value, Message, Rest}
%% (record lowered by src/ir/lower.cxx — element 2/3/4/5/6 = input/position/
%% value/message/rest). Matches src/interpreter/stdlib/number.cxx exactly.
parse_error(S, Position, Value, Message, Rest) ->
    {'ParseError', S, Position, Value, Message, Rest}.

%% Integer.parse(s) -> Ok(Int) | Error(ParseError). Full match returns bare
%% Ok(Int). Partial (trailing) returns Error(ParseError{value, rest, ...}) so
%% the caller can inspect what was parsed and where to continue.
integer_parse(S) when is_binary(S) ->
    case string:to_integer(S) of
        {Int, <<>>} -> {'Ok', Int};
        {Int, Rest} when is_integer(Int) ->
            {'Error', parse_error(S, byte_size(S) - byte_size(Rest), Int,
                                  <<"trailing characters after integer">>, Rest)};
        _ -> {'Error', parse_error(S, 0, 'None', <<"invalid integer">>, S)}
    end;
integer_parse(S) ->
    case string:to_integer(S) of
        {Int, ""} -> {'Ok', Int};
        {Int, Rest} when is_integer(Int) ->
            {'Error', parse_error(unicode:characters_to_binary(S),
                                  length(S) - length(Rest), Int,
                                  <<"trailing characters after integer">>,
                                  unicode:characters_to_binary(Rest))};
        _ -> {'Error', parse_error(unicode:characters_to_binary(S), 0, 'None',
                                   <<"invalid integer">>,
                                   unicode:characters_to_binary(S))}
    end.

%% Integer.parsePrefix(s) -> Just({Int, Rest}) | none. Parses a leading
%% integer, returning a proper Optional: Just((val, rest)) on success, none
%% on failure. Enables one-liner: let Just((val, rest)) = Int.parsePrefix(s)
integer_parse_prefix(S) when is_binary(S) ->
    case string:to_integer(S) of
        {Int, Rest} when is_integer(Int) -> {'Just', {Int, Rest}};
        _ -> 'None'
    end;
integer_parse_prefix(S) ->
    case string:to_integer(S) of
        {Int, Rest} when is_integer(Int) -> {'Just', {Int, Rest}};
        _ -> 'None'
    end.

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

as_binary(S) when is_binary(S) -> S;
as_binary(S) -> unicode:characters_to_binary(S).

%% Integer.parse(s, radix: N). In a non-decimal base there is no meaningful
%% numeric prefix to report for a bad digit ("12z" in base 16), so the whole
%% string either parses or it does not — see src/interpreter/stdlib/number.cxx.
parse(S, Radix) when is_integer(Radix), Radix >= 2, Radix =< 36 ->
    case parse_in_base(S, Radix) of
        {ok, N} -> {'Ok', N};
        error ->
            Msg = unicode:characters_to_binary(
                    "invalid integer for radix " ++ integer_to_list(Radix)),
            {'Error', parse_error(as_binary(S), 0, 'None', Msg, as_binary(S))}
    end;
parse(S, _Radix) ->
    {'Error', parse_error(as_binary(S), 0, 'None',
                          <<"radix must be between 2 and 36">>, as_binary(S))}.

parse(S) -> integer_parse(S).
parsePrefix(S) -> integer_parse_prefix(S).
