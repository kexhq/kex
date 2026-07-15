-module(kex_intrinsic_http).
-export([mockStart/0, mockRespond/2, mockRespond/3, mockStop/0]).

mockStart() -> kex_http:mock_start().
mockRespond(Status, Body) -> kex_http:mock_respond(Status, Body).
mockRespond(Status, Body, Headers) ->
    kex_http:mock_respond(Status, Body, Headers).
mockStop() -> kex_http:mock_stop().
