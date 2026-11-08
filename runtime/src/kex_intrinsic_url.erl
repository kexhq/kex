-module(kex_intrinsic_url).
-export([parse/1, build/4, normalize/1, resolve/2, scheme/1, host/1, query/1]).

%% Mirrors the walker's URL::parse: any failure to read the text is
%% InvalidSyntax "malformed URL" (an empty authority included, as in
%% `file:///`), then a scheme and an authority are required.
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
        _ -> error_value('InvalidSyntax', <<"malformed URL">>)
    end.

%% As the walker builds it: scheme and host lower-cased, and both required.
build(Scheme, Host, _Path, _Query) when Scheme =:= <<>>; Host =:= <<>> ->
    error_value('MissingAuthority', <<"URL requires scheme and host">>);
build(Scheme, Host, Path, Query) ->
    Segments = iolist_to_binary([[<<"/">>, uri_string:quote(S)] || S <- Path]),
    Q = kex_intrinsic_uri:queryEncode(Query),
    Tail = case Q of <<>> -> <<>>; _ -> <<"?", Q/binary>> end,
    parse(<<(string:lowercase(Scheme))/binary, "://", (string:lowercase(Host))/binary,
            Segments/binary, Tail/binary>>).

normalize({'URI.URL', Text}) -> case kex_intrinsic_uri:normalize({'URI.URI', Text}) of {'URI.URI', N} -> {'URI.URL', N} end.
resolve({'URI.URL', Base}, Ref) -> case kex_intrinsic_uri:resolve({'URI.URI', Base}, Ref) of {'Ok', {'URI.URI', V}} -> parse(V); E -> E end.
%% The scheme is case-insensitive and reported lower-cased (RFC 3986 3.1).
scheme({'URI.URL', Text}) -> string:lowercase(maps:get(scheme, uri_string:parse(Text))).
host({'URI.URL', Text}) ->
    H = kex_intrinsic_uri:bracketed(maps:get(host, uri_string:parse(Text))),
    {'URI.Host', H, string:lowercase(H)}.
query({'URI.URL', Text}) -> kex_intrinsic_uri:query({'URI.URI', Text}).
error_value(K, M) -> {'Error', {'URI.URIError', K, M, 'None'}}.
