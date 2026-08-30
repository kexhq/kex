-module(kex_intrinsic_nettcp).
-export([connect/1, connectWith/2, listen/1, listenWith/2, accept/1, acceptWithin/2,
         sendAll/2, receiveChunk/2, receiveChunkWithin/3,
         receiveExactly/2, receiveExactlyWithin/3, receiveUntil/3,
         receiveUntilWithin/4, receiveLine/2, receiveLineWithin/3,
         shutdownWrite/1, close/1, 'closed?'/1,
         localAddress/1, peerAddress/1]).

connect(Endpoint) ->
    connectWith(Endpoint, {'Net.Socket.TCP.ConnectOptions', {'Duration',10.0},
                           true, true, 0, 0}).
connectWith({'Net.Socket.TCP.Endpoint', Host, {'Net.Port', Port}},
            {'Net.Socket.TCP.ConnectOptions', {'Duration',Timeout}, NoDelay,
             KeepAlive, SendBuffer, ReceiveBuffer})
  when is_number(Timeout), Timeout >= 0, is_boolean(NoDelay),
       is_boolean(KeepAlive), is_integer(SendBuffer), SendBuffer >= 0,
       is_integer(ReceiveBuffer), ReceiveBuffer >= 0 ->
    SocketOptions = buffer_options(SendBuffer, ReceiveBuffer,
        [binary, {active,false}, {packet,raw}, {nodelay,NoDelay},
         {keepalive,KeepAlive}]),
    start_owner(fun() ->
        gen_tcp:connect(binary_to_list(Host), Port, SocketOptions,
                        timeout_ms(Timeout))
    end, 'Net.Socket.TCP.TCPConnection', 'Connect');
connectWith(_, _) -> net_error('Parse','Connect',<<"invalid TCP connect options">>).

listen(Endpoint) ->
    listenWith(Endpoint, {'Net.Socket.TCP.ListenOptions',128,true,true,true,0,0}).
listenWith({'Net.Socket.TCP.Endpoint', Host, {'Net.Port', Port}},
           {'Net.Socket.TCP.ListenOptions',Backlog,ReuseAddress,NoDelay,
            KeepAlive,SendBuffer,ReceiveBuffer})
  when is_integer(Backlog), Backlog > 0, is_boolean(ReuseAddress),
       is_boolean(NoDelay), is_boolean(KeepAlive), is_integer(SendBuffer),
       SendBuffer >= 0, is_integer(ReceiveBuffer), ReceiveBuffer >= 0 ->
    start_owner(fun() ->
        case inet:parse_address(binary_to_list(Host)) of
            {ok, IP} -> gen_tcp:listen(Port, buffer_options(SendBuffer,ReceiveBuffer,
                [binary,{active,false},{packet,raw},{reuseaddr,ReuseAddress},
                 {backlog,Backlog},{nodelay,NoDelay},{keepalive,KeepAlive},{ip,IP}]));
            _ -> {error, bad_address}
        end
    end, 'Net.Socket.TCP.TCPListener', 'Connect');
listenWith(_, _) -> net_error('Parse','Connect',<<"invalid TCP listen options">>).

accept({'Net.Socket.TCP.TCPListener', Owner}) -> call(Owner, accept).
acceptWithin({'Net.Socket.TCP.TCPListener',Owner}, {'Duration',Seconds})
  when is_number(Seconds), Seconds >= 0 ->
    Timeout=timeout_ms(Seconds), call(Owner,{accept,Timeout},Timeout+1000);
acceptWithin(_,_) -> net_error('Parse','Connect',<<"invalid TCP accept timeout">>).
sendAll({'Net.Socket.TCP.TCPConnection', Owner}, {'Binary', Data}) -> call(Owner, {send, Data}).
receiveChunk({'Net.Socket.TCP.TCPConnection', Owner}, Limit) -> call(Owner, {recv_chunk, Limit}).
receiveChunkWithin({'Net.Socket.TCP.TCPConnection',Owner},Limit,{'Duration',Seconds})
  when is_number(Seconds), Seconds >= 0 -> Timeout=timeout_ms(Seconds), call(Owner,{recv_chunk,Limit,Timeout},Timeout+1000);
