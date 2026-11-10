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
- **The HIR is the AST plus the checker's answers** (2026-11-09, by the
  user). No separate HIR node types: `hir::Program` answers, per AST node,
  every decision (call target, selected signature, type, `Type.of` answer,
  resolved import, traits), and lowering and the walker walk the AST reading
  only those answers. "Lowering from HIR nodes" / "the walker evaluates HIR
  nodes" mean exactly that: no consumer re-derives a decision or reads a
  checker table except through `hir::Program`.
- **Every program the walker runs is checked first** (2026-11-09, by the
  user), in three steps: (1) each module the walker loads at run time is
  checked as its own unit against its dependencies' interfaces, as the BEAM
  build checks it; (2) compile-time evaluation: the checker checks
  `compiled do` blocks, and expansion checks what it is about to run; (3)
  the walker's method call becomes a switch on the target kind, its by-name
  lookup kept only for `Dispatch` calls. The extra checking is measured on
  slick.
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
- **The size of `src/` is measured, not a gate** (2026-11-08, by the user,
  revising the decision of 2026-11-05 that `src/` must end below `main`'s
  89,708 lines). Much of the branch's growth is infrastructure the plan
  wants to keep: the HIR and its persistence, the interface cache that
  separate compilation needs, the checker's decisions. What the plan does
  require is that every old path is deleted once its replacement serves
  the corpus, with no two implementations of one decision; the final
  measurement reports the line count against `main` beside that. On
  2026-11-08 the `.cxx`/`.hxx` files under `src/` held 93,709 lines against
  `main`'s 89,766 (94,309 on 2026-11-06, before the provider search went).
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
| 2 | One sema decides every call; HIR | **Done** (2026-11-09; `verify-hir-stdlib`: 0 undecided call sites; captures decided too): decisions recorded, `hir::Program` answers them, node identities and `hir::persist`; the HIR is the AST plus these answers (decided 2026-11-09, no separate node types) |
| 2b | Intrinsic registry + `Type<T>` signatures | **Done** (2026-11-10): `Type<T>` and `to` in step 3; the registry decides `Process.spawn`, the checker decides the testing DSL and the prelude `each`/`times` fold, and the backends read those decisions (a program's own `assert` or `each` is its own). The Duration-as-Measure, singular-unit and quantity-equality items of step 9 stay **deferred** (by the user) |
| 3 | Lowering only translates; IR passes | **Done** (2026-11-09): lowering is checked-only (it throws without a HIR); the provider search and its run-time choices are the checker's (alternatives), with `dualProviderDispatch` and `importedFreeFunctionDispatch` deleted; no override of a checker target is left (`localBeatsTraitDefault`, `ownedMakeFunction`, the module-path search deleted); every call reaching the local fallback is a decided kind (dispatch, trait method, field read on a structural or conflicting record, a free function called UFCS). `lowerMethodCallImpl` is a switch on the target kind and a dispatch to handlers (80 lines, from ~1150); the IR passes are in `src/ir/passes/`. Lowering reads the checker only through `hir::Program`, and captures are decided too. |
| 4 | Walker on the HIR | **Done** (2026-11-09): every program the walker runs is checked first: the entry (kex::driver, merged with the modules it uses), each module loaded at run time, and compile-time code (`compiled do`, collapse, template scans, tag validators), the checks cached across builds; the method call reads the decided target (conversion, runtime primitive, `Type.of`, trait dictionary, own method, the selected module function unless a `with` replaced its capability, value-or-namespace receivers). What goes by the value at run time is what BEAM leaves to its dispatchers too: `Dispatch` calls and the prelude's receiver functions |
| 5 | LSP / SemanticDB on the same sema | **Done** (2026-11-08): one resolver, the checker's name pass; the REPLs and `--complete` hold no SemanticDB; the language server's SemanticDB is a document store and query front end over the checker's index |
| 6 | Delete old paths, measure | **Done** (2026-11-09; the cold cost accepted by the user as the price of checking every program the walker runs; final slick table under Stage 6: every warm scenario 19–31% faster than `main`, cold +41%, the first checks of compile-time code). The merged program's `expand` pass is deleted. `docs/architecture.md`; measurements below. `src/`'s size against `main` is reported, not a gate (2026-11-08) |
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
8. **Stage 1 leftovers:** one `TypeExpr` printer — **done** (2026-11-07,
   `ast::typeText`).
9. **Stage 2b:** finish the intrinsic registry and migrate the remaining
   pseudo-functions onto the `Type<T>` foundation from step 3. **Done**
   (2026-11-10; below, under Stage 2b).
   **Still deferred (2026-11-06, kept 2026-11-10, by the user): Duration is a time Measure.**
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
10. **Stage 6: measure** against the Stage 0 baseline, and report `src/`'s
    size against `main` (no longer a gate: see Decisions).

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
- **T3, value-position type names** (2026-11-07): a type named as a value
  (`let t = String`, `x.to(Integer)`) is a `Type<X>`; `Void` stays the unit
  value. A built-in type's name no longer reads as the type itself through
  the globals annotations resolve against. Through a type's name its own
  `make` method takes the type as receiver (`Response.ok(body)`,
  `throughTypeName`), and completion after `List.` completes the namespace.
  `spec/lib/record_value`'s `first(as: Type<T>)` is the one generic API
  in the corpus that took a type; no stdlib declaration did.
- **T2, the run-time `Type` record** (2026-11-07): a type named as a value
  (`String`, `App.Models.User`) is the `Type` record on both backends, built
  from what the checker typed it (`hir::Program::typeValueOf`; a sized
  integer keeps its written name, so `to(Byte)` still narrows). A
  parameter written as a type name (`let to(value, String)`) matches the
  record's name (`Lowering::typeNameParameter`, the walker's pattern
  match). Showing a type value (`"${String}"`) works now; it failed on
  both backends. `Kex.Intrinsic.Fun.convertTo/3` reads the record as `/2`
  already did. APIs that took a type as `T` and called `Type.of` on it take
  `Type<T>` and use it (`spec/type_token_select`, the name-collision test).
  Without a checked program (`--no-check`, `compiled do`) a type name is
  still the old run-time value (an atom on BEAM, a module value on the
  walker). The walker's patterns match both; BEAM's type-name parameters
  match only the record, so such a value reaches the generic clause
  (`to(value, t)`), whose `convertTo` takes both.
- **Left of `Type<T>`:** nothing in the plan's four stages. The walker keeps `makeMethodInScope`
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
- **A method matching values by literal, `_` or name is pre-registered.**
  `let productKind(:mass, :acceleration) = :force` in `make Measure` was
  registered only when its block was checked, so `this.productKind(...)` in
  an earlier body stayed undecided (36 calls in `units/si.kex`). A literal,
  wildcard or variable pattern takes any value, like an untyped name, and
  the body types it; only constructor and structure patterns (`to(String)`)
  wait for their block.
