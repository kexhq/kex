# Kex Regex Plan

## Context

Kex has no regular expressions yet. This plan covers the **`Regex` module** —
the `regex` function, the `Match` type, matching/replace/split operations, and
the PCRE2/BEAM backends.

Regex has two spellings: a **call form** `` regex(`…`) `` and a **tag form**
`` regex`…` ``. Both are just the `regex` function; the tag is sugar. The
general machinery they sit on — backtick raw/multiline strings and tagged
literals — is **not regex-specific** and is designed separately in
**`docs/strings-and-tags-plan.md`**. This plan depends on that one but does not
define it: the call form needs only backtick raw strings; the tag form needs the
full tagged-literal feature. See "Dependencies & sequencing" below.

## The decision trail (why not `/…/`)

Three delimiter options were considered:

1. **`/pat/flags`** (Ruby/JS/Perl) — reads best, but collides with division in
   a UFCS-heavy language. Requires context-sensitive lexing (`canPrecedeRegex()`
   tracking the previous significant token). Solvable but a lexer hack, and it
   makes `/` forever ambiguous.
2. **Backticks `` `pat` `` as a dedicated regex delimiter** — kills the `/`
   ambiguity, but backticks are already earmarked for **multiline / TypeScript-
   style tagged strings** (`` sql`…` ``), so they're spoken for.
3. **Tagged backtick literal `` regex`pat` ``** — *chosen.* Reuses the tagged-
   string machinery we're building anyway. No new lexer mode, no `/` ambiguity,
   slash-free escaping, multiline bodies, and interpolation all for free.

## Design Decisions

- **Regex is an ordinary `regex` function over a raw backtick string**, with a
  tagged-literal shorthand — **both spellings, one function**:
  `` regex(`\d+`) `` (call form, the primitive) and `` regex`\d+` `` (tag form,
  sugar desugaring to the call). No dedicated `/…/` delimiter. The backtick body
  is the raw/multiline string lexing already planned for tagged strings, so the
  call form needs zero new grammar. `regex(str)` also accepts a computed string,
  so no separate `Regex.new` constructor is needed.
- **Lowercase tag, uppercase type.** `regex` (and alias `re`) is the tag/builder
  function; `Regex` is the type it produces — matching Kex's `Option`/`Match`
  casing (lowercase functions, PascalCase types).
- **`re` is an optional short alias for `regex`**, bound at the definition site
  (a second export pointing at the same function), *not* a lexer synonym. Every
  property of `regex` (folding, BEAM mapping, errors) applies to `re` for free.
- **Not global — `using`-gated.** The `regex`/`re` tags live in the `Regex`
  module and only enter scope after `using Regex`. No prelude pollution;
  consistent with the one-module-per-BEAM-module design (see
  `docs/module-sys-plan.md`).
- **The tag form is not sugar — it is the compile-time-validated form** (general
  machinery in `docs/strings-and-tags-plan.md`). It shares the call form's
  implementation but returns a bare `Regex` where the call returns
  `Result<Regex, RegexError>`, because a literal has already been proven to
  compile. That difference is why both forms exist; see "The tag returns
  `Regex`; the call returns `Result`". Compile-time pattern validation comes from
  the optional **`validateRegex`** companion — a bad raw tagged pattern is a
  compile error, but through a convention any library can use. The compiler has
  no regex-specific knowledge and no blessed-tag list. The `re` alias delegates
  through `validateRe`.
- **Purity is required for compile-time validation.** `validateRegex` must be
  non-`foul`, since the compiler evaluates it on the tree-walk interpreter at
  compile time. This is already true of every regex operation (below), so it
  costs nothing here — but it means marking one `foul` would break *builds*, not
  just calls.
- **BEAM backend maps to Erlang's `re`** (PCRE-ish); the interpreter is designed
  to match `re` semantics to avoid behavior drift between backends.
- **All regex operations are pure** — compiling, `capture`, `matches?`, `scan`,
  `replace`, `split` are deterministic and effect-free (no `foul`). Only code
  that *does IO with* the results (printing, reading files) is foul. Marking a
  pure matcher `foul` is a bug — it would wrongly poison purity inference for
  every caller.

## Syntax

Two spellings of one name, **differing in what they can promise** — the tag is
compile-time validated and so cannot fail; the call takes any string and so can:

```kex
regex`\s+\d`      -> Regex                      # validated at compile time
regex(`\s+\d`)    -> Result<Regex, RegexError>  # same text, but checked at runtime
```

They are *not* interchangeable, by design — see "The tag returns `Regex`; the
call returns `Result`" below, which is the core justification for having both.

- **Call form** `` regex(`…`) `` — the primitive. An ordinary function call whose
  argument is a raw backtick string. Works with zero grammar beyond backtick
  strings; always available. Also accepts a computed string: `regex(userInput)`.
  Returns `Result` because its argument is unknown until runtime.
- **Tag form** `` regex`…` `` — the form whose pattern is known at compile time.
  Terser, and returns a bare `Regex` because `validateRegex` has already proven
  it compiles. If never implemented, the call form still gives working regex —
  just with a `Result` to unwrap at every literal site.

```kex
using Regex

regex(`\d+`)                       # call form — primitive
regex`\d+`                         # tag form — sugar, same function
regex`(?<year>\d{4})-(?<month>\d{2})`
re`https?://\S+`                   # short alias; slash-free escaping (no \/\/ noise)
regex(userInput)                   # computed pattern — no separate constructor needed

