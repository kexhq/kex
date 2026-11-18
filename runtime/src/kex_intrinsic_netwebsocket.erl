-module(kex_intrinsic_netwebsocket).
-export([connect/2, send/2, receiveMessage/1, receiveMessageWithin/2, session/1,
         close/1, 'closed?'/1, upgrade/2, accept/4]).

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

% A server connection carries its socket (`Direct`), so any process holding it
% writes a frame straight to the socket: one gen_tcp:send, which the port
% never interleaves with another, rather than a round trip through the
% connection process (kexhq/kex#670). Only a connection on another node, where
% the port isn't reachable, goes through the process, as a client's always
% does.
send({'Net.HTTP.WebSocket.Connection', Pid, Direct = {Socket, _, _}}, Message)
  when node(Socket) =:= node() -> direct_send(Pid, Direct, Message);
send({'Net.HTTP.WebSocket.Connection', Pid, _}, Message) -> call(Pid, {send, Message});
send({'Net.HTTP.WebSocket.Connection', Pid}, Message) -> call(Pid, {send, Message});
send(_, _) -> error_value('Parse', <<"invalid WebSocket connection">>).
% Unlike send/session/close, there is nothing to bound here: `receiveMessage`
% is supposed to block until the peer speaks, which can legitimately be a
% long time for an idle-but-connected client (kexhq/kex#381 — the 31000ms
% budget `call/2` gives every OTHER operation used to also cap this one,
% reporting a live, silent peer as a `Timeout` failure after 31s and closing
% the connection out from under it). A real disconnect still reaches the
% caller through `{tcp_closed, ...}`/`{tcp_error, ...}`, both of which
% already answer a pending receive with a `Closed` error, so waiting forever
% here costs nothing when the peer actually goes away.
% The request carries its own deadline (`infinity`, or a millisecond bound)
% so `server_loop` can enforce it too, not just the caller's own `call/3`
% wait — see that clause's own comment for why the caller alone is not
% enough once a receive can be bounded at all.
receiveMessage({'Net.HTTP.WebSocket.Connection', Pid, _}) -> call(Pid, {receive_message, infinity}, infinity);
receiveMessage({'Net.HTTP.WebSocket.Connection', Pid}) -> call(Pid, {receive_message, infinity}, infinity);
receiveMessage(_) -> error_value('Parse', <<"invalid WebSocket connection">>).
% An explicit, caller-chosen deadline (kexhq/kex#381) — `None` asks for the
% exact same unbounded wait `receiveMessage/1` gives by default, spelled out
% rather than left implicit in omitting an argument, and `Just(duration)`
% asks for a real bound.
%
% The connection process alone enforces that bound and answers `Timeout`
% itself, so the caller waits for its answer rather than timing out on its
% own. With two clocks the caller's always fired first (the process arms
% its timer only once the request reaches it), and a message landing in
% between went to a request nobody was waiting on any more — lost, and the
% handler's next receive never saw it.
receiveMessageWithin({'Net.HTTP.WebSocket.Connection', Pid, _}, Timeout) ->
    receiveMessageWithin({'Net.HTTP.WebSocket.Connection', Pid}, Timeout);
receiveMessageWithin({'Net.HTTP.WebSocket.Connection', Pid}, 'None') ->
    call(Pid, {receive_message, infinity}, infinity);
receiveMessageWithin({'Net.HTTP.WebSocket.Connection', Pid}, {'Just', {'Measure', Seconds, _, _}})
  when is_number(Seconds), Seconds >= 0 ->
    call(Pid, {receive_message, timeout_ms(Seconds)}, infinity);
receiveMessageWithin(_, _) -> error_value('Parse', <<"invalid WebSocket receive timeout">>).
session({'Net.HTTP.WebSocket.Connection', _, {_, _, Selected}}) ->
    {'Net.HTTP.WebSocket.Session', option(Selected)};
session({'Net.HTTP.WebSocket.Connection', Pid}) ->
    case call(Pid, session) of
        {'Error', _} -> {'Net.HTTP.WebSocket.Session', 'None'};
        Value -> Value
    end.
close({'Net.HTTP.WebSocket.Connection', Pid, _}) -> close({'Net.HTTP.WebSocket.Connection', Pid});
close({'Net.HTTP.WebSocket.Connection', Pid}) ->
    case is_process_alive(Pid) of
        true -> _ = call(Pid, close), 'Kex.Unit';
        false -> 'Kex.Unit'
    end.
% A server connection is closed once either side has closed it, which its
% flag says without asking the process; it used to read open until the
% handler returned.
'closed?'({'Net.HTTP.WebSocket.Connection', Pid, {_, Flags, _}}) ->
    atomics:get(Flags, 1) =:= 1 orelse not is_process_alive(Pid);
'closed?'({'Net.HTTP.WebSocket.Connection', Pid}) -> not is_process_alive(Pid).

direct_send(Pid, {Socket, Flags, _}, Message) ->
    case atomics:get(Flags, 1) of
        1 -> error_value('Closed', <<"WebSocket is closed">>);
        _ ->
            case send_message({tcp, Socket}, server, Message, ?DEFAULT_MAX_MESSAGE) of
                Error = {'Error', {'Net.NetError', 'Closed', _, _, _, _}} ->
                    %% The socket failed under us: the connection process
                    %% closes it and answers its waiting receivers.
                    Pid ! send_failed,
                    Error;
                Reply -> Reply
            end
    end.

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
% are on the wire. This process, already the socket's owner as the server's
% per-connection worker, becomes the connection process, and a fresh process
% runs the application's handler, so the handler can wait on this one without
% deadlocking on itself. Returns once the handler has returned (or crashed),
% which is what lets the server's bookkeeping and graceful shutdown keep
% working unchanged.
%
% The handler and anyone it hands the connection to get `{Socket, Flags,
% Selected}` with it: `send` writes frames straight to the socket, and
% `session` and `closed?` read it, without a round trip through here. `Flags`
% is set to 1 once the connection is closed, before any close frame is
% written, so a send after it is refused rather than following the close.
accept(Socket, Buffered, HandlerFun, Selected) ->
    %% A send blocked on a peer that stopped reading gives up after 30 s and
    %% closes the socket, as the 31 s call that used to carry it did.
    _ = inet:setopts(Socket, [{send_timeout, 30000}, {send_timeout_close, true}]),
    Flags = atomics:new(1, []),
    Connection = {'Net.HTTP.WebSocket.Connection', self(), {Socket, Flags, Selected}},
    HandlerPid = spawn(fun() -> _ = HandlerFun(Connection), ok end),
    Monitor = erlang:monitor(process, HandlerPid),
    State = #{transport => {tcp, Socket}, buffer => Buffered, fragment => none,
              maximum => ?DEFAULT_MAX_MESSAGE, selected => Selected, flags => Flags,
              handler => HandlerPid, monitor => Monitor, closed => false,
              reading => true, armed => false,
              inbox => queue:new(), inbox_bytes => 0, receivers => queue:new()},
    server_loop(arm(drain(State))).

