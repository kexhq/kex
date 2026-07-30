%% Kex.Intrinsic.Time — BEAM primitive backend for the clock boundary.
%%
%% Only two primitives: "what time is it" and "what is this machine's UTC
%% offset". Civil conversion, formatting, parsing and arithmetic all live in
%% src/stdlib/time.kex as pure Kex, so the two backends cannot drift on
%% calendar behavior.
-module(kex_intrinsic_time).
-export(['nowNanos'/0, 'localOffset'/1]).

'nowNanos'() ->
    erlang:system_time(nanosecond).

%% Seconds east of UTC for the system zone AT the given instant (not today's
%% offset applied to every timestamp), derived by comparing the two civil
%% renderings of the same instant.
'localOffset'(EpochSeconds) ->
    Universal = calendar:gregorian_seconds_to_datetime(
                  EpochSeconds + epoch_gregorian_seconds()),
    case calendar:universal_time_to_local_time(Universal) of
        Local ->
            calendar:datetime_to_gregorian_seconds(Local) -
                calendar:datetime_to_gregorian_seconds(Universal)
    end.

epoch_gregorian_seconds() ->
    calendar:datetime_to_gregorian_seconds({{1970, 1, 1}, {0, 0, 0}}).
