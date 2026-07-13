%% Runtime for the typed Web.Server prelude API.
-module(kex_intrinsic_web).
-export([serve/1]).

-define(HEADER_LIMIT, 65536).
-define(BODY_LIMIT, 10485760).
-define(RECV_TIMEOUT, 30000).

serve({'Server', Port, Routes}) when is_integer(Port), is_list(Routes) ->
    Options = [binary, {packet, raw}, {active, false}, {reuseaddr, true},
               {nodelay, true}, {backlog, 128}],
    case gen_tcp:listen(Port, Options) of
        {ok, Listener} ->
            accept_loop(Listener, Routes);
        {error, Reason} ->
            {'Error', reason_binary(Reason)}
    end;
serve(_) ->
    {'Error', <<"invalid Web.Server configuration">>}.

accept_loop(Listener, Routes) ->
    case gen_tcp:accept(Listener) of
        {ok, Socket} ->
            Handler = spawn(fun connection_wait/0),
            case gen_tcp:controlling_process(Socket, Handler) of
                ok -> Handler ! {serve, Socket, Routes};
                {error, _} -> gen_tcp:close(Socket), exit(Handler, normal)
            end,
            accept_loop(Listener, Routes);
        {error, closed} ->
            {'Ok', 'Kex.Unit'};
        {error, Reason} ->
            gen_tcp:close(Listener),
            {'Error', reason_binary(Reason)}
    end.

connection_wait() ->
    receive
        {serve, Socket, Routes} ->
            try handle_connection(Socket, Routes)
            catch
                Class:Reason:Stack ->
                    logger:error("Web.Server connection failed: ~p:~p ~p",
                                 [Class, Reason, Stack]),
                    send_response(Socket, internal_error(), <<"GET">>)
            after
                gen_tcp:close(Socket)
            end
    after 5000 ->
        ok
    end.

handle_connection(Socket, Routes) ->
    case read_request(Socket) of
        {ok, Request = {'Request', Method, _, _, _, _, _}} ->
            send_response(Socket, dispatch(Request, Routes), Method);
        {error, too_large} ->
            send_response(Socket, text_response(413, <<"Payload Too Large\n">>), <<"GET">>);
        {error, _} ->
            send_response(Socket, text_response(400, <<"Bad Request\n">>), <<"GET">>)
    end.

dispatch(Request = {'Request', Method, Path, _, _, _, _}, Routes) ->
    case find_route(Method, Path, Routes) of
        none -> text_response(404, <<"Not Found\n">>);
        {'Route', _, _, Handler} when is_function(Handler, 1) ->
            try Handler(Request) of
                Response = {'Response', _, _, _} -> Response;
                _ -> internal_error()
            catch
                Class:Reason:Stack ->
                    logger:error("Web.Server handler failed: ~p:~p ~p",
                                 [Class, Reason, Stack]),
                    internal_error()
            end;
        _ -> internal_error()
    end.

find_route(_, _, []) -> none;
find_route(Method, Path, [Route = {'Route', RouteMethod, RoutePath, _} | Rest]) ->
    case RoutePath =:= Path andalso
         (RouteMethod =:= <<"*">> orelse RouteMethod =:= Method orelse
          (Method =:= <<"HEAD">> andalso RouteMethod =:= <<"GET">>)) of
        true -> Route;
        false -> find_route(Method, Path, Rest)
    end;
find_route(Method, Path, [_ | Rest]) -> find_route(Method, Path, Rest).

read_request(Socket) ->
    case recv_headers(Socket, <<>>) of
        {ok, HeaderBlock, BufferedBody} -> parse_request(Socket, HeaderBlock, BufferedBody);
        Error -> Error
    end.

recv_headers(_, Acc) when byte_size(Acc) > ?HEADER_LIMIT -> {error, too_large};
recv_headers(Socket, Acc) ->
    case binary:match(Acc, <<"\r\n\r\n">>) of
        {Pos, 4} ->
            HeaderBlock = binary:part(Acc, 0, Pos),
            BodyStart = Pos + 4,
            BufferedBody = binary:part(Acc, BodyStart, byte_size(Acc) - BodyStart),
            {ok, HeaderBlock, BufferedBody};
        nomatch ->
            case gen_tcp:recv(Socket, 0, ?RECV_TIMEOUT) of
                {ok, Chunk} -> recv_headers(Socket, <<Acc/binary, Chunk/binary>>);
                {error, Reason} -> {error, Reason}
            end
    end.

