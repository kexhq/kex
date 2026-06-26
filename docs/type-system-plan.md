# Kex Type System Plan

Status (2026-06-26): phases 1, 2 (bignum half only), 3, 4, 4.5, 5
(scoped + 5a extended + lambda inference + make-block GenericType fix +
5b topological ordering for forward references), 6,
and 7 (infrastructure) are implemented. `kex run` gates on `kex --check`
by default; use `--no-check` to skip (also honoured via `# kex: no-check`
file pragma). spec suite is 78/78. All 30 examples checker-clean. See the per-phase notes in Rollout phases for exactly what
shipped vs. was scoped out. Originally written 2026-06-23.

## Problem

This is foundational infrastructure, not a point fix. Kex's other
semantics (UFCS, immutability-by-default, the Elixir-style process model,
records/ADTs, the `Result`/`Option`-based error model) are already settled
— a real, principled type system is the remaining prerequisite before
compiler/codegen work can start, since codegen needs to know concrete
representations and signatures that today don't exist anywhere in the
checker. `even?('c')` silently returning `false` instead of being a type
error is the clearest illustration of the gap, not the reason for doing
this — `even?`, `odd?`, `ok?`, and effectively every stdlib function
currently accept any value at runtime and silently degrade, because there
is no notion of function signatures, no type hierarchy, and the existing
typechecker never looks at calls at all:

```cxx
// src/semantic/typechecker.cxx, inferExpr() on FunctionCall/MethodCall:
return Type::unknown();   // args are inferred (for side effects) and discarded
```

Also, `kex check` (the only place `Analyzer`/`TypeChecker` run) is a separate
CLI mode from `kex run` — type errors never block normal execution today.

## What already exists (don't rebuild)

`src/semantic/`:
- `types.hxx/cxx` — `Type` variant (Primitive, Named, Func, Tuple, List, Map,
  Optional, Union, TypeVar, Unknown), `TypeEnv`, `typesEqual`, `typeToString`.
- `symbol.hxx/cxx` — `SymbolTable`/`Scope`/`Symbol` for scope + purity
  (`isFoul`) tracking. Independent of the type system.
- `analyzer.cxx` — orchestrates: phase 1 scope/purity walk, phase 2 calls
  `TypeChecker::check`.
- `typechecker.cxx` — local bidirectional-ish inference for literals,
  binary/unary ops, let/var/assign, control flow, collections. No function
  signatures, no stdlib knowledge, no type hierarchy, no Char/Number.

This is a reasonable skeleton for scope/diagnostics plumbing but the `Type`
representation and `inferExpr` need real extension to do anything useful.

## Design goals

1. **Haskell-flavored static structure, Ruby-flavored surface.** Every
   function (stdlib or user) has a signature with real types and is checked
   at compile time (`kex check`, and eventually as a hard gate before
   `kex run`). No type annotations required from users in the common case —
   infer everything, same as Haskell/OCaml, not Java.
2. **A real numeric tower**, not flat primitives:
   ```
   Number
   ├── Integer            (arbitrary precision — the default literal type)
   │   ├── Byte             (= UInt8)
   │   ├── Int8, Int16, Int32, Int64   (signed, fixed-width; Int is an alias for Int64)
   │   └── UInt8, UInt16, UInt32, UInt64   (unsigned, fixed-width)
   ├── Float                (umbrella trait — see below; not a concrete type)
   │   ├── Float32
   │   └── Float64           (the default for a plain float literal)
   ├── Rational            (exact fraction — see Rational/Decimal below; future, not initial rollout)
   └── Decimal             (arbitrary-precision base-10 — same caveat)
   ```
   `Integer` is arbitrary precision (Python `int`/Haskell `Integer`
   semantics) and is what a plain integer literal like `42` is typed as by
   default — so there's no separate `BigInteger` type; unbounded-ness is
   just what `Integer` means, not a fallback you promote into. `Int64` is a
   concrete fixed-width type (the interpreter already represents integers
   as a C++ `int64_t` internally, so "native machine width" is just
   `Int64`, not something that varies per build target), and **`Int` is an
   alias for `Int64`** — both names resolve to the identical type, `Int`
   is just the shorter spelling. The sized integer types (`Byte`,
   `Int8`/`Int16`/`Int32`/`Int64`/`Int`, `UInt8`..`UInt64`) are explicit
   opt-ins for when you need fixed width, overflow-trapping, or
   portability/interop (e.g. binary protocols, FFI) — most code never
   names them and just lets literals default to `Integer`.

   Floats mirror `Integer`'s role as an umbrella, **not** its alias
   pattern: `Integer` doubles as both the umbrella trait *and* a concrete
   arbitrary-precision type (a value can actually *be* `Integer`-typed),
   because arbitrary precision is a real, distinct representation.
   There's no sensible "arbitrary precision float" (that's what
   `Rational`/`Decimal` would be, a separate future concern, not part of
   this tower) — so `Float` can only be the umbrella/trait matching
   `Float32` *or* `Float64`, never a concrete type a value is directly
   stored as. Consequently **`Float` is not an alias for `Float64`** (an
   earlier draft of this doc said it was — wrong, since that would make
   `x.is?(Float)` falsely reject `Float32` values, breaking the same
   umbrella semantics `Integer` gets). `Float64` is the concrete IEEE-754
   double type the interpreter already uses and is what a plain float
   literal like `3.14` defaults to *directly* — there is no separate
   short alias name for it the way `Int` shortens `Int64`, since
   `Float64` is already the name in normal use.

   `even?`/`odd?` etc. take `Integer` the trait (any integer type,
   sized or not — `2.0.even?` is a type error, matching Ruby raising
   `NoMethodError` for `Float#even?`). Subtyping is
   structural-by-supertype-set, not nominal inheritance: a type just
   declares which built-in classes it belongs to.
3. **`String = [Char]`.** String becomes sugar/an alias for a list of Char,
   not its own primitive. This affects every place String is special-cased
   today (lexer/parser are unaffected; `types.hxx` and stdlib signatures are
   not).
4. **Traits/typeclasses, not ad-hoc overloading**, for things like `ok?`.
   `ok?` isn't "any value with an Ok variant" — it's defined once for
   `Result<X, E>` and dispatches structurally. Modeling this as a typeclass
   (`Checkable`, `Resultable`, whatever) is what generalizes to user-defined
   records later (e.g. a user `Maybe` type implementing the same trait).
5. **Errors must be Elm/Rust-quality**: point at the exact subexpression,
   state what was expected vs. what was found, and where possible suggest
   the fix. This is a hard requirement, not a nice-to-have — see Error UX
   section.
6. **Build for tools, not just `kex check`.** Lint, formatter, LSP hover/
   completion, and REPL completion all need to query "what is the type of
   this expression / what are this function's overloads / what traits
   does this type implement" through a stable API — not by re-running the
   whole checker ad hoc. See Architecture for Tooling section.

## Type representation changes (`src/semantic/types.hxx`)

Replace the flat `PrimitiveType::Kind` enum with a small built-in class
hierarchy plus structural membership, instead of nominal subclassing.
Keep `PrimitiveType` for the unbounded/scalar cases and add a separate
`SizedIntType` for the fixed-width opt-in integers, rather than exploding
`Kind` into 9 integer variants:

```cxx
struct PrimitiveType {
    enum Kind { Integer, Char, Bool, Atom, Unit };  // Integer = arbitrary precision
    Kind kind;
};

struct SizedIntType {
    int bits;        // 8, 16, 32, 64
    bool isSigned;    // Byte == SizedIntType{8, false}
};

struct SizedFloatType {
    int bits;        // 32, 64
};
```

Note `Float` is dropped from `PrimitiveType::Kind` entirely — with no
arbitrary-precision float, every float is some `SizedFloatType`, and
`Float64` is just the default sized float for literals, not a separate
primitive kind in its own right. This keeps "is this Integer or Float"
(for the `Number` trait) a question of "which struct is it," not an extra
enum branch to keep in sync.

