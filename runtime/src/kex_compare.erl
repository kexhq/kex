%% kex_compare — the ordering behind the no-argument `sort`, `min` and `max`.
%%
%% Those three used to be `lists:sort/1` and friends outright, which is Erlang
%% TERM order. For numbers, strings and chars term order IS the language's
%% order, so nothing looked wrong; for a record it compares the tuple field by
%% field, ignoring a `Comparable` implementation entirely. "Ascending natural
%% order" then meant "by whichever field happens to come first", which even
%% looks plausible when that is the field you meant (kexhq/kex#283).
%%
%% The tree walker has always dispatched here — `compareVia` in
%% src/interpreter/stdlib/list.cxx calls the receiver's `compare` for a record
%% or variant — so this is the two backends agreeing, not a new feature.
%%
%% The owning module of a record tag comes from the registry each compiled
%% unit populates through kex_io:register_method_owners/1. A type with no
%% `compare` of its own, or one whose comparison does not answer an `Ordering`,
%% falls back to term order exactly as before.
-module(kex_compare).
-export([compare/2, less/2, less_or_equal/2, greater/2, min/2, max/2]).

%% 'Less' | 'Equal' | 'Greater', the Ordering ADT's nullary variants.
compare(A, B) ->
    case declared_ordering(A, B) of
        undefined when A < B -> 'Less';
        undefined when A > B -> 'Greater';
        undefined -> 'Equal';
        Ordering -> Ordering
    end.

less(A, B) -> compare(A, B) =:= 'Less'.
%% lists:sort/2 wants an `=<` ordering function: returning false for equal
%% elements makes the sort UNSTABLE (equal elements come back reversed), which
%% is not what either backend's stable sort promises.
less_or_equal(A, B) -> compare(A, B) =/= 'Greater'.
greater(A, B) -> compare(A, B) =:= 'Greater'.

min(A, B) -> case less(B, A) of true -> B; false -> A end.
max(A, B) -> case greater(B, A) of true -> B; false -> A end.

%% Two values of the SAME tagged type, compared by that type's own `compare`.
%% Different tags have no declared order between them, so they keep term
%% order — which is what makes a heterogeneous list sort deterministically.
declared_ordering(A, B)
  when is_tuple(A), tuple_size(A) > 0, is_tuple(B), tuple_size(B) > 0 ->
    same_type(element(1, A), element(1, B), A, B);
%% A NULLARY ADT variant is a bare atom, so there is no tuple to read a tag
%% off — the atom IS the tag. `Low`/`Medium`/`High` sort by their type's
%% `compare` this way instead of alphabetically.
declared_ordering(A, B) when is_atom(A), is_atom(B) ->
    same_type(A, B, A, B);
declared_ordering(_, _) -> undefined.

%% Both tags must belong to one type before its `compare` is asked: the
%% registry maps every variant of an ADT to the same module, so two DIFFERENT
%% variants of one type do reach it, while unrelated tags do not.
same_type(TagA, TagB, A, B) when is_atom(TagA), is_atom(TagB) ->
    case owner(TagA) of
        undefined -> undefined;
        Module ->
            case owner(TagB) of
                Module -> dispatch(Module, A, B);
                _ -> undefined
            end
    end;
same_type(_, _, _, _) -> undefined.

owner(Tag) ->
    maps:get(Tag, persistent_term:get(kex_method_owners, #{}), undefined).

dispatch(Module, A, B) ->
    _ = code:ensure_loaded(Module),
    case erlang:function_exported(Module, compare, 2) of
        false -> undefined;
        true ->
            try ordering(Module:compare(A, B))
            catch _:_ -> undefined
            end
    end.

ordering('Less') -> 'Less';
ordering('Equal') -> 'Equal';
ordering('Greater') -> 'Greater';
%% Anything else — a crash from a module whose `compare` only answers for
%% OTHER types, a value that is not an Ordering — means "no declared order",
%% and term order takes over exactly as it did before.
ordering(_) -> undefined.
