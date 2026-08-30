%% Kex.Intrinsic.Feed — BEAM primitive backend for one-shot feeds.
%%
%% A feed is {'Feed', Pid}: the process owns the cursor, and pulling asks it for
%% the next element. That indirection is what a feed NEEDS and a stream does
%% not — reading a feed consumes it, so every reference to one has to advance a
%% single shared position, and a BEAM term cannot be advanced in place. The
%% 'Feed' tag matches the compiler's record-style dispatch guard the same way
%% 'Stream' does.
%%
%% The process holds a source fun answering {Value, NextSource} | done, so a
%% file, a list and a mapped feed are all the same shape. map/filter spawn a
%% cursor over the inner feed rather than a second cursor over its source, so
%% consuming the result consumes the receiver — one pass, as on the walker.
-module(kex_intrinsic_feed).
-export([empty/0, elements/1, of_device/1, of_handle/1, of_stream/1, pull/1,
         'spent?'/1, take/2, drop/2, map/2, filter/2, each/2, collect/1,
         toStream/1]).

start(Source) -> {'Feed', spawn(fun() -> loop(Source, false) end)}.

%% Spent is latched separately from the source: once a source has answered
%% `done` it is never asked again, so a spent feed holds no file device open
%% and a second traversal is a no-op rather than a second read.
loop(Source, Spent) ->
    receive
        {pull, From, Ref} when Spent ->
            From ! {Ref, done},
            loop(Source, true);
        {pull, From, Ref} ->
            case Source() of
                done ->
                    From ! {Ref, done},
                    loop(fun() -> done end, true);
                {V, Next} ->
                    From ! {Ref, {ok, V}},
                    loop(Next, false)
            end;
        {spent, From, Ref} ->
            From ! {Ref, Spent},
            loop(Source, Spent);
        stop -> ok
    end.

%% Monitored, so a feed whose process has died (or never started) reports the
%% end rather than blocking the caller forever in `receive`.
ask(Pid, Tag) ->
    Mon = erlang:monitor(process, Pid),
    Ref = make_ref(),
    Pid ! {Tag, self(), Ref},
    receive
        {Ref, Answer} ->
            erlang:demonitor(Mon, [flush]),
            Answer;
        {'DOWN', Mon, process, Pid, _} ->
            case Tag of spent -> true; _ -> done end
    end.

empty() -> start(fun() -> done end).

%% elements/1 — a feed over a list already in hand.
elements(Xs) -> start(list_source(Xs)).

list_source([]) -> fun() -> done end;
list_source([H | T]) -> fun() -> {H, list_source(T)} end.

%% of_device/1 — a feed over a device this feed OWNS (File.feed opened it for
%% no one else), read a line at a time. The device is already a stateful
%% cursor, so this is the natural shape: nothing but the current line is ever
%% held, and reaching the end closes it.
of_device(Dev) -> start(device_source(Dev, true)).

%% of_handle/1 — the same over a device the CALLER owns (FileHandle.feed). It
%% shares the handle's position, and leaves closing to whoever opened it.
of_handle(Dev) -> start(device_source(Dev, false)).

device_source(Dev, Owned) ->
    fun() ->
        case file:read_line(Dev) of
            {ok, Line} ->
                %% iolist_to_binary, not list_to_binary: the device is opened
                %% in binary mode, so chomp answers a binary already.
                {iolist_to_binary(string:chomp(Line)), device_source(Dev, Owned)};
            _ ->
                case Owned of true -> file:close(Dev); false -> ok end,
                done
        end
    end.

of_stream(S) -> start(stream_source(S)).

stream_source({'Stream', T}) ->
    fun() ->
        case T() of
            done      -> done;
            {V, Next} -> {V, stream_source(Next)}
        end
    end.

feed_source({'Feed', Pid}) ->
    fun() ->
        case ask(Pid, pull) of
            done     -> done;
            {ok, V}  -> {V, feed_source({'Feed', Pid})}
        end
    end.

pull({'Feed', Pid}) ->
    case ask(Pid, pull) of
        done    -> 'None';
        {ok, V} -> {'Just', V}
    end;
pull(_) -> 'None'.

'spent?'({'Feed', Pid}) -> ask(Pid, spent);
'spent?'(_) -> true.

take(_, N) when N =< 0 -> [];
take({'Feed', Pid} = F, N) ->
    case ask(Pid, pull) of
        done    -> [];
        {ok, V} -> [V | take(F, N - 1)]
    end;
take(_, _) -> [].

%% drop/2 — discard N elements and answer the same feed: there is only ever the
%% one cursor to hand back.
drop(F, N) when N =< 0 -> F;
drop({'Feed', Pid} = F, N) ->
    case ask(Pid, pull) of
        done   -> F;
        {ok, _} -> drop(F, N - 1)
    end;
drop(F, _) -> F.

map(F, Fun) ->
    Source = feed_source(F),
    start(map_source(Source, Fun)).

map_source(Source, Fun) ->
    fun() ->
        case Source() of
            done      -> done;
            {V, Next} -> {Fun(V), map_source(Next, Fun)}
        end
    end.

filter(F, Pred) ->
    Source = feed_source(F),
    start(filter_source(Source, Pred)).

filter_source(Source, Pred) ->
    fun() -> scan(Source, Pred) end.

scan(Source, Pred) ->
    case Source() of
        done -> done;
        {V, Next} ->
            case Pred(V) of
                true  -> {V, filter_source(Next, Pred)};
                false -> scan(Next, Pred)
            end
    end.

each({'Feed', Pid} = F, Fun) ->
    case ask(Pid, pull) of
        done    -> 'unit';
        {ok, V} -> Fun(V), each(F, Fun)
    end;
each(_, _) -> 'unit'.

collect({'Feed', Pid} = F) ->
    case ask(Pid, pull) of
        done    -> [];
        {ok, V} -> [V | collect(F)]
    end;
collect(_) -> [].

%% toStream/1 — drains the feed and answers a stream over what it held.
%%
%% Eager where the walker's is lazy, because a BEAM stream cannot memoise:
%% forcing a lazy bridge twice would pull the feed twice and answer different
%% elements, while the walker's memoised cells answer the same ones. Draining
%% up front is what makes both backends agree, and it costs no more space than
%% the walker's stream ends up holding once fully walked — which is exactly
%% what asking a one-shot source to become replayable buys.
toStream(F) -> kex_intrinsic_stream:of_list(collect(F)).
