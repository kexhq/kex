# Kex Style Guide

The required style for Kex (`.kex`) source. This document is the specification
that `kex fmt` (the formatter) and `kex lint` (the standard lint) implement:

- **[F]** — **Formatter.** Mechanical; the formatter rewrites the code. Never
  a lint diagnostic, because it cannot survive a format.
- **[L]** — **Lint.** Not mechanically fixable in general; reported as a
  diagnostic with the rule ID from §21.
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
(§10).

---

## 4. Expression spacing

**[F]** One space either side of every binary operator, including `=` in a
binding. Two exceptions: `..` takes none, and a token being aligned to a column
(§10) takes the padding that alignment computes — one space is the minimum
there, not the rule:

```kex
let total = subtotal + tax * rate
let inRange = n >= low && n <= high
let window = 1..10                   # no spaces around `..`
```

**[F]** No space between a unary operator and its operand: `-count`, `!found?`,
`~parse`, `&.even?`, `@radius`, `...rest`. `-` is unary only where a binary
reading is impossible; `a - b` keeps its spaces.

**[F]** No space inside parentheses or square brackets, and none before a
call's opening paren: `f(x)`, `[1, 2, 3]`, `xs[0]`, `Just(x)`, `(a + b) * c`. A
space after every comma, none before it.

**[F]** Braces are the opposite: one space inside each, on both the map literal
and the block — `{ "name": "Kex" }`, `{ |n| n > 3 }` (§12). A brace that
follows a call is preceded by one space: `scores.filter { |n| n > 3 }`.

**[F]** Argument labels and map keys bind tight on the left, loose on the
right: `parse(text, options: opts)`, `{ "name": "Kex" }`. This is the same
shape as a parameter annotation (§6), and the opposite of a record
field's declaration colon.

**[F]** Interpolation holes carry no inner padding: `"${name}"`, never
`"${ name }"`. The expression inside a hole is spaced by these same rules:
`"${a + b}"`.

**[F]** A trailing comment is separated from code by at least two spaces, and
`#` is followed by one:

```kex
let margin = 4        # points, not pixels
```

**[A]** Spacing is not an emphasis mechanism. Extra spaces around an operator to
suggest precedence — `a*b + c*d` — are removed by the formatter; use
parentheses when precedence needs saying out loud.

---

## 5. Naming

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
(§13) and may only be written at a call site.

**[A]** Prefer the noun for the thing produced over a `get`/`compute` verb:
`parse`, `count`, `parent`, not `getParent`. Reserve verbs for actions with
effects: `write`, `send`, `close`.

**[A]** Do not abbreviate beyond the well-known (`src`, `dst`, `pos`, `acc`,
`idx`). Single-letter names are for type parameters, block parameters in short
lambdas, and mathematical arguments.

---

## 6. Declarations and signatures

**[F]** Signature separators and their spacing:

```kex
parse : String -> URI or! URIError   # module-level function
or    :> X -> X                      # method; the receiver is this
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
colon (§10). This asymmetry is deliberate and the formatter enforces both.

**[F]** Return annotation is ` -> Type` before the `do` or `=`:

```kex
let peekAt(offset: Integer) -> Char? do
```

**[F]** Body form follows length: `let name(args) = expr` when the body is a
single expression that fits, `let name(args) do ... end` otherwise. A
single-expression function takes neither a block nor a `return` — the
formatter collapses one that has them, dropping the `return` with the `do`:

```kex
let square(n: Integer) -> Integer = n * n           # yes

let square(n: Integer) -> Integer do                # no — collapses to the above
  return n * n
end
```

The collapse happens **only** when the result fits the soft limit and the body
holds no comment; otherwise the block stays and the `return` with it.

**[A]** **Every early exit is an explicit `return`; the last one may be
implicit.** The trailing expression of a body is the one exit that can go
unspoken, and leaving it bare is fine even when guards above it return
explicitly — a guard is visibly an exit because it is a `return` on its own
line, so nothing is hidden by the final expression not repeating the word.

```kex
let basename(path: String, suffix: String) -> String do    # both fine
  let name = path.split(FS.Path.separator).last.or("")
  return "" if name.startsWith?(".")
  name.dropSuffix(suffix)
end

let basename(path: String, suffix: String) -> String do
  let name = path.split(FS.Path.separator).last.or("")
  return "" if name.startsWith?(".")
  return name.dropSuffix(suffix)
end
```

Exits are counted at the statement level of the body: a body that is one
`match`, `if`/`else`, or `then`/`else` expression has one exit no matter how
many arms or branches it has, and needs no `return` at all.

What stays out is a bare expression in the *middle* of a body — a value
computed, not bound, and not returned. That is dead code wherever the language
does not treat it as an exit.

**[L]** Multi-clause functions are contiguous — every clause of `first` is
adjacent, most specific first, with no unrelated declaration in between
(`split-clauses`).

**[L]** **A pattern-matching function types itself once, in a standalone
signature.** Clause heads then carry patterns only — no per-parameter
annotations, no return annotation (`clause-signature`):

```kex
or :> X -> X                      # yes — the types live here
let or(@Just(x), _) = x
let or(@None, default) = default

let or(@Just(x: X), _) = x        # no — annotations repeated per clause
let or(@None, default: X) -> X = default
```

The signature is the one place the reader looks for the type, and the one place
it has to change. Annotating clause heads instead means saying it as many times
as there are clauses, and nothing keeps those spellings agreeing with each
other — clauses can drift apart in a way a single signature cannot.

This applies whatever the function's visibility: `missing-public-signature`
asks for a signature on the public API, and a multi-clause function wants one
even when it is private or local, because the clauses cannot carry the type
between them.

**[L]** **A function with no parameters drops its parentheses**, at the
definition and at every call site (`empty-parens`). Empty parens are noise;
UFCS makes the bare name read as the property it usually is:

```kex
foul getLine -> String = ...        # yes
foul getLine() -> String = ...      # no

