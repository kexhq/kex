# Strings, Chars, and Tagged Literals

## String literals

A double-quoted string is UTF-8 and supports `${…}` interpolation:

```kex
let name = "world"
IO.printLine("Hello, ${name}!")      # Hello, world!
IO.printLine("2 + 2 = ${2 + 2}")     # 2 + 2 = 4
```

Escape sequences: `\n`, `\r`, `\t`, `\\`, `\$`, `\"`. Write `\$` for a literal
dollar sign so it is not read as the start of an interpolation.

## `Char` vs `String`

`Char` is a single character, written `'a'`, `'0'`, `'\n'`. It is a **distinct
type** from `String` — `'a'` is not a 1-character string.

`[Char]` (a list of `Char`), on the other hand, **is** `String` — the same type,
interchangeable for comparison, concatenation, and display.

```kex
let c = "hello".at(1)        # Char?  → Just('e')  (indexing yields a Char)
c == 'e'                     # (after unwrap) true  — Char == Char
'a' + 'b'                    # "ab"   — concatenation builds a String
"ab" + 'c'                   # "abc"
['h', 'i'] == "hi"           # true   — [Char] IS String
IO.printLine(['h', 'i'])     # prints "hi"
```

A `Char` only compares and orders against another `Char`; comparing a `Char` to
a `String` is `false` (or, for ordering, an error — like any two unrelated
types).

## Common operations

String is `Foldable`/`Enumerable`, so it has the collection operations plus
string-specific ones. Selected methods (see `src/prelude/string.kex`):

| Method | Result | Notes |
|---|---|---|
| `s.count` / `s.length` | `Integer` | number of characters |
| `s.empty?` | `Bool` | |
| `s.at(i)` | `Char?` | `i`'th character |
| `s.chars` | `[Char]` | |
| `s.rest` | `String` | all but the first character |
| `s.reverse` | `String` | |
| `s.trim` | `String` | strip surrounding whitespace |
| `s.upperCase` / `s.lowerCase` | `String` | (also defined on `Char`) |
| `s.split(sep)` | `[String]` | split on a separator (or a `Regex` — see `docs/regex.md`) |
| `s.contains?(sub)` | `Bool` | |
| `s.startsWith?(p)` / `s.endsWith?(p)` | `Bool` | |
| `s.map(f)` / `s.filter(f)` / `s.reduce(acc, f)` | — | operate over `Char`s |

`Char` predicates: `digit?`, `alpha?`, `letter?`, `upper?`, `lower?`, `space?`,
and `c.in?(range)` (e.g. `c.in?('a'..'z')`).

```kex
"  Hello  ".trim.lowerCase            # "hello"
"a,b,c".split(",")                    # ["a", "b", "c"]
"level: ${n}".contains?("level")      # true
'7'.digit?                            # true
```

## Raw backtick strings

A backtick string is **multiline and non-interpolating**. Backslashes are always
literal, `${…}` is ordinary text, and two consecutive backticks inside the body
produce one literal backtick:

```kex
let pattern = `\d+`                          # backslash is literal
let text    = `the syntax ${name} stays raw` # not interpolated
let quoted  = `a ``backtick```               # → a `backtick`
```

When the opening backtick is followed immediately by a newline and the closing
backtick sits on its own line, the whitespace prefix before the closing backtick
is stripped from every non-blank content line (a dedent margin). The opening
newline is dropped; the newline before the closing line stays.

```kex
let block = `
  line one
  line two
  `
# "line one\nline two"
```

## Interpolating backtick strings

A `$` before the opening backtick enables `${…}` holes while keeping backslashes
literal — useful for patterns and templates that also need interpolation:

```kex
let name = "Ada"
let greeting = $`Hello, ${name}!`
```

Write `$${` for a literal `${`.

## Tagged literals

An adjacent lower-case identifier **tags** a raw backtick string. The tag is an
ordinary function called as `tag(parts, values)`:

```kex
let text = myTag`raw body`
# calls myTag(["raw body"], [])
```

- A raw tag supplies one string in `parts` and an empty `values` list.
- The `$` marker works with a tag too: `` capture$`… ${x} …` `` calls
  `capture(["… ", " …"], [x])`, passing values in their original types so the
  tag decides whether to escape, bind, or reject them.
- **Adjacency matters:** whitespace between the identifier and the backtick
  disables tagging — `` myTag `body` `` is two separate expressions.
- **The tag must be a bare lower-case identifier.** It is *not* a method call and
  cannot be module-qualified: `` Regex.regex`…` `` is **not** "the `regex` tag
  from module `Regex`" — the parser reads it as member access `Regex.regex`
  followed by a stray backtick string. Bring the tag into scope with
  `using Regex` and write it bare: `` regex`…` `` (see `docs/regex.md`).

### Compile-time validation

A raw tag may have a **pure companion** named `validate<Tag>` (`regex` →
`validateRegex`, `html` → `validateHtml`, `myTag` → `validateMyTag`) with the
signature `String -> [TaggedValidation.Issue]`. The compiler runs it at compile
time on the raw literal body; fatal issues fail the build, warnings continue it:

```kex
let query(parts: [String], values: [Any]) -> String = parts.first.or("")

let validateQuery(source: String) -> [TaggedValidation.Issue] do
  if source.blank? then
    [TaggedValidation.fatal("query must not be empty")]
  else
    []
  end
end

let users = query`SELECT * FROM users`   # validated at compile time
```

`TaggedValidation` provides `fatal`, `fatalAt`, `fatalBetween`, `warn`, `warnAt`,
and `warnBetween`; offsets are zero-based UTF-8 byte offsets into the cooked
literal body.

> Only **raw** tagged literals are validated. Interpolating tags
> (`` tag$`…` ``) and ordinary function calls remain runtime operations — see
> `docs/known-gaps.md`.

See `examples/backticks.kex`, `examples/interp.kex`, and `examples/strings.kex`
for runnable examples, and `docs/regex.md` for the most-used tag.
