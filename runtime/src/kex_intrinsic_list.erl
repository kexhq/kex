%% Kex.Intrinsic.List — BEAM primitive backend for the list intrinsics.
%%
%% The ONLY place Core Erlang list BIFs are named for the stdlib. The typed
%% list stdlib lives in src/stdlib/list.kex, which calls
%% `Kex.Intrinsic.List.<fn>`; the compiler maps that module path to a plain
%% cross-module call here. Adding a primitive is a one-line function.
-module(kex_intrinsic_list).
-export([reverse/1, sort/1, sort/2, uniq/1, flatten/1, take/2, drop/2, zip/2, push/2,
         sum/1, product/1, indexOf/2, at/2, foldLeft/3, min/1, max/1, length/1,
         first/1, last/1,
         join/1, join/2, partition/2, member/2, as_list/1,
         map/2, filter/2, each/2, find/2, flatMap/2, reject/2,
         'all?'/2, 'any?'/2, count/2,
         minBy/2, maxBy/2,
         get/2, get/3, repack/2]).

%% as_list/1 — a String receiver (UTF-8 binary) as its [Char] list of tagged
%% {'Char', N} tuples; everything else unchanged. The list HOF lowerings wrap
%% their receiver in this so `"h3llo".filter(&alpha?)` etc. work; repack/2
%% turns the result back into a String, since the two are distinct types.
as_list(B) when is_binary(B) ->
    [{'Char', C} || C <- unicode:characters_to_list(B)];
as_list(L) -> L.

%% Receiver is always the first argument (Kex UFCS convention). A Kex String
%% is a UTF-8 binary, and the prelude's String methods reuse several List
%% intrinsics (length/at/reverse/member) — so those take binaries too.
%% Every receiver goes through as_list/1, so each function transparently
%% takes a String binary as its [Char] codepoint list.
reverse(B) when is_binary(B) ->
    unicode:characters_to_binary(lists:reverse(unicode:characters_to_list(B)));
reverse(L) -> lists:reverse(L).
%% repack/2 keeps the string-preserving contract: an operation on a String
%% answers with a String, so `"hello".sort` is "ehllo" and not ['e','h',...].
%% A String and a [Char] are DIFFERENT types, so handing back the list would
%% both contradict the declared signature and disagree with the walker.
repack(Src, Result) when is_binary(Src) -> repack_chars(Result);
repack(_Src, Result) -> Result.

repack_chars(L) when is_list(L) ->
    case lists:all(fun({'Char', C}) -> is_integer(C); (_) -> false end, L) of
        true -> unicode:characters_to_binary([C || {'Char', C} <- L]);
        false -> L
    end;
repack_chars(X) -> X.