names.first.or("nobody")            # yes
names.first().or("nobody")          # no
```

This holds for predicates (`empty?`, `ok?`) and for zero-argument methods on a
receiver alike. Parens come back the moment there is an argument to put in
them.

The rule is about **declared** no-parameter functions. It does not touch a
call through a function-valued binding, where the parens are what distinguishes
invoking the function from naming it — and it never rewrites a capture, which
is spelled `~name` (§12). This is why the rule is a lint rather than a
reformat: telling those apart needs resolved names, not just a parse tree.

**[L]** **Match in the head, not the body.** A function whose body is a `match`
on one of its own parameters is written as clauses instead — the patterns
belong in the parameter list (`param-match`):

```kex
let set?(@Just(_)) = true          # yes
let set?(@None)    = false

let set?(opt) do                   # no — the match is the whole body
  match opt do
    Just(_) => true
    None    => false
  end
end
```

Inside a `make`, the same holds for the receiver, matched with a leading `@`:

```kex
make Optional<X> do
  let or(@Just(x), _)      = x     # yes
  let or(@None, default)   = default

  let or(default) do               # no — matching this in the body
    match this do
      Just(x) => x
      None    => default
    end
  end
end
```

`@` is only needed at the top level of a clause head; inside a nested pattern,
ordinary pattern syntax applies (`let render({ children: [first | _] }) = ...`).

The rule fires when the scrutinee is exactly a parameter or the receiver and
the `match` is the entire body. A `match` on a computed value, on a tuple of
two parameters, or one that is a step among others in the body is untouched —
as is a `match` on a `Bool`, which §11 sends to a conditional rather than to
clauses.

---

## 7. Visibility and modules

**[L]** One top-level module per file. `module Name` with no `do ... end` opens
file-scope module form and must be the first declaration in the file; nested
modules use `module Name do ... end`.

**[F]** Within whatever scope it appears, `using` comes first: after the
`module` declaration and its doc comment at file scope, or at the top of the
body at function and block scope, before any other declaration. One `using` per
line, alphabetized — and the names inside an `only:` list alphabetized too.

**[L]** **`using` goes in the narrowest scope that needs it.** If exactly one
function or block uses a module's names, the `using` belongs inside that
function or block, not at file scope (`overbroad-using`). File-scope `using` is
for a module the file leans on throughout.

```kex
module App do
  using Http do                                  # yes — only these handlers need it
    let handleHome(req: Request) -> Response = Response.ok("Welcome!")
  end

  let unrelated(n: Integer) -> Integer = n * 2   # unaffected by the import
end

let report(rows: [Row]) -> String do             # yes — one function, one import
  using Formatting
  return rows.map(&.render).join("\n")
end
```

Both forms are available: `using M do ... end` scopes the names to a block,
while a bare `using M` inside a body scopes them to the rest of that body.

**[L]** **An import names what it takes.** Write `using M, only: [f, g]` and
keep the list short — a handful of names, on one line (`using-only-list`):

```kex
using Math, only: [square, cube]     # yes
using Math                           # no — every public name, silently
using Math, except: [cube]           # no — grows as Math grows
```

A short `only:` list is a claim about coupling that stays true: the reader sees
what this scope borrowed, and a name added to `M` later cannot appear here
without an edit. `except:` inverts that — it takes everything the module has
now plus everything it gains — so reach for it only when a module is genuinely
used wholesale minus a name or two.

When the list stops being short, the import is telling you something. Either
the scope leans on `M` throughout, in which case a bare `using M` at the
narrowest scope that needs it is honest about it, or the names belong to a module that
wants splitting. `as:` is the third option: `using Pricing, as: P` keeps the
call sites qualified while shortening a long path, and imports nothing.

Bare `using M` stays right for a DSL, whose whole point is that its vocabulary
is in scope unqualified.

**[L]** **A `using` block that contains everything should be a `using`
statement.** The block form exists to limit which declarations can see `M`'s
names. When the block holds every declaration in its enclosing body, it limits
nothing — the names are in scope for the whole body either way — and all it has
added is a level of indentation. Write the statement form instead
(`redundant-using-block`).

The usual place this shows up is a file that is one module:

```kex
module JSON
using Parsing                       # yes — statement form, scopes to the file

record Document do
  root : Value
end

let parse(text: String) -> Document or! ParseError = ...
```

```kex
module JSON
using Parsing do                    # no — this block holds the whole file
  record Document do
    root : Value
  end

  let parse(text: String) -> Document or! ParseError = ...
end
```

Both put `Parsing`'s names in scope for every declaration in the file. The
second only indents them to say so.

The block earns its keep when something is left outside it — the `App` / `Http`
example above, where `unrelated` sits outside the block and cannot see Http's
names — or a DSL scope.

**[L]** Internal helpers go in a single `private do ... end` block, placed
**after** the public API of that module or `make` block (`private-placement`).
Do not scatter multiple `private` blocks.

**[L]** **Do not qualify a name with the module you are already in.** Inside
`module Tey.Manifest`, its own `argument` is `argument`, not
`Tey.Manifest.argument` (`self-qualified-call`). The prefix says "this comes
from somewhere else", which is exactly wrong, and it turns every call site into
a line that has to be read past:

```kex
let value = argument(arguments).or("")               # yes
let value = Tey.Manifest.argument(arguments).or("")  # no — you are in Tey.Manifest
```

A name that needs the prefix to resolve is a name that collides with something
in scope, which is worth fixing at the declaration rather than papering over at
29 call sites.

**[A]** Narrower still is no import at all: a qualified call
(`Math.Constants.pi`) beats any `using` for a module referenced two or three
times. The ladder, narrowest first: a qualified call, then `as:` for a long
path, then `using M, only: [...]` in the tightest scope that needs it, and only
then a bare `using M`.

### Re-exporting

`export` republishes another module's names as this module's own, with the same
`only:` / `except:` options `using` takes:

```kex
module Prelude do
  export Geometry                      # everything Geometry exports
  export Math, only: [add, mul]        # two names, republished here
  export Text, except: [internalTrim]  # all but one
