-module(kex_intrinsic_nethttpserver).
-export([start/2, start/3, serve/2, serve/3, stop/1, stop/2, join/1,
         'running?'/1, localAddress/1]).

-define(HEADER_LIMIT, 65536).
-define(REQUEST_LINE_LIMIT, 8192).
-define(HEADER_FIELDS_LIMIT, 100).
-define(TIMEOUT, 30000).

start({'Net.Socket.TCP.Endpoint', Host, {'Net.Port', Port}},
      Router = {'Net.HTTP.Router', _}) ->
    start({'Net.Socket.TCP.Endpoint', Host, {'Net.Port', Port}}, Router,
          {'Net.HTTP.ServerOptions', 1024, 1024, kex_intrinsic_time:seconds(10.0), 10485760});
start(_, _) -> net_error('Parse', <<"invalid HTTP server configuration">>).

%% `MaximumBody` is the request body limit of every route that does not set
%% its own (kexhq/kex#488).
start({'Net.Socket.TCP.Endpoint', Host, {'Net.Port', Port}},
      Router = {'Net.HTTP.Router', _},
      {'Net.HTTP.ServerOptions', MaximumHandlers, Backlog, {'Measure', GraceSeconds, _, _},
       MaximumBody})
  when is_integer(MaximumHandlers), MaximumHandlers > 0,
       is_integer(Backlog), Backlog > 0, is_number(GraceSeconds), GraceSeconds >= 0,
       is_integer(MaximumBody), MaximumBody >= 0 ->
    case valid_route_limits(Router) of
        false -> net_error('Parse', <<"a route's maximumBodyBytes must not be negative">>);
        true ->
            case parse_address(Host) of
                {ok, Address} -> open(Address, Port, {Router, MaximumBody}, MaximumHandlers,
                                      Backlog, trunc(GraceSeconds * 1000));
                {error, Reason} -> net_error('Parse', Reason)
            end
    end;
start(_, _, _) -> net_error('Parse', <<"invalid HTTP server configuration">>).

valid_route_limits({'Net.HTTP.Router', Routes}) ->
    lists:all(fun({'Net.HTTP.Route', _, _, _, 'None'}) -> true;
                 ({'Net.HTTP.Route', _, _, _, {'Just', Limit}}) -> is_integer(Limit) andalso Limit >= 0;
                 (_) -> false
              end, Routes).

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
stop({'Net.HTTP.Server.Running', Pid, _, _}, {'Measure', Seconds, _, _})
  when is_number(Seconds), Seconds >= 0 -> stop_owner(Pid, trunc(Seconds * 1000));
stop(_, _) -> net_error('Parse', <<"invalid graceful shutdown duration">>).

stop_owner(Pid, Grace) ->
    case is_process_alive(Pid) of
        false -> {'Ok', {'Net.HTTP.ShutdownReport', 0, 0, 0, 0}};
        true ->
            Monitor = erlang:monitor(process, Pid),
            Ref = make_ref(),
            Pid ! {call, self(), Ref, {stop, Grace}},
            receive
                {Ref, Report} -> await_exit(Monitor, Pid, Report);
                {'DOWN', Monitor, process, Pid, _} ->
                    erlang:demonitor(Monitor, [flush]),
                    {'Ok', {'Net.HTTP.ShutdownReport', 0, 0, 0, 0}}
            after Grace + 1000 ->
                erlang:demonitor(Monitor, [flush]),
                net_error('Timeout', <<"HTTP server operation timed out">>)
            end
    end.

%% `finish_shutdown` hands the caller its report and only then returns, so the
%% owner terminates a moment AFTER the reply is deliverable. A `stop` that
%% returned in that moment left `running?` — plain `is_process_alive` —
%% reporting a stopped server as running, which a loaded CI runner hit as a
%% flaky failure. The reply means the accept loop is finished, so the exit
%% cannot be far behind; the kill only bounds the wait in case a future change
%% leaves the owner alive after replying.
await_exit(Monitor, Pid, Report) ->
    receive
        {'DOWN', Monitor, process, Pid, _} -> Report
    after 1000 ->
        exit(Pid, kill),
        receive {'DOWN', Monitor, process, Pid, _} -> Report end
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
            manage(top_up(#{listener => Listener, service => Router,
                            maximum => MaximumHandlers,
                            pool => max(16, 2 * erlang:system_info(schedulers_online)),
                            acceptors => #{}, active => #{}, http => 0,
                            completed => 0, failed => 0}));
        {error, Reason} -> Parent ! {Ref, error, Reason}
    end.

