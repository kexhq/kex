%% Kex.Intrinsic.Regex — BEAM primitive backend for the regex intrinsics.
%% The typed Regex stdlib lives in src/stdlib/regex.kex. Receiver is the first
%% argument. Must match src/interpreter/stdlib/regex.cxx behaviour exactly —
%% the decisions both backends implement are documented inline below.
%%
%% Erlang's `re` is built on PCRE, the same engine family the interpreter uses
%% through PCRE2, so the pattern *language* agrees for free. What does NOT
%% agree for free, and is handled explicitly below:
%%
%%   1. `[unicode, ucp]` must be passed or \d/\w silently mean ASCII only.
%%   2. `{capture, all, binary}` renders a non-participating group and a group
%%      that matched "" both as <<>>. Only `index` mode distinguishes them
%%      ({-1,0} vs a real offset), and that distinction is the Match contract.
%%   3. Named captures come back from `all_names` sorted by NAME with the names
%%      stripped; they must be zipped with re:inspect/2's namelist, which is
%%      also sorted. Here they are resolved to group NUMBERS instead, so the
%%      name and the index address the same capture.
%%   4. Offsets are byte offsets into UTF-8; Kex strings are logical text, so
%%      every offset crossing into Kex is converted to a character offset.
-module(kex_intrinsic_regex).
-export([compile/1, tag/2, validate/1, quote/1, matches/2, 'matches?'/2,
         scan/2, replace/3, split/2, split/3]).

%% Pinned on both backends. Without ucp, `\d` matches only [0-9] here while
%% PCRE2_UCP makes it match Arabic-Indic digits in the interpreter.
-define(COMPILE_OPTS, [unicode, ucp]).

%% ---------------------------------------------------------------------------
%% Compiling
%% ---------------------------------------------------------------------------