end
```

**[L]** An `export` list is subject to the same discipline as an import: name
what you re-export (`using-only-list`). A facade module that fronts several
implementation modules is the case `export` exists for; anything else is a
module boundary that has not been decided yet.

**[A]** `export` and `private do` are the two halves of a module's surface —
what it adds and what it withholds. A module that needs neither is exporting
exactly its own public declarations, which is the common case and wants no
ceremony.

---

## 8. Traits and `make`

**[F]** `make Type, implement: Trait do` — comma before `implement:`, one space
after each colon. Multiple traits are comma-separated on one line:
`make [X], implement: Enumerable, Foldable do`.

**[L]** Inside a `make`, reach for the receiver's fields with `@field`, not
`this.field`. Use `this` only for a whole-receiver reference or to call another
method on it (`prefer-at-field`):

```kex
make Circle, implement: Shape do
  let area      = Math.PI * @radius * @radius
  let describe  = "Circle r=${@radius} (area=${this.area})"   # a method
end
```

**[L]** **Return `New { ... }`, not the type with every field restated.** Inside
a `make`, `New` is the receiver cloned: it seeds from `this`, so a copy names
only the fields that move and inherits the rest (`prefer-new-copy`):

```kex
make OptionConfig do
  let flag(long: String) -> OptionConfig do          # yes
    let option = OptionSpec { long: long, kind: FlagValue }
    New { options: [...@options, option] }
  end

  let within(timeout) = New { timeout: timeout }     # yes

  let flag(long: String) -> OptionConfig do          # no — restates the record
    let option = OptionSpec { long: long, kind: FlagValue }
    OptionConfig { options: [...@options, option], commands: @commands,
                   description: @description, version: @version }
  end
end
```

Restating the type is not just longer, it rots: a field added to the record
later takes its default in every copy that spelled the fields out, rather than
the receiver's value — where the new field has no default, the same code stops
compiling instead. `New` carries both cases through untouched. `New {}` with no
overrides is a copy equal to the receiver.

`New` resolves only inside a `make` block, where there is a receiver to clone
from. A record built from nothing — a constructor, a parse result, a literal in
a test — names its type: `Input { input: text }`. The literal form is
capitalized like `This`; lowercase `new` is the sibling binding you assign
fields on (`new.items = ...`), not a second spelling of this one.

**[A]** Order a `make` block: required trait methods first, then the rest, then
`private do`.

**[A]** A trait's default method bodies are for behavior derivable from the
required methods. If a default needs a cast or a type test to work, it is not
a default — make it required.

---

## 9. Generics and constraints

**[A]** Reach for a type parameter when the function genuinely does not care
what it holds: `first : [X] -> X?` works for every element type because it
never inspects one. If the body needs any capability of `X`, that capability
belongs in the signature rather than in a comment.

**[L]** Constrain by asking for the trait, not by taking a concrete type and
hoping: a parameter typed `Shape` accepts anything that `implement: Shape`, and
the checker holds the caller to it (`prefer-trait-param`):

```kex
foul printShape(s: Shape) = ...            # yes — any Shape
foul printShape(s: Circle) = ...           # no — needlessly narrow
```

**[A]** Two capabilities at once is an intersection: `A & B` means a value
satisfying both, and it means the same thing in a parameter, a return, a field,
or an alias. An open record type — `{ label: String }` — asks only for the
field the code touches, which is the lightest constraint available and the one
to prefer when a whole trait would be overkill.

**[A]** `This` is the receiver's own type inside a `make` or `trait` block. A
method returning a new receiver returns `This`, so the type follows the
implementing type rather than being restated per implementation.

**[A]** The marker traits — `Optionable`, `Resultable`, `Eitherable` — exist to
constrain a parameter to "some optional", "some result", "some either" without
naming its payload. Use them for that and nothing else; they carry no methods.

**[A]** Single uppercase letters are the convention for type parameters (§5),
and their meaning should be recoverable from position: `X` the element, `E` the
error, `L`/`R` the two sides. A parameter that needs a longer name is usually a
sign the abstraction is doing too much.

---

## 10. Types, records, and literals

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

**[F]** **Alignment is computed, not preserved.** The formatter aligns every
run to a column; author spacing is discarded and re-derived. There is no
opt-out and no opt-in — aligned is the only conforming output, so alignment
never shows up in a diff as a matter of taste.

A **run** is a maximal group of consecutive lines that are siblings at the same
indentation and of the same kind. A run is ended by a blank line, a comment
line, a line of a different kind, or a construct that spans more than one line.

These kinds align, on the token named:

| Kind | Aligned token |
|------|---------------|
| Record fields | `:`, then the `=` of defaults as a second column |
| One-line `let` / `foul` definitions | `=` |
| Signature lines | `:` or `:>` |
| `match`, `receive`, and `rescue` arms | `=>` |

The column is one space past the widest left-hand side in the run; every other
line is padded to it. A run of one aligns to a single space, which is why a
lone declaration never grows trailing padding.

```kex
make Circle, implement: Shape do
  let area      = Math.PI * @radius * @radius
  let perimeter = 2.0 * Math.PI * @radius
end

match parts.last do
  Just("") => parts.take(parts.count - 1)
  _        => parts
