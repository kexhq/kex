-module(kex_intrinsic_uri).
-export([parse/1, fromIRI/1, normalize/1, resolve/2, scheme/1, host/1, query/1, bracketed/1,
         queryParse/1, queryEncode/1, formFrom/1, formParse/1, formEncode/1,
         idnaHost/1, redacted/1]).

%% The same checks, in the same order and with the same kinds and positions,
%% as the walker's URI::parse (src/interpreter/stdlib/net.cxx): uri_string on
%% its own accepts a space, an empty authority and any port number.
parse(Text) when is_binary(Text) ->
    case check_text(Text) of
        ok -> case well_formed(Text) of
            true -> {'Ok', {'URI.URI', Text}};
            false -> uri_error('InvalidSyntax', <<"malformed URI reference">>)
        end;
        Error -> Error
    end.

check_text(Text) ->
    case ascii(Text) of
        false -> uri_error('NonASCII', <<"URI contains non-ASCII text">>);
        true -> check_bytes(Text, 0)
    end.

check_bytes(<<>>, _) -> ok;
check_bytes(<<C, _/binary>>, I) when C < 16#20; C =:= 16#7f; C =:= $  ->
    uri_error('InvalidSyntax', <<"invalid URI text">>, I);
check_bytes(<<$%, A, B, Rest/binary>>, I) ->
    case hex(A) andalso hex(B) of
        true -> check_bytes(Rest, I + 3);
        false -> uri_error('InvalidEscape', <<"invalid URI text">>, I)
    end;
check_bytes(<<$%, _/binary>>, I) -> uri_error('InvalidEscape', <<"invalid URI text">>, I);
check_bytes(<<_, Rest/binary>>, I) -> check_bytes(Rest, I + 1).

%% An authority, when there is one, names a host, and a port fits in 16 bits.
well_formed(Text) ->
    case safe_parse(Text) of
        {ok, Map} ->
            HostOk = case maps:find(host, Map) of
                {ok, Host} -> Host =/= <<>>;
                error -> binary:match(Text, <<"//">>) =:= nomatch orelse
                         not authority_marker(Text)
            end,
            PortOk = case maps:find(port, Map) of
                {ok, Port} when is_integer(Port) -> Port =< 65535;
                {ok, _} -> false;
                error -> true
            end,
            HostOk andalso PortOk;
        error -> false
    end.

%% Whether "//" starts an authority (right after the scheme, or at the start
%% of a relative reference), rather than appearing inside a path.
authority_marker(Text) ->
    case binary:match(Text, <<":">>) of
        {Colon, 1} -> binary:part(Text, Colon + 1, min(2, byte_size(Text) - Colon - 1)) =:= <<"//">>;
        nomatch -> binary:part(Text, 0, min(2, byte_size(Text))) =:= <<"//">>
    end.

fromIRI(Text) when is_binary(Text) ->
    try
        _ = case unicode:characters_to_list(Text) of
                Value when is_list(Value) -> Value;
                _ -> throw(invalid_unicode)
            end,
        ASCII = iri_to_uri(unicode:characters_to_nfc_binary(Text)),
        case safe_parse(ASCII) of
            {ok, _} -> {'Ok', {'URI.URI', ASCII}};
            error -> uri_error('InvalidSyntax', <<"malformed IRI reference">>)
        end
    catch _:_ -> uri_error('InvalidSyntax', <<"malformed IRI reference">>) end.

normalize(Value) -> {'URI.URI', normalized(source(Value))}.

resolve(Base, Reference) ->
    try {'Ok', {'URI.URI', unicode:characters_to_binary(uri_string:resolve(source(Reference), source(Base)))}}
    catch _:_ -> uri_error('InvalidSyntax', <<"could not resolve URI">>) end.

scheme(Value) ->
    case safe_parse(source(Value)) of
        {ok, Map} -> case maps:get(scheme, Map, undefined) of
            undefined -> 'None'; S -> {'Just', lower(S)}
        end;
        error -> 'None'
    end.

host(Value) ->
    case safe_parse(source(Value)) of
        {ok, Map} -> case maps:get(host, Map, undefined) of
            undefined -> 'None';
            <<>> -> 'None';
            H0 -> H = bracketed(H0), {'Just', {'URI.Host', H, lower(H)}}
        end;
        error -> 'None'
    end.

query(Value) ->
    case safe_parse(source(Value)) of
        {ok, Map} -> case maps:get(query, Map, undefined) of
            undefined -> 'None'; Q -> case queryParse(Q) of {'Ok', V} -> {'Just', V}; _ -> 'None' end
        end;
        error -> 'None'
    end.

queryParse(Text) -> parse_pairs(Text, false, 'URI.Query').
formParse(Text) -> parse_pairs(Text, true, 'URI.Form').
queryEncode({'URI.Query', Entries}) -> encode_pairs(Entries, false).
formEncode({'URI.Form', Entries}) -> encode_pairs(Entries, true).
formFrom(Entries) -> {'URI.Form', [{K, {'Just', V}} || {K, V} <- Entries]}.

