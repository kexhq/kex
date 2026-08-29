# Kex Style Guide

The required style for Kex (`.kex`) source. This document is the specification
that `kex fmt` (the formatter) and `kex lint` (the standard lint) implement:

- **[F]** — **Formatter.** Mechanical; the formatter rewrites the code. Never
  a lint diagnostic, because it cannot survive a format.
- **[L]** — **Lint.** Not mechanically fixable in general; reported as a
  diagnostic with the rule ID from §15.
- **[A]** — **Advisory.** House style for the stdlib and examples. Not
  enforced by default; available under `--pedantic`.

The reference corpus is `src/stdlib/**/*.kex` and `examples/**/*.kex`. Where
this document and the corpus disagree, the corpus is the bug.

---

## 1. Files

**[F]** UTF-8, no BOM. LF line endings. Exactly one trailing newline at end of
file. No trailing whitespace on any line. No tabs anywhere — indentation and
alignment are spaces.

**[F]** No form feeds, no vertical tabs, no non-breaking spaces outside string
literals.

**[L]** File names are lowercase, no separators: `optionparser.kex`,
`taggedvalidation.kex`, `filehandle.kex`. Directories group a family:
`net/socket.kex`, `data/queue.kex`, `control/retry.kex`. A file that defines
one module is named after it, lowercased and de-punctuated (`URI` → `uri.kex`,
`Kex.AST` → `kex/ast.kex`).

**[L]** Tests live beside the code as `<name>.spec.kex`, which auto-loads
`<name>.kex`'s declarations — do not re-declare or wrap in `main`.

---

## 2. Indentation and blank lines

**[F]** Two spaces per level. Every `do`/`end`, `record`/`end`, `match`/`end`,
`private do`/`end` pair opens one level; `end` sits at the column of the line
that opened it.

**[F]** At most one consecutive blank line inside a block; at most two at top
level. No blank line immediately after a `do` or immediately before an `end`.

**[A]** One blank line between top-level declarations. A declaration and the
doc comment that describes it are one unit: no blank line between them.

---

## 3. Line length

**[F]** Soft limit 80 columns. The formatter wraps what it can wrap (argument
lists, collection literals, chains) to fit.

**[L]** Hard limit 100 columns (`line-too-long`). Exempt: a line that cannot be
split — a long string literal, a URL in a comment, a wide `type` alternation.
A `type` with many variants that exceeds 100 is wrapped one variant per line
(§8).

---

## 4. Naming

**[L]**

| Kind | Convention | Example |
|------|-----------|---------|
| Module | `PascalCase`, dotted for nesting | `URI`, `Kex.Intrinsic.List` |
| Record, type, trait | `PascalCase` | `Input`, `ParseError`, `Enumerable` |
| Type variant | `PascalCase` | `Just`, `Unexpected` |
| Function, method, binding | `camelCase` | `fromIRI`, `takeWhile`, `peekAt` |
| Field | `camelCase` | `httpClient`, `pos` |
| Atom | `:lowerCamel` or `:lowercase` | `:macos`, `:allowComments` |
| Type parameter | single uppercase letter | `X`, `E`, `L`, `R` |

**[L]** A function returning `Bool` ends in `?`: `set?`, `empty?`, `absolute?`,
`ok?`. A field holding a `Bool` does too: `compiled? : Bool`. Do not prefix
with `is` or `has` — `isEmpty` is wrong, `empty?` is right (`predicate-naming`).

**[L]** `!` is not part of a name. It is the mutating-call operator on a `var`
(§11) and may only be written at a call site.

**[A]** Prefer the noun for the thing produced over a `get`/`compute` verb:
`parse`, `count`, `parent`, not `getParent`. Reserve verbs for actions with
effects: `write`, `send`, `close`.

**[A]** Do not abbreviate beyond the well-known (`src`, `dst`, `pos`, `acc`,
`idx`). Single-letter names are for type parameters, block parameters in short
lambdas, and mathematical arguments.

---

## 5. Declarations and signatures

**[F]** Signature separators and their spacing:

```kex
parse : String -> Result<URI, URIError>     # free function: name : Type
or    :> X -> X                             # method in a make/trait: name :> Type
```

