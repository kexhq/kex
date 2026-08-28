-module(kex_intrinsic_netudp).
-export([bind/1, sendTo/3, receiveFrom/2, close/1, 'closed?'/1, localAddress/1]).

bind({'Net.Socket.UDP.Endpoint', Host, {'Net.Port', Port}}) ->
    case inet:parse_address(binary_to_list(Host)) of
        {ok, IP} -> start(fun() -> gen_udp:open(Port, [binary, {active,false}, {reuseaddr,true}, {ip,IP}]) end);
        _ -> net_error('Parse', <<"invalid UDP bind address">>)
    end.
sendTo({'Net.Socket.UDP.Socket', Owner}, {'Net.Socket.UDP.Endpoint', Host, {'Net.Port', Port}}, {'Binary', Data}) ->
    call(Owner, {send, Host, Port, Data}).
receiveFrom({'Net.Socket.UDP.Socket', Owner}, Limit) -> call(Owner, {'receive', Limit}).
close({'Net.Socket.UDP.Socket', Owner}) -> case is_process_alive(Owner) of true -> Owner ! close; false -> ok end, ok.
'closed?'({'Net.Socket.UDP.Socket', Owner}) -> not is_process_alive(Owner).
localAddress({'Net.Socket.UDP.Socket', Owner}) -> call(Owner, local_address).

start(Open) -> Parent=self(), Ref=make_ref(), Pid=spawn(fun()->case Open() of {ok,S}->Parent!{Ref,ok,self()},loop(S);{error,R}->Parent!{Ref,error,R} end end),
    receive {Ref,ok,Pid}->{'Ok',{'Net.Socket.UDP.Socket',Pid}}; {Ref,error,R}->net_error('Backend',diag(R)) after 10000->exit(Pid,kill),net_error('Timeout',<<"UDP bind timed out">>) end.
loop(Socket) -> receive
    {call,From,Ref,{send,Host,Port,Data}} ->
        Reply=case gen_udp:send(Socket,binary_to_list(Host),Port,Data) of ok->{'Ok',byte_size(Data)};{error,R}->net_error('Backend',diag(R)) end,
        From!{Ref,Reply},loop(Socket);
    {call,From,Ref,{'receive',Limit}} ->
        Reply=case is_integer(Limit) andalso Limit>0 andalso Limit=<65535 of
            false->net_error('Limit',<<"invalid UDP receive limit">>);
            true->case gen_udp:recv(Socket,65535,30000) of
                {ok,{_,_,Data}} when byte_size(Data)>Limit->{'Error',{'Net.NetError','Limit','UDP',<<"datagram exceeds receive limit">>,'None',{'Just',byte_size(Data)}}};
                {ok,{IP,Port,Data}}->{'Ok',{'Net.Socket.UDP.Datagram',{'Net.Socket.UDP.Endpoint',iolist_to_binary(inet:ntoa(IP)),{'Net.Port',Port}},{'Binary',Data}}};
                {error,R}->net_error(case R of timeout->'Timeout';closed->'Closed';_->'Backend' end,diag(R)) end end,
        From!{Ref,Reply},loop(Socket);
    {call,From,Ref,local_address} ->
        Reply=case inet:sockname(Socket) of
            {ok,{IP,Port}}->{'Ok',{'Net.Socket.UDP.Endpoint',iolist_to_binary(inet:ntoa(IP)),{'Net.Port',Port}}};
            {error,R}->net_error('Backend',diag(R))
        end,
        From!{Ref,Reply},loop(Socket);
    close -> gen_udp:close(Socket),ok;
    _ -> loop(Socket)
end.
call(Owner,Req)->case is_process_alive(Owner) of false->net_error('Closed',<<"UDP socket is closed">>);true->Ref=make_ref(),Owner!{call,self(),Ref,Req},receive{Ref,R}->R after 31000->net_error('Timeout',<<"UDP operation timed out">>)end end.
diag(R)->unicode:characters_to_binary(io_lib:format("~p",[R])).
net_error(K,M)->{'Error',{'Net.NetError',K,'UDP',M,'None','None'}}.