- **One method, one signature.** Three registrations of the same method
  looked like overloads, and `this.helper(untyped)` was left to the value
  (`Dispatch`):
  - the pre-pass placeholder (`Files -> A -> B`) beside the checked body
    (`Files -> A -> String?`): the checked signature now replaces the
    placeholder registered for its clause;
  - a `:>` contract on a generic receiver (`contains? :> A -> Bool` in
    `make Set<A>`) beside the body's own registration: contracts were keyed
    by the printed receiver, whose variables each pass spells apart, so the
    body never saw its contract. `annotatedReceiverKey` files `Set<A>` by
    its head, and a concrete instantiation (`[Integer]`) whole;
  - a stdlib unit's own methods arriving again through the prelude
    interface (`Mock.Files.cannedRead`): a module the unit declares shadows
    the interface's for receiver methods on its types, as it already did
    for qualified calls (`mangle::pathScopeOf`).
  Lowering's `ownedMakeFunction` then reads the owner from the checker's
  `OwnMethod`/`TraitMethod` target instead of from the receiver's static
  type; no `Dispatch` call reached it any more. An error's candidate list
  shows the answered result (`+ : Vec -> Vec -> Vec`, was `-> A`).
- **A type's own answer counts only `make`s in scope.** `ownTypeAnswers`
  counted every `make` of the receiver's type, `Patcher`'s unimported one
  included; the duplicate placeholder above made that two (overloaded, no
  answer) and hid it. It now counts what the call's candidates include: a
  `make` in scope, or of the type's own module.
