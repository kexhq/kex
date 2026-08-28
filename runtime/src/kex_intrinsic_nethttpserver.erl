-module(kex_intrinsic_nethttpserver).
-export([start/2, start/3, serve/2, serve/3, stop/1, stop/2, join/1,
         'running?'/1, localAddress/1]).

-define(HEADER_LIMIT, 65536).
-define(BODY_LIMIT, 10485760).
-define(TIMEOUT, 30000).

start({'Net.Socket.TCP.Endpoint', Host, {'Net.Port', Port}},
      Router = {'Net.HTTP.Router', _}) ->
    start({'Net.Socket.TCP.Endpoint', Host, {'Net.Port', Port}}, Router,
          {'Net.HTTP.ServerOptions', 1024, 128, {'Duration', 10.0}});
start(_, _) -> net_error('Parse', <<"invalid HTTP server configuration">>).

start({'Net.Socket.TCP.Endpoint', Host, {'Net.Port', Port}},
      Router = {'Net.HTTP.Router', _},
      {'Net.HTTP.ServerOptions', MaximumHandlers, Backlog, {'Duration', GraceSeconds}})
  when is_integer(MaximumHandlers), MaximumHandlers > 0,
       is_integer(Backlog), Backlog > 0, is_number(GraceSeconds), GraceSeconds >= 0 ->
    case parse_address(Host) of
        {ok, Address} -> open(Address, Port, Router, MaximumHandlers, Backlog,
                              trunc(GraceSeconds * 1000));
        {error, Reason} -> net_error('Parse', Reason)
    end;
start(_, _, _) -> net_error('Parse', <<"invalid HTTP server configuration">>).

serve(Endpoint, Router) ->
    case start(Endpoint, Router) of
        {'Ok', Running} -> join(Running);
        Error -> Error
    end.
serve(Endpoint, Router, Options) ->
    case start(Endpoint, Router, Options) of
        {'Ok', Running} -> join(Running);
        Error -> Error
    end.

stop({'Net.HTTP.Server.Running', Pid, _, Grace}) -> stop_owner(Pid, Grace);
stop({'Net.HTTP.Server.Running', Pid, _}) -> stop_owner(Pid, 10000).
stop({'Net.HTTP.Server.Running', Pid, _, _}, {'Duration', Seconds})
  when is_number(Seconds), Seconds >= 0 -> stop_owner(Pid, trunc(Seconds * 1000));
stop(_, _) -> net_error('Parse', <<"invalid graceful shutdown duration">>).

stop_owner(Pid, Grace) ->
    case is_process_alive(Pid) of
        false -> {'Ok', {'Net.HTTP.ShutdownReport', 0, 0, 0, 0}};
        true -> call(Pid, {stop, Grace}, Grace + 1000)
    end.

join({'Net.HTTP.Server.Running', Pid, _, _}) -> join_owner(Pid);
join({'Net.HTTP.Server.Running', Pid, _}) -> join_owner(Pid).
join_owner(Pid) ->
    case is_process_alive(Pid) of
        false -> {'Ok', 'Kex.Unit'};
        true ->
            Monitor = erlang:monitor(process, Pid),
            receive {'DOWN', Monitor, process, Pid, _} -> {'Ok', 'Kex.Unit'} end
    end.

'running?'({'Net.HTTP.Server.Running', Pid, _, _}) -> is_process_alive(Pid);
'running?'({'Net.HTTP.Server.Running', Pid, _}) -> is_process_alive(Pid).
localAddress({'Net.HTTP.Server.Running', _, Endpoint, _}) -> Endpoint;
localAddress({'Net.HTTP.Server.Running', _, Endpoint}) -> Endpoint.

open(Address, Port, Router, MaximumHandlers, Backlog, Grace) ->
    Parent = self(), Ref = make_ref(),
    Pid = spawn(fun() -> init(Parent, Ref, Address, Port, Router,
                              MaximumHandlers, Backlog) end),
    receive
        {Ref, ok, Endpoint} -> {'Ok', {'Net.HTTP.Server.Running', Pid, Endpoint, Grace}};
        {Ref, error, Reason} -> net_error('Connect', diagnostic(Reason))
    after 11000 ->
        exit(Pid, kill), net_error('Timeout', <<"HTTP server bind timed out">>)
    end.