sort(L)    -> repack(L, lists:sort(as_list(L))).
%% sort/2 — custom comparator. Kex passes (list, fun), Erlang's lists:sort/2
%% takes (fun, list), so swap the argument order.
sort(L, Fun) -> repack(L, lists:sort(Fun, as_list(L))).
%% Order-preserving, keeping the FIRST occurrence — `uniq` is a filter, not a
%% sort, so lists:usort/1 is the wrong primitive here.
uniq(L)    -> repack(L, uniq_seen(as_list(L), #{}, [])).
uniq_seen([], _Seen, Acc) -> lists:reverse(Acc);
uniq_seen([H | T], Seen, Acc) ->
    case maps:is_key(H, Seen) of
        true -> uniq_seen(T, Seen, Acc);
        false -> uniq_seen(T, Seen#{H => true}, [H | Acc])
    end.
flatten(L) -> lists:flatten(as_list(L)).
take(L, N) -> repack(L, lists:sublist(as_list(L), N)).
drop(L, N) ->
    L2 = as_list(L),
    repack(L, lists:nthtail(min(N, erlang:length(L2)), L2)).
zip(L, R)  -> zip_short(as_list(L), as_list(R)).
zip_short([], _) -> [];
zip_short(_, []) -> [];
zip_short([A|As], [B|Bs]) -> [{A, B} | zip_short(As, Bs)].
push(L, X) -> as_list(L) ++ [X].
sum(L)     -> lists:sum(as_list(L)).
product(L) -> list_product(as_list(L)).
indexOf(L, X) -> index_of(X, as_list(L)).
at(L, I)   -> list_at(L, I).
get(L, I)  -> list_at(L, I).
get(L, I, Default) -> list_get(L, I, Default).
%% foldLeft/3 — the universal left fold backing Enumerable.reduce (and so every
%% HOF derived from it). Kex's reducer takes (acc, elem); Erlang's lists:foldl
%% takes fun(elem, acc), so swap the argument order at the boundary.
foldLeft(L, Acc, Fun) -> lists:foldl(fun(Elem, A) -> Fun(A, Elem) end, Acc, as_list(L)).

%% partition/2 — splits into {Matching, NonMatching} per predicate. Kex is
%% receiver-first; Erlang's lists:partition is fun-first, so swap.
partition(L, Fun) ->
    {Yes, No} = lists:partition(
        fun(X) -> kex_intrinsic_fun:applyPredicate(Fun, X) end, as_list(L)),
    {repack(L, Yes), repack(L, No)}.

%% member/2 — element membership check, backing `.in?` on Integer/Float/Char.
%% Element is the receiver, container is the arg.
member(Elem, Container) when is_binary(Container) ->
    lists:member(Elem, as_list(Container));
member(Elem, Container) -> lists:member(Elem, Container).

%% first/1, last/1 — the first/last element wrapped in Just, or None for [].
%% Backing for the prelude's `first`/`last` (pattern-based impls hit the
%% one-element-pattern semantics; a direct primitive is simpler and O(1)/O(n)).
first(B) when is_binary(B) -> first(as_list(B));
first([])      -> 'None';
first([X | _]) -> {'Just', X}.
last(B) when is_binary(B) -> last(as_list(B));
last([])       -> 'None';
last(L)        -> {'Just', lists:last(L)}.

%% min/1, max/1 — the smallest/largest element wrapped in Just, or None for [].
%% lists:min/max crash on the empty list, so guard here (the prelude's old
%% recursive impl was non-tail and returned this same Just/None form).
min(B) when is_binary(B) -> min(as_list(B));
min([]) -> 'None';
min(L)  -> {'Just', lists:min(L)}.
max(B) when is_binary(B) -> max(as_list(B));
max([]) -> 'None';
max(L)  -> {'Just', lists:max(L)}.

%% length/1 — element count, backing List.count (was a non-tail `1 + xs.count`
%% recursion in the prelude). Also backs String.count (codepoint count) —
%% `string:length/1` is the wrong primitive here: it counts *extended
%% grapheme clusters* (Unicode UAX #29), which folds a `\r\n` pair into a
%% single unit and silently undercounts any string containing one. Decoding
%% through `unicode:characters_to_list/1`, the same way `as_list/1` above
%% does for `.chars`, counts codepoints instead, so `String.count` agrees
%% with `String.chars.count` the way the interpreter backend already does.
length(B) when is_binary(B) -> erlang:length(unicode:characters_to_list(B));
length(L) -> erlang:length(L).

%% join/1,2 — string join for [String|Char] lists, producing a String (UTF-8
%% binary). Tagged Chars normalize to their codepoints; binaries and nested
%% lists are chardata already. The old prelude form was non-tail recursive
%% (`x + sep + xs.join(sep)`).
join(L) ->
    iolist_to_binary([kex_io:to_string_bin(E) || E <- as_list(L)]).
join(L, Sep) ->
    Text = [kex_io:to_string_bin(E) || E <- as_list(L)],
    iolist_to_binary(lists:join(kex_io:to_string_bin(Sep), Text)).

%% list_at/2 — `list[i]`, `list.at(i)`, `list.get(i)`: the element wrapped in
%% Just, or None when the index is out of range — the same shape Map.get's
%% 2-arg form answers with.
list_at(List, Idx) ->
    case list_get(List, Idx, 'kex_missing') of
        'kex_missing' -> 'None';
        Element -> {'Just', Element}
    end.

%% list_get/3 — `list.get(i, default)`, the total form: the raw element, or
%% Default when the index is out of range.
list_get(Bin, Idx, Default) when is_binary(Bin) ->
    list_get(as_list(Bin), Idx, Default);
list_get(List, Idx, _Default) when is_integer(Idx), Idx >= 0, Idx < erlang:length(List) ->
    lists:nth(Idx + 1, List);
list_get(_List, _Idx, Default) ->
    Default.

%% list.indexOf(value) -> Just(index) | None (0-based). No lists:indexOf/2 BIF.
index_of(Value, List) -> index_of(Value, List, 0).
index_of(_Value, [], _I) -> 'None';
index_of(Value, [Value | _], I) -> {'Just', I};
index_of(Value, [_ | Rest], I) -> index_of(Value, Rest, I + 1).

%% list.product — no lists:product/1 BIF.
list_product(List) -> lists:foldl(fun(E, A) -> A * E end, 1, List).

%% HOF intrinsics — BIF-backed versions that override the Enumerable trait's
%% reduce-based defaults. Handle String (binary) receivers via as_list/1.
%% Each HOF routes its block through kex_intrinsic_fun:applyItem/2 so that
%% 2-param blocks (`{ |k, v| ... }`) auto-splat pair elements (e.g. when
%% mapping over Map.entries), matching the Enumerable trait defaults and
%% the interpreter's block invocation.
map(L, Fun)    -> repack(L, [kex_intrinsic_fun:applyItem(Fun, I) || I <- as_list(L)]).
filter(L, Fun) -> repack(L, lists:filter(fun(X) -> kex_intrinsic_fun:applyPredicate(Fun, X) end, as_list(L))).
each(L, Fun)   -> lists:foreach(fun(X) -> kex_intrinsic_fun:applyItem(Fun, X) end, as_list(L)), 'None'.
%% A block returning a String contributes its Chars, not the binary itself —
%% lists:flatmap/2 cannot walk a binary.
flatMap(L, Fun) ->
    lists:flatmap(fun(X) -> as_list(kex_intrinsic_fun:applyItem(Fun, X)) end,
                  as_list(L)).
reject(L, Fun) -> repack(L, lists:filter(fun(X) -> not kex_intrinsic_fun:applyPredicate(Fun, X) end, as_list(L))).
'all?'(L, Fun)  -> lists:all(fun(X) -> kex_intrinsic_fun:applyPredicate(Fun, X) end, as_list(L)).
'any?'(L, Fun)  -> lists:any(fun(X) -> kex_intrinsic_fun:applyPredicate(Fun, X) end, as_list(L)).
find(L, Fun)   ->
    case lists:search(fun(X) -> kex_intrinsic_fun:applyPredicate(Fun, X) end, as_list(L)) of
        {value, V} -> {'Just', V};
        false      -> 'None'
    end.
count(L, Fun)  -> erlang:length(lists:filter(fun(X) -> kex_intrinsic_fun:applyPredicate(Fun, X) end, as_list(L))).

minBy(L, F) -> extreme_by(as_list(L), F, fun(A, B) -> A < B end).
maxBy(L, F) -> extreme_by(as_list(L), F, fun(A, B) -> A > B end).

extreme_by([], _F, _Wins) -> 'None';
extreme_by([H | T], F, Wins) ->
    Seed = {H, kex_intrinsic_fun:applyItem(F, H)},
    {Best, _} = lists:foldl(fun(I, {B, BK}) ->
        K = kex_intrinsic_fun:applyItem(F, I),
        case Wins(K, BK) of true -> {I, K}; false -> {B, BK} end
    end, Seed, T),
    {'Just', Best}.
