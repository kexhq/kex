-module(kex_intrinsic_nethttpclient).
-export([open/1, request/5, get/2, post/3, statistics/1, close/1]).

-define(HEADER_LIMIT, 65536).
-define(BODY_LIMIT, 16777216).
-define(TIMEOUT, 30000).

open({'Net.HTTP.ClientOptions', {'Net.HTTP.PoolOptions', PerOrigin, Total, Queued, Idle}})
  when is_integer(PerOrigin), PerOrigin > 0, is_integer(Total), Total > 0,
       is_integer(Queued), Queued >= 0, is_integer(Idle), Idle >= 0 ->
    Pid = spawn(fun() -> loop(#{}, Total, Idle, 0, 0) end),
    {'Ok', {'Net.HTTP.Client', Pid}};
open(_) -> error_value('Parse', <<"invalid HTTP client options">>).

get(Client, URL) -> request(Client, <<"GET">>, URL, {'Net.HTTP.Headers', []}, {'Binary', <<>>}).
post(Client, URL, Body) -> request(Client, <<"POST">>, URL, {'Net.HTTP.Headers', []}, Body).

request({'Net.HTTP.Client', Pid}, Method, URL, Headers, Body) ->
    call(Pid, {request, Method, URL, Headers, Body}, ?TIMEOUT + 11000).
statistics({'Net.HTTP.Client', Pid}) ->
    call(Pid, statistics, 1000).
close({'Net.HTTP.Client', Pid}) ->
    call(Pid, close, 2000).

loop(Pool0, Total, Idle, Requests, Reused) ->
    Pool = expire(Pool0, Idle),
    receive
        {call, From, Ref, {request, Method, URL, Headers, Body}} ->
            {Reply, NextPool, WasReused} = perform(Method, URL, Headers, Body, Pool, Total),
            From ! {Ref, Reply},
            loop(NextPool, Total, Idle, Requests + 1, Reused + bool_int(WasReused));
        {call, From, Ref, statistics} ->
            From ! {Ref, {'Net.HTTP.ClientStatistics', maps:size(Pool), Requests, Reused}},
            loop(Pool, Total, Idle, Requests, Reused);
        {call, From, Ref, close} ->
            maps:foreach(fun(_, {Transport, _}) -> transport_close(Transport) end, Pool),
            From ! {Ref, {'Ok', {'Net.HTTP.ClientCloseReport', maps:size(Pool)}}};
        _ -> loop(Pool, Total, Idle, Requests, Reused)
    end.

perform(Method, URL, {'Net.HTTP.Headers', Headers}, {'Binary', Body}, Pool, Total)
  when is_binary(Method), is_binary(URL), is_list(Headers), is_binary(Body) ->
    case parse_url(URL) of
        {ok, Origin, Target, HostHeader} ->
            case maps:take(Origin, Pool) of
                {{Transport, _}, Rest} ->
                    case exchange(Transport, Method, Target, HostHeader, Headers, Body) of
                        {ok, Response, reusable} -> {Response, put_pool(Origin, Transport, Rest, Total), true};
                        {ok, Response, close} -> transport_close(Transport), {Response, Rest, true};
                        {error, Error} -> transport_close(Transport), {Error, Rest, true}
                    end;
                error ->
                    case connect(Origin) of
                        {ok, Transport} ->
                            case exchange(Transport, Method, Target, HostHeader, Headers, Body) of
                                {ok, Response, reusable} -> {Response, put_pool(Origin, Transport, Pool, Total), false};
                                {ok, Response, close} -> transport_close(Transport), {Response, Pool, false};
                                {error, Error} -> transport_close(Transport), {Error, Pool, false}
                            end;
                        {error, Error} -> {Error, Pool, false}
                    end
            end;
        {error, Error} -> {Error, Pool, false}
    end;
perform(_, _, _, _, Pool, _) -> {error_value('Parse', <<"invalid HTTP request values">>), Pool, false}.

parse_url(URL) ->
    try
        Parts = uri_string:parse(URL),
        Scheme = maps:get(scheme, Parts), Host = maps:get(host, Parts),
        Port = maps:get(port, Parts, default_port(Scheme)),
        true = (Scheme =:= <<"http">> orelse Scheme =:= <<"https">>),
        Path = case maps:get(path, Parts, <<>>) of <<>> -> <<"/">>; P -> P end,
        Target = case maps:find(query, Parts) of {ok, Q} -> <<Path/binary, "?", Q/binary>>; error -> Path end,
        HostHeader = case Port =:= default_port(Scheme) of
                         true -> Host;
                         false -> <<Host/binary, ":", (integer_to_binary(Port))/binary>>
                     end,
        {ok, {Scheme, Host, Port}, Target, HostHeader}
    catch _:_ -> {error, error_value('Parse', <<"HTTP requires an absolute http or https URL">>)} end.
default_port(<<"http">>) -> 80; default_port(<<"https">>) -> 443.

connect({<<"http">>, Host, Port}) ->
    case gen_tcp:connect(binary_to_list(Host), Port, [binary, {active, false}, {packet, raw}, {nodelay, true}], 10000) of
        {ok, Socket} -> {ok, {tcp, Socket}};
        {error, Reason} -> {error, native_error('Connect', Reason)}
    end;
connect({<<"https">>, Host, Port}) ->
    ssl:start(),
    Options = [binary, {active, false}, {server_name_indication, binary_to_list(Host)},
               {verify, verify_peer}, {cacerts, public_key:cacerts_get()},
               {versions, ['tlsv1.2', 'tlsv1.3']}, {alpn_advertised_protocols, [<<"http/1.1">>]}],
    case ssl:connect(binary_to_list(Host), Port, Options, 10000) of
        {ok, Socket} -> {ok, {tls, Socket}};
        {error, Reason} -> {error, native_error('Connect', Reason)}
    end.

exchange(Transport, Method, Target, Host, Headers0, Body) ->
    Headers1 = remove_managed(Headers0),
    Headers = [{<<"Host">>, Host}, {<<"Connection">>, <<"keep-alive">>} |
               case byte_size(Body) of 0 -> Headers1; N -> [{<<"Content-Length">>, integer_to_binary(N)} | Headers1] end],
    Request = [Method, <<" ">>, Target, <<" HTTP/1.1\r\n">>,
               [[Name, <<": ">>, Value, <<"\r\n">>] || {Name, Value} <- Headers], <<"\r\n">>, Body],
    case transport_send(Transport, Request) of
        ok -> read_response(Transport, Method);
        {error, Reason} -> {error, native_error('Connect', Reason)}
    end.

read_response(Transport, Method) ->
    case recv_until(Transport, <<>>, <<"\r\n\r\n">>, ?HEADER_LIMIT) of
        {ok, Block, Buffered} ->
            case parse_response_head(Block) of
                {ok, Version, Code, Headers} ->
                    case response_body(Transport, Method, Code, Headers, Buffered) of
                        {ok, Body, Framing} ->
                            ConnectionClose = lists:any(fun(V) -> lower(V) =:= <<"close">> end,
                                                        values(<<"connection">>, Headers)),
                            Reuse = case Version =:= <<"HTTP/1.1">> andalso not ConnectionClose andalso Framing =/= eof of true -> reusable; false -> close end,
                            {ok, {'Ok', {'Net.HTTP.Response', {'Net.HTTP.Status', Code},
                                        {'Net.HTTP.Headers', Headers}, {'Binary', Body}}}, Reuse};
                        {error, Error} -> {error, Error}
                    end;
                {error, Error} -> {error, Error}
            end;
        {error, Reason} -> {error, native_error('Protocol', Reason)}
    end.

parse_response_head(Block) ->
    case binary:split(Block, <<"\r\n">>, [global]) of
        [StatusLine | Lines] ->
            case binary:split(StatusLine, <<" ">>, [global, trim_all]) of
                [Version, CodeText | _] ->
                    try {ok, Version, binary_to_integer(CodeText), parse_headers(Lines, [])}
                    catch _:_ -> {error, error_value('Protocol', <<"invalid HTTP status line">>)} end;
                _ -> {error, error_value('Protocol', <<"invalid HTTP status line">>)}
            end;
        _ -> {error, error_value('Protocol', <<"missing HTTP status line">>)}
    end.
parse_headers([], Acc) -> lists:reverse(Acc);
parse_headers([Line | Rest], Acc) ->
    case binary:split(Line, <<":">>) of
        [Name, Value] when Name =/= <<>> -> parse_headers(Rest, [{Name, string:trim(Value)} | Acc]);
        _ -> throw(invalid_header)
    end.

response_body(_, <<"HEAD">>, _, _, _) -> {ok, <<>>, fixed};
response_body(_, _, Code, _, _) when Code =:= 204; Code =:= 304 -> {ok, <<>>, fixed};
response_body(Transport, _, _, Headers, Buffered) ->
    case {values(<<"transfer-encoding">>, Headers), lists:usort(values(<<"content-length">>, Headers))} of
        {[Transfer], []} ->
            case lower(Transfer) of <<"chunked">> -> read_chunks(Transport, Buffered, <<>>); _ -> {error, error_value('Protocol', <<"unsupported transfer encoding">>)} end;
        {[], [LengthText]} ->
            try
                Length = binary_to_integer(LengthText),
                case Length >= 0 andalso Length =< ?BODY_LIMIT of
                    true -> case ensure(Transport, Buffered, Length) of {ok, Data, _} -> {ok, Data, fixed}; {error, R} -> {error, native_error('Protocol', R)} end;
                    false -> {error, error_value('Limit', <<"HTTP response body exceeds limit">>)}
                end
            catch _:_ -> {error, error_value('Protocol', <<"invalid Content-Length">>)} end;
        {[], []} -> read_eof(Transport, Buffered);
        _ -> {error, error_value('Protocol', <<"ambiguous HTTP response framing">>)}
    end.

read_chunks(Transport, Buffer, Acc) when byte_size(Acc) =< ?BODY_LIMIT ->
    case read_line(Transport, Buffer) of
        {ok, Line, Rest} ->
            SizeText = hd(binary:split(Line, <<";">>)),
            try list_to_integer(binary_to_list(SizeText), 16) of
                0 -> consume_trailers(Transport, Rest, Acc);
                Size when byte_size(Acc) + Size =< ?BODY_LIMIT ->
                    case ensure(Transport, Rest, Size + 2) of
                        {ok, DataAndCRLF, Tail} ->
                            <<Data:Size/binary, "\r\n">> = DataAndCRLF,
                            read_chunks(Transport, Tail, <<Acc/binary, Data/binary>>);
                        {error, R} -> {error, native_error('Protocol', R)}
                    end;
                _ -> {error, error_value('Limit', <<"HTTP response body exceeds limit">>)}
            catch _:_ -> {error, error_value('Protocol', <<"invalid chunk framing">>)} end;
        {error, R} -> {error, native_error('Protocol', R)}
    end;
read_chunks(_, _, _) -> {error, error_value('Limit', <<"HTTP response body exceeds limit">>)}.

consume_trailers(_, <<"\r\n">>, Acc) -> {ok, Acc, chunked};
consume_trailers(_, <<"\r\n", _/binary>>, _) ->
    {error, error_value('Protocol', <<"unexpected bytes after chunked response">>)};
consume_trailers(Transport, Buffer, Acc) ->
    case recv_until(Transport, Buffer, <<"\r\n\r\n">>, ?HEADER_LIMIT) of
        {ok, TrailerBlock, <<>>} ->
            try parse_headers(binary:split(TrailerBlock, <<"\r\n">>, [global]), []) of
                Trailers ->
                    case values(<<"content-length">>, Trailers) ++
                         values(<<"transfer-encoding">>, Trailers) of
                        [] -> {ok, Acc, chunked};
                        _ -> {error, error_value('Protocol', <<"framing fields are forbidden in trailers">>)}
                    end
            catch _:_ -> {error, error_value('Protocol', <<"invalid HTTP trailers">>)} end;
        {ok, _, _} -> {error, error_value('Protocol', <<"unexpected bytes after chunked response">>)};
        {error, Reason} -> {error, native_error('Protocol', Reason)}
    end.

read_line(Transport, Buffer) ->
    case binary:match(Buffer, <<"\r\n">>) of
        {Position, 2} -> {ok, binary:part(Buffer, 0, Position), binary:part(Buffer, Position + 2, byte_size(Buffer) - Position - 2)};
        nomatch -> case transport_recv(Transport, 0) of {ok, Data} -> read_line(Transport, <<Buffer/binary, Data/binary>>); Error -> Error end
    end.
recv_until(_, Acc, _, Limit) when byte_size(Acc) > Limit -> {error, header_limit};
recv_until(Transport, Acc, Marker, Limit) ->
    case binary:match(Acc, Marker) of
        {Position, Size} -> {ok, binary:part(Acc, 0, Position), binary:part(Acc, Position + Size, byte_size(Acc) - Position - Size)};
        nomatch -> case transport_recv(Transport, 0) of {ok, Data} -> recv_until(Transport, <<Acc/binary, Data/binary>>, Marker, Limit); Error -> Error end
    end.
ensure(_, Buffer, Count) when byte_size(Buffer) >= Count ->
    {ok, binary:part(Buffer, 0, Count), binary:part(Buffer, Count, byte_size(Buffer) - Count)};
ensure(Transport, Buffer, Count) ->
    case transport_recv(Transport, Count - byte_size(Buffer)) of {ok, Data} -> ensure(Transport, <<Buffer/binary, Data/binary>>, Count); Error -> Error end.
read_eof(Transport, Acc) when byte_size(Acc) =< ?BODY_LIMIT ->
    case transport_recv(Transport, 0) of {ok, Data} -> read_eof(Transport, <<Acc/binary, Data/binary>>); {error, closed} -> {ok, Acc, eof}; {error, R} -> {error, native_error('Protocol', R)} end;
read_eof(_, _) -> {error, error_value('Limit', <<"HTTP response body exceeds limit">>)}.

remove_managed(Headers) -> [Pair || Pair = {Name, _} <- Headers,
                                    not lists:member(lower(Name), [<<"host">>, <<"connection">>, <<"content-length">>, <<"transfer-encoding">>])].
values(Name, Headers) -> [Value || {Key, Value} <- Headers, lower(Key) =:= Name].
lower(Value) -> string:lowercase(Value).
put_pool(Origin, Transport, Pool, Total) ->
    Trimmed = case maps:size(Pool) >= Total of true -> close_one(Pool); false -> Pool end,
    maps:put(Origin, {Transport, erlang:monotonic_time(millisecond)}, Trimmed).
close_one(Pool) ->
    [{Key, {Transport, _}} | _] = maps:to_list(Pool), transport_close(Transport), maps:remove(Key, Pool).
expire(Pool, 0) -> maps:foreach(fun(_, {T, _}) -> transport_close(T) end, Pool), #{};
expire(Pool, Idle) ->
    Now = erlang:monotonic_time(millisecond),
    maps:filter(fun(_, {Transport, Used}) -> case Now - Used =< Idle of true -> true; false -> transport_close(Transport), false end end, Pool).
transport_send({tcp, S}, Data) -> gen_tcp:send(S, Data); transport_send({tls, S}, Data) -> ssl:send(S, Data).
transport_recv({tcp, S}, Count) -> gen_tcp:recv(S, Count, ?TIMEOUT); transport_recv({tls, S}, Count) -> ssl:recv(S, Count, ?TIMEOUT).
transport_close({tcp, S}) -> gen_tcp:close(S); transport_close({tls, S}) -> ssl:close(S).
bool_int(true) -> 1; bool_int(false) -> 0.
call(Pid, Message, Timeout) ->
    case is_process_alive(Pid) of
        false -> error_value('Closed', <<"HTTP client is closed">>);
        true -> Ref = make_ref(), Pid ! {call, self(), Ref, Message}, receive {Ref, Value} -> Value after Timeout -> error_value('Timeout', <<"HTTP client operation timed out">>) end
    end.
native_error(timeout, Reason) -> native_error('Timeout', Reason);
native_error(Kind, Reason) -> error_value(Kind, unicode:characters_to_binary(io_lib:format("~p", [Reason]))).
error_value(Kind, Message) -> {'Error', {'Net.NetError', Kind, 'HTTPClient', Message, 'None', 'None'}}.
