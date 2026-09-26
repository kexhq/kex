%% Kex.Intrinsic.Fun — function-application primitives.
%%
%% applyItem/2 is the auto-splat used by Enumerable's default HOFs: a block may
%% be written `{ |x| }` over elements (List/Range) or `{ |k, v| }` over pairs
%% (Map). When the item is a 2-tuple and the block takes 2 args, spread it.
%%
%% applyIndexed/3 is the same idea for the indexed HOFs (eachIndexed /
%% mapIndexed). The index is always the LAST argument, so `|entry, I|` gets the
%% whole item and `|K, V, I|` gets a Map entry spread. A 1-arity block ignores
%% the index.
-module(kex_intrinsic_fun).
-export([applyFlexible/2, applyItem/2, applyIndexed/3, applyPredicate/2, truthy/1,
         convertTo/2, convertTo/3, items/1]).

%% A `Type` VALUE names its target too: `x.to(Type.of(y))`. Its name is a
%% binary, so it routes back through the atom-keyed clauses below.
convertTo(V, {'Type', Name, _Args, _Pure}) -> convertTo(V, binary_to_atom(Name, utf8));
convertTo({'Binary', V}, 'String') ->
    case unicode:characters_to_binary(V, utf8, utf8) of
        Valid when is_binary(Valid) -> {'Just', Valid};
        _ -> 'None'
    end;
convertTo(V, 'String') -> kex_io:to_string_optional(V);
convertTo(V, 'Integer') -> kex_intrinsic_number:to_integer(V);
convertTo(V, 'Float') -> kex_intrinsic_number:to_float(V);
convertTo(V, 'Byte') when is_integer(V), V >= 0, V =< 255 -> {'Just', V};
convertTo(_, 'Byte') -> 'None';
convertTo(V, 'Binary') when is_binary(V) -> {'Just', {'Binary', V}};
convertTo({'Binary', V}, 'Binary') -> {'Just', {'Binary', V}};
convertTo({'Range', _, _} = Range, 'List') -> {'Just', kex_intrinsic_range:items(Range)};
convertTo(V, 'List') when is_list(V) -> {'Just', V};
%% `"ok".to(Atom)` finds an atom that already EXISTS and never makes one:
%% atoms are never freed, so creating them from untrusted text can exhaust the
%% atom table. `Atom.from` is the creating form.
convertTo(V, 'Atom') when is_binary(V) ->
    try binary_to_existing_atom(V, utf8) of
        Atom -> {'Just', Atom}
    catch error:badarg -> 'None'
    end;
convertTo(V, 'Atom') when is_atom(V), V =/= true, V =/= false, V =/= 'None' ->
    {'Just', V};
convertTo(_, _) -> 'None'.

%% Base-aware Integer <-> String, `.to(String, radix: 16)`. Only that pair is
%% base dependent; anything else with a radix is a conversion that was not
%% meant, so it fails rather than silently ignoring the base.
convertTo(V, 'String', Radix)
  when is_integer(V), is_integer(Radix), Radix >= 2, Radix =< 36 ->
    %% integer_to_list/2 uppercases digits above 9; the walker's GMP get_str
    %% lowercases them, and the two have to agree.
    {'Just', unicode:characters_to_binary(string:lowercase(integer_to_list(V, Radix)))};
convertTo(V, 'Integer', Radix) when is_binary(V) ->
    case kex_intrinsic_integer:parse_in_base(V, Radix) of
        {ok, N} -> {'Just', N};
        error   -> 'None'
    end;
convertTo(_, _, _) -> 'None'.

