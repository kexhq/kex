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
colon (§8). This asymmetry is deliberate and the formatter enforces both.

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

**[L]** **A multi-exit function spells out every `return`.** If a `do ... end`
body leaves through more than one point, every one of them — the guards and the
final value alike — is written with an explicit `return`. Mixing an explicit
guard with an implicit trailing expression is the diagnostic
(`implicit-multi-return`):

```kex
let basename(path: String, suffix: String) -> String do    # yes
  let name = path.split(FS.Path.separator).last.or("")
  return "" if name.startsWith?(".")
  return name if suffix.empty?
  return name.dropSuffix(suffix)
end

let basename(path: String, suffix: String) -> String do    # no — implicit last exit
  let name = path.split(FS.Path.separator).last.or("")
  return "" if name.startsWith?(".")
  name.dropSuffix(suffix)
end
```

A **single**-exit body may end with a bare expression, and normally should.
Exits are counted at the statement level of the body: a body that is one
`match`, `if`/`else`, or `then`/`else` expression has one exit no matter how
many arms or branches it has, and needs no `return` at all.

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
is spelled `~name` (§10). This is why the rule is a lint rather than a
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
as is a `match` on a `Bool`, which §9 sends to a conditional rather than to
clauses.

---

## 6. Visibility and modules

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
    let handleHome(req: Request) -> Response do
      return Response.ok("Welcome!")
    end
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

**[L]** **The block form must scope less than the body it sits in.** In a file
that is one module — the file-scope `module Name` form — imports are standalone
`using M` statements below the module declaration. Do not wrap the file's whole
body in `using M do ... end`; a block that ends where its scope would have
ended anyway buys nothing and costs every declaration in the file a level of
indentation (`redundant-using-block`):

```kex
module URI                          # yes
using Parsing
using Text

record URI do
  source : String
end
```

```kex
module URI                          # no — the block spans the entire file
using Parsing do
  record URI do
    source : String
  end
end
```

Reach for the block form only where it genuinely narrows: a run of declarations
inside a larger module, or a DSL scope.

**[L]** Internal helpers go in a single `private do ... end` block, placed
**after** the public API of that module or `make` block (`private-placement`).
Do not scatter multiple `private` blocks.

**[A]** Narrower still is no import at all: a qualified call
(`Math.Constants.pi`) beats any `using` for a module referenced two or three
times. The ladder, narrowest first: a qualified call, then `as:` for a long
path, then `using M, only: [...]` in the tightest scope that needs it, and only
then a bare `using M`.

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

Nothing inside a comment or a string literal is ever aligned — see §16.4.

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
asked for (§17).

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

## 9. Control flow

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

## 11. Purity and mutability

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

---

## 12. Errors

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
  the inside of a comment (§16.4).

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
| `clause-signature` | 5 | warn |
| `param-match` | 5 | warn |
| `empty-parens` | 5 | warn |
| `implicit-multi-return` | 5 | warn |
| `overbroad-using` | 6 | warn |
| `using-only-list` | 6 | warn |
| `redundant-using-block` | 6 | warn |
| `private-placement` | 6 | warn |
| `prefer-at-field` | 7 | warn |
| `prefer-new-copy` | 7 | warn |
| `single-variant-pipe` | 8 | error |
| `prefer-optional-shorthand` | 8 | warn |
| `prefer-result-shorthand` | 8 | warn |
| `prefer-either-shorthand` | 8 | warn |
| `multiline-string` | 8 | warn |
| `one-line-if-end` | 9 | warn |
| `prefer-guard-modifier` | 9 | warn |
| `wildcard-not-last` | 9 | error |
| `lazy-wildcard` | 9 | warn |
| `bool-match` | 9 | warn |
| `prefer-if-let` | 9 | warn |
| `prefer-receiver-shorthand` | 10 | warn |
| `prefer-capture` | 10 | warn |
| `gratuitous-foul` | 11 | warn |
| `escaping-var` | 11 | error |
| `sentinel-failure` | 12 | warn |
| `prefer-try` | 12 | warn |
| `early-unwrap` | 12 | warn |
| `or-optional-default` | 12 | warn |
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
   rewrapped or re-spaced. The one exception is the block-shaped backtick
   literal of §8, whose body it re-indents — legal precisely because the shared
   margin is stripped from the value, so the string it holds does not change.
5. **Aligns deterministically** as described in §8 — the aligned column is a
   function of the run alone, so it never depends on the author's spacing.
6. **Never changes a `do ... end` body to `= expr`** when a comment lives
   inside it, and never the reverse when the result would exceed 80 columns.
7. **Fails loudly** on input it cannot parse, and writes nothing.

---

## 17. Adoption

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

**`New` and `new` are different things, by design.** §7's rule is about `New
{ ... }`, the constructor. Lowercase `new` is not a second spelling of it: it is
an ordinary `var` bound to the clone, which you assign fields on
(`new.age = @age + 1`) and return. `new { ... }` is an error, and deliberately
so — keeping `new` an ordinary binding is what makes a `make` block pure sugar
for `var new = This { ...this }`, granting no power a hand-written function
lacks. See `docs/new-operator-plan.md` (Decisions, and "Casing: `new` and `New`
are different things") and `examples/new.kex`.
