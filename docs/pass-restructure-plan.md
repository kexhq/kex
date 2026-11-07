# Restructuring the compiler into passes

The plan for the `refactor/kex-bloat` branch: a smaller, DRY compiler with one
semantic analysis whose decisions every backend follows, built as real passes.
This file is the plan of record and its status; update it as stages land.

Last updated: 2026-11-06.

## Why

Measured against `main` when the plan was written:

- `main.cxx` was 7.5k lines and `main()` alone about 3.8k. `Analyzer` was
  built ad hoc at about 13 sites; lower/emit was called from about 5.
- The hotspots were untouched: `lowerMethodCallImpl` about 2.3k lines,
  `inferExpr` about 2.3k, `checkCall` about 2.1k.
- 659 string-literal comparisons in `src/` (`"Integer"` 21×, `"kex_prelude"`
  17×, `"to"`, `"new"`, `"_"`, codec tags).
- Performance had been judged on `fib(35)`; the real benchmark is `../slick`
  (about 135 files, 9.5k lines, harmat/metro/shakespeare dependencies).

**Root cause of the recurring bugs.** Name resolution and call selection run
three times: in the type checker, again in the lowerer, and again in the walker.
The checker's answers reach the lowerer only as side tables keyed by AST node
(`resolvedCalls`, `typeMap`, `selectedCallSignatures`), so the lowerer
re-derives them, and the three disagree.

## Decisions

- **Middle end:** a typed HIR. Sema decides everything; lowering only translates.
- **Walker:** runs on the same HIR, so walker and BEAM share every decision.
- **Delivery: in place, gated.** The plan first said a strangler behind
  `KEX_PIPELINE=hir`; no such flag was ever built. Each decision moves into
  sema and its re-derivation is deleted in the same change, with every gate
  green. The merged build path, the one place an old path stood beside the new
  one, is deleted (step 2).
- **Separate compilation is the default build** (2026-11-05, by the user). Each
  unit is checked and lowered against the interfaces of the units below it;
  unchanged units come from the unit cache. The merged build is deleted. The
  point is incremental builds; the measure is slick's incremental scenario.
- **Receiver devirtualization** follows what the emitter records: each
  receiver method's per-type implementation and the dispatcher routes
  (`KexiMethod::implementation`). The old 510-line proof predicate is down to
  a few clauses over that record; the rest goes with Stage 3.
- **Visibility is decided at the declaration.** Whether a `make` targets a type
  its module declares is recorded in its interface (`KexiMethod::
  foreignTarget`); consumers read it, never re-derive it from names.
- **`src/` ends below `main`'s 89,708 lines** (2026-11-05, by the user): a hard
  goal, not only a reported delta. Not met yet, and the gap is growing: on
  2026-11-06 the `.cxx`/`.hxx` files under `src/` held 94,309 lines against
  `main`'s 89,766, counted the same way on both (about +4,500; +3,458 on
  2026-11-05 by the earlier count). Each step-3/4 item adds checker code;
  the deletions that close the gap are scheduled for Stages 3–5.
- **Pseudo-functions** (2026-11-04): one intrinsic registry plus `Type<T>`
  signatures instead of `name == "..."` branches per subsystem (Stage 2b).
  The `Type<T>` foundation and `to` signature move into step 3 because the
  `using` gate cannot be a checker decision while `to` bypasses ordinary
  argument matching. The rest of Stage 2b stays deferred to step 9.
- **`Type<T>` is the reflection record** (2026-11-06, by the user). The
  stdlib's `record Type` (`name`, `args`, `pure`) becomes generic, and a
  type-name value (`String` written as an expression) IS that record on both
  backends: `String.name == "String"`. Today it is a module value on the
  walker and the atom `:String` on BEAM, and the checker types the
  expression `String` as `String` itself (`let t = String; t.size` checks).
- **Breaking changes are fine** pre-stability (e.g. `--no-check` still runs
  resolution).

## Target architecture

```
driver   Command + Options → Session → pipeline of named, timed passes
units    unit graph; each unit checked and lowered against the KexI of the
         units below it; unit cache keyed by sources + those interfaces
front    Lex → Parse → AST → compiled-expand (AST→AST)
sema     names → TypeCheck → Select → HIR   (+ hir::verify)
         └─ the LSP reads the same symbols (SemanticDB is deleted)
mid      HIR ──► Walker (evaluates HIR)
         HIR ──► Lower (mechanical HIR→IR, no name resolution)
ir       IR passes: guard expansion, rescue guards, loop lifting,
         op devirt, verify — each a named, timed pass
back     Core Erlang emit → erlc; KexI / interface-cache emission
```

HIR (`src/hir/`): every expression carries its type; every name is a symbol in
one table; every call carries a resolved target (local, imported, dispatch,
trait method, constructor, builtin, ...) with placed arguments, defaults and
trait dictionaries. `hir::verify()` checks every name resolved, every call
targeted, every expression typed (`--verify-hir`); `--dump-hir` prints it.

## Status at a glance

| Stage | What | Status |
|---|---|---|
| 0 | Benchmark on slick | Done |
| 1 | Foundations (cache, enums, shared walkers, audit, `main()` split) | Done; the open items are in the order below |
| 2 | One sema decides every call; HIR | **In progress**: decisions recorded, `hir::Program` answers them, node identities and `hir::persist`; no HIR node types yet |
| 2b | Intrinsic registry + `Type<T>` signatures | `Type<T>` and `to` moved to step 3; the rest **deferred** |
| 3 | Lowering only translates; IR passes | **In progress**: lowering re-derives no call (metric 0); still walks the AST |
| 4 | Walker on the HIR | **In progress**: runs, REPL and prelude read checked decisions; still evaluates the AST |
| 5 | LSP / SemanticDB on the same sema | **In progress**: runs and builds have no SemanticDB; `kex -C` and the LSP still use it |
| 6 | Delete old paths, measure | **Started**: `docs/architecture.md`; measurements below |
| — | Separate compilation as the default | **Done** (2026-11-06); the merged build path is deleted |

## Order of work (decided 2026-11-05)

All of these are part of the refactor; none is optional. In order:

1. **Separate compilation is the default build — done.** The separate check
   decides the build alone; slick builds and `tey test` passes under it, and
   the incremental scenario was measured against `main`. The unit cache skips
   check and lowering for a cacheable unit whose sources and imported
   interfaces are unchanged.
2. **Delete the merged build path — done.** One build path is left for the HIR to
   serve. The lowerer's same-unit scoping rules are not merged-only: one unit
   can hold several modules. They go in steps 3 and 4. Step 3's error spec
   (`error_measure_to_without_unit_module`) must keep passing through it.
3. **The `using` gate is a checker decision.** A call that only a hidden
   `make` of an out-of-scope module takes gets a target saying so; lowering
   emits it and the walker raises it. The backends' own gates are deleted
   (below: `moduleScopedMethods`, `gatedOut`, both `gatedModuleTaking`s).
   No backend guesses the module from type names. Visibility already comes from the declaration
   (`KexiMethod::foreignTarget`).
   Found 2026-11-06: the checker does not check `to`'s arguments at all
   (`90.minute.to(String, in: 5)` passes it and fails in lowering), because
   `to` is still a special case. Ordinary functions move first. Then add
   `Type<T>` and the ordinary `to : A -> Type<T> -> T?` signature here, so
   the same argument matching and visibility rule handles `Units.SI`'s
   `to(String, in: SIPrefix)`. Remove the old `to` branches from the checker
   and backends with that change.
   A global `Type<T>` type for bare type-name values breaks existing generic
   APIs and the `Void` value: migrate those signatures together (stage T3
   below). The bare-`to` branch itself is gone (below).
