%% Kex.Intrinsic.Stream — BEAM primitive backend for lazy streams.
%%
%% A stream is {'Stream', Thunk} where Thunk() -> {Value, NextStream} | done:
%% forcing the thunk yields one element and the rest of the stream, or reports
%% the end. The `done` case is what lets a stream be FINITE — a file's lines
%% converted with Feed.toStream — rather than only the endlessly generated kind
%% take/2 used to assume. The 'Stream' tag matches the compiler's record-style
%% dispatch guard (is_tuple, element(1)), so prelude Stream methods dispatch on
%% it like any make-block type. map/filter stay lazy (they wrap the thunk);
%% take/2 and each/2 are the materializers.
%%
%% Unlike the walker's, these cells do not memoise: BEAM values are immutable,
%% so there is nowhere to write a forced element back to. Walking a stream
%% twice therefore recomputes it, which costs time but never changes an answer
%% — every operation here is pure. The one place that difference is visible is
%% Feed.toStream, and kex_intrinsic_feed handles it there.
-module(kex_intrinsic_stream).
-export([generate/2, empty/0, take/2, drop/2, map/2, filter/2, each/2,
         toFeed/1, of_list/1]).

%% make/2 — infinite stream from a seed and a successor function.
make(Seed, Succ) ->
    {'Stream', fun() -> {Seed, make(Succ(Seed), Succ)} end}.

generate(Seed, Succ) -> make(Seed, Succ).

empty() -> {'Stream', fun() -> done end}.

%% of_list/1 — a finite stream over elements already in hand.
of_list([]) -> empty();
of_list([H | T]) -> {'Stream', fun() -> {H, of_list(T)} end}.

%% take/2 — the first N elements as a real list, stopping at the stream's end
%% rather than padding the answer out to N.
take(_, N) when N =< 0 -> [];
take({'Stream', T}, N) ->
    case T() of
        done      -> [];
        {V, Next} -> [V | take(Next, N - 1)]
    end.

%% drop/2 — skip N elements, returning the rest of the stream (still lazy).
drop(S, N) when N =< 0 -> S;
drop({'Stream', T}, N) ->
    case T() of
        done      -> empty();
        {_, Next} -> drop(Next, N - 1)
    end.

%% map/2 — lazily transform each element.
map({'Stream', T}, F) ->
    {'Stream', fun() ->
        case T() of
            done      -> done;
            {V, Next} -> {F(V), map(Next, F)}
        end
    end}.

%% filter/2 — lazily keep matching elements. Forcing scans forward until the
%% predicate holds, so a too-strict predicate on an infinite stream diverges —
%% same contract as the walker.
filter({'Stream', T}, Pred) ->
    {'Stream', fun() -> next_match(T, Pred) end}.

next_match(T, Pred) ->
    case T() of
        done -> done;
        {V, Next} ->
            case Pred(V) of
                true  -> {V, filter(Next, Pred)};
                false -> {'Stream', NextT} = Next, next_match(NextT, Pred)
            end
    end.

%% each/2 — force the whole stream for its effects. Runs forever on a stream
%% that never ends, exactly as the same loop written by hand would.
each({'Stream', T}, F) ->
    case T() of
        done      -> 'unit';
        {V, Next} -> F(V), each(Next, F)
    end.

%% toFeed/1 — hand the stream to a one-shot cursor.
toFeed(S) -> kex_intrinsic_feed:of_stream(S).