end
```

**Bail-out:** if aligning a run would push any of its lines past the hard limit
(§3), the whole run falls back to a single space on every line. The decision is
per run and all-or-nothing, so the result stays idempotent.

Nothing inside a comment or a string literal is ever aligned — see §22.4.

**[F]** Collection and call literals fit on one line, or break one element per
line with the closing bracket at the opening line's indent. No trailing comma
on the closing line; no dangling opening bracket alone on a line.

**[L]** **Write the sugar, not the constructor.** Three built-in types have a
sugared spelling, and in type position the sugar is the required one:

| Long form | Write | Rule |
|-----------|-------|------|
| `Optional<X>` | `X?` | `prefer-optional-shorthand` |
| `Result<A, E>` | `A or! E` | `prefer-result-shorthand` |
| `Either<L, R>` | `L or R` | `prefer-either-shorthand` |

```kex
parse : String -> URI or! URIError            # yes
parse : String -> Result<URI, URIError>       # no

let classify(n: Integer) -> String or Integer do    # yes
type Id = Integer or String                         # yes
```

Together these make the shape of a result read as part of the signature rather
than as a container wrapped around it. Routine formatting never rewrites a
type, so these stay diagnostics; each carries an autofix that runs only when
asked for (§23).

`or` and `or!` bind tighter than `->`, so `String -> Int or! ParseError` is
`String -> (Int or! ParseError)` — a function returning a result, which is
almost always what is meant. Parenthesize for the other reading.

Do not confuse `A or B` with the union type `A | B`: `or` builds an `Either`,
whose sides are tagged `Left` and `Right` and which you take apart by pattern
matching; `|` is an untagged union of two types. Reach for `or` when the two
sides are legitimately two shapes of one value, and for `or!` when one side
means failure — `Either` is not an error type.

Write the long name only where the sugar cannot reach: a type argument, a
trait bound (`Resultable`, `Eitherable`), the type's own declaration, or prose.

**[A]** A single-line string is double-quoted; `'c'` is a `Char`, not a
one-character string. Prefer `"${a}, ${b}"` interpolation over chained `+`.

**[A]** **Integer and Float are one numeric tower.** `0 == 0.0` is true,
ordering compares across them, and a literal pattern matches either side:
`match x do 0 => ... end` catches `0.0`. Do not convert to compare, and do not
write two clauses where one literal pattern covers both.

**[L]** Equality on floats is still equality on floats: `total == 0.1 + 0.2` is
false for the usual reason, tower or no tower. Compare a computed float against
a tolerance, and keep `==` for values you can name exactly — a literal, a
constant, a parse result you control (`float-equality`).

**[A]** An atom is a name with no payload and no declared set: `:macos`,
`:infinity`, a map key, a slot label. The moment the set is closed and you want
the checker to know it, declare a variant type — `type Level = Debug | Info |
Warn` beats `:debug | :info | :warn` because a misspelling then fails to
compile rather than silently never matching.

**[L]** **Multiline text is a backtick string.** A double-quoted literal that
spans lines, or that builds them with `\n` escapes, becomes a backtick literal
instead (`multiline-string`):

```kex
let activeUsers -> String do          # yes
  return `
    SELECT id, email
    FROM users
    WHERE active = true
  `
end

let activeUsers -> String do          # no
  return "SELECT id, email\n" +
         "FROM users\n" +
         "WHERE active = true"
end
```

**[F]** **Indent the body.** The lines of a block-shaped backtick literal are
indented one level past the line that opens it, and the closing backtick sits
at that opening line's own indentation. Nothing is lost by indenting: the
margin every nonblank line shares is stripped from the value, so the string is
the same wherever the literal sits in the file, and reindenting the code around
it produces no change in what it holds.

```kex
module Report do
  let header = `
    id,email
    ----
  `
end
```

Flush-left bodies are legal and produce the same value, but they fight the
surrounding indentation and make the literal's extent hard to see. The one case
for column 0 is text whose leading whitespace must not be shared away — a line
that has to start at the margin while its neighbours are indented.

Backtick strings are raw: backslashes stay literal and `${…}` is ordinary text,
which is what makes them the right home for regexes and paths (`` `\d{3}-\d{4}` ``,
`` `C:\Users\akos` ``) whatever their length. Double a backtick to include one.
When the text genuinely needs holes, opt in with `$` — `` $`Hello ${name}` `` —
rather than falling back to a quoted string with escapes.

---

## 11. Control flow

**[F]** A conditional whose branches are single expressions is written
`<cond> then <a> else <b>` — no `if`, no `end`:

```kex
let peek = @pos < @input.count then @input.at(@pos) else None
```

Spell out `if` / `elif` / `else` / `end` only when a branch occupies its own
line. `elif`, not `else if`.

**[L]** **A conditional on one line does not spell `if` or `end`.** Packing
`if c then a else b end` into a single line is the diagnostic
(`one-line-if-end`) — the `then`/`else` expression says the same thing without
the bookends:

```kex
let sign = n < 0 then "neg" else "pos"        # yes
let sign = if n < 0 then "neg" else "pos" end # no

if n < 0                                      # yes — branches on their own lines
  IO.printError("negative")
else
  IO.printLine("fine")
end
```

The two forms are not interchangeable styles of the same thing: `if ... end` is
what you reach for when a branch needs its own lines, and the bare
`then`/`else` when the whole conditional is one expression. A single line that
still carries `if` and `end` is the one combination that buys nothing.

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

**[L]** **Do not `match` on a `Bool`.** A `match` whose arms are exactly the two
boolean literals — or `true` plus a wildcard standing in for `false` — is a
conditional wearing a costume. Write the conditional (`bool-match`):

```kex
let padLeft(s: String, width: Integer) -> String do   # yes
  let padding = width - s.count
  return s if padding <= 0
  return repeat(" ", padding) + s
end