str.capture(regex`\d+`)            # -> Match?   (NOT `match` — that's a keyword)
str.matches?(regex`\d+`)           # -> Bool
```

Because both forms are just the `regex` name, both are `using`-gated identically —
without the import it's an ordinary unresolved name:

```kex
regex`\d+`        # error: `regex` not in scope — did you `using Regex`?
```

### Flags

Flags fold **inline** using standard PCRE syntax rather than a trailing suffix
after the closing backtick:

```kex
regex`(?i)hello`        # case-insensitive
regex`(?im)^foo`        # case-insensitive + multiline
```

Rationale: a tag is `identifier + backtick-body`, so a trailing `` `pat`i ``
suffix would need a special parser production. Inline `(?i)` is native to
Erlang `re`/PCRE and keeps the grammar dead simple. (A trailing-suffix form
could be added later as sugar if desired, parsed as part of the tagged-literal
production.)

Flag → Erlang `re` option mapping to settle when implementing:

| Inline | Meaning | Erlang `re` option |
|---|---|---|
| `(?i)` | case-insensitive | `caseless` |
| `(?m)` | `^`/`$` match line boundaries | `multiline` |
| `(?s)` | `.` matches newline | `dotall` |
| `(?x)` | extended / whitespace + comments | `extended` |

## Tag machinery — see the strings & tags plan

The general tagged-literal mechanism (`` tag`…` `` desugars to a
parts-plus-values function call, uniform body lexing, adjacency rule, user tags)
lives in **`docs/strings-and-tags-plan.md`** and is not repeated here. What
matters for regex:

- `` regex`…` `` resolves to the same `regex` name as `` regex(`…`) `` and
  shares its implementation, but binds the **two-argument tag ABI overload**,
  which returns `Regex` rather than `Result<Regex, RegexError>`. Same matching
  semantics, different guarantee.
- Because `regex` is an ordinary name, tag scoping is plain name resolution:
  available after `using Regex`, an unresolved-name error otherwise.

### Compile-time pattern validation (via `validateRegex`)

A malformed `` regex`[` `` is a **compile error**, not a runtime failure. But
this is **not hardcoded in the compiler** — `validateRegex` is discovered by
the same companion naming convention as a third-party `validateSql`. The
compiler knows nothing about regex.

```kex
let validateRegex(source: String) -> [Issue] do
  # PCRE2 compile check; report the engine's UTF-8 byte span.
  ...
end

let validateRe(source) = validateRegex(source)
```

Rules follow from the general contract (see `docs/strings-and-tags-plan.md`,
"Companion validators"):

- **Raw tagged literal** (`` regex`…` ``) → the pattern is fully known, so
  `validateRegex` runs at compile time. `Fatal` issues fail the build with a
  caret pointing inside the literal.
- **Interpolating tag or ordinary call**
  (`` regex$`${x}` `` / `regex(computedString)`) → validation happens at
  runtime (cached on first use). Ordinary calls are not compile-time validated
  in v1.

Because raw and interpolating literals are distinguished *syntactically* by the
`$` marker rather than by whether the body happens to contain `${}`, which
regime a given pattern gets is visible at the call site.

Note that `validate` returns issues only — it does **not** hand back a compiled
pattern. Precompiling a literal into the emitted program is a separate,
later concern; embedding a compiled `{re_pattern,…}` term would bake the host's
PCRE version into the artifact, which is a hazard for `.kexo` distribution and
needs its own decision.

## API shape

**A match is a `Match` type with a map-like interface.** `capture` returns an
`Option<Match>`. `Match` is accessed like a map keyed by *both* group number
(`Int`) and group name (`Atom`), but it is a **named type** so it can grow new
capabilities (spans, surrounding context) without breaking any call site.

```kex
str.capture(re)              # -> Match?     (None if no match)
str.matches?(re)             # -> Bool       (predicate; no captures)
str.scan(re)                 # -> [Match]    (one Match per match, always)
str.replace(re, "literal")   # -> String     (ALL matches — gsub, not sub)
str.replace(re) { |m| ... }  # -> String     (replacer block, see below)
str.split(re)                # -> [String]   (Ruby semantics, see below)
str.split(re, limit)         # -> [String]   (negative limit keeps trailing empties)

# All five search anywhere in the subject — anchor with `^…$` in the pattern.
```

### Every operation is unanchored — decided

All five operations ask **"does the pattern occur anywhere in the subject"**,
never "does it consume the whole subject". Anchoring is expressed in the
*pattern*, with `^…$` or `\A…\z`, exactly as in Ruby, Python, Perl, and PCRE
itself.

```kex
"hello world".matches?(regex`world`)      # -> true    (substring is enough)
"hello world".matches?(regex`^world$`)    # -> false   (anchor it yourself)
```

Worth stating explicitly because `matches?` is the one name that misleads: "does
this string match the pattern" reads as *whole-string* to many people, and the
two readings disagree on every unanchored pattern — silently, and in the
permissive direction, which is how regex validation bugs get shipped. `matches?`
is the boolean shadow of `capture`, and `capture` is plainly a search, so
anchoring `matches?` alone would make the pair incoherent.

If a whole-string variant is wanted later it gets its own name — `fullMatch?`,
implemented by wrapping the pattern in `\A(?:…)\z` — and never becomes a flag or
option on `matches?`, since a boolean that changes meaning under a flag is worse
than two clearly named functions.

### `scan` returns `[Match]`, unlike Ruby — decided

`split` follows Ruby closely (above), so it is worth being explicit that `scan`
does **not**. Ruby's `scan` changes its return shape based on the pattern's
internals:

```ruby
"a1b2".scan(/\d/)       # => ["1", "2"]             no groups -> flat strings
"a1b2".scan(/(\d)/)     # => [["1"], ["2"]]         ONE group -> nested anyway
"a1b2".scan(/(\w)(\d)/) # => [["a","1"], ["b","2"]] two groups
```

Note the middle line: wrapping an existing pattern in a single group — a
no-op to the *matching* — turns `["1", "2"]` into `[["1"], ["2"]]` and breaks
callers.

Kex returns `[Match]` in both cases. Adding a capture group to a pattern is a
refactor of the *pattern*; it must not silently change the type flowing out of
`scan` and break every downstream `.map`. Uniform `[Match]` also means whole-match
and named groups stay reachable (`m.get(0)`, `m.get(:name)`) in the grouped case,
which Ruby's array-of-strings form discards outright.

**So: Kex is not "Ruby-compatible regex".** It borrows Ruby's `split` semantics
because they are good and widely expected; `scan`, `Match`, and replacement
(block-based, no `$1`) are deliberately different. The pattern *language* is
PCRE — that is the compatibility claim worth making, and it is with PCRE, not
with Ruby.

### The Match type

`Match` behaves like a map, populated on every access path:

- key `0` → the whole match (`Int` key)
- keys `1`, `2`, … → positional groups (`Int` keys)
- `:year`, `:month`, … → named groups (`Atom` keys)

A named group is *also* a numbered group in PCRE, so `(?<year>…)` is reachable
under both its number and its name-atom — same value, two keys. Access via
`get`:

```kex
m.get(:year)        # -> String?   (None if the group didn't participate)
m.get(1, "?")       # -> String    (with default)
m.get(0)            # -> String?   (whole match)
```

**Group `0` is always present on a successful match — decided.** If `capture`
returns `Just(m)`, then `m.get(0)` is `Just(s)`, where `s` may be the empty
string. A zero-width match (`` regex`x*` `` against `"abc"`, a lookahead-only
pattern) still *matched*, so it yields a `Match` whose whole-match group is
`""` — not `None`. The absent-key rule applies only to **groups that did not
participate**; the whole match always participates. This keeps `capture`'s
contract simple: `Just(m)` means matched, and `m.get(0)` never surprises.
Numbered/named groups keep the `None` behavior — `` regex`(a)|(b)` `` against
`"a"` leaves key `2` absent.

### Zero-width matches follow the engines — decided

**The rule.** Zero-width matches are reported everywhere, exactly as PCRE and
every mainstream engine do: report the empty match, then advance the cursor one
character so iteration can't loop. Kex adds **no filtering**. Suppressing them
was considered and rejected — it would be a divergence each backend had to
re-implement identically, and it breaks patterns where zero-width is the point.

```kex
"a1b2".scan(regex`\d*`)          # -> 5 matches: "", "1", "", "2", ""
"abc".replace(regex`x*`, "-")    # -> "-a-b-c-"
"a1".matches?(regex`^(?=.*\d)`)  # -> true             (lookahead-only pattern)
"abc".capture(regex`x*`)         # -> Just(m), m.get(0) == ""
"abc".split(regex`x*`)           # -> ["a", "b", "c"]  (split-into-chars idiom)
```

Verified against Ruby 4.0 (`"a1b2".scan(/\d*/)` -> `["", "1", "", "2", ""]`,
`"abc".gsub(/x*/, "-")` -> `"-a-b-c-"`, `"abc".split(//)` -> `["a","b","c"]`);
Python `re.findall`, JS `/g`, and Erlang `re:run(…, [global])` agree. Matching
them means the behavior falls out of PCRE2 and `re` for free on both backends
rather than needing a hand-written filter in each.

The load-bearing cases this preserves: `` regex`^(?=.*\d)` `` and `` regex`\b` ``
match zero characters but did genuinely match, and `` split(regex`x*`) `` is how
you split into characters.

### Compilation failure is a `Result` — decided

`regex` returns **`Result<Regex, RegexError>`**, matching the prelude's existing
convention for fallible parsing — `Integer.parse : String -> Result<Integer,
ParseError>` (`src/prelude/number.kex:137`), `Parser.parse : String ->
Result<Program, ParseError>`, and all of `Http`. Not a raise, not `Regex?`: a
bad pattern has a *reason* and a *position*,
and discarding them would make `regex(userInput)` undebuggable.

```kex
regex : String -> Result<Regex, RegexError>

record RegexError do
  source   : String     # the pattern text that failed
  position : Integer    # character offset into the pattern (see offsets below)
  message  : String     # the engine's explanation
end
```

`RegexError` implements **`Errorable`** (`src/prelude/errorable.kex`) so generic
handlers can display it, and mirrors `ParseError`'s shape — same fields, same
role. Both engines supply what it needs: PCRE2's `pcre2_compile` yields an error
code plus an error offset (`pcre2_get_error_message` for the text), and Erlang's
`re:compile/2` returns `{error, {Reason, Position}}`. **The offset must be
converted to a character offset** on both sides, per the offsets decision above —
engine-native byte offsets would disagree on a pattern containing non-ASCII.

Usage follows `Result`'s existing surface (`ok?`, `or`, `map`, `flatMap`,
`toOptional`, and `.try` inside a `trying` block):

```kex
match regex(userInput) do
  Ok(re)   -> text.scan(re)
  Error(e) -> IO.printLine("bad pattern at ${e.position}: ${e.message}")
end
```

### The tag returns `Regex`; the call returns `Result` — and that is the point

**The two forms deliberately differ in type.** A raw tagged literal *cannot
fail* — `validateRegex` rejected it at compile time — so wrapping it in a
`Result` would guard an impossibility, and there is no sensible `.or(default)`
for a regex anyway. A computed pattern *can* fail, and must say so in its type.

```kex
regex`\d+`        # -> Regex                        cannot fail; already validated
regex(userInput)  # -> Result<Regex, RegexError>    can fail; must be handled
```

**This is the reason both forms exist.** The tag is not cosmetic sugar over the
call — it is the form that carries a *compile-time proof of validity*, and the
return type is where that proof shows up. Anything else makes one of the two
forms redundant: if both returned `Result`, the tag would buy only brevity; if
both returned `Regex`, the call form would have to raise on bad input.

**It needs no new mechanism.** Tagged literals already lower to a *two-argument*
ABI — `tag(parts, values)` (`ast::TaggedLiteral`, `src/ast/ast.hxx:184`) — while
the call form is one argument. So these are ordinary overloads of one name,
distinguished by arity, each returning what its inputs justify:

```kex
regex : String -> Result<Regex, RegexError>          # call form
regex : [String] -> [String] -> Regex                # tag ABI (raw literal)
```

**Under the hood they are the same code.** The tag overload joins its parts
(quote-escaping any values), hands the result to the `String` overload, and
unwraps the `Ok` — one compile path, one cache, one set of engine options. The
divergence is entirely in the *type*, not the machinery: nothing about matching,
flags, or backend behavior differs between a pattern that arrived as a literal
and one that arrived as a string.

```kex
let regex(parts, values) = regex(joinQuoted(parts, values)).unwrapValidated
```

The unwrap is sound *only* because compile-time validation ran — which makes `validateRegex` load-bearing rather than optional
for this module, and worth stating in the purity note above: marking it `foul`
would not merely skip a check, it would invalidate the tag form's return type.

**Interpolating tags also return `Regex` — decided.** The rule is uniform and
has no exceptions: **every tag form returns `Regex`; only the call form returns
`Result`.** `` regex$`^${x}` `` is typed `Regex` like its raw sibling.

This holds because escaped values are *inert*. With per-character escaping (see
"Interpolation safety" — not `\Q…\E` wrapping, which a value containing `\E`
escapes out of), an interpolated value cannot contribute pattern syntax, so
pattern validity is decided entirely by the author-written skeleton — which is
known at compile time. Validate the skeleton with placeholders substituted for
the holes and the tag is proven to compile, exactly as the raw form is.

Verified: `\Q\E*` compiles, so an *empty* interpolation adjacent to a quantifier
is not a failure mode — the intuitive worry doesn't hold.

**What this costs, and why it's acceptable:**

- **Dynamic pattern *structure* has no opt-out inside a tag.** Building pattern
  syntax from a value is exactly what the call form is for:
  `regex("(${a}|${b})")` -> `Result`. So the tag/call split lines up with the
  real distinction — inert values in a fixed skeleton stay a tag, dynamic
  structure becomes a computed string and takes a `Result`. This is what keeps
  the tag's `Regex` honest, and it *removes* the escape-hatch the earlier draft
  had rather than special-casing it.
- **Pattern size limits still fail at runtime.** A pathologically large
  interpolated value can exceed PCRE2's compile limits. This **raises** rather
  than returning a value — a resource-exhaustion failure like OOM, not a
  data-validation failure the caller was expected to branch on. Rare enough,
  and un-actionable enough, that a `Result` on every interpolating literal would
  be the wrong trade.

### `split` follows Ruby — decided

Ruby's `String#split` semantics, verified against Ruby 4.0:

```kex
"a,b,,".split(regex`,`)        # -> ["a", "b"]        trailing empties dropped
"a,b,,".split(regex`,`, -1)    # -> ["a", "b", "", ""]   negative limit keeps them
"a,b,c".split(regex`,`, 2)     # -> ["a", "b,c"]      limit caps the field count
",a".split(regex`,`)           # -> ["", "a"]         *leading* empty is kept
"  a b".split(regex`\s+`)      # -> ["", "a", "b"]    (see note)
"a1b2c".split(regex`(\d)`)     # -> ["a","1","b","2","c"]  captures interleaved
"abc".split(regex``)           # -> ["a", "b", "c"]
"".split(regex`,`)             # -> []                empty input -> empty list
```

The three non-obvious rules, all inherited deliberately:

1. **Trailing empty fields are dropped; leading ones are not.** `` "a,b,,"  ``
   gives two fields, but `",a"` gives three. Asymmetric and slightly odd, but it
   is what everyone expects from `split` in practice, and the `limit`-based
   opt-out below recovers the untrimmed form.
2. **A negative `limit` disables the trailing trim**; a positive `limit` caps
   the number of fields, leaving the remainder unsplit in the last one. Kex
   spells the limit as a second positional argument, matching Ruby.
3. **Capture groups in the pattern are interleaved into the result.**
   `` split(regex`(\d)`) `` returns the delimiters alongside the fields. This is
   how you split while keeping separators; a non-capturing `(?:…)` opts out.

Not inherited: Ruby's **awk mode**, where the *string* argument `" "` (not a
regex) means "split on runs of whitespace and strip leading whitespace" — hence
`` "  a b".split(" ") `` -> `["a","b"]` while `` split(/\s+/) `` -> `["", "a", "b"]`.
That special case belongs to `String.split`, not the regex overload, and Kex
should not replicate the magic-single-space rule at all; `` split(regex`\s+`) ``
with its leading `""` is the honest behavior.