`Type::byte()`, `Type::int8()`, ..., `Type::int64()`, ..., `Type::uint64()`,
`Type::float32()`, `Type::float64()` are thin constructors over
`SizedIntType`/`SizedFloatType`. Only `Int` is a true **name alias** —
it's not a separate constructor; the name resolves (parser/typechecker
symbol resolution for type names) directly to `Type::int64()`, so there is
exactly one canonical `Type` value for "the 64-bit signed integer"
regardless of which spelling was written, and `typeToString` always prints
`Int` (not `Int64`) for it. **`Float` is *not* a name alias for anything**
— it has no `Type::float()` constructor at all, because it isn't a
concrete type; it only exists as the `"Float"` trait name in the
`TraitRegistry` (satisfied by both `SizedFloatType{32}` and
`SizedFloatType{64}`), exactly parallel to how `"Number"` and the
`Integer`-the-trait sense of `"Integer"` exist as registry entries rather
than `Type` constructors. Writing `Float` in a type position (a param
annotation, a signature) means "any float," structurally identical to
writing `Number`. `typeToString` prints `Byte`/`Int8`/`Int16`/`Int32`/
`UInt8`..`UInt64`/`Float32`/`Float64` for the sized constructors by table
lookup rather than reconstructing a name from the fields.

String stops being a `PrimitiveType` kind; `Type::string()` becomes
`Type::list(Type::charT())`, kept as a named alias for display purposes only
(`typeToString` should still print `String`, not `[Char]`, when the list
element is exactly `Char` — same trick Haskell's `show` does for `[Char]`).

Add a notion of **typeclasses/traits** the checker knows about, independent
of the `Type` variant itself — and design this as an **open, name-keyed
registry from day one**, not a closed `enum class`. A fixed enum (the
original draft of this section) can't express a user declaring their own
trait, which directly contradicts design goal #4 ("generalizes to
user-defined records implementing the same trait") — if user-defined
typeclasses are ever wanted, an enum is a rewrite waiting to happen, so
build the open version now while the surface area is still small:

```cxx
struct TraitDef {
    std::string name;                              // "Number", "Comparable", user-defined names too
    std::vector<Signature> requiredMethods;        // e.g. Comparable requires `compare : This -> Comparison`
};

class TraitRegistry {
public:
    auto define(TraitDef def) -> void;
    auto get(const std::string& name) const -> const TraitDef*;

    // Is `type` a member of `traitName`? Built-ins (Number, Integer, Float, ...)
    // are checked structurally against the type's shape; user/NamedType
    // membership is checked against declared `implements`/method presence.
    auto satisfies(const TypePtr& type, const std::string& traitName) const -> bool;

    // Record that `typeName` implements `traitName` (from a `type`/`record`/
    // `make` block's declared parents, or built-in registrations below).
    // Coherence: at most one registration per (typeName, traitName) pair —
    // a second `make X implement: Trait` for a pair already registered is
    // a compile error, not a silent override. Same rule Rust/Haskell use,
    // and for the same reason: `this.method()` dispatch must resolve to
    // exactly one implementation with no priority/ordering rule to fall
    // back on. registerImplementation itself enforces this (throws/asserts
    // on a duplicate pair) rather than leaving it to call sites to check.
    auto registerImplementation(const std::string& typeName, const std::string& traitName) -> void;
};
```

Built-in traits (`Number`, `Integer`, `Float`, `Equatable`, `Comparable`,
`Resultable`, `Optionable`, `Showable`) are just `TraitDef`s registered at
checker startup, exactly like user traits would be — `Number` is
satisfied structurally (any `PrimitiveType::Integer`, `SizedIntType`, or
`SizedFloatType`), while `Resultable`/`Optionable`/`Showable` are satisfied
by registered implementation (a `NamedType` explicitly associated with the
trait, the same path a future user trait would use). This means
`satisfies(Int32, "Number")` and a hypothetical future
`satisfies(MyOrderedPair, "MyComparable")` go through the *same* lookup —
no special-casing built-ins vs. user traits in the call sites that consult
the registry (`inferBinaryOp`, call/overload resolution, `is?`/`as`).

### Surface syntax for declaring a trait

A new `trait` keyword/block, listing required method signatures using the
same `name : type_expr ( -> type_expr )*` shape standalone signatures
already use (Standalone top-level type signatures section), with a
reserved `This` placeholder usable inside the block to mean "the concrete
implementing type" — **not** the trait's own name, which would be
ambiguous between "the same concrete type" and "any value satisfying this
trait" (an existential, a different and much harder feature):

```
trait Equatable do
  == : This -> This -> Bool
end
```

When `Point` implements `Equatable`, conformance checking substitutes
`Point` for every `This` in `Equatable`'s stored signatures and verifies
`Point` actually defines a matching `==`/`!=`.

Parsing the block itself is structurally identical to a `module`/`make`
body — a keyword, a name, `do ... end`, containing a list of
`TypeAnnotation`s. `parseTypeAnnotation` itself needs one small grammar
addition first, though: today `type_annotation` requires the name to be a
`LOWER_IDENT` (`grammar.ebnf`'s `type_annotation` rule), which doesn't fit
operator names like `==`/`!=` (`binary_op` tokens, not identifiers) — so
`== : This -> This -> Bool` doesn't parse as written above without this
fix. `function_def` already solved the identical problem for operator
*overloads* with a dedicated alternative sitting next to the `LOWER_IDENT`
one (`FOUL? LET binary_op function_clauses`, for `let +(other) ... end`
inside `make`); mirror that shape for signatures:

```
type_annotation
  = LOWER_IDENT TYPE_ANNOTATION type_expr
  | LOWER_IDENT COLON type_expr
  | binary_op TYPE_ANNOTATION type_expr     (* NEW *)
  | binary_op COLON type_expr               (* NEW: == : This -> This -> Bool *)
  ;
```

`Parser::parseTypeAnnotation` adds one branch — check for a `binary_op`
token before falling back to `LowerIdent`, same dispatch
`parseFunctionDef` already does — and stores the operator's textual form
(`"=="`, `"!="`) in `ast::TypeAnnotation::name`, which is already a plain
`std::string` so no AST change is needed. With that one addition,
`parseTypeAnnotation` is reused as-is for trait bodies, exactly as
originally claimed. Each `TypeAnnotation` inside the block becomes one
`Signature` in that trait's `TraitDef::requiredMethods`. A type
*implements* a trait via `make TypeName implement: TraitName do ... end`
(see `plan-effects-traits.md`'s Traits section — **not** the nominal
`type Point < Equatable` form `grammar.ebnf`'s `inheritance` rule
produces, which is a separate, older mechanism this plan doesn't reuse for
trait conformance) — at that point the checker performs the
`This`-substitution described above and verifies the type actually
defines every required method with a matching signature, turning trait
conformance into a real, checked obligation rather than a name-only
declaration. This is also exactly how `Equatable`/`Comparable`/`Showable`
themselves should be specified, once this lands — not hardcoded
`TraitDef`s built in C++, but the same `trait ... end` block, just shipped
as a stdlib-equivalent source/registration rather than user-typed. (This
is a grammar/parser addition layered onto the registry described above,
not a blocker for landing the registry itself — the registry should
accept `TraitDef`s constructed either way from day one, so this syntax is
additive once it's ready, tracked alongside phase 3.)

`NamedType` (`Result<X,E>`, `Option<X>`, user records) implementations are
populated into the same `TraitRegistry` when a `make ... implement: ...`
block is checked, plus built-in entries for `Result`/`Option`.

## Runtime representation (`src/interpreter/`)

Everything above is `src/semantic` — compile-time only. None of it is
real unless the runtime `Value` actually backs it: a `Byte` that's really
just an `int64_t` underneath isn't fixed-width (it can silently hold 9999
at runtime even though the checker promised 0..255), and an `Integer`
that's really an `int64_t` underneath isn't arbitrary-precision (it
silently wraps/UB-overflows past `INT64_MAX` even though the checker
promised it never would). Decision: implement this for real rather than
fake it at the checker level only.

- **Sized integers (`Byte`/`Int8`../`UInt64`) get real per-width storage
  and real overflow behavior**, not a shared `int64_t` with a checker-side
  "trust me" label. Concretely, extend `IntValue`
  (`src/interpreter/` value representation) with a single 64-bit slot plus
  a `{bits, isSigned}` tag — not a tagged union of separately-typed
  fixed-width fields, which would mean every arithmetic op handling N
  separate field types instead of one slot with a mask/range-check against
  the tag. Less code, and reuses `IntValue`'s existing shape rather than
  replacing it. Arithmetic on a sized int must trap or wrap
  *consistently with the checker's promotion rule* — mixed-width
  arithmetic always promotes to the larger type (`Int8 + Int32 -> Int32`;
  same-width signed/unsigned mixing promotes to the next wider signed type
  that fits both, e.g. `Int32 + UInt32 -> Int64`; mixing with `Integer` or
  any `Float` always promotes to that wider/unbounded operand) — the
  runtime and the checker need to agree on this one rule, not have the
  checker assume "safe" while the runtime silently wraps.
- **`Integer` (the default, arbitrary-precision type) needs a real bignum
  backing**, not `int64_t`. This is the single largest piece of net-new
  runtime work in this entire plan — arithmetic (`+`, `-`, `*`, `/`,
  `modulo`), comparison, and string conversion all need bignum
  implementations, not just a type-checker label. **Decision: vendor
  GMP** rather than hand-roll a minimal bignum — correctness and
  performance for arbitrary-precision arithmetic are exactly the kind of
  thing not worth re-deriving in-tree, and GMP is the standard choice
  every other language with this feature reaches for. This does mean
  `CMakeLists.txt` needs an actual dependency story (today it has none —
  no package manager, no vendored deps), so the first concrete step here
  is adding GMP as a build dependency (via `find_package`/`FetchContent`
  or a vendored copy) before any bignum-backed `IntValue` work starts.
  Either way: this needs its own implementation plan and shouldn't be
  estimated as "just change `PrimitiveType::Kind`."
  **Licensing decision: dynamic linking only.** GMP is dual-licensed
  LGPLv3/GPLv2 — fine to depend on under LGPL terms when dynamically
  linked, but statically linking it into a distributed `kex` binary would
  pull the GPL's stricter terms onto that binary. Decision: `kex` links
  GMP dynamically (via `find_package`, not a statically-vendored copy),
  and release/distribution builds are not statically linked against it.
  Revisit only if static distribution becomes a hard requirement later —
  don't default into it by picking `FetchContent` + static build for
  convenience without re-deciding this.
- **Conversion boundaries (`as(Type)`, literal-to-sized-type assignment)
  are where width/range checks actually execute** — `300.as(Byte)` must
  inspect the *runtime* value against `Byte`'s range and return `Error`,
  which only works if the runtime value is wide enough to hold 300 in the
  first place (i.e. literals default to the bignum-backed `Integer`
  representation at runtime too, consistent with them defaulting to
  `Integer` statically — the static and runtime default must match, or
  `as` boundary checks are checking against a value that was already
  silently truncated before `as` ever ran).
- Until this lands, sized types and arbitrary precision are **type-level
  only** and should be documented/labeled as such in any interim
  release — claiming "arbitrary precision" or "fixed-width Int8" in
  user-facing docs before the runtime backs it would be a correctness
  regression worse than today's status quo (today there's no claim being
  made at all).

## Type aliases (`type X = <type_expr>`)

The grammar already accepts this — `type_def`'s `=` form is
`type_expr ( PIPE type_expr )*`, and `type_expr` already includes tuples,
unions, functions, lists, maps (`grammar.ebnf:47-70`). So
`type MyType = (Float64, Int)` parses today (`Parser::parseTypeDef`,
`src/parser/parser.cxx:175`, the `LParen` branch at line ~270 falls
through to `parseTypePrimary`). What's missing is everything past parsing:

- `TypeChecker::checkTopLevel` (`src/semantic/typechecker.cxx`) has no
  branch for `ast::TypeDef` at all today — user type declarations are
  invisible to the typechecker. `Analyzer::analyzeTypeDef` only registers
  a `Symbol` for scope/purity purposes, not a `Type`.
- Need a registry, `std::unordered_map<std::string, TypePtr> m_typeAliases`
  (or folded into the same per-name table the trait-membership registry
  uses — see Type representation changes above), populated by walking each
  `TypeDef`'s `variants` before any function bodies are checked (aliases
  can be used before their textual position, same as functions).
