-module(kex_intrinsic_nettls).
-export([connect/2, sendAll/2, receiveChunk/2, close/1, 'closed?'/1]).

connect({'Net.Socket.TCP.Endpoint',Host,{'Net.Port',Port}},
        {'Net.Socket.TLS.ClientConfig',ServerName,Verify,ALPN}) ->
    ssl:start(),
    VerifyOpts=case Verify of true->[{verify,verify_peer},{cacerts,public_key:cacerts_get()}];false->[{verify,verify_none}]end,
    Opts=[binary,{active,false},{server_name_indication,binary_to_list(ServerName)},
          {versions,['tlsv1.2','tlsv1.3']},{alpn_advertised_protocols,ALPN}|VerifyOpts],
    start(fun()->ssl:connect(binary_to_list(Host),Port,Opts,10000)end).
sendAll({'Net.Socket.TLS.TLSConnection',P},{'Binary',D})->call(P,{send,D}).
receiveChunk({'Net.Socket.TLS.TLSConnection',P},L)->call(P,{recv,L}).
close({'Net.Socket.TLS.TLSConnection',P})->case is_process_alive(P)of true->P!close;false->ok end,ok.
'closed?'({'Net.Socket.TLS.TLSConnection',P})->not is_process_alive(P).

start(Open)->Parent=self(),Ref=make_ref(),Pid=spawn(fun()->case Open()of{ok,S}->Parent!{Ref,ok,self()},loop(S,<<>>);{error,E}->Parent!{Ref,error,E}end end),receive{Ref,ok,Pid}->{'Ok',{'Net.Socket.TLS.TLSConnection',Pid}};{Ref,error,E}->err(kind(E),diag(E))after 11000->exit(Pid,kill),err('Timeout',<<"TLS handshake timed out">>)end.
loop(S,Buffer)->receive
 {call,F,R,{send,D}}->V=case ssl:send(S,D)of ok->{'Ok',byte_size(D)};{error,E}->err(kind(E),diag(E))end,F!{R,V},loop(S,Buffer);
 {call,F,R,{recv,L}}->{V,Next}=recv_chunk(S,L,Buffer),F!{R,V},loop(S,Next);
 close->ssl:close(S),ok;
 _->loop(S,Buffer)
end.
recv_chunk(_,L,Buffer)when not is_integer(L);L=<0->{err('Limit',<<"TLS receive limit must be positive">>),Buffer};
recv_chunk(_,L,Buffer)when byte_size(Buffer)>0->N=min(L,byte_size(Buffer)),{{'Ok',{'Binary',binary:part(Buffer,0,N)}},binary:part(Buffer,N,byte_size(Buffer)-N)};
recv_chunk(S,L,<<>>)->case ssl:recv(S,0,30000)of{ok,D}->recv_chunk(S,L,D);{error,E}->{err(kind(E),diag(E)),<<>>}end.
call(P,Q)->case is_process_alive(P)of false->err('Closed',<<"TLS connection is closed">>);true->R=make_ref(),P!{call,self(),R,Q},receive{R,V}->V after 31000->err('Timeout',<<"TLS operation timed out">>)end end.
kind(timeout)->'Timeout';kind(closed)->'Closed';kind({tls_alert,_})->'Protocol';kind(_)->'Backend'.
diag(E)->unicode:characters_to_binary(io_lib:format("~p",[E])).
err(K,M)->{'Error',{'Net.NetError',K,'TLS',M,'None','None'}}.
