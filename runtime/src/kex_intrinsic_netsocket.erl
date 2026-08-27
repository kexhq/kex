-module(kex_intrinsic_netsocket).
-export([sendAll/2, receiveChunk/2, accept/1, close/1, 'closed?'/1]).

sendAll(C = {'Net.Socket.TCP.TCPConnection', _}, D) -> kex_intrinsic_nettcp:sendAll(C, D);
sendAll(C = {'Net.Socket.Unix.UnixConnection', _}, D) -> kex_intrinsic_netunix:sendAll(C, D);
sendAll(C = {'Net.Socket.TLS.TLSConnection', _}, D) -> kex_intrinsic_nettls:sendAll(C, D).

receiveChunk(C = {'Net.Socket.TCP.TCPConnection', _}, L) -> kex_intrinsic_nettcp:receiveChunk(C, L);
receiveChunk(C = {'Net.Socket.Unix.UnixConnection', _}, L) -> kex_intrinsic_netunix:receiveChunk(C, L);
receiveChunk(C = {'Net.Socket.TLS.TLSConnection', _}, L) -> kex_intrinsic_nettls:receiveChunk(C, L).

accept(L = {'Net.Socket.TCP.TCPListener', _}) -> kex_intrinsic_nettcp:accept(L);
accept(L = {'Net.Socket.Unix.UnixListener', _}) -> kex_intrinsic_netunix:accept(L).

close(H = {'Net.Socket.TCP.TCPConnection', _}) -> kex_intrinsic_nettcp:close(H);
close(H = {'Net.Socket.TCP.TCPListener', _}) -> kex_intrinsic_nettcp:close(H);
close(H = {'Net.Socket.UDP.Socket', _}) -> kex_intrinsic_netudp:close(H);
close(H = {'Net.Socket.Unix.UnixConnection', _}) -> kex_intrinsic_netunix:close(H);
close(H = {'Net.Socket.Unix.UnixListener', _}) -> kex_intrinsic_netunix:close(H);
close(H = {'Net.Socket.TLS.TLSConnection', _}) -> kex_intrinsic_nettls:close(H).

'closed?'({'Net.Socket.TCP.TCPConnection', P}) -> not is_process_alive(P);
'closed?'({'Net.Socket.TCP.TCPListener', P}) -> not is_process_alive(P);
'closed?'({'Net.Socket.UDP.Socket', P}) -> not is_process_alive(P).