- **Rebased on `main` (643e509).** Two conflicts: `digest.cxx` (main's
  directory guard now sits in the shared `readFileBytes`, for every reader)
  and `mock.kex` (main's `Result` API, its `FS` errors qualified). Two
  semantic ones:
  - main's `module Map do let from ... end` made `Map.get(m, k)` "`Map` has
    no function `get`" (main's own fuzz target 0 fails the same way). A
    member a same-named module does not export is the structural-alias form
    (`m.get(k)`), as `List.first(xs)` is; `moduleNamesType` is the one test
    both member checks share.
  - `spec/error_to_named_argument_type` lists main's new `Culture` and
    `DateFormat` overloads of `to`.
  `names::kListType`/`kMapType` replace the 13 `== "List"`/`"Map"` compares
  (four were main's).
- **A type implementing a trait has its defaults as its own methods.**
  `make Coin, implement: Addable` registers Addable's defaults (`twice`)
  for Coin, through the same pre-pass as the block's own `let`s, unless the
  block defines that name at that arity (lowering's override rule). The
  trait body is still checked once with `This`; its checked signature then
  replaces each implementing type's copy with `This` bound to that type
  (`bindThis`), pending results answered likewise. The trait's own `This`
  signature is a candidate only in its own bodies, so an untyped value
  never inherits (`Assert.some`'s `value.present?` had become `This`), and
  two traits' same-shaped defaults stay apart (`sameMethod`). Inherited
  calls now carry the checker's selected clause; lowering's
  `selectedMakeMethod` takes the copy the call's owner names.
- **`ownedMakeFunction` and `localBeatsTraitDefault` are deleted.** The
  last own-method call lowering found by name and arity was `Word.repeat`,
  a two-clause default: `ast::signatureClauses` now adds no signature for a
  clause declaring no types beside a typed sibling of its arity
  (`repeat(0)` is a case of `repeat(n: Integer)`), and the checker's
  `receiverIsFirstParam` reads `ast::takesReceiverPattern`, the rule
  `methodArity` already used (it took `repeat(0)`'s `0` for the receiver).
  A `TraitMethod` is the value's to answer through the trait's dispatcher,
  as lowering emitted it anyway. With every own-method call carrying the
  checker's selected clause, the fallback's capability check reads that
  method. The override that put a local method in place of a trait default
  the checker chose fired only for prelude receivers (`"x".showValue`,
  `3.to(String)`) where the checker's answer was right; switched off,
  nothing changed.
- **`lowerMethodCallImpl` splits by case** (~1150 lines → ~630):
  `lowerServerSlotCall`, `lowerResolvedCall` (the checker's one target) and
  `lowerValueReceiverCall` (the value-receiver tail) are their own
  functions, moved as they were. In the tail, the branch that found a
  `using`-imported function by name and arity (`10.double`) reads the
  checker's selected signature instead: its `declaringModule` names the
  module function. That exposed a checker gap, now closed: for a receiver
  whose type is not settled it selected any module's free function of the
  name, imported or not (`Bits.or` for `findIndex {..}.or(..)` inside the
  prelude's `make String`, `badarith` on BEAM). A module's free function is
  a method-call candidate only where its module is in scope
  (`makeModuleVisible`: inside it, or under a `using` naming it). The undocumented `KEX_DEBUG_TO2`/`KEX_DEBUG_SIG` stderr
  hooks are deleted. Still undecided at the tail: a capture's expansion
  (`~even?`, `~String.upperCase`), which lowering builds after checking.
- **Calls decided by what they name lower from the target.** The head of
  `lowerMethodCallImpl` is a switch on the target kind: `Foreign`
  (`BEAM.lists.reverse`) and a `Builtin` runtime primitive
  (`Kex.Intrinsic.List.reverse`) are one plain call into the BEAM module
  the decided path names (`lowerRuntimeCall`); the other `Builtin`,
  `Process.spawn`, is `lowerProcessSpawn` on the decided state type (now
  decided even while that type is unknown); `Conversion` is
  `lowerConversion`. `beamModulePath` and the receiver-path matching are
  deleted; the roots are `names::kForeignRoot`/`kIntrinsicRoot`, and the
  checker's test is the same `isIntrinsicPath` (it took any `Intrinsic`
  segment under `Kex`). `verify-hir-stdlib` had been failing unnoticed
  since the rebase (the gate script now prints failing make targets): main's
  `DateFormat` read `culture.time` and `weekday.number` without importing
  either. `Culture` named no record there, and `Time`'s `make Weekday` was
  hidden, so both were undecided and ran by dispatch. It now imports them
  (`using Culture, only: [Culture]`, `using Time, only: [number]`).
  The switch also takes `Intrinsic` (`Type.of`, from `hir->typeOfAnswer`;
  lowering's own `staticTypeOfCalls` copy of the table is deleted),
  `TraitMethod` (the hidden-dictionary call) and `FieldRead` (through `this`
  in a `make` block, else `staticFieldRead`). These three branches had run
  for any call that matched their shape, whatever the checker had decided.
  The rest of the cascade is two handlers: `lowerModuleFunctionCall` (the
  module the checker named for a call on a module path, this unit's or one
  compiled apart; the one-element candidate loops are gone) and
  `lowerTypeNamespaceCall` (a type's static function, a module ADT's tag,
  "Undefined function"), then `lowerValueReceiverCall`.
  `lowerMethodCallImpl` is 80 lines (from ~1150 at the start of Stage 3);
  `long-functions` 17 → 16.
- **Lowering reads the checker only through the HIR.** `hir::Program`
  gained `resolvedTargetOf(call)` (the imported target with its placed
  arguments and dictionaries) and `traitsOf(make)`. Lowering's
  `resolvedCalls`, `resolvedTraits` and (unread) `expressionTypes` members
  are deleted, and `lower.cxx` no longer touches the analysis.
- **Captures are decided.** `~name` / `~Mod.fn` is a `FunctionReference`
  (owner `Module::name`), decided by `captureModule` with the rules a call
  of the name follows: a bare name reaches what `checkCall` would
  (`reachableFreeFunctions`, extracted from it and shared), else a function
  a `using` imported from a unit compiled apart; a qualified one resolves its
  module with `resolveModulePath`. Lowering's own resolution in
  `lowerCurry` (enclosing-module walk, `using` imports, aliases, opened
  modules; ~70 lines) is deleted after a trace over spec, examples and the
  prelude found the decision equal to it at every capture.
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
- **The corpus is decided: 16 → 0** (2026-11-07). Undecided call sites in
  the `spec/` and `examples/` programs that check clean were 16 after the
  `to` change, and are 0 now. Each fix:
  - *A type that claims a trait answers the trait's required methods.*
    `Kilo(Watt).factor` (Units.SI's unannotated `let factor(@Kilo(unit))`)
    has no signature on record; the requirement (`requiredMethod` in
    `traits.cxx`) types the call, decided `TraitMethod`. Not when the
    type's own method rejects the receiver's refinement: writing through a
    read-only `FileHandle` stays a mismatch, whatever `Writable` says.
  - *A trait method's `This` is its receiver's type* (`"x".repeat(3)`,
    Monoid's default, is a String).
  - *`~banner` names the function*, even one of no parameters: a
    `() -> String`, not its result.
  - *A spread* (`[...xs, 99]`) is checked and stands for the list's elements.
  - *Rescue clauses are checked everywhere one can appear*: on a function
    clause, `main`, a block and a lambda, not only on `trying`
    (`checkRescue`, one walk for all of them).
  - *An overload the arguments cannot pick yet* (a closure body over
    untyped parameters) is decided `Dispatch`: the value picks at run time.
  - `examples/real_world.kex` called an `Http.Server.listen` that did not
    exist; the example now defines it.
- **The stdlib is decided: 18 → 0** (2026-11-07). `--verify-hir
  --build-prelude DIR` verifies the prelude build's own HIR (each problem
  names the receiver type the checker saw), and `make verify-hir-stdlib`, part
  of `test-all`, keeps it at 0. Each fix:
  - *A module function's qualified entry has its own arities.* `Time.now`
    calling `DateTime.now` (defined later) found no `DateTime::now`: the bare
    `now` that `Time.now` had registered first made the pre-pass skip
    `DateTime.now`'s clause, qualified entry included. In user code it was a
    false error (`` `Source` has no function `now` ``).
  - *A receiver only an out-of-scope `make` takes is `Hidden`*, also when the
    name exists on other types (`d.number` with `Cal`'s `make Day` and the
    prelude's `Weekday.number`). It was undecided, and ran anyway. The
    stdlib's own `make Date` called `make Weekday` (inside `module Time`)
    that way; it now calls `Time.weekdayNumber`.
  - *An interop path ends at a call.* `Kex.Intrinsic.Type.fieldLayoutsOf(n).map`
    was decided as the path `Kex.Intrinsic.Type.fieldLayoutsOf.map`, and the
    intrinsic's argument went unchecked.
  - *Any binding called is a `ValueCall`* (`Just(rule) => rule()` with a
    `Block<DateTime>` `rule`), not only function- or variable-typed ones.
  - *An inherited default with no home module to answer for it is
    `Dispatch`*: Monoid's `repeat` on a String, Enumerable's `map` on a Range.
  - *Stdlib declarations fixed*: `Mock.FS` built `FS.FileError`s with bare
    constructors no `using` reached (`ReadFailed` resolved to `ReadError`'s
    nullary one); they are qualified now.
- **A constructor's payload count is checked** (also missing on `main`):
  `Broken(5)` for a nullary `Broken` and `Torn(1, 2)` for `Torn(Integer)`
  checked clean, and the first died at run time. Payload TYPES are not
  checked yet: tried, it rejected `Get(Process.self)` (a `Pid` for a
  `Process<T>` payload, which an ordinary call rejects too) in three specs
  and examples, so `Process.self`'s declared type is the fix to make first.
  `taggedvalidation.kex` passed a bare `ByteSpan` for its `ByteSpan?`
  payload; it passes `Just(At(offset))` now, and the tag validator reads
  both. A payload is re-resolved in its declaring module by one helper
  (`declaredPayload`).
  Not yet measured: slick.

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
- One `TypeExpr` printer — done 2026-11-07: `ast::typeText(type, spelling)`
  (`src/ast/type_text.cxx`) replaces `diagnostics::typeExprToString`,
  `ast::renderDispatchType` and the walker's `runtimeTypeKey`. Its
  `TypeSpelling` is `Source` (diagnostics, the language server) or `Dispatch`
  (the backends' type tests, the walker's overload key).

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

## Stage 2b: declarative pseudo-functions — done (2026-11-10); the Duration items deferred

Resumed after the HIR, as decided (2026-11-10, by the user: the registry and
`Type<T>` now, the Duration-as-Measure, singular-unit and quantity-equality
items of step 9 still deferred). What it finished:

- **The registry decides `Process.spawn` alone.** Its check that the argument
  is a state, not a record's name, was a `"spawn"`/`"Process"` name test in
  the method-call path; it is the entry's `check` hook now
  (`checkProcessSpawn`), beside the `fold` that already recorded the state's
  type. `spec/error_process_spawn_type_name` pins the diagnostic.
- **The testing DSL is a checker decision.** `describe`/`it`/`before`/
  `after`/`assert` were recognised by spelling in lowering (to `kex_test`) and
  by the walker (public natives, and the `assert` location by name in
  `eval`), so a program's own `assert` ran the prelude's on both backends. The
  checker records a bare call answered by the prelude's declaration as a
  `Builtin` whose owner is `Kex.Intrinsic.Test.<name>` (`names::TestBuiltin`,
  the one table of the names); lowering and the walker read it, and the
  walker's natives are only `Test` intrinsics. An unchecked run (`--no-check`)
  goes by the spelling. `spec/own_assert_shadows_builtin`.
- **Bare `send`/`sendFrom`/`self` are no longer lowered by name.** Lowering
  turned them into `erlang:send` / `erlang:self` itself; a bare `send(pid, m)`
  is the checker's selection of `Pid`'s `send` and lowers as every bare UFCS
  call does, and a bare `self()` is a checker error, so its branch was dead.
- **The `each`/`times` fold reads the target.** Lowering folded any method
  spelled `each` or `times` whose block reassigns an outer variable, so a
  type's own `each` was folded over the type's value as a list and crashed on
  BEAM (`case_clause`). Only a call the checker decided is
  `kex_prelude:each`/`times` is folded now. `spec/own_each_not_lifted`.
- **`Kex.embed`** is spelled once (`names::kEmbedFunction`, with
  `names::kRuntimeRoot`) for the checker and both expander sites.

Left as intrinsic backing, not a second decision: the walker's native
`Server` value answers `within`/`process`/`link`/... in C++, where BEAM runs
the prelude's `make Server<X>` methods. Moving that would change the walker's
`Server` representation, not who decides the call.

Originally: about 183 places in checker, lowerer and walker test a callee name against a
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
- **`send` is an ordinary signature** (2026-11-07): the checker's two
  `send` special cases are deleted; `make Process<X>`'s `send :> X -> Void`
  decides it. What it needed: **a generic several parameters share is one
  type** at a call (`genericConflict` in the full-match test). That was
  missing for every call, also on `main`: `same : A -> A -> Bool` took `1`
  and `"s"`, `put : Box<A> -> A` a `Box<Integer>` and a String, and
  `Process<A>.send` any message. `spec/regex_basics` passed `.or(0)` as a
  `Regex` fallback 23 times; it passes a `Regex` now. A value of a generic
  type written without its arguments (`Ask(Process)`) goes where the type
  with arguments is expected, as an unparameterized parameter already took
  any. **`fromBytes` is ordinary too**: its range check on literal bytes was
  the checker's special case; a literal where a Byte goes is range-checked
  wherever it is passed now (`checkByteLiterals`, also the `let` check's two
  copies), so `g(256)` for `g(b: Byte)` is an error as `let b: Byte = 256`
  was. **`as` is a registry entry** (`{"as", 1, checkAs}` in
  `typechecker_intrinsics.cxx`), not a name test in the method-call path:
  what it checks (a compatible representation, `.as(Atom)` only on a
  literal) is no signature's to say. A deferred `reply` of the wrong type
  is the shared-generic rule's too (`reply : From<A> -> A -> Void`); its
  own check is deleted. Both backends lower and evaluate `.as` from the
  checker's `Conversion` decision (its owner is the target), not by the
  method's name; the walker reads the spelling only with no checked program
  (`compiled do`).

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
- **Guard expansion is an IR pass** (2026-11-07, `src/ir/passes/guards.cxx`,
  first in `standardPasses`): lowering marks a match written in source
  (`GuardExpansion::Source`, with the location its no-clause failure names)
  or a rescue's arms (`Rescue`), and the pass turns `when` guards into
  nested matches and adds the failure. `Lowering::expandGuards` is gone.
  Loop lifting and the IR verifier moved there too (`passes/upgrade.cxx`,
  `passes/verify.cxx`). Operator devirtualization stays in lowering, on
  purpose: it reads the checker's `Operator` decision (`operatorOwner`), and
  an IR pass would have to rediscover the receiver type that decision holds.
- **IR verifier pass** (`src/ir/passes/verify.{hxx,cxx}`, last in `standardPasses`):
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

**Coverage-guided deletion (2026-11-07).** Production lowering always has a
HIR: the separate build checks every unit before lowering it, `--no-check`
and `--emit-core` included, and the REPL and the prelude build lower with
their own check's answers. So a lowering path the BEAM corpus never takes is
one the checker's decisions made dead. Measured with an instrumented build
(`-fprofile-instr-generate`; prelude build, spec-beam, the prelude, stdlib
and example BEAM suites, ctest without `codegen_test`, which lowers with no
HIR): 1,099 of 11,574 lowering lines never ran, 553 of them in
`lowering_method_call.cxx`. Deleted there so far (339 lines): named-argument
placement for externals and providers (placed earlier, from the resolved
target; a call that still reaches them is a `LowerError` now), a record
built through a method name, module functions and exports through a type
name, and the structural-alias provider loop. Two `codegen_test` cases
pinned only those no-HIR paths and are gone. `noClauseMatched`, dead since
the guards pass, is deleted too. A path a program outside the corpus can
still reach is kept (a `let` destructuring a record, error paths).

**What still reaches the cascade (measured 2026-11-07).** Of 50.5k method
calls lowered over the corpus, the resolved-target block (`resolvedCalls`)
takes 41k; 7.7k reach the cascade after it. By the checker's decision:
Dispatch 974, FieldRead 570 (`pos`, `input`, `port`: a record whose layout
lowering does not hold, read through its accessor), Imported 274 (`kind`,
`second`: imported targets the resolved block did not take), Undecided 273
(programs that do not check, lowered under `--emit-core`), OwnMethod 165,
Selected 21, TraitMethod 9, Hidden 3, ModuleFunction 2. The structural
switch is next: lower each of these kinds from its decision before the
cascade (a FieldRead from the interface's layout, a Dispatch to its
dispatcher), then measure the cascade dead again and delete it.
First of these done: a FieldRead of an imported record is read from its
interface's layout (`staticFieldRead`); only a bare name a local record
also declares still goes through the accessor (`localRecordNames`).
A Dispatch call only one imported module provides (at any arity: a unit
compiled apart beside the prelude is routed by the value's owner) and no
local method or function shares is resolved to that module's dispatcher
by the checker (`resolveDispatchProviders`); 43 of 974 calls. The rest have
several real providers (`showValue` 457: every unit with a Showable type).
Choosing among them at run time is the semantics, so the next step is a
checker target that names the providers to dispatch among, which lowering
emits as written; then the search's per-owner, guarded and dual-provider
branches can go.
Measured after it: 615 of the Dispatch calls the search still lowers have
ONE provider at their arity, the prelude, and the search only calls its
dispatcher. The checker cannot take them yet: counted at the call's arity,
`Kex.AST`'s `pattern.to(String)` went to the prelude, because the checker's
interfaces did not hold `Kex.AST`'s `to` while lowering's did (an opt-in
module read from source shows the checker its `make` methods only once a
package approves them). Prerequisite: the checker sees every receiver
function lowering sees; then the arity rule takes these 615 and the
search's single-provider path can go.
Narrowed later the same day: the decision moved to where the scope is live
(`decideDispatch`, the one helper all eight Dispatch sites call; a `make`
of a unit compiled apart counts as a provider only where its module is in
scope, as lowering filters it). What still blocks the 615 (`showValue`
441): the checker's interfaces hold EVERY opt-in stdlib module
(`Kex.Data.Set`'s `showValue`, `Kex.Net.HTTP.Headers`'), linked or not,
while lowering's hold only what this build links. The checker needs the
build's linked-module set to count providers the way lowering does.
Done: the separate build hands each unit's check the modules its lowering
will find providers in (`unitExternalModules`, the one place both read;
`Analyzer::setLinkedModules`), and `decideDispatch` counts only those. A
call with a foul provider at its arity stays the search's (it threads the
capability context). Dispatch calls the provider search lowers: 615 → 31
(`count`, `each`, `get`: foul providers). What the search still lowers:
those 31, 45 OwnMethod (`Kex.Net.Socket`), 5 Imported, 57 from programs
that do not check. The 31 were held back for nothing: their foul
providers are `Mock.Env`'s `make` methods, in the prelude beside the pure
ones, and the call ends in `callE`, which appends the capability context
for a target registered foul at that arity, on the resolved path as on the
search's. Foul providers count as ordinary ones now; Dispatch calls the
search lowers: 31 → 0. Coverage after it: the imported clause chosen by
labels (and by a shorter written arity), the search's label-picked
same-module overload and its read of a resolved target (a resolved call
never reaches the search) were dead; deleted (~110 lines).
Where the search's remaining ~1,900 runs come from: about half (914) is the
prelude build, which lowers against its own names with
`preferExternalReceivers` (`compilePreludeCore`'s self-externals), so the
search is its dispatch by design, and its check has no linked-module set.
By decision: Dispatch 304, OwnMethod 202, Undecided 198 (split-module
copies of nodes the HIR does not index), Imported 178.
Done (2026-11-07): the prelude lowers from its check's decisions with no
externals at all. Its receiver calls take the local make-method path, as a
multi-module unit's do, and `lowerModules` files each with its module.
The self-externals (`sourcePreludeInterfaceNames`, every prelude receiver
name at arities 1–4 pointing at `kex_prelude`) and the search's
`preferExternalReceivers` clause are deleted; the flag that remains,
`lowersPrelude`, only keeps the prelude's own `to` forwarding. Search runs
in the prelude build: 808 → 0; over all of `make test-all`: 351.
Then (same day):
- The REPL's check has the linked set too (`providerModules`, shared with
  the separate build).
- A type of a unit compiled apart that answers a call itself is the target
  of that module's dispatcher (`connection.close`).
- Trait default bodies are checked, with `this` as `This`, and a call on it
  is Dispatch. The prelude verifies with no undecided site, trait bodies
  included (`forEachFunction` walks them).
- A provider declared for a type alias (`FS.Path.join` over `FilePath`) is
  reached only through a static type, never by the value (decided by the
  user, keeping what lowering did): `ImportedFunction::aliasReceiver`, set
  from the prelude's source and from KexI.

- `&.method(args)` is `{ |__shorthand| __shorthand.method(args) }`, as its
  chained form already was: the parser hangs that lambda on the
  `ShorthandLambda` node, which only reflection (`Kex.AST`) still reads as
  written. Every other pass reads the lambda, so the call in it is checked
  and decided like any other; the walker's two shorthand evaluators and
  lowering's own call node for it are gone. The walker now evaluates the
  arguments on each call, as BEAM did.

- Lowering requires the checked program (`LowerContext::hir`; it throws
  without one) and reads every answer of the check from it: the four maps
  `LowerContext` took beside it and `withCheckerAnswers` are gone, as is
  `inheritedDefaults`' own resolution of `implement:` names from the
  scopes' `using`s. The last unchecked lowering was `codegen_test`; its
  fixtures are checked now and lowered against the prelude's externals
  with their own laid over them (`ir::mergeExternalModules`, moved out of
  `main.cxx`), as a build lowers.

- A call several linked modules answer, its receiver known only at run
  time, is the checker's too: the target is the prelude's dispatcher, and
  `ResolvedCallTarget::alternatives` lists the others' providers, each for
  a value of its receiver type (a record or a primitive, which a guard can
  test) or, for a module's dispatcher over any receiver, for a value that
  module owns. Lowering emits them as written (`providerChoice`). The
  search's `runtimeProviders` branch, which made that choice, had no
  caller left and is deleted (−40 lines). Devirtualizations unchanged
  (1,248, 114 spellings). A provider declared for a union
  (`make TypeRef | PatternRef`, `Kex.AST`'s `to`) is chosen by the value's
  owner too.
- A trait default a type of a unit compiled apart inherits (`set.any?`,
  Foldable's) is the target of the type's home module, which forwards it
  under the method's own name; the search guarded it beside the prelude's.
- A constant a `using` imports from a unit compiled apart (`STATUSES`) is
  a constant to the check (`constantType`), not a module: `STATUSES.map`
  was checked as a call into a module of that name, decided nothing, and
  left the call to the search. Search runs: 47 → 26.
- A method call the check rejects, though its receiver is one the method
  takes (a truthy block for `filter`), is lowered only where the check does
  not gate the build (`--no-check`), and there the value decides: it is
  Dispatch. A receiver no candidate takes (`"a b".whiteSpaces`) keeps the
  search's failure, which names the call's location.
- Lowering no longer sets aside the checker's target for a call on `this`
  (a rule from kexhq/kex#143 that made every `this.x` the search's). A
  call on a trait's `This` is the type claiming it, in the copy of the
  default lowered for that type: Dispatch, never an import's target
  (`Repeatable`'s `this.repeat` beside the prelude's Monoid `repeat`).
- A sized number claims what its family's `make` claims (`make Float,
  implement: Blankable` covers a Float64): `1.5.present?` found no
  candidate, its implementor key being `Float64`. One `familyKey` serves
  the list, map and optional families too.
- A rejected call whose candidates are all records' methods, on a receiver
  that is no such record (`"a b".whiteSpaces`), is `Hidden`: no value
  reaches one, and both backends raise the undefined method where it is
  written (the walker naming the receiver as BEAM does,
  `Result<Date, ?>`, not its constructor).
- **The provider search is gone** (204 lines → a 40-line fallback). A call
  the checker left to the value with no target goes to the one module
  answering it at that arity, else to the prelude's dispatcher; choosing
  among several modules' providers is the checker's alone (its
  alternatives). `lowering_method_call.cxx`: 1,558 → 1,396 lines.
- `dualProviderDispatch` is deleted (−171 lines with `methodReceiverOwner`):
  its last caller, the local make-method path, guarded the unit's own type
  ahead of the prelude, which the unit's own dispatcher already does;
  `make test-all` passed with it switched off. Left of the old choices:
  `importedFreeFunctionDispatch` (one spec), a run-time choice between an
  imported free function and records' methods, for the checker's
  alternatives next.
- Coverage after it (`make test-all` on an instrumented build): the prelude's
  split-`to` fallback clause, the argument dispatchers' single-module and
  `Prelude`-export fallbacks, `externalPrefixCall`'s single-module loop and
  `moduleInScope` (whose one caller's filter the checker's `Hidden` decides
  first) never ran, nor did either branch for a `compiled do` block, which
  is expanded before the check; deleted (−113 lines).
- `importedFreeFunctionDispatch` is deleted: the checker decides that choice
  (`importedFunctionChoice`). A free function a `using` brings in beside
  records' own methods of the name, on a receiver of unknown type, is the
  target, each record's method an alternative for a value of its type.
  Alternatives and targets may name this unit's functions as lowering emits
  them (`Dsl.get`, a record's bare dispatcher, no module), which lowering
  calls locally, with a foul one's capability context. The 20 spellings of
  a module's BEAM atom (`"Kex." + path`) are one `names::moduleAtom`.
- Step 6: the walker's run-time `using` gates are deleted
  (`makeMethodInScope`, `makeDefInScope`, `moduleInScope` and the two maps
  they read, −86 lines). The check decides what a call out of scope reaches
  (`Hidden`); with the gates off, the walker spec suites and the REPL and
  interpreter tests were unchanged.
- Step 7, first increments (2026-11-08):
  - The name check keeps what it finds (`NameIndex`, Analyzer::nameIndex):
    the program's symbols, the references to them and the names in scope
    at each point, recorded through `ProgramModules::referenced` (by name).
  - SemanticDB's queries are one implementation over a list of indexed
    files (`symbol_queries`): `findSymbol`, `symbolInModule`,
    `receiverSymbol`, `symbolAt`, `completionsFor`/`At`.
  - Completion reads the checker: `completeAt` takes a check of the buffer
    (`CompletionChecker`, each caller's own pipeline) and a base of names
    (the prelude's sources, `symbolsOfFiles`). Both REPLs, the wasm REPL
    and `kex --complete` hold no SemanticDB; their per-input `<repl>`
    re-indexing is gone, the session's definitions being in what completion
    checks.
  - The language server resolves an open document once, by the checker:
    SemanticDB indexes it without its own pass and takes the checker's
    `NameIndex` (`setResolved`), and loads the modules the analysis names
    (`referencedModules`) for navigation. SemanticDB's export entries are
    dropped by address, not by file (`updateFile`, `setResolved`,
    `removeFile`, which read them after freeing the file).
  - The workspace reference index resolves through the checker's name pass
    too: its files are indexed without SemanticDB's own, the modules their
    analyses name are loaded, and `resolveTogether` resolves them against
    every indexed file's symbols at once, so a reference in one file still
    lands on a declaration in another. The prelude's and receiver
    recovery's loads no longer resolve: nothing in the compiler, the REPLs
    or the language server runs SemanticDB's own resolve mode.
  - SemanticDB's own resolve mode is deleted: `ResolvePass::run(SemanticDB&)`,
    `IndexedModules`, `updateFile`'s resolve and its re-entrancy guard,
    `CollectPass::run`, `resolveSourceModulePath`, and the module-export map
    and shadowed-path table nothing read once it went (−222 lines). One
    resolver is left, the checker's name pass (`ResolveOptions`; a
    workspace's files keep their top-level declarations to themselves,
    `fileLocalTopLevel`). Its tests resolve through `resolveTogether`. The
    shadowed-module warning went with it: only that mode reported it, and
    the language server has published the checker's diagnostics since
    2026-11-07.
  - SemanticDB is now a document store, a module loader and the symbol
    queries' front end over the checker's index.
- Steps 5–6, HIR nodes (started 2026-11-08, by the user's choice to do the
  rewrite): a signature made from a FunctionDef keeps its clause
  (`Signature::clause`), the HIR answers the selected signature by call
  node, and lowering places a labelled call's arguments into the clause the
  checker chose rather than choosing one by labels itself (`namedClause`
  and `selectClause` are deleted). `makeMethodFor` takes the method
  whose clause the checker selected; only the extension entry (the first
  module in path order) is still lowering's. `trailingMakeDefaults` and
  `methodDefaults` are deleted: with the selected clause placing every
  positional call that leaves defaults out, neither padded a call in
  `make test-all`.
  - Not yet: `ownedMakeFunction` (does the receiver's own make function
    take the capability context). Over the spec suites the checker
    selects no clause for 86 of the 143 calls it answers, 12 of them to a
    `foul` function. Recording `ownTypeAnswers`' one declared signature
    as the selection, and linking `:>`-annotated methods to their clause,
    covered one more. The rest need checker work: inherited trait
    defaults (`describe`, `passing?`), `this.helper(x)` with an untyped
    argument left to Dispatch (`Mock.Files.cannedRead`, `contains?`),
    Undecided private helpers (`Measure.productKind`), and stdlib types
    checked from their interface, whose signatures carry no clause.
  - A namespaced call into this unit's own module (`Util.double(21)`)
    reaches the function the checker named (`checkedCallModule`: the
    selected signature's scope, the owner of a Dispatch or value call,
    the module of a constructor's type) instead of trying each module
    path candidate. A re-export (`Toolkit.Numbers.double`) is the function
    itself. The same answer (with an Imported target's selected signature)
    serves calls into units compiled apart and the capability check, so
    `modulePathCandidates` and `capabilityModuleNamed` are deleted: lowering
    searches no module paths.
  - Defaults across units (decided 2026-11-08, the user's choice; defaults
    are pure): the callee fills its own. A caller compiled apart passes
    the sentinel atom `'$kex_default'` (`names::kDefaultArgument`) for
    each argument it leaves out, and the function rebinds each defaulted
    parameter on entry, the default lowered in its own scope and already
    checked; a type guard on such a parameter accepts the sentinel. A
    record from another unit takes each field it leaves out from its
    module's `__kex_record_default(Tag, Field)`, which every module that
    declares a record with defaults now has (it was served records only).
    Lowering parses nothing any more: `importedDefault`, its cache, and
    `lowerFieldDefault`'s guess at short module names are deleted. The
    interface keeps the default text only as "has a default".
  - A call the checker resolved is emitted as resolved even when some
    record has a field of the same name (`localFieldAccessor` and
    `resolvedApart` are deleted). Over the spec suites the override threw
    the checker's target away 263 times, nearly all on receivers whose type
    it knew (`2.seconds` on Integer, `measure.kind`); the erased receiver it
    was written for (`opaque(bag).entries`) the checker already decides as
    Dispatch. Devirtualized calls rose 1250 → 1315.
    Likewise a type of this program with a static function of the same
    name (`localTypeShadows`) and a dotted module path naming one of this
    unit's functions (`modulePathOwnsCall`) no longer override a resolved
    target: a call the checker resolved is emitted as resolved.
  - A field read the checker decided reads its slot unless an interface
    lays the same record name out differently (`conflictingRecordLayouts`;
    the same layout seen twice is one record), with the checker's name
    canonicalized (`Port` is `Net.Port`). Field reads reaching the
    local-method cascade fell 128 → 5 (a conflicting `ParseError`,
    structural records).
  - The walker's last re-derivation (`showErasedArgument` reading an
    argument's static type) is the checker's answer now
    (`shownThroughDistinct`, persisted with the HIR): walker-rederives 0.
  - The HIR's node numbering (and every "body that runs" walk) now covers
    parameter and record-field defaults (`forEachBodyExpr`, over the new
    `forEachDeclared`), so a persisted HIR keeps their decisions.

Search runs over `make test-all`: 351 → 47, none of them unchecked. What
is left: the owner choice for `Kex.AST`'s `to` (4), `any?`/`all?` decided
as the receiver's own method but guarded by the search's dual-provider
path (12), and calls the checker leaves undecided (about 25: bare function
values such as `~digit?`, for which lowering still builds its own call
node, among them).
The cascade's traffic halved (7.7k → 4.3k calls), and
coverage then showed the `externalImports` path (a `using`-imported
function of a unit compiled apart), the imported clause's unplaced
named-argument path and its two-provider dispatch dead; they are deleted,
with `importOwnsLocalReceiver` and `importSourceModule`, which only they
called (~130 lines).

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

**Step 1, module loads in a checked run (2026-11-09).** A trace of every
module the walker loads at run time over spec, spec/prelude and examples:
in checked runs the project modules a program uses are merged and checked
before it runs (`resolveBeamDeps`), and the only loads were the namespace
pre-check probing source files for a dotted receiver. `Console.RESET` (a
constant) found `console.kex` and ran it again, from source and unchecked,
at each use; `Culture.DEFAULT`, `Kex.BACKEND` and
`Presence.Statuses.STATUSES` likewise. In a checked run the probe now runs
only for a receiver the checker decided is a module (`ModuleReference`).
What a checked run still loads: an `export`ed module nothing uses
(`Tariff`), none of whose code runs. Every other load is in an evaluator
with no HIR (`compiled do`, collapse, template scans, the stdlib interface
expansion: `Culture` 803 times over the corpus), which is step 2.

**Step 2, compile-time code checked before it runs (2026-11-09).** The
checker never saw a `compiled do` block: it skipped them, and expansion
removes them before the main check. `hir::checkCompileTime`
(`src/hir/compile_time.{hxx,cxx}`) lends a program's blocks to it as the items
they read as (a top-level block's declarations as top-level declarations and
each statement as a top-level statement, a synthetic `main`; a module's
block's statements as one function body of the module), checks silently, and
gives them back by node, so the answers hold for the sandbox. A `let %name`
template is typed by its computed name only; its body is checked as the
declaration it becomes. Expansion checks before running a block and before
collapsing (once, when the collapse sandbox starts), given the prelude's
interfaces (`ExpandOptions::interfaces`, set by every `main.cxx` expansion).
Over `spec/compiled_generate.kex` with a warm cache, every method call the
sandbox runs now has the checker's answer. What still runs unchecked: modules
a sandbox loads from source (`Template` for a template scan, `Culture`,
`FS`), the stdlib interface build on a cold cache (which has no interfaces
yet), and the LSP's and completion's expansions, which pass none.

Then the rest of step 2, the same day. An evaluator is built with the
prelude's interfaces (`Evaluator(interfaces)`; the CLI run, both REPLs,
every expansion sandbox, the tag validator), and a module it loads at run time
is checked as its own unit (`checkCompileTime`, cached per process beside its
expansion) and read like the entry program; its own expansion is given them
too. Constructing with them matters: the prelude's own load reaches `FS`
(`mock.kex` names `FS.FilePath`). A tag validator's companion runs on the
program's analysis, or on its module's check. The LSP, `SemanticDB` and the
wasm REPL expand with them. Over spec, spec/prelude and examples, every module
the walker loads is now checked; the one exception is `FS` while the walker
prelude is first built on a cold cache. `DateFormat` names `Culture.Culture`
instead of `using Culture`: the walker runs a `using` eagerly, so that line
loaded `culture.kex` from source on every evaluator's prelude load. The
shared `ast::heldNode` finds a lent item by node.

**Step 3 begun, and every walker entry through one driver (2026-11-09).**
The walker's method call starts with a switch on the target kind:
`Conversion` (`evalConversion`) and a `Builtin` runtime primitive
(`evalIntrinsicCall`, the category from the decided path) no longer fall
back to reading `.as(T)` or `Kex.Intrinsic.*` by their spelling. For that to
hold, every program the walker runs has to be checked, embedders included,
and checked with the project modules it uses: checked alone,
`examples/parser_combinators` read its own `Parsing` as the stdlib's. The
merge `kex -R` did in `main.cxx` (`resolveBeamDeps`) is now
`src/driver/dependencies.{hxx,cxx}`: `loadDependencies` (each file parsed and
expanded on its own, the discovery cache, build progress) and
`mergeDependencies`, with the failure policy, scan caches and unit graph;
`main.cxx`'s `resolveBeamDeps` composes them around the per-unit BEAM check.
`driver::prepareForWalker` (merge, expand, check silently) is what a harness
and `Evaluator.run` use; the evaluator never checks on its own.
`Evaluator()` takes the process's prelude interfaces
(`kex::defaultPreludeInterfaces`, `src/common/runtime_dir.{hxx,cxx}`, where
`prebuiltRuntimeBeamDir` moved from `main.cxx`). Running checked exposed a
walker bug the BEAM-only specs had hidden: a `Task` value had no dispatch
type, so `task.await(timeout: …)` found no clause. A decided `OwnMethod` call goes straight to that
type's implementation (`ownMethodName`), skipping the run-time lookup. The
hidden trait dictionary is called only for a call the checker decided
`TraitMethod`, not for any receiver whose name has one in scope. A
labelled call reaches the module function the checker selected
(`declaringModule` of its signature); `findImportedNamedOverload`, which
worked it out again from the walker's own `using` scopes, is deleted. In a
decided call, a capitalised receiver the checker typed is a value (a
constructor, a constant); only an untyped one goes through namespace
detection. One rule, `Evaluator::decidedFunction`, names the function a
decided call reaches. Routing every unlabelled call to its selected module
function: its first try failed 15 capability and mock specs, the
clock-mocking `time` specs and `wc`, because a `with` replacement of a
capability (`Clock.now` under `with Clock = …`) is applied on the run-time
path. As lowering's `capabilityDispatchLowered` asks the context first, a
decided module function is now called directly unless its module is a
capability a `with` replaced in scope (`replacedCapability`); then the
A receiver function the checker chose by the receiver's type
(`receiverTypeOf`: the prelude's `List.map` for a list) goes to that type's
implementation too, and the after-the-fact `receiverTypeOf` correction of the
run-time lookup is deleted, unreachable once that rule exists. What still goes
by the value's run-time type: calls the checker left to the value
(`Dispatch`) and code run with no answers.

**Stage 4 closed (2026-11-09).** Compile-time evaluation runs with the
checker's answers (step 2), and "the walker evaluates HIR nodes" (order step 6)
means, under the decision that the HIR is the AST plus those answers, that it
reads the decided target wherever one exists (step 3). `resolveMethodName`
remains for what BEAM also decides at run time: calls the checker left to the
value (`Dispatch`) and the prelude's receiver functions, whose `Imported`
target on BEAM is the prelude's dispatcher.


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
- **The LSP's diagnostics are the one sema's** (done 2026-11-07): the parser's
  and expansion's (`FileState::sourceDiagnostics`), then one Analyzer with the
  name check for the document and the module locator, as `-C` runs. Its
  SYMBOLS still come from SemanticDB (`findSymbol`, `receiverSymbol`,
  `symbolInModule`, `symbolAt`, completion), not the checker's.
- **`kex -C` is on the one sema** (done 2026-11-07): `checkEntry` configures
  its Analyzer as `runSemanticCheck` does (`configureFullCheck`) and reports
  the parser's diagnostics itself, which SemanticDB's re-parse used to.
  One golden changed, to the run path's wording
  (`error_using_alias_imports_nothing`: "... is not imported here").

## Stage 6: delete and measure

Remove the old paths (the merged build, the old lowering, SemanticDB); keep
`docs/architecture.md` current. Final
report against the Stage 0 baseline: `src/` line delta, clean and incremental
C++ build time, dup-audit counts, largest function, link time.

**Started (2026-11-05).** `docs/architecture.md` describes the passes as they
are now: the driver pipeline, one sema (names, then the Analyzer), the checked
program, and both backends on it. It lists what is not on the HIR yet. The
final measurement waits for the old paths to go.

The user's decisions of 2026-11-05 (separate compilation as the default; the
`src/`-below-`main` goal, revised on 2026-11-08 to a reported measurement)
are under Decisions; node identities, which came next, are
done (Stage 2).

**Stage 4 measurement (2026-11-09), against `main` (643e509).** Slick, median
of 5 interleaved runs (cold 3), a `tey` built from this branch (the dev
`tey-run` of Oct 29 cannot read slick's `package "slick" do` manifest):

| scenario    | main   | branch | change |
|-------------|--------|--------|--------|
| cold        | 26.11  | 38.48  | +47%   |
| no-op       | 10.59  | 9.20   | −13%   |
| incremental | 11.41  | 12.83  | +12%   |
| devloop     | 10.57  | 9.57   | −10%   |
| specs       | 113.93 | 106.64 | −6%    |

Every slick spec run passed. The checks that make the walker run on the
checker's answers (Stage 4, step 2) cost compile time: against the committed
1625ec2, one incremental build is 8.6 s → 12.9 s, with dependency expansion
2.8 → 5.4 s, the 47 compile-time sandboxes' start-up 0.4 → 1.6 s and
evaluation 1.8 → 3.5 s, and the entry's expansion 1.1 → 2.8 s. The check
before collapse alone is about 2.3 s of it (six repository and view files).
None of these checks is cached across processes yet: a module a sandbox
loads (harmat's) is checked again by every build, and the program checked
before collapse is checked even though the build checks it again right
after; the walker's answer lookup also walks every checked module. `src/`
(`.cxx`/`.hxx`): 94,499 lines against `main`'s 89,951 (+4,548, +5.1%),
reported, not a gate.

Then, the same day: the checks are cached across processes
(`hir::cachedCheck`, through `hir::persist`): a module the walker loads is
keyed by its source and what its expansion read, compile-time code by a digest
of its tree (`programDigest`, the `--emit-ast` term) before its blocks are
lent; and `hir::ProgramSet` indexes which program answers for each node, so a
lookup asks that one and the prelude instead of every checked module. Slick
again, same setup:

| scenario    | main   | branch | change |
|-------------|--------|--------|--------|
| cold        | 26.02  | 40.07  | +54%   |
| no-op       | 10.28  | 7.96   | −23%   |
| incremental | 11.03  | 10.23  | −7%    |
| devloop     | 10.32  | 8.39   | −19%   |
| specs       | 113.31 | 98.63  | −13%   |

Every warm scenario is faster than `main` again; a cold build pays every check
once, plus writing it to the cache (+25% before Stage 4's checks). Against
1625ec2 an incremental build is 8.6 s → 9.5 s: reading a cached
compile-time check back (digest, node order, answers) is about 30 ms each.

Where a cold build goes (one run each, KEX_TIMINGS): 1625ec2 33.4 s, branch
40.5 s. Expanding the entry 1.1 → 4.0 s: in a build the entry is expanded with
every dependency merged in, so the check before its collapse checks all 160
files' bodies again (each was expanded, and collapsed, on its own already).
The 88 sandboxes: start-up 0.6 → 2.2 s, evaluation 14.5 → 17.3 s (checking
compile-time code and loaded modules, all first seen on a cold build).

That expansion of the merged entry turned out to be dead work: the build
checks and lowers unit by unit inside the `dependencies` pass
(`checkUnitsSeparately`), every dependency was expanded on its own as it
loaded, the entry before the pipeline started, and no pass after `expand` read
the merged program. The `expand` pass is deleted from the compile pipeline
(`docs/architecture.md`). Slick, one run each: cold 40.5 → 36.5 s (1625ec2:
33.4 s), incremental 9.5 → 8.6 s (1625ec2: 8.6 s). What cold still pays is
the sandboxes' first checks (start-up 0.6 → 2.2 s, evaluation 14.5 → 17.3 s):
a file's compile-time code runs before its unit can be checked, so the unit
checks cannot stand in for them.

Final slick measurement against `main` (643e509), median of 5 interleaved
runs (cold 3), every spec run passing:

| scenario    | main   | branch | change |
|-------------|--------|--------|--------|
| cold        | 25.85  | 36.41  | +41%   |
| no-op       | 10.57  | 7.28   | −31%   |
| incremental | 11.35  | 8.56   | −25%   |
| devloop     | 10.60  | 7.61   | −28%   |
| specs       | 113.79 | 91.72  | −19%   |

**Dead code (2026-11-09).** Linker reachability (`kex` relinked with every
library object forced in and `-dead_strip`), clang's unused-code warnings and
a header-caller count found no function in `src/` that nothing uses; ten are
reached only from tests. Coverage of the whole suite (an instrumented build,
`make test-all`) found 191 of 3,725 functions and 5,316 of 79,718 lines never
run; the named functions among them with no caller were deleted
(`hir::Program::ast`/`analysis`, `Fiber::finished`, `ReturnException::frame`,
`RuntimeError::location`, `Lender::lentAny`). Three never-run lowering
branches were traced through a full slick build as well: the capability
dispatch of a module compiled apart and `Type.method` static dispatch in
`lowerTypeNamespaceCall` never ran and are deleted (with `implicitThisMethods`,
which only the latter read, and `receiverPattern`'s copy of
`ast::takesReceiverPattern`); the `Mod.Tag` branch is live in slick
(`Grant.Admin`) and untested. Specifying it
(`spec/module_qualified_nullary_tag.kex`) found that the checker left
`Roles.Admin` undecided where `Roles` names a module through `using` and a
record of that module, so the walker failed with "'Admin' is not callable"
(as at 1625ec2); the checker now resolves the written path through the active
imports, the walker evaluates a decided constructor, and a namespace call goes
to the module function the checker selected.

Stage 6 closed (2026-11-09, by the user): the cold cost is accepted as the
price of checking every program the walker runs, paid once per toolchain or
cache wipe; every warm scenario is faster than `main`.

**Step 10 measurement (2026-11-08), against `main` (3846781).** After the
provider search and SemanticDB's own resolver went (`tools/bench-slick.sh`,
`BENCH_KEX="main=… branch=…"`, median of 3 interleaved runs, cold 2, the dev
`tey`):

| scenario    | main   | branch | change |
|-------------|--------|--------|--------|
| cold        | 24.43  | 30.49  | +25%   |
| no-op       | 9.94   | 7.32   | −26%   |
| incremental | 10.68  | 7.89   | −26%   |
| devloop     | 9.69   | 7.19   | −26%   |
| specs       | 100.93 | 85.97  | −15%   |

Unchanged from the milestone below: the step-5 and step-7 work moved no
build time. The cold cost is still the separate build's (336 modules
compiled against `main`'s 188). Every slick spec run passed. `src/`
(`.cxx`/`.hxx`): 93,651 lines against `main`'s 89,766 (+3,885, +4.3%),
reported, not a gate (Decisions, 2026-11-08).

**Milestone gate (2026-11-07), against `main` (3846781).** Slick, median of
3 interleaved runs (cold 2), the dev `tey` (`~/.local/bin/tey`: brew's
0.2.0 cannot parse slick's `package.kex`):

| scenario    | main   | branch | change |
|-------------|--------|--------|--------|
| cold        | 24.31  | 30.40  | +25%   |
| no-op       | 9.85   | 7.39   | −25%   |
| incremental | 10.44  | 7.94   | −24%   |

The cold cost is the separate build's (step 1: every unit checked and
lowered apart, 336 modules compiled against 188), not this milestone's:
`91f1f4a`, where it became the default, builds slick cold in 31.9 s, the
branch now in 31.6 s. The gate found two regressions, both fixed with specs:
a module's own nullary constructor read as a same-named record elsewhere
(slick's `UserDetail.Name` beside `Net.DNS.Name`;
`spec/constructor_beside_same_named_record`, which also fixed BEAM lowering
the same way, broken on `main` too), and `Type.of` of a type value (harmat's
`first(as: T)` then `Type.of(as)`), which now answers the type the value is
(`spec/type_value_record`). Slick's eight spec files pass on the branch.

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

### `main.cxx` split (by decision, 2026-11-09)

`main.cxx` keeps option parsing and command dispatch; the rest moved out in
gated steps:
1. `src/repl/`: `input.{hxx,cxx}` (line editing, history, script input) and
   `session.{hxx,cxx}` (the walker and BEAM REPLs).
2. `src/driver/`: `separate_build.cxx` (the build unit by unit),
   `toolchain.cxx` (erl/erlc, the module and run caches) and
   `prelude_build.cxx` (`--build-prelude`), declared in `build_support.hxx`.
3. `src/driver/compile.{hxx,cxx}` (the compile pipeline, `compileToBeam`,
   `runCompiledBeam`, `printCompilePipeline`) and `options.hxx` (`Mode`,
   `Options`, `ExpandProgram`).
4. `src/driver/program_support.cxx`: what every command does with a program
   before building or running it (a spec's base merged in, `moduleRootsFor`,
   `runSemanticCheck` and the check helpers, `printAst`).

`main.cxx` is now 1,292 lines, under the ~1,500 target: 6,596 at `HEAD`,
8,353 on `main`.

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