let padLeft(s: String, width: Integer) -> String do   # no
  let padding = width - s.count
  match padding > 0 do
    true  => repeat(" ", padding) + s
    false => s
  end
end
```

Pick by shape: a one-line `then`/`else` when both results are short
expressions, an early `return … if` guard when one side is a bail-out, and
`if` / `else` / `end` when either branch needs its own lines. The rule fires on
the scrutinee's arms, so it also catches a `match` on a predicate call
(`match name.empty? do`).

**[L]** **An `elif` chain testing one value against literals is a `match`.**
When every branch asks `x == <literal>` about the same `x`, the chain is a
dispatch table wearing an `if` (`equality-elif-chain`):

```kex
match declaration do                          # yes
  "version"     => version = argument(arguments).or(version)
  "description" => description = argument(arguments).or(description)
  "license"     => license = argument(arguments).or(license)
  _             => failures.push!("unknown declaration `${declaration}`")
end

if declaration == "version"                   # no
  version = argument(arguments).or(version)
elif declaration == "description"
  description = argument(arguments).or(description)
elif declaration == "license"
  license = argument(arguments).or(license)
else
  failures.push!("unknown declaration `${declaration}`")
end
```

The `match` is shorter, but that is not the point: it lines the cases up in one
column so a missing or duplicated case is visible, and it puts the fallback
where a reader looks for it. A chain of twelve `elif`s hides both.

**[L]** **One interesting arm means `if let`.** A `match` that acts on a single
pattern and discards the rest — the other arm being `_`, `None`, `false`, or an
empty body — is an `if let` (`prefer-if-let`):

```kex
if let Just(largest) = shapes.max { |s| s.area }    # yes
  IO.printLine("largest area: ${largest.area}")
end

match shapes.max { |s| s.area } do                  # no — the second arm is filler
  Just(largest) => IO.printLine("largest area: ${largest.area}")
  _             => Void
end
```

The binding is in scope for the body and nowhere else, which is the whole point:
the shape that produced the value is visible on the line that uses it. It works
on any pattern, not just `Just` — `if let Ok(temp) = Float.parse(s)` is the same
move on a result.

Keep the `match` when both arms do work. Two arms that each produce a value are
a real case analysis, and flattening one of them into an `if let` with an
afterthought below it reads worse than the `match` did.

**[L]** Multi-line boolean conditions require parentheses:

```kex
if (a
    && b)
```

**[A]** Prefer early `return ... if` guards to a nested `if`/`else` pyramid.
Two levels of nesting inside a function body is the practical ceiling.

---

## 12. Blocks, lambdas, and chains

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
list.filter!(&.even?)               # not { |x| x.even? }
cursor.takeWhile(~alpha?)           # not { |c| alpha?(c) }
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

## 13. Purity and mutability

**[A]** `foul` marks a function with side effects, and only ever a function —
there is no foul block and no foul module. A pure function cannot call a foul
one, but that is enforced by the type checker as a compile error; the lint has
no rule for it and needs none.

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

**[L]** **A run of `var`s at the top of a function is a record that has not
been declared.** Ten `var`s initialised to defaults, mutated down the body, and
assembled into a record at the end is a record built the long way — the
compiler cannot check that every field was considered, and the reader cannot
see the shape until the last line (`var-accumulator`):

```kex
var name = ""                      # no — a ManifestPackage, spelled out
var version = "0.1.0"
var license = ""
var dependencies: [Dependency] = []
# ...forty lines of mutation...
Ok(ManifestPackage { name: name, version: version, license: license, ... })
```

Build the value instead. Declare the record with its defaults, then fold the
input over it, letting each step answer a new record:

```kex
let empty = ManifestPackage { }                     # defaults from the record
declarations.reduce(empty) { |package, entry| package.apply(entry) }
```

Now the defaults live on the record where they are documented once, `apply` is
a function you can test on one entry, and adding a field does not mean finding
every `var` that has to learn about it.

Two or three `var`s that genuinely accumulate — a counter, a buffer, a
running best — are fine, and §20's `var-count` is where the line sits.

---

## 14. Processes and `serving`

**[A]** State that outlives a call belongs to a process, and a process is
declared by giving a record a `serving` block. `Process.spawn(state)` hands that
record its own process and returns a `Server<State>`; every `slot` becomes a
checked method on the handle.

```kex
record ShoppingList do
  items : [String] = []
end

serving ShoppingList do
  slot items -> Reply<[String]> = { reply: @items }

  slot add(item: String) -> Reply<Integer> do
    new.items = [item | @items]
    return { new, reply: new.items.count }
  end

  slot clear -> Void = New { items: [] }
