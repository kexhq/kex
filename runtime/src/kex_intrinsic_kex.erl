-module(kex_intrinsic_kex).
-export([backend/0, 'featureHas?'/1, 'featureList'/0]).

backend() -> 'Beam'.

'featureHas?'(Feature) ->
    lists:member(Feature, 'featureList'()).

'featureList'() ->
    ['Http', 'FS', 'Process', 'WebServer'].
