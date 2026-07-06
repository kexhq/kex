%% Kex.Intrinsic.Char — BEAM primitive backend for the Char intrinsics.
%% Chars are integers on BEAM (no distinct runtime type), so these are integer-
%% range checks matching the guard-inlined logic in lower.cxx.
-module(kex_intrinsic_char).
-export([is_digit/1, is_alpha/1, is_space/1]).

is_digit(C) -> C >= $0 andalso C =< $9.
is_alpha(C) -> (C >= $A andalso C =< $Z) orelse (C >= $a andalso C =< $z).
is_space(C) -> lists:member(C, [$\s, $\t, $\n, $\r, $\v, $\f]).
