%% Kex.Intrinsic.Time — BEAM primitive backend for the clock boundary.
%%
%% Only two primitives: "what time is it" and "what is this machine's UTC
%% offset". Civil conversion, formatting, parsing and arithmetic all live in
%% src/stdlib/time.kex as pure Kex, so the two backends cannot drift on
%% calendar behavior.
%%
%% Replacing the clock in a test is not done here: `Time.Clock` is a
%% capability, substituted with `with`, so there is no clock state to keep.
-module(kex_intrinsic_time).
-export(['nowNanos'/0, 'localOffset'/1]).

%% The host clock. `Time.Clock`'s default body reads it, so every reading in
%% the language (Time.now, Date.today, DateTime.utcNow) comes through here
%% unless that capability is replaced.
'nowNanos'() ->
    erlang:system_time(nanosecond).

%% Seconds east of UTC for the system zone AT the given instant (not today's
%% offset applied to every timestamp), derived by comparing the two civil
%% renderings of the same instant.
'localOffset'(EpochSeconds) ->
    Universal = calendar:gregorian_seconds_to_datetime(
                  EpochSeconds + epoch_gregorian_seconds()),
    %% The zone database only answers for instants the host's TZ rules cover.
    %% Outside that `universal_time_to_local_time` raises badarg, which took
    %% the whole program down for anything reading `Date.today()` with the
    %% clock frozen at a historical date. UTC is the honest answer when the
    %% zone cannot be determined, and it is what the interpreter already
    %% returns when localtime_r fails — so the two backends now agree.
    try calendar:universal_time_to_local_time(Universal) of
        Local ->
            calendar:datetime_to_gregorian_seconds(Local) -
                calendar:datetime_to_gregorian_seconds(Universal)
    catch
        _:_ -> 0
    end.

epoch_gregorian_seconds() ->
    calendar:datetime_to_gregorian_seconds({{1970, 1, 1}, {0, 0, 0}}).