end
```

**[L]** A slot's return type is its protocol: `Reply<T>` is a synchronous call,
`Void` an asynchronous cast. Pick by whether the caller needs an answer, not by
which is convenient — a `Reply` nobody reads makes every caller wait for
nothing, and a `Void` that should have been a `Reply` loses the failure
(`slot-protocol`).

**[A]** Slot results are the map forms, and they mean distinct things:
`{ reply: v }` answers without changing state, `{ new, reply: v }` installs the
updated record and answers, `{ new }` installs without answering, and adding
`stop: reason` terminates after the transition.

**[L]** Do not annotate a single-clause slot separately — the inline
declaration carries the type. A multi-clause slot may take a standalone
annotation, and then it uses `::>` for a call (the handler has `from`) or `:>`
for a cast (`slot-annotation`):

```kex
apply ::> Command -> Reply<Integer>
slot apply(Increment(by)) = { reply: @count + by }
slot apply(Current)       = { reply: @count }
```

**[A]** Everything that sends is `foul` — calls because they send and wait,
casts because they send. A pure function may compute what to send; it may not
send it.

**[A]** Prefer the typed `Server<X>` surface to hand-rolled `send`/`receive`.
Raw message passing is right for a protocol that is not a request/response over
one record — a supervisor, a fan-out, a bridge to Erlang — and wrong as a way
to avoid declaring the protocol you actually have.

**[A]** A call can fail: the server may be gone, or the call may time out. That
is why a slot call answers a result — `groceries.add("coffee")` is
`Integer or! CallError` — and `.try` propagates it like any other failure
(§15). Give a bounded wait an explicit `Process.within(timeout)` rather
than relying on the default.

**[A]** Long-lived mutable state lives in the process and nowhere else. A `var`
carried across `receive` cycles is that state and makes its owner `foul`
(§13); a `var` inside one slot body is an ordinary local.

---

## 15. Errors

**[L]** No exceptions. A function that can fail returns `X or! E` when the
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

**[L]** **`or` takes a plain value, not another optional.** `.or(None)` and
`.or(Just(x))` are confusions of the two levels: `or` is what *leaves* the
optional world, so a default that is itself optional means the chain never
leaves it (`or-optional-default`):

```kex
config.get("port").or(8080)          # yes — an Integer comes out
config.get("port").or(None)          # no — a no-op with a default-shaped hole
config.get("port").or(Just(8080))    # no — still wrapped
```

`.or(None)` is the common one, and it usually means one of two other things
was wanted: nothing at all — the value was already `X?`, so the call adds only
noise — or a fallback to a second optional that stays optional, which today is
spelled `a.set? then a else b` (there is no `orElse` in the stdlib). The same
holds for a result: the argument to `or` is the plain fallback, never
`Error(e)`.

**[L]** `die` is for a broken invariant, never for an expected failure, and
never in library code that a caller could reasonably recover from
(`die-in-library`).

**[A]** Error types are records with a `kind` drawn from a closed variant type,
plus a human `message`, plus optional position/context — see `URIError` and
`NetError`. Do not return a bare `String` as an error.

**[A]** **Message wording.** A `die` string or an error record's `message` is
lowercase, has no trailing period, and names the offending thing — the corpus
shape is `"repeat count cannot be negative"`, `"divide by zero"`,
`"unsupported platform"`. It reads as a fragment because it is one: something
will print it after a prefix.

Say what is wrong, not what the caller should have done, and include the value
when it is short enough to be useful:

```kex
die("repeat count cannot be negative")               # yes
die("cannot serialise a ${Type.of(value)}")          # yes — names the thing
die("Invalid input.")                                # no — capitalized, vague
die("You must pass a positive count!")               # no — scolds, no value
```

---

## 16. Comments and doc comments

**[F]** `#` followed by one space. Comments are indented to the code they
describe. No comment banners of repeated punctuation except a section divider
in an example file.

**[L]** A public declaration carries a doc comment, in the block of `#` lines
directly above it (`missing-doc`). The format is RDoc-like:

```kex
# Returns the first element wrapped in +Just+, or +None+ if the list is empty.
#
# @return [X?] the first element, or +None+
#
# @example
#   [1, 2, 3].first   # => Just(1)
#   [].first          # => None
first :> X?

# Returns the first element satisfying +pred+, or +None+ if none does.
#
# @param pred [X -> Bool] the test each element is put to
# @return [X?] the first match, or +None+
#
# @example
#   [1, 2, 3].find(~even?)   # => Just(2)
find :> (X -> Bool) -> X?
```

- First paragraph is a one-sentence summary in the third person present
  (“Returns…”, “Parses…”), ending in a period.
- `+code+` marks inline code; it renders as backticks.
- Paragraphs are separated by a bare `#`.
- Tags, in order: `@param` (one per parameter, in order), `@return`,
  `@example`, `@deprecated`. `@example` may carry a title on its own line.
- Example lines are indented three spaces after the `#` and show results as
  `# => value`. Align them by hand if you like — the formatter does not touch
  the inside of a comment (§22.4).

**[L]** Tag/parameter agreement: an `@param` must name a real parameter and
every parameter should have one (`doc-param-mismatch`).

**[A]** A doc comment explains what the caller gets and when to reach for this
over the alternative. It does not restate the signature.

---

## 17. Compile-time blocks

**[A]** `compiled do ... end` runs at compile time and reifies the result into
the program: constants are inlined at their use sites, `let %name` / `type
%name` / `make %name` generate declarations, and a fully-determined builder
chain collapses to the value it would have produced.

**[L]** A `compiled` block is for work whose inputs are known when the compiler
runs — a table, a schema, a generated set of accessors, a validated literal. It
is not a performance annotation to sprinkle on ordinary code
(`gratuitous-compiled`). If the block reads a runtime value, it cannot collapse
and the block bought nothing.

**[A]** `ENV.get` is the one effect permitted at compile time, and it makes the
build depend on the environment that ran it. Reach for it when the value is
genuinely a build input — a version, a target — and never for configuration a
deployment should be able to change.

**[A]** Check what actually collapsed with `--collapse-report` rather than
assuming. A chain that quietly stayed at runtime is the common failure, and the
report names the reason. Generated declarations are ordinary declarations
afterwards: they take signatures and doc comments like anything else.

**[A]** Keep generated names predictable. A reader who greps for a function
must be able to find where it comes from, so a generator that invents names
from data should be documented at the block, with an example of what it emits.

---

## 18. Tests

**[L]** Specs use `describe` / `it` with `assert`; `before` / `after` are
available. One `describe` per file, named for the unit under test.

**[A]** `it` names finish the sentence “it …”: `it("returns None for an empty
list")`. Assert on values, not on formatting of values.

**[A]** Keep an `it` to one behavior. A spec that asserts a dozen unrelated
facts is a spec that reports one failure for a dozen bugs.

---

## 19. Correctness

