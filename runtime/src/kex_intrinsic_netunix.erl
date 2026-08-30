-module(kex_intrinsic_netunix).
-include_lib("kernel/include/file.hrl").
-export([address/1, connect/1, connectWith/2, listen/1, listenWith/2,
         accept/1, sendAll/2, receiveChunk/2,
         receiveExactly/2, receiveUntil/3, receiveLine/2, shutdownWrite/1,
         close/1, 'closed?'/1]).

address(Path) when is_binary(Path), byte_size(Path)>0, byte_size(Path)<104 ->
    case Path of <<"/",_/binary>> -> {'Ok',{'Net.Socket.Unix.Address',Path}}; _ -> error_value('Parse',<<"Unix socket path must be absolute">>) end;
address(_) -> error_value('Parse',<<"invalid Unix socket path">>).

connect(Address) -> connectWith(Address,{'Net.Socket.Unix.ConnectOptions',{'Duration',10.0},{'Duration',30.0}}).
connectWith({'Net.Socket.Unix.Address',Path},{'Net.Socket.Unix.ConnectOptions',
            {'Duration',ConnectTimeout},{'Duration',ReceiveTimeout}})
  when is_number(ConnectTimeout),ConnectTimeout>=0,is_number(ReceiveTimeout),ReceiveTimeout>=0 ->
    ConnectMs=timeout_ms(ConnectTimeout),ReceiveMs=timeout_ms(ReceiveTimeout),
    start(fun()->gen_tcp:connect({local,binary_to_list(Path)},0,[binary,{active,false},{packet,raw}],ConnectMs) end,
          connection,Path,ReceiveMs,30000,ConnectMs+1000);
connectWith(_,_) -> error_value('Parse',<<"invalid Unix connect options">>).
listen(Address) -> listenWith(Address,{'Net.Socket.Unix.ListenOptions',128,false,{'Duration',30.0},{'Duration',30.0}}).
listenWith({'Net.Socket.Unix.Address',Path},{'Net.Socket.Unix.ListenOptions',Backlog,
           RemoveStale,{'Duration',AcceptTimeout},{'Duration',ReceiveTimeout}})
  when is_integer(Backlog),Backlog>0,is_boolean(RemoveStale),
       is_number(AcceptTimeout),AcceptTimeout>=0,is_number(ReceiveTimeout),ReceiveTimeout>=0 ->
    case prepare_path(Path,RemoveStale) of
        ok -> start(fun()->gen_tcp:listen(0,[binary,{active,false},{packet,raw},
                     {backlog,Backlog},{ifaddr,{local,binary_to_list(Path)}}]) end,
                    listener,Path,timeout_ms(ReceiveTimeout),timeout_ms(AcceptTimeout),11000);
        Error -> Error
    end;
listenWith(_,_) -> error_value('Parse',<<"invalid Unix listen options">>).
accept({'Net.Socket.Unix.UnixListener',P})->call(P,accept).
sendAll({'Net.Socket.Unix.UnixConnection',P},{'Binary',D})->call(P,{send,D}).
receiveChunk({'Net.Socket.Unix.UnixConnection',P},L)->call(P,{recv,L}).
receiveExactly({'Net.Socket.Unix.UnixConnection',P},N)->call(P,{recv_exact,N}).
receiveUntil({'Net.Socket.Unix.UnixConnection',P},{'Binary',D},L)->call(P,{recv_until,D,L}).
receiveLine({'Net.Socket.Unix.UnixConnection',P},L)->call(P,{recv_until,<<"\n">>,L}).
shutdownWrite({'Net.Socket.Unix.UnixConnection',P})->call(P,shutdown_write).
close({_,P})->case is_process_alive(P) of true->P!close;false->ok end,ok.
'closed?'({_,P})->not is_process_alive(P).

start(Open,Kind,Path,ReceiveTimeout,AcceptTimeout,Wait)->Parent=self(),Ref=make_ref(),Pid=spawn(fun()->case Open() of {ok,S}->Parent!{Ref,ok,self()},loop(S,Kind,Path,<<>>,ReceiveTimeout,AcceptTimeout);{error,R}->Parent!{Ref,error,R} end end),receive{Ref,ok,Pid}->{'Ok',{tag(Kind),Pid}};{Ref,error,R}->error_value(kind(R),diag(R))after Wait->exit(Pid,kill),error_value('Timeout',<<"Unix socket initialization timed out">>)end.
tag(connection)->'Net.Socket.Unix.UnixConnection';tag(listener)->'Net.Socket.Unix.UnixListener'.
loop(S,Kind,Path,Buffer,ReceiveTimeout,AcceptTimeout)->receive
 {call,F,R,accept}when Kind==listener->Reply=case gen_tcp:accept(S,AcceptTimeout)of{ok,A}->handoff(A,Path,ReceiveTimeout);{error,E}->error_value(kind(E),diag(E))end,F!{R,Reply},loop(S,Kind,Path,Buffer,ReceiveTimeout,AcceptTimeout);
 {call,F,R,{send,D}}->Reply=case gen_tcp:send(S,D)of ok->{'Ok',byte_size(D)};{error,E}->error_value(kind(E),diag(E))end,F!{R,Reply},loop(S,Kind,Path,Buffer,ReceiveTimeout,AcceptTimeout);
 {call,F,R,{recv,L}}->{Reply,Next}=recv_chunk(S,L,Buffer,ReceiveTimeout),F!{R,Reply},loop(S,Kind,Path,Next,ReceiveTimeout,AcceptTimeout);
 {call,F,R,{recv_exact,N}}->{Reply,Next}=recv_exact(S,N,Buffer,ReceiveTimeout),F!{R,Reply},loop(S,Kind,Path,Next,ReceiveTimeout,AcceptTimeout);
 {call,F,R,{recv_until,D,L}}->{Reply,Next}=recv_until(S,D,L,Buffer,ReceiveTimeout),F!{R,Reply},loop(S,Kind,Path,Next,ReceiveTimeout,AcceptTimeout);
 {call,F,R,shutdown_write}->Reply=case gen_tcp:shutdown(S,write)of ok->{'Ok',ok};{error,E}->error_value(kind(E),diag(E))end,F!{R,Reply},loop(S,Kind,Path,Buffer,ReceiveTimeout,AcceptTimeout);
 close->gen_tcp:close(S),case Kind of listener->file:delete(Path);_->ok end,ok;
 _->loop(S,Kind,Path,Buffer,ReceiveTimeout,AcceptTimeout)