**BEAM mapping.** Erlang `re:split/3` is close but not identical: it keeps
trailing empties by default (Ruby's default is `re:split`'s `trim` option) and
takes `{parts, N}` where Ruby takes a positive limit. Both are option-level
adjustments, not reimplementations — but they must be applied, or `split` will
disagree across backends on exactly the trailing-empty case.

### `replace` is global — it's `gsub`, not `sub`

`replace` substitutes **every** match, i.e. Ruby's `gsub` semantics under a
plainer name. Kex has no `sub`/`gsub` pair; there is one obvious operation and
it is spelled `replace`.

```kex
"a-b-c".replace(regex`-`, "+")   # -> "a+b+c"    (all three, not just the first)
```

Rationale: `gsub` is a Perl-inherited name that has to be explained; replacing
only the first match is the rarer intent and reads better as an explicit
opt-in. If a first-only variant is wanted later it lands as `replaceFirst`
(or a `count:` named argument) — additive either way, so it needn't be decided
now. `String.replace` for plain-string patterns must use the same all-matches
rule, or the two overloads would disagree.

**Why a type and not a bare map.** The map-like access is the right *interface*,
but a raw `{ Atom | Int : String }` map is **closed at its value type** — adding
offsets later would mean changing the value type (`String` → `(String, Int,
Int)`) and **breaking every `m.get(…)` call site**, or bolting on a parallel
`captureSpans` map that callers must keep correlated by hand. A named `Match`
**extends cleanly**: new fields like `m.spans` / `m.pre` / `m.post` (byte
offsets, surrounding text — things Erlang `re` can return but a string map
can't) are additive and leave existing code untouched. Every mature regex lib
(Ruby `MatchData`, Python `re.Match`, .NET `Match`) landed on a type for exactly
this reason. The map interface still buys us the nice part for free:
**optional groups need no special rule** — a group that didn't participate is
simply an absent key, so `m.get(…)` returns `None`.

**Type-system verification (checked against the current checker).** The map-like
`Match` is expressible today, but *only* as a named type — not as a bare mixed
map:

- `Match` is a `make Match do … end` type whose accessor is declared with a
  **union-typed parameter**:
  ```kex
  get :> (Atom | Int) -> String?         # no default -> Option
  get :> (Atom | Int) -> String -> String
  ```
  Arg-matching special-cases union params (`argMatchesParam`, typechecker.cxx
  ~2207): an argument matches a union param if it matches *any* member. So both
  `m.get(0)` (`Int`) and `m.get(:year)` (`Atom`) typecheck. Verified.
- Anonymous union types (`Atom | Int`) parse in annotation position
  (`parseTypeUnion`, parser.cxx ~849). Verified.
- The `V?` / `V` two-arity `get` shape the examples rely on already exists —
  it is exactly `Map.get`'s signature (`get :> K -> V?`, `get :> K -> V -> V`
  in `src/prelude/map.kex`). `Match.get` mirrors it. Verified.

