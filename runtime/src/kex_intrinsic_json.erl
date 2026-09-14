%% Kex.Intrinsic.Json — the BEAM fast path behind JSON.parse / JSON.stringify.
%%
%% src/stdlib/json.kex keeps the Kex implementation as the definition: it is
%% what the tree-walker runs, what parses JSONC, and what reports a precise
%% `JSON.Error` with a position. These answer only when they can give exactly
%% the value that implementation would, and `None` otherwise, so the Kex code
%% falls back to itself (kexhq/kex#333).
-module(kex_intrinsic_json).
-export([decode/1, encode/1]).

%% decode(Text) -> Just(Value) | None.
%% Objects are maps with String (binary) keys, arrays lists, null None —
%% the shapes json.kex's own parser builds. Any failure is None: the Kex parser
%% then runs and produces the positioned error.
decode(Text) ->
    Bin = kex_io:to_string_bin(Text),
    Decoders = #{null => 'None',
                 %% Last duplicate key wins, as `Map.put` in a loop does.
                 object_finish => fun(Acc, Old) ->
                                          {maps:from_list(lists:reverse(Acc)), Old}
                                  end},
    try json:decode(Bin, ok, Decoders) of
        {Value, ok, Rest} ->
            case only_whitespace(Rest) of
                true -> {'Just', Value};
                false -> 'None'
            end
    catch
        _:_ -> 'None'
    end.

only_whitespace(<<>>) -> true;
only_whitespace(<<C, Rest/binary>>) when C =:= $\s; C =:= $\t; C =:= $\n; C =:= $\r ->
    only_whitespace(Rest);
only_whitespace(_) -> false.

%% encode(Value) -> Just(Text). Mirrors json.kex's `stringify` rule for rule:
%% unknown kinds (chars, tuples, records) render as null, object keys come out
%% in canonical (sorted) order, strings escape exactly the characters
%% `encodeString` does and nothing else.
encode(Value) ->
    {'Just', iolist_to_binary(value(Value))}.

value('None') -> <<"null">>;
%% An optional is JSON's nullable: `Just(x)` is `x`, as `None` is `null`.
value({'Just', V}) -> value(V);
value(true) -> <<"true">>;
value(false) -> <<"false">>;
value(V) when is_integer(V) -> integer_to_binary(V);
value(V) when is_float(V) -> kex_io:to_string_bin(V);
value(V) when is_binary(V) -> string(V);
value(V) when is_list(V) -> [$[, lists:join($,, [value(E) || E <- V]), $]];
value(V) when is_map(V) ->
    Fields = [[string(key(K)), $:, value(Item)] || {K, Item} <- lists:sort(maps:to_list(V))],
    [${, lists:join($,, Fields), $}];
value(_) -> <<"null">>.

%% json.kex's objectKey: a String key as-is; anything else by its text, minus
%% the leading colon an atom's text carries.
key(K) when is_binary(K) -> K;
key(K) ->
    case kex_io:to_string_bin(K) of
        <<$:, Rest/binary>> -> Rest;
        Text -> Text
    end.

%% Byte-wise is exact: every byte escaped is ASCII, and no byte of a UTF-8
%% multi-byte sequence is below 128.
string(S) -> [$", escape(S, []), $"].

escape(<<>>, Acc) -> lists:reverse(Acc);
escape(<<$", R/binary>>, Acc) -> escape(R, [<<"\\\"">> | Acc]);
escape(<<$\\, R/binary>>, Acc) -> escape(R, [<<"\\\\">> | Acc]);
escape(<<8, R/binary>>, Acc) -> escape(R, [<<"\\b">> | Acc]);
escape(<<12, R/binary>>, Acc) -> escape(R, [<<"\\f">> | Acc]);
escape(<<$\n, R/binary>>, Acc) -> escape(R, [<<"\\n">> | Acc]);
escape(<<$\r, R/binary>>, Acc) -> escape(R, [<<"\\r">> | Acc]);
escape(<<$\t, R/binary>>, Acc) -> escape(R, [<<"\\t">> | Acc]);
escape(<<C, R/binary>>, Acc) when C < 32 ->
    Hex = string:lowercase(integer_to_binary(C, 16)),
    Pad = binary:copy(<<"0">>, 4 - byte_size(Hex)),
    escape(R, [<<"\\u", Pad/binary, Hex/binary>> | Acc]);
escape(<<C, R/binary>>, Acc) -> escape(R, [C | Acc]).
