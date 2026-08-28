-module(kex_intrinsic_netwebsocket).
-export([connect/2, send/2, receiveMessage/1, session/1, close/1, 'closed?'/1]).

-define(TIMEOUT, 30000).
-define(HEADER_LIMIT, 65536).

connect(URL, {'Net.HTTP.WebSocket.ClientOptions', Protocols, Maximum})
  when is_binary(URL), is_list(Protocols), is_integer(Maximum), Maximum > 0 ->
    case lists:all(fun valid_protocol/1, Protocols) of
        false -> error_value('Parse', <<"invalid WebSocket subprotocol">>);
        true -> start(URL, Protocols, Maximum)
    end;
connect(_, _) -> error_value('Parse', <<"invalid WebSocket client options">>).

send({'Net.HTTP.WebSocket.Connection', Pid}, Message) -> call(Pid, {send, Message});
send(_, _) -> error_value('Parse', <<"invalid WebSocket connection">>).
receiveMessage({'Net.HTTP.WebSocket.Connection', Pid}) -> call(Pid, receive_message);
receiveMessage(_) -> error_value('Parse', <<"invalid WebSocket connection">>).
session({'Net.HTTP.WebSocket.Connection', Pid}) ->
    case call(Pid, session) of
        {'Error', _} -> {'Net.HTTP.WebSocket.Session', 'None'};
        Value -> Value
    end.
close({'Net.HTTP.WebSocket.Connection', Pid}) ->
    case is_process_alive(Pid) of
        true -> _ = call(Pid, close), 'Kex.Unit';
        false -> 'Kex.Unit'
    end.
'closed?'({'Net.HTTP.WebSocket.Connection', Pid}) -> not is_process_alive(Pid).

start(URL, Protocols, Maximum) ->
    Parent = self(), Ref = make_ref(),
    Pid = spawn(fun() -> init(Parent, Ref, URL, Protocols, Maximum) end),
    receive
        {Ref, ok} -> {'Ok', {'Net.HTTP.WebSocket.Connection', Pid}};
        {Ref, error, Error} -> Error
    after 11000 ->
        exit(Pid, kill), error_value('Timeout', <<"WebSocket handshake timed out">>)
    end.

init(Parent, Ref, URL, Protocols, Maximum) ->
    case parse_url(URL) of
        {ok, Origin, Target, HostHeader} ->
            case open_transport(Origin) of
                {ok, Transport} ->
                    case handshake(Transport, Target, HostHeader, Protocols) of
                        {ok, Selected, Buffered} ->
                            Parent ! {Ref, ok}, loop(Transport, Buffered, none,
                                                     Maximum, Selected);
                        {error, Error} ->
                            transport_close(Transport), Parent ! {Ref, error, Error}
                    end;
                {error, Error} -> Parent ! {Ref, error, Error}
            end;
        {error, Error} -> Parent ! {Ref, error, Error}
    end.

loop(Transport, Buffer, Fragment, Maximum, Selected) ->
    receive
        {call, From, Ref, {send, Message}} ->
            Reply = send_message(Transport, Message, Maximum),
            From ! {Ref, Reply}, loop(Transport, Buffer, Fragment, Maximum, Selected);
        {call, From, Ref, receive_message} ->
            case receive_message(Transport, Buffer, Fragment, Maximum) of
                {Reply, NextBuffer, NextFragment, continue} ->
                    From ! {Ref, Reply},
                    loop(Transport, NextBuffer, NextFragment, Maximum, Selected);
                {Reply, _, _, stop} ->
                    From ! {Ref, Reply}, transport_close(Transport)
            end;
        {call, From, Ref, session} ->
            From ! {Ref, {'Net.HTTP.WebSocket.Session', option(Selected)}},
            loop(Transport, Buffer, Fragment, Maximum, Selected);
        {call, From, Ref, close} ->
            _ = send_frame(Transport, 8, <<1000:16/big>>),
            transport_close(Transport), From ! {Ref, 'Kex.Unit'};
        _ -> loop(Transport, Buffer, Fragment, Maximum, Selected)
    end.

send_message(Transport, {'Text', Text}, Maximum) when is_binary(Text) ->
    case valid_utf8(Text) andalso byte_size(Text) =< Maximum of
        true -> result(send_frame(Transport, 1, Text));
        false -> error_value('Protocol', <<"invalid or oversized WebSocket text">>)
    end;
send_message(Transport, {'BinaryMessage', {'Binary', Data}}, Maximum)
  when byte_size(Data) =< Maximum -> result(send_frame(Transport, 2, Data));