**Why `Match` genuinely can't be a bare map** (stronger than the extensibility
argument): map *literals* are inferred **first-key-wins** — `MapExpr` inference
(typechecker.cxx ~1602) takes the first entry's key/value type and ignores the
rest; there is no union synthesis and no heterogeneous-key check. So a literal
mixing `0` and `:year` would infer `Map<Integer, String>` and silently skip the
atom keys. A heterogeneous capture map is therefore *not constructible* as a
plain literal at all — it has to be a named type with an explicit union-typed
accessor. The type system pushes us to exactly the `Match` design we chose.

### Replacement — no `$1`/`\1` anywhere

A replacement is either a plain literal string (no magic) or a **replacer
block** `do |m| … end` that receives the `Match` and returns the replacement
text. No backreference mini-language, no escaping rules:

```kex
"call 555-1234".replace(regex`(\d{3})-(\d{4})`) do |m|
  "${m.get(1, "")}-XXXX"       # captures via the Match, not $1
end
```

### Naming — `matches` / `matches?`, because `match` is a keyword

The pattern-matching construct `match … do … end` reserves the bare `match`
token, so a `.match(…)` method would fail to parse (after `.` the lexer yields
the `match` keyword, not an identifier). Note this is the lowercase keyword —
the `Match` *type* is a distinct capitalized token and fine as a name.

