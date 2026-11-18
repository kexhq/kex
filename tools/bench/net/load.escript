#!/usr/bin/env escript
%%! +S 4
%% Load generator for `make bench-net` (kexhq/kex#670).
%%
%%   load.escript ws   Port Connections MessagesPerConnection
%%   load.escript http Port Connections RequestsPerConnection
%%
%% `ws`: every connection opens, upgrades at /ws, then keeps one 64-byte
%% masked text message in flight until it has had all its echoes back.
%% `http`: every connection sends keep-alive GET /hello requests, one at a
%% time. Throughput counts from the first connect, so connection set-up and
%% the handshake are part of it, as in the issue's table. Latency is per
%% message or request, from send until the reply has been read.
-mode(compile).

main([Mode, Port, Conns, Per]) ->
    run(list_to_atom(Mode), list_to_integer(Port), list_to_integer(Conns), list_to_integer(Per));
main(_) ->
    io:format("usage: load.escript ws|http Port Connections PerConnection~n"),
    halt(2).

run(Mode, Port, Conns, Per) ->
    Parent = self(),
    Start = erlang:monotonic_time(microsecond),
    Pids = [spawn_link(fun() -> Parent ! {done, self(), client(Mode, Port, Per)} end)
            || _ <- lists:seq(1, Conns)],
    Results = [receive {done, P, R} -> R end || P <- Pids],
    Elapsed = erlang:monotonic_time(microsecond) - Start,
    Latencies = lists:sort(lists:append([L || {ok, L} <- Results])),
    Errors = length([E || {error, E} <- Results]),
    N = length(Latencies),
    Rate = case Elapsed of 0 -> 0; _ -> N * 1000000 div Elapsed end,
    io:format("~s conns=~w per=~w done=~w errors=~w time=~.2fs rate=~w/s p50=~wus p99=~wus~n",
              [Mode, Conns, Per, N, Errors, Elapsed / 1.0e6, Rate,
               percentile(Latencies, 50), percentile(Latencies, 99)]).

percentile([], _) -> 0;
percentile(Sorted, P) ->
    lists:nth(max(1, min(length(Sorted), (length(Sorted) * P + 99) div 100)), Sorted).

connect(Port) ->
    connect(Port, 50).
connect(Port, Tries) ->
    case gen_tcp:connect({127,0,0,1}, Port, [binary, {active, false}, {packet, raw}, {nodelay, true}], 30000) of
        {ok, S} -> {ok, S};
        {error, _} when Tries > 0 -> timer:sleep(20), connect(Port, Tries - 1);
        Error -> Error
    end.

client(Mode, Port, Per) ->
    try
        {ok, S} = connect(Port),
        Result = case Mode of
            ws -> ws_client(S, Per);
            http -> http_client(S, Per)
        end,
        gen_tcp:close(S),
        {ok, Result}
    catch C:E -> {error, {C, E}}
    end.

%% --- HTTP -------------------------------------------------------------------

http_client(S, Per) ->
    Req = <<"GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n">>,
    http_loop(S, Req, Per, <<>>, []).

http_loop(_S, _Req, 0, _Buf, Acc) -> Acc;
http_loop(S, Req, N, Buf, Acc) ->
    T0 = erlang:monotonic_time(microsecond),
    ok = gen_tcp:send(S, Req),
    Rest = read_response(S, Buf),
    http_loop(S, Req, N - 1, Rest, [erlang:monotonic_time(microsecond) - T0 | Acc]).

read_response(S, Buf) ->
    case binary:split(Buf, <<"\r\n\r\n">>) of
        [Head, Body] ->
            Len = content_length(Head),
            read_exact_body(S, Body, Len);
        [_] -> read_response(S, <<Buf/binary, (recv(S))/binary>>)
    end.

read_exact_body(_S, Body, Len) when byte_size(Body) >= Len ->
    binary:part(Body, Len, byte_size(Body) - Len);
read_exact_body(S, Body, Len) ->
    read_exact_body(S, <<Body/binary, (recv(S))/binary>>, Len).

content_length(Head) ->
    Lines = binary:split(Head, <<"\r\n">>, [global]),
    hd([list_to_integer(string:trim(binary_to_list(V)))
        || L <- Lines,
           [K, V] <- [binary:split(L, <<":">>)],
           string:lowercase(binary_to_list(K)) =:= "content-length"] ++ [0]).

recv(S) ->
    {ok, D} = gen_tcp:recv(S, 0, 30000),
    D.

%% --- WebSocket ----------------------------------------------------------------

ws_client(S, Per) ->
    Key = base64:encode(crypto:strong_rand_bytes(16)),
    ok = gen_tcp:send(S, [<<"GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                            "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ">>,
                          Key, <<"\r\n\r\n">>]),
    Rest = read_upgrade(S, <<>>),
    Payload = binary:copy(<<"x">>, 64),
    ws_loop(S, Payload, Per, Rest, []).

read_upgrade(S, Buf) ->
    case binary:split(Buf, <<"\r\n\r\n">>) of
        [<<"HTTP/1.1 101", _/binary>>, Rest] -> Rest;
        [Other, _] -> error({no_upgrade, Other});
        [_] -> read_upgrade(S, <<Buf/binary, (recv(S))/binary>>)
    end.

ws_loop(S, _Payload, 0, _Buf, Acc) ->
    gen_tcp:send(S, client_frame(8, <<1000:16>>)),
    Acc;
ws_loop(S, Payload, N, Buf, Acc) ->
    T0 = erlang:monotonic_time(microsecond),
    ok = gen_tcp:send(S, client_frame(1, Payload)),
    {Payload, Rest} = read_frame(S, Buf),
    ws_loop(S, Payload, N - 1, Rest, [erlang:monotonic_time(microsecond) - T0 | Acc]).

client_frame(Opcode, Payload) ->
    Mask = crypto:strong_rand_bytes(4),
    Len = byte_size(Payload),
    Header = if
        Len < 126 -> <<1:1, 0:3, Opcode:4, 1:1, Len:7>>;
        Len < 65536 -> <<1:1, 0:3, Opcode:4, 1:1, 126:7, Len:16>>;
        true -> <<1:1, 0:3, Opcode:4, 1:1, 127:7, Len:64>>
    end,
    [Header, Mask, crypto:exor(Payload, binary:part(binary:copy(Mask, (Len + 3) div 4), 0, Len))].

%% A server frame is unmasked.
read_frame(S, <<_:1, _:3, _:4, 0:1, 127:7, Len:64, Rest/binary>> = Buf) -> take(S, Buf, Rest, Len);
read_frame(S, <<_:1, _:3, _:4, 0:1, 126:7, Len:16, Rest/binary>> = Buf) -> take(S, Buf, Rest, Len);
read_frame(S, <<_:1, _:3, _:4, 0:1, Len:7, Rest/binary>> = Buf) when Len < 126 -> take(S, Buf, Rest, Len);
read_frame(S, Buf) -> read_frame(S, <<Buf/binary, (recv(S))/binary>>).

take(_S, _Buf, Rest, Len) when byte_size(Rest) >= Len ->
    <<Payload:Len/binary, More/binary>> = Rest,
    {Payload, More};
take(S, Buf, _Rest, _Len) ->
    read_frame(S, <<Buf/binary, (recv(S))/binary>>).
