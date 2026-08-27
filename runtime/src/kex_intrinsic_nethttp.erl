-module(kex_intrinsic_nethttp).
-export([headers/1, parseHeaders/1, addHeader/3, setHeader/3, removeHeader/2,
         getHeader/2, getAllHeaders/2, status/1, get/1, request/4,
         responseBinary/3, responseText/2, responseEmpty/1]).

headers(Entries) -> case lists:all(fun valid/1, Entries) of true -> {'Ok', {'Net.HTTP.Headers', Entries}}; false -> net_error(<<"invalid HTTP header">>) end.
parseHeaders(Text) ->
    Lines = [L || L <- binary:split(Text, <<"\n">>, [global]), L =/= <<>>, L =/= <<"\r">>],
    try headers([parse_line(L) || L <- Lines]) catch _:_ -> net_error(<<"invalid HTTP header">>) end.
addHeader({'Net.HTTP.Headers', E}, N, V) -> case valid({N,V}) of true -> {'Net.HTTP.Headers', E ++ [{N,V}]}; false -> {'Net.HTTP.Headers', E} end.
setHeader({'Net.HTTP.Headers', E}, N, V) -> addHeader({'Net.HTTP.Headers', [P || P={K,_} <- E, lower(K) =/= lower(N)]}, N, V).
removeHeader({'Net.HTTP.Headers', E}, N) -> {'Net.HTTP.Headers', [P || P={K,_} <- E, lower(K) =/= lower(N)]}.
getHeader(H, N) -> case getAllHeaders(H, N) of [] -> 'None'; [V|_] -> {'Just', V} end.
getAllHeaders({'Net.HTTP.Headers', E}, N) -> [V || {K,V} <- E, lower(K) == lower(N)].
status(C) when is_integer(C), C >= 100, C =< 599 -> {'Ok', {'Net.HTTP.Status', C}};
status(_) -> net_error(<<"HTTP status must be between 100 and 599">>).
responseBinary(Code, Body = {'Binary', _}, Headers = {'Net.HTTP.Headers', _}) ->
    {'Net.HTTP.Response', {'Net.HTTP.Status', Code}, Headers, Body}.
responseText(Code, Text) ->
    {'Net.HTTP.Response', {'Net.HTTP.Status', Code},
     {'Net.HTTP.Headers', [{<<"Content-Type">>, <<"text/plain; charset=utf-8">>}]},
     {'Binary', Text}}.
responseEmpty(Code) ->
    {'Net.HTTP.Response', {'Net.HTTP.Status', Code}, {'Net.HTTP.Headers', []}, {'Binary', <<>>}}.
get(URL) -> request(<<"GET">>, URL, {'Net.HTTP.Headers', []}, {'Binary', <<>>}).

request(MethodText, URL, {'Net.HTTP.Headers', RequestHeaders}, {'Binary', RequestBody})
  when is_binary(MethodText), is_binary(URL) ->
    inets:start(),
    ssl:start(),
    case method(MethodText) of
      error -> net_error(<<"unsupported HTTP method">>);
      Method ->
    HTTPOptions = [{timeout, 30000}, {connect_timeout, 10000},
                   {autoredirect, false},
                   {ssl, [{verify, verify_peer},
                          {cacerts, public_key:cacerts_get()},
                          {depth, 10}]}],
    Headers0 = [{binary_to_list(Name), binary_to_list(Value)} || {Name, Value} <- RequestHeaders],
    Request = case Method of
                  get -> {binary_to_list(URL), Headers0};
                  head -> {binary_to_list(URL), Headers0};
                  _ -> {binary_to_list(URL), Headers0, "application/octet-stream", RequestBody}
              end,
    case httpc:request(Method, Request, HTTPOptions,
                       [{body_format, binary}]) of
        {ok, {{_Version, Code, _Reason}, ResponseHeaders, Body}} ->
            Headers = [{unicode:characters_to_binary(Name),
                        unicode:characters_to_binary(Value)}
                       || {Name, Value} <- ResponseHeaders],
            {'Ok', {'Net.HTTP.Response', {'Net.HTTP.Status', Code},
                    {'Net.HTTP.Headers', Headers}, {'Binary', Body}}};
        {error, Reason} ->
            Kind = case Reason of
                       timeout -> 'Timeout';
                       {failed_connect, _} -> 'Connect';
                       {tls_alert, _} -> 'Protocol';
                       _ -> 'Backend'
                   end,
            {'Error', {'Net.NetError', Kind, 'HTTPClient', diagnostic(Reason),
                       'None', 'None'}}
    end
    end;
request(_, _, _, _) -> net_error(<<"invalid HTTP request values">>).

parse_line(Line0) ->
    Line = binary:replace(Line0, <<"\r">>, <<>>),
    [N,V0] = binary:split(Line, <<":">>),
    {N, string:trim(V0)}.
valid({N,V}) -> byte_size(N) > 0 andalso binary:match(N, <<"\r">>) == nomatch andalso binary:match(N, <<"\n">>) == nomatch andalso binary:match(V, <<"\r">>) == nomatch andalso binary:match(V, <<"\n">>) == nomatch.
lower(B) -> string:lowercase(B).
net_error(M) -> {'Error', {'Net.NetError', 'Parse', 'HTTPClient', M, 'None', 'None'}}.
diagnostic(Value) -> unicode:characters_to_binary(io_lib:format("~p", [Value])).
method(<<"GET">>) -> get;
method(<<"POST">>) -> post;
method(<<"PUT">>) -> put;
method(<<"PATCH">>) -> patch;
method(<<"DELETE">>) -> delete;
method(<<"HEAD">>) -> head;
method(<<"OPTIONS">>) -> options;
method(_) -> error.
