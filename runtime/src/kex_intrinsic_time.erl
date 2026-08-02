%% Kex.Intrinsic.Time — BEAM primitive backend for the clock boundary.
%%
%% Only two primitives: "what time is it" and "what is this machine's UTC
%% offset". Civil conversion, formatting, parsing and arithmetic all live in
%% src/stdlib/time.kex as pure Kex, so the two backends cannot drift on
%% calendar behavior.
%%
%% The exception is the test clock, which overrides "what time is it" so a
%% test can pin the calendar to a known instant. Its state lives in a
%% persistent_term so every process sees the same clock, the way a real one
%% behaves.
-module(kex_intrinsic_time).
-export(['nowNanos'/0, 'localOffset'/1,
         'freezeAt'/1, 'travelTo'/1, release/0, 'clockMode'/0]).

-define(CLOCK_KEY, {kex, clock_override}).

%% Every clock reading in the language funnels through here — Time.now,
%% Date.today, DateTime.utcNow — so controlling this one function controls
%% all of them.
'nowNanos'() ->
    case persistent_term:get(?CLOCK_KEY, real) of
        {frozen, Nanos} -> Nanos;
        {travelling, Delta} -> erlang:system_time(nanosecond) + Delta;
        real -> erlang:system_time(nanosecond)
    end.

%% Pin the clock to an instant. Repeated readings return the same value.
'freezeAt'(Nanos) ->
    persistent_term:put(?CLOCK_KEY, {frozen, Nanos}),
    {}.

%% Move the clock to an instant and let it run from there.
'travelTo'(Nanos) ->
    persistent_term:put(?CLOCK_KEY, {travelling, Nanos - erlang:system_time(nanosecond)}),
    {}.

%% Back to the host clock.
release() ->
    persistent_term:erase(?CLOCK_KEY),
    {}.

%% 0 = real, 1 = frozen, 2 = travelling. An integer rather than an atom so
%% both backends report the same thing without a shared variant table.
'clockMode'() ->
    case persistent_term:get(?CLOCK_KEY, real) of
        {frozen, _} -> 1;
        {travelling, _} -> 2;
        real -> 0
    end.

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
