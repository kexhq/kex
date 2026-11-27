-module(kex_intrinsic_netip).
-export([address/1, network/1, addressString/1, networkString/1, version/1,
         'loopback?'/1, 'private?'/1, 'unspecified?'/1, 'multicast?'/1,
         prefix/1, 'contains?'/2, first/1, last/1]).

%% Addresses take the syntax the walker's parseIP takes: an IPv4 dotted
%% quad of exactly four decimal octets without leading zeros, or an IPv6
%% address (optionally ending in such a quad), with no zone identifier.
%% inet:parse_address/1 is far looser (127.1, 0x7f.0.0.1, octal 010.0.0.1,
%% %zone), and those spellings are how SSRF allow-lists get bypassed.
address(Text) -> case strict(Text) of
    {ok, IP} -> {'Ok', {'Net.IP.Address', render(IP)}};
    error -> error_value(<<"invalid IP address">>)
end.

network(Text) ->
    case binary:matches(Text, <<"/">>) of
        [] -> error_value(<<"network requires a prefix length">>);
        Slashes ->
            {Slash, 1} = lists:last(Slashes),
            IPText = binary:part(Text, 0, Slash),
            PrefixText = binary:part(Text, Slash + 1, byte_size(Text) - Slash - 1),
            case strict(IPText) of
                {ok, IP} ->
                    Bits = bits(IP),
                    case parse_prefix(PrefixText) of
                        P when is_integer(P), P =< Bits ->
                            Base = int_ip((ip_int(IP) bsr (Bits-P)) bsl (Bits-P), Bits),
                            {'Ok', {'Net.IP.Network', <<(render(Base))/binary, "/", (integer_to_binary(P))/binary>>}};
                        _ -> error_value(<<"invalid network prefix">>)
                    end;
                error -> error_value(<<"invalid network address">>)
            end
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
prefix({'Net.IP.Network', Text}) -> {_, P, _} = network_parts({'Net.IP.Network', Text}), P.
'contains?'(Network, {'Net.IP.Address', Text}) ->
    {Base, P, Bits} = network_parts(Network), IP = parsed(Text),
    bits(IP) == Bits andalso (ip_int(IP) bsr (Bits-P)) == (ip_int(Base) bsr (Bits-P)).
first(Network) -> {Base,_,_} = network_parts(Network), {'Net.IP.Address', render(Base)}.
last(Network) -> {Base,P,Bits} = network_parts(Network), Mask = (1 bsl (Bits-P))-1, {'Net.IP.Address', render(int_ip(ip_int(Base) bor Mask, Bits))}.

network_parts({'Net.IP.Network', Text}) -> [A,P0] = binary:split(Text, <<"/">>), IP=parsed(A), {IP,binary_to_integer(P0),bits(IP)}.
parsed(Text) -> {ok, IP} = strict(Text), IP.

%% inet:parse_strict_address/1 still takes a leading `+` on an IPv4 octet and
%% a `%zone` suffix, so only hex digits, `:` and `.` get that far, and every
%% octet of a trailing dotted quad must be plain decimal with no leading zero.
strict(Text) ->
    S = binary_to_list(Text),
    case lists:all(fun ip_char/1, S) andalso quad_ok(S) of
        true -> case inet:parse_strict_address(S) of {ok, IP} -> {ok, IP}; _ -> error end;
        false -> error
    end.

ip_char(C) -> (C >= $0 andalso C =< $9) orelse (C >= $a andalso C =< $f)
    orelse (C >= $A andalso C =< $F) orelse C == $: orelse C == $..

quad_ok(S) -> case lists:member($., S) of
    false -> true;
    true -> lists:all(fun octet_ok/1, string:split(lists:last(string:split(S, ":", all)), ".", all))
end.

octet_ok("0") -> true;
octet_ok([$0 | _]) -> false;
octet_ok(D) -> D =/= [] andalso length(D) =< 3 andalso lists:all(fun digit/1, D).

%% A prefix length is plain decimal: no sign, no spaces, no leading zero.
parse_prefix(<<"0">>) -> 0;
parse_prefix(<<$0, _/binary>>) -> invalid;
parse_prefix(B) when byte_size(B) >= 1, byte_size(B) =< 3 ->
    case lists:all(fun digit/1, binary_to_list(B)) of true -> binary_to_integer(B); false -> invalid end;
parse_prefix(_) -> invalid.

digit(C) -> C >= $0 andalso C =< $9.

%% RFC 5952 text, the walker's renderIP, so both backends print the same form:
%% the longest run of two or more zero groups (the first, on a tie) becomes
%% `::`, and only an IPv4-mapped address (::ffff:) ends in a dotted quad.
%% inet:ntoa/1 differs, e.g. it renders ::2 as ::0.0.0.2.
render({_,_,_,_} = IP) -> iolist_to_binary(inet:ntoa(IP));
render(IP) ->
    Words = tuple_to_list(IP),
    {Base, Len} = zero_run(Words),
    iolist_to_binary(render6(Words, 0, Base, Len)).

render6(_, 8, Base, Len) when Base + Len == 8 -> ":";
render6(_, 8, _, _) -> [];
render6(Words, I, Base, Len) when I >= Base, I < Base + Len ->
    [case I == Base of true -> ":"; false -> [] end | render6(Words, I + 1, Base, Len)];
render6([_,_,_,_,_,16#ffff,W6,W7], 6, 0, 5) ->
    [":", inet:ntoa({W6 bsr 8, W6 band 255, W7 bsr 8, W7 band 255})];
render6(Words, I, Base, Len) ->
    [case I of 0 -> []; _ -> ":" end, string:lowercase(integer_to_list(lists:nth(I + 1, Words), 16))
     | render6(Words, I + 1, Base, Len)].

zero_run(Words) -> zero_run(Words, 0, -1, 0, -1, 0).
zero_run([], _, Cur, CurLen, Best, BestLen) ->
    {B, L} = case CurLen > BestLen of true -> {Cur, CurLen}; false -> {Best, BestLen} end,
    case L >= 2 of true -> {B, L}; false -> {-1, 0} end;
zero_run([0 | T], I, -1, _, Best, BestLen) -> zero_run(T, I + 1, I, 1, Best, BestLen);
zero_run([0 | T], I, Cur, CurLen, Best, BestLen) -> zero_run(T, I + 1, Cur, CurLen + 1, Best, BestLen);
zero_run([_ | T], I, Cur, CurLen, _, BestLen) when CurLen > BestLen -> zero_run(T, I + 1, -1, 0, Cur, CurLen);
zero_run([_ | T], I, _, _, Best, BestLen) -> zero_run(T, I + 1, -1, 0, Best, BestLen).

bits(IP) when tuple_size(IP)==4 -> 32; bits(_) -> 128.
ip_int(IP) -> lists:foldl(fun(X,A)->(A bsl (case tuple_size(IP) of 4->8;_->16 end)) bor X end,0,tuple_to_list(IP)).
int_ip(N,32) -> list_to_tuple([(N bsr S) band 255 || S <- [24,16,8,0]]);
int_ip(N,128) -> list_to_tuple([(N bsr S) band 65535 || S <- [112,96,80,64,48,32,16,0]]).
error_value(M) -> {'Error', {'Net.NetError', 'Parse', 'Connect', M, 'None', 'None'}}.
