%% Kex.Intrinsic.Json — the BEAM fast path behind JSON.parse / JSON.stringify.
%%
%% src/stdlib/json.kex keeps the Kex implementation as the definition: it is
%% what the tree-walker runs, what parses JSONC, and what reports a precise
%% `JSON.Error` with a position. These answer only when they can give exactly
%% the value that implementation would, and `None` otherwise, so the Kex code
%% falls back to itself (kexhq/kex#333). They rely on OTP's `json` module,
%% new in OTP 27 — the oldest release Kex supports on BEAM (CMakeLists.txt
%% refuses to build against anything older).
-module(kex_intrinsic_json).
-export([decode/1, decodeCommented/1, encode/1]).

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

%% decodeCommented(Text) -> Just(Value) | None. JSONC: `decode` after
%% blanking out `//` and `/* */` comments, which json.kex accepts wherever it
%% accepts whitespace. Without this `allowComments: true` always ran the Kex
%% parser, taking seconds on a few kilobytes (kexhq/kex#405). An unterminated
%% comment is None, and the Kex parser reports it with its position.
decodeCommented(Text) ->
    case strip_comments(kex_io:to_string_bin(Text), []) of
        {ok, Stripped} -> decode(Stripped);
        error -> 'None'
    end.

%% Comment markers inside a string literal are text, so strings are copied
%% through as they are, escapes included.
strip_comments(<<>>, Acc) ->
    {ok, iolist_to_binary(lists:reverse(Acc))};
strip_comments(<<$", _/binary>> = Bin, Acc) ->
    End = string_end(Bin, 1),
    strip_comments(binary:part(Bin, End, byte_size(Bin) - End),
                   [binary:part(Bin, 0, End) | Acc]);
strip_comments(<<"//", Rest/binary>>, Acc) ->
    case binary:match(Rest, <<"\n">>) of
        {At, _} -> strip_comments(binary:part(Rest, At, byte_size(Rest) - At), [$\s | Acc]);
        nomatch -> strip_comments(<<>>, [$\s | Acc])
    end;
strip_comments(<<"/*", Rest/binary>>, Acc) ->
    case binary:match(Rest, <<"*/">>) of
        {At, _} -> strip_comments(binary:part(Rest, At + 2, byte_size(Rest) - At - 2), [$\s | Acc]);
        nomatch -> error
    end;
strip_comments(Bin, Acc) ->
    case binary:match(Bin, [<<"\"">>, <<"/">>]) of
        {0, _} ->
            %% A lone `/`: not a comment, and not JSON either. Kept, so the
            %% decoder rejects it and the Kex parser explains why.
            <<Slash, Rest/binary>> = Bin,
            strip_comments(Rest, [Slash | Acc]);
        {At, _} ->
            strip_comments(binary:part(Bin, At, byte_size(Bin) - At),
                           [binary:part(Bin, 0, At) | Acc]);
        nomatch ->
            strip_comments(<<>>, [Bin | Acc])
    end.

%% The offset just past the closing quote of the string literal whose body
%% starts at From, or the end of Bin when it is unterminated.
string_end(Bin, From) when From >= byte_size(Bin) -> byte_size(Bin);
string_end(Bin, From) ->
    case binary:match(Bin, [<<"\\">>, <<"\"">>], [{scope, {From, byte_size(Bin) - From}}]) of
        {At, 1} ->
            case binary:at(Bin, At) of
                $" -> At + 1;
                _ -> string_end(Bin, At + 2)
            end;
        nomatch -> byte_size(Bin)
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
