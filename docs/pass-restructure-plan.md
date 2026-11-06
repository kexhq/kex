# Restructuring the compiler into passes

The plan for the `refactor/kex-bloat` branch: a smaller, DRY compiler with one
semantic analysis whose decisions every backend follows, built as real passes.
This file is the plan of record and its status; update it as stages land.

Last updated: 2026-11-05.

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
  green. The two build paths (merged and separate) are the one place where an
  old path still stands beside the new one.
- **Separate compilation is the default build** (2026-11-05, by the user). Each
  unit is checked and lowered against the interfaces of the units below it;
  unchanged units come from the unit cache. The merged build retires. The
  point is incremental builds; the measure is slick's incremental scenario.
- **Receiver devirtualization** follows what the emitter records: each
  receiver method's per-type implementation and the dispatcher routes
  (`KexiMethod::implementation`). The old 510-line proof predicate is down to
  a few clauses over that record; the rest goes with Stage 3.
- **Visibility is decided at the declaration.** Whether a `make` targets a type
  its module declares is recorded in its interface (`KexiMethod::
  foreignTarget`); consumers read it, never re-derive it from names.
- **`src/` ends below `main`'s 89,708 lines** (2026-11-05, by the user): a hard
  goal, not only a reported delta.
- **Pseudo-functions** (2026-11-04): one intrinsic registry plus `Type<T>`
  signatures instead of `name == "..."` branches per subsystem (Stage 2b).
  **Deferred** until the HIR work is done (order step 9).
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
| 2b | Intrinsic registry + `Type<T>` signatures | Started, then **deferred** |
| 3 | Lowering only translates; IR passes | **In progress**: lowering re-derives no call (metric 0); still walks the AST |
| 4 | Walker on the HIR | **In progress**: runs, REPL and prelude read checked decisions; still evaluates the AST |
| 5 | LSP / SemanticDB on the same sema | **In progress**: the checked run has no SemanticDB; the LSP still reads it |
| 6 | Delete old paths, measure | **Started**: `docs/architecture.md`; measurements below |
| — | Separate compilation as the default | **In progress**: spec-beam 586/586 under it; not yet the default |

## Order of work (decided 2026-11-05)

All of these are part of the refactor; none is optional. In order:

1. **Separate compilation is the default build.** Today a separate build also
   runs the merged analysis (`passCheck` analyses the whole merged program
   after `checkUnitsSeparately`), so it does more work, not less. The separate
   check must decide the build alone. Then: slick builds and `tey test`
   passes under it, and the incremental scenario is measured against `main`.
   The unit cache already skips check and lowering for a unit whose sources
   and imported interfaces are unchanged.
2. **Delete the merged build path**, with the lowerer's same-unit scoping
   rules (`moduleScopedMethods`, `gatedOut`, `gatedModuleTaking`). One build
   path is left for the HIR to serve. Step 3's error spec
   (`error_measure_to_without_unit_module`) must keep passing through it.
3. **The `using` gate is a checker decision.** A call that only a hidden
   `make` of an out-of-scope module takes gets a target saying so; lowering
   emits it and the walker raises it. No backend guesses the module from type
   names. Visibility already comes from the declaration
   (`KexiMethod::foreignTarget`).
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
9. **Stage 2b:** the intrinsic registry and `Type<T>` signatures.
10. **Stage 6: measure** against the Stage 0 baseline; `src/` below `main`.

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
- **Left for step 1:** units whose sources read outside
  files (`compiled do`, embeds, `Template`) are never cached, because the key
  does not cover those reads. Recording the reads in the key is the next
  speed-up. `separate_check_cli_test` and `run_cache_cli_test` pin today's
  opt-in flags and change with the flip.

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
  `runBuiltProgram`, `preCheckWithDependencies`, `copyRuntimeBeams`); both REPLs
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
- Lowering reads `FieldRead` decisions (`LowerContext::decidedCalls`,
  `staticFieldRead`).
- Bug fix: a `make`-block method beside a same-named prelude method no longer
  commits to the import for an untyped receiver (false type error; BEAM called
  the wrong function). Spec: `spec/make_method_beside_prelude_import_beam.kex`.
- Two candidates for different concrete receiver types with an untyped
  receiver now record `Dispatch` instead of picking the first.

Coverage (call sites with no decision):

| Corpus | Start | Now |
|---|---|---|
| `spec/` | 4,200 (12%) | 572 (1.5%) |
| `examples/` | 1,333 | 296 |

What remains undecided: calls into modules not in scope (`found.get` on a Regex
match), receivers typed by patterns, error-test specs (correctly undecided),
and a long tail.

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
`canonicalRecordName`/`registerModuleAliases`/`using` chain). The coverage
figures above predate the owner decisions; re-measure before relying on them.

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
- **`Type<T>` arguments** (decided, not started): a type-name expression has
  type `Type<X>`; a `Type<T>` parameter binds `T`. Then
  `to : A -> Type<T> -> T?`, `as : A -> Type<T> -> T`,
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
identically. What still builds an evaluator without a HIR: the walker REPL,
the wasm REPL and compile-time evaluation (`compiled do`). Until they have
one, the walker keeps its run-time lookup as their fallback.

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
- `make bench-slick` for performance claims; slick must build and its
  `tey test` pass.
- `make test-wasm` in CI for changes to the driver or the stdlib loader.
- New specs at the top of `spec/` need a `.expected` golden file
  (`make spec-orphans`); `describe`/`it` suites go under `spec/stdlib/`.

## Ground rules

- Never commit or push from the agent; changes stay in the working tree/index.
- No Python, JavaScript or TypeScript in the repository.
- C++20, `.hxx`/`.cxx`, camelCase functions, `m_member`, trailing return types,
  namespace `kex`; enums PascalCase.
- Find duplication proactively; prefer shared helpers and enums over copies and
  magic strings.
