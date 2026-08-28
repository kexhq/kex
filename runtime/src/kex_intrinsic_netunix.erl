-module(kex_intrinsic_netunix).
-export([address/1, connect/1, listen/1, accept/1, sendAll/2, receiveChunk/2,
         receiveExactly/2, receiveUntil/3, receiveLine/2, shutdownWrite/1,
         close/1, 'closed?'/1]).

address(Path) when is_binary(Path), byte_size(Path)>0, byte_size(Path)<104 ->
    case Path of <<"/",_/binary>> -> {'Ok',{'Net.Socket.Unix.Address',Path}}; _ -> error_value('Parse',<<"Unix socket path must be absolute">>) end;
address(_) -> error_value('Parse',<<"invalid Unix socket path">>).

connect({'Net.Socket.Unix.Address',Path}) -> start(fun()->gen_tcp:connect({local,binary_to_list(Path)},0,[binary,{active,false},{packet,raw}],10000) end,connection,Path).
listen({'Net.Socket.Unix.Address',Path}) -> start(fun()->gen_tcp:listen(0,[binary,{active,false},{packet,raw},{ifaddr,{local,binary_to_list(Path)}}]) end,listener,Path).
accept({'Net.Socket.Unix.UnixListener',P})->call(P,accept).
sendAll({'Net.Socket.Unix.UnixConnection',P},{'Binary',D})->call(P,{send,D}).
receiveChunk({'Net.Socket.Unix.UnixConnection',P},L)->call(P,{recv,L}).
receiveExactly({'Net.Socket.Unix.UnixConnection',P},N)->call(P,{recv_exact,N}).
receiveUntil({'Net.Socket.Unix.UnixConnection',P},{'Binary',D},L)->call(P,{recv_until,D,L}).
receiveLine({'Net.Socket.Unix.UnixConnection',P},L)->call(P,{recv_until,<<"\n">>,L}).
shutdownWrite({'Net.Socket.Unix.UnixConnection',P})->call(P,shutdown_write).
close({_,P})->case is_process_alive(P) of true->P!close;false->ok end,ok.
'closed?'({_,P})->not is_process_alive(P).

start(Open,Kind,Path)->Parent=self(),Ref=make_ref(),Pid=spawn(fun()->case Open() of {ok,S}->Parent!{Ref,ok,self()},loop(S,Kind,Path,<<>>);{error,R}->Parent!{Ref,error,R} end end),receive{Ref,ok,Pid}->{'Ok',{tag(Kind),Pid}};{Ref,error,R}->error_value('Backend',diag(R))after 11000->exit(Pid,kill),error_value('Timeout',<<"Unix socket initialization timed out">>)end.
tag(connection)->'Net.Socket.Unix.UnixConnection';tag(listener)->'Net.Socket.Unix.UnixListener'.
loop(S,Kind,Path,Buffer)->receive
 {call,F,R,accept}when Kind==listener->Reply=case gen_tcp:accept(S,30000)of{ok,A}->handoff(A,Path);{error,E}->error_value(kind(E),diag(E))end,F!{R,Reply},loop(S,Kind,Path,Buffer);
 {call,F,R,{send,D}}->Reply=case gen_tcp:send(S,D)of ok->{'Ok',byte_size(D)};{error,E}->error_value(kind(E),diag(E))end,F!{R,Reply},loop(S,Kind,Path,Buffer);
 {call,F,R,{recv,L}}->{Reply,Next}=recv_chunk(S,L,Buffer),F!{R,Reply},loop(S,Kind,Path,Next);
 {call,F,R,{recv_exact,N}}->{Reply,Next}=recv_exact(S,N,Buffer),F!{R,Reply},loop(S,Kind,Path,Next);
 {call,F,R,{recv_until,D,L}}->{Reply,Next}=recv_until(S,D,L,Buffer),F!{R,Reply},loop(S,Kind,Path,Next);
 {call,F,R,shutdown_write}->Reply=case gen_tcp:shutdown(S,write)of ok->{'Ok',ok};{error,E}->error_value(kind(E),diag(E))end,F!{R,Reply},loop(S,Kind,Path,Buffer);
 close->gen_tcp:close(S),case Kind of listener->file:delete(Path);_->ok end,ok;
 _->loop(S,Kind,Path,Buffer)
end.
recv_chunk(_,L,Buffer)when not is_integer(L);L=<0->{error_value('Limit',<<"Unix receive limit must be positive">>),Buffer};
recv_chunk(_,L,Buffer)when byte_size(Buffer)>0->N=min(L,byte_size(Buffer)),{{'Ok',{'Binary',binary:part(Buffer,0,N)}},binary:part(Buffer,N,byte_size(Buffer)-N)};
recv_chunk(S,L,<<>>)->case gen_tcp:recv(S,0,30000)of{ok,D}->recv_chunk(S,L,D);{error,E}->{error_value(kind(E),diag(E)),<<>>}end.
recv_exact(_,N,Buffer)when not is_integer(N);N<0->{error_value('Limit',<<"Unix receive count must be non-negative">>),Buffer};
recv_exact(_,0,Buffer)->{{'Ok',{'Binary',<<>>}},Buffer};
recv_exact(_,N,Buffer)when byte_size(Buffer)>=N->{{'Ok',{'Binary',binary:part(Buffer,0,N)}},binary:part(Buffer,N,byte_size(Buffer)-N)};
recv_exact(S,N,Buffer)->case gen_tcp:recv(S,N-byte_size(Buffer),30000)of{ok,D}->{{'Ok',{'Binary',<<Buffer/binary,D/binary>>}},<<>>};{error,E}->{error_value(kind(E),diag(E)),Buffer}end.
recv_until(_,D,_,Buffer)when not is_binary(D);byte_size(D)=:=0->{error_value('Parse',<<"Unix receive delimiter must not be empty">>),Buffer};
recv_until(_,_,L,Buffer)when not is_integer(L);L=<0->{error_value('Limit',<<"Unix receive limit must be positive">>),Buffer};
recv_until(S,D,L,Buffer)->case binary:match(Buffer,D)of
 {P,N}when P+N=<L->Count=P+N,{{'Ok',{'Binary',binary:part(Buffer,0,Count)}},binary:part(Buffer,Count,byte_size(Buffer)-Count)};
 _ when byte_size(Buffer)>=L->{{'Error',{'Net.NetError','Limit','Unix',<<"delimiter not found within limit">>,'None',{'Just',L}}},Buffer};
 nomatch->case gen_tcp:recv(S,0,30000)of{ok,More}->recv_until(S,D,L,<<Buffer/binary,More/binary>>);{error,E}->{error_value(kind(E),diag(E)),Buffer}end
end.
handoff(S,Path)->Ref=make_ref(),P=spawn(fun()->receive{Ref,S}->loop(S,connection,Path,<<>>)end end),case gen_tcp:controlling_process(S,P)of ok->P!{Ref,S},{'Ok',{'Net.Socket.Unix.UnixConnection',P}};{error,E}->exit(P,kill),gen_tcp:close(S),error_value(kind(E),diag(E))end.
call(P,Q)->case is_process_alive(P)of false->error_value('Closed',<<"Unix socket is closed">>);true->R=make_ref(),P!{call,self(),R,Q},receive{R,V}->V after 31000->error_value('Timeout',<<"Unix socket operation timed out">>)end end.
kind(timeout)->'Timeout';kind(closed)->'Closed';kind(_)->'Backend'.
diag(E)->unicode:characters_to_binary(io_lib:format("~p",[E])).
error_value(K,M)->{'Error',{'Net.NetError',K,'Unix',M,'None','None'}}.
