-module(kex_intrinsic_netip).
-export([address/1, network/1, addressString/1, networkString/1, version/1,
         'loopback?'/1, 'private?'/1, 'unspecified?'/1, 'multicast?'/1,
         prefix/1, 'contains?'/2, first/1, last/1]).

address(Text) -> case inet:parse_address(binary_to_list(Text)) of
    {ok, IP} -> {'Ok', {'Net.IP.Address', iolist_to_binary(inet:ntoa(IP))}};
    _ -> error_value(<<"invalid IP address">>)
end.

network(Text) ->
    case binary:split(Text, <<"/">>) of
        [IPText, PrefixText] -> case {inet:parse_address(binary_to_list(IPText)), parse_int(PrefixText)} of
            {{ok, IP}, P} when is_integer(P) ->
                Bits = bits(IP),
                case P >= 0 andalso P =< Bits of
                    true -> Base = int_ip((ip_int(IP) bsr (Bits-P)) bsl (Bits-P), Bits),
                            {'Ok', {'Net.IP.Network', <<(iolist_to_binary(inet:ntoa(Base)))/binary, "/", PrefixText/binary>>}};
                    false -> error_value(<<"invalid network prefix">>)
                end;
            _ -> error_value(<<"invalid network address">>)
        end;
        _ -> error_value(<<"network requires a prefix length">>)
    end.

addressString({'Net.IP.Address', Text}) -> Text.
networkString({'Net.IP.Network', Text}) -> Text.
version({'Net.IP.Address', Text}) -> case parsed(Text) of IP when tuple_size(IP) == 4 -> 4; _ -> 6 end.
'loopback?'({'Net.IP.Address', Text}) -> case parsed(Text) of {127,_,_,_} -> true; {0,0,0,0,0,0,0,1} -> true; _ -> false end.
'private?'({'Net.IP.Address', Text}) -> case parsed(Text) of
    {10,_,_,_} -> true; {172,B,_,_} when B >= 16, B =< 31 -> true; {192,168,_,_} -> true;
    {A,_,_,_,_,_,_,_} when (A band 16#fe00) == 16#fc00 -> true; _ -> false end.
'unspecified?'({'Net.IP.Address', Text}) -> ip_int(parsed(Text)) == 0.
'multicast?'({'Net.IP.Address', Text}) -> case parsed(Text) of {A,_,_,_} when A >= 224, A =< 239 -> true; {A,_,_,_,_,_,_,_} when (A band 16#ff00) == 16#ff00 -> true; _ -> false end.
prefix({'Net.IP.Network', Text}) -> [_, P] = binary:split(Text, <<"/">>), parse_int(P).
'contains?'(Network, {'Net.IP.Address', Text}) ->
    {Base, P, Bits} = network_parts(Network), IP = parsed(Text),
    bits(IP) == Bits andalso (ip_int(IP) bsr (Bits-P)) == (ip_int(Base) bsr (Bits-P)).
first(Network) -> {Base,_,_} = network_parts(Network), {'Net.IP.Address', iolist_to_binary(inet:ntoa(Base))}.
last(Network) -> {Base,P,Bits} = network_parts(Network), Mask = (1 bsl (Bits-P))-1, {'Net.IP.Address', iolist_to_binary(inet:ntoa(int_ip(ip_int(Base) bor Mask, Bits)))}.

network_parts({'Net.IP.Network', Text}) -> [A,P0] = binary:split(Text, <<"/">>), IP=parsed(A), {IP,parse_int(P0),bits(IP)}.
parsed(Text) -> {ok, IP}=inet:parse_address(binary_to_list(Text)), IP.
bits(IP) when tuple_size(IP)==4 -> 32; bits(_) -> 128.
ip_int(IP) -> lists:foldl(fun(X,A)->(A bsl (case tuple_size(IP) of 4->8;_->16 end)) bor X end,0,tuple_to_list(IP)).
int_ip(N,32) -> list_to_tuple([(N bsr S) band 255 || S <- [24,16,8,0]]);
int_ip(N,128) -> list_to_tuple([(N bsr S) band 65535 || S <- [112,96,80,64,48,32,16,0]]).
parse_int(B) -> try binary_to_integer(B) catch _:_ -> invalid end.
error_value(M) -> {'Error', {'Net.NetError', 'Parse', 'Connect', M, 'None', 'None'}}.
