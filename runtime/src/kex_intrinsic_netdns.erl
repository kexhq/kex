-module(kex_intrinsic_netdns).
-export([name/1, addresses/1]).
name(Text) when is_binary(Text), byte_size(Text) > 0 -> {'Ok', {'Net.DNS.Name', Text, string:lowercase(Text)}};
name(_) -> {'Error', {'Net.NetError', 'Parse', 'DNS', <<"invalid DNS name">>, 'None', 'None'}}.

addresses({'Net.DNS.Name', _Display, ASCII}) ->
    V4 = case inet:getaddrs(binary_to_list(ASCII), inet) of {ok, A4} -> A4; _ -> [] end,
    V6 = case inet:getaddrs(binary_to_list(ASCII), inet6) of {ok, A6} -> A6; _ -> [] end,
    case V6 ++ V4 of
        [] -> {'Error', {'Net.NetError', 'Resolve', 'DNS', <<"DNS name did not resolve">>, 'None', 'None'}};
        Values -> {'Ok', [{'Net.IP.Address', iolist_to_binary(inet:ntoa(IP))} || IP <- Values]}
    end.