receiveChunkWithin(_,_,_) -> net_error('Parse','TCP',<<"invalid TCP receive timeout">>).
receiveExactly({'Net.Socket.TCP.TCPConnection', Owner}, Count) -> call(Owner, {recv_exact, Count}).
receiveExactlyWithin({'Net.Socket.TCP.TCPConnection',Owner},Count,{'Duration',Seconds})
  when is_number(Seconds), Seconds >= 0 -> Timeout=timeout_ms(Seconds), call(Owner,{recv_exact,Count,Timeout},Timeout+1000);
receiveExactlyWithin(_,_,_) -> net_error('Parse','TCP',<<"invalid TCP receive timeout">>).
receiveUntil({'Net.Socket.TCP.TCPConnection', Owner}, {'Binary', Delimiter}, Limit) ->
    call(Owner, {recv_until, Delimiter, Limit}).
receiveUntilWithin({'Net.Socket.TCP.TCPConnection',Owner},{'Binary',Delimiter},Limit,{'Duration',Seconds})
  when is_number(Seconds), Seconds >= 0 -> Timeout=timeout_ms(Seconds), call(Owner,{recv_until,Delimiter,Limit,Timeout},Timeout+1000);
receiveUntilWithin(_,_,_,_) -> net_error('Parse','TCP',<<"invalid TCP receive timeout">>).
receiveLine({'Net.Socket.TCP.TCPConnection', Owner}, Limit) -> call(Owner, {recv_line, Limit}).
receiveLineWithin({'Net.Socket.TCP.TCPConnection',Owner},Limit,{'Duration',Seconds})
  when is_number(Seconds), Seconds >= 0 -> Timeout=timeout_ms(Seconds), call(Owner,{recv_line,Limit,Timeout},Timeout+1000);
receiveLineWithin(_,_,_) -> net_error('Parse','TCP',<<"invalid TCP receive timeout">>).
shutdownWrite({'Net.Socket.TCP.TCPConnection', Owner}) -> call(Owner, shutdown_write).
close({_, Owner}) when is_pid(Owner) ->
    case is_process_alive(Owner) of true -> Owner ! close; false -> ok end,
    ok.
'closed?'({_, Owner}) when is_pid(Owner) -> not is_process_alive(Owner).
localAddress({_, Owner}) when is_pid(Owner) -> call(Owner, local_address).
peerAddress({'Net.Socket.TCP.TCPConnection', Owner}) -> call(Owner, peer_address);
peerAddress(_) -> net_error('Parse', 'TCP', <<"peer address requires a connection">>).

start_owner(Open, Tag, Operation) ->
    Parent = self(), Ref = make_ref(),
    Pid = spawn(fun() ->
        case Open() of
            {ok, Socket} -> Parent ! {Ref, ok, self()}, owner(Socket, Tag, <<>>);
            {error, Reason} -> Parent ! {Ref, error, Reason}
        end
    end),
    receive
        {Ref, ok, Pid} -> {'Ok', {Tag, Pid}};
        {Ref, error, Reason} -> net_error(classify(Reason), Operation, diagnostic(Reason))
    after 11000 -> exit(Pid, kill), net_error('Timeout', Operation, <<"socket initialization timed out">>) end.

