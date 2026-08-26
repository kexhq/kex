%% Kex.Intrinsic.Binary — primitives over the opaque {'Binary', Bin} value.
-module(kex_intrinsic_binary).
-export([fromBytes/1, bytes/1, length/1, at/2, take/2, drop/2, concat/2,
         hex/1, fromHex/1, base64/1, fromBase64/1, render/1]).

fromBytes(Values) -> {'Binary', list_to_binary(Values)}.
bytes({'Binary', Bin}) -> binary_to_list(Bin).
length({'Binary', Bin}) -> byte_size(Bin).
at({'Binary', Bin}, I) when is_integer(I), I >= 0, I < byte_size(Bin) ->
    {'Just', binary:at(Bin, I)};
at(_, _) -> 'None'.
take({'Binary', _Bin}, N) when N =< 0 -> {'Binary', <<>>};
take({'Binary', Bin}, N) ->
    Size = min(N, byte_size(Bin)), {'Binary', binary:part(Bin, 0, Size)}.
drop({'Binary', Bin}, N) when N =< 0 -> {'Binary', Bin};
drop({'Binary', Bin}, N) ->
    Size = min(N, byte_size(Bin)),
    {'Binary', binary:part(Bin, Size, byte_size(Bin) - Size)}.
concat({'Binary', A}, {'Binary', B}) -> {'Binary', iolist_to_binary([A, B])}.
hex({'Binary', Bin}) -> string:lowercase(binary:encode_hex(Bin)).
fromHex(Text) when is_binary(Text), byte_size(Text) rem 2 =:= 0 ->
    case strict_lower_hex(Text) of
        true -> try {'Just', {'Binary', binary:decode_hex(Text)}} catch _:_ -> 'None' end;
        false -> 'None'
    end;
fromHex(_) -> 'None'.
strict_lower_hex(Text) ->
    lists:all(fun(C) -> (C >= $0 andalso C =< $9) orelse
                        (C >= $a andalso C =< $f) end, binary_to_list(Text)).
base64({'Binary', Bin}) -> base64:encode(Bin).
fromBase64(Text) when is_binary(Text) ->
    try
        Decoded = base64:decode(Text),
        case base64:encode(Decoded) =:= Text of
            true -> {'Just', {'Binary', Decoded}};
            false -> 'None'
        end
    catch _:_ -> 'None' end;
fromBase64(_) -> 'None'.
render({'Binary', Bin}) ->
    iolist_to_binary(["#Binary<", integer_to_binary(byte_size(Bin)), " bytes>"]).