- **Single non-constructor variant = pure alias, not a wrapped ADT.**
  `def->variants->size() == 1` and that one variant isn't a bare
  `UPPER_IDENT`/constructor-with-args (i.e. it's a tuple/list/map/function/
  union `type_expr`) means `MyType` should resolve to *exactly* the
  aliased `Type` — `MyType` and `(Float64, Int)` are the same type for
  unification purposes, no nominal wrapper, no runtime tag. This matches
  how `Int`/`Float` resolve to `Int64`/`Float64` (alias-by-name-resolution,
  not a `NamedType` wrapper) — same mechanism, just user-declared instead
  of built-in.
- Contrast with multi-variant or constructor-form `type X = A(Int) | B`,
  which **is** a real ADT (a tagged union, needs a `NamedType` with
  constructor identity) — only the single-bare-type-expr case collapses to
  a transparent alias. The parser already distinguishes these shapes
  structurally; the typechecker just needs to branch on
  `variants->size() == 1 && !isConstructorShape(variants[0])`.
- Recursive aliases (`type Tree = (Int, [Tree])`) need cycle detection
  during resolution (a worklist/visited-set when expanding a name), not
  unbounded recursion — same concern self-referential records already
  have via `parents`/`typeParams`.

## Standalone top-level type signatures (`something : Int -> Float -> Float`)

This is the Haskell-style `foo :: Int -> Int` convention, and most of it
already exists in Kex — `ast::TypeAnnotation` (`name`, `type`,
`implicitThis` for `:>` vs `:`) is parsed today via
`Parser::parseTypeAnnotation` (`src/parser/parser.cxx:595`) and is already
a valid item inside `module_body`, `make_body`, and `VisibilityBlock`
(`src/ast/ast.hxx:401,414,452`), e.g.:

```
module Foo do
  element : String -> [String] -> Element
  let element(name, attrs) ... end
end
```

What's missing is **top level**: `ast::TopLevelItem`
(`src/ast/ast.hxx:462-472`) doesn't include `TypeAnnotation`, and
`Parser::parseTopLevelItem` (`src/parser/parser.cxx:83`) has no case for a
bare `LOWER_IDENT (COLON | TYPE_ANNOTATION)` before a `let`. Two small,
mechanical changes, not a new concept:

1. Grammar: add `type_annotation` to `top_level_item` in `grammar.ebnf`
   (it's already a `type_expr`-based rule, so `Int -> Float -> Float`
   parses as nested `FunctionType`s with no grammar change beyond the
   top-level alternative).
2. Parser: in `parseTopLevelItem`, detect `LowerIdent` followed by `Colon`
   or `TypeAnnotation` token and call `parseTypeAnnotation()`; add
   `std::unique_ptr<TypeAnnotation>` to the `TopLevelItem` variant.

**Typechecker consumption** is the part that's actually new work: a
top-level (or module/make-level) `TypeAnnotation` for `foo` should be
treated as the *declared* signature for `foo`'s `FunctionDef`, overriding
inference rather than competing with it — when both are present, check the
inferred body type against the declared signature (and report a mismatch
there, not silently prefer one). This is also the annotation mechanism
that answers the earlier open question about "how should explicit type
annotations look in the grammar" — they already do, at the signature
level; per-parameter inline annotations (`LOWER_IDENT COLON type_expr` in
`param`, `grammar.ebnf:133`) already exist too and should compose with this
(a declared top-level signature plus inline param types should agree, not
both be required).

## Function signatures

New concept: every callable (native stdlib fn or user `FunctionDef`) gets a
`Signature`:

```cxx
struct Signature {
    std::vector<TypePtr> params;     // may reference Trait-constrained type vars
    TypePtr result;
    std::vector<TypeConstraint> constraints;  // e.g. T: Number
};
```

Stdlib signatures are declared once, centrally, e.g.
`src/semantic/stdlib_signatures.cxx`, mirroring the structure of
`src/interpreter/stdlib/*.cxx` so it's obvious which native function a
signature belongs to:

```cxx
sig("even?", {Type::constrained("T", "Integer")}, Type::boolean());
sig("odd?",  {Type::constrained("T", "Integer")}, Type::boolean());
sig("ok?",   {Type::constrained("T", "Resultable")}, Type::boolean());
```