`:` for a module-level function, `:>` for a method whose receiver is `this`.
One space either side of the arrow, one space either side of `:` / `:>`.

**[L]** A signature sits on the line directly above the definition it types,
with no blank line between them (`signature-adjacent`).

**[L]** Every public function in a module or `make` block carries a signature.
Private helpers and local `let` bindings do not need one
(`missing-public-signature`).

**[F]** Parameter annotations bind tight: `x: Integer`, no space before the
colon. Record fields are the opposite: `input : String`, one space before the
colon (§8). This asymmetry is deliberate and the formatter enforces both.

**[F]** Return annotation is ` -> Type` before the `do` or `=`:

```kex
let peekAt(offset: Integer) -> Char? do
```

**[F]** Body form follows length: `let name(args) = expr` when the body is a
single expression that fits, `let name(args) do ... end` otherwise. The
formatter converts a one-expression `do ... end` body **only** when the result
fits the soft limit and the body holds no comment.

**[L]** Multi-clause functions are contiguous — every clause of `first` is
adjacent, most specific first, with no unrelated declaration in between
(`split-clauses`).

**[A]** Prefer multi-clause definitions with patterns over a single clause that
opens with a `match` on its own argument:

```kex
let set?(@Just(_)) = true      # yes
let set?(@None)    = false
```

---

## 6. Visibility and modules

**[L]** One top-level module per file. `module Name` with no `do ... end` opens
file-scope module form and must be the first declaration in the file; nested
modules use `module Name do ... end`.

**[F]** `using` declarations come first in their scope, after the module
declaration and its doc comment, before any other declaration. One `using` per
line, alphabetized.

**[L]** Internal helpers go in a single `private do ... end` block, placed
**after** the public API of that module or `make` block (`private-placement`).
Do not scatter multiple `private` blocks.

**[A]** Reach for a qualified call (`Math.Constants.pi`) over `using` when a
module is used two or three times. Reserve `using` for a module you lean on
throughout the file, or for a DSL scope.

---

## 7. Traits and `make`

**[F]** `make Type, implement: Trait do` — comma before `implement:`, one space
after each colon. Multiple traits are comma-separated on one line:
`make [X], implement: Enumerable, Foldable do`.

**[L]** Inside a `make`, reach for the receiver's fields with `@field`, not
`this.field`. Use `this` only for a whole-receiver reference or to call another
method on it (`prefer-at-field`):

```kex
make Circle, implement: Shape do
  let area      = Math.PI * @radius * @radius
  let describe  = "Rectangle (area=${this.area})"    # this.area is a method
end
```

**[A]** Order a `make` block: required trait methods first, then the rest, then
`private do`.

**[A]** A trait's default method bodies are for behavior derivable from the
required methods. If a default needs a cast or a type test to work, it is not
a default — make it required.

---

## 8. Types, records, and literals

**[F]** `type` alternation on one line while it fits; one variant per line
otherwise, with `|` leading and aligned under the `=`:

```kex
type ParseError = Unexpected(String, Integer)
                | Expected(String, Integer)
                | NoMatch(Integer)
```

**[L]** A single-variant ADT keeps a leading `|` — `type E = | Boom` — which is
what distinguishes it from the alias `type FilePath = String`
(`single-variant-pipe`).

**[F]** Record fields: one per line, `name : Type`, defaults as ` = value`.

```kex
record Input do
  input : String
  pos   : Integer = 0
end
```

**[F]** **Alignment is preserved, never invented and never destroyed.** If the
author aligned the `:` in a record, the `=` in a run of `let`s, or the `=>` in
a run of match arms, the formatter keeps the alignment and re-computes the
column. If they did not, it does not add any. Alignment is a per-run choice:
one blank line or a non-conforming line ends the run.

**[F]** Collection and call literals fit on one line, or break one element per
line with the closing bracket at the opening line's indent. No trailing comma
on the closing line; no dangling opening bracket alone on a line.

**[F]** `X?` is the spelling of `Optional<X>` — the formatter rewrites the long
form in type position.

**[A]** Strings are double-quoted; `'c'` is a `Char`, not a one-character
string. Prefer `"${a}, ${b}"` interpolation over chained `+`.

---

## 9. Control flow

