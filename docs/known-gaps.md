# Known gaps

Defects and unfinished edges that are understood but not fixed, each with a
reproduction. Anything listed here is reachable from ordinary code — this is
not a wishlist.

Verify the state of a section before acting on it; `make test-all` and
`make spec-beam` are the two commands that show it.

## Interpreter crashes on the no-argument `String.split`

```kex
main do
  IO.printLine("hi".split.join("|"))   # Internal error: <garbled bytes>
end
```

The error message itself is corrupted, which points at a memory bug rather
than a plain failure. BEAM handles the same program correctly (`h|i`), so this
is interpreter-only, in the no-argument `split` path (`split :> [String]` in
`src/prelude/string.kex`). Pre-existing; unrelated to any recent regex work.

## A record field named like a prelude method breaks on BEAM

```kex
record Holder do
  items : Map<Any, String>       # `entries` fails the same way
end
```

`h.items` compiles to `kex_prelude:items/1` — that is `Range.items` — and dies
at runtime with `if_clause`; `entries` hits `Map.entries` and gives `badmap`.
`makeAccessors` (`src/ir/lower.cxx`) deliberately skips emitting a local field
accessor when a prelude receiver function shares the name, so the field is
unreachable. The interpreter is unaffected.

The fix is the same shape as the one already applied to method calls: dispatch
on the record tag rather than skipping, falling back to the prelude only for
other receivers. `spec/union_param_atom.kex` names its field `slots` to avoid
this.

## Interpolating tags are typed as if they cannot fail

`` regex$`^${x}` `` returns a bare `Regex`, on the grounds that the
author-written skeleton is validated at compile time. The tag subsystem
**deliberately does not validate interpolating literals** — it skips any
literal with `interpolating` set, and `tests/validation_test.cxx` enforces that
with a test named "does not invoke companions for interpolating tags". So:

```kex
regex$`(^${word}`    # type says it cannot fail; raises at runtime
```

Skeleton validation was implemented (join the parts with an inert placeholder,
validate that, report at the literal since the cooked-offset map only
reconstructs raw bodies) and reverted: it works, but it changes a cross-cutting
rule for every tag, which is the tagged-literal design's call rather than
regex's. Resolving it means either lifting that rule or giving interpolating
tags a `Result` return.

## `Match.get` can mis-resolve where the receiver type is unknown

```kex
line.matches(logRe).map do |m|
  LogLine { level: m.get(:level, ""), msg: m.get(:msg, "") }
end
# error: `get` expects argument 2 to be Integer, but got Atom
#   get : [A] -> Integer -> A?
```

In a position where the receiver's type is not inferable, `get` can resolve
against another `get` overload. Block parameters cannot be annotated
(`|m: Match|` is a syntax error), so the workaround is a `match` binding, which
`examples/regexes.kex` uses.

**The boundary is not characterized.** Every minimal reproduction of that same
shape — same record, same lambda, same atom key — resolves correctly; only the
fuller example failed. A uniquely named accessor would not have this failure
mode; `get` was kept because it is the map-like interface the type presents and
it makes `m[0]` work.

Related and equally uncharacterized: make-block `let` methods now register
signatures (`src/semantic/typechecker.cxx`), which fixed `.to(String)` dispatch
for three specs, yet a minimal one-record reproduction still resolves to the
prelude. Something in the fuller context enables it.

## BEAM parity: 4 specs differ

`make spec-beam` reports 127 matching, 4 differing. It is informational and
never fails the build — the BEAM backend is secondary by design.

| Spec | Symptom |
|---|---|
| `char_type.kex` | `function_clause` at runtime |
| `my_capitalize.kex` | `function_clause` at runtime |
| `standalone_module.kex` | `if_clause` at runtime |
| `json_parser.spec.kex` | `function_clause` in "parses null" |

All four are runtime crashes rather than wrong answers, and all are likely the
same make-block/receiver dispatch family as the two fixed items above.
`static_namespacing.kex` was in this list until make-block `let` methods began
registering signatures; the remaining four did not benefit, which is the same
uncharacterized boundary noted above.

## No regex on the wasm build

The wasm target has no PCRE2, so every `Regex` call raises "Regex is
unavailable in this build". `spec/regex_basics.kex` and
`spec/regex_compile_error.kex` carry `# kex: skip-wasm` for this reason.

Fixing it means a vendored `third_party/pcre2-wasm` pipeline alongside
`gmp-wasm`: a static build pinned to Emscripten 5.0.7 (see
`third_party/gmp-wasm/README.md` for why that pin exists), with
`-DPCRE2_SUPPORT_JIT=OFF` (wasm has no JIT — the analogue of GMP's
`--disable-assembly`) and `-DPCRE2_SUPPORT_UNICODE=ON`, which is required
rather than optional since it backs the `PCRE2_UTF|PCRE2_UCP` pinning that
keeps `\d`/`\w` agreeing with the BEAM backend. PCRE2 is BSD-licensed, so it
raises none of GMP's static-linking questions.

## Interpreter REPL prints a stray continuation prompt

```
kex> let Error(ParseError(err)) = Integer.parse("12x")
  ...>
  error: `ParseError` record takes 5 value(s), but the pattern destructures 1
```

The diagnostic is correct and appears on both REPLs; the interpreter one also
emits a `...>` continuation prompt first, so some multi-line heuristic still
reads the input as an unfinished definition.

## Type-annotated `let`/`foul` bindings do not parse

Only `var` bindings accept a type annotation; `let` and `foul` bindings reject
the `:`.

```kex
main do
  var x : Int = 5      # ok
  let y : Int = 5      # error: Expected '=' in let binding, got :
end

foul c : Int = 5       # error: Unexpected token at top level: :
```

The grammar's binding rule is `LET LOWER_IDENT EQUALS expr` (no annotation),
and no example uses an annotated `let`. This is a parser asymmetry, not an
intended restriction: the docs write annotated bindings freely (e.g.
`docs/types.md`'s `let email: String? = None`, `docs/streams.md`'s
`foul lines: Feed<String> = ...`, `docs/concurrency.md`'s
`foul counter: Process<CounterMessage> = spawn`). Today the workaround is to
drop the annotation and let inference run, or use `var` when an annotation is
wanted. The fix is to extend the `let`/`foul` binding form with the same
`LOWER_IDENT COLON type_expr EQUALS expr` shape that typed params and `var`
already accept.

## `&.field` shorthand fails at runtime for plain record fields

`&.method` works, but `&.field` on a bare record field raises at runtime:

```kex
record R do
  title : String
end
main do
  let rs = [R { title: "a" }]
  IO.printLine(rs.map { |r| r.title })   # ["a"] — explicit lambda
  IO.printLine(rs.map(&.title))          # Internal error: Undefined function: title
end
```

The field accessor resolves in the type checker (`&.title` types as
`R -> String`), so the program type-checks, but the interpreter has no callable
`title/1` function for `&.` to reference, so it dies with `Undefined function`.
`&.method` (a make-block or prelude method) is unaffected — every passing
example uses that form (`&.adult?`, `&.fizzBuzz`, `&.to(String)`). The docs,
however, present `&.field` as field-access sugar (`docs/functions.md`'s
`arr.map(&.name)  # { |x| x.name }`, the same in `README.md` and
`docs/error-handling.md`). The fix is to register record field accessors as
callable functions at runtime, the same way methods are — this is the same
accessor-registration family as the prelude-name-collision gap above.
