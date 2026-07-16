-module(kex_intrinsic_http).
-export([get/1, get/2, post/2, post/3, put/2, put/3,
         patch/2, patch/3, delete/1, delete/2,
         head/1, head/2, options/1, options/2,
         mockStart/0, mockRespond/2, mockRespond/3, mockStop/0]).

get(Url)          -> kex_http:get(Url).
get(Url, Opts)    -> kex_http:get(Url, Opts).
post(Url, Body)         -> kex_http:post(Url, Body).
post(Url, Body, Opts)   -> kex_http:post(Url, Body, Opts).
put(Url, Body)          -> kex_http:put(Url, Body).
put(Url, Body, Opts)    -> kex_http:put(Url, Body, Opts).
patch(Url, Body)        -> kex_http:patch(Url, Body).
patch(Url, Body, Opts)  -> kex_http:patch(Url, Body, Opts).
delete(Url)       -> kex_http:delete(Url).
delete(Url, Opts) -> kex_http:delete(Url, Opts).
head(Url)         -> kex_http:head(Url).
head(Url, Opts)   -> kex_http:head(Url, Opts).
options(Url)      -> kex_http:options(Url).
options(Url, Opts) -> kex_http:options(Url, Opts).

mockStart() -> kex_http:mock_start().
mockRespond(Status, Body) -> kex_http:mock_respond(Status, Body).
mockRespond(Status, Body, Headers) ->
    kex_http:mock_respond(Status, Body, Headers).
mockStop() -> kex_http:mock_stop().
