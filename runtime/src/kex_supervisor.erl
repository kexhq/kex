-module(kex_supervisor).
-behaviour(supervisor).
-export([start_link/1, init/1, worker/1, start_child/1]).

%% Kex → OTP restart strategy translation.
strategy(only_crashed)      -> one_for_one;
strategy(all)               -> one_for_all;
strategy(crashed_and_newer) -> rest_for_one;
strategy(one_for_one)       -> one_for_one;  % passthrough for OTP-familiar users
strategy(one_for_all)       -> one_for_all;
strategy(rest_for_one)      -> rest_for_one;
strategy(Bad) ->
    error({unknown_kex_restart_strategy, Bad,
           [only_crashed, all, crashed_and_newer]}).

%% start_link/1 — entry point from Kex codegen.
%% Spec is #{strategy => atom, children => [ChildSpec]}.
%% Returns Ok(Pid) | Error(Reason) to match Kex Result conventions.
start_link(Spec) ->
    case supervisor:start_link(?MODULE, Spec) of
        {ok, Pid} -> {'Ok', Pid};
        {error, Reason} -> {'Error', Reason}
    end.

%% OTP supervisor callback.
init(#{strategy := Strat, children := Children}) ->
    OTPStrategy = strategy(Strat),
    OTPChildren = lists:map(fun to_otp_child/1, Children),
    {ok, {#{strategy => OTPStrategy, intensity => 10, period => 60},
          OTPChildren}}.

%% Wrapper: calls Fun() which returns a Pid, wraps it as {ok, Pid} for OTP.
start_child(Fun) ->
    Pid = Fun(),
    {ok, Pid}.

%% Convert a Kex child-spec map to an OTP child_spec map.
to_otp_child(#{start_fun := Fun, id := Id}) ->
    #{id      => Id,
      start   => {kex_supervisor, start_child, [Fun]},
      restart => permanent,
      shutdown => 5000,
      type    => worker};
to_otp_child(#{start_fun := Fun}) ->
    Id = make_ref(),
    to_otp_child(#{start_fun => Fun, id => Id}).

%% worker(Fun) — build a child-spec from a 0-arity fun.
%% Called from Kex codegen for `worker { block }` form.
worker(Fun) ->
    #{start_fun => Fun}.
