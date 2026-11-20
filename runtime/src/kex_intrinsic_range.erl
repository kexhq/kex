%% Ranges retain bounds; materialization is explicit through items/1.
%%
%% `a..b` is `{'Range', A, B}`, enumerated upward by one. A range with any
%% other step (`(1..10).by(2)`, `10.downTo(1)`, a reversed range) is
%% `{'Range', A, B, Step}`; a step of 1 is always stored in the short form,
%% so equal ranges are equal terms.
-module(kex_intrinsic_range).
-export([make/2, make/3, items/1, bounds/1, first/1, last/1, step/1, by/2, reverse/1,
         member/2]).
make(A, B) -> {'Range', A, B}.
make(A, B, 1) -> {'Range', A, B};
make(A, B, Step) -> {'Range', A, B, Step}.
bounds({'Range', A, B}) -> {A, B};
bounds({'Range', A, B, _}) -> {A, B}.
first({'Range', A, _}) -> A;
first({'Range', A, _, _}) -> A.
last({'Range', _, B}) -> B;
last({'Range', _, B, _}) -> B.
step({'Range', _, _}) -> 1;
step({'Range', _, _, Step}) -> Step.

%% The range with its step replaced by Step: its sign is the direction.
by(_, 0) -> erlang:error(<<"a range's step cannot be 0">>);
by(R, Step) when is_integer(Step) ->
    {A, B} = bounds(R),
    case enumerable(A, B) of
        true -> make(A, B, Step);
        false -> erlang:error(<<"Float ranges cannot be stepped">>)
    end.

%% The same elements in the opposite order, as a range: it starts at the
%% last element the step reaches, not at the written end.
reverse(R) ->
    {A, B} = bounds(R),
    Step = step(R),
    case enumerable(A, B) of
        false -> erlang:error(<<"Float ranges cannot be enumerated">>);
        true ->
            case count(code(A), code(B), Step) of
                0 -> R;
                N -> make(wrap(A, code(A) + (N - 1) * Step), A, -Step)
            end
    end.

%% Whether V is one of the range's elements; a Float range contains any
%% value between its bounds.
member(R, V) ->
    {A, B} = bounds(R),
    Step = step(R),
    case enumerable(A, B) of
        false -> V >= A andalso V =< B;
        true ->
            case comparable(A, V) of
                false -> false;
                true ->
                    I = code(V) - code(A),
                    I rem Step =:= 0 andalso I div Step >= 0
                        andalso I div Step < count(code(A), code(B), Step)
            end
    end.

items({'Range', _, _} = R) -> enumerate(R);
items({'Range', _, _, _} = R) -> enumerate(R);
items(L) when is_list(L) -> L.

enumerate(R) ->
    {A, B} = bounds(R),
    Step = step(R),
    case enumerable(A, B) of
        false -> erlang:error(<<"Float ranges cannot be enumerated">>);
        true ->
            case count(code(A), code(B), Step) of
                0 -> [];
                N -> [wrap(A, C) || C <- lists:seq(code(A), code(A) + (N - 1) * Step, Step)]
            end
    end.

%% How many elements from First towards Last by Step.
count(First, Last, Step) when Step > 0, First =< Last -> (Last - First) div Step + 1;
count(First, Last, Step) when Step < 0, First >= Last -> (First - Last) div -Step + 1;
count(_, _, _) -> 0.

enumerable({'Char', _}, {'Char', _}) -> true;
enumerable(A, B) -> is_integer(A) andalso is_integer(B).

comparable({'Char', _}, {'Char', _}) -> true;
comparable(A, V) -> is_integer(A) andalso is_integer(V).

code({'Char', C}) -> C;
code(I) -> I.

wrap({'Char', _}, C) -> {'Char', C};
wrap(_, I) -> I.
