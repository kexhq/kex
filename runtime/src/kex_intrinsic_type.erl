%% Kex.Intrinsic.Type — what a VALUE is, structurally.
%%
%% The fallback behind `Type.of` for everything the checker could not pin
%% down. kex_io:value_type_name/1 answers the same question as a flat string
%% for display; this builds the `Type { name, args }` record instead, so the
%% answer can be taken apart. Both consult the same display registry, which is
%% what lets a tagged tuple be recognised as a record or an ADT variant.
-module(kex_intrinsic_type).
-export(['ofValue'/1, 'fieldsOf'/1, 'constructorsOf'/1, 'updateRecord'/3,
         'fieldLayoutsOf'/1, 'constructorLayoutsOf'/1, register_layouts/2,
         'construct'/2, 'constructVariant'/3]).

'updateRecord'(Record, Field, Value) when is_tuple(Record), tuple_size(Record) > 0 ->
    Tag = element(1, Record),
    case record_fields(Tag) of
        Fields when is_list(Fields) ->
            case index_of(Field, Fields, 2) of
                {ok, Index} -> setelement(Index, Record, Value);
                error -> erlang:error({unknown_record_field, Tag, Field})
            end;
        _ -> erlang:error({field_assignment_requires_record, Record})
    end;
'updateRecord'(Record, _Field, _Value) ->
    erlang:error({field_assignment_requires_record, Record}).

index_of(_Wanted, [], _Index) -> error;
index_of(Wanted, [Wanted | _], Index) -> {ok, Index};
index_of(Wanted, [_ | Rest], Index) -> index_of(Wanted, Rest, Index + 1).

'ofValue'(X) -> type_of(X).

type_of(X) when is_binary(X) -> named(<<"String">>);
type_of(X) when is_integer(X) -> named(<<"Integer">>);
type_of(X) when is_float(X) -> named(<<"Float">>);
type_of(true) -> named(<<"Bool">>);
type_of(false) -> named(<<"Bool">>);
type_of({'Range', _, _}) -> named(<<"Range">>);
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
        _ ->
            %% Empty records also lower to bare atoms. Their nominal tag is
            %% still their type, including its module qualification.
            case record_fields(X) of
                [] -> named(atom_to_binary(X, utf8));
                _ -> named(<<"Atom">>)
            end
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
        _ -> [FieldName || {FieldName, _, _, _} <- 'fieldLayoutsOf'(Name)]
    end.

%% A sum type's constructor names, in declaration order.
'constructorsOf'(Name) ->
    [Tag || {Tag, _ArgTypes} <- 'constructorLayoutsOf'(Name)].

