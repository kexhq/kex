%% Kex.Intrinsic.Type — what a VALUE is, structurally.
%%
%% The fallback behind `Type.of` for everything the checker could not pin
%% down. kex_io:value_type_name/1 answers the same question as a flat string
%% for display; this builds the `Type { name, args }` record instead, so the
%% answer can be taken apart. Both consult the same display registry, which is
%% what lets a tagged tuple be recognised as a record or an ADT variant.
-module(kex_intrinsic_type).
-export(['ofValue'/1, 'fieldsOf'/1, 'constructorsOf'/1]).

'ofValue'(X) -> type_of(X).

type_of(X) when is_binary(X) -> named(<<"String">>);
type_of(X) when is_integer(X) -> named(<<"Integer">>);
type_of(X) when is_float(X) -> named(<<"Float">>);
type_of(true) -> named(<<"Bool">>);
type_of(false) -> named(<<"Bool">>);
type_of({'Char', _}) -> named(<<"Char">>);
type_of([]) -> with(<<"List">>, [named(<<"?">>)]);
type_of(X) when is_list(X) -> with(<<"List">>, [element_type(X)]);
type_of(X) when is_map(X), map_size(X) =:= 0 -> named(<<"Map">>);
type_of(X) when is_map(X) ->
    %% Same rule as a list: report key/value types when every entry agrees.
    with(<<"Map">>, [element_type(maps:keys(X)), element_type(maps:values(X))]);
type_of({'Just', V}) -> with(<<"Option">>, [type_of(V)]);
type_of({'Some', V}) -> with(<<"Option">>, [type_of(V)]);
type_of('None') -> with(<<"Option">>, [named(<<"?">>)]);
type_of({'Ok', V}) -> with(<<"Result">>, [type_of(V), named(<<"?">>)]);
type_of({'Error', V}) -> with(<<"Result">>, [named(<<"?">>), type_of(V)]);
type_of(X) when is_tuple(X), tuple_size(X) > 0 ->
    Tag = element(1, X),
    Arity = tuple_size(X) - 1,
    case variant_metadata(Tag) of
        {Arity, Owner} -> named(atom_to_binary(Owner, utf8));
        _ ->
            case is_atom(Tag) andalso record_fields(Tag) of
                Fields when is_list(Fields), length(Fields) =:= Arity ->
                    named(atom_to_binary(Tag, utf8));
                _ ->
                    %% A plain tuple: its type IS its element types.
                    with(<<"Tuple">>, [type_of(E) || E <- tuple_to_list(X)])
            end
    end;
type_of(X) when is_atom(X) ->
    %% A nullary constructor lowers to a bare atom; the registry says which
    %% ADT it belongs to.
    case variant_metadata(X) of
        {0, Owner} -> named(atom_to_binary(Owner, utf8));
        _ -> named(<<"Atom">>)
    end;
type_of(X) when is_function(X) -> named(<<"Function">>);
type_of(X) when is_pid(X) -> named(<<"Process">>);
type_of(_) -> named(<<"Any">>).

%% A list's element type, when every element agrees on it.
element_type([H | T]) ->
    Head = type_of(H),
    case lists:all(fun(E) -> type_of(E) =:= Head end, T) of
        true -> Head;
        false -> named(<<"Any">>)
    end.

'fieldsOf'(Name) ->
    case record_fields(binary_to_atom(Name, utf8)) of
        Fields when is_list(Fields) ->
            [atom_to_binary(F, utf8) || F <- Fields];
        _ -> []
    end.

%% The inverse of the variant registry: every tag whose owner is this type,
%% in registration order.
'constructorsOf'(Name) ->
    Owner = binary_to_atom(Name, utf8),
    Variants = persistent_term:get(kex_display_variants, #{}),
    [atom_to_binary(Tag, utf8)
     || {Tag, {_Arity, TagOwner}} <- maps:to_list(Variants), TagOwner =:= Owner].

record_fields(Tag) when is_atom(Tag) ->
    maps:get(Tag, persistent_term:get(kex_display_records, #{}), undefined);
record_fields(_) -> undefined.

variant_metadata(Tag) when is_atom(Tag) ->
    maps:get(Tag, persistent_term:get(kex_display_variants, #{}), undefined);
variant_metadata(_) -> undefined.

named(Name) -> {'Type', Name, [], true}.
with(Name, Args) -> {'Type', Name, Args, true}.