(the `"Integer"` trait, not `"Number"` — `even?`/`odd?` reject floats,
matching Ruby raising `NoMethodError` for `Float#even?`.)

This is the file that needs updating whenever stdlib gains/changes a
function — same maintenance burden as keeping a header in sync, but it's
the only place, and it's exactly the kind of data a future formatter/LSP
also wants to load (see Tooling section).

User-defined functions: each `FunctionDef` clause's param/return types are
inferred (Hindley-Milner style: assign type vars, unify across clauses and
call sites) rather than required as annotations, but the surface syntax
should accept *optional* type annotations on params (`fn foo(x : Int)`) for
cases inference can't resolve (e.g. an empty list literal, or a function
used before its only call site). That's a grammar/parser addition tracked
as a follow-up — out of scope for the first cut, but the signature
representation above must not assume annotations exist.

## Built-in type-introspection and conversion: `is?`, `as`

Two universal methods, available on every value via UFCS, analogous to
Crystal's `is_a?`/`as`/`as?`:

- **`x.is?(Type)`** — runtime type-membership predicate, always
  `This -> Bool` for any receiver and never throws. `1.is?(Integer)` is
  `true`, `'c'.is?(Number)` is `false`. This is also the mechanism
  flow-sensitive narrowing would hook into, as future work — `if
  x.is?(String) ... end` should narrow `x`'s static type to `String`
  inside the branch, same as Crystal's `is_a?` — not designed in this
  plan, just noted as a natural extension point.
- **`x.as(Type)`** — explicit conversion, **always returns
  `Result<Type, ConversionError>`**, never raises and never silently
  truncates. This deliberately covers both "safe" widening (`5.as(Float)`
  is always `Ok`) and "unsafe"/lossy conversions (`300.as(Byte)` is `Error`
  because 300 doesn't fit in a byte; `3.7.as(Int)` truncates and is
  intentionally `Ok(3)` only if truncation-without-data-loss is the defined
  semantics for that pair — needs to be nailed down per-pair, see below).
  Naming it `as` (not `to`/`cast`) reads naturally in UFCS position
  (`x.as(Int)`) and avoids colliding with conventional `to_s`/`toString`-
  style naming the stdlib might want later for display/`Showable`.
  Because the result is always a `Result`, callers use `?` (the existing
  `ErrorPropagate`/error-bang machinery) or `match` to handle failure —
  there's no separate unchecked variant; if a future ergonomic shortcut for
  "trust me" conversions is wanted, that's a `.as(Type)!` or
  `.as(Type).unwrap()` built from existing `Result` primitives, not a new
  method.
  - `as` needs a per-type-pair conversion table (which pairs are
    representable at all, and under what condition they're `Ok` vs
    `Error`) — e.g. `Integer -> Byte` is `Ok` iff the value fits in
    0..255; `Float -> Integer` truncates and is always `Ok` (no value is
    "too big" for arbitrary-precision `Integer`); `Char -> Integer` is the
    codepoint and always `Ok`; `Integer -> Char` is `Error` outside the
    valid codepoint range. This table is exactly the kind of thing the
    stdlib signature table (`stdlib_signatures.cxx`) should hold
    declaratively, not scatter across native function bodies.

Both should be implemented once, generically, against the `Trait`/type
registry rather than per-type — `is?` is just `satisfiesTrait`-or-exact-
type-match against the value's runtime tag, and `as` dispatches into the
conversion table keyed by `(sourceType, targetType)`.

## Additional built-in types: `Bool`, `Unknown`, `Void`

- **`Bool`** already exists (`PrimitiveType::Bool`) — listed here only to
  confirm it stays a `PrimitiveType` kind, unaffected by the numeric-tower
  changes above.
- **`Unknown`** becomes a *surfaceable* type, not just the checker's
  internal "couldn't infer this" placeholder. Today `UnknownType` is purely
  internal bookkeeping (`src/semantic/types.hxx:54`, returned when
  inference gives up). The surfaced `Unknown` is the gradual-typing escape
  hatch — modeled on TypeScript's `unknown` (safe), not `any` (unchecked):
  a value of static type `Unknown` can come from anywhere (FFI boundary,
  `compiled` blocks, deserialization), but the checker must refuse any
  type-specific operation on it (arithmetic, field access, trait-
  constrained calls) until it's narrowed via `is?`/`match`. This needs a
  distinct variant from the inference-internal placeholder — conflating
  "checker doesn't know yet" with "programmer explicitly opted out of
  static guarantees here" would make diagnostics confusing (one is a
  checker limitation to fix, the other is intentional). Recommend keeping
  `UnknownType` for the internal case and adding a `DynamicType{}` (surface
  spelling `Unknown`) for the explicit one.
- **`Void`** is the *uninhabited* (bottom) type — for functions that never
  return a normal value (always raise, always loop, always `exit`), same
  role as Rust's `!` or TypeScript's `never`. This is **not** the same as
  `Unit`/`()`, which has exactly one inhabitant and means "ran to
  completion, no meaningful result." `Void` unifies with anything (since a
  function that never returns never produces a mismatched value at
  runtime) — `inferBinaryOp`/call-result unification should treat `Void`
  as compatible with every expected type, the same special-case Haskell
  gives `undefined`/bottom. Mostly useful once the checker tracks
  control-flow reachability (a function whose every branch raises/loops);
  until then it can exist as a nameable type with no inference support
  beyond "always unifies," which is enough for it to typecheck stdlib
  functions like a hypothetical `panic(String) -> Void`.

## Function overloading (parameter types — return-type overloading deferred)

Kex already has the *syntax* for this — `ast::FunctionDef::clauses` is a
`std::vector<FunctionClause>` (`src/ast/ast.hxx:350-361`), i.e. multiple
clauses sharing one name already exist and are dispatched **at runtime**
today by pattern/arity match (Erlang/Elixir-style multi-clause functions).
What's new is making this a **static, type-directed** overload set —
clauses distinguished by declared/inferred parameter *types*, not just
arity or literal pattern shape, with the checker resolving which clause a
given call site hits and reporting "no matching clause" / "ambiguous"
as compile errors instead of only finding out at runtime.

### Parameter-type overloading (straightforward)

This falls directly out of work already planned above: once each
`FunctionDef` has a `Signature` per clause (Function signatures section)
and `inferExpr` resolves calls by unifying argument types against a
signature (Checking calls section below), overload resolution is just
"try each clause's signature against the call-site argument types, keep
the ones that unify." Concretely:

- **Unambiguous match** (exactly one clause's params unify with the call
  site's argument types) → use that clause's result type, done. This is
  the common case: `let area(r : Float) = ... end` /
  `let area(w : Float, h : Float) = ... end` overloaded by arity, or
  `let show(x : Int) = ... end` / `let show(x : String) = ... end`
  overloaded by parameter type — both resolve trivially from argument
  types alone, no context needed.
- **Zero matches** → the existing "type mismatch" diagnostic, but listing
  *all* candidate signatures so the error shows every overload that was
  tried (Elm-style "I was expecting one of these" listing), not just one.
- **Multiple matches** (e.g. a call with an `Unknown`/not-yet-narrowed
  argument, or two overloads whose parameter types both accept the actual
  argument type via trait membership — e.g. one clause takes `Number`
  and another takes `Integer`, and the argument is `Int`) → needs a
  deterministic tie-break rule, not a runtime coin flip. Recommend: most
  specific trait wins (`Integer` beats `Number` for an `Int` argument,
  mirroring how Crystal/most overload resolution rules pick the narrowest
  applicable overload), and a hard ambiguity error only when two
  candidates have the *same* parameter type signature but are otherwise
  unrelated.

  **Important distinction this doc previously got wrong:** type-directed
  overloading is a layer *on top of* Kex's existing pattern-clause
  dispatch, not a replacement for it. Two clauses with the *identical*
  parameter type signature, distinguished only by pattern shape — e.g.
  `let len([]) = 0` / `let len([_|t]) = 1 + len(t)`, both `[Int] -> Int` —
  are completely ordinary and must keep working exactly as today: the
  checker resolves the *signature* once (there's only one type-level
  overload here), and the existing runtime pattern match still chooses
  between the two clauses within that signature. Static overload
  resolution by parameter type only kicks in when clauses have
  *genuinely different* signatures (different arity, or a parameter type
  that isn't unifiable with another clause's, e.g. `String` vs `Int`).
  Conflating "different signature" overloads with "same signature,
  different pattern" clauses — e.g. by treating two same-typed clauses as
  a redeclaration error — would break ordinary recursive pattern-matched
  functions, which are idiomatic Kex, not an edge case.

### Return-type overloading — deferred, not in scope for this plan

"Overloading on what we return" (picking a clause based on the type the
*caller* expects, e.g. Haskell's `read :: String -> a`) is a different
and significantly harder problem than parameter-type overloading above —
it needs **bidirectional type checking**, where an "expected type" flows
*into* `inferExpr` at call sites, not just types flowing *out* as the
current `inferExpr -> TypePtr` signature does everywhere. That's a
structural change to the checker (a second `checkExpr(expr, expectedType)`
mode), not an incremental addition, and isn't needed for anything else in
this plan.

**Decision: skip it entirely for now.** No phase below implements it, and
no other part of this plan should assume it exists — a call that would
need return-type-only disambiguation is simply unsupported (today's
either-zero-or-one-match overload resolution still applies; if that's
ambiguous without bidirectional checking, it's a compile error, same as
any other unresolvable overload). Worth reconsidering once the core
signature-checking machinery (phases 1-7) is solid and there's a concrete
motivating case for it — at that point it'd need its own follow-up plan
covering the `checkExpr` addition outlined above, not a phase bolted onto
this doc.

## Checking calls (`TypeChecker::inferExpr` for `FunctionCall`/`MethodCall`)

Currently these branches infer args "for effect" and return `unknown()`.
Change to:
1. Resolve the callee's `Signature` (stdlib table, or the symbol table
   entry built while checking `FunctionDef`s — multiple clauses become an
   overload set resolved by parameter type, per the Function overloading
   section above, not just arity/pattern shape).
2. Infer each argument's type, unify against the corresponding param type
   (respecting `Trait` constraints, not just nominal equality).
3. On mismatch, emit a `Diagnostic` with the call-site location, the
   resolved signature, and the concrete mismatching argument (see Error UX).
4. Return the signature's result type (substituting solved type vars).

`MethodCall` (UFCS sugar, `x.even?`) desugars to the same signature lookup
with the receiver as the first argument — this should share the exact same
code path as `FunctionCall`, not a parallel implementation, since UFCS is
"the same call, different syntax" by design.

## Error UX

Every type diagnostic should follow one template, modeled on Elm:

```
<file>:<line>:<col>: error: `even?` expects Integer, but `'c'` is Char

    'c'.even?
    ^^^ Char, not Integer

even? : Integer -> Bool
```

Concretely this means `Diagnostic` needs to carry enough structured data
(not just a pre-formatted string) for `kex check`'s renderer to build this
— the offending subexpression's source span, expected type/trait,
actual type, and (when known) the full signature being violated. Keep the
freeform `message` for cases that don't fit the template, but prefer the
structured path for anything coming out of signature-checking.

## Architecture for tooling (lint, formatter, LSP, REPL completion)

The reason to get this right now rather than later: every future tool
needs "given this AST node, what's its type / candidate completions /
which traits does it satisfy" as a **query**, not as a side effect of
running the whole pipeline. Concretely:

- `TypeChecker` should build and expose a `TypedProgram` (AST node ->
  resolved `TypePtr` map), not just a `vector<Diagnostic>`. `kex check`
  only needs the diagnostics; LSP hover and REPL completion need the map.
- Keep `analyzer.cxx`'s two phases (scope/purity, then types) but make both
  produce queryable tables (`SymbolTable`, the new type map) that outlive
  the single `check()` call, so a long-lived LSP process can re-run just
  the changed function's inference and patch the table incrementally
  later, rather than nothing being designed for incrementality from day one.
- Stdlib signatures living in one declarative table (`stdlib_signatures.cxx`)
  is what lets completion ("what can I call on a Number?") and lint
  ("this stdlib call is deprecated/wrong-arity") share one source of truth
  with the typechecker, instead of three places knowing about `even?`.
- Don't bake "report to stderr" into `TypeChecker`; it already returns
  `Diagnostic` structs to a caller-supplied vector, which is right — keep
  every future consumer (formatter doing "is this safe to rewrite",
  LSP) going through `Analyzer::analyze` / `TypeChecker::check` and reading
  the result, never re-implementing inference.

## Rollout phases

1. **DONE. Numeric tower + Char + String-as-[Char] in `types.hxx`/`types.cxx`.**
   No checker-visible behavior change yet beyond representation, plus
   `typesEqual`/`typeToString` updates and tests. (Still type-level only —
   see phase 2 for making it real at runtime.)
2. **DONE (bignum `Integer`), NOT STARTED (sized-int overflow trapping).**
   `Integer`'s arbitrary-precision case is real now, not type-level-only:
   `IntValue{int64_t}` stays the fast path, and a new `BigIntValue{mpz_class}`
   (`src/interpreter/value.hxx/cxx`) is the fallback — every arithmetic op
   (`+`/`-`/`*` via `__builtin_{add,sub,mul}_overflow`, `/`/`%`/unary `-`),
   `valuesEqual`, literal parsing (`evaluator.cxx`'s `IntLiteral` case),
   `LiteralPattern`/type-name pattern matching, and `even?`/`odd?`/`abs`/
   `modulo`/`Integer::parse` (`stdlib/integer.cxx`) all promote on overflow
   and demote back to `IntValue` when a result fits again
   (`asInteger`/`integerResult` in `value.hxx/cxx` are the shared entry
   points). GMP is a required dependency now (`CMakeLists.txt`,
   `find_library` forced to shared-only suffixes, `FATAL_ERROR` if
   missing) — dynamically linked only, per the licensing decision above;
   confirmed via `otool -L` that the built binary links `libgmp`/`libgmpxx`
   as `.dylib`, not statically. 15 new tests in `interpreter_test.cxx`
   cover overflow promotion, demotion, comparison/equality/division/modulo
   across representations, `factorial(25)`, and a too-large-for-int64_t
   literal both as a value and as a `match` pattern.
   **Not done:** sized integers (`Byte`/`Int8`../`UInt64`) still have zero
   runtime backing or even runtime *surface* — nothing in the language
   today constructs a value statically tagged as one (no `as(Byte)`, no
   stdlib function takes one), so there's nothing to trap/wrap yet. Revisit
   once `is?`/`as` (Built-in type-introspection section, not started) gives
   sized types an actual entry point.
   **Phase 5 note:** phase 5 below was implemented *without* waiting for
   this phase to be complete (it shipped before the bignum work above
   existed), by deliberately not enforcing the distinction this phase
   would add — see phase 5's note for why that's the safer order in
   practice, not a violation of the dependency above.
3. **DONE. Trait system** (`TraitRegistry`/`TraitDef`, `satisfies`, registry
   for `NamedType`s) + stdlib signature table — `src/semantic/traits.hxx/cxx`
   and `stdlib_signatures.hxx/cxx`, ~25 entries starting with
   `even?`/`odd?`/`ok?`/`error?`/arithmetic-adjacent functions and the
   `Math::*` namespace, per the file's own comments for the full list.
4. **DONE. Wire signature checking into `FunctionCall`/`MethodCall`** in
   `inferExpr` (`TypeChecker::checkCall`), replacing the
   `return Type::unknown()` stubs for any name found in the stdlib table.
   **Not done:** the Elm-style source-snippet-with-carets renderer — that
   needs raw source text plumbed into `TypeChecker`/`Diagnostic`, which
   doesn't exist today. Used a structured multi-line freeform `message`
   instead (headline + every candidate signature listed), which the Error
   UX section explicitly allows as a fallback.
   - **4.5. DONE. `match` exhaustiveness checking** (`checkMatchExhaustiveness`).
     Built-in `Option`/`Result` (the real `Just`/`None`/`Ok`/`Error`
     constructors per `adt.cxx` — note `docs/types.md`'s `Nothing` spelling
     for `Option`'s empty case doesn't match the actual prelude and should
     be corrected there) plus any user `type X = A | B | ...` are tracked
     in a constructor registry built from a pre-pass over `TypeDef`s. Bails
     out (no diagnostic) whenever it can't prove a closed set — an
     unguarded wildcard/var clause, an unregistered constructor, or
     patterns spanning more than one ADT — rather than risk a false
     positive. **Side fix required to make this usable at all:** neither
     `TypeChecker::inferExpr` nor `Analyzer::analyzeExpr`'s `MatchExpr`
     handling bound the variables a pattern introduces (e.g. `Circle(r)`
     never defined `r`), so every real match with bindings failed
     `kex check` with a false "Undefined identifier" — fixed in both via a
     new `bindPatternVars` recursing through constructor/list/tuple/record
     patterns. `ReceiveExpr` had the same gap in `Analyzer` (and wasn't
     checking guards at all) — fixed alongside since it shares
     `MatchClause`.
5. **SCOPED, PARTIALLY DONE. User-defined function signature inference.**
   The full sub-phase breakdown below (5a generalization/instantiation, 5b
   call-graph SCCs, 5c overload tie-break) is **not** implemented — what
   shipped instead is a deliberately smaller, real slice: each
   `FunctionDef` clause gets a `Signature` from its declared
   `param : Type` annotations (`TypeChecker::resolveTypeExpr`, new —
   handles `TypeName`/`GenericType`/`FunctionType`/`TupleType`/`ListType`/
   `MapType`/`OptionalType`/`UnionType`/`AtomType`/`GenericVar`, with
   single-letter names resolving to a `TypeVar` reused across one clause)
   or a plain `freshTypeVar()` when unannotated, registered into
   `m_userSignatures` and checked via the same `checkCall` path as phase 4.
   Deliberately **not** done, matching 5a/5b's own scope: no let-
   polymorphism generalization beyond the single-clause generic-var reuse,
   no call-graph SCC analysis — a call to a function defined *later* in the
   file, or a self/mutually-recursive call, isn't checked (silently falls
   back to `Type::unknown()`, same as before this phase — not a new gap,
   just not yet improved). `make`-block methods are deliberately excluded
   from `m_userSignatures` — their implicit `this` receiver isn't a regular
   param, so `checkCall`'s UFCS "receiver is argument 0" desugaring would
   mis-count their arity; their bodies are still checked, just not
   registered for call-site checking.
   **Note on phase 2 ordering:** this landed *before* phase 2's runtime
   backing, which the dependency note above says not to do — handled by
   relaxing `argMatchesParam` rather than waiting: any two `Integer`-trait
   members (arbitrary-precision `Integer`, any `SizedIntType`) are treated
   as mutually compatible, and likewise for `Float`, since `IntValue`/
   `FloatValue` are still just one `int64_t`/`double` at runtime today —
   hard-erroring on a width/precision distinction the runtime doesn't keep
   would itself be the premature-gating problem phase 2's note warns about.
   `Integer`-vs-`Float` stays a real mismatch (that one *is* runtime-backed
   today). This was found empirically: `let double(n: Int) = n * 2` infers
   return type `Integer` (literal `2` defaults to `Integer`, and mixing
   with `Int` promotes to the wider type per the Numeric tower rules), and
   `double(double(n))` then fails matching `double`'s own `Int` param
   without this relaxation — a real tension between the numeric tower's
   promotion rules and sized-param declarations that's worth keeping in
   mind once phase 2 actually adds runtime width/overflow semantics; this
   relaxation may need revisiting at that point, not just deleting.
   - **5a. DONE (unification slice).** TypeVar substitution map
     (`m_subst: unordered_map<int, TypePtr>`) with `resolve`/`unifyVar`
     methods in `TypeChecker`. `inferBinaryOp` now propagates type
     constraints from binary-op operands back to the TypeVars that
     represent unannotated parameters: arithmetic ops (`-`, `*`, `/`, `%`)
     constrain operand TypeVars to `Number`; `+` constrains to `String`
     when the concrete side is `String`/`Char`, to `Number` when it's
     numeric; ordered comparisons (`<`, `>`, `<=`, `>=`) constrain the
     TypeVar to match the concrete operand. `checkFunctionDef` resolves all
     param TypeVars through `m_subst` before storing the `Signature`, so
     `let double(n) = n * 2` records `double : Number -> Integer` — and
     `double("hello")` is caught at compile time with no annotation.
     **Extended in 5a-ext session:** TypeVar constraints broadened to cover
     all binary ops (`+` string/numeric inference, `&&`/`||` Bool
     constraint, ordered comparisons), unary `-` (Number), `!` (Bool),
     `if`/`while`/`then-else` condition positions (all constrain their
     condition TypeVar to Bool), call-site back-propagation (when a
     unique signature matches and an argument is a TypeVar, it's unified
     with the concrete param type — so `s.split(",")` constrains `s` to
     String without annotation), branch type consistency (`then/else` and
     `if/else` arms must agree on a type — mismatched arms are now compile
     errors), match arm type consistency (all arms must return compatible
     types), typed overload set accumulation (`let f(n: Integer)` plus a
     second `let f(s: String)` now correctly build an overload set rather
     than the second overwriting the first, so `process(3.14)` against
     both overloads lists all candidates in the error), record field type
     registry (`m_recordFields` pre-pass over all `RecordDef`s — `u.age`
     on a `User` record returns `Integer`, and a TypeVar receiver of a
     field access gets unified with the record type when the field name is
     unambiguous — so `let distance(p) do p.x * p.x ... end` infers `p :
     Point` without annotation), and `ThisExpr` / `@field` typing inside
     `make` blocks (`m_currentMakeType` tracks the current record name so
     `@width + @label` inside `make Box do` reports a type mismatch when
     the two fields have incompatible types). 76/76 specs.
     **Not done:** full let-polymorphism (generalizing a TypeVar into a
     scheme instantiated fresh per call site); the current approach is
     first-writer-wins monomorphic unification — enough for the common
     "numeric/string mismatch" case, but `identity(42)` and
     `identity("hi")` from the same call site still both resolve to the
     first concrete binding. This is the full slice intentionally shipped;
     the generalization half remains below as the future 5a-gen sub-phase.
   - **5a-lambda. DONE.** Lambda/block param type inference from call-site
     signatures. `resolveBlockHints` does a shallow generic-placeholder
     substitution (negative TypeVar IDs resolved against the concrete arg
     types) to build hint param types, `inferBlock` uses those hints to
     pre-seed the lambda params (instead of fresh TypeVars) before the body
     is checked. `[1,2,3].map { |x| x + "oops" }` is now caught because
     `x` is constrained to `Integer` from the list element type, and adding
     a String to Integer is flagged. `filter { |x| x + 1 }` is caught
     because the block returns Integer, not Bool. Paramless blocks
     (`do...end` with no `|params|` list) stay permissive (return
     `Type::unknown()`) so they match any FuncType without arity errors.
     Also added `each(Map<K,V>, (K,V)->())` to the stdlib sig table for the
     `ENV.each do |key, value|` pattern. Also implemented `# kex: no-check`
     file pragma in `main.cxx` (scan first 10 lines for the comment and set
     `skipCheck = true` if found — needed so files like
     `spec/mutating_calls.kex` and `spec/testing_dsl.spec.kex` run without
     the checker blocking intentionally-runtime-only behaviour). 78/78 specs.
   - **5b. DONE (topological ordering).** Forward-reference type checking:
     functions are now checked in dependency order (callee before caller)
     via a DFS post-order toposort of the call graph. Back edges (cycles)
     are handled by the existing pre-registration mechanism — mutually
     recursive functions still type-check correctly via shared TypeVar
     unification. Concretely: `let b(x) = a(x) + "world"; let a(x) = x * 2`
     now correctly reports a type error on line 1 (Integer + String) rather
     than silently passing. The call-graph traversal handles FunctionCall,
     MethodCall, IfExpr, MatchExpr, Lambda, BinaryOp, etc. SCCs (e.g.
     `isEven`/`isOdd`) still check correctly — the pre-registered TypeVar
     result gets unified across both function bodies before either is
     finalized, same as before. Overloaded functions (same name, multiple
     FunctionDefs) are all checked as a group under one sorted slot.
   - **5c. NOT STARTED.** Parameter-type overload resolution
     (Function overloading
     section), once 5a/5b produce a real signature per function — resolving
     overload sets by parameter type, applying the trait-specificity
     tie-break, and reporting ambiguity/no-match per the Error UX template.
     Return-type overloading is **not** part of this phase or any other —
     deferred indefinitely, see that section.
     **Current interim behavior:** `checkCall` already tolerates multiple
     full matches (silently picks the first, no diagnostic) because
     ordinary pattern-clause overloading (`len([])`/`len([_|t])`) routinely
     produces this when params are unannotated — that's the right safe
     default until 5c adds the real tie-break, not a bug to "fix" by
     erroring.
6. **DONE. `kex run` gates on `kex --check`; `--no-check` skips.**
   Two 6a audit rounds and a 6b round (prior session + this session)
   brought checker false positives to zero across all 30 example files;
   all genuinely-undefined pseudocode references in `processes.kex` and
   `real_world.kex` fixed. `--strict` flag replaced by making checking the
   default; `--no-check` is the new escape hatch. Original note follows:
   **NOT YET VIABLE — two 6a audit rounds done, false-positive rate much
   lower but not zero.** The actual blocker turned out not to be phase 2
   (the bignum half landed cleanly, see above) but the false-positive rate
   of `kex check` itself — running it over every example for the first
   time ever (`examples_test` only runs the *interpreter*, never the
   checker) found real bugs spanning multiple components, not just this
   plan's own work. Two passes so far:
   - **Round 1 fixes (all with regression tests):**
     - `argMatchesParam` didn't recurse into compound types — `[Int]` vs
       `[Integer]`, and `String`/`[Char]` vs a generic `[A]` param, were
       hard mismatches because only the *top-level* type got the
       Integer/Float-trait relaxation, never a wrapped element type.
     - `resolveTypeExpr` didn't recognize trait-only names (`Float`,
       `Number`, `Comparable`, ...) in a param annotation — `Float` resolved
       to a bogus opaque `NamedType` instead of `Type::constrained("Float",
       "Float")`, so e.g. a `Float`-annotated param rejected every actual
       `Float64` argument.
     - `checkCall` had no defense against name collisions: a `make
       CustomType do let modulo(...) ... end` method isn't registered
       anywhere (make-block methods are deliberately excluded, see phase
       5's note) — so a call to it got checked against the unrelated
       *stdlib* `modulo` signature purely because the names matched.
       Fixed with a narrow guard (only triggers when the receiver/first
       arg is a `NamedType` and *no* known overload's first param could
       ever accept it) plus an explicit bail-out whenever any argument is
       the literal unsubstituted `This` placeholder (no substitution
       mechanism exists yet — see Surface syntax for declaring a trait).
     - `checkMatchExhaustiveness` never recognized `None` as covering
       `Option` — `None` lexes as its own `TokenType::None` (`lexer.cxx`)
       and parses as a `LiteralPattern`, not `ConstructorPattern{"None"}`,
       which the exhaustiveness check only looked for. This meant **every**
       `Just(...)/None` match in the example suite was flagged as
       non-exhaustive even though it explicitly handled `None` — the
       phase 4.5 unit tests never caught this because they only tested the
       "no `None` clause at all" case, never "a `None` clause is present
       and should count."
     - `Analyzer::analyzeFunctionDef` and `Analyzer::analyzeExpr`'s
       `LetExpr` case only ever registered `param.name`/`VarPattern` —
       never `param.pattern` (e.g. `@Just(x)` this-patterns) or any other
       `let` destructuring shape (`let (a, b) = ...`, `let { k, v } = ...`).
       `TypeChecker` got the equivalent fix for params back in phase 5 but
       `Analyzer` (a separate component, separate symbol table) didn't;
       fixed both sides now via the shared `bindPatternVars`.
   - **Round 2 fixes — these turned out to be real bugs, not just checker
     false positives, confirmed by their actual runtime behavior:**
     - **`Parser::parseLetExpr` silently dropped params.** A nested local
       function (`let loop(state: Int) do ... end` used as a statement
       inside another function's body, as opposed to a top-level
       `FunctionDef`) desugars to `LetExpr{VarPattern, Lambda}` — and the
       `Lambda` was hardcoded to `{}` params, discarding the original
       clause's params entirely while keeping the body that still
       referenced them. Confirmed as a real runtime bug, not just a
       checker artifact: before the fix, calling such a function returned
       garbage (e.g. the inner value unevaluated) instead of recursing
       correctly. Fixed by converting `FunctionClause::Param` into
       `LambdaParam` (name + type; a param that's pattern-only with no
       name falls back to `"_"`, since `LambdaParam` has no pattern field —
       a pre-existing structural limit, not made worse by this fix).
     - **`main(args) do ... end` never bound `args`.** Neither
       `Analyzer::analyzeMainBlock` nor `TypeChecker::checkMainBlock`
       registered `block.params` at all — every reference to a declared
       `main` param was "Undefined identifier." Fixed in both (mirrors the
       function-param fix from phase 5/round 1).
     - **The "auto-call zero-arg, then apply" idiom isn't an arity error.**
       Every top-level `let NAME = EXPR` is a 0-param `FunctionDef`
       (`Parser::parseFunctionDef` — there's no separate top-level
       value-binding production), and referencing `NAME` bare auto-calls it
       (`Evaluator::autoCallZeroArgConstant`). So `let hello =
       makeGreeter("Hello")` then `hello("Alice")` means "call `hello()`,
       then apply `"Alice"` to *that* result" — not "call `hello` with 1
       argument," which is what 0 declared params would otherwise signal.
       `checkCall` now treats a single, unambiguous 0-param signature
       called with arguments as this idiom (returns `Unknown`) rather than
       an arity mismatch. (An overload set mixing 0-param and non-0-param
       signatures still goes through normal arity checking — this only
       fires when there's exactly one signature and it takes zero params.)
     - **A trailing `do...end`/`{ }` block argument wasn't counted toward
       arity at all** — `FunctionCall`/`MethodCall`'s `node.block` was
       inferred "for effect" and discarded, so `times(3) do ... end`
       against `times(n, block)` looked like 1 argument against 2 params.
       Now pushes a permissive `Type::unknown()` placeholder for the block
       onto `argTypes` (not its real inferred type — `Block<T>` isn't
       structurally modeled yet, see `resolveTypeExpr`'s `BlockType` case).
   - **Found but explicitly NOT fixed — out of scope, listed here so the
     next pass doesn't have to rediscover them:**
     - **Parser ambiguity:** `let NAME = { ... }` at the top level parses
       as a zero-arg `FunctionDef` with body `{ ... }`, not a value binding
       to a map literal — confirmed by printing `config` from `let config =
       { host: "x" }` and getting `<function:config>`. This is also *why*
       `examples/maps.kex` doesn't crash at runtime despite its map-literal
       keys (`host`, `port`, ...) being bare identifiers with no atom-key
       sugar: the body is never evaluated unless called. Same root grammar
       ambiguity class as the two round-2 parser fixes above, but a
       different code path (`parseFunctionDef`'s top-level production, not
       `parseLetExpr`'s nested-statement desugaring) — not attempted since
       "what should `let NAME = mapLiteral` even mean" needs a real
       decision (is *every* top-level `let X = Y` secretly a function?
       probably not the intent for non-callable values), not a mechanical
       fix like the other two were.
     - **Type aliases aren't resolved** (the Type aliases section above,
       still NOT STARTED) — a param annotated with a user `type Level = ...`
       resolves to an opaque `NamedType("Level")` via `resolveTypeExpr`'s
       fallback, so passing an atom literal that's structurally a member of
       that union still mismatches (`examples/modules.kex`'s `log` calls).
     - `examples/real_world.kex`'s `router`: a module-level `let router =
       ...` (inside a nested `using Http do ... end`) referenced from a
       sibling `foul start do ... end` in the same module — not yet
       root-caused whether this is a real cross-scope visibility gap or
       expected to fail (the surrounding file uses several undefined
       namespaces — `Router`, `Http`, `UserService`, `Logger` — suggesting
       it may be illustrative pseudocode rather than meant to fully check).
     - A pre-existing (predates this entire plan) `ListExpr` homogeneity
       check oddity in `examples/html_dsl.kex` ("Expected T2, got String").
     - **Confirmed NOT bugs** (genuinely undefined names in illustrative
       example code, same as `Router`/`Http` above) — `examples/closures.kex`'s
       `users`/`words`/`strings`/`items` and `examples/processes.kex`'s
       `config`/`Database`/`Supervisor`/`Task`/`WebServer`: none of these
       are defined anywhere in their files. `kex check` correctly flags
       them; they're not false positives.
   - **Round 3 fixes (6b, this session):**
     - Parser ambiguity: `let NAME = expr` at top level was always routed
       to `parseFunctionDef()`, so `let ages = { ... }` produced a
       zero-arg `FunctionDef` instead of a value binding. Fixed via
       `isLetFunctionDefAhead()` helper + `MainBlock::synthetic` flag so
       the bound name persists into subsequent top-level bindings
       (previously each synthetic `MainBlock` pushed its own scope).
     - Bare atom-key map literal: `{ host: "x" }` parsed `host` as a
       variable reference instead of an atom literal. Fixed in
       `parseMapOrBlock()`.
     - Type aliases: `type Level = :a | :b | ...` wasn't resolved in
       param annotations — `level: Level` rejected every `Atom` argument.
       Fixed via `m_typeAliases` pre-pass + `UnionType` branch in
       `argMatchesParam`.
     - `ListExpr` homogeneity check fired when the first list element had
       a TypeVar type (from pattern destructuring) and later elements were
       concrete — the check only skipped when `t` was TypeVar, not when
       `elemType` was. Fixed: adopt the concrete type when `elemType` is
       permissive.
   - **Conclusion after 6b:** zero checker false positives. Remaining errors
     in 4 example files are genuine undefined identifiers in illustrative
     pseudocode (`closures.kex`, `maps.kex`, `processes.kex`,
     `real_world.kex`) — `kex check` correctly flags them. Wiring `kex
     check` into `kex run` (phase 6's actual goal) is now viable.
7. **INFRASTRUCTURE DONE.** `TypeChecker::inferExpr` records every inferred
   `TypePtr` into `m_typeMap[&expr]` before returning. Public API:
   `typeOf(const ast::Expr*)` and `typeMap()`. `Analyzer` promotes
   `TypeChecker` from a local variable to a `m_checker` member so the map
   survives `analyze()`, and delegates both accessors. `main.cxx` gains a
   `--types` flag that dumps the full map (sorted by source location) when
   used with `--check`. Tooling consumers (LSP hover, formatter, REPL
   completion) can now query the map directly — no new inference logic needed,
   just new front ends reading from `analyzer.typeMap()`.

This is the full rollout — there is no phase 8. Bidirectional checking
and return-type overloading are explicitly out of scope (see Function
overloading section) and would be their own future plan, not a next
phase here.

## Sanity check / risks before implementing

A few things found on re-reading the whole plan that need resolving, not
just noting:

- **`inferBinaryOp` will silently break for sized/aliased numeric types.**
  The current implementation (`src/semantic/typechecker.cxx:328-411`)
  dispatches by `std::get_if<PrimitiveType>(&left->kind)` and compares
  `leftPrim->kind == PrimitiveType::Int`. Once `Int`/`Float` resolve to
  `SizedIntType{64,true}`/`SizedFloatType{64}` rather than a `PrimitiveType`
  kind, that `get_if<PrimitiveType>` returns `nullptr` for them, and
  arithmetic on the *most common* numeric type in the language (anything
  spelled `Int`/`Float`, or any other sized type) falls through to the
  generic `leftPrim && rightPrim` failure path. **`inferBinaryOp` must be
  rewritten to dispatch by `"Number"`/`"Integer"`/`"Float"` trait membership,
  not by pattern-matching `PrimitiveType`**, or this whole effort
  regresses arithmetic checking the day `Int` stops being a `PrimitiveType`.
  Same applies to the relational/logical-operator branches that check
  `leftPrim->kind == PrimitiveType::Bool` — `Bool` stays a `PrimitiveType`
  so those are fine, but it's a sign the function needs a trait-based
  rewrite as a unit, not a patch.
- **Three different "don't fully know the type" representations
  (`TypeVar`, `UnknownType`, the new surfaced `Unknown`/`DynamicType`) must
  not collapse into one permissive bypass.** The existing inference code
  already treats `TypeVar` and `UnknownType` identically — both skip
  checks (e.g. `inferBinaryOp`'s very first `if`, `typechecker.cxx:330`).
  The new surfaced `Unknown` type is supposed to be the *opposite* of
  permissive (TypeScript `unknown`, not `any` — must refuse operations
  until narrowed). If `Unknown`/`DynamicType` gets swept into the existing
  "skip checks for TypeVar-or-UnknownType" branches by a future
  implementer reusing that pattern, it silently becomes `any` and defeats
  the entire point of distinguishing it. Worth a code comment at that
  branch when it's touched, not just a doc note.
  **Decision for the specific case of a `TypeVar` unifying against surfaced
  `Unknown`**: unification succeeds and binds the var to `Unknown` — the
  fact propagates rather than failing unification outright. What stays
  non-permissive is every *use site* downstream (arithmetic, trait-
  constrained call, field access): each must independently check for
  `Unknown` and hard-error until the value is narrowed via `is?`/`match`,
  per the existing rule. Unification's job is only to thread the fact
  through the program; it never itself green-lights an operation on an
  `Unknown`-typed value.
- **`typesEqual`, `satisfiesTrait`, and numeric widening/coercion are
  three different relations.** `typesEqual` is exact structural equality
  (today's `types.cxx:134`, and it doesn't even handle `UnionType` —
  falls through to `return false` for any union compared against
  anything, a pre-existing gap worth fixing alongside the other
  `types.cxx` changes in phase 1). `satisfiesTrait` is membership
  (`Int32` *is a* `Number`, but `Int32 != Float64`). Widening/coercion
  (`Integer` auto-widens with a sized int; `Float32`/`Float64` widen to
  `Float64`) is a third, directional relation — "can `a` be used where `b`
  is expected," not "is `a` equal to or a member of `b`." `inferBinaryOp`
  and call-argument unification need to pick the right one of these three
  per situation, and conflating them (e.g. trying to make widening "just"
  a looser `typesEqual`) is how subtle bugs creep in. **Decision: add a
  fourth, explicit function**, `auto coerces(const TypePtr& from, const
  TypePtr& to) -> TypePtr` (returning the widened result type, or
  `nullptr`/empty if no coercion applies), kept distinct from both
  `typesEqual` and `satisfiesTrait` rather than folded into either —
  `inferBinaryOp`'s promotion logic (mixed-width/signedness/Integer/Float
  widening, all spelled out above) is exactly what `coerces` should
  implement, in one place, rather than scattered across call sites.
- **`is?(Type)`/`as(Type)` need type names to be valid *argument
  expressions*.** Calling `x.is?(Integer)` requires `Integer` to parse and
  evaluate to something in expression/argument position, not just in
  `type_expr` position. **Decision: first-class `TypeValue`** — every
  nameable type (built-in primitive, sized numeric, trait name, `NamedType`,
  alias) gets a real runtime/static value representing it, rather than
  `is?`/`as` parsing their argument with bespoke macro-like grammar. This
  is more machinery upfront than the bespoke-parsing alternative, but
  avoids painting `is?`/`as` into a permanent special case — once type
  names are ordinary values, anything else that later wants to talk about
  types as data (reflection, a `typeof`-style builtin, generic code keyed
  on a type argument) reuses the same mechanism instead of needing its own
  bespoke grammar. Concretely: a `TypeValue` variant in the interpreter's
  `Value`, a corresponding static type for it in the checker (a type whose
  values are types — i.e. `Integer`-the-name has static type
  `TypeValue<Integer>`, or simply a dedicated `Type::typeValue()` wrapper),
  and `is?`/`as` typecheck/evaluate by reading the `TypeValue` out of their
  second argument like any other value, not by special-casing the call
  syntax. Today's stdlib namespacing trick (`m_globalEnv->define("Float",
  Value::record("Float", {}))` in `src/interpreter/stdlib/integer.cxx:59`,
  used for `Float::parse`) is a record value standing in for a type name
  for static-dispatch purposes only — it should be superseded by real
  `TypeValue`s once those exist, not kept as a parallel mechanism.