parse_pairs(Text, Form, Type) ->
    %% An empty piece (an empty query, or "&&") is no entry, as on the walker.
    try {'Ok', {Type, [parse_pair(P, Form) || P <- binary:split(Text, <<"&">>, [global]), P =/= <<>>]}}
    catch _:_ -> uri_error('InvalidEscape', <<"invalid percent escape in query">>) end.

parse_pair(Pair, Form) ->
    case binary:split(Pair, <<"=">>) of
        [K] -> {decode(K, Form), 'None'};
        [K, V] -> {decode(K, Form), {'Just', decode(V, Form)}}
    end.

encode_pairs(Entries, Form) ->
    iolist_to_binary(lists:join(<<"&">>, [encode_pair(E, Form) || E <- Entries])).
encode_pair({K, 'None'}, Form) -> encode(K, Form);
encode_pair({K, {'Just', V}}, Form) -> [encode(K, Form), $=, encode(V, Form)].

encode(Text, Form) ->
    iolist_to_binary([encode_byte(C, Form) || <<C>> <= Text]).
encode_byte($ , true) -> <<"+">>;
encode_byte(C, _) when (C >= $a andalso C =< $z) orelse (C >= $A andalso C =< $Z) orelse
                           (C >= $0 andalso C =< $9) orelse C == $- orelse C == $. orelse C == $_ orelse C == $~ -> <<C>>;
encode_byte(C, _) -> io_lib:format("%~2.16.0B", [C]).

decode(Text, Form) ->
    Input = case Form of true -> binary:replace(Text, <<"+">>, <<" ">>, [global]); false -> Text end,
    uri_string:percent_decode(Input).

normalized(Text) ->
    try unicode:characters_to_binary(uri_string:normalize(Text)) catch _:_ -> Text end.