**[F]** A conditional whose branches are single expressions is written
`<cond> then <a> else <b>` — no `if`, no `end`:

```kex
let peek = @pos < @input.count then @input.at(@pos) else None
```

Spell out `if` / `elif` / `else` / `end` only when a branch occupies its own
line. `elif`, not `else if`.

**[L]** An `if` whose entire body is a `return`, and which fits, uses the
trailing modifier form (`prefer-guard-modifier`):

```kex
return "." if present.empty?
return None if index < 0 || index >= @input.count
```

**[F]** Match arms are `pattern => expr`, one per line. A guard is a trailing
`if` on the pattern: `n if n > 0 => "positive"`. The bare `when expr => expr`
form is for a condition chain with no scrutinee.

**[L]** Every `match` is exhaustive by pattern, not by a trailing `_` that
exists only to silence the checker. A `_` arm is for genuinely "everything
else", and comes last (`wildcard-not-last`, `lazy-wildcard`).

**[L]** Multi-line boolean conditions require parentheses:

```kex
if (a
    && b)
```

**[A]** Prefer early `return ... if` guards to a nested `if`/`else` pyramid.
Two levels of nesting inside a function body is the practical ceiling.

---

## 10. Blocks, lambdas, and chains

**[F]** A block body that fits on one line uses braces; anything multi-line
uses `do ... end`. This is not a preference — `{ |x| ... }` spanning lines is a
format error.

```kex
scores.filter { |n| n > 3 }

users.each do |u|
  IO.printLine(u.name)
  audit(u)
end
```

**[F]** Block parameters: `{ |n| ... }` — one space inside each brace, no space
inside the pipes, `, ` between parameters.

**[L]** A lambda that only calls one method on its argument is written with
`&.`; a lambda that only calls a named function is written with `~`
(`prefer-receiver-shorthand`, `prefer-capture`):

```kex
list.filter!(&.even?)        # not { |x| x.even? }
word = cursor.takeWhile(~alpha?)   # not { |c| alpha?(c) }
```

`&` is always followed by `.` and an identifier; every operator capture goes
through `~`: `~(+)`, `~(!)`. `&name` and `&.+` are invalid.

**[F]** A method chain fits on one line, or breaks with each `.` leading its
own line, indented one level from the receiver:

```kex
let parts = path.split(FS.Path.separator)
  .filter { |part| !part.empty? && part != "." }
```

**[A]** There is no pipe operator and there never will be — UFCS is the
chaining mechanism. Do not simulate one with a helper.

**[A]** No custom operators. The operator set is fixed; overriding an existing
operator in a `make` block is the only extension point, and only where the
meaning is the conventional one (`+` on `Vector`, `==` structurally).

---

## 11. Purity and mutability

**[L]** `foul` marks a function with side effects, and only ever a function —
there is no foul block and no foul module. A pure function cannot call a foul
one; that is a compile error, not a lint (`purity-violation` covers only the
advisory cases the checker skips).

**[L]** Do not mark a function `foul` unless it actually performs an effect.
`foul` on a pure body is a diagnostic (`gratuitous-foul`) — it poisons every
caller.

**[L]** `var` is for a mutable local whose mutation does not escape the
function. `var` that persists across `receive` cycles is process state and its
owner must be `foul` (`escaping-var`).

**[F]** The `!` mutating call takes its arguments like any call:
`list.push!(6)`, `ages.put!("alice", 33)`. It is only legal on a `var`.

**[A]** Prefer `xs.filter!(&.even?)` to `xs = xs.filter(&.even?)` when the
binding is already a `var` — it is the same thing and says so.

---

## 12. Errors

**[L]** No exceptions. A function that can fail returns `Result<X, E>` when the
reason matters to the caller, and `X?` when it does not. Do not encode failure
as a sentinel value, an empty collection, or a bare `Bool`
(`sentinel-failure`).

**[L]** Prefer `.try` to hand-matching `Ok`/`Error` or `Just`/`None` where the
enclosing function can propagate the failure (`prefer-try`):

```kex
let base = URL.parse("https://example.test/a/").try     # yes

match URL.parse(url) do                                  # only when the
  Ok(u)    => ...                                        # two branches
  Error(e) => ...                                        # really differ
end
```