4. **Stage 2 proper.** `--verify-hir` clean on `spec/`, `examples/`, the
   stdlib and slick; HIR node types (every call its target, arguments placed,
   defaults filled; every expression its type); one resolver (`ResolvePass`
   and the checker's scope tracking merged).
   Stdlib declarations written for the older compiler are corrected where the
   check finds them imprecise, rather than worked around: `type Range` becomes
   `Range<T>`, so `items` returns `[T]` and `get` returns `T?`.
5. **Stage 3: lowering from HIR nodes**, a switch on the target kind; the IR
   rewrites (guard and rescue expansion, loop lifting, operator devirt) move
   to `src/ir/passes/`; the devirtualization predicate's remaining clauses
   become a read of the target; the old `lower*.cxx` cascade and the side
   tables are deleted.
6. **Stage 4: the walker evaluates HIR nodes**, compile-time evaluation
   (`compiled do`) included; its own resolution and its run-time copies of
   the visibility rules go.
7. **Stage 5: the LSP reads the checker's symbols; SemanticDB is deleted.**
8. **Stage 1 leftovers:** one `TypeExpr` printer (three exist).
9. **Stage 2b:** finish the intrinsic registry and migrate the remaining
   pseudo-functions onto the `Type<T>` foundation from step 3.
   **Deferred with it (2026-11-06, by the user): Duration is a time Measure.**
   `1.second` gives a `Measure` and `1.seconds` a `Duration` today, the type
   picked by a spelling. Decided: one value, a `Measure` refined to dimension
   `:time`, so unit maths and the typed Time/Date/DateTime APIs both apply.
   It needs a dimensioned (refined) type in the checker.
   **Singular unit names only** (2026-11-06, by the user): `10.watt`, `1.watt`,
   `1.meter`, `10.second`. The plural time spellings go with this item:
   `nanoseconds` … `weeks` (on Integer and Float, and `Duration`'s
   `seconds(count)` …) and `months`/`years` for a `Period`. Today they are the
   only way to build a `Duration`, so they go once `10.second` is that value.
   **A measure is its quantity** (2026-11-06, by the user): `1.minute ==
   60.second`, `1.hour == 3600.second`, `1.kilometer == 1000.meter`, and so on.
   Today `==` on `Measure` is structural (the unit too), so all of these are
   `false` on both backends, and `<` is not defined for `Measure`. Equality and
   ordering compare the canonical value within one dimension; different
   dimensions are never equal, and ordering them is an error.
10. **Stage 6: measure** against the Stage 0 baseline; `src/` below `main`.

### Step 3 progress (2026-11-06)

- **The checker decides a hidden call.** A call every method of whose name
  is a `make` for a type declared outside its module, in a module no `using`
  brings in, gets target `Hidden` (owner: that module); lowering emits the
  undefined method from it instead of re-deriving the gate by name.
- **Hidden imported receiver overloads with named arguments are decided.**
  When the labelled arguments fit only an out-of-scope receiver method, the
  checker records `Hidden` with that method's module. The walker and lowerer
  raise from this target. `power.to(String, in: Units.SI.Kilo(...))` is now
  `hidden -> Units.SI` in `--dump-hir`, while the prelude's
  `90.minute.to(String, in: Hour)` stays imported. The source-derived
  interface now carries `foreignTarget` for a `make` of another module's type;
  previously that declaration fact was lost in this interface path. The
  completed slice passed `make test-all` (673 walker specs, 586 BEAM matches),
  400+4 fuzz cases, devirt correspondence and dup-audit.
- **Found on the way:** the checker's `makeModuleVisible` read only the
  declaration's own `using`s, not the lexical `using M do ... end` frames,
  so `"lexical".shout` inside `using Patcher do` was left undecided (both
  backends got it right by their own rules).
- **The backends' gates are deleted** (later 2026-11-06). Traced over
  `spec/`, `examples/`, the walker and BEAM suites and every ctest: the
  lowerer's `gatedOut` and its `staticTypeOf`-based `gatedModule`, and the
  walker's `gatedModuleTaking`, never fired. The checker's `Hidden` target
  answers first wherever there is a HIR. The one exception was the
  prelude build, whose entry module lowered with NO HIR:
  `lowerProgramTiered` copied six fields of its `LowerContext` and dropped
  `hir`. It now passes the whole context, and the rest of what `gatedOut`
  saw there were field reads (`constructor.name`) that the local
  dispatcher answers the same way. Deleted: `moduleScopedMethods`,
  `noteModuleScopedMake`, the lowerer's `makeMethodInScope`, `gatedOut`,
  `gatedModule`, the walker's `gatedModuleTaking`.
- **A receiver call's labels filter its candidates.** The per-candidate
  label test ran only for calls without a receiver, so
  `90.minute.to(String, in: 5)` picked `to(value, t, radix: Integer)` by
  position. It now runs for receiver calls too (names with or without the
  receiver). The call still checks clean, because every `to` candidate
  types its target parameter `Unknown` and the checker reports nothing
  from "unreliable" candidates: that is the `Type<T>` item.
- **`Type<T>`, stage T1 of four (checker only).** The `Type<T>` work is
  staged: T1 the checker; T2 the run-time representation (a type named as a
  value is the `Type` record on both backends, its patterns and the
  intrinsics that read type names move with it); T3 every value-position
  type name typed `Type<X>` (not `Void`, not a type used as a namespace,
  `Parser.parse(..)`), and generic APIs taking a type (`select(as: T)`)
  declared `Type<T>`; T4 the bare-`to(T)` special case and the `Conversion`
  target deleted. T1 is done:
  - A parameter written as a type name (`let to(String, in: TimeUnit)`) is
    a `Type<String>` (`typeSelectorParameter`), and a type named as the
    argument of a `Type<X>` parameter is that type as a value, for
    matching and for binding generics (`selectedType` in `checkCall`).
    `90.minute.to(String, in: 5)` is an error now
    (`spec/error_to_named_argument_type`).
  - The prelude declares `to : A -> Type<T> -> T?` and
    `to : A -> Type<T> -> Integer -> T?`; `1.to(Float, radix: 2)` is a
    `Float?` (was `Unknown?`), and `&.to(Float)` resolves through it: a
    shorthand lambda now gives `checkCall` its argument expressions.
  - **Found on the way: standalone generic contracts were never generic.**
    `ident : T -> T` over `let ident(x) = x` left `T` one shared variable,
    so `let n: Integer = ident("s")` checked clean (inline `let ident(x: T)
    -> T` was right). Contracts are generalized when registered now.
  - **And the KexI writer dropped every generic the checker inferred**
    (`convertSemanticType` wrote type variables as `Unknown`). A top-level
    function's checked signature now keeps them as letters; a method's
    does not, since its receiver's own letters (`make [X]`) would not
    match. A complete generic contract replaces the checker's signature
    wholesale; otherwise a contract fills the positions the checker left
    open (unknown or generic), so `FS.File.open`'s mode still selects
    `Read`/`Write`. One export per same-arity contract
    (`addPatchedExports`, which replaced two copies of that loop): a
    contract of another arity used to leave an unpatched duplicate behind.
  - Interim, until T2: the walker's dispatch accepts a type's module value
    for a `Type` / `Type<T>` parameter (`runtimeTypeMatches`).
- **`to` is an ordinary call (T4 for `to`, done before T2).** The
  checker's bare-`to(T)` branch is deleted: `"42".to(Integer)` resolves to
  the prelude's `to : A -> Type<T> -> T?` and is an `Integer?` by the
  ordinary rules, and a written target (`to([Integer])`,
  `to(List<Integer>)`) is the call's `targetType`, read the same way. No
  backend read the `Conversion` target for `to` (the plan's note above was
  stale): both call the prelude's `to`, whose `convertTo` takes today's
  run-time type value, so T2 is not a prerequisite. `Conversion` remains for
  `as` (Stage 2b).
- **Left:** `Type<T>` stages T2-T3. The walker keeps `makeMethodInScope`
  until it evaluates the HIR (step 6, Stage 4); without a HIR
  (`compiled do`) it is the run-time scope rule.

### Step 4 progress (2026-11-06)

Toward `--verify-hir` clean, done alongside step 3: each call below was left
undecided or typed Unknown.

- **`private do` bodies are checked.** They were
  skipped because make methods registered as they were checked; every make
  method is registered before any body now, so the skip had no cause left.
  No check result changes on `spec/` or `examples/`, slick included, and
  the undecided call sites in `spec/` fall from 517 to 401
  (`examples/json_parser.kex`: 110 to 1).
- **A module's signatures name its own types qualified, everywhere.**
  `s.matches(r)` (with `using Regex`) returned a bare `Match?` while
  `Regex.matches(s, r)` returned `Regex.Match?`. The source-read opt-in
  stdlib qualified only a signature's first parameter, and the prelude's
  flattened entry chunk qualified nothing. One qualifier
  (`moduleTypeQualifier` in `collect_metadata.cxx`) now serves units
  compiled apart, the prelude's companions and its flattened entry, and the
  source reader qualifies the whole signature. `Just(m) => m.get(0, "")`
  is decided, and `let m: Match = ...` checks. A nested module's types read
  by their full path (`Control.Retry.Retry.Schedule`), as their values
  already did.
- **More calls decided.** A call through a binding of not yet
  known type (`let applyTo(f, x) = f(x)`) is a `ValueCall`; `BEAM.m.f(..)`
  is `Foreign` (owner: its path) and `Kex.Intrinsic.*` a `Builtin`, their
  segments `ModuleReference`s; a type's `make` method called through the
  type's name (`Parser.parse(text)`) is checked as the method call both
  backends run, with the type as receiver. It was typed Unknown, so
  `let a: String = Parser.parse("x")` checked clean.
- **Intrinsics and constants.** A call the intrinsic table folds
  (`Type.of`, `Type.returnedBy`) is decided `Intrinsic` at the table's one
  dispatch point. A module constant named bare (`LIMIT.abs`, `"${LIMIT}"`)
  was taken as a namespace and typed Unknown, so
  `let twice() -> String = LIMIT.abs` checked clean; `constantType` looks
  it up through the enclosing modules, and a constant receiver is a value.
  A nullary constructor receiver (`Monday.name`, `"${Low}"`) is a value
  the same way (`nullaryConstructorType`, the one lookup the name's typing
  uses too).
- **A bare call only `make` methods answer is a method call**,
  as lowering already ran it: inside a make block, one argument short of
  the block's own method, `this` is the receiver (`decorate(@name)`);
  otherwise the first argument is (`shout(tag)`). It was typed Unknown.
- **A bare imported constructor** (`ReadFailed(path)` under `using FS`) is
  checked as its qualified spelling is and decided `Constructor`; it was
  typed Unknown (`let e: Integer = ReadFailed("x")` checked clean).
- **One declaration, one signature.** A make method is registered ahead of
  the bodies and again when its body is checked; the duplicate test used
  `typesEqual`, which never equates two Unknowns, so a parameter declared
  `Any` registered it twice, and a named-argument call (`t.string(:b,
  nullable: true)`) then saw two candidates and typed Unknown. One
  `sameParameters` (traits.hxx) replaces the seven hand-written copies of
  that test.
- **An overload set's bodies are typed by their annotations.**
  `speak : Cat -> String` / `let speak(c)` beside
  `speak : Dog -> String` / `let speak(d)` folds into ONE `FunctionDef`
  with a clause per overload, and several same-arity contracts make
  `checkFunctionDef` treat the whole set as one permissive body, so `c` is
  untyped inside it (`let n: Integer = c` checks clean). Pairing each
  definition with the annotation above it was tried and reverted: the
  definitions are one node, so the pairing gave every clause the first
  contract and `Digest.sha256(binary)` lost its `Binary` overload (slick).
  Fixed per clause instead: when a folded definition has one clause per
  same-arity contract, clause i is checked against contract i (source
  order); several contracts over one clause (`FS.File.open` by mode) stay
  one permissive body. Publication is unchanged: the annotations remain the
  call-site signatures.
- **List tails and structural fields.** The tail of `[x | rest]` was never
  inferred: `[1 | "no"]` and `["a" | [2]]` checked clean, and `@row` in a
  tail was undecided; it is checked as a list of the same elements. A field
  of a structural record (`r: { name: String } & R`) answers `r.name`
  before another type's same-named method (`structuralField`, one helper
  for both places that ask), decided `FieldRead`.