owner(Socket, Tag, Buffer) ->
    receive
        {call, From, Ref, accept} when Tag == 'Net.Socket.TCP.TCPListener' ->
            Reply = case gen_tcp:accept(Socket, 30000) of
                {ok, Accepted} -> handoff(Accepted);
                {error, R} -> net_error(classify(R), 'Connect', diagnostic(R))
            end,
            From ! {Ref, Reply}, owner(Socket, Tag, Buffer);
        {call,From,Ref,{accept,Timeout}} when Tag == 'Net.Socket.TCP.TCPListener' ->
            Reply=case gen_tcp:accept(Socket,Timeout) of
                {ok,Accepted}->handoff(Accepted);
                {error,R}->net_error(classify(R),'Connect',diagnostic(R))
            end,
            From!{Ref,Reply},owner(Socket,Tag,Buffer);
        {call, From, Ref, {send, Data}} ->
            Reply = case gen_tcp:send(Socket, Data) of
                ok -> {'Ok', byte_size(Data)};
                {error, R} -> net_error(classify(R), 'TCP', diagnostic(R))
            end,
            From ! {Ref, Reply}, owner(Socket, Tag, Buffer);
        {call, From, Ref, {recv_chunk, Limit}} ->
            {Reply, Next} = recv_chunk(Socket, Limit, Buffer), From ! {Ref, Reply}, owner(Socket, Tag, Next);
        {call,From,Ref,{recv_chunk,Limit,Timeout}} ->
            {Reply,Next}=recv_chunk(Socket,Limit,Buffer,Timeout),From!{Ref,Reply},owner(Socket,Tag,Next);
        {call, From, Ref, {recv_exact, Count}} ->
            {Reply, Next} = recv_exact(Socket, Count, Buffer), From ! {Ref, Reply}, owner(Socket, Tag, Next);
        {call,From,Ref,{recv_exact,Count,Timeout}} ->
            {Reply,Next}=recv_exact(Socket,Count,Buffer,Timeout),From!{Ref,Reply},owner(Socket,Tag,Next);
        {call, From, Ref, {recv_until, Delimiter, Limit}} ->
            {Reply, Next} = recv_until(Socket, Delimiter, Limit, Buffer), From ! {Ref, Reply}, owner(Socket, Tag, Next);
        {call,From,Ref,{recv_until,Delimiter,Limit,Timeout}} ->
            {Reply,Next}=recv_until(Socket,Delimiter,Limit,Buffer,Timeout),From!{Ref,Reply},owner(Socket,Tag,Next);
        {call, From, Ref, {recv_line, Limit}} ->
            {Reply, Next} = recv_line(Socket, Limit, Buffer), From ! {Ref, Reply}, owner(Socket, Tag, Next);
        {call,From,Ref,{recv_line,Limit,Timeout}} ->
            {Reply,Next}=recv_line(Socket,Limit,Buffer,Timeout),From!{Ref,Reply},owner(Socket,Tag,Next);
        {call, From, Ref, shutdown_write} ->
            Reply = case gen_tcp:shutdown(Socket, write) of ok -> {'Ok', ok}; {error,R} -> net_error(classify(R),'TCP',diagnostic(R)) end,
            From ! {Ref, Reply}, owner(Socket, Tag, Buffer);
        {call, From, Ref, local_address} ->
            From ! {Ref, endpoint_result(inet:sockname(Socket))}, owner(Socket, Tag, Buffer);
        {call, From, Ref, peer_address} ->
            From ! {Ref, endpoint_result(inet:peername(Socket))}, owner(Socket, Tag, Buffer);
        close -> gen_tcp:close(Socket), ok;
        _ -> owner(Socket, Tag, Buffer)
    end.

handoff(Socket) ->
    Ref = make_ref(),
    Pid = spawn(fun() -> receive {Ref, Socket} -> owner(Socket, 'Net.Socket.TCP.TCPConnection', <<>>) end end),
    case gen_tcp:controlling_process(Socket, Pid) of
        ok -> Pid ! {Ref, Socket}, {'Ok', {'Net.Socket.TCP.TCPConnection', Pid}};
        {error, R} -> exit(Pid, kill), gen_tcp:close(Socket), net_error(classify(R), 'Connect', diagnostic(R))
    end.

recv_chunk(_, Limit, Buffer) when not is_integer(Limit); Limit =< 0 ->
    {net_error('Limit', 'TCP', <<"receive limit must be positive">>), Buffer};
recv_chunk(_, Limit, Buffer) when byte_size(Buffer) > 0 ->
    Count = min(Limit, byte_size(Buffer)),
    {{'Ok', {'Binary', binary:part(Buffer, 0, Count)}},
     binary:part(Buffer, Count, byte_size(Buffer) - Count)};
recv_chunk(Socket, Limit, <<>>) -> case gen_tcp:recv(Socket, 0, 30000) of
    {ok, Data} -> recv_chunk(Socket, Limit, Data);
    {error, R} -> {net_error(classify(R), 'TCP', diagnostic(R)), <<>>}
end.
recv_chunk(_,Limit,Buffer,_) when not is_integer(Limit); Limit =< 0 ->
    {net_error('Limit','TCP',<<"receive limit must be positive">>),Buffer};
recv_chunk(_,Limit,Buffer,_) when byte_size(Buffer) > 0 ->
    Count=min(Limit,byte_size(Buffer)),
    {{'Ok',{'Binary',binary:part(Buffer,0,Count)}},binary:part(Buffer,Count,byte_size(Buffer)-Count)};
recv_chunk(Socket,Limit,<<>>,Timeout) -> case gen_tcp:recv(Socket,0,Timeout) of
    {ok,Data}->recv_chunk(Socket,Limit,Data,Timeout);
    {error,R}->{net_error(classify(R),'TCP',diagnostic(R)),<<>>}
end.