The two operations are therefore **`matches`** (-> `Match?`) and **`matches?`**
(-> `Bool`) — the third-person form sidesteps the keyword while staying the
name every other language uses, and the pair reads as one operation with the
`?` asking only for a yes/no, which is Kex's `?`-means-Bool convention
(`empty?`, `required?`). The bare/`?` pairing has prelude precedent: `ok`/`ok?`,
`error`/`error?`, `none`/`none?`.

Alternatives considered and rejected:

| Name | Why not |
|---|---|
| `capture` | Earlier choice. Names the *mechanism* (capture groups) rather than the operation, and reads oddly for a pattern with no groups at all. |
| `firstMatch` | Dart's spelling, and unambiguous — but verbose at every call site, and the ambiguity it removes is not one that bites in practice. |
| `find` | Rust's `Regex::find`. Free on `String`, and `List.find` already returns `X?` — but `List.find` takes a *predicate*, so the shapes only rhyme, and it invites a future `String.find(substring)` to collide. |
| `.match` via a lexer fix | Allowing keyword tokens as member names after `.` would give the universally expected name. Deferred: it is a language-level change affecting every keyword, far beyond regex, and it leaves `match` meaning two things in one file. Worth revisiting on its own merits, not under this plan. |

**The one hazard, recorded deliberately.** `matches` and `matches?` differ by a
single character *and* both are usable in a boolean context, because
`Optional` implements `Truthyable` with `Just(_)` truthy
(`src/prelude/truthyable.kex:30`). So:

