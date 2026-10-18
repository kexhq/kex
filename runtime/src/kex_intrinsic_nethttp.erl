-module(kex_intrinsic_nethttp).
-export([headers/1, parseHeaders/1, addHeader/3, removeHeader/2,
         getHeader/2, getAllHeaders/2, status/1, get/1, request/4, request/5,
         responseBinary/3, responseText/2, responseEmpty/1, redactedHeaders/1]).

headers(Entries) ->
    case lists:all(fun valid/1, Entries) of
        true -> {'Ok', {'Net.HTTP.Headers', Entries}};
        false -> net_error(<<"invalid HTTP header">>)
    end.
parseHeaders(Text) ->
    Lines = [L || L <- binary:split(Text, <<"\n">>, [global]), L =/= <<>>, L =/= <<"\r">>],
    try headers([parse_line(L) || L <- Lines]) catch _:_ -> net_error(<<"invalid HTTP header">>) end.
addHeader({'Net.HTTP.Headers', E}, N, V) ->
    case valid({N,V}) of
        true -> {'Ok', {'Net.HTTP.Headers', E ++ [{N,V}]}};
        false -> net_error(<<"invalid HTTP header">>)
    end.
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
redactedHeaders({'Net.HTTP.Headers', Entries}) ->
    Rendered = [[Name, <<": ">>, redact_header(Name, Value)] || {Name, Value} <- Entries],
    iolist_to_binary([<<"Headers { ">>, lists:join(<<", ">>, Rendered), <<" }">>]).
redact_header(Name, Value) ->
    case lists:member(lower(Name), [<<"authorization">>, <<"proxy-authorization">>,
                                    <<"cookie">>, <<"set-cookie">>]) of
        true -> <<"***">>;
        false -> Value
    end.
get(URL) -> request(<<"GET">>, URL, {'Net.HTTP.Headers', []}, {'Binary', <<>>}).

request(MethodText, URL, Headers, Body) ->
    request(MethodText, URL, Headers, Body, default_options()).

%% Today's limits, the defaults `Net.HTTP.RequestOptions` declares.
default_options() ->
    {'Net.HTTP.RequestOptions', {'Duration', 10.0}, {'Duration', 30.0}, 16777216, 65536}.

request(MethodText, URL, {'Net.HTTP.Headers', RequestHeaders}, {'Binary', RequestBody}, Options)
  when is_binary(MethodText), is_binary(URL) ->
    inets:start(),
    ssl:start(),
    case {method(MethodText), kex_intrinsic_nethttpclient:request_limits(Options)} of
      {error, _} -> net_error(<<"unsupported HTTP method">>);
      {_, error} -> net_error(<<"invalid HTTP request options">>);
      {Method, {ok, #{connect := Connect, timeout := Timeout, body := BodyLimit}}} ->
    HTTPOptions = [{timeout, Timeout}, {connect_timeout, min(Connect, Timeout)},
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
    %% Streamed, so a body over the limit is refused as it arrives rather
    %% than read whole into memory first (kexhq/kex#484).
    Deadline = erlang:monotonic_time(millisecond) + Timeout,
    case httpc:request(Method, Request, HTTPOptions,
                       [{body_format, binary}, {sync, false}, {stream, self}]) of
        {ok, RequestId} -> await(RequestId, Deadline, BodyLimit);
        {error, Reason} -> client_error(Reason)
    end
    end;
request(_, _, _, _, _) -> net_error(<<"invalid HTTP request values">>).

%% httpc streams only a 200 or 206 answer; any other arrives whole.
await(RequestId, Deadline, BodyLimit) ->
    receive
        {http, {RequestId, {{_Version, Code, _Reason}, ResponseHeaders, Body}}} ->
            case byte_size(Body) =< BodyLimit of
                true -> response(Code, ResponseHeaders, Body);
                false -> body_limit()
            end;
        {http, {RequestId, stream_start, ResponseHeaders}} ->
            Code = case lists:keymember("content-range", 1, ResponseHeaders) of
                       true -> 206;
                       false -> 200
                   end,
            stream(RequestId, Deadline, BodyLimit, Code, ResponseHeaders, [], 0);
        {http, {RequestId, {error, Reason}}} -> client_error(Reason)
    after remaining(Deadline) ->
        httpc:cancel_request(RequestId),
        client_error(timeout)
    end.

stream(RequestId, Deadline, BodyLimit, Code, ResponseHeaders, Parts, Bytes) ->
    receive
        {http, {RequestId, stream, Part}} ->
            Next = [Part | Parts],
            NextBytes = Bytes + iolist_size(Part),
            case NextBytes =< BodyLimit of
                true -> stream(RequestId, Deadline, BodyLimit, Code, ResponseHeaders, Next, NextBytes);
                false -> httpc:cancel_request(RequestId), body_limit()
            end;
        {http, {RequestId, stream_end, _Trailers}} ->
            response(Code, ResponseHeaders, iolist_to_binary(lists:reverse(Parts)));
        {http, {RequestId, {error, Reason}}} -> client_error(Reason)
    after remaining(Deadline) ->
        httpc:cancel_request(RequestId),
        client_error(timeout)
    end.

response(Code, ResponseHeaders, Body) ->
    Headers = [{unicode:characters_to_binary(Name),
                unicode:characters_to_binary(Value)}
               || {Name, Value} <- ResponseHeaders],
    {'Ok', {'Net.HTTP.Response', {'Net.HTTP.Status', Code},
            {'Net.HTTP.Headers', Headers}, {'Binary', Body}}}.

client_error(Reason) ->
    Kind = case Reason of
               timeout -> 'Timeout';
               {failed_connect, _} -> 'Connect';
               {tls_alert, _} -> 'Protocol';
               _ -> 'Backend'
           end,
    {'Error', {'Net.NetError', Kind, 'HTTPClient', diagnostic(Reason), 'None', 'None'}}.

body_limit() ->
    {'Error', {'Net.NetError', 'Limit', 'HTTPClient', <<"HTTP response body exceeds limit">>,
               'None', 'None'}}.

remaining(Deadline) -> max(0, Deadline - erlang:monotonic_time(millisecond)).

parse_line(Line0) ->
    Line = binary:replace(Line0, <<"\r">>, <<>>),
    [N,V0] = binary:split(Line, <<":">>),
    {N, string:trim(V0)}.
valid({N,V}) when is_binary(N), is_binary(V), byte_size(N) > 0 ->
    lists:all(fun token_char/1, binary:bin_to_list(N)) andalso
    lists:all(fun(C) -> C =:= 9 orelse C >= 32 andalso C =/= 127 end,
              binary:bin_to_list(V));
valid(_) -> false.
token_char(C) -> C > 32 andalso C < 127 andalso
                 not lists:member(C, "()<>@,;:\\\"/[]?={} ").
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
