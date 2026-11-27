-module(kex_intrinsic_url).
-export([parse/1, build/4, normalize/1, resolve/2, scheme/1, host/1, query/1]).

%% Mirrors the walker's URL::parse: URI.parse's checks with its kinds (an
%% empty authority, as in `file:///`, is InvalidSyntax "malformed URL"),
%% then a scheme and an authority are required.
parse(Text) ->
    case kex_intrinsic_uri:parse(Text) of
        {'Ok', _} ->
            {ok, Parts} = kex_intrinsic_uri:components(Text),
            case Parts of
                #{scheme := undefined} -> error_value('NotAbsolute', <<"URL requires a scheme">>);
                #{authority := false} ->
                    error_value('MissingAuthority', <<"URL requires an authority">>);
                _ -> {'Ok', {'URI.URL', Text}}
            end;
        {'Error', {'URI.URIError', 'InvalidSyntax', <<"malformed URI reference">>, Position}} ->
            {'Error', {'URI.URIError', 'InvalidSyntax', <<"malformed URL">>, Position}};
        Error -> Error
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
resolve({'URI.URL', Base}, Ref) ->
    case kex_intrinsic_uri:resolve({'URI.URI', Base}, Ref) of
        {'Ok', {'URI.URI', V}} -> case parse(V) of
            {'Error', {'URI.URIError', 'MissingAuthority', _, Position}} ->
                {'Error', {'URI.URIError', 'MissingAuthority',
                           <<"resolved URL has no authority">>, Position}};
            Result -> Result
        end;
        E -> E
    end.
%% The scheme is case-insensitive and reported lower-cased (RFC 3986 3.1).
scheme({'URI.URL', Text}) ->
    case kex_intrinsic_uri:components(Text) of
        {ok, #{scheme := Scheme}} when is_binary(Scheme) -> Scheme;
        _ -> <<>>
    end.
host({'URI.URL', Text}) ->
    case kex_intrinsic_uri:host({'URI.URI', Text}) of
        {'Just', Host} -> Host;
        'None' -> {'URI.Host', <<>>, <<>>}
    end.
query({'URI.URL', Text}) -> kex_intrinsic_uri:query({'URI.URI', Text}).
error_value(K, M) -> {'Error', {'URI.URIError', K, M, 'None'}}.
