%% Kex.Intrinsic.Char — BEAM primitive backend for the Char intrinsics.
%% A Char is the tagged tuple {'Char', Codepoint} (bare integers also
%% accepted for internal charlist call sites).
-module(kex_intrinsic_char).
-export([is_digit/1, is_alpha/1, is_letter/1, is_upper/1, is_lower/1, is_space/1,
         codepoint/1]).

is_digit({'Char', C}) -> is_digit(C);
is_digit(C) -> C >= $0 andalso C =< $9.
codepoint({'Char', C}) -> C;
codepoint(C) -> C.
is_alpha({'Char', C}) -> is_alpha(C);
is_alpha(C) -> (C >= $A andalso C =< $Z) orelse (C >= $a andalso C =< $z).
is_letter(C) -> is_alpha(C).
is_upper({'Char', C}) -> is_upper(C);
is_upper(C) -> C >= $A andalso C =< $Z.
is_lower({'Char', C}) -> is_lower(C);
is_lower(C) -> C >= $a andalso C =< $z.
is_space({'Char', C}) -> is_space(C);
is_space(C) -> lists:member(C, [$\s, $\t, $\n, $\r, $\v, $\f]).
