# Known gaps

Defects and unfinished edges that are understood but not fixed, each with a
reproduction. Anything listed here is reachable from ordinary code — this is
not a wishlist.

Verify the state of a section before acting on it; `make test-all` and
`make spec-beam` are the two commands that show it.

## `Block<[A]>` collection blocks are not implemented

`docs/dsl.md` documents a `Block<[A]>` parameter as collecting each expression
in the `do…end` body into a list. Neither backend does that — the block arrives
as an opaque callable instead:

```kex
record El do
  tag : String
end
let p(text: String) -> El = El { tag: text }
let div(children: Block<[El]>) -> [El] = children

main do
  let out = div do
    p("first")
    p("second")
  end
  IO.printLine(out.length())
  # walker: Undefined method: length for Function
  # BEAM:   Undefined method: length for Any
end
```

The `...expr` spread that `docs/dsl.md` describes as part of the same feature
is accepted in a `do` body by the parser, but nothing consumes it: on BEAM it
reaches lowering as `IR lower: unimplemented expr node SpreadExpr`. Spreading
into a list or a map literal is unaffected and works on both backends.

Every use of `Block<[A]>` in the tree is under `examples/aspirational/`, which
is not run by any suite — so nothing catches this today.

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

## A type parameter inside a function-typed parameter does not reach the result

```kex
record Cursor do
  n : Integer = 0
end

make Cursor do
  let onlyGeneric(f: Cursor -> Result<(T, Cursor), String>) -> Result<(T, Cursor), String> = f(this)
  let mixed(tag: String, f: Cursor -> Result<(T, Cursor), String>) -> Result<(T, Cursor), String> = f(this)
end

let mk(w: Cursor) -> Result<(Integer, Cursor), String> = Ok((1, w))

main do
  let w = Cursor { n: 0 }
  match w.mixed("x", mk) do Ok((v, _)) -> IO.printLine("${v}") Error(_) -> () end
end
# error: `mixed` expects argument 3 to be Cursor -> Result<(Unknown, Cursor), String>,
#        but got Result<(Integer, Cursor), String>
#   mixed : A -> String -> (B -> Result<(C, B), String>) -> Result<(D, E), String>
```

The same `T` is instantiated as three unrelated variables — `C` in the
parameter, `D` in the result — so the type the caller supplies can never reach
the return, and every call site reports the result as `Unknown` and then
rejects the argument it was just handed.

**Located, not fixed.** `TypeChecker::preRegisterFunctionDef`
(`src/semantic/typechecker.cxx`, the two `Signature{...}` constructions) builds
the provisional signature's result as a bare `freshTypeVar()`, discarding
`clause.returnAnnotation` and the `genericVars` map the parameters were
resolved with. Resolving the annotation with that same map fixes the signature.

It cannot land on its own. Giving those functions a concrete result type also
gives their CALLERS concrete receiver types, and that exposes a second defect
underneath: `src/stdlib/json.kex:47` then resolves `rest.atEnd?` to
`FileHandle.atEnd?` even though `rest` is a `Parsing.Input`, because method
overload selection does not filter by receiver type first. Today an
unconstrained receiver keeps that check permissive and hides it — the same
shape as the `Match.get` gap above. Receiver-based overload selection is the
prerequisite; the return-type fix drops in after it.

This is what blocks four combinators (`zeroOrOne`, `separatedBy`, `between`,
`label`) from joining `Parsing`: they compile into the prelude and fail only at
call sites. `string` and `takeWhile` shipped because they are not generic.

## `this` is unbound in functions compiled after a fall-through `rescue`

Only in the prelude build (`kex --build-prelude`, how `src/stdlib/*.kex`
becomes `kex_prelude.beam`). A `trying` block whose `rescue` arm ASSIGNS rather
than returning corrupts `this` for functions compiled after it in the same
`make` block:

```kex
# added to src/stdlib/parsing.kex — `choice` below it was not touched
let zeroOrOne(f: Input -> Result<(T, Input), ParseError>) -> (T?, Input) do
  var found: T? = None
  var cursor = this
  trying do
    let (value, advanced) = f(cursor).try
    found = Just(value)
    cursor = advanced
  rescue found = None
  end
  return (found, cursor)
end
# kex_prelude: unbound variable 'This' in choice/1
```

Returning from both paths is clean, and is the workaround:

```kex
trying do
  let (value, advanced) = f(this).try
  return (Just(value), advanced)
rescue return (None, this)
end
```

The arities in the error (`choice/1`, `between/3`) count only the explicit
parameters, so the receiver is missing from the emitted signature entirely.
Ordinary compilation of the same shapes is fine on both backends — it is
specific to the prelude path. Binding `let self = this` first does NOT work
around it.

**Note for anyone reproducing this:** an incremental `cmake --build build` can
skip the prelude step silently when `CMakeFiles/VerifyGlobs.cmake` errors,
which makes the failure look fixed. Touch `src/stdlib/parsing.kex` and build
`kex_prelude_beam` directly before believing a green result.

## BEAM parity: 6 specs differ

`make spec-beam` reports 158 matching, 6 differing. It is informational and
never fails the build — the BEAM backend is secondary by design.

| Spec | Symptom |
|---|---|
| `char_type.kex` | `Undefined method: + for Char` at runtime |
| `my_capitalize.kex` | `Undefined method: + for Char` at runtime |
| `module_scoped_make_import_collision.kex` | `if_clause` at runtime |
| `to_string_universal_with_local.kex` | `function_clause` at runtime |
| `json_parser.spec.kex` | `Undefined constructor or type: JsonNull` at compile time |
| `regex_basics.kex` | `Ambiguous overload for replace` at compile time |

The first four are runtime crashes rather than wrong answers. Two are `Char`
arithmetic, which the walker allows and the BEAM lowering has no arm for; the
other two look like the make-block/receiver dispatch family. The last two fail
the BEAM-side ANALYSIS rather than running at all, so they are a different
problem from the rest — the names resolve for the walker but not for the
compile path.

`standalone_module.kex` and `static_namespacing.kex` (now
`record_module_constructors.kex`, after `static do` was removed) were both in
this list previously and now match.

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