end.
recv_chunk(_,L,Buffer,_)when not is_integer(L);L=<0->{error_value('Limit',<<"Unix receive limit must be positive">>),Buffer};
recv_chunk(_,L,Buffer,_)when byte_size(Buffer)>0->N=min(L,byte_size(Buffer)),{{'Ok',{'Binary',binary:part(Buffer,0,N)}},binary:part(Buffer,N,byte_size(Buffer)-N)};
recv_chunk(S,L,<<>>,Timeout)->case gen_tcp:recv(S,0,Timeout)of{ok,D}->recv_chunk(S,L,D,Timeout);{error,E}->{error_value(kind(E),diag(E)),<<>>}end.
recv_exact(_,N,Buffer,_)when not is_integer(N);N<0->{error_value('Limit',<<"Unix receive count must be non-negative">>),Buffer};
recv_exact(_,0,Buffer,_)->{{'Ok',{'Binary',<<>>}},Buffer};
recv_exact(_,N,Buffer,_)when byte_size(Buffer)>=N->{{'Ok',{'Binary',binary:part(Buffer,0,N)}},binary:part(Buffer,N,byte_size(Buffer)-N)};
recv_exact(S,N,Buffer,Timeout)->case gen_tcp:recv(S,N-byte_size(Buffer),Timeout)of{ok,D}->{{'Ok',{'Binary',<<Buffer/binary,D/binary>>}},<<>>};{error,E}->{error_value(kind(E),diag(E)),Buffer}end.
recv_until(_,D,_,Buffer,_)when not is_binary(D);byte_size(D)=:=0->{error_value('Parse',<<"Unix receive delimiter must not be empty">>),Buffer};
recv_until(_,_,L,Buffer,_)when not is_integer(L);L=<0->{error_value('Limit',<<"Unix receive limit must be positive">>),Buffer};
recv_until(S,D,L,Buffer,Timeout)->case binary:match(Buffer,D)of
 {P,N}when P+N=<L->Count=P+N,{{'Ok',{'Binary',binary:part(Buffer,0,Count)}},binary:part(Buffer,Count,byte_size(Buffer)-Count)};
 _ when byte_size(Buffer)>=L->{{'Error',{'Net.NetError','Limit','Unix',<<"delimiter not found within limit">>,'None',{'Just',L}}},Buffer};
 nomatch->case gen_tcp:recv(S,0,Timeout)of{ok,More}->recv_until(S,D,L,<<Buffer/binary,More/binary>>,Timeout);{error,E}->{error_value(kind(E),diag(E)),Buffer}end
end.
handoff(S,Path,ReceiveTimeout)->Ref=make_ref(),P=spawn(fun()->receive{Ref,S}->loop(S,connection,Path,<<>>,ReceiveTimeout,30000)end end),case gen_tcp:controlling_process(S,P)of ok->P!{Ref,S},{'Ok',{'Net.Socket.Unix.UnixConnection',P}};{error,E}->exit(P,kill),gen_tcp:close(S),error_value(kind(E),diag(E))end.
call(P,Q)->case is_process_alive(P)of false->error_value('Closed',<<"Unix socket is closed">>);true->R=make_ref(),P!{call,self(),R,Q},receive{R,V}->V after 31000->error_value('Timeout',<<"Unix socket operation timed out">>)end end.
kind(timeout)->'Timeout';kind(closed)->'Closed';kind(_)->'Backend'.
diag(E)->unicode:characters_to_binary(io_lib:format("~p",[E])).
error_value(K,M)->{'Error',{'Net.NetError',K,'Unix',M,'None','None'}}.

prepare_path(Path,false)->case file:read_link_info(Path) of
    {error,enoent}->ok;
    {ok,_}->error_value('Backend',<<"Unix socket path already exists">>);
    {error,R}->error_value('Backend',diag(R))
end;
prepare_path(Path,true)->case file:read_link_info(Path) of
    {error,enoent}->ok;
    {ok,#file_info{mode=Mode}} when (Mode band 8#170000) =:= 8#140000 ->
        case file:delete(Path) of ok->ok;{error,R}->error_value('Backend',diag(R)) end;
    {ok,_}->error_value('UnsupportedOption',<<"removeStale refuses to remove a non-socket path">>);
    {error,R}->error_value('Backend',diag(R))
end.
timeout_ms(Seconds)->max(0,round(Seconds*1000)).
