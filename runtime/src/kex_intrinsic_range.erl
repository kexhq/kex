%% Ranges retain bounds; materialization is explicit through items/1.
-module(kex_intrinsic_range).
-export([make/2, items/1, bounds/1, first/1, last/1]).
make(A, B) -> {'Range', A, B}.
bounds({'Range', A, B}) -> {A, B}.
first({'Range', A, _}) -> A.
last({'Range', _, B}) -> B.
items({'Range', {'Char', A}, {'Char', B}}) ->
    [{'Char', C} || C <- lists:seq(A, B)];
items({'Range', A, B}) when is_integer(A), is_integer(B) ->
    case A > B of true -> []; false -> lists:seq(A, B) end;
items({'Range', _, _}) -> erlang:error({kex_error, <<"Float ranges cannot be enumerated">>});
items(L) when is_list(L) -> L.