The rules above are about how code reads. These are about code that is wrong —
they describe programs that compile and typecheck but do not mean what they
appear to mean. A finding here is a probable bug, not a preference, which is
why several default to `error`.

None of them duplicate the type checker. Where the compiler already refuses a
program — a pure function calling a foul one, `!` on a `let`, a missing trait
method — there is no lint rule and none is needed.

### Dead code

**[L]** A binding that is never read is either a mistake or a leftover
(`unused-binding`). The same for a parameter (`unused-parameter`), which is
spelled `_` or `_name` when it exists only to satisfy a shape:

```kex
let or(@Just(x), _) = x            # deliberate: the default is unread here
```

**[L]** An import whose names are never used in its scope is noise, and it
misleads the next reader about what this code depends on (`unused-using`).

**[L]** A `private` helper that nothing in its module calls is dead
(`unused-private`). Public functions are exempt — their callers are elsewhere.

**[L]** Statements after an unconditional `return` never run
(`unreachable-code`). This includes a clause after one whose head matches
everything, and an arm after a bare `_` (§11's `wildcard-not-last` covers the
ordering; this one covers the arm that can never be reached at all).

**[L]** A `do ... end` with an empty body, or an `else` with nothing in it, is
either unfinished or a leftover (`empty-body`). Say `Void` if a branch
deliberately does nothing.

### Duplication

**[L]** Two `match` arms with the same pattern: the second can never run
(`duplicate-match-arm`). Two function clauses with identical heads: same
(`duplicate-clause`).

**[L]** A repeated key in a map literal, or a repeated field in a record
construction, silently keeps one of them (`duplicate-key`).

**[L]** Both branches of a conditional with the same body means the condition
does not matter (`identical-branches`) — either the condition is wrong or one
branch is.

### Suspicious expressions

**[L]** A comparison whose operands are the same expression — `x == x`,
`n < n` — is always true or always false (`self-comparison`). The intended
operand is usually a different variable.

**[L]** A condition that cannot vary — `if true`, a guard on a literal, a
`while` on a constant — is a branch that always goes one way
(`constant-condition`). Debug scaffolding left behind is the usual cause.

**[L]** A `var` assigned a value that is overwritten before any read did
nothing (`useless-assignment`).

**[L]** A bare expression in the middle of a body — computed, not bound, not
returned — is a value thrown away (`discarded-value`). §6 permits exactly one
implicit exit, the last expression; anything earlier is dead.

**[L]** A bare `()` written to make two branches agree, or to end a branch that
already ended, is filler (`redundant-unit`). If the branches of a conditional
disagree about type because one produces a value and one does not, that is the
bug — `()` only hides it.

**[L]** A call whose result is an `X?` or an `X or! E`, used as a statement
with the result dropped, discards a failure the signature went out of its way
to report (`ignored-failure`). Bind it, `.try` it, or say `.or(default)` — but
do not let it fall on the floor. **Default: error.**

```kex
FS.File.write(path, rendered)                  # no — the failure vanishes
FS.File.write(path, rendered).try              # yes — propagate it
if !FS.File.write(path, rendered).ok?          # yes — handle it
  IO.printError("could not write ${path}")
end
```

### Effects and processes

**[L]** A `receive` with no `after timeout:` clause waits forever if the
message never comes (`receive-without-timeout`). A server loop that is meant to
block indefinitely says so with `after timeout: :infinity`, which reads as a
decision rather than an omission.

**[L]** A spawned process whose handle is discarded cannot be called, awaited,
or stopped (`orphan-spawn`). If the process is genuinely fire-and-forget, bind
it to `_` so the next reader knows that was deliberate.

**[L]** A `foul` call in a loop whose result is ignored every iteration is
usually a missing accumulation (`ignored-loop-effect`).

### Shadowing

**[L]** An inner binding that reuses the name of one already in scope in the
same function hides it (`shadowed-binding`). Rebinding across a lambda boundary
is the common accident: `list.map { |item| ... }` inside a function that
already has an `item`.

**[L]** Inside a `make`, a local with the same name as a field of the receiver
makes `@name` and `name` two different values on adjacent lines
(`shadowed-field`). **Default: error.**

---

## 20. Size and shape

Every rule here is a threshold with a default, and every threshold is arguable.
They exist because the failure they describe is not a bad line — it is a
function that grew one branch at a time until nobody could hold it in their
head, and no single edit along the way looked wrong.

**[L]** A function body over **40 lines** is doing more than one thing
(`function-length`). Count the body, not the doc comment. The fix is almost
never "split it in half" — it is to notice that one branch of it is a
transformation with a name.

**[L]** More than **three levels** of nesting inside a body
(`nesting-depth`). A `match` arm holding an `if` chain holding a `push!` is
already three; the fourth is where the code stops being readable top to bottom.
Early `return`s (§11) and extracted helpers are the two ways out.

**[L]** More than **three `var` bindings** in one body (`var-count`). See
§13: a run of `var`s is usually a record that has not been declared.

**[L]** A `match` arm whose body runs past **five lines** should be a call to a
named function (`match-arm-body`). An arm that opens `=> do` and continues for
fifty lines is a function with the pattern as its name, written in the wrong
place — and it hides the shape of the `match`, which is the one thing a reader
came to the `match` to see.

**[L]** More than **five parameters** (`parameter-count`). Past that, the
caller cannot tell the arguments apart at the call site; group them into a
record whose field names do the telling.

**[A]** Thresholds are defaults, not truths. A parser's dispatch table or a
generated table may legitimately exceed one; suppress it there with a reason
(§21) rather than raising the limit for the whole project. What is not
legitimate is suppressing it everywhere because one function is over.

---

## 21. Lint rule index

