-module(kex_intrinsic_url).
-export([parse/1, build/4, normalize/1, resolve/2, scheme/1, host/1, query/1]).

parse(Text) ->
    case kex_intrinsic_uri:parse(Text) of
        {'Ok', _} -> try
            Map = uri_string:parse(Text),
            case {maps:get(scheme, Map, undefined), maps:get(host, Map, undefined)} of
                {undefined, _} -> error_value('NotAbsolute', <<"URL requires a scheme">>);
                {_, undefined} -> error_value('MissingAuthority', <<"URL requires an authority">>);
                _ -> {'Ok', {'URI.URL', Text}}
            end
        catch _:_ -> error_value('InvalidSyntax', <<"malformed URL">>) end;
        Error -> Error
    end.

build(Scheme, Host, Path, Query) ->
    Segments = iolist_to_binary([[<<"/">>, uri_string:quote(S)] || S <- Path]),
    Q = kex_intrinsic_uri:queryEncode(Query),
    Tail = case Q of <<>> -> <<>>; _ -> <<"?", Q/binary>> end,
    parse(<<Scheme/binary, "://", Host/binary, Segments/binary, Tail/binary>>).

normalize({'URI.URL', Text}) -> case kex_intrinsic_uri:normalize({'URI.URI', Text}) of {'URI.URI', N} -> {'URI.URL', N} end.
resolve({'URI.URL', Base}, Ref) -> case kex_intrinsic_uri:resolve({'URI.URI', Base}, Ref) of {'Ok', {'URI.URI', V}} -> parse(V); E -> E end.
scheme({'URI.URL', Text}) -> maps:get(scheme, uri_string:parse(Text)).
host({'URI.URL', Text}) -> H = maps:get(host, uri_string:parse(Text)), {'URI.Host', H, string:lowercase(H)}.
query({'URI.URL', Text}) -> kex_intrinsic_uri:query({'URI.URI', Text}).
error_value(K, M) -> {'Error', {'URI.URIError', K, M, 'None'}}.
