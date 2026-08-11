%% Kex.Intrinsic.Char — BEAM primitive backend for the Char intrinsics.
%% A Char is the tagged tuple {'Char', Codepoint} (bare integers also
%% accepted for internal charlist call sites).
-module(kex_intrinsic_char).
-export([is_digit/1, is_alpha/1, is_letter/1, is_upper/1, is_lower/1, is_space/1,
         codepoint/1]).

%% digit? stays ASCII on purpose: it backs the JSON number scanner, and JSON
%% numbers are ASCII by specification. The other predicates are Unicode
%% categories (kex_unicode_category), shared with the interpreter.
is_digit({'Char', C}) -> is_digit(C);
is_digit(C) -> C >= $0 andalso C =< $9.
codepoint({'Char', C}) -> C;
codepoint(C) -> C.
is_alpha({'Char', C}) -> is_alpha(C);
is_alpha(C) -> kex_unicode_category:is_letter(C).
is_letter(C) -> is_alpha(C).
is_upper({'Char', C}) -> is_upper(C);
is_upper(C) -> kex_unicode_category:is_upper(C).
is_lower({'Char', C}) -> is_lower(C);
is_lower(C) -> kex_unicode_category:is_lower(C).
is_space({'Char', C}) -> is_space(C);
is_space(C) -> kex_unicode_category:is_space(C).
