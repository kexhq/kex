-module(kex_intrinsic_netwebsocket).
-export([connect/2, send/2, receiveMessage/1, session/1, close/1, 'closed?'/1,
         upgrade/2, accept/4]).

-define(TIMEOUT, 30000).
-define(HEADER_LIMIT, 65536).
-define(DEFAULT_MAX_MESSAGE, 16777216).
-define(WS_GUID, <<"258EAFA5-E914-47DA-95CA-C5AB0DC85B11">>).

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

% Server-side upgrade. Called synchronously from the route handler, before
% `Net.HTTP.Server` has sent any bytes: it only decides what the response
% should be. `kex_intrinsic_nethttpserver` recognizes the
% 'Net.HTTP.WebSocket.UpgradeResponse' wrapper an Accept produces, sends the
% 101 response itself, then calls `accept/4` to hand the raw socket over.
upgrade({'Net.HTTP.Request', Method, _URI, {'Net.HTTP.Headers', Headers}, _Body}, Decide)
  when is_function(Decide, 1) ->
    Lower = [{lower(Name), Value} || {Name, Value} <- Headers],
    case validate_upgrade_request(Method, Lower) of
        {ok, Key} ->
            Subprotocols = split_protocols(header_values(<<"sec-websocket-protocol">>, Lower)),
            decision_response(Decide({'Net.HTTP.WebSocket.Handshake', Subprotocols}), Key, Subprotocols);
        {reject, Response} -> Response
    end;
upgrade(_, _) -> http_response(400, <<"Bad Request\n">>).

decision_response({'Reject', Response = {'Net.HTTP.Response', _, _, _}}, _, _) -> Response;
decision_response({'Accept', HandlerFun, {'Net.HTTP.Headers', ExtraHeaders}, SubprotocolOption}, Key, Subprotocols)
  when is_function(HandlerFun, 1) ->
    case selected_protocol(SubprotocolOption, Subprotocols) of
        {ok, Selected} ->
            {'Net.HTTP.WebSocket.UpgradeResponse',
             accept_response(Key, Selected, ExtraHeaders), HandlerFun, Selected};
        error -> http_response(500, <<"Internal Server Error\n">>)
    end;
decision_response(_, _, _) -> http_response(500, <<"Internal Server Error\n">>).

% A GET with a syntactically valid handshake reaches the app's decision; an
% unsupported version gets the RFC 6455 §4.4 required response; anything
% else isn't a WebSocket attempt at all.
validate_upgrade_request(<<"GET">>, Headers) ->
    case headers_have_token(<<"upgrade">>, Headers, <<"websocket">>) andalso
         headers_have_token(<<"connection">>, Headers, <<"upgrade">>) andalso
         valid_key(header_values(<<"sec-websocket-key">>, Headers)) of
        true ->
            case header_values(<<"sec-websocket-version">>, Headers) of
                [<<"13">>] -> {ok, hd(header_values(<<"sec-websocket-key">>, Headers))};
                _ -> {reject, version_required_response()}
            end;
        false -> {reject, http_response(400, <<"Bad Request\n">>)}
    end;
validate_upgrade_request(_, _) -> {reject, http_response(400, <<"Bad Request\n">>)}.

valid_key([Key]) -> try byte_size(base64:decode(Key)) =:= 16 catch _:_ -> false end;
valid_key(_) -> false.

version_required_response() ->
    {'Net.HTTP.Response', {'Net.HTTP.Status', 426},
     {'Net.HTTP.Headers', [{<<"Sec-WebSocket-Version">>, <<"13">>}]},
     {'Binary', <<"Upgrade Required\n">>}}.

split_protocols(Values) ->
    [Part || Value <- Values,
             Raw <- binary:split(Value, <<",">>, [global]),
             Part <- [string:trim(Raw)],
             Part =/= <<>>].

selected_protocol('None', _) -> {ok, undefined};
selected_protocol({'Just', Protocol}, Subprotocols) when is_binary(Protocol) ->
    case valid_protocol(Protocol) andalso lists:member(Protocol, Subprotocols) of
        true -> {ok, Protocol};
        false -> error
    end;
selected_protocol(_, _) -> error.

