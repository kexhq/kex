%% Kex.Intrinsic.Char — BEAM primitive backend for the Char intrinsics.
%% A Char is the tagged tuple {'Char', Codepoint} (bare integers also
%% accepted for internal charlist call sites).
-module(kex_intrinsic_char).
-export([is_digit/1, is_alpha/1, is_space/1]).

is_digit({'Char', C}) -> is_digit(C);
is_digit(C) -> C >= $0 andalso C =< $9.
is_alpha({'Char', C}) -> is_alpha(C);
is_alpha(C) -> (C >= $A andalso C =< $Z) orelse (C >= $a andalso C =< $z).
is_space({'Char', C}) -> is_space(C);
is_space(C) -> lists:member(C, [$\s, $\t, $\n, $\r, $\v, $\f]).
