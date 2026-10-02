%% Kex.Intrinsic.Supervisor — the prelude's `Supervisor.start(restart:)`,
%% `Supervisor.supervisor(restart:)` and `Supervisor.worker { ... }`.
-module(kex_intrinsic_supervisor).
-export([start/2, nested/2, worker/1]).

start(Strategy, Children) ->
    kex_supervisor:start_link(spec(Strategy, Children)).

nested(Strategy, Children) ->
    kex_supervisor:supervisor(spec(Strategy, Children)).

worker(Start) ->
    kex_supervisor:worker(Start).

%% The children block arrives as a 0-arity fun returning the collected list.
spec(Strategy, Children) when is_function(Children, 0) ->
    spec(Strategy, Children());
spec(Strategy, Children) ->
    #{strategy => Strategy, children => Children}.
