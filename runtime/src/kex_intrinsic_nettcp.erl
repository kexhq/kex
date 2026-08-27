-module(kex_intrinsic_nettcp).
-export([connect/1, listen/1, accept/1, sendAll/2, receiveChunk/2,
         receiveExactly/2, receiveLine/2, shutdownWrite/1, close/1, 'closed?'/1]).

connect({'Net.Socket.TCP.Endpoint', Host, {'Net.Port', Port}}) ->
    start_owner(fun() ->
        gen_tcp:connect(binary_to_list(Host), Port,
                        [binary, {active, false}, {packet, raw}, {nodelay, true}], 10000)
    end, 'Net.Socket.TCP.TCPConnection', 'Connect').

listen({'Net.Socket.TCP.Endpoint', Host, {'Net.Port', Port}}) ->
    start_owner(fun() ->
        case inet:parse_address(binary_to_list(Host)) of
            {ok, IP} -> gen_tcp:listen(Port, [binary, {active, false}, {packet, raw},
                                              {reuseaddr, true}, {backlog, 128}, {ip, IP}]);
            _ -> {error, bad_address}
        end
    end, 'Net.Socket.TCP.TCPListener', 'Connect').

accept({'Net.Socket.TCP.TCPListener', Owner}) -> call(Owner, accept).
sendAll({'Net.Socket.TCP.TCPConnection', Owner}, {'Binary', Data}) -> call(Owner, {send, Data}).
receiveChunk({'Net.Socket.TCP.TCPConnection', Owner}, Limit) -> call(Owner, {recv_chunk, Limit}).
receiveExactly({'Net.Socket.TCP.TCPConnection', Owner}, Count) -> call(Owner, {recv_exact, Count}).
receiveLine({'Net.Socket.TCP.TCPConnection', Owner}, Limit) -> call(Owner, {recv_line, Limit}).
shutdownWrite({'Net.Socket.TCP.TCPConnection', Owner}) -> call(Owner, shutdown_write).
close({_, Owner}) when is_pid(Owner) ->
    case is_process_alive(Owner) of true -> Owner ! close; false -> ok end,
    ok.
'closed?'({_, Owner}) when is_pid(Owner) -> not is_process_alive(Owner).

start_owner(Open, Tag, Operation) ->
    Parent = self(), Ref = make_ref(),
    Pid = spawn(fun() ->
        case Open() of
            {ok, Socket} -> Parent ! {Ref, ok, self()}, owner(Socket, Tag);
            {error, Reason} -> Parent ! {Ref, error, Reason}
        end
    end),
    receive
        {Ref, ok, Pid} -> {'Ok', {Tag, Pid}};
        {Ref, error, Reason} -> net_error(classify(Reason), Operation, diagnostic(Reason))
    after 11000 -> exit(Pid, kill), net_error('Timeout', Operation, <<"socket initialization timed out">>) end.

owner(Socket, Tag) ->
    receive
        {call, From, Ref, accept} when Tag == 'Net.Socket.TCP.TCPListener' ->
            Reply = case gen_tcp:accept(Socket, 30000) of
                {ok, Accepted} -> handoff(Accepted);
                {error, R} -> net_error(classify(R), 'Connect', diagnostic(R))
            end,
            From ! {Ref, Reply}, owner(Socket, Tag);
        {call, From, Ref, {send, Data}} ->
            Reply = case gen_tcp:send(Socket, Data) of
                ok -> {'Ok', byte_size(Data)};
                {error, R} -> net_error(classify(R), 'TCP', diagnostic(R))
            end,
            From ! {Ref, Reply}, owner(Socket, Tag);
        {call, From, Ref, {recv_chunk, Limit}} ->
            Reply = recv_chunk(Socket, Limit), From ! {Ref, Reply}, owner(Socket, Tag);
        {call, From, Ref, {recv_exact, Count}} ->
            Reply = recv_exact(Socket, Count), From ! {Ref, Reply}, owner(Socket, Tag);
        {call, From, Ref, {recv_line, Limit}} ->
            Reply = recv_line(Socket, Limit, <<>>), From ! {Ref, Reply}, owner(Socket, Tag);
        {call, From, Ref, shutdown_write} ->
            Reply = case gen_tcp:shutdown(Socket, write) of ok -> {'Ok', ok}; {error,R} -> net_error(classify(R),'TCP',diagnostic(R)) end,
            From ! {Ref, Reply}, owner(Socket, Tag);
        close -> gen_tcp:close(Socket), ok;
        _ -> owner(Socket, Tag)
    end.

