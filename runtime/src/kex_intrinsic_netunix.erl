-module(kex_intrinsic_netunix).
-export([address/1, connect/1, listen/1, accept/1, sendAll/2, receiveChunk/2, close/1]).

address(Path) when is_binary(Path), byte_size(Path)>0, byte_size(Path)<104 ->
    case Path of <<"/",_/binary>> -> {'Ok',{'Net.Socket.Unix.Address',Path}}; _ -> error_value('Parse',<<"Unix socket path must be absolute">>) end;
address(_) -> error_value('Parse',<<"invalid Unix socket path">>).

connect({'Net.Socket.Unix.Address',Path}) -> start(fun()->gen_tcp:connect({local,binary_to_list(Path)},0,[binary,{active,false},{packet,raw}],10000) end,connection,Path).
listen({'Net.Socket.Unix.Address',Path}) -> start(fun()->gen_tcp:listen(0,[binary,{active,false},{packet,raw},{ifaddr,{local,binary_to_list(Path)}}]) end,listener,Path).
accept({'Net.Socket.Unix.UnixListener',P})->call(P,accept).
sendAll({'Net.Socket.Unix.UnixConnection',P},{'Binary',D})->call(P,{send,D}).
receiveChunk({'Net.Socket.Unix.UnixConnection',P},L)->call(P,{recv,L}).
close({_,P})->case is_process_alive(P) of true->P!close;false->ok end,ok.

start(Open,Kind,Path)->Parent=self(),Ref=make_ref(),Pid=spawn(fun()->case Open() of {ok,S}->Parent!{Ref,ok,self()},loop(S,Kind,Path);{error,R}->Parent!{Ref,error,R} end end),receive{Ref,ok,Pid}->{'Ok',{tag(Kind),Pid}};{Ref,error,R}->error_value('Backend',diag(R))after 11000->exit(Pid,kill),error_value('Timeout',<<"Unix socket initialization timed out">>)end.
tag(connection)->'Net.Socket.Unix.UnixConnection';tag(listener)->'Net.Socket.Unix.UnixListener'.
loop(S,Kind,Path)->receive
 {call,F,R,accept}when Kind==listener->Reply=case gen_tcp:accept(S,30000)of{ok,A}->handoff(A,Path);{error,E}->error_value(kind(E),diag(E))end,F!{R,Reply},loop(S,Kind,Path);
 {call,F,R,{send,D}}->Reply=case gen_tcp:send(S,D)of ok->{'Ok',byte_size(D)};{error,E}->error_value(kind(E),diag(E))end,F!{R,Reply},loop(S,Kind,Path);
 {call,F,R,{recv,L}}->Reply=case gen_tcp:recv(S,0,30000)of{ok,D}when byte_size(D)=<L->{'Ok',{'Binary',D}};{ok,D}->{'Error',{'Net.NetError','Limit','Unix',<<"received chunk exceeds limit">>,'None',{'Just',byte_size(D)}}};{error,E}->error_value(kind(E),diag(E))end,F!{R,Reply},loop(S,Kind,Path);
 close->gen_tcp:close(S),case Kind of listener->file:delete(Path);_->ok end,ok;
 _->loop(S,Kind,Path)
end.
handoff(S,Path)->Ref=make_ref(),P=spawn(fun()->receive{Ref,S}->loop(S,connection,Path)end end),case gen_tcp:controlling_process(S,P)of ok->P!{Ref,S},{'Ok',{'Net.Socket.Unix.UnixConnection',P}};{error,E}->exit(P,kill),gen_tcp:close(S),error_value(kind(E),diag(E))end.
call(P,Q)->case is_process_alive(P)of false->error_value('Closed',<<"Unix socket is closed">>);true->R=make_ref(),P!{call,self(),R,Q},receive{R,V}->V after 31000->error_value('Timeout',<<"Unix socket operation timed out">>)end end.
kind(timeout)->'Timeout';kind(closed)->'Closed';kind(_)->'Backend'.
diag(E)->unicode:characters_to_binary(io_lib:format("~p",[E])).
error_value(K,M)->{'Error',{'Net.NetError',K,'Unix',M,'None','None'}}.