send_message(Transport, {'CloseMessage', Code, Reason}, _)
  when is_integer(Code), is_binary(Reason), byte_size(Reason) =< 123 ->
    case valid_close_code(Code) andalso valid_utf8(Reason) of
        true -> result(send_frame(Transport, 8, <<Code:16/big, Reason/binary>>));
        false -> error_value('Protocol', <<"invalid WebSocket close message">>)
    end;
send_message(_, _, _) -> error_value('Protocol', <<"invalid or oversized WebSocket message">>).

receive_message(Transport, Buffer, Fragment, Maximum) ->
    case read_frame(Transport, Buffer, Maximum) of
        {ok, Fin, Opcode, Payload, Rest} ->
            case handle_frame(Transport, Fin, Opcode, Payload, Rest, Fragment, Maximum) of
                {continue_receive, NextBuffer, NextFragment, continue} ->
                    receive_message(Transport, NextBuffer, NextFragment, Maximum);
                Result -> Result
            end;
        {error, Error} -> {Error, <<>>, none, stop}
    end.

handle_frame(Transport, true, 9, Payload, Rest, Fragment, Maximum) ->
    case send_frame(Transport, 10, Payload) of
        ok -> receive_message(Transport, Rest, Fragment, Maximum);
        {error, Reason} -> {native_error('Closed', Reason), Rest, Fragment, stop}
    end;
handle_frame(Transport, true, 10, _, Rest, Fragment, Maximum) ->
    receive_message(Transport, Rest, Fragment, Maximum);
handle_frame(Transport, true, 8, Payload, Rest, _, _) ->
    case close_payload(Payload) of
        {ok, Code, Reason} ->
            _ = send_frame(Transport, 8, Payload),
            {{'Ok', {'CloseMessage', Code, Reason}}, Rest, none, stop};
        error ->
            _ = send_frame(Transport, 8, <<1002:16/big>>),
            {error_value('Protocol', <<"invalid WebSocket close frame">>), Rest, none, stop}
    end;
handle_frame(_, true, 1, Payload, Rest, none, _) ->
    case valid_utf8(Payload) of
        true -> {{'Ok', {'Text', Payload}}, Rest, none, continue};
        false -> {error_value('Protocol', <<"invalid WebSocket UTF-8">>), Rest, none, stop}
    end;
handle_frame(_, true, 2, Payload, Rest, none, _) ->
    {{'Ok', {'BinaryMessage', {'Binary', Payload}}}, Rest, none, continue};
handle_frame(_, false, Opcode, Payload, Rest, none, _)
  when Opcode =:= 1; Opcode =:= 2 ->
    {continue_receive, Rest, {Opcode, Payload}, continue};
handle_frame(_, Fin, 0, Payload, Rest, {Opcode, Acc}, Maximum) ->
    Joined = <<Acc/binary, Payload/binary>>,
    case byte_size(Joined) =< Maximum of
        false -> {error_value('Limit', <<"WebSocket message exceeds limit">>), Rest, none, stop};
        true when Fin ->
            case Opcode of
                1 -> case valid_utf8(Joined) of
                         true -> {{'Ok', {'Text', Joined}}, Rest, none, continue};
                         false -> {error_value('Protocol', <<"invalid WebSocket UTF-8">>), Rest, none, stop}
                     end;
                2 -> {{'Ok', {'BinaryMessage', {'Binary', Joined}}}, Rest, none, continue}
            end;
        true -> {continue_receive, Rest, {Opcode, Joined}, continue}
    end;
handle_frame(_, _, _, _, Rest, _, _) ->
    {error_value('Protocol', <<"invalid WebSocket fragmentation">>), Rest, none, stop}.

read_frame(Transport, Buffer0, Maximum) ->
    case take(Transport, Buffer0, 2) of
        {ok, <<FinBit:1, Rsv:3, Opcode:4, Mask:1, LengthCode:7>>, Buffer1}
          when Rsv =:= 0, Mask =:= 0 ->
            case frame_length(Transport, Buffer1, LengthCode) of
                {ok, Length, Buffer2} when Length =< Maximum,
                                           not (Opcode >= 8 andalso (FinBit =:= 0 orelse Length > 125)) ->
                    case take(Transport, Buffer2, Length) of
                        {ok, Payload, Rest} -> {ok, FinBit =:= 1, Opcode, Payload, Rest};
                        {error, Reason} -> {error, native_error('Closed', Reason)}
                    end;
                {ok, _, _} -> {error, error_value('Limit', <<"WebSocket frame exceeds limit">>)};
                {error, Reason} -> {error, native_error('Closed', Reason)}
            end;
        {ok, _, _} -> {error, error_value('Protocol', <<"invalid WebSocket frame header">>)};
        {error, Reason} -> {error, native_error('Closed', Reason)}
    end.