%% Regex is the record {'Regex.Regex', Source} — it carries only its pattern source;
%% the compiled form is derived on use (and cached by the VM's own re cache).
%% A compiled {re_pattern,...} must never be embedded in an artifact, since it
%% bakes in the host's PCRE version.
%%
%% compile(Source) -> {'Ok', Regex} | {'Error', RegexError}
%% RegexError is the record {'Regex.RegexError', Source, Position, Message}.
compile(Source) when is_binary(Source) ->
    case re:compile(Source, ?COMPILE_OPTS) of
        {ok, _} -> {'Ok', {'Regex.Regex', Source}};
        {error, {Reason, Position}} ->
            {'Error', {'Regex.RegexError', Source,
                       char_offset(Source, Position),
                       unicode:characters_to_binary(Reason)}}
    end;
compile(_) ->
    {'Error', {'Regex.RegexError', <<>>, 0, <<"regex expects a String">>}}.

%% tag(Parts, Values) -> Regex — the tagged-literal ABI. `` regex`\d+` ``
%% lowers to regex(Parts, Values) and returns a BARE Regex, not a Result: the
%% pattern is known at compile time, so it cannot fail. Interpolated values are
%% escaped per character, so a value contributes literal text and can never
%% inject pattern syntax.
tag(Parts, Values) ->
    Source = iolist_to_binary(splice(Parts, Values)),
    case re:compile(Source, ?COMPILE_OPTS) of
        {ok, _} -> {'Regex.Regex', Source};
        {error, {Reason, _}} ->
            erlang:error({invalid_regex, Source,
                          unicode:characters_to_binary(Reason)})
    end.

splice([], _) -> [];
splice([Part | Parts], []) -> [Part | splice(Parts, [])];
splice([Part | Parts], [Value | Values]) ->
    [Part, quote(to_binary(Value)) | splice(Parts, Values)].

to_binary(V) when is_binary(V) -> V;
to_binary(V) -> unicode:characters_to_binary(io_lib:format("~ts", [V])).

%% validate(Source) -> {'Just', {Offset, Message}} | 'None' — backs
%% validateRegex. The offset is a BYTE offset (unlike RegexError.position),
%% because TaggedValidation.ByteSpan is defined in bytes.
validate(Source) when is_binary(Source) ->
    case re:compile(Source, ?COMPILE_OPTS) of
        {ok, _} -> 'None';
        {error, {Reason, Position}} ->
            {'Just', {Position, unicode:characters_to_binary(Reason)}}
    end;
validate(_) -> 'None'.

%% Escapes each metacharacter individually rather than wrapping in \Q...\E:
%% a value containing \E would close the quoted span early, turning the rest
%% into live pattern syntax. Mirrors Regex::quote in the interpreter.
quote(S) when is_binary(S) ->
    << <<(escape_char(C))/binary>> || <<C>> <= S >>;
quote(_) -> <<>>.

escape_char(C) when C >= $a, C =< $z -> <<C>>;
escape_char(C) when C >= $A, C =< $Z -> <<C>>;
escape_char(C) when C >= $0, C =< $9 -> <<C>>;
escape_char($_) -> <<"_">>;
%% Bytes >= 128 are UTF-8 continuation/lead bytes, never metacharacters —
%% escaping them would corrupt the encoding.
escape_char(C) when C >= 128 -> <<C>>;
escape_char(C) -> <<$\\, C>>.

%% ---------------------------------------------------------------------------
%% Matching
%% ---------------------------------------------------------------------------

%% matches(Subject, Regex) -> {'Just', Match} | 'None'. Unanchored.
matches(Subject, {'Regex.Regex', Source}) when is_binary(Subject) ->
    case run(Subject, Source, 0) of
        nomatch -> 'None';
        {match, Captures} -> {'Just', build_match(Subject, Source, Captures)}
    end;
matches(_, _) -> 'None'.

'matches?'(Subject, {'Regex.Regex', Source}) when is_binary(Subject) ->
    case run(Subject, Source, 0) of
        nomatch -> false;
        {match, _} -> true
    end;
'matches?'(_, _) -> false.

%% index mode, not binary mode — see the header note. Returns the raw
%% [{Start, Len}] list including {-1, 0} for groups that did not participate.
run(Subject, Source, Offset) ->
    re:run(Subject, Source, ?COMPILE_OPTS ++ [{offset, Offset}, {capture, all, index}]).

%% Builds the Match record {'Regex.Match', Captures} where Captures is a Kex map
%% keyed by group number AND by name atom, exactly as the interpreter builds it.
%% A group that did not participate is left out entirely, which is what makes
%% get(:opt) return None while a group matching "" returns Just(<<>>).
build_match(Subject, Source, Captures) ->
    Numbered = numbered_entries(Subject, Captures, 0, []),
    Named = [{Name, Value}
             || {Name, Number} <- name_table(Source),
                {ok, Value} <- [nth_capture(Subject, Captures, Number)]],
    {'Regex.Match', maps:from_list(Numbered ++ Named)}.

numbered_entries(_Subject, [], _Index, Acc) -> lists:reverse(Acc);
numbered_entries(Subject, [{-1, _} | Rest], Index, Acc) ->
    numbered_entries(Subject, Rest, Index + 1, Acc);
numbered_entries(Subject, [{Start, Len} | Rest], Index, Acc) ->
    Value = binary:part(Subject, Start, Len),
    numbered_entries(Subject, Rest, Index + 1, [{Index, Value} | Acc]).

nth_capture(Subject, Captures, Number) ->
    case Number < length(Captures) of
        false -> skip;
        true ->
            case lists:nth(Number + 1, Captures) of
                {-1, _} -> skip;
                {Start, Len} -> {ok, binary:part(Subject, Start, Len)}
            end
    end.

%% Named groups resolved to their group NUMBER, so a name and its index address
%% the same capture. Deliberately NOT re:run's `all_names`, which returns values
%% sorted by name with the names stripped — zipping those against the namelist
%% is easy to get subtly wrong (it silently swaps values between names).
name_table(Source) ->
    {ok, Compiled} = re:compile(Source, ?COMPILE_OPTS),
    case re:inspect(Compiled, namelist) of
        {namelist, []} -> [];
        {namelist, Names} ->
            %% Group numbers come from matching each name against the pattern's
            %% own (?<name>...) declarations, in declaration order.
            Numbers = name_numbers(Source, Names),
            [{binary_to_atom(N, utf8), Num} || {N, Num} <- Numbers]
    end.

%% Determines each named group's number by counting capturing groups up to its
%% declaration. re does not expose this directly, so the pattern is scanned:
%% every "(" that is not "(?" opens a numbered group, and "(?<name>" / "(?'name'"
%% / "(?P<name>" open a numbered group that also carries a name.
name_numbers(Source, Names) ->
    Found = scan_groups(Source, 0, 0, []),
    [{N, Num} || N <- Names, {FN, Num} <- Found, FN =:= N].

scan_groups(<<>>, _Index, _Depth, Acc) -> lists:reverse(Acc);
scan_groups(<<$\\, _C, Rest/binary>>, Index, Depth, Acc) ->
    scan_groups(Rest, Index, Depth, Acc);
scan_groups(<<"(?<", Rest/binary>>, Index, Depth, Acc) ->
    named_group(Rest, Index, Depth, Acc, $>);
scan_groups(<<"(?'", Rest/binary>>, Index, Depth, Acc) ->
    named_group(Rest, Index, Depth, Acc, $');
scan_groups(<<"(?P<", Rest/binary>>, Index, Depth, Acc) ->
    named_group(Rest, Index, Depth, Acc, $>);
%% (?: (?= (?! (?# etc. — non-capturing, so the group number does not advance.
scan_groups(<<"(?", Rest/binary>>, Index, Depth, Acc) ->
    scan_groups(Rest, Index, Depth, Acc);
scan_groups(<<$(, Rest/binary>>, Index, Depth, Acc) ->
    scan_groups(Rest, Index + 1, Depth, Acc);
scan_groups(<<_C, Rest/binary>>, Index, Depth, Acc) ->
    scan_groups(Rest, Index, Depth, Acc).

named_group(Bin, Index, Depth, Acc, Terminator) ->
    case binary:split(Bin, <<Terminator>>) of
        [Name, Rest] ->
            scan_groups(Rest, Index + 1, Depth, [{Name, Index + 1} | Acc]);
        _ ->
            scan_groups(Bin, Index + 1, Depth, Acc)
    end.

%% ---------------------------------------------------------------------------
%% Global operations
%% ---------------------------------------------------------------------------

%% Every match, left to right. Zero-width matches are reported (as in every
%% engine) with the cursor advancing one CHARACTER so iteration terminates and
%% never resumes mid-UTF-8-sequence.
scan(Subject, {'Regex.Regex', Source}) when is_binary(Subject) ->
    scan_loop(Subject, Source, 0, []);
scan(_, _) -> [].

scan_loop(Subject, Source, Offset, Acc) when Offset =< byte_size(Subject) ->
    case run(Subject, Source, Offset) of
        nomatch -> lists:reverse(Acc);
        {match, [{Start, Len} | _] = Captures} ->
            Match = build_match(Subject, Source, Captures),
            scan_loop(Subject, Source, next_offset(Subject, Start, Len),
                      [Match | Acc])
    end;
scan_loop(_Subject, _Source, _Offset, Acc) -> lists:reverse(Acc).

%% Report-then-advance: an empty match steps one character forward.
next_offset(Subject, Start, 0) -> next_char_boundary(Subject, Start);
next_offset(_Subject, Start, Len) -> Start + Len.

%% Replaces EVERY match (gsub, not sub). Replacement is either a literal binary,
%% inserted verbatim with no $1/\1 syntax, or a fun receiving the Match.
replace(Subject, {'Regex.Regex', Source}, Replacement) when is_binary(Subject) ->
    replace_loop(Subject, Source, Replacement, 0, 0, []);
replace(Subject, _, _) -> Subject.

replace_loop(Subject, Source, Replacement, Offset, Copied, Acc)
  when Offset =< byte_size(Subject) ->
    case run(Subject, Source, Offset) of
        nomatch ->
            finish_replace(Subject, Copied, Acc);
        {match, [{Start, Len} | _] = Captures} ->
            Before = binary:part(Subject, Copied, Start - Copied),
            Text = replacement_text(Subject, Source, Captures, Replacement),
            Next = next_offset(Subject, Start, Len),
            %% An empty match consumed nothing, so the character the cursor
            %% steps over still has to be copied through.
            {Skipped, NewCopied} =
                case Next > Start + Len andalso Start + Len < byte_size(Subject) of
                    true -> {binary:part(Subject, Start + Len,
                                         Next - (Start + Len)), Next};
                    false -> {<<>>, Start + Len}
                end,
            replace_loop(Subject, Source, Replacement, Next, NewCopied,
                         [Skipped, Text, Before | Acc])
    end;
replace_loop(Subject, _Source, _Replacement, _Offset, Copied, Acc) ->
    finish_replace(Subject, Copied, Acc).

finish_replace(Subject, Copied, Acc) ->
    Tail = case Copied < byte_size(Subject) of
               true -> binary:part(Subject, Copied, byte_size(Subject) - Copied);
               false -> <<>>
           end,
    iolist_to_binary(lists:reverse([Tail | Acc])).

replacement_text(_Subject, _Source, _Captures, Replacement)
  when is_binary(Replacement) -> Replacement;
replacement_text(Subject, Source, Captures, Fun) when is_function(Fun, 1) ->
    case Fun(build_match(Subject, Source, Captures)) of
        R when is_binary(R) -> R;
        R -> unicode:characters_to_binary(io_lib:format("~ts", [R]))
    end.

%% Ruby's split semantics: trailing empty fields dropped (leading kept),
%% positive limit caps the field count, negative limit keeps trailing empties,
%% capture groups interleaved into the result.
%% Arity-2 form for the default `limit = 0` — a Kex default argument can lower
%% to a call that omits the trailing parameter.
split(Subject, Regex) -> split(Subject, Regex, 0).

%% A plain String separator splits literally. Needed because `using Regex`
%% makes BEAM route every `.split` call through this module, including
%% `"a,b".split(",")` — see src/stdlib/regex.kex.
split(Subject, Sep, _Limit) when is_binary(Subject), is_binary(Sep) ->
    kex_intrinsic_string:split(Subject, Sep);

split(Subject, {'Regex.Regex', Source}, Limit) when is_binary(Subject) ->
    case Subject of
        <<>> -> [];                        %% Ruby: "".split(",") is []
        _ ->
            Fields = split_loop(Subject, Source, Limit, 0, 0, []),
            case Limit of
                0 -> drop_trailing_empty(Fields);
                _ -> Fields
            end
    end;
split(Subject, _, _) -> [Subject].

split_loop(Subject, Source, Limit, Offset, FieldStart, Acc)
  when Offset =< byte_size(Subject) ->
    case Limit > 0 andalso length(Acc) >= Limit - 1 of
        true -> finish_split(Subject, FieldStart, Acc);
        false ->
            case run(Subject, Source, Offset) of
                nomatch -> finish_split(Subject, FieldStart, Acc);
                {match, [{Start, Len} | Groups]} ->
                    Next = next_offset(Subject, Start, Len),
                    %% A zero-width match at the field start would emit an empty
                    %% field forever; skipping it is what makes splitting on an
                    %% empty pattern yield characters.
                    case Len =:= 0 andalso Start =:= FieldStart of
                        true ->
                            split_loop(Subject, Source, Limit, Next, FieldStart, Acc);
                        false ->
                            Field = binary:part(Subject, FieldStart,
                                                Start - FieldStart),
                            Captured = [binary:part(Subject, GS, GL)
                                        || {GS, GL} <- Groups, GS =/= -1],
                            split_loop(Subject, Source, Limit, Next, Start + Len,
                                       lists:reverse(Captured) ++ [Field | Acc])
                    end
            end
    end;
split_loop(Subject, _Source, _Limit, _Offset, FieldStart, Acc) ->
    finish_split(Subject, FieldStart, Acc).

finish_split(Subject, FieldStart, Acc) ->
    Tail = binary:part(Subject, FieldStart, byte_size(Subject) - FieldStart),
    lists:reverse([Tail | Acc]).

drop_trailing_empty(Fields) ->
    lists:reverse(lists:dropwhile(fun(F) -> F =:= <<>> end,
                                  lists:reverse(Fields))).

%% ---------------------------------------------------------------------------
%% UTF-8 helpers
%% ---------------------------------------------------------------------------

%% Byte offset -> character offset. Both engines report byte offsets into
%% UTF-8; Kex strings are logical text, so offsets are converted before they
%% cross into Kex or the two backends disagree on non-ASCII patterns.
char_offset(Bin, ByteOffset) when ByteOffset =< byte_size(Bin) ->
    Prefix = binary:part(Bin, 0, ByteOffset),
    case unicode:characters_to_list(Prefix) of
        L when is_list(L) -> length(L);
        _ -> ByteOffset
    end;
char_offset(_Bin, ByteOffset) -> ByteOffset.

%% Advances past one UTF-8 character, so an empty match never resumes in the
%% middle of a multi-byte sequence (which `re` would reject as bad UTF-8).
next_char_boundary(Bin, Offset) when Offset >= byte_size(Bin) -> Offset + 1;
next_char_boundary(Bin, Offset) ->
    skip_continuation(Bin, Offset + 1).

skip_continuation(Bin, Pos) when Pos >= byte_size(Bin) -> Pos;
skip_continuation(Bin, Pos) ->
    case binary:at(Bin, Pos) of
        B when B band 16#C0 =:= 16#80 -> skip_continuation(Bin, Pos + 1);
        _ -> Pos
    end.