safe_parse(Text) -> try {ok, uri_string:parse(Text)} catch _:_ -> error end.
source({'URI.URI', Text}) -> Text; source({'URI.URL', Text}) -> Text.
redacted(Value) ->
    Text = source(Value),
    try
        Map = uri_string:parse(Text),
        case maps:find(userinfo, Map) of
            {ok, UserInfo} -> unicode:characters_to_binary(
                uri_string:recompose(Map#{userinfo => redact_userinfo(UserInfo)}));
            error -> Text
        end
    catch _:_ -> <<"<invalid URI>">> end.
redact_userinfo(UserInfo) ->
    case binary:split(UserInfo, <<":">>) of
        [User, _] -> <<User/binary, ":***">>;
        [_] -> UserInfo
    end.
lower(B) -> string:lowercase(B).
ascii(B) -> lists:all(fun(C) -> C < 128 end, binary:bin_to_list(B)).
hex(C) -> (C >= $0 andalso C =< $9) orelse (C >= $a andalso C =< $f) orelse (C >= $A andalso C =< $F).
uri_error(Kind, Message) -> {'Error', {'URI.URIError', Kind, Message, 'None'}}.
uri_error(Kind, Message, Position) ->
    {'Error', {'URI.URIError', Kind, Message, {'Just', Position}}}.

iri_to_uri(Text) ->
    case binary:match(Text, <<"://">>) of
        {SchemeEnd, 3} ->
            AuthorityStart = SchemeEnd + 3,
            Prefix = binary:part(Text, 0, AuthorityStart),
            Tail = binary:part(Text, AuthorityStart,
                               byte_size(Text) - AuthorityStart),
            {Authority, Rest} = split_authority(Tail),
            <<Prefix/binary, (ascii_authority(Authority))/binary,
              (quote_non_ascii(Rest))/binary>>;
        nomatch -> quote_non_ascii(Text)
    end.

split_authority(Text) ->
    Positions = [Position || Marker <- [<<"/">>, <<"?">>, <<"#">>],
                             {Position, 1} <- [binary:match(Text, Marker)]],
    case Positions of
        [] -> {Text, <<>>};
        _ -> Position = lists:min(Positions),
             {binary:part(Text, 0, Position),
              binary:part(Text, Position, byte_size(Text) - Position)}
    end.

ascii_authority(Authority) ->
    {UserInfo, HostPort} = case binary:matches(Authority, <<"@">>) of
        [] -> {<<>>, Authority};
        Matches -> {Position, 1} = lists:last(Matches),
                   {<<(quote_non_ascii(binary:part(Authority, 0, Position)))/binary, "@">>,
                    binary:part(Authority, Position + 1,
                                byte_size(Authority) - Position - 1)}
    end,
    {Host, Port} = split_host_port(HostPort),
    <<UserInfo/binary, (ascii_host(Host))/binary, Port/binary>>.

split_host_port(<<"[", _/binary>> = HostPort) ->
    case binary:match(HostPort, <<"]">>) of
        {Position, 1} ->
            {binary:part(HostPort, 0, Position + 1),
             binary:part(HostPort, Position + 1,
                         byte_size(HostPort) - Position - 1)};
        nomatch -> throw(invalid_host)
    end;
split_host_port(HostPort) ->
    case binary:matches(HostPort, <<":">>) of
        [] -> {HostPort, <<>>};
        [{Position, 1}] ->
            Port = binary:part(HostPort, Position + 1,
                               byte_size(HostPort) - Position - 1),
            true = Port =/= <<>> andalso
                   lists:all(fun(C) -> C >= $0 andalso C =< $9 end,
                             binary:bin_to_list(Port)),
            {binary:part(HostPort, 0, Position), <<":", Port/binary>>};
        _ -> throw(invalid_host)
    end.

ascii_host(<<"[", _/binary>> = Host) -> lower(Host);
ascii_host(Host) ->
    Labels = binary:split(
        binary:replace(binary:replace(Host, <<16#E3,16#80,16#82>>, <<".">>, [global]),
                       <<16#EF,16#BC,16#8E>>, <<".">>, [global]),
        <<".">>, [global]),
    true = Labels =/= [] andalso lists:all(fun(L) -> L =/= <<>> end, Labels),
    ASCII = iolist_to_binary(lists:join(<<".">>, [ascii_label(L) || L <- Labels])),
    true = byte_size(ASCII) =< 253,
    ASCII.

idnaHost(Host) when is_binary(Host) ->
    try {ok, ascii_host(unicode:characters_to_nfc_binary(Host))}
    catch _:_ -> error end;
idnaHost(_) -> error.

ascii_label(Label) ->
    Points = unicode:characters_to_list(Label),
    case lists:all(fun(C) -> C < 128 end, Points) of
        true ->
            Lower = string:lowercase(Label),
            true = byte_size(Lower) =< 63,
            Lower;
        false ->
            Encoded = <<"xn--", (list_to_binary(punycode(Points)))/binary>>,
            true = byte_size(Encoded) =< 63,
            Encoded
    end.

punycode(Points) ->
    Basic = [lower_codepoint(C) || C <- Points, C < 128],
    Prefix = case Basic of [] -> []; _ -> Basic ++ [$-] end,
    Prefix ++ punycode_loop(Points, 128, 0, 72, length(Basic), length(Basic)).

punycode_loop(Points, _, _, _, Handled, _) when Handled >= length(Points) -> [];
punycode_loop(Points, N, Delta, Bias, Handled, BasicCount) ->
    M = lists:min([C || C <- Points, C >= N]),
    Delta1 = Delta + (M - N) * (Handled + 1),
    {Output, Delta2, Bias2, Handled2} =
        punycode_points(Points, M, Delta1, Bias, Handled, BasicCount, []),
    Output ++ punycode_loop(Points, M + 1, Delta2 + 1, Bias2,
                            Handled2, BasicCount).

punycode_points([], _, Delta, Bias, Handled, _, Output) ->
    {lists:reverse(Output), Delta, Bias, Handled};
punycode_points([C | Rest], N, Delta0, Bias0, Handled0, BasicCount, Output0) ->
    Delta1 = case C < N of true -> Delta0 + 1; false -> Delta0 end,
    case C =:= N of
        true ->
            Digits = encode_delta(Delta1, Bias0, 36, []),
            Bias1 = adapt_bias(Delta1, Handled0 + 1,
                               Handled0 =:= BasicCount),
            punycode_points(Rest, N, 0, Bias1, Handled0 + 1,
                            BasicCount, lists:reverse(Digits) ++ Output0);
        false ->
            punycode_points(Rest, N, Delta1, Bias0, Handled0,
                            BasicCount, Output0)
    end.

encode_delta(Q, Bias, K, Acc) ->
    T = case K =< Bias of true -> 1; false ->
            case K >= Bias + 26 of true -> 26; false -> K - Bias end
        end,
    case Q < T of
        true -> lists:reverse([puny_digit(Q) | Acc]);
        false -> encode_delta((Q - T) div (36 - T), Bias, K + 36,
                              [puny_digit(T + ((Q - T) rem (36 - T))) | Acc])
    end.

adapt_bias(Delta0, Points, First) ->
    Delta1 = case First of true -> Delta0 div 700; false -> Delta0 div 2 end,
    adapt_bias_loop(Delta1 + Delta1 div Points, 0).
adapt_bias_loop(Delta, K) when Delta > 455 -> adapt_bias_loop(Delta div 35, K + 36);
adapt_bias_loop(Delta, K) -> K + (36 * Delta) div (Delta + 38).
puny_digit(D) when D < 26 -> $a + D;
puny_digit(D) -> $0 + D - 26.
lower_codepoint(C) when C >= $A, C =< $Z -> C + 32;
lower_codepoint(C) -> C.

quote_non_ascii(Text) -> iolist_to_binary([quote_iri_byte(C) || <<C>> <= Text]).
quote_iri_byte(C) when C < 16#20; C =:= 16#7f -> throw(invalid_character);
quote_iri_byte(C) when C < 128 -> <<C>>;
quote_iri_byte(C) -> io_lib:format("%~2.16.0B", [C]).

%% uri_string drops the brackets of an IP literal; the host component keeps
%% them (RFC 3986 3.2.2), as the walker reports it: `[::1]`, not `::1`.
bracketed(Host) ->
    case binary:match(Host, <<":">>) of
        nomatch -> Host;
        _ -> <<"[", Host/binary, "]">>
    end.