frame_length(_, Buffer, Code) when Code < 126 -> {ok, Code, Buffer};
frame_length(Transport, Buffer, 126) ->
    case take(Transport, Buffer, 2) of {ok, <<Length:16/big>>, Rest} -> {ok, Length, Rest}; Error -> Error end;
frame_length(Transport, Buffer, 127) ->
    case take(Transport, Buffer, 8) of
        {ok, <<0:1, Length:63/big>>, Rest} -> {ok, Length, Rest};
        {ok, _, _} -> {error, invalid_length};
        Error -> Error
    end.

send_frame(Transport, Opcode, Payload) ->
    Mask = crypto:strong_rand_bytes(4), Masked = mask(Payload, Mask),
    Length = byte_size(Payload),
    Header = if Length < 126 -> <<1:1, 0:3, Opcode:4, 1:1, Length:7>>;
                Length =< 65535 -> <<1:1, 0:3, Opcode:4, 1:1, 126:7, Length:16/big>>;
                true -> <<1:1, 0:3, Opcode:4, 1:1, 127:7, 0:1, Length:63/big>>
             end,
    transport_send(Transport, [Header, Mask, Masked]).

mask(Data, <<A, B, C, D>>) ->
    list_to_binary(mask_bytes(binary:bin_to_list(Data), [A,B,C,D], 0, [])).
mask_bytes([], _, _, Acc) -> lists:reverse(Acc);
mask_bytes([Byte | Rest], Key, Index, Acc) ->
    mask_bytes(Rest, Key, Index + 1,
               [Byte bxor lists:nth((Index rem 4) + 1, Key) | Acc]).

handshake(Transport, Target, Host, Protocols) ->
    Key = base64:encode(crypto:strong_rand_bytes(16)),
    ProtocolHeader = case Protocols of
        [] -> [];
        _ -> [<<"Sec-WebSocket-Protocol: ">>,
              lists:join(<<", ">>, Protocols), <<"\r\n">>]
    end,
    Request = [<<"GET ">>, Target, <<" HTTP/1.1\r\nHost: ">>, Host,
               <<"\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ">>,
               Key, <<"\r\n">>, ProtocolHeader, <<"\r\n">>],
    case transport_send(Transport, Request) of
        ok ->
            case recv_until(Transport, <<>>, <<"\r\n\r\n">>, ?HEADER_LIMIT) of
                {ok, Block, Buffered} -> validate_handshake(Block, Key, Protocols, Buffered);
                {error, Reason} -> {error, native_error('Protocol', Reason)}
            end;
        {error, Reason} -> {error, native_error('Connect', Reason)}
    end.