```kex
if s.matches(re)     # works — Just is truthy
if s.matches?(re)    # works — Bool
```

Writing the wrong one in an `if` is not a type error and behaves identically.
That is acceptable — the two agree on exactly the question an `if` asks — but
it means the `?` cannot be relied on as a safety net, only as a readability and
allocation hint. Anywhere the *result* is used, the types diverge immediately
and the checker catches it.

## Backend mapping

**Engine: PCRE2 for the interpreter — decided.** Erlang's `re` module is built
on **PCRE** (the original Perl-Compatible Regular Expressions library, vendored
and patched inside the BEAM for cooperative yielding). PCRE2 is the maintained
successor to that exact library — same Perl-compatible *language*, just a newer
API — so it is the only interpreter engine that matches the BEAM side. The
alternatives were rejected:

- **RE2** — different engine; **no backreferences, no lookbehind**. Would
  silently be a *different regex language* than the BEAM backend.
- **`std::regex`** — ECMAScript/POSIX flavor; weak/absent named groups,
  different `\d`/Unicode semantics, no `(?x)` extended mode. Not PCRE-compatible.

Both would break `capture`/`matches?` parity across backends — exactly the
divergence `project_beam_general_parity` exists to prevent.

| Concern | Interpreter (C++) | BEAM |
|---|---|---|
| Compile | **PCRE2** (`pcre2_compile`) | `re:compile/2` (bundled PCRE) |
| Match | `pcre2_match` → `Match` value | `re:run/3` → `Match` value |
| Flags | inline `(?i)`/… → PCRE2 compile options | inline `(?…)` handed straight to `re` |

**Unicode is on by default — decided.** Erlang `re` operates on **bytes / UTF-8
binaries**; Unicode-aware `\d`/`\w`/`.`/case-folding only apply with its
`unicode`/`ucp` options. PCRE2 has the matching `PCRE2_UTF`/`PCRE2_UCP` flags.
These **must be pinned identically on both backends**, or a pattern like `\d`
will match different inputs depending on where it runs. Kex strings are logical
text, so both emitters set the pair unconditionally:

| Backend | Options |
|---|---|
| Interpreter | `PCRE2_UTF \| PCRE2_UCP` on every `pcre2_compile` |
| BEAM | `[unicode, ucp]` on every `re:compile/2` |

Consequence to spec-test: `\d` matches Arabic-Indic digits, `\w` matches
accented letters, and case-insensitive `(?i)` folds non-ASCII — identically on
both backends. There is no opt-out flag in v1; if an ASCII-only mode is ever
wanted it should be an explicit inline construct (`[0-9]`, or PCRE's `(*UCP_OFF)`
style), never a global default that can diverge.

Design the Kex `Regex`/`Match` API against these shared PCRE semantics so both
backends agree — the same backend-parity discipline used for the rest of the
language (`project_beam_general_parity`).

### BEAM implementation notes — two traps, both verified

These are not design choices; they are ways `re:run/3` will silently produce a
`Match` that disagrees with PCRE2 if implemented the obvious way.

**1. `binary` capture mode cannot express "group didn't participate".** The
`Match` design rests on absent-key-means-`None` versus empty-string-means-
`Just("")`. In `{capture, all, binary}` both come back as `<<>>` — indistinguishable:

```erlang
re:run("ac", "(a)(b)?(c)", [{capture, all, binary}]).  %% [<<"ac">>,<<"a">>,<<>>,<<"c">>]
re:run("ac", "(a)(x*)(c)", [{capture, all, binary}]).  %% [<<"ac">>,<<"a">>,<<>>,<<"c">>]
```

The BEAM backend must therefore use **`{capture, all, index}`**, where a
non-participating group is `{-1, 0}` and an empty match carries a real offset:

```erlang
re:run("ac", "(a)(b)?(c)", [{capture, all, index}]).   %% [{0,2},{0,1},{-1,0},{1,1}]
```

