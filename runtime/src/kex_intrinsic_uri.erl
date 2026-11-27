-module(kex_intrinsic_uri).
-export([parse/1, fromIRI/1, normalize/1, resolve/2, scheme/1, host/1, query/1,
         components/1, queryParse/1, queryEncode/1, formFrom/1, formParse/1, formEncode/1,
         idnaHost/1, redacted/1]).

%% The same checks, in the same order and with the same kinds and positions,
%% as the walker's URI::parse (src/interpreter/stdlib/net.cxx).
parse(Text) when is_binary(Text) ->
    case check_text(Text) of
        ok -> case components(Text) of
            {ok, _} -> {'Ok', {'URI.URI', Text}};
            {error, Kind, Message} -> uri_error(Kind, Message)
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

%% The parts of a URI reference, read as the walker's parseURI reads them,
%% check for check and kind for kind, so both backends accept the same texts
%% and report the same parts: {ok, #{scheme, authority, host, port, query}},
%% or {error, Kind, Message}. uri_string:parse is not used for this: it
%% accepts a space, an empty authority and any port, rejects IP literals the
%% walker accepts (`[fe80::1%25eth0]`, `[v1.fe]`), and reports a rejection by
%% returning {error, _, _} rather than by raising.
components(Text) ->
    case ascii(Text) andalso check_bytes(Text, 0) =:= ok of
        false -> {error, 'InvalidSyntax', <<"malformed URI reference">>};
        true ->
            try {ok, read_components(Text)}
            catch throw:{uri_rejected, Kind, Message} -> {error, Kind, Message}
            end
    end.

read_components(Text) ->
    Size = byte_size(Text),
    FragmentAt = find(Text, $#, 0),
    {End, Query} = case find(Text, $?, 0) of
        At when At < FragmentAt -> {At, binary:part(Text, At + 1, FragmentAt - At - 1)};
        _ -> {FragmentAt, undefined}
    end,
    Colon = find(Text, $:, 0),
    {Scheme, Start} = case Colon < End andalso Colon < find(Text, $/, 0) of
        true -> {lower(scheme_of(binary:part(Text, 0, Colon))), Colon + 1};
        false -> {undefined, 0}
    end,
    Parts = #{scheme => Scheme, authority => false, host => undefined,
              port => undefined, query => Query},
    case binary:part(Text, Start, min(2, Size - Start)) of
        <<"//">> ->
            AuthorityEnd = min(find(Text, $/, Start + 2), End),
            Authority = binary:part(Text, Start + 2, AuthorityEnd - Start - 2),
            Authority =/= <<>> orelse reject_syntax(),
            {Host, Port} = host_port(Authority),
            Parts#{authority => true, host => Host, port => Port};
        _ -> Parts
    end.

%% The first position of Char at or after From, or the size when there is none.
find(Text, Char, From) ->
    Size = byte_size(Text),
    case From =< Size andalso binary:match(Text, <<Char>>, [{scope, {From, Size - From}}]) of
        {At, 1} -> At;
        _ -> Size
    end.

scheme_of(<<First, Rest/binary>> = Scheme) ->
    (alpha(First) andalso lists:all(fun scheme_char/1, binary_to_list(Rest)))
        orelse reject_syntax(),
    Scheme;
scheme_of(<<>>) -> reject_syntax().

scheme_char(C) -> alpha(C) orelse (C >= $0 andalso C =< $9) orelse C =:= $+ orelse
                  C =:= $- orelse C =:= $..
alpha(C) -> (C >= $a andalso C =< $z) orelse (C >= $A andalso C =< $Z).

%% An authority names a host, an IP literal keeps its brackets, and a port
%% is a number from 0 to 65535.
host_port(Authority) ->
    HostPort = case binary:matches(Authority, <<"@">>) of
        [] -> Authority;
        Ats -> {At, 1} = lists:last(Ats),
               binary:part(Authority, At + 1, byte_size(Authority) - At - 1)
    end,
    {Host, Port} = split_port(HostPort),
    Host =/= <<>> orelse reject_syntax(),
    case Port of
        <<>> ->
            binary:last(HostPort) =/= $: orelse reject_port(),
            {Host, undefined};
        _ ->
            lists:all(fun(C) -> C >= $0 andalso C =< $9 end, binary_to_list(Port))
                orelse reject_port(),
            binary_to_integer(Port) =< 65535 orelse reject_port(),
            {Host, binary_to_integer(Port)}
    end.

split_port(<<"[", _/binary>> = HostPort) ->
    case binary:match(HostPort, <<"]">>) of
        nomatch -> reject_authority();
        {Close, 1} ->
            Host = binary:part(HostPort, 0, Close + 1),
            case binary:part(HostPort, Close + 1, byte_size(HostPort) - Close - 1) of
                <<>> -> {Host, <<>>};
                <<$:, Port/binary>> -> {Host, Port};
                _ -> reject_authority()
            end
    end;
split_port(HostPort) ->
    {Host, Port} = case binary:matches(HostPort, <<":">>) of
        [] -> {HostPort, <<>>};
        Colons -> {At, 1} = lists:last(Colons),
                  {binary:part(HostPort, 0, At),
                   binary:part(HostPort, At + 1, byte_size(HostPort) - At - 1)}
    end,
    %% Only an IP literal in brackets may hold a colon: `http://::1/` is no host.
    binary:match(Host, <<":">>) =:= nomatch orelse reject_authority(),
    {Host, Port}.

reject_syntax() -> throw({uri_rejected, 'InvalidSyntax', <<"malformed URI reference">>}).
reject_authority() -> throw({uri_rejected, 'InvalidAuthority', <<"invalid URI authority">>}).
reject_port() ->
    throw({uri_rejected, 'InvalidPort', <<"URI port must be a number from 0 to 65535">>}).

fromIRI(Text) when is_binary(Text) ->
    try
        _ = case unicode:characters_to_list(Text) of
                Value when is_list(Value) -> Value;
                _ -> throw(invalid_unicode)
            end,
        ASCII = iri_to_uri(unicode:characters_to_nfc_binary(Text)),
        case components(ASCII) of
            {ok, _} -> {'Ok', {'URI.URI', ASCII}};
            {error, _, _} -> uri_error('InvalidSyntax', <<"malformed IRI reference">>)
        end
    catch _:_ -> uri_error('InvalidSyntax', <<"malformed IRI reference">>) end.

normalize(Value) -> {'URI.URI', normalized(source(Value))}.

%% As on the walker, the base must have a scheme.
resolve(Base, Reference) ->
    case components(source(Base)) of
        {ok, #{scheme := Scheme}} when Scheme =/= undefined ->
            try {'Ok', {'URI.URI', unicode:characters_to_binary(
                                       uri_string:resolve(source(Reference), source(Base)))}}
            catch _:_ -> uri_error('InvalidSyntax', <<"could not resolve URI">>) end;
        _ -> uri_error('NotAbsolute', <<"base URI must be absolute">>)
    end.

scheme(Value) ->
    case components(source(Value)) of
        {ok, #{scheme := Scheme}} when is_binary(Scheme) -> {'Just', Scheme};
        _ -> 'None'
    end.

host(Value) ->
    case components(source(Value)) of
        {ok, #{host := Host}} when is_binary(Host) -> {'Just', {'URI.Host', Host, lower(Host)}};
        _ -> 'None'
    end.

query(Value) ->
    case components(source(Value)) of
        {ok, #{query := Q}} when is_binary(Q) ->
            case queryParse(Q) of {'Ok', V} -> {'Just', V}; _ -> 'None' end;
        _ -> 'None'
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
source({'URI.URI', Text}) -> Text; source({'URI.URL', Text}) -> Text.
%% The walker's redactedURI: the password, from the authority's first `:` to
%% its last `@`, is written as `***`. Read from the text, not uri_string, so a
%% URI uri_string rejects still prints.
redacted(Value) ->
    Text = source(Value),
    Size = byte_size(Text),
    case binary:match(Text, <<"//">>) of
        nomatch -> Text;
        {Slashes, 2} ->
            Start = Slashes + 2,
            End = case binary:match(Text, [<<"/">>, <<"?">>, <<"#">>],
                                    [{scope, {Start, Size - Start}}]) of
                {Found, 1} -> Found;
                nomatch -> Size
            end,
            Authority = binary:part(Text, Start, End - Start),
            case {binary:match(Authority, <<":">>), binary:matches(Authority, <<"@">>)} of
                {{Colon, 1}, [_ | _] = Ats} ->
                    case lists:last(Ats) of
                        {At, 1} when Colon < At ->
                            <<(binary:part(Text, 0, Start + Colon + 1))/binary, "***",
                              (binary:part(Text, Start + At, Size - Start - At))/binary>>;
                        _ -> Text
                    end;
                _ -> Text
            end
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