| ID | § | Default |
|----|---|---------|
| `line-too-long` | 3 | warn |
| `predicate-naming` | 5 | warn |
| `signature-adjacent` | 6 | error |
| `missing-public-signature` | 6 | warn |
| `split-clauses` | 6 | error |
| `clause-signature` | 6 | warn |
| `param-match` | 6 | warn |
| `empty-parens` | 6 | warn |
| `overbroad-using` | 7 | warn |
| `using-only-list` | 7 | warn |
| `redundant-using-block` | 7 | warn |
| `private-placement` | 7 | warn |
| `self-qualified-call` | 7 | warn |
| `prefer-at-field` | 8 | warn |
| `prefer-new-copy` | 8 | warn |
| `prefer-trait-param` | 9 | warn |
| `single-variant-pipe` | 10 | error |
| `prefer-optional-shorthand` | 10 | warn |
| `prefer-result-shorthand` | 10 | warn |
| `prefer-either-shorthand` | 10 | warn |
| `multiline-string` | 10 | warn |
| `float-equality` | 10 | warn |
| `one-line-if-end` | 11 | warn |
| `prefer-guard-modifier` | 11 | warn |
| `wildcard-not-last` | 11 | error |
| `lazy-wildcard` | 11 | warn |
| `bool-match` | 11 | warn |
| `equality-elif-chain` | 11 | warn |
| `prefer-if-let` | 11 | warn |
| `prefer-receiver-shorthand` | 12 | warn |
| `prefer-capture` | 12 | warn |
| `gratuitous-foul` | 13 | warn |
| `escaping-var` | 13 | error |
| `var-accumulator` | 13 | warn |
| `slot-protocol` | 14 | warn |
| `slot-annotation` | 14 | warn |
| `sentinel-failure` | 15 | warn |
| `prefer-try` | 15 | warn |
| `early-unwrap` | 15 | warn |
| `or-optional-default` | 15 | warn |
| `die-in-library` | 15 | error |
| `missing-doc` | 16 | warn (error in stdlib) |
| `doc-param-mismatch` | 16 | warn |
| `gratuitous-compiled` | 17 | warn |
| `unused-binding` | 19 | warn |
| `unused-parameter` | 19 | warn |
| `unused-using` | 19 | warn |
| `unused-private` | 19 | warn |
| `unreachable-code` | 19 | error |
| `empty-body` | 19 | warn |
| `duplicate-match-arm` | 19 | error |
| `duplicate-clause` | 19 | error |
| `duplicate-key` | 19 | error |
| `identical-branches` | 19 | warn |
| `self-comparison` | 19 | error |
| `constant-condition` | 19 | warn |
| `useless-assignment` | 19 | warn |
| `discarded-value` | 19 | warn |
| `redundant-unit` | 19 | warn |
| `ignored-failure` | 19 | error |
| `receive-without-timeout` | 19 | warn |
| `orphan-spawn` | 19 | warn |
| `ignored-loop-effect` | 19 | warn |
| `shadowed-binding` | 19 | warn |
| `shadowed-field` | 19 | error |
| `function-length` | 20 | warn |
| `nesting-depth` | 20 | warn |
| `var-count` | 20 | warn |
| `match-arm-body` | 20 | warn |
| `parameter-count` | 20 | warn |

A rule is suppressed for the next declaration with `# lint:allow <id> — reason`.
The reason is required; a bare suppression is itself a diagnostic.

---

## 22. Formatter guarantees

The formatter is expected to hold to these, and its test suite should pin them:

1. **Idempotent.** `fmt(fmt(x)) == fmt(x)`.
2. **Semantics-preserving.** The AST after formatting is equal to the AST
   before, modulo position.
3. **Comment-preserving.** Every comment survives, attached to the same
   declaration or expression.
4. **Never reflows prose.** Text inside comments and string literals is not
   rewrapped or re-spaced. The one exception is the block-shaped backtick
   literal of §10, whose body it re-indents — legal precisely because the shared
   margin is stripped from the value, so the string it holds does not change.
5. **Aligns deterministically** as described in §10 — the aligned column is a
   function of the run alone, so it never depends on the author's spacing.
6. **Never changes a `do ... end` body to `= expr`** when a comment lives
   inside it, and never the reverse when the result would exceed 80 columns.
7. **Fails loudly** on input it cannot parse, and writes nothing.

---

## 23. Adoption

The rules above describe the target state, not the current corpus. Four rules
have a backlog in it:

| Rule | Non-conforming sites in `src/stdlib` + `examples` |
|------|--------------------------------------------------|
| `prefer-result-shorthand` | 281 (against 19 already written `or!`) |
| `bool-match` | 10 — clustered in `units.kex`, `units/data.kex`, `units/si.kex` |
| `prefer-optional-shorthand` | 6 |
| `prefer-either-shorthand` | 4 |

**Sequencing.** `kex fmt` lands first. The corpus migration follows it, as one
mechanical pass rather than as drive-by edits — a formatted tree is what makes
the sugar rewrite a reviewable diff instead of a source of merge conflicts.
Until that pass has run, the four rules above ship disabled by default for
`src/stdlib`, so the stdlib's own build stays quiet.

Type rewriting is a migration, not a formatting concern: it belongs to
`kex lint --fix`, invoked deliberately, and never to a routine `kex fmt` that
someone's editor runs on save.

**`New` and `new` are different things, by design.** §8's rule is about `New
{ ... }`, the constructor. Lowercase `new` is not a second spelling of it: it is
an ordinary `var` bound to the clone, which you assign fields on
(`new.age = @age + 1`) and return. `new { ... }` is an error, and deliberately
so — keeping `new` an ordinary binding is what makes a `make` block pure sugar
for `var new = This { ...this }`, granting no power a hand-written function
lacks. See `docs/new-operator-plan.md` (Decisions, and "Casing: `new` and `New`
are different things") and `examples/new.kex`.