validate_handshake(Block, Key, Protocols, Buffered) ->
    [Status | Lines] = binary:split(Block, <<"\r\n">>, [global]),
    Headers = parse_headers(Lines, []),
    Expected = base64:encode(crypto:hash(sha, <<Key/binary, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11">>)),
    Accept = first_header(<<"sec-websocket-accept">>, Headers),
    Upgrade = lower(first_header(<<"upgrade">>, Headers)),
    Connection = lower(first_header(<<"connection">>, Headers)),
    Selected = first_header(<<"sec-websocket-protocol">>, Headers),
    case binary:match(Status, <<"HTTP/1.1 101 ">>) =:= {0, 13} andalso
         Accept =:= Expected andalso Upgrade =:= <<"websocket">> andalso
         header_has_token(Connection, <<"upgrade">>) andalso
         (Selected =:= <<>> orelse lists:member(Selected, Protocols)) of
        true -> {ok, case Selected of <<>> -> undefined; _ -> Selected end, Buffered};
        false -> {error, error_value('Protocol', <<"invalid WebSocket handshake response">>)}
    end.

parse_url(URL) ->
    try
        Parts = uri_string:parse(URL), Scheme = maps:get(scheme, Parts),
        Host = maps:get(host, Parts), Port = maps:get(port, Parts, default_port(Scheme)),
        true = (Scheme =:= <<"ws">> orelse Scheme =:= <<"wss">>),
        Path = case maps:get(path, Parts, <<>>) of <<>> -> <<"/">>; Value -> Value end,
        Target = case maps:find(query, Parts) of {ok, Query} -> <<Path/binary, "?", Query/binary>>; error -> Path end,
        HostHeader = case Port =:= default_port(Scheme) of true -> Host; false -> <<Host/binary, ":", (integer_to_binary(Port))/binary>> end,
        {ok, {Scheme, Host, Port}, Target, HostHeader}
    catch _:_ -> {error, error_value('Parse', <<"WebSocket requires an absolute ws or wss URL">>)} end.
default_port(<<"ws">>) -> 80; default_port(<<"wss">>) -> 443.

open_transport({<<"ws">>, Host, Port}) ->
    case gen_tcp:connect(binary_to_list(Host), Port, [binary, {active, false}, {packet, raw}, {nodelay, true}], 10000) of
        {ok, Socket} -> {ok, {tcp, Socket}};
        {error, Reason} -> {error, native_error('Connect', Reason)}
    end;
open_transport({<<"wss">>, Host, Port}) ->
    ssl:start(),
    Options = [binary, {active, false}, {server_name_indication, binary_to_list(Host)},
               {verify, verify_peer}, {cacerts, public_key:cacerts_get()},
               {versions, ['tlsv1.2', 'tlsv1.3']}],
    case ssl:connect(binary_to_list(Host), Port, Options, 10000) of
        {ok, Socket} -> {ok, {tls, Socket}};
        {error, Reason} -> {error, native_error('Connect', Reason)}
    end.

take(_, Buffer, Count) when byte_size(Buffer) >= Count ->
    {ok, binary:part(Buffer, 0, Count),
     binary:part(Buffer, Count, byte_size(Buffer) - Count)};
take(Transport, Buffer, Count) ->
    case transport_recv(Transport, Count - byte_size(Buffer)) of
        {ok, Data} -> take(Transport, <<Buffer/binary, Data/binary>>, Count);
        Error -> Error
    end.
recv_until(_, Acc, _, Limit) when byte_size(Acc) > Limit -> {error, header_limit};
recv_until(Transport, Acc, Marker, Limit) ->
    case binary:match(Acc, Marker) of
        {Position, Size} -> {ok, binary:part(Acc, 0, Position), binary:part(Acc, Position + Size, byte_size(Acc) - Position - Size)};
        nomatch -> case transport_recv(Transport, 0) of {ok, Data} -> recv_until(Transport, <<Acc/binary, Data/binary>>, Marker, Limit); Error -> Error end
    end.
parse_headers([], Acc) -> lists:reverse(Acc);
parse_headers([Line | Rest], Acc) ->
    case binary:split(Line, <<":">>) of [Name, Value] -> parse_headers(Rest, [{lower(Name), string:trim(Value)} | Acc]); _ -> throw(invalid_header) end.
first_header(Name, Headers) -> case [V || {K,V} <- Headers, K =:= Name] of [Value | _] -> Value; [] -> <<>> end.
valid_protocol(Value) when is_binary(Value), byte_size(Value) > 0 ->
    lists:all(fun(C) -> C > 32 andalso C < 127 andalso not lists:member(C, "()<>@,;:\\\"/[]?={} ") end, binary:bin_to_list(Value));
valid_protocol(_) -> false.
valid_utf8(Text) -> case unicode:characters_to_list(Text) of Value when is_list(Value) -> true; _ -> false end.
valid_close_code(Code) -> Code >= 1000 andalso Code < 5000 andalso not lists:member(Code, [1004,1005,1006,1015]).
header_has_token(Value, Token) ->
    lists:member(Token, [string:trim(Part) || Part <- binary:split(Value, <<",">>, [global])]).
close_payload(<<>>) -> {ok, 1005, <<>>};
close_payload(<<Code:16/big, Reason/binary>>) ->
    case valid_close_code(Code) andalso valid_utf8(Reason) of
        true -> {ok, Code, Reason};
        false -> error
    end;
close_payload(_) -> error.
result(ok) -> {'Ok', 'Kex.Unit'}; result({error, Reason}) -> native_error('Closed', Reason).
option(undefined) -> 'None'; option(Value) -> {'Just', Value}.
lower(Value) -> string:lowercase(Value).
transport_send({tcp, S}, Data) -> gen_tcp:send(S, Data); transport_send({tls, S}, Data) -> ssl:send(S, Data).
transport_recv({tcp, S}, Count) -> gen_tcp:recv(S, Count, ?TIMEOUT); transport_recv({tls, S}, Count) -> ssl:recv(S, Count, ?TIMEOUT).
transport_close({tcp, S}) -> gen_tcp:close(S); transport_close({tls, S}) -> ssl:close(S).
call(Pid, Message) ->
    case is_process_alive(Pid) of
        false -> error_value('Closed', <<"WebSocket is closed">>);
        true -> Ref = make_ref(), Pid ! {call, self(), Ref, Message}, receive {Ref, Value} -> Value after 31000 -> error_value('Timeout', <<"WebSocket operation timed out">>) end
    end.
native_error(timeout, _) -> error_value('Timeout', <<"WebSocket operation timed out">>);
native_error(Kind, Reason) -> error_value(Kind, unicode:characters_to_binary(io_lib:format("~p", [Reason]))).
error_value(Kind, Message) -> {'Error', {'Net.NetError', Kind, 'WebSocketClient', Message, 'None', 'None'}}.