parse_request(Socket, HeaderBlock, BufferedBody) ->
    case binary:split(HeaderBlock, <<"\r\n">>, [global]) of
        [RequestLine | HeaderLines] ->
            case binary:split(RequestLine, <<" ">>, [global, trim_all]) of
                [Method, Target, _Version] ->
                    Headers = parse_headers(HeaderLines, #{}),
                    case content_length(Headers) of
                        Length when Length > ?BODY_LIMIT -> {error, too_large};
                        Length when Length >= 0 ->
                            case read_body(Socket, BufferedBody, Length) of
                                {ok, Body} ->
                                    {Path, Query} = split_target(Target),
                                    QueryParams = parse_query(Query),
                                    {ok, {'Request', Method, Path, Query,
                                          QueryParams, Headers, Body}};
                                Error -> Error
                            end;
                        _ -> {error, bad_content_length}
                    end;
                _ -> {error, bad_request_line}
            end;
        _ -> {error, missing_request_line}
    end.

parse_headers([], Acc) -> Acc;
parse_headers([Line | Rest], Acc) ->
    case binary:split(Line, <<":">>) of
        [RawName, RawValue] ->
            Name = string:lowercase(string:trim(RawName)),
            Value = string:trim(RawValue),
            parse_headers(Rest, maps:put(Name, Value, Acc));
        _ -> parse_headers(Rest, Acc)
    end.

content_length(Headers) ->
    case maps:get(<<"content-length">>, Headers, <<"0">>) of
        Value ->
            try binary_to_integer(Value)
            catch _:_ -> -1
            end
    end.

read_body(_, Buffered, Length) when byte_size(Buffered) >= Length ->
    {ok, binary:part(Buffered, 0, Length)};
read_body(Socket, Buffered, Length) ->
    Missing = Length - byte_size(Buffered),
    case gen_tcp:recv(Socket, Missing, ?RECV_TIMEOUT) of
        {ok, Rest} -> {ok, <<Buffered/binary, Rest/binary>>};
        {error, Reason} -> {error, Reason}
    end.

split_target(Target) ->
    case binary:split(Target, <<"?">>) of
        [Path, Query] -> {Path, Query};
        [Path] -> {Path, <<>>}
    end.

parse_query(<<>>) -> #{};
parse_query(Query) ->
    lists:foldl(fun(Part, Acc) ->
        case binary:split(Part, <<"=">>) of
            [RawKey, RawValue] -> maps:put(url_decode(RawKey), url_decode(RawValue), Acc);
            [RawKey] -> maps:put(url_decode(RawKey), <<>>, Acc)
        end
    end, #{}, binary:split(Query, <<"&">>, [global])).

url_decode(Value) ->
    WithSpaces = binary:replace(Value, <<"+">>, <<" ">>, [global]),
    try uri_string:percent_decode(WithSpaces)
    catch _:_ -> WithSpaces
    end.

send_response(Socket, {'Response', Status, Headers0, Body0}, Method)
  when is_integer(Status), is_map(Headers0) ->
    Body = kex_io:to_string_bin(Body0),
    Headers1 = remove_header_ci(<<"content-length">>, Headers0),
    Headers2 = remove_header_ci(<<"connection">>, Headers1),
    Headers = Headers2#{<<"Content-Length">> => integer_to_binary(byte_size(Body)),
                       <<"Connection">> => <<"close">>},
    Head = [<<"HTTP/1.1 ">>, integer_to_binary(Status), <<" ">>,
            reason_phrase(Status), <<"\r\n">>, encode_headers(Headers), <<"\r\n">>],
    Payload = case Method of <<"HEAD">> -> Head; _ -> [Head, Body] end,
    gen_tcp:send(Socket, Payload);
send_response(Socket, _, Method) ->
    send_response(Socket, internal_error(), Method).

encode_headers(Headers) ->
    maps:fold(fun(K, V, Acc) ->
        [[kex_io:to_string_bin(K), <<": ">>, kex_io:to_string_bin(V), <<"\r\n">>] | Acc]
    end, [], Headers).

remove_header_ci(Name, Headers) ->
    maps:filter(fun(K, _) ->
        string:lowercase(kex_io:to_string_bin(K)) =/= Name
    end, Headers).

text_response(Status, Body) ->
    {'Response', Status, #{<<"Content-Type">> => <<"text/plain; charset=utf-8">>}, Body}.

internal_error() -> text_response(500, <<"Internal Server Error\n">>).

reason_phrase(200) -> <<"OK">>;
reason_phrase(201) -> <<"Created">>;
reason_phrase(202) -> <<"Accepted">>;
reason_phrase(204) -> <<"No Content">>;
reason_phrase(301) -> <<"Moved Permanently">>;
reason_phrase(302) -> <<"Found">>;
reason_phrase(304) -> <<"Not Modified">>;
reason_phrase(400) -> <<"Bad Request">>;
reason_phrase(401) -> <<"Unauthorized">>;
reason_phrase(403) -> <<"Forbidden">>;
reason_phrase(404) -> <<"Not Found">>;
reason_phrase(405) -> <<"Method Not Allowed">>;
reason_phrase(413) -> <<"Payload Too Large">>;
reason_phrase(500) -> <<"Internal Server Error">>;
reason_phrase(502) -> <<"Bad Gateway">>;
reason_phrase(503) -> <<"Service Unavailable">>;
reason_phrase(_) -> <<"Response">>.

reason_binary(Reason) ->
    unicode:characters_to_binary(io_lib:format("~p", [Reason])).
