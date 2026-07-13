%% Kex.Intrinsic.Integer — BEAM primitive backend for integer intrinsics.
%% The typed Integer stdlib lives in src/prelude/integer.kex; `even?`/`odd?`
%% are expressed there in Kex on top of `modulo`. Receiver is the first arg.
-module(kex_intrinsic_integer).
-export([modulo/2, times/2, integer_parse/1, integer_parse_prefix/1]).

%% Mathematical modulo: the result has the divisor's sign, matching the Kex
%% interpreter rather than Erlang's dividend-signed rem/2.
modulo(A, B) -> ((A rem B) + B) rem B.
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
