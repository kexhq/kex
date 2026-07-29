%% Kex.Intrinsic.String — BEAM primitive backend for the string intrinsics.
%% A Kex String is a UTF-8 binary; a Char is a bare codepoint integer. Thin
%% wrappers over the `string`/`unicode` BIFs (which preserve the binary
%% representation); the typed string stdlib lives in the Kex prelude
%% (src/prelude/string.kex). Receiver is the first argument.
-module(kex_intrinsic_string).
-export([upperCase/1, lowerCase/1, trim/1, split/1, split/2, replace/3, chars/1,
          'startsWith?'/2, 'endsWith?'/2, 'contains?'/2]).

%% upperCase/lowerCase also take a Char ({'Char', N}) — Char in, Char out
%% ('h'.upperCase → 'H').
upperCase({'Char', C}) -> {'Char', hd(string:to_upper([C]))};
upperCase(C) when is_integer(C) -> hd(string:to_upper([C]));
upperCase(S) -> string:uppercase(S).
lowerCase({'Char', C}) -> {'Char', hd(string:to_lower([C]))};
lowerCase(C) when is_integer(C) -> hd(string:to_lower([C]));
lowerCase(S) -> string:lowercase(S).
trim(S)      -> string:trim(S).

%% chars/1 — the string's characters as a real [Char] (tagged tuples).
chars(S) -> kex_intrinsic_list:as_list(S).

%% No-sep split — break into individual 1-char Strings ("hi" → ["h","i"]).
split(S) -> [<<C/utf8>> || C <- unicode:characters_to_list(S)].
%% A Regex separator dispatches to the regex engine, so `str.split(re)` and
%% `str.split(",")` share one UFCS name — mirroring the same branch in
%% src/interpreter/stdlib/string.cxx. Constructing a Regex needs `using Regex`,
%% so this clause is unreachable without it.
split(S, {'Regex', _} = Regex) -> kex_intrinsic_regex:split(S, Regex, 0);
split(S, Sep) -> string:split(S, Sep, all).

%% Global literal replacement. Empty pattern follows Ruby's boundary
%% semantics: "abc".replace("", "-") => "-a-b-c-".
replace(S, <<>>, Replacement) ->
    Chars = unicode:characters_to_list(S),
    iolist_to_binary(
      [Replacement,
       [[unicode:characters_to_binary([C]), Replacement] || C <- Chars]]);
replace(S, Pattern, Replacement) ->
    binary:replace(S, Pattern, Replacement, [global]).

'startsWith?'(S, Pre) ->
    Sz = byte_size(Pre),
    byte_size(S) >= Sz andalso binary:part(S, 0, Sz) =:= Pre.
'endsWith?'(S, Suf) ->
    Sz = byte_size(Suf),
    byte_size(S) >= Sz andalso binary:part(S, byte_size(S) - Sz, Sz) =:= Suf.
%% contains?/2 — substring search (string:find returns nomatch when absent).
'contains?'(S, Sub) -> string:find(S, Sub) =/= nomatch.