accept_response(Key, Selected, ExtraHeaders) ->
    ProtocolHeader = case Selected of
        undefined -> [];
        _ -> [{<<"Sec-WebSocket-Protocol">>, Selected}]
    end,
    Filtered = [{Name, Value} || {Name, Value} <- ExtraHeaders, not reserved_header(Name)],
    {'Net.HTTP.Response', {'Net.HTTP.Status', 101},
     {'Net.HTTP.Headers', [{<<"Upgrade">>, <<"websocket">>},
                           {<<"Connection">>, <<"Upgrade">>},
                           {<<"Sec-WebSocket-Accept">>, accept_value(Key)}] ++
                          ProtocolHeader ++ Filtered},
     {'Binary', <<>>}}.

reserved_header(Name) ->
    lists:member(lower(Name), [<<"upgrade">>, <<"connection">>, <<"sec-websocket-accept">>,
                               <<"sec-websocket-protocol">>, <<"content-length">>]).

accept_value(Key) -> base64:encode(crypto:hash(sha, <<Key/binary, ?WS_GUID/binary>>)).

http_response(Status, Body) ->
    {'Net.HTTP.Response', {'Net.HTTP.Status', Status},
     {'Net.HTTP.Headers', [{<<"Content-Type">>, <<"text/plain; charset=utf-8">>}]},
     {'Binary', Body}}.

% Called by `kex_intrinsic_nethttpserver` right after the 101 response bytes
% are on the wire. This process — already the raw socket's reader/writer in
% the HTTP server's per-connection worker — becomes the connection's message
% loop, exactly as `loop/5` is for a client; a fresh process runs the
% application's handler function so it can call back into this same loop via
% `send`/`receiveMessage` without deadlocking on itself. Blocks until the
% handler returns (or crashes), which is what lets the HTTP server's own
% handler bookkeeping and graceful shutdown keep working unchanged.
accept(Socket, Buffered, HandlerFun, Selected) ->
    Transport = {tcp, Socket},
    ConnPid = self(),
    HandlerPid = spawn(fun() -> run_handler(HandlerFun, ConnPid) end),
    Monitor = erlang:monitor(process, HandlerPid),
    server_loop(Transport, Buffered, none, ?DEFAULT_MAX_MESSAGE, Selected, HandlerPid, Monitor, false).

run_handler(HandlerFun, ConnPid) ->
    _ = HandlerFun({'Net.HTTP.WebSocket.Connection', ConnPid}),
    ok.

server_loop(Transport, Buffer, Fragment, Maximum, Selected, HandlerPid, Monitor, Closed) ->
    receive
        {call, From, Ref, {send, _}} when Closed ->
            From ! {Ref, error_value('Closed', <<"WebSocket is closed">>)},
            server_loop(Transport, Buffer, Fragment, Maximum, Selected, HandlerPid, Monitor, Closed);
        {call, From, Ref, {send, Message}} ->
            Reply = send_message(Transport, server, Message, Maximum),
            From ! {Ref, Reply},
            case Reply of
                {'Error', _} ->
                    transport_close(Transport),
                    server_loop(Transport, Buffer, Fragment, Maximum, Selected, HandlerPid, Monitor, true);
                _ -> server_loop(Transport, Buffer, Fragment, Maximum, Selected, HandlerPid, Monitor, Closed)
            end;
        {call, From, Ref, receive_message} when Closed ->
            From ! {Ref, error_value('Closed', <<"WebSocket is closed">>)},
            server_loop(Transport, Buffer, Fragment, Maximum, Selected, HandlerPid, Monitor, Closed);
        {call, From, Ref, receive_message} ->
            case receive_message(Transport, server, Buffer, Fragment, Maximum) of
                {Reply, NextBuffer, NextFragment, continue} ->
                    From ! {Ref, Reply},
                    server_loop(Transport, NextBuffer, NextFragment, Maximum, Selected, HandlerPid, Monitor, Closed);
                {Reply, NextBuffer, _, stop} ->
                    From ! {Ref, Reply},
                    server_loop(Transport, NextBuffer, none, Maximum, Selected, HandlerPid, Monitor, true)
            end;
        {call, From, Ref, session} ->
            From ! {Ref, {'Net.HTTP.WebSocket.Session', option(Selected)}},
            server_loop(Transport, Buffer, Fragment, Maximum, Selected, HandlerPid, Monitor, Closed);
        {call, From, Ref, close} ->
            case Closed of
                false -> _ = send_frame(Transport, server, 8, <<1000:16/big>>), transport_close(Transport);
                true -> ok
            end,
            From ! {Ref, 'Kex.Unit'},
            server_loop(Transport, Buffer, Fragment, Maximum, Selected, HandlerPid, Monitor, true);
        {'DOWN', Monitor, process, HandlerPid, Reason} ->
            case Closed of
                false -> _ = send_frame(Transport, server, 8, <<1000:16/big>>), transport_close(Transport);
                true -> ok
            end,
            case Reason of normal -> ok; _ -> error end
    end.

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
            Reply = send_message(Transport, client, Message, Maximum),
            From ! {Ref, Reply},
            case Reply of
                {'Error', _} -> transport_close(Transport);
                _ -> loop(Transport, Buffer, Fragment, Maximum, Selected)
            end;
        {call, From, Ref, receive_message} ->
            case receive_message(Transport, client, Buffer, Fragment, Maximum) of
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
            _ = send_frame(Transport, client, 8, <<1000:16/big>>),
            transport_close(Transport), From ! {Ref, 'Kex.Unit'};
        _ -> loop(Transport, Buffer, Fragment, Maximum, Selected)
    end.