**[L]** Close an optional chain with a single `.or(default)` at the end rather
than unwrapping in the middle (`early-unwrap`).

**[L]** `die` is for a broken invariant, never for an expected failure, and
never in library code that a caller could reasonably recover from
(`die-in-library`).

**[A]** Error types are records with a `kind` drawn from a closed variant type,
plus a human `message`, plus optional position/context — see `URIError` and
`NetError`. Do not return a bare `String` as an error.

---

## 13. Comments and doc comments

**[F]** `#` followed by one space. Comments are indented to the code they
describe. No comment banners of repeated punctuation except a section divider
in an example file.

**[L]** A public declaration carries a doc comment, in the block of `#` lines
directly above it (`missing-doc`). The format is RDoc-like:

```kex
# Returns the first element wrapped in +Just+, or +None+ if the list is empty.
#
# @param f [X -> Number] how each element is scored
# @return [X?] the first element, or +None+
#
# @example
#   [1, 2, 3].first   # => Just(1)
#   [].first          # => None
first :> X?
```

- First paragraph is a one-sentence summary in the third person present
  (“Returns…”, “Parses…”), ending in a period.
- `+code+` marks inline code; it renders as backticks.
- Paragraphs are separated by a bare `#`.
- Tags, in order: `@param` (one per parameter, in order), `@return`,
  `@example`, `@deprecated`. `@example` may carry a title on its own line.
- Example lines are indented three spaces after the `#` and show results as
  `# => value`, aligned within an example block.

**[L]** Tag/parameter agreement: an `@param` must name a real parameter and
every parameter should have one (`doc-param-mismatch`).

**[A]** A doc comment explains what the caller gets and when to reach for this
over the alternative. It does not restate the signature.

---

## 14. Tests

**[L]** Specs use `describe` / `it` with `assert`; `before` / `after` are
available. One `describe` per file, named for the unit under test.

**[A]** `it` names finish the sentence “it …”: `it("returns None for an empty
list")`. Assert on values, not on formatting of values.

**[A]** Keep an `it` to one behavior. A spec that asserts a dozen unrelated
facts is a spec that reports one failure for a dozen bugs.

---

## 15. Lint rule index

| ID | § | Default |
|----|---|---------|
| `line-too-long` | 3 | warn |
| `predicate-naming` | 4 | warn |
| `signature-adjacent` | 5 | error |
| `missing-public-signature` | 5 | warn |
| `split-clauses` | 5 | error |
| `private-placement` | 6 | warn |
| `prefer-at-field` | 7 | warn |
| `single-variant-pipe` | 8 | error |
| `prefer-guard-modifier` | 9 | warn |
| `wildcard-not-last` | 9 | error |
| `lazy-wildcard` | 9 | warn |
| `prefer-receiver-shorthand` | 10 | warn |
| `prefer-capture` | 10 | warn |
| `gratuitous-foul` | 11 | warn |
| `escaping-var` | 11 | error |
| `sentinel-failure` | 12 | warn |
| `prefer-try` | 12 | warn |
| `early-unwrap` | 12 | warn |
| `die-in-library` | 12 | error |
| `missing-doc` | 13 | warn (error in stdlib) |
| `doc-param-mismatch` | 13 | warn |

A rule is suppressed for the next declaration with `# lint:allow <id> — reason`.
The reason is required; a bare suppression is itself a diagnostic.

---

## 16. Formatter guarantees

The formatter is expected to hold to these, and its test suite should pin them:

1. **Idempotent.** `fmt(fmt(x)) == fmt(x)`.
2. **Semantics-preserving.** The AST after formatting is equal to the AST
   before, modulo position.
3. **Comment-preserving.** Every comment survives, attached to the same
   declaration or expression.
4. **Never reflows prose.** Text inside comments and string literals is not
   rewrapped, re-spaced, or re-indented.
5. **Respects author alignment** as described in §8 — it re-computes alignment
   columns but never adds or removes the choice to align.
6. **Never changes a `do ... end` body to `= expr`** when a comment lives
   inside it, and never the reverse when the result would exceed 80 columns.
7. **Fails loudly** on input it cannot parse, and writes nothing.