handoff(Socket) ->
    Ref = make_ref(),
    Pid = spawn(fun() -> receive {Ref, Socket} -> owner(Socket, 'Net.Socket.TCP.TCPConnection') end end),
    case gen_tcp:controlling_process(Socket, Pid) of
        ok -> Pid ! {Ref, Socket}, {'Ok', {'Net.Socket.TCP.TCPConnection', Pid}};
        {error, R} -> exit(Pid, kill), gen_tcp:close(Socket), net_error(classify(R), 'Connect', diagnostic(R))
    end.

recv_chunk(_, Limit) when not is_integer(Limit); Limit =< 0 -> net_error('Limit', 'TCP', <<"receive limit must be positive">>);
recv_chunk(Socket, Limit) -> case gen_tcp:recv(Socket, 0, 30000) of
    {ok, Data} when byte_size(Data) =< Limit -> {'Ok', {'Binary', Data}};
    {ok, Data} -> {'Error', {'Net.NetError', 'Limit', 'TCP', <<"received chunk exceeds limit">>, 'None', {'Just', byte_size(Data)}}};
    {error, R} -> net_error(classify(R), 'TCP', diagnostic(R))
end.

recv_exact(_, Count) when not is_integer(Count); Count < 0 -> net_error('Limit', 'TCP', <<"receive count must be non-negative">>);
recv_exact(_, 0) -> {'Ok', {'Binary', <<>>}};
recv_exact(Socket, Count) -> case gen_tcp:recv(Socket, Count, 30000) of
    {ok, Data} -> {'Ok', {'Binary', Data}}; {error, R} -> net_error(classify(R), 'TCP', diagnostic(R)) end.

recv_line(_, Limit, Acc) when byte_size(Acc) >= Limit ->
    {'Error', {'Net.NetError', 'Limit', 'TCP', <<"line exceeds limit">>, 'None', {'Just', byte_size(Acc)}}};
recv_line(Socket, Limit, Acc) ->
    case gen_tcp:recv(Socket, 1, 30000) of
        {ok, <<C>>} -> New = <<Acc/binary, C>>, case C of $\n -> {'Ok', {'Binary', New}}; _ -> recv_line(Socket, Limit, New) end;
        {error, R} -> net_error(classify(R), 'TCP', diagnostic(R))
    end.

call(Owner, Request) when is_pid(Owner) ->
    case is_process_alive(Owner) of
        false -> net_error('Closed', 'TCP', <<"socket is closed">>);
        true -> Ref=make_ref(), Mon=erlang:monitor(process,Owner), Owner ! {call,self(),Ref,Request},
            receive {Ref, Reply} -> erlang:demonitor(Mon,[flush]), Reply;
                    {'DOWN',Mon,process,Owner,_} -> net_error('Closed','TCP',<<"socket is closed">>)
            after 31000 -> erlang:demonitor(Mon,[flush]), net_error('Timeout','TCP',<<"socket operation timed out">>) end
    end.

classify(econnrefused) -> 'Connect'; classify(timeout) -> 'Timeout'; classify(closed) -> 'Closed'; classify(_) -> 'Backend'.
diagnostic(Reason) -> unicode:characters_to_binary(io_lib:format("~p", [Reason])).
net_error(Kind, Operation, Message) -> {'Error', {'Net.NetError', Kind, Operation, Message, 'None', 'None'}}.
