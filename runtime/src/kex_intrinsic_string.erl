%% Kex.Intrinsic.String — BEAM primitive backend for the string intrinsics.
%% A Kex String is a UTF-8 binary; a Char is a bare codepoint integer. Thin
%% wrappers over the `string`/`unicode` BIFs (which preserve the binary
%% representation); the typed string stdlib lives in the Kex prelude
%% (src/stdlib/string.kex). Receiver is the first argument.
-module(kex_intrinsic_string).
-export([upperCase/1, lowerCase/1, trim/1, split/1, split/2, replace/3, chars/1,
          'startsWith?'/2, 'endsWith?'/2, 'contains?'/2, fromCodepoint/1,
          bytes/1, fromBytes/1, graphemeCount/1,
          byteSize/1, byteAt/2, bytePart/3]).

%% upperCase/lowerCase also take a Char ({'Char', N}) — Char in, Char out
%% ('h'.upperCase → 'H'). A Char cannot expand, so it takes the SIMPLE
%% mapping (kex_unicode_case): `ß` stays `ß` as a Char, while the String
%% "ß" upcases to "SS" through the full mapping below. Same split as the
%% walker's.
%%
%% string:to_upper/1 used to back the Char path, but it is the pre-Unicode
%% function and only maps ISO-8859-1 — 'α'.upperCase and 'а'.upperCase were
%% silent no-ops on BEAM while the walker mapped them.
upperCase({'Char', C}) -> {'Char', kex_unicode_case:simple_upper(C)};
upperCase(C) when is_integer(C) -> kex_unicode_case:simple_upper(C);
upperCase(S) -> string:uppercase(S).
lowerCase({'Char', C}) -> {'Char', kex_unicode_case:simple_lower(C)};
lowerCase(C) when is_integer(C) -> kex_unicode_case:simple_lower(C);
lowerCase(S) -> string:lowercase(S).
trim(S)      -> string:trim(S).

%% chars/1 — the string's characters as a real [Char] (tagged tuples).
chars(S) -> kex_intrinsic_list:as_list(S).

%% bytes/1 — the string's UTF-8 encoding, one integer per byte. `chars` is the
%% TEXT view and this is the STORAGE view: "é" is one Char and two bytes.
bytes(S) when is_binary(S) -> binary_to_list(S);
bytes(_) -> [].

%% The STORAGE view without materialising it. `bytes` builds a list, so
%% `s.bytes.count` on a megabyte costs a million cons cells just to answer a
%% number; these three answer from the binary itself.
byteSize(S) when is_binary(S) -> byte_size(S);
byteSize(_) -> 0.

%% Out of range is 'None', matching `at` on every other indexed type rather
%% than the badarg binary:at/2 would raise.
byteAt(S, I) when is_binary(S), is_integer(I), I >= 0, I < byte_size(S) ->
    {'Just', binary:at(S, I)};
byteAt(_, _) -> 'None'.

%% Clamped rather than raising: a slice past the end yields what is there,
%% the way `take`/`drop` already behave.
bytePart(S, Off, Len) when is_binary(S), is_integer(Off), is_integer(Len) ->
    Size = byte_size(S),
    Start = min(max(Off, 0), Size),
    Count = min(max(Len, 0), Size - Start),
    binary:part(S, Start, Count);
bytePart(_, _, _) -> <<>>.

%% graphemeCount/1 — the string's length in extended grapheme clusters
%% (Unicode UAX #29): `"e\x{301}"` (e + combining acute) and `"\r\n"` each
%% count as one, where `count` (codepoints, via
%% kex_intrinsic_list:length/1) counts two. `string:length/1` IS the
%% grapheme-cluster count on BEAM — it is the wrong primitive for `count`
%% (see kex_intrinsic_list:length/1's comment) but the right one here.
graphemeCount(S) -> string:length(S).

%% fromBytes/1 — the inverse. A String is TEXT, so bytes outside 0..255 or a
%% malformed UTF-8 sequence answer None rather than building a binary that
%% would decode to replacement characters.
fromBytes(Values) when is_list(Values) ->
    case lists:all(fun(B) -> is_integer(B) andalso B >= 0 andalso B =< 255 end,
                   Values) of
        false -> 'None';
        true ->
            Bin = list_to_binary(Values),
            case unicode:characters_to_binary(Bin, utf8, utf8) of
                Valid when is_binary(Valid) -> {'Just', Valid};
                _ -> 'None'
            end
    end;
fromBytes(_) -> 'None'.

fromCodepoint(Value)
  when is_integer(Value), Value >= 0, Value =< 16#10ffff,
       not (Value >= 16#d800 andalso Value =< 16#dfff) ->
    {'Just', unicode:characters_to_binary([Value])};
fromCodepoint(_) -> 'None'.

%% No-sep split — break into individual 1-char Strings ("hi" → ["h","i"]).
split(S) -> [<<C/utf8>> || C <- unicode:characters_to_list(S)].
%% A Regex separator dispatches to the regex engine, so `str.split(re)` and
%% `str.split(",")` share one UFCS name — mirroring the same branch in
%% src/interpreter/stdlib/string.cxx. Constructing a Regex needs `using Regex`,
%% so this clause is unreachable without it.
split(S, {'Regex.Regex', _} = Regex) -> kex_intrinsic_regex:split(S, Regex, 0);
%% An empty separator is the string form of `.chars` — one part per character,
%% the same answer `split/1` gives. `string:split/3` returns the whole string
%% for it instead, which is where the walker and BEAM disagreed.
split(S, <<>>) -> split(S);
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