% The connection process reads the socket whenever its inbox has room, not
% only while somebody is receiving, so a ping is answered and a peer's close
% noticed while the handler is busy elsewhere. Frames are parsed as they
% arrive: control frames are handled here, and complete messages (or the error
% that ended reading) queue in `inbox` until a `receive_message` takes them.
%
% Receivers queue too, so two processes receiving at once are both answered
% in turn. A bounded receive's deadline is kept by this process alone
% (`{receive_timeout, Ref}`), never by the caller's own clock: with two clocks
% the caller's fired first, and a message arriving in between went to a
% request nobody was waiting on any more (kexhq/kex#381, #414). Data that
% arrives after a `Timeout` simply waits in the inbox for the next receive.
server_loop(State) ->
    receive
        {call, From, Ref, {receive_message, Wait}} ->
            server_loop(take_receiver(From, Ref, Wait, State));
        {receive_timeout, Ref} ->
            server_loop(expire_receiver(Ref, State));
        {tcp, _Socket, Data} ->
            #{buffer := Buffer} = State,
            server_loop(arm(deliver(drain(State#{armed := false, buffer := <<Buffer/binary, Data/binary>>}))));
        {tcp_closed, _Socket} ->
            server_loop(lost(native_error('Closed', closed), State#{armed := false}));
        {tcp_error, _Socket, Reason} ->
            transport_close(maps:get(transport, State)),
            server_loop(lost(native_error('Closed', Reason), State#{armed := false}));
        send_failed ->
            case State of
                #{closed := true} -> server_loop(State);
                #{transport := Transport} ->
                    transport_close(Transport),
                    server_loop(lost(error_value('Closed', <<"WebSocket is closed">>), State))
            end;
        {call, From, Ref, {send, _}} when map_get(closed, State) ->
            From ! {Ref, error_value('Closed', <<"WebSocket is closed">>)},
            server_loop(State);
        {call, From, Ref, {send, Message}} ->
            #{transport := Transport, maximum := Maximum} = State,
            Reply = send_message(Transport, server, Message, Maximum),
            From ! {Ref, Reply},
            case Reply of
                {'Error', _} ->
                    transport_close(Transport),
                    server_loop(lost(error_value('Closed', <<"WebSocket is closed">>), State));
                _ -> server_loop(State)
            end;
        {call, From, Ref, session} ->
            From ! {Ref, {'Net.HTTP.WebSocket.Session', option(maps:get(selected, State))}},
            server_loop(State);
        {call, From, Ref, close} ->
            State1 = close_once(State),
            From ! {Ref, 'Kex.Unit'},
            %% What the peer sent before the close is not read any more.
            server_loop(lost(error_value('Closed', <<"WebSocket is closed">>),
                             State1#{inbox := queue:new(), inbox_bytes := 0}));
        {'DOWN', Monitor, process, HandlerPid, Reason}
          when Monitor =:= map_get(monitor, State), HandlerPid =:= map_get(handler, State) ->
            _ = close_once(State),
            answer_receivers(error_value('Closed', <<"WebSocket is closed">>), maps:get(receivers, State)),
            case Reason of normal -> ok; _ -> error end
    end.

%% A receive is answered from the inbox if it can be, refused if the
%% connection is closed and nothing is left to read, and otherwise waits.
take_receiver(From, Ref, Wait, State = #{receivers := Receivers}) ->
    case answer_one(From, Ref, State) of
        {answered, State1} -> arm(State1);
        waiting ->
            arm_receive_deadline(Wait, Ref),
            State#{receivers := queue:in({From, Ref}, Receivers)}
    end.

answer_one(From, Ref, State = #{inbox := Inbox, inbox_bytes := Bytes, closed := Closed}) ->
    case queue:out(Inbox) of
        {{value, {Reply, Size}}, Inbox1} ->
            From ! {Ref, Reply},
            {answered, State#{inbox := Inbox1, inbox_bytes := Bytes - Size}};
        {empty, _} when Closed ->
            From ! {Ref, error_value('Closed', <<"WebSocket is closed">>)},
            {answered, State};
        {empty, _} -> waiting
    end.

expire_receiver(Ref, State = #{receivers := Receivers}) ->
    case lists:keytake(Ref, 2, queue:to_list(Receivers)) of
        {value, {From, Ref}, Rest} ->
            From ! {Ref, error_value('Timeout', <<"WebSocket operation timed out">>)},
            State#{receivers := queue:from_list(Rest)};
        %% Already answered: the timer outlived its request.
        false -> State
    end.

%% Hands queued messages to waiting receivers, oldest to oldest; once the
%% connection is closed and nothing is left, the rest are told `Closed`.
deliver(State) ->
    State1 = hand_out(State),
    case State1 of
        #{closed := true, inbox_bytes := 0, receivers := Receivers} ->
            case queue:is_empty(maps:get(inbox, State1)) of
                true ->
                    answer_receivers(error_value('Closed', <<"WebSocket is closed">>), Receivers),
                    State1#{receivers := queue:new()};
                false -> State1
            end;
        _ -> State1
    end.

hand_out(State = #{receivers := Receivers, inbox := Inbox, inbox_bytes := Bytes}) ->
    case {queue:out(Receivers), queue:out(Inbox)} of
        {{{value, {From, Ref}}, Rest}, {{value, {Reply, Size}}, Inbox1}} ->
            From ! {Ref, Reply},
            hand_out(State#{receivers := Rest, inbox := Inbox1, inbox_bytes := Bytes - Size});
        _ -> State
    end.

%% Parses every complete frame in the buffer. A ping is answered and a pong
%% dropped (handle_frame); a message or the error that ends reading is queued.
drain(State = #{reading := false}) -> State;
drain(State = #{transport := Transport, buffer := Buffer, fragment := Fragment, maximum := Maximum}) ->
    case read_frame_buffered(server, Buffer, Maximum) of
        need_more -> State;
        {error, Error} -> stop_reading(Error, State#{buffer := <<>>});
        {ok, Fin, Opcode, Payload, Rest} ->
            case handle_frame(Transport, server, Fin, Opcode, Payload, Rest, Fragment, Maximum) of
                {continue_receive, Rest1, Fragment1, continue} ->
                    drain(State#{buffer := Rest1, fragment := Fragment1});
                {Reply, Rest1, Fragment1, continue} ->
                    drain(queue_reply(Reply, State#{buffer := Rest1, fragment := Fragment1}));
                {Reply, Rest1, _, stop} ->
                    stop_reading(Reply, State#{buffer := Rest1, fragment := none})
            end
    end.

queue_reply(Reply, State = #{inbox := Inbox, inbox_bytes := Bytes}) ->
    Size = reply_size(Reply),
    State#{inbox := queue:in({Reply, Size}, Inbox), inbox_bytes := Bytes + Size}.

reply_size({'Ok', {'Text', Text}}) -> byte_size(Text);
reply_size({'Ok', {'BinaryMessage', {'Binary', Data}}}) -> byte_size(Data);
reply_size(_) -> 0.

%% The peer's close (returned once, as a CloseMessage) or a protocol error
%% ends reading: it is the last thing a receive gets before `Closed`.
stop_reading(Reply, State) ->
    set_closed(queue_reply(Reply, State#{reading := false})).

%% The transport is gone (or failing): queued messages are still handed out,
%% receivers left waiting get `Error`, and later receives `Closed`.
lost(Error, State) ->
    State1 = hand_out(set_closed(State#{reading := false})),
    answer_receivers(Error, maps:get(receivers, State1)),
    State1#{receivers := queue:new()}.

answer_receivers(Error, Receivers) ->
    lists:foreach(fun({From, Ref}) -> From ! {Ref, Error} end, queue:to_list(Receivers)).

%% Sends this side's normal close frame, unless the connection already closed.
close_once(State = #{closed := true}) -> State;
close_once(State = #{transport := Transport}) ->
    State1 = set_closed(State),
    _ = send_frame(Transport, server, 8, <<1000:16/big>>),
    transport_close(Transport),
    State1#{reading := false}.

set_closed(State = #{flags := Flags}) ->
    atomics:put(Flags, 1, 1),
    State#{closed := true}.

%% Asks for the next chunk of the socket while reading goes on and the inbox
%% has room: past `maximum` bytes queued and unread, the peer waits.
arm(State = #{armed := false, reading := true, inbox_bytes := Bytes, maximum := Maximum,
              transport := {tcp, Socket}}) when Bytes < Maximum ->
    case inet:setopts(Socket, [{active, once}]) of
        ok -> State#{armed := true};
        {error, _} -> State
    end;
arm(State) -> State.

% No deadline at all needs no timer; a real one schedules this process's own
% `{receive_timeout, Ref}` to itself (see `server_loop`).
arm_receive_deadline(infinity, _Ref) -> ok;
arm_receive_deadline(Wait, Ref) -> erlang:send_after(Wait, self(), {receive_timeout, Ref}), ok.

read_frame_buffered(Role, Buffer0, Maximum) ->
    ExpectedMask = mask_bit(Role),
    case take_buffered(Buffer0, 2) of
        need_more -> need_more;
        {ok, <<FinBit:1, Rsv:3, Opcode:4, Mask:1, LengthCode:7>>, Buffer1} ->
            ValidHeader = Rsv =:= 0 andalso Mask =:= ExpectedMask andalso
                          (Opcode =:= 0 orelse Opcode =:= 1 orelse Opcode =:= 2 orelse
                           Opcode =:= 8 orelse Opcode =:= 9 orelse Opcode =:= 10),
            case ValidHeader of
                false -> {error, error_value('Protocol', <<"invalid WebSocket frame header">>)};
                true ->
                    case frame_length_buffered(Buffer1, LengthCode) of
                        need_more -> need_more;
                        {error, Reason} -> {error, native_error('Closed', Reason)};
                        {ok, Length, Buffer2} ->
                            case Length =< Maximum andalso
                                 not (Opcode >= 8 andalso (FinBit =:= 0 orelse Length > 125)) of
                                false -> {error, error_value('Limit', <<"WebSocket frame exceeds limit">>)};
                                true ->
                                    case read_payload_buffered(Role, Buffer2, Length) of
                                        need_more -> need_more;
                                        {ok, Payload, Rest} -> {ok, FinBit =:= 1, Opcode, Payload, Rest}
                                    end
                            end
                    end
            end
    end.

frame_length_buffered(Buffer, Code) when Code < 126 -> {ok, Code, Buffer};
frame_length_buffered(Buffer, 126) ->
    case take_buffered(Buffer, 2) of
        need_more -> need_more;
        {ok, <<Length:16/big>>, Rest} when Length >= 126 -> {ok, Length, Rest};
        {ok, _, _} -> {error, invalid_length}
    end;
frame_length_buffered(Buffer, 127) ->
    case take_buffered(Buffer, 8) of
        need_more -> need_more;
        {ok, <<0:1, Length:63/big>>, Rest} when Length >= 65536 -> {ok, Length, Rest};
        {ok, _, _} -> {error, invalid_length}
    end.

read_payload_buffered(client, Buffer, Length) -> take_buffered(Buffer, Length);
read_payload_buffered(server, Buffer, Length) ->
    case take_buffered(Buffer, 4 + Length) of
        need_more -> need_more;
        {ok, <<MaskKey:4/binary, Masked/binary>>, Rest} -> {ok, mask(Masked, MaskKey), Rest}
    end.

take_buffered(Buffer, Count) when byte_size(Buffer) >= Count ->
    {ok, binary:part(Buffer, 0, Count), binary:part(Buffer, Count, byte_size(Buffer) - Count)};
take_buffered(_, _) -> need_more.

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
        {call, From, Ref, {receive_message, Wait}} ->
            % This loop reads frames with a blocking `gen_tcp:recv` (the
            % server side's `{active, once}`, kexhq/kex#370, was never
            % mirrored here), so a caller's bound is honored by waiting
            % that long for the first bytes: a quiet peer is a `Timeout`
            % that consumed nothing, and once bytes arrive the frame is
            % read as before.
            case await_bytes(Transport, Buffer, Wait) of
                {ok, Ready} ->
                    case receive_message(Transport, client, Ready, Fragment, Maximum) of
                        {Reply, NextBuffer, NextFragment, continue} ->
                            From ! {Ref, Reply},
                            loop(Transport, NextBuffer, NextFragment, Maximum, Selected);
                        {Reply, _, _, stop} ->
                            From ! {Ref, Reply}, transport_close(Transport)
                    end;
                {error, timeout} ->
                    From ! {Ref, native_error(timeout, timeout)},
                    loop(Transport, Buffer, Fragment, Maximum, Selected);
                {error, Reason} ->
                    From ! {Ref, native_error('Closed', Reason)}, transport_close(Transport)
            end;
        {call, From, Ref, session} ->
            From ! {Ref, {'Net.HTTP.WebSocket.Session', option(Selected)}},
            loop(Transport, Buffer, Fragment, Maximum, Selected);
        {call, From, Ref, close} ->
            _ = send_frame(Transport, client, 8, <<1000:16/big>>),
            transport_close(Transport), From ! {Ref, 'Kex.Unit'};
        _ -> loop(Transport, Buffer, Fragment, Maximum, Selected)
    end.

% Buffered bytes, or up to `Wait` for the first new ones. Unbounded (and
% already-buffered) receives go straight to the frame reader.
await_bytes(_, Buffer, infinity) -> {ok, Buffer};
await_bytes(_, Buffer, _) when byte_size(Buffer) > 0 -> {ok, Buffer};
await_bytes(Transport, Buffer, Wait) ->
    case transport_recv(Transport, 0, Wait) of
        {ok, Data} -> {ok, <<Buffer/binary, Data/binary>>};
        Error -> Error
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

% Ping/pong don't produce a user-visible message, so this reports
% "keep reading" the same way an in-progress fragmented message does
% (the `continue_receive` tuple below), rather than recursing into
% `receive_message` directly: the buffered, non-blocking server-side
% reader (`server_try_receive`) drives that tuple through its own loop
% instead, and this way both callers share one control-frame path.
handle_frame(Transport, Role, true, 9, Payload, Rest, Fragment, _Maximum) ->
    case send_frame(Transport, Role, 10, Payload) of
        ok -> {continue_receive, Rest, Fragment, continue};
        {error, Reason} -> {native_error('Closed', Reason), Rest, Fragment, stop}
    end;
handle_frame(_Transport, _Role, true, 10, _, Rest, Fragment, _Maximum) ->
    {continue_receive, Rest, Fragment, continue};
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

%% XOR with the 4-byte key repeated over the payload (RFC 6455 §5.3), as one
%% crypto:exor of two binaries rather than a list walk per byte: it runs on
%% every frame a server receives.
mask(<<>>, _Key) -> <<>>;
mask(Data, Key) ->
    Size = byte_size(Data),
    crypto:exor(Data, binary:part(binary:copy(Key, (Size + 3) div 4), 0, Size)).

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
               {customize_hostname_check,
                [{match_fun, public_key:pkix_verify_hostname_match_fun(https)}]},
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
%% characters_to_binary answers the binary itself for valid UTF-8 and an
%% error or incomplete tuple otherwise, without building a list of it.
valid_utf8(Text) -> is_binary(unicode:characters_to_binary(Text)).
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
transport_recv(Transport, Count) -> transport_recv(Transport, Count, ?TIMEOUT).
transport_recv({tcp, S}, Count, Wait) -> gen_tcp:recv(S, Count, Wait); transport_recv({tls, S}, Count, Wait) -> ssl:recv(S, Count, Wait).
transport_close({tcp, S}) -> gen_tcp:close(S); transport_close({tls, S}) -> ssl:close(S).
% Bounded operations (send/session/close) keep the 31s default; a receive
% waits `infinity` for the connection's own answer (see `receiveMessage`).
% The monitor is what makes that safe: a connection that dies instead of
% answering is `Closed`, never a caller blocked for good.
call(Pid, Message) -> call(Pid, Message, 31000).
%% The monitor's alias is both the reply address and the tag, as gen:call
%% does: one reference instead of two, and once the call is over the alias is
%% gone, so a reply that comes too late is dropped instead of left in the
%% caller's mailbox.
call(Pid, Message, Wait) ->
    Alias = erlang:monitor(process, Pid, [{alias, demonitor}]),
    Pid ! {call, Alias, Alias, Message},
    receive
        {Alias, Value} -> erlang:demonitor(Alias, [flush]), Value;
        {'DOWN', Alias, process, Pid, _} -> error_value('Closed', <<"WebSocket is closed">>)
    after Wait ->
        erlang:demonitor(Alias, [flush]),
        error_value('Timeout', <<"WebSocket operation timed out">>)
    end.
native_error(timeout, _) -> error_value('Timeout', <<"WebSocket operation timed out">>);
native_error(Kind, Reason) -> error_value(Kind, unicode:characters_to_binary(io_lib:format("~p", [Reason]))).
timeout_ms(Seconds) -> max(0, round(Seconds * 1000)).
error_value(Kind, Message) -> {'Error', {'Net.NetError', Kind, 'WebSocketClient', Message, 'None', 'None'}}.