init(Parent, Ref, Address, Port, Router, MaximumHandlers, Backlog) ->
    Options = [binary, {packet, raw}, {active, false}, {reuseaddr, true},
               {nodelay, true}, {backlog, Backlog}, {ip, Address}],
    case gen_tcp:listen(Port, Options) of
        {ok, Listener} ->
            {ok, {LocalAddress, LocalPort}} = inet:sockname(Listener),
            Endpoint = {'Net.Socket.TCP.Endpoint', address_text(LocalAddress),
                        {'Net.Port', LocalPort}},
            Parent ! {Ref, ok, Endpoint},
            accept_loop(Listener, Router, MaximumHandlers, #{}, 0, 0);
        {error, Reason} -> Parent ! {Ref, error, Reason}
    end.

accept_loop(Listener, Router, MaximumHandlers, Active, Completed, Failed) ->
    receive
        {call, From, Ref, {stop, Grace}} ->
            gen_tcp:close(Listener),
            shutdown_loop(Active, Completed, Failed, From, Ref,
                          erlang:monotonic_time(millisecond), Grace);
        {handler_done, Worker, ok} ->
            accept_loop(Listener, Router, MaximumHandlers,
                        maps:remove(Worker, Active), Completed + 1, Failed);
        {handler_done, Worker, error} ->
            accept_loop(Listener, Router, MaximumHandlers,
                        maps:remove(Worker, Active), Completed, Failed + 1)
    after 0 ->
        case maps:size(Active) >= MaximumHandlers of
          true ->
            receive
                {handler_done, Worker, ok} ->
                    accept_loop(Listener, Router, MaximumHandlers,
                                maps:remove(Worker, Active), Completed + 1, Failed);
                {handler_done, Worker, error} ->
                    accept_loop(Listener, Router, MaximumHandlers,
                                maps:remove(Worker, Active), Completed, Failed + 1);
                {call, From, Ref, {stop, Grace}} ->
                    gen_tcp:close(Listener),
                    shutdown_loop(Active, Completed, Failed, From, Ref,
                                  erlang:monotonic_time(millisecond), Grace)
            end;
          false -> case gen_tcp:accept(Listener, 250) of
            {ok, Socket} ->
                Owner = self(), Worker = spawn(fun connection_wait/0),
                case gen_tcp:controlling_process(Socket, Worker) of
                    ok -> Worker ! {serve, Socket, Router, Owner};
                    {error, _} -> gen_tcp:close(Socket), Owner ! {handler_done, Worker, error}
                end,
                accept_loop(Listener, Router, MaximumHandlers,
                            maps:put(Worker, true, Active), Completed, Failed);
            {error, timeout} -> accept_loop(Listener, Router, MaximumHandlers, Active, Completed, Failed);
            {error, closed} -> ok;
            {error, _} -> accept_loop(Listener, Router, MaximumHandlers, Active, Completed, Failed + 1)
          end
        end
    end.

shutdown_loop(Active, Completed, Failed, From, Ref, Started, Grace) ->
    case maps:size(Active) of
        0 -> finish_shutdown(Completed, Failed, 0, From, Ref, Started);
        _ ->
            Elapsed = erlang:monotonic_time(millisecond) - Started,
            Remaining = max(0, Grace - Elapsed),
            receive
                {handler_done, Worker, ok} ->
                    shutdown_loop(maps:remove(Worker, Active), Completed + 1,
                                  Failed, From, Ref, Started, Grace);
                {handler_done, Worker, error} ->
                    shutdown_loop(maps:remove(Worker, Active), Completed,
                                  Failed + 1, From, Ref, Started, Grace)
            after Remaining ->
                maps:foreach(fun(Worker, _) -> exit(Worker, kill) end, Active),
                finish_shutdown(Completed, Failed, maps:size(Active), From, Ref, Started)
            end
    end.

finish_shutdown(Completed, Failed, Forced, From, Ref, Started) ->
    Elapsed = erlang:monotonic_time(millisecond) - Started,
    From ! {Ref, {'Ok', {'Net.HTTP.ShutdownReport', Completed, Failed, Forced, Elapsed}}}.

connection_wait() ->
    receive
        {serve, Socket, Router, Owner} ->
            Result = try handle_connection(Socket, Router) catch _:_ -> error end,
            gen_tcp:close(Socket), Owner ! {handler_done, self(), Result}
    after 5000 -> ok
    end.

handle_connection(Socket, Router) ->
    case read_request(Socket) of
        {ok, Request, Method, Path} ->
            case send_response(Socket, dispatch(Request, Method, Path, Router), Method) of
                ok -> handle_connection(Socket, Router);
                _ -> error
            end;
        {error, closed} -> ok;
        {error, timeout} -> ok;
        {error, too_large} -> send_response(Socket, response(413, <<"Payload Too Large\n">>), <<"GET">>), error;
        {error, _} -> send_response(Socket, response(400, <<"Bad Request\n">>), <<"GET">>), error
    end.

dispatch(Request, Method, Path, {'Net.HTTP.Router', Routes}) ->
    case find_route(Method, Path, Routes) of
        {route, {'Net.HTTP.Route', _, _, Handler}, Parameters} ->
            Context = {'Net.HTTP.Context', {'Net.HTTP.RouteContext', Parameters}},
            try Handler(Request, Context) of
                Value = {'Net.HTTP.Response', _, _, _} -> Value;
                _ -> response(500, <<"Internal Server Error\n">>)
            catch _:_ -> response(500, <<"Internal Server Error\n">>) end;
        {methods, Methods} when Method =:= <<"OPTIONS">> ->
            {'Net.HTTP.Response', {'Net.HTTP.Status', 204},
             {'Net.HTTP.Headers', [{<<"Allow">>, join_methods(Methods)}]}, {'Binary', <<>>}};
        {methods, Methods} ->
            {'Net.HTTP.Response', {'Net.HTTP.Status', 405},
             {'Net.HTTP.Headers', [{<<"Allow">>, join_methods(Methods)}]}, {'Binary', <<"Method Not Allowed\n">>}};
        none -> response(404, <<"Not Found\n">>)
    end.

find_route(Method, Path, Routes) ->
    Matches = [{R, Parameters} || R = {'Net.HTTP.Route', M, Pattern, _} <- Routes,
                  (M =:= Method orelse (Method =:= <<"HEAD">> andalso M =:= <<"GET">>)),
                  {ok, Parameters} <- [match_path(Pattern, Path)]],
    case Matches of
        [{Route, Parameters} | _] -> {route, Route, Parameters};
        [] ->
            Methods = [M || {'Net.HTTP.Route', M, Pattern, _} <- Routes,
                              {ok, _} <- [match_path(Pattern, Path)]],
            case Methods of [] -> none; _ -> {methods, lists:usort(Methods ++ [<<"OPTIONS">>])} end
    end.

match_path(Pattern, Path) ->
    match_segments(split_path(Pattern), split_path(Path), #{}).
split_path(<<"/">>) -> [];
split_path(Path) -> binary:split(Path, <<"/">>, [global]).
match_segments([], [], Parameters) -> {ok, Parameters};
match_segments([<<":", Name/binary>> | Patterns], [Value | Values], Parameters)
  when Name =/= <<>> ->
    case decode_segment(Value) of
        {ok, Decoded} -> match_segments(Patterns, Values, maps:put(Name, Decoded, Parameters));
        error -> no
    end;
match_segments([<<"*", Name/binary>>], Values, Parameters) when Name =/= <<>> ->
    case decode_segment(iolist_to_binary(lists:join(<<"/">>, Values))) of
        {ok, Decoded} -> {ok, maps:put(Name, Decoded, Parameters)};
        error -> no
    end;
match_segments([Segment | Patterns], [Segment | Values], Parameters) ->
    match_segments(Patterns, Values, Parameters);
match_segments(_, _, _) -> no.
decode_segment(Value) -> try {ok, uri_string:percent_decode(Value)} catch _:_ -> error end.

read_request(Socket) ->
    case recv_headers(Socket, <<>>) of
        {ok, HeaderBlock, Buffered} -> parse_request(Socket, HeaderBlock, Buffered);
        Error -> Error
    end.

recv_headers(_, Acc) when byte_size(Acc) > ?HEADER_LIMIT -> {error, too_large};
recv_headers(Socket, Acc) ->
    case binary:match(Acc, <<"\r\n\r\n">>) of
        {Position, 4} ->
            {ok, binary:part(Acc, 0, Position),
             binary:part(Acc, Position + 4, byte_size(Acc) - Position - 4)};
        nomatch ->
            case gen_tcp:recv(Socket, 0, ?TIMEOUT) of
                {ok, Chunk} -> recv_headers(Socket, <<Acc/binary, Chunk/binary>>);
                {error, Reason} -> {error, Reason}
            end
    end.

parse_request(Socket, Block, Buffered) ->
    case binary:split(Block, <<"\r\n">>, [global]) of
        [Line | HeaderLines] ->
            case binary:split(Line, <<" ">>, [global, trim_all]) of
                [Method, Target, <<"HTTP/1.1">>] -> build_request(Socket, Method, Target, HeaderLines, Buffered);
                [Method, Target, <<"HTTP/1.0">>] -> build_request(Socket, Method, Target, HeaderLines, Buffered);
                _ -> {error, bad_request_line}
            end;
        _ -> {error, bad_request_line}
    end.

build_request(Socket, Method, Target, Lines, Buffered) ->
    case parse_headers(Lines, []) of
        {ok, Headers} ->
            case body_length(Headers) of
                Length when is_integer(Length), Length >= 0, Length =< ?BODY_LIMIT ->
                    case read_body(Socket, Buffered, Length) of
                        {ok, Body} ->
                            Path = hd(binary:split(Target, <<"?">>)),
                            Request = {'Net.HTTP.Request', Method, {'URI.URI', Target},
                                       {'Net.HTTP.Headers', Headers}, {'Binary', Body}},
                            {ok, Request, Method, Path};
                        Error -> Error
                    end;
                Length when is_integer(Length), Length > ?BODY_LIMIT -> {error, too_large};
                _ -> {error, invalid_framing}
            end;
        Error -> Error
    end.

parse_headers([], Acc) -> {ok, lists:reverse(Acc)};
parse_headers([Line | Rest], Acc) ->
    case binary:split(Line, <<":">>) of
        [Name, Value] when Name =/= <<>> ->
            parse_headers(Rest, [{Name, string:trim(Value)} | Acc]);
        _ -> {error, invalid_header}
    end.

body_length(Headers) ->
    Transfers = values(<<"transfer-encoding">>, Headers),
    Lengths = values(<<"content-length">>, Headers),
    case {Transfers, lists:usort(Lengths)} of
        {[], []} -> 0;
        {[], [Length]} -> try binary_to_integer(Length) catch _:_ -> invalid end;
        _ -> invalid
    end.

values(Name, Headers) -> [V || {K, V} <- Headers, string:lowercase(K) =:= Name].
read_body(_, Buffered, Length) when byte_size(Buffered) >= Length -> {ok, binary:part(Buffered, 0, Length)};
read_body(Socket, Buffered, Length) ->
    case gen_tcp:recv(Socket, Length - byte_size(Buffered), ?TIMEOUT) of
        {ok, Rest} -> {ok, <<Buffered/binary, Rest/binary>>};
        {error, Reason} -> {error, Reason}
    end.

send_response(Socket, {'Net.HTTP.Response', {'Net.HTTP.Status', Status},
                        {'Net.HTTP.Headers', Headers0}, {'Binary', Body}}, Method) ->
    Headers = [{K, V} || {K, V} <- Headers0,
                         string:lowercase(K) =/= <<"content-length">>,
                         string:lowercase(K) =/= <<"connection">>],
    Head = [<<"HTTP/1.1 ">>, integer_to_binary(Status), <<" ">>, reason(Status), <<"\r\n">>,
            [[K, <<": ">>, V, <<"\r\n">>] || {K, V} <- Headers],
            <<"Content-Length: ">>, integer_to_binary(byte_size(Body)),
            <<"\r\nConnection: keep-alive\r\n\r\n">>],
    gen_tcp:send(Socket, case Method of <<"HEAD">> -> Head; _ -> [Head, Body] end);
send_response(Socket, _, Method) -> send_response(Socket, response(500, <<"Internal Server Error\n">>), Method).

response(Status, Body) -> {'Net.HTTP.Response', {'Net.HTTP.Status', Status},
                           {'Net.HTTP.Headers', [{<<"Content-Type">>, <<"text/plain; charset=utf-8">>}]},
                           {'Binary', Body}}.
reason(200) -> <<"OK">>; reason(204) -> <<"No Content">>; reason(400) -> <<"Bad Request">>;
reason(404) -> <<"Not Found">>; reason(405) -> <<"Method Not Allowed">>;
reason(413) -> <<"Payload Too Large">>; reason(500) -> <<"Internal Server Error">>;
reason(_) -> <<"Response">>.
join_methods(Methods) -> iolist_to_binary(lists:join(<<", ">>, Methods)).
call(Pid, Message, Timeout) ->
    case is_process_alive(Pid) of
        false -> net_error('Closed', <<"HTTP server is stopped">>);
        true -> Ref = make_ref(), Pid ! {call, self(), Ref, Message},
                receive {Ref, Value} -> Value after Timeout -> net_error('Timeout', <<"HTTP server operation timed out">>) end
    end.
parse_address(Host) -> case inet:parse_address(binary_to_list(Host)) of {ok, A} -> {ok, A}; _ -> {error, <<"server endpoint must contain an IP address">>} end.
address_text(Address) -> list_to_binary(inet:ntoa(Address)).
diagnostic(Value) -> unicode:characters_to_binary(io_lib:format("~p", [Value])).
net_error(Kind, Message) -> {'Error', {'Net.NetError', Kind, 'HTTPServer', Message, 'None', 'None'}}.
