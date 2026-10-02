-module(kex_supervisor).
-behaviour(supervisor).
-export([start_link/1, start_link_otp/1, init/1, worker/1, supervisor/1, start_child/1]).

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

%% start_link_otp/1 — the OTP-shaped start for a nested supervisor, which
%% its parent calls (and recalls on restart) and expects {ok, Pid} from.
start_link_otp(Spec) ->
    supervisor:start_link(?MODULE, Spec).

%% OTP supervisor callback.
init(#{strategy := Strat, children := Children}) ->
    OTPStrategy = strategy(Strat),
    OTPChildren = lists:map(fun to_otp_child/1, Children),
    {ok, {#{strategy => OTPStrategy, intensity => 10, period => 60},
          OTPChildren}}.

%% Wrapper: calls Fun() which returns a Pid, wraps it as {ok, Pid} for OTP.
%% A Kex `spawn` is unlinked, so link the child here — this runs in the
%% supervisor, and without the link OTP never hears that the child died.
%% (Linking an already-dead pid delivers a `noproc` exit, which the
%% supervisor treats as a crash and restarts.)
start_child(Fun) ->
    Pid = Fun(),
    link(Pid),
    {ok, Pid}.

%% Convert a Kex child-spec map to an OTP child_spec map.
to_otp_child(#{supervisor := Spec, id := Id}) ->
    #{id       => Id,
      start    => {kex_supervisor, start_link_otp, [Spec]},
      restart  => permanent,
      shutdown => infinity,
      type     => supervisor};
to_otp_child(#{start_fun := Fun, id := Id}) ->
    #{id      => Id,
      start   => {kex_supervisor, start_child, [Fun]},
      restart => permanent,
      shutdown => 5000,
      type    => worker};
to_otp_child(Child) ->
    to_otp_child(Child#{id => make_ref()}).

%% worker(Fun) — build a child-spec from a 0-arity fun.
%% Called from Kex codegen for `worker { block }` form.
worker(Fun) ->
    #{start_fun => Fun}.

%% supervisor(Spec) — a nested supervisor as a child-spec. Spec has the
%% same shape start_link/1 takes; the parent starts it (and restarts it)
%% through start_link_otp/1.
%% Called from Kex codegen for `supervisor(restart: ...) do [...] end`.
supervisor(Spec) ->
    #{supervisor => Spec}.
