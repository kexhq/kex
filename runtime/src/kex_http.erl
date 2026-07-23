%% kex_http — Http client runtime (OTP httpc) + Mock.Http registry.
%%
%% Real requests use httpc (inets + ssl). Mock.Http stores queued
%% responses in the process dictionary (same pattern as kex_file's
%% mock FS) so both interpreter and BEAM tests can run without
%% network access.
-module(kex_http).
-compile({no_auto_import, [get/1, put/2]}).
-export([get/1, get/2, post/2, post/3, put/2, put/3, patch/2, patch/3,
         delete/1, delete/2, head/1, head/2, options/1, options/2,
         mock_start/0, mock_respond/2, mock_respond/3, mock_stop/0]).

%% ── Mock.Http registry ─────────────────────────────────────────────────
mock_start() ->
    erlang:put(kex_mock_http, true),
    erlang:put(kex_mock_http_queue, []),
    ok.

mock_respond(Status, Body) ->
    mock_respond(Status, Body, #{}).

mock_respond(Status, Body, Headers) ->
    Q = mock_queue(),
    erlang:put(kex_mock_http_queue, Q ++ [{'HttpResponse', Status,
                                            kex_io:to_string_bin(Body),
                                            headers_to_map(Headers)}]),
    ok.

mock_stop() ->
    erlang:erase(kex_mock_http),
    erlang:erase(kex_mock_http_queue),
    ok.

mock_active() -> erlang:get(kex_mock_http) =:= true.
mock_queue()  -> case erlang:get(kex_mock_http_queue) of undefined -> []; Q -> Q end.

mock_dequeue() ->
    case mock_queue() of
        [Resp | Rest] ->
            erlang:put(kex_mock_http_queue, Rest),
            {'Ok', Resp};
        [] ->
            {'Error', {'HttpError', 'MockEmpty', <<"no responses staged">>}}
    end.

%% ── Public API ──────────────────────────────────────────────────────────
get(Url)      -> do_request(get, Url, none, default_opts()).
get(Url, Opts) -> do_request(get, Url, none, extract_opts(Opts)).

post(Url, Body)       -> do_request(post, Url, Body, default_opts()).
post(Url, Body, Opts) -> do_request(post, Url, Body, extract_opts(Opts)).

put(Url, Body)       -> do_request(put, Url, Body, default_opts()).
put(Url, Body, Opts) -> do_request(put, Url, Body, extract_opts(Opts)).

patch(Url, Body)       -> do_request(patch, Url, Body, default_opts()).
patch(Url, Body, Opts) -> do_request(patch, Url, Body, extract_opts(Opts)).

delete(Url)      -> do_request(delete, Url, none, default_opts()).
delete(Url, Opts) -> do_request(delete, Url, none, extract_opts(Opts)).

head(Url)      -> do_request(head, Url, none, default_opts()).
head(Url, Opts) -> do_request(head, Url, none, extract_opts(Opts)).

options(Url)      -> do_request(options, Url, none, default_opts()).
options(Url, Opts) -> do_request(options, Url, none, extract_opts(Opts)).

%% ── Internals ───────────────────────────────────────────────────────────
default_opts() -> {#{}, 30000}.

extract_opts({'HttpOptions', Headers, Timeout}) -> {Headers, Timeout};
extract_opts(_) -> default_opts().

do_request(Method, Url, Body, {Headers, Timeout}) ->
    case mock_active() of
        true -> mock_dequeue();
        false -> real_request(Method, Url, Body, Headers, Timeout)
    end.

ensure_started() ->
    case erlang:get(kex_http_started) of
        true -> ok;
        _ ->
            application:ensure_all_started(inets),
            application:ensure_all_started(ssl),
            erlang:put(kex_http_started, true),
            ok
    end.

real_request(Method, Url, Body, Headers, Timeout) ->
    ensure_started(),
    UrlStr = binary_to_list(kex_io:to_string_bin(Url)),
    HdrList = maps:fold(fun(K, V, Acc) ->
        [{binary_to_list(kex_io:to_string_bin(K)),
          binary_to_list(kex_io:to_string_bin(V))} | Acc]
    end, [], Headers),
    Request = case Body of
        none -> {UrlStr, HdrList};
        _ ->
            ContentType = case maps:find(<<"Content-Type">>, Headers) of
                {ok, CT} -> binary_to_list(kex_io:to_string_bin(CT));
                error ->
                    case maps:find(<<"content-type">>, Headers) of
                        {ok, CT2} -> binary_to_list(kex_io:to_string_bin(CT2));
                        error -> "application/octet-stream"
                    end
            end,
            {UrlStr, HdrList, ContentType, kex_io:to_string_bin(Body)}
    end,
    HttpOpts = [{timeout, Timeout}, {autoredirect, true}],
    Opts = [{body_format, binary}],
    try httpc:request(Method, Request, HttpOpts, Opts) of
        {ok, {{_, Status, _}, RespHeaders, RespBody}} ->
            HdrMap = lists:foldl(fun({K, V}, Acc) ->
                maps:put(list_to_binary(K), list_to_binary(V), Acc)
            end, #{}, RespHeaders),
            {'Ok', {'HttpResponse', Status, RespBody, HdrMap}};
        {error, Reason} ->
            {Kind, Msg} = classify_error(Reason),
            {'Error', {'HttpError', Kind, Msg}}
    catch
        _:Reason2 ->
            {Kind2, Msg2} = classify_error(Reason2),
            {'Error', {'HttpError', Kind2, Msg2}}
    end.

classify_error({failed_connect, Info}) ->
    Flat = lists:flatten(io_lib:format("~p", [Info])),
    case string:find(Flat, "nxdomain") of
        nomatch ->
            case string:find(Flat, "econnrefused") of
                nomatch -> {'Unknown', list_to_binary(Flat)};
                _ -> {'ConnectionRefused', <<"connection refused">>}
            end;
        _ -> {'DnsError', <<"DNS resolution failed">>}
    end;
classify_error(timeout) ->
    {'Timeout', <<"request timed out">>};
classify_error({tls_alert, _} = E) ->
    {'SslError', list_to_binary(lists:flatten(io_lib:format("~p", [E])))};
classify_error(Reason) ->
    {'Unknown', list_to_binary(lists:flatten(io_lib:format("~p", [Reason])))}.

headers_to_map(M) when is_map(M) -> M;
headers_to_map(_) -> #{}.
