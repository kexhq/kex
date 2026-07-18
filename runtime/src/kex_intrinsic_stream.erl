%% Kex.Intrinsic.Stream — BEAM primitive backend for lazy streams.
%%
%% A stream is {'Stream', Thunk} where Thunk() -> {Value, NextStream}: forcing
%% the thunk yields one element and the rest of the stream. The 'Stream' tag
%% matches the compiler's record-style dispatch guard (is_tuple, element(1)),
%% so prelude Stream methods dispatch on it like any make-block type.
%% map/filter stay lazy (they wrap the thunk); take/2 is the only
%% materializer.
-module(kex_intrinsic_stream).
-export([make/2, generate/2, take/2, drop/2, map/2, filter/2]).

%% make/2 — infinite stream from a seed and a successor function.
make(Seed, Succ) ->
    {'Stream', fun() -> {Seed, make(Succ(Seed), Succ)} end}.

generate(Seed, Succ) -> make(Seed, Succ).

%% take/2 — the first N elements as a real list.
take(_, N) when N =< 0 -> [];
take({'Stream', T}, N) ->
    {V, Next} = T(),
    [V | take(Next, N - 1)].

%% drop/2 — skip N elements, returning the rest of the stream (still lazy).
drop(S, N) when N =< 0 -> S;
drop({'Stream', T}, N) ->
    {_, Next} = T(),
    drop(Next, N - 1).

%% map/2 — lazily transform each element.
map({'Stream', T}, F) ->
    {'Stream', fun() ->
        {V, Next} = T(),
        {F(V), map(Next, F)}
    end}.

%% filter/2 — lazily keep matching elements. Forcing scans forward until the
%% predicate holds, so a too-strict predicate on an infinite stream diverges —
%% same contract as the walker.
filter({'Stream', T}, Pred) ->
    {'Stream', fun() -> next_match(T, Pred) end}.

next_match(T, Pred) ->
    {V, Next} = T(),
    case Pred(V) of
        true  -> {V, filter(Next, Pred)};
        false -> {'Stream', NextT} = Next, next_match(NextT, Pred)
    end.
