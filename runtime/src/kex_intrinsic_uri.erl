-module(kex_intrinsic_uri).
-export([parse/1, fromIRI/1, normalize/1, resolve/2, scheme/1, host/1, query/1,
         queryParse/1, queryEncode/1, formFrom/1, formParse/1, formEncode/1]).

parse(Text) when is_binary(Text) ->
    case ascii(Text) andalso valid_percent(Text) of
        false -> uri_error('InvalidSyntax', <<"invalid URI text">>);
        true -> case safe_parse(Text) of
            {ok, _} -> {'Ok', {'URI.URI', Text}};
            error -> uri_error('InvalidSyntax', <<"malformed URI reference">>)
        end
    end.

fromIRI(Text) when is_binary(Text) ->
    try
        Quoted = unicode:characters_to_binary(uri_string:quote(unicode:characters_to_list(Text))),
        {'Ok', {'URI.URI', Quoted}}
    catch _:_ -> uri_error('InvalidSyntax', <<"malformed IRI reference">>) end.

normalize(Value) -> {'URI.URI', normalized(source(Value))}.

resolve(Base, Reference) ->
    try {'Ok', {'URI.URI', unicode:characters_to_binary(uri_string:resolve(source(Reference), source(Base)))}}
    catch _:_ -> uri_error('InvalidSyntax', <<"could not resolve URI">>) end.

scheme(Value) ->
    case safe_parse(source(Value)) of
        {ok, Map} -> option(maps:get(scheme, Map, undefined));
        error -> 'None'
    end.

host(Value) ->
    case safe_parse(source(Value)) of
        {ok, Map} -> case maps:get(host, Map, undefined) of
            undefined -> 'None'; H -> {'Just', {'URI.Host', H, lower(H)}}
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
    try {'Ok', {Type, [parse_pair(P, Form) || P <- binary:split(Text, <<"&">>, [global])]}}
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
option(undefined) -> 'None'; option(V) -> {'Just', V}.
lower(B) -> string:lowercase(B).
ascii(B) -> lists:all(fun(C) -> C < 128 end, binary:bin_to_list(B)).
valid_percent(<<>>) -> true;
valid_percent(<<$%, A, B, Rest/binary>>) -> hex(A) andalso hex(B) andalso valid_percent(Rest);
valid_percent(<<$%, _/binary>>) -> false;
valid_percent(<<_, Rest/binary>>) -> valid_percent(Rest).
hex(C) -> (C >= $0 andalso C =< $9) orelse (C >= $a andalso C =< $f) orelse (C >= $A andalso C =< $F).
uri_error(Kind, Message) -> {'Error', {'URI.URIError', Kind, Message, 'None'}}.