endpoint_result({ok, {Address, Port}}) ->
    {'Ok', {'Net.Socket.TCP.Endpoint', iolist_to_binary(inet:ntoa(Address)), {'Net.Port', Port}}};
endpoint_result({error, Reason}) -> net_error(classify(Reason), 'TCP', diagnostic(Reason)).

recv_exact(_, Count, Buffer) when not is_integer(Count); Count < 0 -> {net_error('Limit', 'TCP', <<"receive count must be non-negative">>), Buffer};
recv_exact(_, 0, Buffer) -> {{'Ok', {'Binary', <<>>}}, Buffer};
recv_exact(_, Count, Buffer) when byte_size(Buffer) >= Count ->
    {{'Ok', {'Binary', binary:part(Buffer, 0, Count)}}, binary:part(Buffer, Count, byte_size(Buffer)-Count)};
recv_exact(Socket, Count, Buffer) -> case gen_tcp:recv(Socket, Count-byte_size(Buffer), 30000) of
    {ok, Data} -> {{'Ok', {'Binary', <<Buffer/binary,Data/binary>>}}, <<>>};
    {error, R} -> {net_error(classify(R), 'TCP', diagnostic(R)), Buffer}
end.
recv_exact(_,Count,Buffer,_) when not is_integer(Count); Count < 0 -> {net_error('Limit','TCP',<<"receive count must be non-negative">>),Buffer};
recv_exact(_,0,Buffer,_) -> {{'Ok',{'Binary',<<>>}},Buffer};
recv_exact(_,Count,Buffer,_) when byte_size(Buffer) >= Count ->
    {{'Ok',{'Binary',binary:part(Buffer,0,Count)}},binary:part(Buffer,Count,byte_size(Buffer)-Count)};
recv_exact(Socket,Count,Buffer,Timeout) -> case gen_tcp:recv(Socket,Count-byte_size(Buffer),Timeout) of
    {ok,Data}->{{'Ok',{'Binary',<<Buffer/binary,Data/binary>>}},<<>>};
    {error,R}->{net_error(classify(R),'TCP',diagnostic(R)),Buffer}
end.

recv_line(_, Limit, Buffer) when not is_integer(Limit); Limit =< 0 ->
    {net_error('Limit', 'TCP', <<"line limit must be positive">>), Buffer};
recv_line(Socket, Limit, Buffer) ->
    case binary:match(Buffer, <<"\n">>) of
        {Position, 1} when Position + 1 =< Limit ->
            Count = Position + 1,
            {{'Ok', {'Binary', binary:part(Buffer, 0, Count)}},
             binary:part(Buffer, Count, byte_size(Buffer)-Count)};
        _ when byte_size(Buffer) >= Limit ->
            {{'Error', {'Net.NetError', 'Limit', 'TCP', <<"line exceeds limit">>, 'None', {'Just', Limit}}}, Buffer};
        nomatch -> case gen_tcp:recv(Socket, 0, 30000) of
            {ok, Data} -> recv_line(Socket, Limit, <<Buffer/binary,Data/binary>>);
            {error, R} -> {net_error(classify(R), 'TCP', diagnostic(R)), Buffer}
        end
    end.
recv_line(_,Limit,Buffer,_) when not is_integer(Limit); Limit =< 0 ->
    {net_error('Limit','TCP',<<"line limit must be positive">>),Buffer};
recv_line(Socket,Limit,Buffer,Timeout) ->
    case binary:match(Buffer,<<"\n">>) of
        {Position,1} when Position+1 =< Limit -> Count=Position+1,
            {{'Ok',{'Binary',binary:part(Buffer,0,Count)}},binary:part(Buffer,Count,byte_size(Buffer)-Count)};
        _ when byte_size(Buffer) >= Limit ->
            {{'Error',{'Net.NetError','Limit','TCP',<<"line exceeds limit">>,'None',{'Just',Limit}}},Buffer};
        nomatch -> case gen_tcp:recv(Socket,0,Timeout) of
            {ok,Data}->recv_line(Socket,Limit,<<Buffer/binary,Data/binary>>,Timeout);
            {error,R}->{net_error(classify(R),'TCP',diagnostic(R)),Buffer}
        end
    end.

recv_until(_, Delimiter, _, Buffer) when not is_binary(Delimiter); byte_size(Delimiter) =:= 0 ->
    {net_error('Parse', 'TCP', <<"receive delimiter must not be empty">>), Buffer};