send_message(Transport, Role, {'Text', Text}, Maximum) when is_binary(Text) ->
    case valid_utf8(Text) andalso byte_size(Text) =< Maximum of
        true -> result(send_frame(Transport, Role, 1, Text));
        false -> error_value('Protocol', <<"invalid or oversized WebSocket text">>)
    end;
send_message(Transport, Role, {'BinaryMessage', {'Binary', Data}}, Maximum)
  when byte_size(Data) =< Maximum -> result(send_frame(Transport, Role, 2, Data));
send_message(Transport, Role, {'CloseMessage', Code, Reason}, _)
  when is_integer(Code), is_binary(Reason), byte_size(Reason) =< 123 ->
    case valid_close_code(Code) andalso valid_utf8(Reason) of
        true -> result(send_frame(Transport, Role, 8, <<Code:16/big, Reason/binary>>));
        false -> error_value('Protocol', <<"invalid WebSocket close message">>)
    end;
send_message(_, _, _, _) -> error_value('Protocol', <<"invalid or oversized WebSocket message">>).

receive_message(Transport, Role, Buffer, Fragment, Maximum) ->
    case read_frame(Transport, Role, Buffer, Maximum) of
        {ok, Fin, Opcode, Payload, Rest} ->
            case handle_frame(Transport, Role, Fin, Opcode, Payload, Rest, Fragment, Maximum) of
                {continue_receive, NextBuffer, NextFragment, continue} ->
                    receive_message(Transport, Role, NextBuffer, NextFragment, Maximum);
                Result -> Result
            end;
        {error, Error} -> {Error, <<>>, none, stop}
    end.

handle_frame(Transport, Role, true, 9, Payload, Rest, Fragment, Maximum) ->
    case send_frame(Transport, Role, 10, Payload) of
        ok -> receive_message(Transport, Role, Rest, Fragment, Maximum);
        {error, Reason} -> {native_error('Closed', Reason), Rest, Fragment, stop}
    end;
handle_frame(Transport, Role, true, 10, _, Rest, Fragment, Maximum) ->
    receive_message(Transport, Role, Rest, Fragment, Maximum);
handle_frame(Transport, Role, true, 8, Payload, Rest, _, _) ->
    case close_payload(Payload) of
        {ok, Code, Reason} ->
            _ = send_frame(Transport, Role, 8, Payload),
            {{'Ok', {'CloseMessage', Code, Reason}}, Rest, none, stop};
        error ->
            _ = send_frame(Transport, Role, 8, <<1002:16/big>>),
            {error_value('Protocol', <<"invalid WebSocket close frame">>), Rest, none, stop}
    end;
handle_frame(_, _, true, 1, Payload, Rest, none, _) ->
    case valid_utf8(Payload) of
        true -> {{'Ok', {'Text', Payload}}, Rest, none, continue};
        false -> {error_value('Protocol', <<"invalid WebSocket UTF-8">>), Rest, none, stop}
    end;
handle_frame(_, _, true, 2, Payload, Rest, none, _) ->
    {{'Ok', {'BinaryMessage', {'Binary', Payload}}}, Rest, none, continue};
handle_frame(_, _, false, Opcode, Payload, Rest, none, _)
  when Opcode =:= 1; Opcode =:= 2 ->
    {continue_receive, Rest, {Opcode, Payload}, continue};