%% The process `start` returns (the manager) owns the listening socket and
%% keeps a pool of acceptors blocked in gen_tcp:accept on it, so accepting
%% never waits on bookkeeping and connections are taken up in parallel
%% (kexhq/kex#670). An acceptor that gets a connection becomes that
%% connection's worker: it owns the socket from the accept on, with no
%% hand-over, and the manager starts a replacement.
%%
%% State:
%% - `acceptors`: pid => monitor, for processes still waiting to accept.
%% - `active`: pid => {http | websocket, monitor}, for workers.
%% - `http`: how many workers are `http`. Only those hold a handler slot: a
%%   worker that has upgraded to a WebSocket reports `handler_upgraded` and
%%   stops counting, since its lifetime is unbounded and counting it would cap
%%   a WebSocket server at `maximum` connections, later clients stuck in the
%%   backlog (#390). Upgraded workers stay in `active`, so graceful shutdown
%%   still waits for them and, past the grace period, kills them.
%%
%% `http + |acceptors|` never exceeds `maximum`: each acceptor takes at most
%% one connection, so the limit holds even before the manager hears of an
%% accept, and the pool is only topped up as slots free.
manage(State) ->
    receive
        {accepted, Pid} -> manage(top_up(accepted(Pid, State)));
        {handler_upgraded, Pid} -> manage(top_up(upgraded(Pid, State)));
        {handler_done, Pid, Result} -> manage(top_up(done(Pid, Result, State)));
        {'DOWN', _, process, Pid, Reason} -> manage(top_up(down(Pid, Reason, State)));
        {call, From, Ref, {stop, Grace}} ->
            %% Closing the listener answers every blocked accept with
            %% {error, closed} at once, and those acceptors end.
            gen_tcp:close(maps:get(listener, State)),
            shutdown_loop(State, From, Ref, erlang:monotonic_time(millisecond), Grace)
    end.

top_up(State = #{acceptors := Acceptors, http := Http, maximum := Maximum, pool := Pool}) ->
    case map_size(Acceptors) < min(Pool, Maximum - Http) of
        true -> top_up(spawn_acceptor(State));
        false -> State
    end.

spawn_acceptor(State = #{listener := Listener, service := Service, acceptors := Acceptors}) ->
    Manager = self(),
    {Pid, Monitor} = spawn_monitor(fun() -> acceptor(Listener, Service, Manager) end),
    State#{acceptors := Acceptors#{Pid => Monitor}}.

%% The manager hands each acceptor `{Router, MaximumBody}`.
acceptor(Listener, Service, Manager) ->
    case gen_tcp:accept(Listener) of
        {ok, Socket} ->
            Manager ! {accepted, self()},
            Result = try handle_connection(Socket, Service, <<>>, Manager) catch _:_ -> error end,
            gen_tcp:close(Socket),
            Manager ! {handler_done, self(), Result};
        {error, closed} -> ok;
        %% Out of file descriptors, say: the manager counts it and starts
        %% another acceptor, after a pause so a lasting error doesn't spin.
        {error, _} -> timer:sleep(10), exit(accept_failed)
    end.

accepted(Pid, State = #{acceptors := Acceptors, active := Active, http := Http}) ->
    case maps:take(Pid, Acceptors) of
        {Monitor, Acceptors1} ->
            State#{acceptors := Acceptors1, active := Active#{Pid => {http, Monitor}}, http := Http + 1};
        error -> State
    end.

upgraded(Pid, State = #{active := Active, http := Http}) ->
    case maps:find(Pid, Active) of
        {ok, {http, Monitor}} -> State#{active := Active#{Pid := {websocket, Monitor}}, http := Http - 1};
        _ -> State
    end.

done(Pid, Result, State = #{active := Active}) ->
    case maps:take(Pid, Active) of
        {{Kind, Monitor}, Active1} ->
            erlang:demonitor(Monitor, [flush]),
            tally(Result, release(Kind, State#{active := Active1}));
        error -> State
    end.

%% A process that ended without saying so: an acceptor whose accept failed
%% (or that the closed listener ended, which is no failure), or a worker
%% that crashed or was killed before reporting.
down(Pid, Reason, State = #{acceptors := Acceptors, active := Active}) ->
    case maps:take(Pid, Acceptors) of
        {_, Acceptors1} when Reason =:= normal -> State#{acceptors := Acceptors1};
        {_, Acceptors1} -> tally(error, State#{acceptors := Acceptors1});
        error ->
            case maps:take(Pid, Active) of
                {{Kind, _}, Active1} -> tally(error, release(Kind, State#{active := Active1}));
                error -> State
            end
    end.

release(http, State = #{http := Http}) -> State#{http := Http - 1};
release(websocket, State) -> State.

tally(ok, State = #{completed := Completed}) -> State#{completed := Completed + 1};
tally(_, State = #{failed := Failed}) -> State#{failed := Failed + 1}.

%% After `stop`: the listener is closed, so no new connection arrives, but an
%% acceptor may have taken one just before; it still reports `accepted`, and
%% is then waited for like any other worker.
shutdown_loop(State = #{acceptors := Acceptors, active := Active}, From, Ref, Started, Grace) ->
    case map_size(Acceptors) + map_size(Active) of
        0 -> finish_shutdown(State, 0, From, Ref, Started);
        _ ->
            Elapsed = erlang:monotonic_time(millisecond) - Started,
            Remaining = max(0, Grace - Elapsed),
            receive
                {accepted, Pid} -> shutdown_loop(accepted(Pid, State), From, Ref, Started, Grace);
                {handler_upgraded, Pid} -> shutdown_loop(upgraded(Pid, State), From, Ref, Started, Grace);
                {handler_done, Pid, Result} -> shutdown_loop(done(Pid, Result, State), From, Ref, Started, Grace);
                {'DOWN', _, process, Pid, Reason} -> shutdown_loop(down(Pid, Reason, State), From, Ref, Started, Grace)
            after Remaining ->
                maps:foreach(fun(Pid, _) -> exit(Pid, kill) end, Acceptors),
                maps:foreach(fun(Pid, _) -> exit(Pid, kill) end, Active),
                finish_shutdown(State, map_size(Active), From, Ref, Started)
            end
    end.

finish_shutdown(#{completed := Completed, failed := Failed}, Forced, From, Ref, Started) ->
    Elapsed = erlang:monotonic_time(millisecond) - Started,
    From ! {Ref, {'Ok', {'Net.HTTP.ShutdownReport', Completed, Failed, Forced, Elapsed}}}.

handle_connection(Socket, Service, Buffered, Owner) ->
    case read_request(Socket, Buffered, Service) of
        {ok, Request, Method, Found, Rest, KeepAlive} ->
            case dispatch(Request, Method, Found) of
                {'Net.HTTP.WebSocket.UpgradeResponse', Response, HandlerFun, Selected} ->
                    case send_upgrade_response(Socket, Response) of
                        ok ->
                            Owner ! {handler_upgraded, self()},
                            kex_intrinsic_netwebsocket:accept(Socket, Rest, HandlerFun, Selected);
                        _ -> error
                    end;
                Response ->
                    case send_response(Socket, Response, Method, KeepAlive) of
                        ok when KeepAlive -> handle_connection(Socket, Service, Rest, Owner);
                        ok -> ok;
                        _ -> error
                    end
            end;
        {error, closed} -> ok;
        {error, timeout} -> ok;
        {error, too_large} -> send_response(Socket, response(413, <<"Payload Too Large\n">>), <<"GET">>, false), error;
        {error, _} -> send_response(Socket, response(400, <<"Bad Request\n">>), <<"GET">>, false), error
    end.

%% `Found` is find_route's answer for the request line, worked out once in
%% build_request, where it also chose the body limit.
dispatch(Request, Method, Found) ->
    case Found of
        {route, {'Net.HTTP.Route', _, _, Handler, _}, Parameters} ->
            Context = {'Net.HTTP.Context', {'Net.HTTP.RouteContext', Parameters}},
            try Handler(Request, Context) of
                Value = {'Net.HTTP.Response', _, _, _} -> Value;
                Value = {'Net.HTTP.WebSocket.UpgradeResponse', _, _, _} -> Value;
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
    Exact = route_matches(Method, Path, Routes),
    Matches = case {Method, Exact} of
        {<<"HEAD">>, []} -> route_matches(<<"GET">>, Path, Routes);
        _ -> Exact
    end,
    case Matches of
        [{Route, Parameters} | _] -> {route, Route, Parameters};
        [] ->
            Declared = [M || {'Net.HTTP.Route', M, Pattern, _, _} <- Routes,
                               {ok, _} <- [match_path(Pattern, Path)]],
            Methods = case lists:member(<<"GET">>, Declared) of
                true -> Declared ++ [<<"HEAD">>, <<"OPTIONS">>];
                false -> Declared ++ [<<"OPTIONS">>]
            end,
            case Declared of [] -> none; _ -> {methods, lists:usort(Methods)} end
    end.

route_matches(Method, Path, Routes) ->
    [{R, Parameters} || R = {'Net.HTTP.Route', M, Pattern, _, _} <- Routes,
                         M =:= Method,
                         {ok, Parameters} <- [match_path(Pattern, Path)]].

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

read_request(Socket, Buffered, Service) ->
    case recv_headers(Socket, Buffered) of
        {ok, HeaderBlock, Rest} -> parse_request(Socket, HeaderBlock, Rest, Service);
        Error -> Error
    end.

recv_headers(Socket, Acc) ->
    case binary:match(Acc, <<"\r\n\r\n">>) of
        {Position, 4} when Position =< ?HEADER_LIMIT ->
            {ok, binary:part(Acc, 0, Position),
             binary:part(Acc, Position + 4, byte_size(Acc) - Position - 4)};
        {_, 4} -> {error, too_large};
        nomatch when byte_size(Acc) > ?HEADER_LIMIT -> {error, too_large};
        nomatch ->
            case gen_tcp:recv(Socket, 0, ?TIMEOUT) of
                {ok, Chunk} -> recv_headers(Socket, <<Acc/binary, Chunk/binary>>);
                {error, Reason} -> {error, Reason}
            end
    end.

parse_request(Socket, Block, Buffered, Service) ->
    case binary:split(Block, <<"\r\n">>, [global]) of
        [Line | HeaderLines] when byte_size(Line) =< ?REQUEST_LINE_LIMIT,
                                  length(HeaderLines) =< ?HEADER_FIELDS_LIMIT ->
            case binary:split(Line, <<" ">>, [global, trim_all]) of
                [Method, Target, Version = <<"HTTP/1.1">>] -> build_request(Socket, Method, Target, Version, HeaderLines, Buffered, Service);
                [Method, Target, Version = <<"HTTP/1.0">>] -> build_request(Socket, Method, Target, Version, HeaderLines, Buffered, Service);
                _ -> {error, bad_request_line}
            end;
        _ -> {error, bad_request_line}
    end.

build_request(Socket, Method, Target, Version, Lines, Buffered, Service) ->
    case valid_token(Method) andalso valid_target(Target) of
      false -> {error, bad_request_line};
      true -> case parse_headers(Lines, []) of
        {ok, Headers} ->
            Path = hd(binary:split(Target, <<"?">>)),
            Found = find_route(Method, Path, routes(Service)),
            BodyLimit = body_limit(Found, Service),
            %% Names lowercased once, for every lookup below.
            Lowered = [{string:lowercase(K), V} || {K, V} <- Headers],
            case valid_host(Version, Lowered) andalso body_length(Lowered) of
                Length when is_integer(Length), Length >= 0, Length =< BodyLimit ->
                    case read_body(Socket, Buffered, Length) of
                        {ok, Body, Rest} ->
                            Request = {'Net.HTTP.Request', Method, {'URI.URI', Target},
                                       {'Net.HTTP.Headers', Headers}, {'Binary', Body}},
                            {ok, Request, Method, Found, Rest,
                             keep_alive(Version, Lowered)};
                        Error -> Error
                    end;
                Length when is_integer(Length), Length > BodyLimit -> {error, too_large};
                _ -> {error, invalid_framing}
            end;
        Error -> Error
    end end.

%% The route that will answer is known from the request line alone, so its
%% own limit applies before a byte of the body is read; any other request,
%% routed or not, gets the server's.
body_limit(Found, {_Router, ServerLimit}) ->
    case Found of
        {route, {'Net.HTTP.Route', _, _, _, {'Just', RouteLimit}}, _} -> RouteLimit;
        _ -> ServerLimit
    end.

routes({{'Net.HTTP.Router', Routes}, _ServerLimit}) -> Routes.

parse_headers([], Acc) -> {ok, lists:reverse(Acc)};
parse_headers([Line | Rest], Acc) ->
    case binary:split(Line, <<":">>) of
        [Name, Value] when Name =/= <<>> ->
            Trimmed = trim_ows(Value),
            case valid_token(Name) andalso valid_field_value(Trimmed) of
                true -> parse_headers(Rest, [{Name, Trimmed} | Acc]);
                false -> {error, invalid_header}
            end;
        _ -> {error, invalid_header}
    end.

body_length(Headers) ->
    Transfers = values(<<"transfer-encoding">>, Headers),
    Lengths = values(<<"content-length">>, Headers),
    case {Transfers, Lengths} of
        {[], []} -> 0;
        {[], [Length]} -> try binary_to_integer(Length) catch _:_ -> invalid end;
        _ -> invalid
    end.

%% `Lowered` has its names lowercased already (build_request).
values(Name, Lowered) -> [V || {K, V} <- Lowered, K =:= Name].
valid_host(<<"HTTP/1.1">>, Headers) -> length(values(<<"host">>, Headers)) =:= 1;
valid_host(<<"HTTP/1.0">>, _) -> true.
valid_target(<<"*">>) -> true;
valid_target(<<"/", _/binary>>) -> true;
valid_target(_) -> false.
%% A token is visible ASCII without the separators (RFC 9110 §5.6.2).
%% Checked byte by byte on the binary, building no list.
valid_token(Value) when is_binary(Value), byte_size(Value) > 0 -> token_bytes(Value);
valid_token(_) -> false.
token_bytes(<<>>) -> true;
token_bytes(<<C, Rest/binary>>) when C > 32, C < 127 ->
    case separator(C) of
        true -> false;
        false -> token_bytes(Rest)
    end;
token_bytes(_) -> false.
separator(C) -> lists:member(C, "()<>@,;:\\\"/[]?={} ").
valid_field_value(<<>>) -> true;
valid_field_value(<<9, Rest/binary>>) -> valid_field_value(Rest);
valid_field_value(<<C, Rest/binary>>) when C >= 32, C =/= 127 -> valid_field_value(Rest);
valid_field_value(_) -> false.

%% Optional whitespace around a field value is spaces and tabs only (RFC 9110
%% §5.6.3), so it is trimmed byte-wise rather than with Unicode's notion.
trim_ows(Value) -> trim_trailing(trim_leading(Value)).
trim_leading(<<C, Rest/binary>>) when C =:= $\s; C =:= $\t -> trim_leading(Rest);
trim_leading(Value) -> Value.
trim_trailing(<<>>) -> <<>>;
trim_trailing(Value) ->
    case binary:last(Value) of
        C when C =:= $\s; C =:= $\t -> trim_trailing(binary:part(Value, 0, byte_size(Value) - 1));
        _ -> Value
    end.
read_body(_, Buffered, Length) when byte_size(Buffered) >= Length ->
    {ok, binary:part(Buffered, 0, Length),
     binary:part(Buffered, Length, byte_size(Buffered) - Length)};
read_body(Socket, Buffered, Length) ->
    case gen_tcp:recv(Socket, Length - byte_size(Buffered), ?TIMEOUT) of
        {ok, Rest} -> {ok, <<Buffered/binary, Rest/binary>>, <<>>};
        {error, Reason} -> {error, Reason}
    end.

keep_alive(Version, Headers) ->
    Tokens = [string:trim(Token) || Value <- values(<<"connection">>, Headers),
                                    Token <- binary:split(string:lowercase(Value), <<",">>, [global])],
    case Version of
        <<"HTTP/1.1">> -> not lists:member(<<"close">>, Tokens);
        <<"HTTP/1.0">> -> lists:member(<<"keep-alive">>, Tokens)
    end.

send_response(Socket, {'Net.HTTP.Response', {'Net.HTTP.Status', Status},
                        {'Net.HTTP.Headers', Headers0}, {'Binary', Body}}, Method, KeepAlive) ->
    case valid_response(Status, Headers0, Body) of
        false -> {error, invalid_response};
        true ->
            Headers = [{K, V} || {K, V} <- Headers0,
                                 string:lowercase(K) =/= <<"content-length">>,
                                 string:lowercase(K) =/= <<"connection">>],
            Head = [<<"HTTP/1.1 ">>, integer_to_binary(Status), <<" ">>, reason(Status), <<"\r\n">>,
                    [[K, <<": ">>, V, <<"\r\n">>] || {K, V} <- Headers],
                    <<"Content-Length: ">>, integer_to_binary(byte_size(Body)),
                    <<"\r\nConnection: ">>, case KeepAlive of true -> <<"keep-alive">>; false -> <<"close">> end,
                    <<"\r\n\r\n">>],
            gen_tcp:send(Socket, case Method of <<"HEAD">> -> Head; _ -> [Head, Body] end)
    end;
send_response(Socket, _, Method, KeepAlive) ->
    send_response(Socket, response(500, <<"Internal Server Error\n">>), Method, KeepAlive).

% 101 has no body and no Content-Length/Connection framing — the connection
% stops being HTTP the moment these bytes land, so `send_response`'s
% keep-alive machinery doesn't apply. `valid_response` rejects 101 outright
% (it's meaningless for an ordinary buffered response), so this checks
% headers directly instead of reusing it.
send_upgrade_response(Socket, {'Net.HTTP.Response', {'Net.HTTP.Status', 101},
                        {'Net.HTTP.Headers', Headers}, {'Binary', <<>>}})
  when length(Headers) =< ?HEADER_FIELDS_LIMIT ->
    case lists:all(fun({Name, Value}) -> valid_token(Name) andalso valid_field_value(Value) end, Headers) of
        true ->
            Head = [<<"HTTP/1.1 101 Switching Protocols\r\n">>,
                    [[K, <<": ">>, V, <<"\r\n">>] || {K, V} <- Headers],
                    <<"\r\n">>],
            gen_tcp:send(Socket, Head);
        false ->
            _ = send_response(Socket, response(500, <<"Internal Server Error\n">>), <<"GET">>, false),
            {error, invalid_response}
    end;
send_upgrade_response(Socket, _) ->
    _ = send_response(Socket, response(500, <<"Internal Server Error\n">>), <<"GET">>, false),
    {error, invalid_response}.

response(Status, Body) -> {'Net.HTTP.Response', {'Net.HTTP.Status', Status},
                           {'Net.HTTP.Headers', [{<<"Content-Type">>, <<"text/plain; charset=utf-8">>}]},
                           {'Binary', Body}}.
valid_response(Status, Headers, Body) ->
    is_integer(Status) andalso Status >= 100 andalso Status =< 599 andalso
    Status =/= 101 andalso length(Headers) =< ?HEADER_FIELDS_LIMIT andalso
    lists:all(fun({Name, Value}) -> valid_token(Name) andalso valid_field_value(Value);
                 (_) -> false
              end, Headers) andalso
    not ((Status >= 100 andalso Status < 200 orelse Status =:= 204 orelse Status =:= 304)
         andalso byte_size(Body) =/= 0).
reason(200) -> <<"OK">>; reason(204) -> <<"No Content">>; reason(400) -> <<"Bad Request">>;
reason(404) -> <<"Not Found">>; reason(405) -> <<"Method Not Allowed">>;
reason(413) -> <<"Payload Too Large">>; reason(426) -> <<"Upgrade Required">>;
reason(500) -> <<"Internal Server Error">>;
reason(_) -> <<"Response">>.
join_methods(Methods) -> iolist_to_binary(lists:join(<<", ">>, Methods)).
parse_address(Host) -> case inet:parse_address(binary_to_list(Host)) of {ok, A} -> {ok, A}; _ -> {error, <<"server endpoint must contain an IP address">>} end.
address_text(Address) -> list_to_binary(inet:ntoa(Address)).
diagnostic(Value) -> unicode:characters_to_binary(io_lib:format("~p", [Value])).
net_error(Kind, Message) -> {'Error', {'Net.NetError', Kind, 'HTTPServer', Message, 'None', 'None'}}.