recv_until(_, _, Limit, Buffer) when not is_integer(Limit); Limit =< 0 ->
    {net_error('Limit', 'TCP', <<"receive limit must be positive">>), Buffer};
recv_until(Socket, Delimiter, Limit, Buffer) ->
    case binary:match(Buffer, Delimiter) of
        {Position, Length} when Position + Length =< Limit ->
            Count = Position + Length,
            {{'Ok', {'Binary', binary:part(Buffer, 0, Count)}},
             binary:part(Buffer, Count, byte_size(Buffer) - Count)};
        _ when byte_size(Buffer) >= Limit ->
            {{'Error', {'Net.NetError', 'Limit', 'TCP', <<"delimiter not found within limit">>, 'None', {'Just', Limit}}}, Buffer};
        nomatch ->
            case gen_tcp:recv(Socket, 0, 30000) of
                {ok, Data} -> recv_until(Socket, Delimiter, Limit, <<Buffer/binary, Data/binary>>);
                {error, R} -> {net_error(classify(R), 'TCP', diagnostic(R)), Buffer}
            end
    end.
recv_until(_,Delimiter,_,Buffer,_) when not is_binary(Delimiter); byte_size(Delimiter) =:= 0 ->
    {net_error('Parse','TCP',<<"receive delimiter must not be empty">>),Buffer};
recv_until(_,_,Limit,Buffer,_) when not is_integer(Limit); Limit =< 0 ->
    {net_error('Limit','TCP',<<"receive limit must be positive">>),Buffer};
recv_until(Socket,Delimiter,Limit,Buffer,Timeout) ->
    case binary:match(Buffer,Delimiter) of
        {Position,Length} when Position+Length =< Limit -> Count=Position+Length,
            {{'Ok',{'Binary',binary:part(Buffer,0,Count)}},binary:part(Buffer,Count,byte_size(Buffer)-Count)};
        _ when byte_size(Buffer) >= Limit ->
            {{'Error',{'Net.NetError','Limit','TCP',<<"delimiter not found within limit">>,'None',{'Just',Limit}}},Buffer};
        nomatch -> case gen_tcp:recv(Socket,0,Timeout) of
            {ok,Data}->recv_until(Socket,Delimiter,Limit,<<Buffer/binary,Data/binary>>,Timeout);
            {error,R}->{net_error(classify(R),'TCP',diagnostic(R)),Buffer}
        end
    end.

call(Owner, Request) when is_pid(Owner) ->
    case is_process_alive(Owner) of
        false -> net_error('Closed', 'TCP', <<"socket is closed">>);
        true -> Ref=make_ref(), Mon=erlang:monitor(process,Owner), Owner ! {call,self(),Ref,Request},
            receive {Ref, Reply} -> erlang:demonitor(Mon,[flush]), Reply;
                    {'DOWN',Mon,process,Owner,_} -> net_error('Closed','TCP',<<"socket is closed">>)
            after 31000 -> erlang:demonitor(Mon,[flush]), net_error('Timeout','TCP',<<"socket operation timed out">>) end
    end.
call(Owner,Request,Wait) when is_pid(Owner) ->
    case is_process_alive(Owner) of
        false->net_error('Closed','TCP',<<"socket is closed">>);
        true->Ref=make_ref(),Mon=erlang:monitor(process,Owner),Owner!{call,self(),Ref,Request},
            receive {Ref,Reply}->erlang:demonitor(Mon,[flush]),Reply;
                    {'DOWN',Mon,process,Owner,_}->net_error('Closed','TCP',<<"socket is closed">>)
            after Wait->erlang:demonitor(Mon,[flush]),net_error('Timeout','TCP',<<"socket operation timed out">>) end
    end.

buffer_options(0,0,Options)->Options;
buffer_options(Send,0,Options)->[{sndbuf,Send}|Options];
buffer_options(0,Receive,Options)->[{recbuf,Receive}|Options];
buffer_options(Send,Receive,Options)->[{sndbuf,Send},{recbuf,Receive}|Options].
timeout_ms(Seconds)->max(0,round(Seconds*1000)).

classify(econnrefused) -> 'Connect'; classify(timeout) -> 'Timeout'; classify(closed) -> 'Closed'; classify(_) -> 'Backend'.
diagnostic(Reason) -> unicode:characters_to_binary(io_lib:format("~p", [Reason])).
net_error(Kind, Operation, Message) -> {'Error', {'Net.NetError', Kind, Operation, Message, 'None', 'None'}}.