- **Calls through a structural alias** (`List.first(xs)` with
  `type List<X> = [X]`) are `xs.first`, as both backends run them: the
  checker looked up `List::first`, found nothing and left them Unknown. The
  resolved target passes no receiver, since the value is the first argument.
- **A zero-argument binding called with arguments** (`let hello = do |n|
  ... end`, then `hello(a)` or `Greet.hello(a)`) calls the value it holds:
  decided `ValueCall` and typed by the closure's result; it was Unknown.
- **Module paths through re-exports and enclosing modules.** A re-export
  (`export Numbers` in `Toolkit`) is followed when a call's path is
  resolved: `Toolkit.Numbers.double(21)` calls `Numbers.double`. A bare
  nested module named from its parent (`Router.status` inside `module Http`)
  is `Http.Router` whenever the name reaches no module with the member;
  `resolveModulePath`'s two scope walks are one.
- **A numeric function's result is its argument's type** (also wrong on
  `main`): `let sub(p, q) = p * q` returned a bare `N`, so
  `let d: Float = sub(12.5, 4.0)` was rejected. The operators bind both
  operands and the result to one `N: Number` placeholder, which
  `generalizeSignature` now turns into a generic when it relates several
  positions, and a call's `applyGenerics` substitutes bound generics as it
  did plain ones. `spec/inference_numeric_operators` pinned `N` for
  `fact(5)` awaiting exactly this; it is `Integer` now.
- **Stdlib declaration fixed: `re`.** Documented as `regex`'s alias, it had
  no annotations, so its overloaded body inferred Unknown and every
  ``re`...` `` literal (and each call taking one, `text.replace(re`..`,
  "#")`) was unchecked; it now carries `regex`'s two contracts.
- **A method matching its receiver by pattern is pre-registered.** `let
  factor(@Px) = 1.0` in `make CssUnit` was registered only when its block
  was checked, after every top-level function, so `unit.factor` there typed
  Unknown. The pre-pass now takes an explicit receiver (`@...`) as the first
  parameter, and the body types that pattern as the make target, as it did
  a destructured record receiver.
- **Top-level values are declarations in the check order.** Functions were
  checked first and top-level statements after, so a function reading a
  top-level value (`let frame = Template.html(...)`, `let greeting = ...`)
  saw nothing typed: `let show() -> Integer = greeting` checked clean, and
  every template call through such a binding was undecided. A synthetic
  main holding one `let` is now a node of the dependency order (after what
  its expression uses, before what reads or calls it), in source order.
- **A function named, not called** (`Type.of(Date.parse)`,
  `Type.returnedBy(Time.midnight)`) is decided `FunctionReference` (owner:
  its qualified name) where the intrinsic resolves the name.
