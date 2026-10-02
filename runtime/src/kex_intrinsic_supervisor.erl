%% Kex.Intrinsic.Supervisor — the nested-supervisor child spec behind the
%% prelude's `supervisor(restart:) do [...] end`.
-module(kex_intrinsic_supervisor).
-export([nested/2]).

%% The children block arrives as a 0-arity fun; call it now, the way
%% `Supervisor.start` lowers its own block to the children list.
nested(Strategy, Children) when is_function(Children, 0) ->
    nested(Strategy, Children());
nested(Strategy, Children) ->
    kex_supervisor:supervisor(#{strategy => Strategy, children => Children}).