handle_frame(_, _, Fin, 0, Payload, Rest, {Opcode, Acc}, Maximum) ->
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
handle_frame(_, _, _, _, _, Rest, _, _) ->
    {error_value('Protocol', <<"invalid WebSocket fragmentation">>), Rest, none, stop}.

% RFC 6455 masking is directional: a client MUST mask every frame it sends
% and MUST reject an unmasked one from the server; a server does the exact
% opposite. `Role` picks which side of that this process is playing.
read_frame(Transport, Role, Buffer0, Maximum) ->
    ExpectedMask = mask_bit(Role),
    case take(Transport, Buffer0, 2) of
        {ok, <<FinBit:1, Rsv:3, Opcode:4, Mask:1, LengthCode:7>>, Buffer1}
          when Rsv =:= 0, Mask =:= ExpectedMask,
               (Opcode =:= 0 orelse Opcode =:= 1 orelse Opcode =:= 2 orelse
                Opcode =:= 8 orelse Opcode =:= 9 orelse Opcode =:= 10) ->
            case frame_length(Transport, Buffer1, LengthCode) of
                {ok, Length, Buffer2} when Length =< Maximum,
                                           not (Opcode >= 8 andalso (FinBit =:= 0 orelse Length > 125)) ->
                    case read_payload(Transport, Role, Buffer2, Length) of
                        {ok, Payload, Rest} -> {ok, FinBit =:= 1, Opcode, Payload, Rest};
                        {error, Reason} -> {error, native_error('Closed', Reason)}
                    end;
                {ok, _, _} -> {error, error_value('Limit', <<"WebSocket frame exceeds limit">>)};
                {error, Reason} -> {error, native_error('Closed', Reason)}
            end;
        {ok, _, _} -> {error, error_value('Protocol', <<"invalid WebSocket frame header">>)};
        {error, Reason} -> {error, native_error('Closed', Reason)}
    end.

mask_bit(client) -> 0;
mask_bit(server) -> 1.

read_payload(Transport, client, Buffer, Length) -> take(Transport, Buffer, Length);
read_payload(Transport, server, Buffer, Length) ->
    case take(Transport, Buffer, 4 + Length) of
        {ok, <<MaskKey:4/binary, Masked/binary>>, Rest} -> {ok, mask(Masked, MaskKey), Rest};
        Error -> Error
    end.

frame_length(_, Buffer, Code) when Code < 126 -> {ok, Code, Buffer};
frame_length(Transport, Buffer, 126) ->
    case take(Transport, Buffer, 2) of
        {ok, <<Length:16/big>>, Rest} when Length >= 126 -> {ok, Length, Rest};
        {ok, _, _} -> {error, invalid_length};
        Error -> Error
    end;
frame_length(Transport, Buffer, 127) ->
    case take(Transport, Buffer, 8) of
        {ok, <<0:1, Length:63/big>>, Rest} when Length >= 65536 -> {ok, Length, Rest};
        {ok, _, _} -> {error, invalid_length};
        Error -> Error
    end.

send_frame(Transport, client, Opcode, Payload) ->
    Mask = crypto:strong_rand_bytes(4), Masked = mask(Payload, Mask),
    transport_send(Transport, [frame_header(Opcode, 1, byte_size(Payload)), Mask, Masked]);
send_frame(Transport, server, Opcode, Payload) ->
    transport_send(Transport, [frame_header(Opcode, 0, byte_size(Payload)), Payload]).

frame_header(Opcode, MaskBit, Length) when Length < 126 -> <<1:1, 0:3, Opcode:4, MaskBit:1, Length:7>>;
frame_header(Opcode, MaskBit, Length) when Length =< 65535 -> <<1:1, 0:3, Opcode:4, MaskBit:1, 126:7, Length:16/big>>;
frame_header(Opcode, MaskBit, Length) -> <<1:1, 0:3, Opcode:4, MaskBit:1, 127:7, 0:1, Length:63/big>>.

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
    case binary:split(Block, <<"\r\n">>, [global]) of
        [Status | Lines] ->
            case parse_headers(Lines, []) of
                {ok, Headers} -> validate_handshake_values(Status, Headers, Key, Protocols, Buffered);
                error -> {error, error_value('Protocol', <<"invalid WebSocket handshake headers">>)}
            end;
        _ -> {error, error_value('Protocol', <<"invalid WebSocket handshake response">>)}
    end.