%% register_layouts/2 — called when a module loads, with each record's fields
%% (#{Tag => [{Name, Type, Optional, HasDefault, Default}, …]}, Default a
%% zero-argument fun computing the declared default, or `none`) and each sum type's
%% variants (#{Owner => [{Tag, [ArgType, …]}, …]}), in declaration order and
%% with the declared types as `Type` records. Only the compiler knows these;
%% they are what `Type.fields` and `Type.constructors` answer with.
register_layouts(Records, Adts) ->
    persistent_term:put(kex_record_layouts,
                        maps:merge(persistent_term:get(kex_record_layouts, #{}), Records)),
    persistent_term:put(kex_adt_layouts,
                        maps:merge(persistent_term:get(kex_adt_layouts, #{}), Adts)),
    ok.

'fieldLayoutsOf'(Name) ->
    [{FieldName, Type, Optional, HasDefault}
     || {FieldName, Type, Optional, HasDefault, _Default}
            <- field_layouts(layout(persistent_term:get(kex_record_layouts, #{}), Name))].

'constructorLayoutsOf'(Name) ->
    layout(persistent_term:get(kex_adt_layouts, #{}), Name).

%% A type's layout by the name `Type.of` reports. A type declared inside a
%% module is registered under one spelling ('Kex.Version') while a value may
%% report the other ("Version"): a qualified name also matches its bare last
%% segment, and a bare name the one qualified entry ending in it.
layout(Layouts, Name) ->
    case layout_entry(Layouts, Name) of
        {_Key, Found} -> Found;
        none -> []
    end.

%% layout/2 with the key it was found under: the record tag, or the sum
%% type's own name.
layout_entry(Layouts, Name) ->
    Key = binary_to_atom(Name, utf8),
    case maps:find(Key, Layouts) of
        {ok, Found} -> {Key, Found};
        error ->
            Bare = lists:last(binary:split(Name, <<".">>, [global])),
            BareKey = binary_to_atom(Bare, utf8),
            case maps:find(BareKey, Layouts) of
                {ok, Found} when Bare =/= Name -> {BareKey, Found};
                _ ->
                    Suffix = <<".", Name/binary>>,
                    case [{K, V} || {K, V} <- maps:to_list(Layouts),
                                    ends_with(atom_to_binary(K, utf8), Suffix)] of
                        [Only] -> Only;
                        _ -> none
                    end
            end
    end.

%% A layout registered before defaults were published has four elements.
field_layouts(Fields) ->
    [case Field of
         {_, _, _, _, _} -> Field;
         {FieldName, Type, Optional, HasDefault} ->
             {FieldName, Type, Optional, HasDefault, none}
     end || Field <- Fields].

%% construct/2 — a record of the type named Name from #{FieldName => Value}
%% (kexhq/kex#487). A missing field takes its default, or None when optional;
%% a missing required field, an unknown key and a value of the wrong type are
%% each an error naming the field.
'construct'(Name, Values) when is_map(Values) ->
    case layout_entry(persistent_term:get(kex_record_layouts, #{}), Name) of
        none -> {'Error', <<"Not a record type: ", Name/binary>>};
        {Tag, Layout} ->
            Fields = field_layouts(Layout),
            Known = [FieldName || {FieldName, _, _, _, _} <- Fields],
            case [K || K <- maps:keys(Values), not lists:member(K, Known)] of
                [Unknown | _] ->
                    {'Error', <<"Unknown field ", (quoted(Unknown))/binary,
                                " for ", Name/binary>>};
                [] -> construct_fields(Tag, Name, Fields, Values, [])
            end
    end;
'construct'(_Name, _Values) ->
    {'Error', <<"Expected a map of field values">>}.

construct_fields(Tag, _Name, [], _Values, []) -> {'Ok', Tag};
construct_fields(Tag, _Name, [], _Values, Acc) ->
    {'Ok', list_to_tuple([Tag | lists:reverse(Acc)])};
construct_fields(Tag, Name, [{Field, Type, Optional, _HasDefault, Default} | Rest],
                 Values, Acc) ->
    case maps:find(Field, Values) of
        {ok, Value} ->
            case accepts(Type, Value) of
                true -> construct_fields(Tag, Name, Rest, Values, [Value | Acc]);
                false ->
                    {'Error', <<"Field ", (quoted(Field))/binary, " of ", Name/binary,
                                " expects ", (type_name(Type))/binary, ", got ",
                                (type_name(type_of(Value)))/binary>>}
            end;
        error when is_function(Default, 0) ->
            construct_fields(Tag, Name, Rest, Values, [Default() | Acc]);
        error when Optional ->
            construct_fields(Tag, Name, Rest, Values, ['None' | Acc]);
        error ->
            {'Error', <<"Missing field ", (quoted(Field))/binary, " for ",
                        Name/binary>>}
    end.

%% constructVariant/3 — the constructor Ctor of the sum type named Name,
%% applied to Args (kexhq/kex#509). Variant tags are bare at run time.
'constructVariant'(Name, Ctor, Args) when is_list(Args) ->
    case layout_entry(persistent_term:get(kex_adt_layouts, #{}), Name) of
        none -> {'Error', <<"Not a sum type: ", Name/binary>>};
        {_Owner, Variants} ->
            case lists:keyfind(Ctor, 1, Variants) of
                false ->
                    {'Error', <<Name/binary, " has no constructor ", Ctor/binary>>};
                {Ctor, ArgTypes} when length(ArgTypes) =/= length(Args) ->
                    {'Error', <<Ctor/binary, " takes ",
                                (integer_to_binary(length(ArgTypes)))/binary,
                                " argument(s), got ",
                                (integer_to_binary(length(Args)))/binary>>};
                {Ctor, ArgTypes} ->
                    case [{T, A} || {T, A} <- lists:zip(ArgTypes, Args),
                                    not accepts(T, A)] of
                        [{T, A} | _] ->
                            {'Error', <<Ctor/binary, " expects ", (type_name(T))/binary,
                                        ", got ", (type_name(type_of(A)))/binary>>};
                        [] when Args =:= [] -> {'Ok', binary_to_atom(Ctor, utf8)};
                        [] -> {'Ok', list_to_tuple([binary_to_atom(Ctor, utf8) | Args])}
                    end
            end
    end.

%% Whether Value can stand where the declared Type is written. What cannot be
%% told at run time — a type parameter, an alias, a union, a trait — is
%% accepted; a known type has to match by name.
accepts({'Type', <<"Any">>, _, _}, _Value) -> true;
accepts({'Type', <<"Option">>, Args, _}, Value) ->
    case Value of
        'None' -> true;
        {'Just', Inner} ->
            case Args of
                [Arg] -> accepts(Arg, Inner);
                _ -> true
            end;
        _ -> false
    end;
accepts({'Type', <<"List">>, [Element], _}, Value) when is_list(Value) ->
    lists:all(fun(E) -> accepts(Element, E) end, Value);
accepts({'Type', <<"Map">>, [KeyType, ValueType], _}, Value) when is_map(Value) ->
    lists:all(fun({K, V}) -> accepts(KeyType, K) andalso accepts(ValueType, V) end,
              maps:to_list(Value));
accepts({'Type', Name, _, _}, Value) ->
    {'Type', Actual, _, _} = type_of(Value),
    same_type(Name, Actual) orelse not checkable(Name).

same_type(Name, Name) -> true;
same_type(Declared, Actual) ->
    ends_with(Actual, <<".", Declared/binary>>) orelse
        ends_with(Declared, <<".", Actual/binary>>).

checkable(Name) ->
    lists:member(Name, [<<"String">>, <<"Integer">>, <<"Float">>, <<"Bool">>,
                        <<"Char">>, <<"List">>, <<"Map">>, <<"Range">>,
                        <<"Result">>, <<"Tuple">>, <<"Function">>]) orelse
        layout(persistent_term:get(kex_record_layouts, #{}), Name) =/= [] orelse
        layout(persistent_term:get(kex_adt_layouts, #{}), Name) =/= [].

type_name({'Type', Name, [], _}) -> Name;
type_name({'Type', Name, Args, _}) ->
    iolist_to_binary([Name, "<", lists:join(", ", [type_name(A) || A <- Args]), ">"]).

quoted(Field) when is_binary(Field) -> <<"\"", Field/binary, "\"">>;
quoted(Field) -> iolist_to_binary(io_lib:format("~p", [Field])).

ends_with(Binary, Suffix) ->
    Size = byte_size(Binary) - byte_size(Suffix),
    Size >= 0 andalso binary:part(Binary, Size, byte_size(Suffix)) =:= Suffix.

record_fields(Tag) when is_atom(Tag) ->
    maps:get(Tag, persistent_term:get(kex_display_records, #{}), undefined);
record_fields(_) -> undefined.

variant_metadata(Tag) when is_atom(Tag) ->
    maps:get(Tag, persistent_term:get(kex_display_variants, #{}), undefined);
variant_metadata(_) -> undefined.

named(Name) -> {'Type', Name, [], true}.
with(Name, Args) -> {'Type', Name, Args, true}.