- **Inherited trait defaults are the type's own.** A default has
  no signature, so `bot.shout` (a default `Bot` inherits, overridden only by
  `Player`) and `Geo.Square{..}.describe` were left undecided. Both the
  no-signature path and the overload path's existing `traitProvides` now ask
  `ownTypeAnswers` and decide `OwnMethod`.
- **Inline generic results share their parameter variables.** A clause
  such as `let wrap(value: A) -> A?` now publishes one `A`; previously its
  result annotation was resolved with a separate generic map and accepted
  `wrap("text")` as `Integer?`. Qualified function and type queries also
  share the AST's module-path reader instead of recursing through their own
  copies. The interpreter/BEAM specs, semantic tests, fuzz, devirt and
  duplication gates passed with this change.
- **Measured** after "A function named, not called", before the two items
  after it: undecided call sites in the `spec/` and `examples/` programs
  that check clean, 391 when this work started and 14 then. (The 517 → 401
  above counts every `spec/` program, clean or not.) Every change up to the
  measurement kept test-all, fuzz, devirt, dup-audit and slick green.
  Re-measure before quoting a current figure.

### Step 2 done (2026-11-06)

The merged build is deleted: `KEX_SEPARATE` and the check-only comparison
mode, the merged pre-check (`preCheckWithDependencies`), the merged analysis
and lowering in `passCheck`/`passLower`, `attachInterfaces`' collect path, and
the run-cache key's mode. A non-gating build (`--no-check`, `--emit-core`)
lowers every unit whatever the check found (it was the fallback's last user),
and a unit that does not lower fails the build. Found on the way: a qualified
record pattern of a unit compiled apart (`Kex.AST.ParseError { message }`)
resolved to the prelude's `ParseError` by suffix. `kex -C`/`--check`
(`checkEntry`) still indexes the merged program with SemanticDB until Stage
5; the run and build paths (`runSemanticCheck`) do not. Gates green; `src/`
93,842 lines.

### Step 1 progress (2026-11-05)

- **The separate check decides the build.** Each unit gets the full check
  (`configureFullCheck`: names, module rules, tagged literals); its
  diagnostics are kept whole and printed as the merged check prints them. A
  separate build that stood skips the merged analysis, for runs and for
  `kex -c`, which attaches each module's KexI as its own unit described it
  (unit cache v2 stores those chunks).
- **A trait of a unit below** gives its defaults to a type claiming it
  (`backfillExternalTraitDefaults` with the reached units' programs).
- **Cache keys are linear.** Each unit's interface digest is computed once
  (`SeparateUnitCache::interfaceDigest`); keys were re-serialising every
  unit below, about 6 s on slick.
- **Measured** (slick, median of 3 interleaved runs; `edit` changes a file's
  content, `incremental` only touches it):

  | scenario | main | merged | separate |
  |---|---|---|---|
  | no-op | 10.78 | 9.70 | 8.08 |
  | touch | 11.99 | 10.43 | 8.72 |
  | edit | 12.00 | 10.35 | 8.99 |

- **The default flipped (2026-11-06).** Builds and runs are separate unless
  `KEX_SEPARATE=0`; `KEX_SEPARATE_BUILD` is gone. Measured on the default
  build (median of 3): no-op 10.74 / 9.70 / 8.01 s (main / merged /
  separate), edit 11.28 / 10.15 / 8.70 s, `tey test` 114.8 / 113.0 / 100.7 s.
  Devirtualization fires less (1,315 firings against 3,682: calls into
  units compiled apart dispatch), and the spec run is still 12% faster.
- **Tried and reverted (2026-11-06): keying a unit by what its expansion
  read.** A `DependencyRecorder` around each dependency's expansion, plus a
  per-expansion "reached outside" count, can replace the textual
  `compiled do`/`Kex.embed`/`Template` test. But the recorded reads are not
  stable between builds: the compile-time sandbox loads a module once per
  process and the read is noted against whichever expansion loaded it first,
  and a round served from the round cache notes its stored list instead.
  Slick's hits fell from 131 to 119. A sound key needs either reads noted at
  each use in the sandbox, or a lossless digest of the expanded program.
- **Left after step 1:** units whose sources read outside
  files (`compiled do`, embeds, `Template`) are never cached, because the key
  does not cover those reads. Recording the reads in the key is the next
  speed-up. `separate_check_cli_test` and `run_cache_cli_test` cover the
  default build and its cache behavior.

## Stage 0: benchmark on slick — done

- `tools/bench-slick.sh`, `tools/bench_lib.sh`, `make bench-slick`
  (`BENCH_KEX="label=path ..."`, interleaved runs, median + IQR).
- Scenarios: cold build, no-op rebuild, incremental rebuild, dev loop, specs.
- Isolate caches with `KEX_CACHE`, `XDG_CACHE_HOME`, `TEY_CACHE`; seed the tey
  cache from `~/.cache/tey/{src,downloads,snapshots}` or dependencies are
  missing ("Unknown module: Harmat.Database").
- Run with `PATH=/opt/homebrew/bin:$PATH` (asdf's OTP 27 breaks the REPL tests).

Baseline (median seconds, main vs branch at the time): cold 26.4 / 26.5,
no-op 10.8 / 10.7, incremental 11.6 / 11.5, specs 116 / 114. After the
dependency-expansion memo and resolver cache: no-op 9.7 → 8.7 s.

## Stage 1: foundations — done (committed in `1baaf6b`)

Done:
- **Cache module** `src/cache/cache.{hxx,cxx}`: `base()` honours `KEX_CACHE`,
  `toolchainFingerprint()`, `stagingSuffix()`, `atomicWrite`/`atomicCopy`.
- **Digest/timings/well-known names**: `common/digest`, `common/timings.hxx`
  (`PhaseTimer`), `common/well_known.hxx` (`kPreludeModule`).
- **Enums** (PascalCase names and values): `TypeTag`, `CacheMissReason`,
  `NullType`, `SuffixMatchKind`, `StructuralTarget`, `ReplCommandKind`, ...
- **Shared walkers/utilities**: `ast::forEachChild` / `forEachBodyExpr` /
  `writtenModulePath` (`src/ast/walk.hxx`), `ir::forEachChild`
  (`src/ir/walk.hxx`), public `forEachTypeChild` / `mapTypeChildren` /
  `anyType`, one `mentionsUnknown` with an explicit null policy,
  `TypeName::dotted()`, `kex::join`, `readFileText`/`readFileBytes`,
  `withoutStandardLibraryRoots`, `declarationsOf`/`mergeSpecBase`,
  `sourceModulesNamedBy`.
- **`main.cxx`**: `main()` 3,821 → 349 lines. Commands are functions
  (`compileToBeam`, `runOnWalker`, `checkEntry`, `compileCoreModules`,
  `runBuiltProgram`, `preCheckWithDependencies` (deleted in step 2),
  `copyRuntimeBeams`); both REPLs
  are classes (`BeamReplSession`, `WalkerReplSession`). No function in
  `main.cxx` exceeds 400 lines.
- **`LowerContext`**: one struct of named fields replaces 11 positional
  lowering parameters (two entry points took them in different orders);
  `emitModules` replaces four copies of the emit loop.
- **Warnings**: `-Wall -Wextra` on every in-tree target; the tree builds with
  zero warnings. `-DKEX_WERROR=ON` turns them into errors.
- **Duplication audit**: `tools/dup-audit.sh`, `make dup-audit`; no metric may
  rise above `tools/dup-audit.baseline`.
- **Sandbox fix**: `Evaluator.run` now enforces `maxSteps`, `maxDepth` and
  `allow` (it enforced none). `EvaluatorOptions.modules` is still unimplemented.

Not done here, and where it went:
- `src/driver/` and a pass pipeline: done later, in Stage 3 (`src/driver/
  pipeline.hxx`, `ir::PassManager`). Commands stay functions in `main.cxx`.
- `Session::analyze` replacing the ~16 one-line `Analyzer` constructions —
  skipped; the single sema of Stage 2 removes them instead.
- One `TypeExpr` printer (three exist).

## Stage 2: one sema decides every call — in progress

Approach: the checker records a decision for every call-shaped expression; the
HIR coverage tool measures what is still undecided.

Done (staged):
- `src/hir/call_targets.{hxx,cxx}`: every call site with what sema decided.
  `kex -C --dump-hir file` lists them; `kex -C --verify-hir` fails when any is
  undecided.
- `src/semantic/decided_call.hxx` + `TypeChecker::decidedCalls()`: decisions
  that are not imported targets or selected signatures — `Constructor`,
  `FieldRead`, `ModuleReference`, `TypeReference`, `TraitMethod`, `ValueCall`,
  `Dispatch` (receiver type unknown: the value decides), `Conversion`,
  `Builtin`.
- Lowering reads `FieldRead` decisions (`staticFieldRead`; first through
  `LowerContext::decidedCalls`, now through the HIR's targets, below).
- Bug fix: a `make`-block method beside a same-named prelude method no longer
  commits to the import for an untyped receiver (false type error; BEAM called
  the wrong function). Spec: `spec/make_method_beside_prelude_import_beam.kex`.
- Two candidates for different concrete receiver types with an untyped
  receiver now record `Dispatch` instead of picking the first.

Coverage (call sites with no decision), at the start of this stage and
before the owner decisions: `spec/` 4,200 (12%) → 572 (1.5%), `examples/`
1,333 → 296, every program counted. The later figure is in "Step 4 progress"
(391 → 14, programs that check clean only); the two do not compare.

**The HIR object (2026-11-05):** `src/hir/hir.{hxx,cxx}` — `hir::Program`, built
once from the parsed program and its analysis, gives every call one `Target`
(kind, backend module/function/arity, decided owner) and every expression its
type (`typeOf`). It is the only way lowering learns what a call is: the
entry points (`compileToBeam`'s `lower` pass, the REPL, the prelude build,
the separate-unit build) each build one and pass it in `LowerContext::hir`;
lowering's own indexes of decisions are gone. `--dump-hir` prints each
target; `hir::verify` (`--verify-hir`) lists calls without one. Still to do:
HIR *nodes* of their own (today it indexes the AST), and the walker and the
LSP reading it.

What is next for Stage 2 is order step 4: `--verify-hir` clean on the corpus,
the HIR node types, and one resolver (`ResolvePass`, the checker's
`resolveModulePath`/`resolveRecordName`/`resolveTypeExpr`, and the lowerer's
`canonicalRecordName`/`registerModuleAliases`/`using` chain).

**Written module paths (2026-11-06).** `Type.returnedBy`, function references,
and the walker now use `ast::writtenModulePath`, the rule already used by the
checker's module calls. Three local recursive path parsers are gone. This is
a small part of the one-resolver work; name and scope resolution still have
separate walks.

**Node identities and a persisted HIR (2026-11-05, later).**
- **Its own answers.** `hir::Program` reads `Answers` (types, `Type.of`
  answers, selected signatures) borrowed from an analysis or owned by a
  loaded program.
- **Node identity.** `hir/persist` gives a node an identity: its position
  in `nodeOrder`, a fixed pre-order walk. That is stable within one
  toolchain fingerprint, the same sources and compiler. `save` and `load`
  write and read every node's answers by position, reusing the interface
  cache's type and signature codecs, now exposed. A program with another
  node count is refused.
- **The walker's prelude is now checked.** It is analysed as BEAM analyses
  it: tier order and the opt-in modules, `optInStdlibSources` shared with
  `compilePreludeCore`. Its items are lent to the analysis and given back.
  The checked program is the evaluator's base program (`ProgramSet::
  setBase`), read after the program's own, and it is used only when the
  check is clean.
- **Cached per toolchain** (`walker-prelude`, about 350 KB). A cold check
  costs about 150 ms, then every run loads it. A warm walker run of a
  small spec takes 0.12 s.
- **One fix the walker corpus found.** `receiverTypeOf` answered from a
  signature the checker selected for a receiver of another type (a generic
  `"${x}"` choosing Binary's `showValue` for a String). It now answers only
  when the receiver is of that type (compared by last segment, the same way
  `receiverCoversHead` compares heads).
- **Tests.** `semantic_test` "hir::persist" round-trips a program and
  refuses another program's bytes.

## Stage 2b: declarative pseudo-functions — deferred

About 183 places in checker, lowerer and walker test a callee name against a
string (`"to"` 11×, `"new"` 9×, `"send"`, `Type::of`, `"as"`, `fromBytes`,
...). Each subsystem keeps its own list, so a new pseudo-function gets
forgotten somewhere — `Type.of` alone had four handlers.

- **Intrinsic registry** (started): `src/semantic/intrinsics.hxx` +
  `typechecker_intrinsics.cxx`. An entry = name + arity + optional `check`
  (types the call itself; may return null to fall through) + optional `fold`
  (records the answer both backends emit). One dispatch point in the checker.
  Migrated: `Type.of` (both of its former branches), `Type.returnedBy`.
- **`Type<T>` arguments** (foundation moved to step 3): a type-name expression
  has type `Type<X>`; a `Type<T>` parameter binds `T`. Step 3 migrates
  `to : A -> Type<T> -> T?`. Then `as : A -> Type<T> -> T`,
  `send : Process<M> -> M -> M`, `fromBytes : [Byte] -> Type<T> -> T?` are
  ordinary stdlib signatures and the checker special cases go away.
- Then backends read folded answers instead of recognising names; the
  `name == "literal"` count is added to `dup-audit` and must fall.

## Stage 3: lowering from the HIR — in progress

Method, per decision lowering still makes for itself: have the checker record
it, prove lowering's own derivation dead over `spec/` + `examples/` (trace the
fallback in the scratch tree), then delete it and gate. Tracked by the
`dup-audit` metric `lowering-rederives` (lowering reading the raw type map,
`expressionTypes->find`): 23 at the start, **0** since 2026-11-05 (below);
it may not rise.

Done:
- The prelude lowers with the checker's answers (was the blocker).
- Field reads: `staticFieldRead` reads only `decidedFieldReads`; its type-map
  re-derivation is deleted (3,610 reads decided, the last 9 fixed in the
  checker's unique-record path).
- Built-in-owner make methods call their implementation directly
  (`count/List`): no name-based routing, no run-time dispatch.
- `ir::PassManager` (`src/ir/pass_manager.{hxx,cxx}`): named, timed IR passes;
  `standardPasses()` is the one list every emitted module (programs, REPL,
  prelude) goes through. `KEX_TIMINGS` prints one line per pass.
- **The compile pipeline is passes** (`src/driver/pipeline.hxx`): `kex -c` /
  `--emit-core` run 16 named passes over a `CompileUnit` — prelude-interfaces,
  run-manifest, pre-check, output-dir, spec-base, dependencies, runtime-beams,
  run-cache, expand, check, lower, write-core, erlc, store-caches,
  attach-kexi, finish — replacing the 397-line `compileToBeam`. `kex
  --print-passes` lists them; `KEX_TIMINGS` times each.
- **IR verifier pass** (`src/ir/verify.{hxx,cxx}`, last in `standardPasses`):
  every call's arity matches its arguments, every local call names a
  function of its module (or an enclosing `letrec`). Clean on every program
  that type-checks; it stops a broken `--no-check` build at the IR, naming
  the call, instead of in erlc.

Done since: operator overloads read the checker's `Operator` decision
(`operatorOwner`); its type-map fallback was deleted after spec-beam, fuzz
and the examples were unchanged without it (22 → 19).

**2026-11-05, later — down to 2.** Provider choice by receiver type and the
imported-free-function receiver read are gone or a `staticTypeOf`
optimisation; the checker's decisions cover them (spec
`list_join_beside_path_join_beam`). Distinct-type methods and a module's free
function over its own type are ONE rule, `Lowering::ownImplementationOf`: the
receiver type's own implementation where run-time dispatch cannot reach it
(an erased distinct value, a shared bare name). The other remaining site is
the separate build's stale prelude target (a compiled-apart provider for the
receiver's own type), which needs the "methods of a type" model in the
checker's interfaces.

**2026-11-05, later — down to 1.** The separate build's stale prelude target is
gone. It was not a new rule but the existing one seen too narrowly: the
"receiver's own type answers" half of `ownerOfCall` only knew this program's
records. `TypeChecker::ownTypeAnswers` now asks where the type lives
(`typeHomeModule`: this program, or the unit compiled apart that declares it)
and whether it declares or inherits the method; the call belongs to the type
unless the winner already is its home module. The lowering's receiver-type
string comparison against compiled-apart providers is deleted
(`separate_check_cli_test`'s `Bags.Bag` inheriting `Foldable`'s `any?`). The
last counted read is `ownImplementationOf`.

**2026-11-05, later — down to 0.** `ownImplementationOf` no longer reads a type:
the checker records what the receiver's own type answers as a decision of its
own. `CallKind::OwnMethod` (owner: the type) is its `make` method or an
inherited trait default, and `CallKind::ModuleFunction` (owner: the qualified
function) is its home module's function over it, which is `conn.close` for a
`TCP.Conn`. Both come from `ownTypeAnswers`, which now covers this program's
distinct types too, and are recorded wherever no imported target took the
call. Lowering only turns the owner into the emitted name: a distinct value's
per-type implementation, or the module function's symbol. `lowering-rederives`
is 0: lowering no longer decides any call from a type.

**Rule, not exceptions (2026-11-05).** Who owns a method call is decided in ONE
place, `TypeChecker::ownerOfCall`, after the overload rules pick a winner:
`ReceiverType` (the receiver's own type — this program's, or a unit's compiled
apart — answers it with its own `make` method or an inherited trait default,
and the winner is not its home module), `Value` (erased receiver and another
module competes: run-time dispatch), else `Winner`. Lowering's per-shape
overrides are deleted as this rule covers them; a new shape extends the rule,
not the lowering. Type reads for code generation go through
`Lowering::staticTypeOf` (the HIR's per-expression type) and are not counted.

Worklist (historical: measured before the metric reached 0) — measured by disabling lowering's type map entirely
(`L.expressionTypes = nullptr`): 29 of 573 BEAM specs then differ, in these
clusters (the specs pin each one):
- `serving` slot calls (`serving_*`, `net_websocket_*`, `net_http_*`):
  the slot call needs the receiver's server type — receiver ownership.
- Comparable / distinct-type operators (`comparable_max_min`,
  `distinct_type_operators`): pure type queries choosing the comparison —
  these become a typed-HIR accessor, not a decision.
- Method default arguments (`method_default_arguments`, `trait_default_param`,
  `named_block_argument_typed`): the owner decides placed arguments.
- UFCS function vs method, foreign receivers (`ufcs_free_function_vs_method`,
  `backend_foreign_receiver`, `nested_module_receiver_function`,
  `dsl_qualified_calls`): the owner decides the target.
- Other: `process_shared`, `closeable_traits_beam`, `writable_sockets_beam`.

Originally planned:

- New `src/ir/lower_hir*.cxx`: mechanical HIR→IR, a `switch` on the call
  target instead of the `lowerMethodCallImpl` cascade.
- IR passes in `src/ir/passes/`: guard and rescue-guard expansion, loop lifting
  (`upgrade.cxx`), operator devirtualization.
- Receiver devirtualization as an IR pass over emitter-recorded `name/Head`
  in KexI; delete `importDispatchSpelling` and its predicate (~510 lines plus
  ~500 lines of tests). `make test-devirt` stays as the check. What happened
  instead is below ("Devirtualization"): the emitter records the routes and
  the predicate shrank to a few clauses over them.
- Delete `lower.cxx`, `lowering_*.cxx`, `lowering.hxx` and the side tables
  once lowering from HIR nodes covers the corpus (no flag: see Decisions).

**Devirtualization: what the replacement needs (2026-11-05).** KexI records
each receiver method's source name (`collect_metadata.cxx`, `beamFunction =
fd.name`), worked out from the AST. The emitter's mangling (`name/Head`, only
for names in `Lowering::collidingMethods`) is decided during lowering and
written nowhere, so the predicate re-derives it. Clauses (5) and (13b), about
70 lines, only establish that `name/Head` exists.

The replacement, in three steps:
1. One rule for which receiver methods get per-type implementations, used by
   lowering and by the KexI writer alike, which records the emitted name.
2. Clauses (5) and (13b) become a read of the recorded name.
3. The routing clauses (12, DS, the `''` peers) become a per-dispatcher
   arm order the emitter records, and the predicate is deleted.

`KEX_DEVIRT_AUDIT` lists every devirtualized call site, so old and new can be
diffed over the corpus at each step.

**Steps 1 and 2 done (later the same day).**
- **Step 1.** `KexiMethod::implementation` records the per-type function the
  emitter wrote, read from the emitted Core (`recordImplementations` with
  `coreExports`). The head is the `make` target as written
  (`makeTargetName`). The prelude build, the module build and the separate
  build all record it. The separate build's own text scan is now this helper,
  and it still rewrites `beamFunction` for named receivers only. The field
  reaches `ImportedFunction` through `importedReceiverFunction`; the
  interface cache moved to version 2 with the field.
- **Step 2.** Clauses (5) and (13b) are one check:
  `winner.implementation == name/Head`. Their helpers, `signaturesAlphaEqual`
  and `typesAlphaEqual`, are deleted with their tests (−263 lines). The
  corpus audit is identical before and after: 3,396 firings, 92 distinct
  spellings.
- **(1b) stays, and it is routing, not existence.** Dropping it adds 12
  `Binary` devirtualizations (an alias or opaque head).

**Step 3 done.** Lowering records, on each receiver dispatcher's clause
(`FunClause::routes`), which per-type implementations a value of that type
reaches.
- **Arms in their real order.** `makeDispatcher` lays its arms out the way it
  emits them: the String-to-List coercion, the sorted owners (shadowed ones
  already dropped), the record field reads, then `Tuple`. An owner is routed
  when no earlier arm's guard can take one of its values.
- **One overlap rule.** That test is `runtimeShapes`/`shapesMeet`, which sits
  next to `typeGuard`. `typeGuard` and `runtimeShapes` share `guardTag`.
- **Merged ADT dispatchers** route every owner, since each `name/Owner`
  forwards into the merged function.
- **Recording.** `emitCore` keeps a clause's routes only while it is its
  function's first clause, so nothing merged in ahead can answer first. The
  result goes into `EmitResult::routedImplementations`, which every build
  path records as `KexiMethod::implementation`.
- **What the predicate keeps.** It is down to (0), (0b), (1), (1b), "the
  receiver is of that head", and the recorded route. Clauses (12) and (DS),
  the `''` and module-function vetoes, the union bucketing and the
  `candidates` parameter are deleted (−403 lines with their tests). Three
  codegen tests pin the routes (a routed record owner, Number below Integer,
  shadowed Pid).
- **What the audit showed.** With the old routing clauses still in place
  next to the recorded routes, it was identical. So every call site they
  devirtualized is one the emitter routes. Without them, 14 more spellings
  fire (3,684 firings): `showValue/Result`, `map/Result`, `to/Measure`,
  `truthy?/Integer`, and others. The old clauses refused these from the
  overloads alone, conservatively. The recorded route says a value of that
  type does reach that implementation.

## Blockers

- **RESOLVED 2026-11-04: the prelude is lowered without the checker's answers.**
  `buildPrelude` now lowers with `withCheckerAnswers`. Root cause, below, was
  calls addressed by name: a resolved make-method target on a built-in owner
  (`List`, `Map`, primitives) whose implementation is mangled (`count/List`)
  now calls that implementation directly (`collidingImplementationName`, shared
  with the definition naming; `takesPlainParameters`; `ownerNameOf`). Record
  and trait owners keep the dispatcher (unit-compiled-apart `make` extensions,
  trait defaults). Prelude guard calls: `element` −16%, `=/=` −22%.
  Original analysis: `buildPrelude`
  analyzes the merged stdlib, then lowers it with a bare `LowerContext`.
  Passing the answers (`withCheckerAnswers(..., &analyzer)`) breaks three BEAM
  specs (`capability_time_clock(_direct).spec.kex`, `mock_gate_standin.kex`):
  checking the merged prelude resolves `parts.count` / `parts.at(0)` in
  `DateTime.parse` (`src/stdlib/time.kex`) to `Kex.Mock:count` /
  `Kex.Mock:at`, so outside a spec the Mock gate refuses. Likely cause: the
  checker picks the right method (`List`'s `count`), but a resolved make-method
  target records only `count/1` with no owner. Lowering then re-decides the
  owner by name: `lowerModules` redistributes functions to "whichever module
  `definitions` says owns its NAME", and `count` is also declared in `module
  Mock` (Mock.ENV), so the call is addressed to `Kex.Mock`. Ruled out
  (2026-11-04, in the scratch tree): unknown receivers (the receivers were
  typed) and the bare-call rewrite order in `rewriteModuleCalls`. Fix: the call
  target must name its owner (receiver type + declaring module) — the HIR
  target of Stage 2 — and lowering must address it from that, never by name. Worth it beyond
  correctness: with the answers, the prelude's operators devirtualize
  (`Kex.Complex` guard calls 168 `element` → 78, 138 `=/=` → 9).

## Stage 4: the walker on the HIR

Started (2026-11-05): the evaluator takes the same `hir::Program` lowering reads
(`Evaluator::setProgram`), replacing its three separate setters for the type
map, the `Type.of` answers and the selected signatures; `kex -R` builds one
beside its analyzer. Next: the walker's own resolution (method lookup by
name, `makeMethodInScope`, `traitIdentityOf`) reading `targetOf` instead.

**2026-11-05, later.** The walker reads the same decisions as lowering, at its
two sites that chose a method by a type. A distinct type's operator reads the
`Operator` target. A receiver method uses `hir::Program::receiverTypeOf`: an
`OwnMethod`'s owner, otherwise the receiver of the signature the checker
selected (`Process.Shared` for `site.get`, an erased prelude type). One naming
rule, `Evaluator::ownMethodName`, maps the checker's qualified type to the
walker's binding: it tries `Accounts.AccountId::successor`, then the spelling
the module wrote, `AccountId::successor`. A new `walker-rederives` metric
(`staticTypeOf(` in `src/interpreter`) went from 2 to 1: a slot call is a cast when its
target says so (`hir::Target::servingCast`, the checker's `isServingCast`, which
lowering reads too). The one left shows an erased `Showable` argument by its
static type, which is type-directed by definition: it builds the dictionary, as
lowering's `makeTraitDictionary` does through `staticTypeOf`, which that metric
does not count.

`--no-check` is one rule on both backends now: the walker run analyzes silently
as the BEAM build already did, so the evaluator always reads the decisions. A
type error only stops a checked run. The 27 `no-check` programs behave
identically. What still builds an evaluator without a HIR: compile-time
evaluation (`compiled do`); the walker REPL and the wasm REPL got one later
the same day (below). Until it has one, the walker keeps its run-time lookup
as its fallback.

The checker side needed one fix found by the separate-build test. A distinct
type is registered under both its spellings, so `ThingId` and
`ThingIds.ThingId` compared unequal and the own-type rule missed the call.
`ownTypeIdentity` gives one name per type (spec
`distinct_own_method_through_using_beam`).


**Original plan:** `Evaluator::eval(const hir::Expr&)` (order step 6); delete the walker's own resolution
(`traitIdentityOf`, `unknownNamedArgument`, `makeMethodInScope`, the "mirrors
the lowerer" sites). The differential fuzzer keeps comparing walker and BEAM.

**The walker REPL on the HIR (2026-11-05, later).** An expression input is
checked as the very statements that run. Its `main` statements are expanded,
then lent to the checked program (the session's definitions and earlier
bindings), and returned afterwards; nodes do not move, so a HIR keyed by node
answers for them. The input then runs with that HIR set on the evaluator.
`site.put` on a `Process.Shared` (an erased String at run time) used to fail
in the REPL with "Undefined method: put for String"; `repl_cli_test` "runs an
input on the checker's decisions" pins the fix. The lending is one helper, `hir::checkReplInput`
(`src/hir/repl_input.{hxx,cxx}`), and the wasm REPL uses it too; it never ran
a checker before. It keeps its old leniency: an input the check rejects still
runs, on the values alone. `wasm_repl_test` pins it. That test builds
`wasm_repl.cxx` natively; `make test-wasm` needs emsdk 5.0.7 and was not run.
Definition inputs are checked the same way: their items are lent beside the
session's and given back by node. The evaluator reads a `hir::ProgramSet`,
every input's checked program newest first, which the session keeps
(`m_checkedInputs`), so a later call into a definition reads the decisions
made when it was defined. The HIR now also answers `typeOfAnswer` and
`selectedSignatureOf`, so no consumer reaches into the analyzer through it.
A definition the check rejects still runs, as definitions always did.
`/load` is checked the same way, as the loaded definitions themselves, and
still refuses a file that does not check. `repl_cli_test` "runs a loaded
definition on the checker's decisions" pins it.

**The prelude: resolved later the same day.** The walker's prelude is checked
once and cached per toolchain (`walker-prelude`, via `hir::persist`); it is
the evaluator's base program. See Stage 2's node identities.

Found on the way, and present before this change: in the walker REPL a
`distinct type` input is refused as if it were inside `compiled do`, and
`this.as(Integer)` in a distinct type's `make` block fails every later input.

What is open for Stage 4: compile-time evaluation (`compiled do`) still runs
without a HIR, so the walker's run-time lookup (`resolveMethodName`) stays as
its fallback; then the walker evaluates HIR nodes (order step 6).


## Stage 5: LSP and SemanticDB on the same sema

`collect_pass` / `resolve_pass` fold into the resolver; LSP completion and
references read the symbol table.

**Measured (2026-11-05).** Every checked run (`-R`, the `-c` gate) runs two
semas: SemanticDB's collect and resolve passes, then the Analyzer. On
`examples/json_stdlib.kex` SemanticDB costs 83 ms to load the prelude and
7 ms for the entry file, while the Analyzer takes 17 ms. That `-C` printed the same output with SemanticDB's diagnostics switched off
proved nothing: `-C` does not go through `runSemanticCheck`. On the run path
SemanticDB also finds undefined names and unknown named arguments that the
Analyzer does not (`error_undefined_function`, `error_record_module_bare_ctor`,
`error_unknown_named_argument`). Seven of its checks also had no Analyzer
counterpart and no spec: the `export` and `using` validation in
`ResolvePass::resolveExportDecl` and `resolveUsing` (a private name imported,
exported or listed in `except:`, an export of a missing module or of itself,
`only:` beside `except:`, a module with no public names). First step: those
checks move into the checker as one module-validation rule over the program's
module declarations and the imported interfaces, each pinned by a spec. Then
the run path drops SemanticDB (about 90 ms off every checked run). The LSP
keeps it until completion and references read the checker's symbols.

**Done (2026-11-05, later).**
- **One collector.** `collectSymbols(const ast::Program&)` is the rule for what
  a program declares and where. It was `CollectPass`'s file-bound walk.
  SemanticDB indexes with it, and the checker validates with it.
- **One rule.** `module_rules.{hxx,cxx}` holds the `using` and `export` rules
  over a `ModuleSymbols` view. SemanticDB answers that view through
  `IndexedModules`, and the checker through `ProgramModules`.
- **A checker pass.** The Analyzer's phase 0 runs `findModuleRuleViolations`.
  A `using` of a module nothing describes stays the build's to warn about:
  the Analyzer also runs where no modules are discovered (unit tests, the
  REPL). `only:` beside `except:`
  is the parser's check, so the copies of it in the semantic layer are gone.
- **Shared helpers.** The scope walk for a module path is
  `resolve_core::resolveModulePathIn`, and the six copies of the nested-module
  path rule are `ast::nestedModulePath`.
- **The checked run has no SemanticDB (later the same day).** `ResolvePass` no
  longer needs one. It reaches modules through `ModuleSymbols`, and its file
  state is plain views, so it runs over the checked program itself:
  `resolveProgramNames(program, files, interfaces)`. That covers the program
  with its dependencies and spec base merged in, its own modules from
  `collectSymbols`, and the prelude's from their interfaces. Diagnostics are
  kept for the entry file and the base, as the index reported them.
  Discovery, shadowed-definition warnings and symbol references are
  SemanticDB-only and stay with the language server. In that mode the
  checker owns the module rules (`m_checksModuleRules`). On
  `examples/json_stdlib.kex` the name check went from about 90 ms (prelude
  indexing plus the entry file) to 1.2 ms.
- **Specs.** Eight `error_using_*` / `error_export_*` specs pin the messages.
- **An `export` the program does not merge.** The dependency loader follows
  `using`, not `export`, so a re-exported module can be in neither the
  program nor an interface. SemanticDB found such a module on disk. The
  checker asks the build's source roots (`Analyzer::setModuleLocator`, which
  `runSemanticCheck` sets) before calling it missing, and without a locator
  it claims nothing missing. Slick's `harmat` (`export Harmat.Query`) found
  this; it is pinned by spec `export_module_only_in_source_root`.
- **Interface-only modules are checked leniently.** For a module the checker
  knows only by its interface (an opt-in stdlib module), a name counts when the
  interfaces know it at all. An opt-in module's `make` methods appear only once
  a package approves them, so a refusal is never wrong but slightly less
  precise than SemanticDB's read of the source (`spec/stdlib/net_values.spec`).

What is left of Stage 5:
- **One sema call.** The names phase is the Analyzer's first phase
  (`Analyzer::setNameCheckFiles`, set by `runSemanticCheck` to the entry file
  and a spec's base), so a build makes one `analyze` call. Analyzer users that
  set no files (unit tests, REPL checks) are unchanged. `ResolvePass` (names,
  labels) and the type checker are still two walks inside it; merging their
  scope tracking is what is left of "one resolver".
- **The walker's run-time copies of the visibility rules** (`evaluator.cxx`,
  for the REPL and unchecked runs).
- **The LSP** still reads SemanticDB's symbols, not the checker's.
- **`kex -C`** (`checkEntry`) still indexes the program with SemanticDB
  rather than going through `runSemanticCheck`'s one sema.

## Stage 6: delete and measure

Remove the old paths (the merged build, the old lowering, SemanticDB); keep
`docs/architecture.md` current. Final
report against the Stage 0 baseline: `src/` line delta, clean and incremental
C++ build time, dup-audit counts, largest function, link time.

**Started (2026-11-05).** `docs/architecture.md` describes the passes as they
are now: the driver pipeline, one sema (names, then the Analyzer), the checked
program, and both backends on it. It lists what is not on the HIR yet. The
final measurement waits for the old paths to go.

The user's decisions of 2026-11-05 (`src/` below `main`, separate compilation
as the default) are under Decisions; node identities, which came next, are
done (Stage 2).

**Measured (2026-11-05), against `main` (66a5838, the Stage 0 baseline build).**
Slick, median seconds of 3 interleaved runs, `tey` built from this tree
(`make build-tey`). The global `tey` wraps another checkout and cannot parse
slick's current `package.kex`.

| scenario    | main   | branch | change |
|-------------|--------|--------|--------|
| cold        | 24.73  | 23.83  | −3.6%  |
| no-op       | 9.97   | 8.93   | −10.4% |
| incremental | 10.74  | 9.54   | −11.2% |
| dev loop    | 10.01  | 9.26   | −7.4%  |

Checking 161 modules takes 4.9 s on the branch and 6.2 s on `main`. The spec
suite (`tey test`) takes 109.4 s on `main` and 102.3 s on the branch
(−6.5%). One of the three branch runs failed because `build/kex` was rebuilt
while the benchmark ran, so that median is of two runs.

Static measures, branch against `main`:
- `src/`: 93,166 lines against 89,708 (+3,458). The driver, HIR, IR passes,
  module rules and the cache module are new; the old paths are not deleted
  yet.
- Largest function: `inferExpr` at 2,108 lines. On `main` it is `main()` at
  3,818; `lowerMethodCallImpl` went from 2,217 to 2,090.
- dup-audit, branch against `main`: duplicate helpers 24 / 25, literal
  compares 479 / 562, type recursions 43 / 51, expression walkers 18 / 24,
  long functions 17 / 19, lowering re-derivations 0 / 22.

The benchmark also found a regression before it shipped. Slick's `harmat`
failed with "exported module not found: Harmat.Query", and the checker's
module-locator fix (Stage 5) resolves it.

## Gates (every change)

- `cmake --build build` with zero warnings.
- `make test-all` (not bare `ctest` — it misses the spec suite),
  `make spec-beam`, `make fuzz` (400 + targeted), `make test-devirt`,
  `make dup-audit` (no metric may rise).
- `make test-wasm` in CI for changes to the driver or the stdlib loader.
- New specs at the top of `spec/` need a `.expected` golden file
  (`make spec-orphans`); `describe`/`it` suites go under `spec/stdlib/`.

## Milestone gates (between milestones, not every change)

Slick is too slow to verify every change with (2026-11-06, by the user). It
runs when a milestone (an order step or stage) completes, and for
performance claims:

- slick builds and its `tey test` passes;
- `make bench-slick` against the previous milestone and `main`.

## Ground rules

- Never commit or push from the agent; changes stay in the working tree/index.
- No Python, JavaScript or TypeScript in the repository.
- C++20, `.hxx`/`.cxx`, camelCase functions, `m_member`, trailing return types,
  namespace `kex`; enums PascalCase.
- Find duplication proactively; prefer shared helpers and enums over copies and
  magic strings.
