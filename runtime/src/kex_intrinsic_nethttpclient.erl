-module(kex_intrinsic_nethttpclient).
-export([open/1, request/5, request/6, get/2, post/3, statistics/1, close/1,
         request_limits/1]).

%% A request's limits travel as a map: `connect` and `timeout` in
%% milliseconds, `body` and `header` in bytes, and `deadline`, the monotonic
%% millisecond the whole exchange must end by. Every read is given what is
%% left until then, so a server sending a byte at a time cannot hold a
%% request open past `timeout` (kexhq/kex#484).

open({'Net.HTTP.ClientOptions', {'Net.HTTP.PoolOptions', PerOrigin, Total, Queued, Idle}, Request})
  when is_integer(PerOrigin), PerOrigin > 0, is_integer(Total), Total > 0,
       is_integer(Queued), Queued >= 0, is_integer(Idle), Idle >= 0 ->
    case request_limits(Request) of
        {ok, Defaults} ->
            Pid = spawn(fun() -> loop(#{}, Total, Idle, 0, 0, Defaults) end),
            {'Ok', {'Net.HTTP.Client', Pid}};
        error -> error_value('Parse', <<"invalid HTTP request options">>)
    end;
open(_) -> error_value('Parse', <<"invalid HTTP client options">>).

%% A `Net.HTTP.RequestOptions` record as limits, or `error` for a value no
%% request could keep to.
request_limits({'Net.HTTP.RequestOptions', {'Duration', Connect}, {'Duration', Timeout},
                Body, Header})
  when is_number(Connect), Connect > 0, is_number(Timeout), Timeout > 0,
       is_integer(Body), Body >= 0, is_integer(Header), Header > 0 ->
    {ok, #{connect => milliseconds(Connect), timeout => milliseconds(Timeout),
           body => Body, header => Header}};
request_limits(_) -> error.

get(Client, URL) -> request(Client, <<"GET">>, URL, {'Net.HTTP.Headers', []}, {'Binary', <<>>}).
post(Client, URL, Body) -> request(Client, <<"POST">>, URL, {'Net.HTTP.Headers', []}, Body).

request(Client, Method, URL, Headers, Body) ->
    send_request(Client, Method, URL, Headers, Body, default).
request(Client, Method, URL, Headers, Body, Options) ->
    case request_limits(Options) of
        {ok, Limits} -> send_request(Client, Method, URL, Headers, Body, Limits);
        error -> error_value('Parse', <<"invalid HTTP request options">>)
    end.

%% The deadline counts from here, so time spent queued behind the client's
%% other requests is part of it. The client process always answers by then,
%% so the caller only has to notice it dying.
send_request({'Net.HTTP.Client', Pid}, Method, URL, Headers, Body, Limits) ->
    Sent = erlang:monotonic_time(millisecond),
    call(Pid, {request, Method, URL, Headers, Body, Limits, Sent}, infinity).
statistics({'Net.HTTP.Client', Pid}) ->
    call(Pid, statistics, 1000).
close({'Net.HTTP.Client', Pid}) ->
    call(Pid, close, 2000).

loop(Pool0, Total, Idle, Requests, Reused, Defaults) ->
    Pool = expire(Pool0, Idle),
    receive
        {call, From, Ref, {request, Method, URL, Headers, Body, Limits0, Sent}} ->
            Limits1 = case Limits0 of default -> Defaults; _ -> Limits0 end,
            Limits = Limits1#{deadline => Sent + maps:get(timeout, Limits1)},
            {Reply, NextPool, WasReused} = perform(Method, URL, Headers, Body, Pool, Total, Limits),
            From ! {Ref, Reply},
            loop(NextPool, Total, Idle, Requests + 1, Reused + bool_int(WasReused), Defaults);
        {call, From, Ref, statistics} ->
            From ! {Ref, {'Net.HTTP.ClientStatistics', maps:size(Pool), Requests, Reused}},
            loop(Pool, Total, Idle, Requests, Reused, Defaults);
        {call, From, Ref, close} ->
            maps:foreach(fun(_, {Transport, _}) -> transport_close(Transport) end, Pool),
            From ! {Ref, {'Ok', {'Net.HTTP.ClientCloseReport', maps:size(Pool)}}};
        _ -> loop(Pool, Total, Idle, Requests, Reused, Defaults)
    end.

perform(Method, URL, {'Net.HTTP.Headers', Headers}, {'Binary', Body}, Pool, Total, Limits)
  when is_binary(Method), is_binary(URL), is_list(Headers), is_binary(Body) ->
    case valid_method(Method) andalso lists:all(fun valid_header/1, Headers) of
      false -> {error_value('Parse', <<"invalid HTTP method or headers">>), Pool, false};
      true -> case parse_url(URL) of
        {ok, Origin, Target, HostHeader} ->
            case maps:take(Origin, Pool) of
                {{Transport, _}, Rest} ->
                    case exchange(Transport, Method, Target, HostHeader, Headers, Body, Limits) of
                        {ok, Response, reusable} -> {Response, put_pool(Origin, Transport, Rest, Total), true};
                        {ok, Response, close} -> transport_close(Transport), {Response, Rest, true};
                        {error, Error} -> transport_close(Transport), {Error, Rest, true}
                    end;
                error ->
                    case connect(Origin, Limits) of
                        {ok, Transport} ->
                            case exchange(Transport, Method, Target, HostHeader, Headers, Body, Limits) of
                                {ok, Response, reusable} -> {Response, put_pool(Origin, Transport, Pool, Total), false};
                                {ok, Response, close} -> transport_close(Transport), {Response, Pool, false};
                                {error, Error} -> transport_close(Transport), {Error, Pool, false}
                            end;
                        {error, Error} -> {Error, Pool, false}
                    end
            end;
        {error, Error} -> {Error, Pool, false}
    end end;
perform(_, _, _, _, Pool, _, _) -> {error_value('Parse', <<"invalid HTTP request values">>), Pool, false}.

parse_url(URL) ->
    try
        Parts = uri_string:parse(URL),
        Scheme = maps:get(scheme, Parts), Host = maps:get(host, Parts),
        Port = maps:get(port, Parts, default_port(Scheme)),
        true = (Scheme =:= <<"http">> orelse Scheme =:= <<"https">>),
        Path = case maps:get(path, Parts, <<>>) of <<>> -> <<"/">>; P -> P end,
        Target = case maps:find(query, Parts) of {ok, Q} -> <<Path/binary, "?", Q/binary>>; error -> Path end,
        AuthorityHost = authority_host(Host),
        HostHeader = case Port =:= default_port(Scheme) of
                         true -> AuthorityHost;
                         false -> <<AuthorityHost/binary, ":", (integer_to_binary(Port))/binary>>
                     end,
        {ok, {Scheme, Host, Port}, Target, HostHeader}
    catch _:_ -> {error, error_value('Parse', <<"HTTP requires an absolute http or https URL">>)} end.
default_port(<<"http">>) -> 80; default_port(<<"https">>) -> 443.

connect(Origin, Limits) ->
    case min(maps:get(connect, Limits), remaining(Limits)) of
        0 -> {error, error_value('Timeout', <<"HTTP request timed out">>)};
        Timeout -> connect_within(Origin, Timeout)
    end.
connect_within({<<"http">>, Host, Port}, Timeout) ->
    case gen_tcp:connect(binary_to_list(Host), Port, [binary, {active, false}, {packet, raw}, {nodelay, true}], Timeout) of
        {ok, Socket} -> {ok, {tcp, Socket}};
        {error, Reason} -> {error, native_error('Connect', Reason)}
    end;
connect_within({<<"https">>, Host, Port}, Timeout) ->
    ssl:start(),
    Options = [binary, {active, false}, {server_name_indication, binary_to_list(Host)},
               {verify, verify_peer}, {cacerts, public_key:cacerts_get()},
               {customize_hostname_check,
                [{match_fun, public_key:pkix_verify_hostname_match_fun(https)}]},
               {versions, ['tlsv1.2', 'tlsv1.3']}, {alpn_advertised_protocols, [<<"http/1.1">>]}],
    case ssl:connect(binary_to_list(Host), Port, Options, Timeout) of
        {ok, Socket} -> {ok, {tls, Socket}};
        {error, Reason} -> {error, native_error('Connect', Reason)}
    end.

exchange(Transport, Method, Target, Host, Headers0, Body, Limits) ->
    Headers1 = remove_managed(Headers0),
    Headers = [{<<"Host">>, Host}, {<<"Connection">>, <<"keep-alive">>} |
               case byte_size(Body) of 0 -> Headers1; N -> [{<<"Content-Length">>, integer_to_binary(N)} | Headers1] end],
    Request = [Method, <<" ">>, Target, <<" HTTP/1.1\r\n">>,
               [[Name, <<": ">>, Value, <<"\r\n">>] || {Name, Value} <- Headers], <<"\r\n">>, Body],
    case transport_send(Transport, Request) of
        ok -> read_response(Transport, Method, Limits);
        {error, Reason} -> {error, native_error('Connect', Reason)}
    end.

read_response(Transport, Method, Limits) -> read_response(Transport, Method, <<>>, 0, Limits).
read_response(_, _, _, Interim, _) when Interim >= 8 ->
    {error, error_value('Protocol', <<"too many informational HTTP responses">>)};
read_response(Transport, Method, Initial, Interim, Limits) ->
    case recv_until(Transport, Initial, <<"\r\n\r\n">>, Limits) of
        {ok, Block, Buffered} ->
            case parse_response_head(Block) of
                {ok, _, Code, _} when Code >= 100, Code < 200, Code =/= 101 ->
                    read_response(Transport, Method, Buffered, Interim + 1, Limits);
                {ok, _, 101, _} ->
                    {error, error_value('Protocol', <<"HTTP protocol upgrades are unsupported by this client">>)};
                {ok, Version, Code, Headers} ->
                    case response_body(Transport, Method, Code, Headers, Buffered, Limits) of
                        {ok, Body, Framing, Extra} ->
                            ConnectionClose = header_has_token(<<"connection">>, Headers, <<"close">>),
                            Reuse = case Version =:= <<"HTTP/1.1">> andalso not ConnectionClose andalso
                                         Framing =/= eof andalso Extra =:= <<>> of true -> reusable; false -> close end,
                            {ok, {'Ok', {'Net.HTTP.Response', {'Net.HTTP.Status', Code},
                                        {'Net.HTTP.Headers', Headers}, {'Binary', Body}}}, Reuse};
                        {error, Error} -> {error, Error}
                    end;
                {error, Error} -> {error, Error}
            end;
        {error, header_limit} ->
            {error, error_value('Limit', <<"HTTP response headers exceed limit">>)};
        {error, Reason} -> {error, native_error('Protocol', Reason)}
    end.

parse_response_head(Block) ->
    case binary:split(Block, <<"\r\n">>, [global]) of
        [StatusLine | Lines] ->
            case binary:split(StatusLine, <<" ">>, [global, trim_all]) of
                [Version, CodeText | _] when Version =:= <<"HTTP/1.1">>; Version =:= <<"HTTP/1.0">> ->
                    try
                        Code = binary_to_integer(CodeText),
                        true = Code >= 100 andalso Code =< 599,
                        true = length(Lines) =< 100,
                        {ok, Version, Code, parse_headers(Lines, [])}
                    catch _:_ -> {error, error_value('Protocol', <<"invalid HTTP status line or headers">>)} end;
                _ -> {error, error_value('Protocol', <<"invalid HTTP status line">>)}
            end;
        _ -> {error, error_value('Protocol', <<"missing HTTP status line">>)}
    end.
parse_headers([], Acc) -> lists:reverse(Acc);
parse_headers([Line | Rest], Acc) ->
    case binary:split(Line, <<":">>) of
        [Name, Value] when Name =/= <<>> ->
            Trimmed = string:trim(Value),
            case valid_header({Name, Trimmed}) of
                true -> parse_headers(Rest, [{Name, Trimmed} | Acc]);
                false -> throw(invalid_header)
            end;
        _ -> throw(invalid_header)
    end.

response_body(_, <<"HEAD">>, _, _, Buffered, _) -> {ok, <<>>, fixed, Buffered};
response_body(_, _, Code, _, Buffered, _) when Code =:= 204; Code =:= 304 -> {ok, <<>>, fixed, Buffered};
response_body(Transport, _, _, Headers, Buffered, Limits) ->
    case {values(<<"transfer-encoding">>, Headers), lists:usort(values(<<"content-length">>, Headers))} of
        {[Transfer], []} ->
            case lower(Transfer) of <<"chunked">> -> read_chunks(Transport, Buffered, <<>>, Limits); _ -> {error, error_value('Protocol', <<"unsupported transfer encoding">>)} end;
        {[], [LengthText]} ->
            try
                Length = binary_to_integer(LengthText),
                case Length >= 0 andalso Length =< maps:get(body, Limits) of
                    true -> case ensure(Transport, Buffered, Length, Limits) of {ok, Data, Rest} -> {ok, Data, fixed, Rest}; {error, R} -> {error, native_error('Protocol', R)} end;
                    false -> {error, body_limit()}
                end
            catch _:_ -> {error, error_value('Protocol', <<"invalid Content-Length">>)} end;
        {[], []} -> read_eof(Transport, Buffered, Limits);
        _ -> {error, error_value('Protocol', <<"ambiguous HTTP response framing">>)}
    end.

read_chunks(Transport, Buffer, Acc, Limits = #{body := BodyLimit}) when byte_size(Acc) =< BodyLimit ->
    case read_line(Transport, Buffer, Limits) of
        {ok, Line, Rest} ->
            SizeText = hd(binary:split(Line, <<";">>)),
            try list_to_integer(binary_to_list(SizeText), 16) of
                0 -> consume_trailers(Transport, Rest, Acc, Limits);
                Size when byte_size(Acc) + Size =< BodyLimit ->
                    case ensure(Transport, Rest, Size + 2, Limits) of
                        {ok, DataAndCRLF, Tail} ->
                            <<Data:Size/binary, "\r\n">> = DataAndCRLF,
                            read_chunks(Transport, Tail, <<Acc/binary, Data/binary>>, Limits);
                        {error, R} -> {error, native_error('Protocol', R)}
                    end;
                _ -> {error, body_limit()}
            catch _:_ -> {error, error_value('Protocol', <<"invalid chunk framing">>)} end;
        {error, R} -> {error, native_error('Protocol', R)}
    end;
read_chunks(_, _, _, _) -> {error, body_limit()}.

consume_trailers(_, <<"\r\n">>, Acc, _) -> {ok, Acc, chunked, <<>>};
consume_trailers(_, <<"\r\n", _/binary>>, _, _) ->
    {error, error_value('Protocol', <<"unexpected bytes after chunked response">>)};
consume_trailers(Transport, Buffer, Acc, Limits) ->
    case recv_until(Transport, Buffer, <<"\r\n\r\n">>, Limits) of
        {ok, TrailerBlock, <<>>} ->
            try parse_headers(binary:split(TrailerBlock, <<"\r\n">>, [global]), []) of
                Trailers ->
                    case values(<<"content-length">>, Trailers) ++
                         values(<<"transfer-encoding">>, Trailers) of
                        [] -> {ok, Acc, chunked, <<>>};
                        _ -> {error, error_value('Protocol', <<"framing fields are forbidden in trailers">>)}
                    end
            catch _:_ -> {error, error_value('Protocol', <<"invalid HTTP trailers">>)} end;
        {ok, _, _} -> {error, error_value('Protocol', <<"unexpected bytes after chunked response">>)};
        {error, Reason} -> {error, native_error('Protocol', Reason)}
    end.

read_line(Transport, Buffer, Limits = #{header := HeaderLimit}) ->
    case binary:match(Buffer, <<"\r\n">>) of
        {Position, 2} when Position =< HeaderLimit -> {ok, binary:part(Buffer, 0, Position), binary:part(Buffer, Position + 2, byte_size(Buffer) - Position - 2)};
        {_, _} -> {error, header_limit};
        nomatch when byte_size(Buffer) > HeaderLimit -> {error, header_limit};
        nomatch -> case transport_recv(Transport, 0, Limits) of {ok, Data} -> read_line(Transport, <<Buffer/binary, Data/binary>>, Limits); Error -> Error end
    end.
recv_until(Transport, Acc, Marker, Limits = #{header := Limit}) ->
    case binary:match(Acc, Marker) of
        {Position, Size} when Position =< Limit -> {ok, binary:part(Acc, 0, Position), binary:part(Acc, Position + Size, byte_size(Acc) - Position - Size)};
        {_, _} -> {error, header_limit};
        nomatch when byte_size(Acc) > Limit -> {error, header_limit};
        nomatch -> case transport_recv(Transport, 0, Limits) of {ok, Data} -> recv_until(Transport, <<Acc/binary, Data/binary>>, Marker, Limits); Error -> Error end
    end.
ensure(_, Buffer, Count, _) when byte_size(Buffer) >= Count ->
    {ok, binary:part(Buffer, 0, Count), binary:part(Buffer, Count, byte_size(Buffer) - Count)};
ensure(Transport, Buffer, Count, Limits) ->
    case transport_recv(Transport, Count - byte_size(Buffer), Limits) of {ok, Data} -> ensure(Transport, <<Buffer/binary, Data/binary>>, Count, Limits); Error -> Error end.
read_eof(Transport, Acc, Limits = #{body := BodyLimit}) when byte_size(Acc) =< BodyLimit ->
    case transport_recv(Transport, 0, Limits) of {ok, Data} -> read_eof(Transport, <<Acc/binary, Data/binary>>, Limits); {error, closed} -> {ok, Acc, eof, <<>>}; {error, R} -> {error, native_error('Protocol', R)} end;
read_eof(_, _, _) -> {error, body_limit()}.

body_limit() -> error_value('Limit', <<"HTTP response body exceeds limit">>).

remove_managed(Headers) -> [Pair || Pair = {Name, _} <- Headers,
                                    not lists:member(lower(Name), [<<"host">>, <<"connection">>, <<"content-length">>, <<"transfer-encoding">>])].
values(Name, Headers) -> [Value || {Key, Value} <- Headers, lower(Key) =:= Name].
lower(Value) -> string:lowercase(Value).
header_has_token(Name, Headers, Token) ->
    lists:member(Token, [string:trim(Part) || Value <- values(Name, Headers),
                                             Part <- binary:split(lower(Value), <<",">>, [global])]).
valid_method(Method) when byte_size(Method) > 0 ->
    lists:all(fun token_char/1, binary:bin_to_list(Method));
valid_method(_) -> false.
valid_header({Name, Value}) when is_binary(Name), is_binary(Value), byte_size(Name) > 0 ->
    lists:all(fun token_char/1, binary:bin_to_list(Name)) andalso
    lists:all(fun(C) -> C =:= 9 orelse C >= 32 andalso C =/= 127 end,
              binary:bin_to_list(Value));
valid_header(_) -> false.
token_char(C) -> C > 32 andalso C < 127 andalso
                 not lists:member(C, "()<>@,;:\\\"/[]?={} ").
authority_host(Host) ->
    case binary:match(Host, <<":">>) of nomatch -> Host; _ -> <<"[", Host/binary, "]">> end.
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
%% Each read waits only for what is left of the request's deadline.
transport_recv(Transport, Count, Limits) ->
    case remaining(Limits) of
        0 -> {error, timeout};
        Left -> recv(Transport, Count, Left)
    end.
recv({tcp, S}, Count, Timeout) -> gen_tcp:recv(S, Count, Timeout);
recv({tls, S}, Count, Timeout) -> ssl:recv(S, Count, Timeout).
transport_close({tcp, S}) -> gen_tcp:close(S); transport_close({tls, S}) -> ssl:close(S).
remaining(#{deadline := Deadline}) ->
    max(0, Deadline - erlang:monotonic_time(millisecond)).
milliseconds(Seconds) -> max(1, round(Seconds * 1000)).
bool_int(true) -> 1; bool_int(false) -> 0.
call(Pid, Message, Timeout) ->
    case is_process_alive(Pid) of
        false -> error_value('Closed', <<"HTTP client is closed">>);
        true ->
            Monitor = erlang:monitor(process, Pid),
            Ref = make_ref(),
            Pid ! {call, self(), Ref, Message},
            receive
                {Ref, Value} -> erlang:demonitor(Monitor, [flush]), Value;
                {'DOWN', Monitor, process, Pid, _} -> error_value('Closed', <<"HTTP client is closed">>)
            after Timeout ->
                erlang:demonitor(Monitor, [flush]),
                error_value('Timeout', <<"HTTP client operation timed out">>)
            end
    end.
%% Running out of time is a Timeout whichever step it happened in.
native_error(_, timeout) -> error_value('Timeout', <<"HTTP request timed out">>);
native_error(_, header_limit) -> error_value('Limit', <<"HTTP response headers exceed limit">>);
native_error(Kind, Reason) -> error_value(Kind, unicode:characters_to_binary(io_lib:format("~p", [Reason]))).
error_value(Kind, Message) -> {'Error', {'Net.NetError', Kind, 'HTTPClient', Message, 'None', 'None'}}.