Slice the subject yourself from those offsets. This dovetails with the character-
offset decision above — you are converting offsets regardless, so index mode
costs nothing extra. (Note also that *trailing* unset groups are simply truncated
from the list, so key absence can't be inferred from list length alone.)

**2. `all_names` is alphabetical and nameless.** Named captures come back as
bare values ordered by name, with no names attached; the names come separately
from `re:inspect/2`, also alphabetical. They must be zipped:

```erlang
re:inspect(RE, namelist).                              %% {namelist,[<<"month">>,<<"year">>]}
re:run("2026-07", RE, [{capture, all_names, binary}]). %% [<<"07">>,<<"2026">>]
```

Getting this wrong swaps values between names *silently* — `(?<year>…)-(?<month>…)`
would populate `:year` with `07`. Since named groups are also numbered, the
safer construction is to build `Match` from the numbered `index` results and use
`namelist` only to map each name onto its group *number*.

### Interpolation safety

When a `regex` body interpolates (`` regex$`^${prefix}\d+` ``), the spliced value
is untrusted pattern text. **Quote-escape interpolated segments by default** —
treat `${x}` as a literal substring so user input can't inject pattern syntax.
**There is no opt-out**: escaping inside a tag is unconditional, which is what
lets every tag return a bare `Regex`. A caller who genuinely wants to build
pattern *structure* from values uses the call form —
`regex("(${a}|${b})")` -> `Result<Regex, RegexError>` — where the possibility of
failure is in the type.

**Escape per character; do not wrap in `\Q…\E`.** The obvious implementation —
splicing `"\\Q" + value + "\\E"` — is broken, and verified so: a value
containing `\E` closes the quoted span early and the remainder becomes live
pattern syntax.

```
value = "\E["      spliced as \Q…\E  ->  \Q\E[\E   ->  error: Unmatched [
```

That is simultaneously the injection hole this section exists to close and a
spurious compile failure. `Regex.quote` must therefore backslash-escape each
metacharacter individually (Perl's `quotemeta`, Ruby's `Regexp.escape`), which
has no terminator to escape out of. Perl's `qr/\Q$x\E/` is safe only because its
interpolation does exactly this per-character escaping rather than literal
`\Q…\E` splicing — the syntax misleads here.

This is cheap to get right because the tag receives parts and values separately,
so `regex` sees exactly which spans came from the author and which from a value.
Note also that interpolation now requires typing the `$` marker
(`` regex$`…` ``), so a pattern can never interpolate by accident — the raw form
`` regex`…` `` has no holes at all.

## Dependencies & sequencing

The two forms have very different prerequisites — **the call form needs almost
nothing; the tag form needs a whole language feature.**

- **`` regex(`…`) `` (call form)** depends only on **backtick raw strings**
  existing as a string-literal kind, plus a `Regex` module. No new grammar
  beyond the string literal. This is the near-term deliverable.
- **`` regex`…` `` (tag form)** depends on the **tagged-literal feature**
  (parser support for `identifier` immediately-adjacent-to backtick-body,
  desugaring to a parts/values call). That is a general language feature, fully
  designed in `docs/strings-and-tags-plan.md` — it is *not* regex-specific and
  should not be built under the banner of regex.
- **Compile-time validation** depends on companion-validator lookup and
  timeout-bounded compile-time evaluation of pure functions, both owned by the
  strings & tags plan. Nothing regex-side is needed beyond writing
  `validateRegex`.

Suggested order: (1) backtick raw strings, (2) `Regex` module + engine +
`Match` type behind the call form, (3) tagged literals as a separate workstream,
(4) `validateRegex` for compile-time checks, (5) `re` alias
falls out.

## Implementation phases (first cut = call form only)

**Status (2026-07-28): steps 1 and 6's prerequisite are already built**, so the
tag form is not blocked and step 5 needs only the validator function itself.
Nothing regex-specific exists yet — no `Regex` module, no PCRE2 dependency.

1. ✅ **Backtick raw strings** — *done* (PR #17, `27ac8a9`).
   `Token::RawString` / `InterpolatedRawString` (`src/lexer/token.hxx:96`),
   `Lexer::lexRawString(bool interpolating)`. Raw `\`, dedent margin, and the
   `$` interpolation marker all landed together.
   - ✅ **Tagged literals** — *done, ahead of the sequencing below.*
     `ast::TaggedLiteral` (`src/ast/ast.hxx:184`) carries parts/values/
     interpolating plus body offsets for carets; parsed at `parser.cxx:1704`,
     evaluated at `evaluator.cxx:1203`, lowered for BEAM at `ir/lower.cxx:590`.
   - ✅ **Compile-time validator machinery** — *done.*
     `validation::validateTaggedLiterals(program, analyzer, timeout = 1s)` in
     `src/validation/tag_validator.{hxx,cxx}` implements the companion-lookup
     convention and timeout-bounded pure evaluation that step 5 needs.
2. **`Regex` module + `Match` type** — `regex(String) -> Result<Regex,
   RegexError>` (settle the tag-unwrap sub-decision here), `Regex.quote`,
   and `Match` (map-like: `get(Int|Atom) -> String?`, `get(k, default)`).
3. **String ops** — `capture`, `matches?`, `scan`, `replace` (literal + block),
   `split`, as functions in `Regex`/prelude reachable via UFCS.
4. **Backends** — interpreter engine (see Backend mapping) + BEAM `re:compile`/
   `re:run`; keep both semantically aligned, spec-tested like the rest.
5. **`validateRegex`** — check raw tagged patterns at compile time;
   runtime-compile (cached) otherwise. Requires the
   compile-time-evaluation facility from the strings & tags plan.
6. **Tag form + `re` alias** — only after tagged literals land as their own
   feature; pure sugar over step 2's call.

## Open questions

- ~~**Interpreter engine choice**~~ — *resolved:* **PCRE2** (matches the PCRE
  the BEAM `re` uses). RE2 and `std::regex` rejected. See Backend mapping.
- ~~**Unicode/UCP default**~~ — *resolved:* **on**, pinned unconditionally on
  both backends (`PCRE2_UTF|PCRE2_UCP` / `[unicode, ucp]`). No opt-out flag in
  v1. See Backend mapping.
- ~~**Interpolation**~~ — *resolved* in `docs/strings-and-tags-plan.md`: raw by
  default, interpolation opt-in via a `$` marker (`` regex$`…` ``), parts/values
  desugaring. Regex-specific decision retained: **quote interpolated values by
  default** so `` regex$`^${x}` `` can't inject pattern syntax — via
  per-character escaping, **not** `\Q…\E` wrapping, which a value containing
  `\E` breaks out of. See "Interpolation safety" above.
- ~~**Is `matches?` anchored?**~~ — *resolved:* **no, unanchored** — it asks
  "does this string contain a match". See "Every operation is unanchored".
- ~~**`scan` vs Ruby**~~ — *resolved:* Kex returns `[Match]` uniformly, a
  deliberate divergence from Ruby's shape-shifting `scan`. See "`scan` returns
  `[Match]`, unlike Ruby".
- **Trailing-flag sugar** (`` regex`pat`i ``) in addition to inline `(?i)` — add
  later or never?
- ~~**PCRE2 in the wasm build**~~ — *resolved:* **yes, vendor it** —
  `third_party/pcre2-wasm/`, mirroring `gmp-wasm` exactly. Not started;
  **deferred until step 4**, since wiring a hard CMake dependency before
  anything links against PCRE2 would break every build for no benefit. Sketch
  for when it lands:
  - Static build via `emcmake cmake`, pinned to **Emscripten 5.0.7** like GMP
    (`third_party/gmp-wasm/README.md` explains the pin) — don't build the
    archive with Homebrew's newer emcc and link it with 5.0.7.
  - `-DBUILD_SHARED_LIBS=OFF -DPCRE2_BUILD_TESTS=OFF -DPCRE2_BUILD_PCRE2GREP=OFF`
  - **`-DPCRE2_SUPPORT_JIT=OFF`** — there is no JIT on wasm. This is the
    analogue of GMP's `--disable-assembly`, and it is worth checking whether
    the *native* build wants JIT on, since that would be a per-backend
    performance difference to keep in mind (not a semantic one).
  - **`-DPCRE2_SUPPORT_UNICODE=ON`** — required, not optional: it is what backs
    the `PCRE2_UTF|PCRE2_UCP` decision above. Without it `\d`/`\w` silently
    fall back to ASCII and diverge from BEAM.
  - `include/`+`lib/` gitignored as build artifacts (same entries pattern as
    `third_party/gmp-wasm/`), plus a CI cache step keyed on the PCRE2 and
    emsdk versions, mirroring the existing `Cache GMP-for-wasm` job.
  - PCRE2 is unlicense-adjacent BSD, so it raises none of the LGPL
    static-linking questions that GMP's `CMakeLists.txt` comment documents.
- ~~**What does `regex(bad)` do at runtime?**~~ — *resolved:*
  **`Result<Regex, RegexError>`**, following `Integer.parse`/`Parser.parse`/
  `Http`. See "Compilation failure is a `Result`".
- ~~**Do the tag and call forms differ in type?**~~ — *resolved:* **yes,
  deliberately.** `` regex`\d+` `` -> `Regex` (compile-time validated, cannot
  fail); `regex(s)` -> `Result<Regex, RegexError>`. Falls out of the existing
  two-argument tag ABI as a plain arity overload, sharing one implementation.
  This divergence is the justification for having both forms.
- ~~**Interpolating tags**~~ — *resolved:* they return `Regex` too. The rule has
  no exceptions — **every tag returns `Regex`, only the call form returns
  `Result`** — which holds because escaping is unconditional (no opt-out inside
  a tag), leaving validity decided by the compile-time-known skeleton. Dynamic
  pattern *structure* goes through the call form and its `Result`. Size-limit
  failures raise, as resource errors.
- **Where does the compile cache live?** The plan says computed patterns are
  "cached on first use" but not where. The interpreter can hold a process-local
  map; BEAM needs a deliberate choice (`persistent_term`, a named ETS table, or
  no cache at all — recompiling per call). Affects hot-loop performance and has
  to not leak across processes.
- **`Regex` value semantics** — what do `inspect`, `==`, and serialization do
  with a compiled `Regex`? Related to the `.kexo` hazard already noted under
  compile-time validation: a compiled pattern must not be embeddable in an
  artifact, so `Regex` likely needs to carry its source string and recompile
  rather than serialize engine state.
- ~~**`Match.get` default-arg ambiguity**~~ — *resolved:* `Map.get` already has
  the `K -> V?` / `K -> V -> V` two-arity shape (`src/prelude/map.kex`), and
  union-typed params are matched member-wise by the checker, so
  `get :> (Atom | Int) -> String?` accepts both `get(0)` and `get(:name)`.
  `Match` mirrors `Map.get` exactly. (See "Type-system verification" above.)
- ~~**Whole-match / group `0`**~~ — *resolved:* a successful match always has
  key `0`, possibly `""` (zero-width matches included). Only non-participating
  numbered/named groups are absent. See "The Match type".
- ~~**Zero-width matches**~~ — *resolved:* reported everywhere, engine-standard
  (Ruby/Python/JS/Erlang all agree). No Kex-side filtering. See "Zero-width
  matches follow the engines".
- ~~**`replace` arity**~~ — *resolved:* `replace` is global (Ruby's `gsub`)
  under a plainer name; no `sub`/`gsub` pair. A first-only `replaceFirst` can
  be added later without breaking anything.
- ~~**`split` semantics**~~ — *resolved:* Ruby's, including the trailing-empty
  trim, the `limit` argument, and interleaved capture groups — minus the awk
  `split(" ")` special case. Needs `trim`/`{parts,N}` on the BEAM side to match.
  See "`split` follows Ruby".
- ~~**Byte vs. char offsets**~~ — *resolved:* offsets are **character offsets**
  into the logical Kex string, on both backends. Erlang `re` reports *byte*
  offsets into a UTF-8 binary, so the BEAM side must convert before handing
  spans to Kex; PCRE2 likewise reports byte offsets into the UTF-8 subject.
  Neither backend may pass engine offsets through raw. (Still deferred with
  `.spans` — the decision is recorded so `.spans` can't be built the wrong way.)