validate_handshake_values(Status, Headers, Key, Protocols, Buffered) ->
    Expected = accept_value(Key),
    Accepts = header_values(<<"sec-websocket-accept">>, Headers),
    SelectedValues = header_values(<<"sec-websocket-protocol">>, Headers),
    Selected = case SelectedValues of [Value] -> Value; [] -> <<>>; _ -> invalid end,
    Valid = binary:match(Status, <<"HTTP/1.1 101 ">>) =:= {0, 13} andalso
            Accepts =:= [Expected] andalso
            headers_have_token(<<"upgrade">>, Headers, <<"websocket">>) andalso
            headers_have_token(<<"connection">>, Headers, <<"upgrade">>) andalso
            header_values(<<"sec-websocket-extensions">>, Headers) =:= [] andalso
            (Selected =:= <<>> orelse lists:member(Selected, Protocols)),
    case Valid of
        true -> {ok, case Selected of <<>> -> undefined; _ -> Selected end, Buffered};
        false -> {error, error_value('Protocol', <<"invalid WebSocket handshake response">>)}
    end.

parse_url(URL) ->
    try
        Parts = uri_string:parse(URL), Scheme = maps:get(scheme, Parts),
        Host = maps:get(host, Parts), Port = maps:get(port, Parts, default_port(Scheme)),
        true = (Scheme =:= <<"ws">> orelse Scheme =:= <<"wss">>),
        false = maps:is_key(fragment, Parts),
        false = maps:is_key(userinfo, Parts),
        Path = case maps:get(path, Parts, <<>>) of <<>> -> <<"/">>; Value -> Value end,
        Target = case maps:find(query, Parts) of {ok, Query} -> <<Path/binary, "?", Query/binary>>; error -> Path end,
        AuthorityHost = authority_host(Host),
        HostHeader = case Port =:= default_port(Scheme) of true -> AuthorityHost; false -> <<AuthorityHost/binary, ":", (integer_to_binary(Port))/binary>> end,
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
recv_until(Transport, Acc, Marker, Limit) ->
    case binary:match(Acc, Marker) of
        {Position, Size} when Position =< Limit -> {ok, binary:part(Acc, 0, Position), binary:part(Acc, Position + Size, byte_size(Acc) - Position - Size)};
        {_, _} -> {error, header_limit};
        nomatch when byte_size(Acc) > Limit -> {error, header_limit};
        nomatch -> case transport_recv(Transport, 0) of {ok, Data} -> recv_until(Transport, <<Acc/binary, Data/binary>>, Marker, Limit); Error -> Error end
    end.
parse_headers([], Acc) -> {ok, lists:reverse(Acc)};
parse_headers(_, Acc) when length(Acc) >= 100 -> error;
parse_headers([Line | Rest], Acc) ->
    case binary:split(Line, <<":">>) of
        [Name, Value] when Name =/= <<>> ->
            Trimmed = string:trim(Value),
            case valid_header(Name, Trimmed) of
                true -> parse_headers(Rest, [{lower(Name), Trimmed} | Acc]);
                false -> error
            end;
        _ -> error
    end.
header_values(Name, Headers) -> [V || {K,V} <- Headers, K =:= Name].
valid_protocol(Value) when is_binary(Value), byte_size(Value) > 0 ->
    lists:all(fun(C) -> C > 32 andalso C < 127 andalso not lists:member(C, "()<>@,;:\\\"/[]?={} ") end, binary:bin_to_list(Value));
valid_protocol(_) -> false.
valid_header(Name, Value) -> valid_protocol(Name) andalso
    lists:all(fun(C) -> C =:= 9 orelse C >= 32 andalso C =/= 127 end,
              binary:bin_to_list(Value)).
valid_utf8(Text) -> case unicode:characters_to_list(Text) of Value when is_list(Value) -> true; _ -> false end.
valid_close_code(Code) -> Code >= 1000 andalso Code < 5000 andalso not lists:member(Code, [1004,1005,1006,1015]).
headers_have_token(Name, Headers, Token) ->
    lists:member(Token, [string:trim(Part) || Value <- header_values(Name, Headers),
                                             Part <- binary:split(lower(Value), <<",">>, [global])]).
authority_host(Host) ->
    case binary:match(Host, <<":">>) of nomatch -> Host; _ -> <<"[", Host/binary, "]">> end.
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
