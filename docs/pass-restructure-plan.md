# Restructuring the compiler into passes

The plan for the `refactor/kex-bloat` branch: a smaller, DRY compiler with one
semantic analysis whose decisions every backend follows, built as real passes.
This file is the plan of record and its status; update it as stages land.

Last updated: 2026-11-04.

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
- **Delivery:** strangler. The new path is built beside the old one, every
  stage gated green; then the default flips and the old path is deleted.
- **Receiver devirtualization:** redone as a small IR pass over names the
  emitter records, replacing the 510-line proof predicate.
- **Pseudo-functions** (2026-11-04): one intrinsic registry plus `Type<T>`
  signatures instead of `name == "..."` branches per subsystem (Stage 2b).
  **Deferred** until the HIR work is done.
- **Breaking changes are fine** pre-stability (e.g. `--no-check` still runs
  resolution).

## Target architecture

```
driver/  Command + Options (parsed once) → Session → PassManager
front    Lex → Parse → AST → compiled-expand (AST→AST)
sema     Collect → Resolve → TypeCheck → Select → HIR   (+ hir::verify)
         └─ also feeds LSP / SemanticDB (one resolver, not three)
mid      HIR ──► Walker (evaluates HIR)
         HIR ──► Lower (mechanical HIR→IR, no name resolution)
ir       IR passes: guard expansion, rescue guards, loop lifting,
         op devirt, receiver devirt — each a named, timed pass
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
| 1 | Foundations (cache, enums, shared walkers, audit, `main()` split) | Done, with gaps listed below |
| 2 | One sema decides every call; HIR beside the old path | **In progress** |
| 2b | Intrinsic registry + `Type<T>` signatures | Started, then **deferred** |
| 3 | Lowering only translates; IR passes; receiver devirt pass | **In progress** |
| 4 | Walker on the HIR | Not started |
| 5 | LSP / SemanticDB on the same sema | Not started |
| 6 | Delete old paths, measure, `docs/architecture.md` | Not started |

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

Not done (deliberately skipped or still open):
- `src/driver/` directory with one file per command — commands are functions
  in `main.cxx` instead.
- `PassManager` / `Pass` interface — only `PhaseTimer` exists.
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

Next:
1. Fix the Stage 3 blocker (below) — the checker's decisions must be right for
   the merged prelude too.
2. Close the remaining undecided kinds until `--verify-hir` is clean on
   `spec/`, `examples/`, the stdlib and slick.
3. Introduce the HIR node types themselves (`src/hir/hir.hxx`) carrying type +
   target per node, built from the checker's tables, behind `KEX_PIPELINE=hir`.
4. One resolver (`sema/resolve`) built from `ResolvePass`, the checker's
   `resolveModulePath`/`resolveRecordName`/`resolveTypeExpr`, and the
   lowerer's `canonicalRecordName`/`registerModuleAliases`/`using` chain.

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
`expressionTypes->find`): 23 → **22**, may only fall.

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

Worklist — measured by disabling lowering's type map entirely
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
  ~500 lines of tests). `make test-devirt` stays as the check.
- Flip the default once spec-beam, fuzz and slick are green under
  `KEX_PIPELINE=hir`; then delete `lower.cxx`, `lowering_*.cxx`,
  `lowering.hxx` and the side tables.

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

`Evaluator::eval(const hir::Expr&)`; delete the walker's own resolution
(`traitIdentityOf`, `unknownNamedArgument`, `makeMethodInScope`, the "mirrors
the lowerer" sites). The differential fuzzer keeps comparing walker and BEAM.

## Stage 5: LSP and SemanticDB on the same sema

`collect_pass` / `resolve_pass` fold into the resolver; LSP completion and
references read the symbol table.

## Stage 6: delete and measure

Remove the old paths and `KEX_PIPELINE`; write `docs/architecture.md`. Final
report against the Stage 0 baseline: `src/` line delta, clean and incremental
C++ build time, dup-audit counts, largest function, link time.

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
