%% Kex.Intrinsic.String — BEAM primitive backend for the string intrinsics.
%% A Kex String is a UTF-8 binary; a Char is a bare codepoint integer. Thin
%% wrappers over the `string`/`unicode` BIFs (which preserve the binary
%% representation); the typed string stdlib lives in the Kex prelude
%% (src/prelude/string.kex). Receiver is the first argument.
-module(kex_intrinsic_string).
-export([upperCase/1, lowerCase/1, trim/1, split/1, split/2, chars/1,
          'startsWith?'/2, 'endsWith?'/2, 'contains?'/2]).

%% upperCase/lowerCase also take a bare Char (codepoint integer) — Char in,
%% Char out ('h'.upperCase → 'H').
upperCase(C) when is_integer(C) -> hd(string:to_upper([C]));
upperCase(S) -> string:uppercase(S).
lowerCase(C) when is_integer(C) -> hd(string:to_lower([C]));
lowerCase(S) -> string:lowercase(S).
trim(S)      -> string:trim(S).

%% chars/1 — the string's characters as a real [Char] (codepoint list).
chars(S) -> unicode:characters_to_list(S).

%% No-sep split — break into individual 1-char Strings ("hi" → ["h","i"]).
split(S) -> [<<C/utf8>> || C <- unicode:characters_to_list(S)].
split(S, Sep) -> string:split(S, Sep, all).

'startsWith?'(S, Pre) ->
    Sz = byte_size(Pre),
    byte_size(S) >= Sz andalso binary:part(S, 0, Sz) =:= Pre.
'endsWith?'(S, Suf) ->
    Sz = byte_size(Suf),
    byte_size(S) >= Sz andalso binary:part(S, byte_size(S) - Sz, Sz) =:= Suf.
%% contains?/2 — substring search (string:find returns nomatch when absent).
'contains?'(S, Sub) -> string:find(S, Sub) =/= nomatch.