%% A Kex call of a function VALUE whose arity differs from the argument count.
%% Emitted code tries `apply F(Args...)` first when `is_function(F, N)` holds;
%% this is the other case. A captured partial application (`~greet("Hello")`)
%% is one fun over every remaining argument, the shape Erlang callers such as
%% the HTTP server expect, so Kex code that fills it in over several calls
%% lands here:
%%
%%   - fewer arguments than the fun takes: a fun over the rest;
%%   - more: apply what it takes, then apply its result to the rest (a
%%     function returning a function, called with both groups at once).
%%
%% A value that is not a fun is applied anyway, so it fails as `badfun` just as
%% a direct call would.
applyFlexible(F, Args) when is_function(F) ->
    {arity, N} = erlang:fun_info(F, arity),
    Count = length(Args),
    if
        Count =:= N -> erlang:apply(F, Args);
        Count > N ->
            {Now, Rest} = lists:split(N, Args),
            applyFlexible(erlang:apply(F, Now), Rest);
        true -> partial(F, Args, N - Count)
    end;
applyFlexible(F, Args) ->
    erlang:apply(F, Args).

%% Erlang funs have a fixed arity, so the remaining count picks the clause.
partial(F, Got, 1) -> fun(A) -> erlang:apply(F, Got ++ [A]) end;
partial(F, Got, 2) -> fun(A, B) -> erlang:apply(F, Got ++ [A, B]) end;
partial(F, Got, 3) -> fun(A, B, C) -> erlang:apply(F, Got ++ [A, B, C]) end;
partial(F, Got, 4) -> fun(A, B, C, D) -> erlang:apply(F, Got ++ [A, B, C, D]) end;
partial(F, Got, 5) -> fun(A, B, C, D, E) -> erlang:apply(F, Got ++ [A, B, C, D, E]) end;
partial(F, Got, 6) ->
    fun(A, B, C, D, E, G) -> erlang:apply(F, Got ++ [A, B, C, D, E, G]) end;
partial(F, Got, _) ->
    erlang:error({badarity, {F, Got}}).

applyItem(F, Item) ->
    {arity, N} = erlang:fun_info(F, arity),
    case spreadable(Item, N) of
        true  -> erlang:apply(F, tuple_to_list(Item));
        false -> F(Item)
    end.

applyIndexed(F, Item, I) ->
    {arity, N} = erlang:fun_info(F, arity),
    case spreadable(Item, N - 1) of
        true -> erlang:apply(F, tuple_to_list(Item) ++ [I]);
        false when N =:= 1 -> F(Item);
        false -> F(Item, I)
    end.

%% Kex truthiness (Crystal semantics, see src/stdlib/truthyable.kex): only
%% `false`, `None` and `()` are falsy — every other value is truthy. The tree
%% walker has always worked this way; without this the BEAM predicates below
%% demanded a real boolean and died with `bad_filter`/`case_clause` on a block
%% that returned, say, an `Optional`.
truthy(false) -> false;
truthy('None') -> false;
truthy('Kex.Unit') -> false;
truthy(_) -> true.

%% applyItem/2 for a value used as a CONDITION rather than kept.
applyPredicate(F, Item) -> truthy(applyItem(F, Item)).

%% Should `Item` be spread across a block of `N` parameters?
%%
%% Only a genuine tuple of exactly N elements, and only for N > 1 — a
%% single-parameter block always receives the whole item.
%%
%% RECORDS ARE EXCLUDED, and that is the whole subtlety: a record is also a
%% tuple here (`{'Point', 1, 2}`), so without the check a two-parameter block
%% over a list of one-field records got the tag and the field rather than an
%% error. The walker has RecordValue and TupleValue as separate types and
%% rejects it, so this was a silent backend divergence. The registry
%% kex_io:register_display/2 fills is what makes the two distinguishable at
%% all — a bare `{a, 1}` and a record are otherwise the same term.
spreadable(Item, N) when is_tuple(Item), N > 1, tuple_size(Item) =:= N ->
    not is_record_term(Item);
spreadable(_, _) ->
    false.

is_record_term(Item) ->
    case element(1, Item) of
        Tag when is_atom(Tag) ->
            maps:is_key(Tag, persistent_term:get(kex_display_records, #{}));
        _ ->
            false
    end.

%% Normalize an Enumerable receiver to the item representation used by Kex
%% callbacks. Maps yield {Key, Value} pairs; lists and strings retain the same
%% coercion used by the HOF helpers above.
items(M) when is_map(M) -> maps:to_list(M);
items(L) -> kex_intrinsic_list:as_list(L).
