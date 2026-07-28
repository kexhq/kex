# Regular Expressions

Regexes are backed by **PCRE2** in the interpreter and Erlang's **`re`** on the
BEAM backend — the same PCRE pattern language on both. The character-offset
positions in errors agree across backends even for non-ASCII patterns.

Regex is **opt-in, not prelude**: nothing here is in scope until you write
`using Regex`.

```kex
using Regex

main do
  IO.printLine("hello42".matches?(regex`\d+`))   # true
end
```

## Building a pattern

There are two spellings, and they differ in return type on purpose.

### Tag literal — `` regex`…` `` (compile-time checked)

The tagged-literal form returns a **bare `Regex`**: the pattern is known at
compile time and is validated during compilation, so it cannot fail at runtime.

```kex
let r = regex`(\w+)=(\d+)`   # : Regex
```

Because it is a raw backtick string, backslashes are literal — write `\d`, not
`\\d`. An invalid pattern is a **compile error**, with the caret pointing at the
offset PCRE2 reported inside the literal:

```kex
let bad = regex`(`   # error: caret points inside the literal
```

`re` is a short alias for `regex` with identical behavior:

```kex
let url = re`https?://\S+`
```

The tag may also be written **module-qualified** — `` Regex.regex`…` `` (and
`` Regex.re`…` ``) behave exactly like the bare form: same tag, same
compile-time validation, same bare-`Regex` result, on both backends. As with any
`Regex.*` name, the module must be in scope (`using Regex`).

### Call form — `regex("…")` (returns `Result`)

Use the call form when the pattern is a runtime string that might be invalid. It
returns `Result<Regex, RegexError>`:

```kex
match regex("\\d+") do
  Ok(re)   -> IO.printLine("compiled")
  Error(e) -> IO.printLine("bad pattern at ${e.position}: ${e.message}")
end
```

In the call form the argument is an ordinary string, so metacharacters need
string-level escaping (`"\\d+"`). `RegexError` carries `source`, `position` (a
character offset), and `message`.

### Interpolation — `` regex$`…` `` (auto-escaped)

A `$` before the backtick enables `${…}` holes. **Interpolated values are
escaped per character before splicing**, so a value always contributes literal
text and can never inject pattern syntax. There is no opt-out — building pattern
*structure* from a value is what the call form is for.

```kex
let word = "a.b"
"a.b=1".matches?(regex$`^${word}`)   # true  — the "." is matched literally
"axb=1".matches?(regex$`^${word}`)   # false — the "." did not become "any char"
```

> Interpolating literals are **not** compile-time validated (only the raw
> `` regex`…` `` form is). See `docs/known-gaps.md` — an interpolating literal is
> typed as a bare `Regex` even though a malformed author-written skeleton would
> raise at runtime.

### Escaping a literal string — `Regex.quote`

`Regex.quote(s)` escapes every metacharacter in `s` so the result matches `s`
literally:

```kex
Regex.quote("a.b")   # => "a\\.b"
```

## Matching

`matches` and `scan` are **unanchored** — anchor with `^…$` in the pattern for a
whole-string match.

| Call | Returns | Notes |
|---|---|---|
| `s.matches(re)` | `Match?` | first occurrence, or `None` |
| `s.matches?(re)` | `Bool` | boolean-only test |
| `s.scan(re)` | `[Match]` | every match, left to right — always `[Match]` even with no groups |
| `s.replace(re, repl)` | `String` | replaces **every** match (gsub); `repl` is a `String` or a block |
| `s.split(re)` | `[String]` | resolves to `String.split` when handed a `Regex` |
| `Regex.splitLimit(s, re, n)` | `[String]` | split with a field limit (qualified only — see below) |

`matches` returns an **`Option<Match>`**, so unwrap before reading captures:

```kex
using Regex

match "order #4271 shipped".matches(regex`#(\d+)`) do
  Just(m) -> IO.printLine("id: ${m.get(1, "")}")   # id: 4271
  None    -> IO.printLine("no match")
end
```

## Reading a `Match`

`Match` behaves like a **map keyed by both group number and group name**. Group
`0` is the whole match; `1`, `2`, … are positional groups; `:name` is a named
group. The accessor is `get`:

```kex
m.get(0)            # whole match      -> String?
m.get(1)            # first group      -> String?
m.get(:year)        # named group      -> String?
m.get(1, "")        # with a default   -> String
m[2]                # map-like index   -> String?
```

- `get(key)` returns `String?` — `None` for a group that did not participate,
  `Just("")` for a group that matched the empty string.
- `get(key, default)` returns `String` — `default` when the group is absent.

A group that did not participate is an **absent key** (`None`), which is distinct
from a group that matched empty (`Just("")`).

### Named captures

```kex
using Regex

let dateRe = regex`(?<year>\d{4})-(?<month>\d{2})-(?<day>\d{2})`

match "2026-07-27".matches(dateRe) do
  Just(m) -> { year: m.get(:year, ""), month: m.get(:month, ""), day: m.get(:day, "") }
  None    -> { year: "", month: "", day: "" }
end
```

### Scanning into structured data

```kex
using Regex

let pairs = "h=34, a=44"
  .scan(regex`(\w+)=(\d+)`)
  .map do |m|
    (m.get(1, ""), Integer.parse(m.get(2, "")).or(0))
  end
# [("h", 34), ("a", 44)]
```

## Replacing

`replace` is **gsub** — every match. The replacement is a literal `String`
(inserted verbatim, no `$1`/`\1` backreferences) or a block receiving the
`Match`:

```kex
"a-b-c".replace(regex`-`, "+")                       # "a+b+c"
"x1y2".replace(regex`(\d)`) do |m| "[${m.get(1, "")}]" end   # "x[1]y[2]"
```

## Splitting

Plain `s.split(re)` needs no qualification — `String.split` dispatches to the
regex engine when handed a `Regex`:

```kex
"a, b ,c".split(regex`\s*,\s*`)   # ["a", "b", "c"]
```

Only the limit form is qualified as `Regex.splitLimit` (the module deliberately
does **not** export `split`, so that `"a,b".split(",")` and `"hi".split` keep
working). A positive `limit` caps the number of fields; a negative one keeps
trailing empty fields:

```kex
Regex.splitLimit("a,b,c", regex`,`, 2)    # ["a", "b,c"]
Regex.splitLimit("a,b,,", regex`,`, -1)   # ["a", "b", "", ""]
```

## Notes and gaps

- Both spellings exist under two names: `regex`/`regex$` and the `re`/`re$`
  aliases behave identically.
- `Regex` carries only its `source` string; the compiled engine lives in a
  runtime cache keyed by that source. A compiled pattern bakes in the host's
  PCRE version, so it is never embedded in a distributed artifact.
- **wasm build:** the in-browser build has no PCRE2, so every `Regex` call
  raises "Regex is unavailable in this build" (`docs/known-gaps.md`).
- Both the bare (`` regex`…` ``) and module-qualified (`` Regex.regex`…` ``)
  tag spellings are supported and behave identically.

See `examples/regexes.kex` for a runnable tour.
