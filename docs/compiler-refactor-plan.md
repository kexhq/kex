# Kex Compiler Refactor Plan

> **Not done.** "All phases landed" below is wrong: see the ordered review
> findings at the top of `docs/pass-restructure-plan.md` (2026-11-11).

Worktree: `refactor/kex-bloat` (off `main`). Goal: reduce bloat, cut bug surface,
and speed up both kex compile times and the runtime performance of emitted
programs. Work is committed incrementally; nothing is pushed.

Phases are ordered: measurement foundation first, then small in-flight work,
then the bug-surface core, then performance polish. (An earlier revision
claimed plain "bug-surface first" while scheduling Phase 1's build-time work
ahead of 3a/3b; the principle now matches the roadmap.)

Old-numbered phases moved around (see roadmap below); per-phase history is
preserved inside each section.

## Roadmap order (remaining work)

The phases below landed, but the refactor is not finished. The open work
is the ordered list at the top of `docs/pass-restructure-plan.md`.

0. **Phase 0b — `make bench`** (done).
1. **Phase 1/3a/3b — done.**
2. **Phase 2** — name-resolution core (done, steps 1–6 migrated, gated).
3. **Phase 3c** — SAX reader + block-read fix (done); **3d** — deferred
   (overlap ceiling ~1 ms, documented in its section); **Phase 4** —
   receiver devirtualization (landed, see its section).

Risk-register item 1 (walker↔BEAM `List.first` divergence) is fixed in
the working tree (spec + targeted fuzzer case green; filed as
kexhq/kex#626); it no longer blocks Phase 4's receiver devirtualization.

---

## Measurement methodology

All timing claims in this doc follow these rules. Numbers that predate them
(kept for history below) are labeled accordingly.

- **Cache states**:
  - **cold-kex** — kex-owned caches deleted (interface cache, run cache, erlc
    module cache). The OS page cache is *not* dropped, so this is not a true
    cold start; don't call it "cold".
  - **warm** — all caches present (steady state, ≥2 runs after invalidation).
  - **dev-loop** — interface cache invalidated (prelude or compiler changed).
    This recurs on every prelude edit / version bump and is the state
    developers actually pay most often; report it alongside warm.
- **Repeats**: no single-shot numbers. `make bench` (Phase 0b): ≥10 runs per
  scenario, report median + IQR. It is an informational gate, run before
  declaring any perf result; not CI enforcement. It exits 0 on success and 1
  only if a requested scenario produced zero successful runs — a broken
  harness or broken kex, an integrity signal, never a perf verdict.
- **Scenarios**: compile cold-kex / warm / dev-loop, `fib(35)`, 100M-`+` loop,
  plus C++ incremental rebuild and clean-build wall time (Phase 0b/1).
- **Provenance**: every results table cites the commit, the cache state, the
  build type (`-O0` dev vs optimized), and the machine.
- **Same-machine rule**: cold-kex keeps the OS page cache by design, so
  before/after comparisons are only valid on the same machine in the same
  session window. Never compare cold-kex numbers across machines or days.
- **Optimized-build rule**: runtime/codegen claims (Phase 4 especially)
  require optimized-build numbers; `-O0` deltas are screening only.
- **Noise floor**: single-run deltas under ~5% on runtime benches are noise;
  report them as noise, not wins.
- **Deserialization**: any discussion of interface-cache load cost must quote
  the decode-vs-walk split already instrumented in
  `src/beam/interface_cache.cxx` (`[cache] decode X ms walk Y ms` under
  `KEX_TIMINGS`), not just the 212 ms total.

Baselines were recorded with `KEX_TIMINGS=1`. The headline number driving the
pipeline work is the **prelude interface load** — the single largest cost of
every `kex` invocation, warm or cold-kex.

---

## Known defects & risk register

Standing list. Each item names its evidence and what unblocks or resolves it.
Severity is impact × likelihood of biting during this refactor.

| # | Sev | Defect / risk | Evidence | Status / unblocks |
|---|---|---|---|---|
| 1 | high | walker↔BEAM `List.first` discrepancy | Surfaced by the reverted receiver-devirt experiment; only the fuzzer cross-checks (no spec covered it) | **Fixed (uncommitted working tree)**: static calls on a structural alias root (`List.first(x)`, `Map.get(m,k,d)`) worked on the walker but failed on BEAM — check said "Unknown module `List`", lowering said "Undefined method". Root cause, three layers: (1) the source interface collector never visited top-level `TypeDef`s and rejected structural right-hand sides (`src/common/prelude_interfaces.hxx`: driver + `collectTypeAlias` rule mirror `registerTypeAliases`); (2) the KexI emitter recorded no `backingType` for generic structural aliases and the reader skipped them (`src/beam/collect_metadata.cxx`, `src/beam/kexi_registry.cxx` — generic aliases recorded with declared-order typevar binding, exempt from the bare-name splicer); (3) lowering had no prelude-type static stage (`src/ir/lowering_method_call.cxx`: bare-`receiverType` providers, no atom, first arg is the receiver). Collateral: `spec/static_alias_calls.kex` (+`.expected`), one `targetedCases` fuzzer entry. Gates: spec 632+13 env, spec-beam 546+13 env, fuzz 400/400 + targeted 1/1 (at the new 400 default; 200/200 at the old), ctest clean (2 known env), test-fuzz 14/14. `make test-wasm` not run (no pinned emsdk 5.0.7 / prebuilt gmp+pcre2-wasm here — only Homebrew emcc, explicitly the wrong toolchain; CI builds wasm from scratch) — the changed source-collection lines are the same code native `opt-in stdlib from source` exercises every invocation, so native gates cover them. Filed as kexhq/kex#626 |
| 2 | high | Interface-cache staleness hole | Key = stdlib source hash + `MAJOR.MINOR.PATCH` only (`preludeSemanticInterfaces` in `src/common/prelude_interfaces.hxx`); a compiler rebuilt from uncommitted changes at the same version + unchanged stdlib serves stale interfaces | **Closed in 3a**: key now folds in a compiler-binary short hash (`interfaceCacheKey`); proven by `interface_cache_cli_test` |
| 3 | med | Cache schema-evolution hazard | Index-based decode with literal arity checks: two *unrelated* `!= 14` checks (the `fn` tuple in `functionFromTerm` and the top-level `ifaces` tuple) plus `!= 9` in `signatureFromTerm` (`src/beam/interface_cache.cxx`); adding a field without bumping `kVersion` risks silent misparse or stale-shape rejection | **Mitigated in 3a**: `schema` block + `checkedTuple` + `static_assert` + agreement test. Full field tags only if 3c rewrites the format |
| 4 | med | Silent cache misses | `catch (const std::exception&) → nullopt` (`src/beam/interface_cache.cxx`); corruption, version mismatch, and deserializer bugs are indistinguishable; no hit-rate/miss-reason visibility even under `KEX_TIMINGS` | **Closed in 3a**: `[cache] miss: <reason>` under `KEX_TIMINGS`; bench reports HIT/MISS |
| 5 | med | Goldens bless current behavior | 644 + 558 byte-exact suites lock in each backend's output independently, including the known divergence (#1) | Standing caveat; differential fuzzer is the only cross-check → grown to 400 cases (`FUZZ_CASES`, ahead of the Phase 2 migration so the stronger net gates it) + one `targetedCases` entry, plus a targeted differential case for every divergence found |
| 6 | low | No eviction | `~/.cache/kex/prelude-interface/` gains one file per (source-hash, version) forever | **Closed in 3a**: `pruneInterfaceCacheDirectory` keeps newest 5 on write |
| 7 | low | `test-spec-beam-runner` flakes under load | 200 ms-timeout harness shelling to `make spec-beam` with a fake compiler; observed 2/4, 6/0, 5/1, 3/3, 6/0 across back-to-back runs of the identical binary | **Fixed (uncommitted working tree)**: `BEAM_SPEC_TIMEOUT` 0.2 → 2 s (the trivial legs are a bash spawn + file IO, which exceeds 200 ms on a loaded machine and spuriously retries; the deliberate-timeout legs sleep 10, so they still fire deterministically) and the outer `Process.run` budget 20 → 60 s (the TERM-ignoring case now takes ~14 s). Verified 6/6 twice quiet + once under 8-core saturation; the suite's own timeout-leg assertions prove the 2 s timeout still fires |
| 8 | med | BEAM evaluates match-guard comparisons before the type test; fatal on mistyped values | `let check(v: Any, n) = match v do i : Integer when i > n => "big" / _ => "other"` with `check([1,2],10)`: walker prints `other` (type-tests first), BEAM raises `Cannot compare [Integer] and Integer` (emits `and(comparison, is_integer)` — comparison first). Harmless while the comparison is a total BIF (`spec/type_pattern_match.kex` relies on it), fatal on the partial `kex_intrinsic_number` path (any guard comparison with unknown kinds — the untyped `n` here — or every integer comparison under `KEX_NO_OP_DEVIRT=1`). Fuzzer-invisible (never generates guard-over-mistyped-value); `and/2` is strict so swapping conjuncts would not help — needs short-circuiting or a total guard comparison | **Fixed (uncommitted working tree)**: `Lowering::withTypeGuards` (`src/ir/lowering_pattern.cxx`) now emits the type tests short-circuited ahead of the `when` guard (`case and(tests) of true -> guard; _ -> false`), matching the walker; safe in every consumer (match paths via `expandGuards`, rescue drops it, receive rejects the shape). Collateral: `spec/match_guard_type_order.kex` (+`.expected`, failed pre-fix with the `Cannot compare` raise, passes post-fix) and a third `targetedCases` fuzzer entry. Filed as kexhq/kex#628 |
| 9 | med | BEAM drops `when` guards and type tests on rescue arms (takes the first pattern-matching arm); loop multi-arm rescue dies in erlc | `e when false => "guarded" / _ => "fallback"` over `Error("boom")`: walker `fallback`, BEAM `guarded`; `e : Integer` over it answered `"int"`. Root cause: the `TryCatch` emitter never reads `MatchClause.guard`. Collateral find: the loop-body `trying` builder never drained `pendingTypeGuards` and missed the per-arm `subst` reset, so any 2+ arm loop rescue failed erlc with `unbound variable` (guardless included — pre-existing, proven by an unguarded two-arm probe) | **Fixed (uncommitted working tree)**: `Lowering::expandRescueGuards` (new, `src/ir/lowering_pattern.cxx`) wraps guarded arms in one catch-all whose body is the `expandGuards` expansion (new `noMatch` + `failOnFalseOnly` params: `TryThrow` rethrow failure, inverted `false -> cont` test matching the walker's literal-`false` rescue semantics) with a guardless fast path; wired at all four `TryCatch` builders + the loop drain/reset. Collateral: `spec/rescue_guards.kex` (+`.expected`, 9 prints, 5 wrong pre-fix) and a fourth `targetedCases` entry. Open language question noted in the issue draft: rescue's literal-`false` vs match's truthiness. Filed as kexhq/kex#629 |

---

## Phase 0 — Baselines (done)

cold-kex `kex --run examples/fib.kex` ≈ **690ms**, dominated by:

| Stage | Time | Notes |
|---|---|---|
| prelude interfaces | 374 ms | `preludeRegistry().buildSemanticInterfaces()` (169) + opt-in stdlib source parse (202) |
| module cache + erlc | 180 ms | mostly cold-kex `erlc` boot |
| type check | 95 ms | includes loading the prelude into the SemanticDB |
| copy runtime beams | 16 ms | ~222 beams copied to the temp output dir |

C++ incremental rebuild time (Phase 1 closing criteria) and the dev-loop
compile scenario are owned by Phase 0b's harness, not captured ad hoc.

## Measured results (historical, pre-methodology)

At `6c690a5` (pre-refactor baseline, merge of PR #608) vs after the first
batch of refactor commits, `-O0` dev build, single-shot wall-clock — treat with
the noise-floor rule above, and note the optimized-build rule: the codegen
deltas need optimized-build confirmation before they count as wins:

| Metric | Before | After | Change |
|---|---|---|---|
| compile, cold-kex | 1.01 s | 1.15 s | +14% (one-time interface serialize; recurs every cache invalidation — see dev-loop) |
| compile, warm | 0.68 s | 0.52 s | **−23%** |
| fib(35) runtime | 1.66 s | 1.60 s | −4% — **within noise**, not a win |
| loop runtime (100M `+`) | 0.88 s | 0.25 s | **−72% (3.5×)** on `-O0`; still needs optimized-build confirmation |

The interface cache's serialize cost makes a cache-miss run slightly slower;
steady state is faster. The operator devirtualization (`+`/`==` → raw BIFs) is
the apparent source of the loop speedup; fib is call-overhead-bound, so it
barely moves — consistent with noise.

---

## Phase 0b — `make bench` (done)

The repeat-and-median benchmark script. Nothing downstream declares a perf
result without it.

**Acceptance criteria (all hold):**

- ≥10 runs per scenario, reports median + IQR; exits 0 on success, 1 only if
  a requested scenario produced zero successful runs (informational, not CI
  enforcement).
- Scenarios: compile cold-kex / warm / dev-loop, `fib(35)`, 100M-`+` loop;
  C++ incremental rebuild — explicit scenario, 3 runs default (touch
  `BENCH_TOUCH` → relink) — and clean-build wall time (explicit opt-in,
  single run).
- Dumps `KEX_TIMINGS` phases for one warm run; parses `[cache] decode/walk`
  and miss reasons into its report.
- Report header records build type, commit, and machine (per the provenance
  rule).
- Runs green on this worktree (`make bench`, `make test-bench`).

Baseline at `f2e211b+dirty`, Darwin 27.2.0 arm64, default flags (no -O):

| scenario | median | IQR | min | max |
|---|---|---|---|---|
| compile-warm | 0.435 | 0.005 | 0.426 | 0.441 |
| compile-cold-kex | 1.111 | 0.003 | 1.108 | 1.126 |
| compile-dev-loop | 0.758 | 0.002 | 0.752 | 0.763 |
| fib(35) | 1.876 | 0.005 | 1.860 | 1.893 |
| loop (100M tail calls) | 6.123 | 0.163 | 5.980 | 6.787 |

Warm cache-hit phase split at the same commit: decode ~50 ms, walk ~27 ms,
of a 207 ms `prelude interface cache` phase (see 3c).

Note: the historical "loop 0.25 s" figure has no committed source in this
repo and is not comparable. (An earlier revision of this note claimed no
`while` exists — wrong: the lexer/parser support `while cond do`, with
`WhileExpr` in the AST; it is `grammar.ebnf` that is stale on this point.
The loop scenario was first spelled as tail recursion on that wrong basis
and measured ~6.1–6.6 s for 100M calls; it is now a `while` loop measuring
per-iteration `+`/`-` cost. Baseline table above uses the recursive spelling;
`while`-spelling re-baseline (current tree + risk-#1 fix, quiet machine):
median 0.475 s, IQR 0.005, min 0.471, max 0.561 — a different quantity from
the recursive spelling, not comparable to it.)

---

## Phase 1 — Decomposition (done)

The three "god files" split into named modules / translation units:

| File | Before → After | Extracted into |
|---|---|---|
| `src/main.cxx` | 8353 → 7527 | `common/{process,diagnostics,file_io,string_utils}`, `module/discovery` |
| `src/semantic/typechecker.cxx` | 11170 → 4600 | `typechecker_expr.cxx` (5975), `typechecker_unify.cxx` (213), `typechecker_internal.hxx` (418) |
| `src/ir/lower.cxx` | 13159 → 2392 | `lowering.hxx` (10664, the `Lowering` struct), `lower_internal.hxx` (188) |

Honest status: `typechecker_expr.cxx` is a split, not a decomposition — the
god file largely moved. `lowering.hxx` got the real treatment below.

**Closing criteria (all hold):**

- `Lowering` methods moved out-of-line: `lowering.hxx` went 10,664 → ~830
  lines (decls + member data + 6 nested structs + 1 template method kept
  inline); 166 method bodies moved by a one-shot splitter script (kept in
  `/tmp`, not the repo) into 11 TUs along the file's own banner sections
  (`lowering_core/expr/calls/method_call/helpers/pattern/control/body/
  function/record/make.cxx`). The tail free functions moved to
  `lower_internal.hxx` as `inline` (`static` → `inline`, so the mutable
  global no longer risks per-TU divergence). Pure move, verified by the
  golden suites + fuzzer below.
- Rebuild-time before/after (same machine, `default (no -O)` flags):
  - Incremental (touch one lowering method → relink): before = recompile
    the 13,061-line `lower.cxx.o` + relink, 26.1 s wall; after = recompile
    exactly 1 TU + relink, 24.5 s wall (median of 3, `bench rebuild`).
    The wall delta is small because ~24 s of relinking (static lib + 36
    binaries) dominates both; the compile work itself dropped from
    3.7 s → 2.0 s at `kex_lib` level (one TU + `ar`) and now parallelizes.
    Follow-up (not this phase): per-TU include trimming — both sides are
    still parse-bound on the same ~20 shared headers.
  - Clean build: 40 s → 40.6 s (unchanged — same total work; the old
    giant TU was not on the critical path).
- Runtime guard: `fib(35)` 1.876 → 1.861 s (same); compile scenarios all
  slightly faster (3b). `loop` medians read ~8% slower across three runs,
  but the scenario's own in-batch IQR hit 0.79 s (13%) and the
  compiler-side `compile-warm` improved, so no compiler slowdown is
  demonstrated — machine/JIT variance over a long session. An
  old-vs-new emitted-`.beam` comparison was attempted and abandoned: every
  beam embeds absolute source paths, so cross-tree bytes differ
  meaninglessly; the golden suites are the behavior check.
- Gates on the final tree (`a75c569 "Lowering separation"`):
  - `make spec`: 631 passing / 13 failing. The 13 are exactly the TCP/HTTP
    net specs (`net_http_*`, `net_socket_*`, `net_websocket_*`,
    `net_unix_policy`), all failing with sandbox `eperm` on bind/connect
    under `# kex: run-beam`; the 4 non-network net specs pass. Proven
    environmental by re-running the 17 net specs in isolation (counts add
    up: 644 total = 631 + 13).
  - `make spec-beam`: 545 matching / 13 differing. The 13 are the same
    net specs as the walker-gate failures (proven by a net-only run: 4
    matching + 13 differing = all 17 net specs; counts add up to the 558
    total), same sandbox-`eperm` cause.
  - `make fuzz`: 200 cases, 200 pass, 0 FAIL (seed 42).

### DRY (done)

- `common/string_utils.hxx` — one `lowered` / `snakeCased` (was three copies; also
  fixed an acronym-stem inconsistency in the dependency loader).
- `semantic::completeReplInput` — the REPL completion program-building shared
  between the native REPL (`main.cxx`) and the wasm REPL (`wasm_repl.cxx`).
- **mangling centralization** — `common/mangle.hxx` provides `kSeparator` +
  `qualify` / `headOf` / `tailOf` / `scopeOf` / `memberOf` / `hasScope` /
  `manglePath`; ~100 raw `"::"` sites across interpreter, semantic, ir, compiled,
  validation, prelude migrated onto it. Also folded two duplicate `qualified` /
  `qualifiedName` helpers into `qualify`.

---

## Phase 2 — Name-resolution core (elevated; was "Phase 4")

The recurring bug classes (import/module scoping, name collisions, record
identity, capability arity — the source of most of the ~200 `kexhq/kex#` fix
comments) all stem from name-resolution logic being duplicated across the
typechecker, lowerer, and `resolve_pass`. One shared symbol-resolution module
would be consumed by all three. This is also the groundwork the planned module
system needs.

This is the largest bug-surface item in the plan, which is why it precedes
all remaining performance polish.

**Steps, in order:**

1. **Inventory** — map every duplicated name-resolution site across
   typechecker / lowerer / `resolve_pass`. Done = a table in this doc listing
   consumer × site (symbol names, not line numbers) × the behavior contract
   each site's callers rely on. That table is the migration checklist.
   Scope-box: the three consumers only. Interpreter-internal resolution is out
   unless the inventory shows genuinely shared semantics.
2. **Design gate** — interface sketch of the shared module + migration order
   (smallest consumer first — see step 3), recorded in this doc and reviewed.
   "The design has landed" — the event that unblocks Phase 3c/3d and Phase 4 —
   means exactly this: the gate is met at doc level. Migration need not be
   complete.
3. **Migrate** — consumer by consumer, gated on the full spec suites. The
   smallest consumer goes first as a vertical slice to validate the design
   before the rest is committed. If the slice forces a design change, re-pass
   the gate (cheap: it is a doc edit + review).

### Inventory (done)

Method: two read-only research agents (typechecker side, lowerer side;
reports retrieved in full) plus a direct read of `resolve_pass.cxx` and
spot verification of every symbol name below against the tree at `a75c569`.
Symbol names, not line numbers, per the step-1 contract. Tables group by
concern; each row is consumer × site × the contract its callers rely on.

**A. `using` application (import binding)** — three implementations of the
same documented rules (`as:`-alone imports nothing, `only:`/`except:`
mutual exclusion, explicit-beats-whole-module, receiver-method exemption):

| Consumer | Site | Contract |
|---|---|---|
| resolve_pass | `ResolvePass::resolveUsing` (+ `resolveUsingBlock`, `UsingExpr` case) | Two paths (source module via `SemanticDB::exportsFor`, packaged/prelude via `ImportedInterfaces`); ambiguity error unless one side is a receiver method (#272); records references; alias binds the name only |
| typechecker | `TypeChecker::checkUsingBlock`, `m_importScopeStack` / `m_functionImports` / `m_makeImports` / `m_mainImports`, `usingImportedMethods`, `moduleMemberImported`, `importedFunctionVisible` | Deferred `ImportSelection` per body; later lookups see imported members; unresolved imports diagnosed |
| lowerer | `Lowering::applyUsingBindings` / `restoreUsingBindings` (+ inline copies at module-`UsingBlock` and top-level `UsingBlock` in `lower.cxx`) | Fills `usingModules` / `moduleAliases` / `moduleImports` / `externalImports`; scoped with save/restore; in-unit `moduleImports` beats later external fills |

**B. Module-path resolution** — three spellings, one admitted mirror:

| Consumer | Site | Contract |
|---|---|---|
| typechecker | `TypeChecker::resolveModulePath` (+ `resolveImportedModulePath`) | Literal path → alias → current-module-relative → `using`-qualified (#229/#233) |
| lowerer | `Lowering::registerModuleAliases` | Explicitly mirrors the checker's resolution; leaf + children + siblings, same-named child preferred, never overwrites |
| resolve_pass | inline walk in `ResolvePass::resolveUsing` | `hasModule` → `ensureModule` → enclosing-module-relative walk; missing module is a warning, not an error |

**C. Bare-name value/call resolution:**

| Consumer | Site | Contract |
|---|---|---|
| resolve_pass | `ResolvePass::isKnown` | Primitives → import scopes → local scopes → file symbols (enclosing-module filtered) → imports (scoped lookup, module names, receiver fns, type names, ADT ctors, automatic-import with the Uppercase rule); pure predicate |
| typechecker | `lookupVar`, call checking via `resolveArgHints` / `resolveBlockHints` / `importedCandidateSignatures`; produces `m_selectedCallSignatures` per `FunctionCall` | Types the call, picks the signature; the signature table is the handoff other consumers should use |
| lowerer | `Lowering::lowerFunctionCall` (+ `lower` `Identifier` / `UpperIdentifier` / `TaggedLiteral` cases, `externalConstant`, `moduleConstantSymbol`, `externalPrefixCall`) | External-import → param tables → test-DSL builtins → `self`/`send` → zero-arg-thunk chain → constructor → local-indirect → `moduleImports` → implicit-`this` sibling → `knownFns` → external prefix → runtime error. Never consults `resolvedCalls` (by design: bare calls have no resolved-target channel) |

**D. Receiver-call (`MethodCall`) resolution:**

| Consumer | Site | Contract |
|---|---|---|
| typechecker | checking produces `m_resolvedCalls` (`ResolvedCallTarget`), `typeMap()`, `staticTypeOfCalls()` | The winner + receiver/arg static types + `Type.of` answers; the handoff the lowerer consumes |
| lowerer | `Lowering::lowerMethodCall` / `lowerMethodCallImpl` | ~2000-line cascade: special forms → trait-dict / `this`-field → `staticTypeOfCalls` → `staticFieldRead` → `Process.spawn` → resolved-target emission → dotted module path (local, external) → single-module namespace → own-type free fn → `moduleImports` UFCS → compiled-apart UFCS → external receiver fns → local UFCS → `undefinedMethodOrField`. First hit wins |
| resolve_pass | `MethodCall` case of `resolveExpr` | Deliberately skips: UpperIdentifier receivers and member validation deferred to the typechecker |

Known friction: the resolved-target gate second-guesses the checker's
winner (`localTypeShadows`, `stalePreludeTarget`, `localFieldAccessor`,
`modulePathOwnsCall`, local-beats-trait-default, trait-default→dispatcher
demotion) instead of trusting it — each site needs a justification or
removal during migration.

**E. Record / ADT / type-identity resolution:**

| Consumer | Site | Contract |
|---|---|---|
| typechecker | `resolveRecordName`, `resolveTypeExpr`, `resolveDistinctName` | Written spelling → canonical identity used for typing and guards |
| lowerer | `canonicalRecordName`, `qualifiedVariantTag`, `lowerRecordConstruction`, `typeGuard`, `moduleLocalTypeSpelling`, `simpleTypeName` | Source spelling → canonical tag identity (innermost module → own-opaque stop → local-top-level → external+`using` → dispatcher owner → alias-prefix rewrite → bare); guards and construction consume it |
| resolve_pass | `isKnown` (type names, ADT ctors); patterns bind only | Patterns introduce locals; constructor/tag identity is NOT validated here |

**F. Trait identity — already shared (the migration template):**

`kex::resolveTraitName` (`common/type_def_utils.hxx`) is consumed by
`TypeChecker::resolveTraitReference`, `Lowering::traitIdentityOf`, the
evaluator, and `collect_metadata`. One gap: the lowerer's
`inheritedDefaults` re-resolves `implement:` lists from pre-pass
`scopeUsings` instead of consuming a resolved table.

**G. Named-argument clause selection:**

| Consumer | Site | Contract |
|---|---|---|
| shared primitive | `kex::namedArgumentsFit` (`common/named_arguments.hxx`) | Clause/shape fits labels+positionals; used by checker and lowerer — the *loops* around it are what diverge |
| resolve_pass | `declaredLabels` / `checkNamedArguments` (+ `collectLabels`) | Union of labels across local clauses + imported overloads + `using`-origin modules; unknown label is an error with an edit-distance hint |
| typechecker | `m_selectedCallSignatures` | Per-`FunctionCall` signature choice — computed, then ignored by the lowerer (see trust boundaries) |
| lowerer | `namedClause` (template in `lowering.hxx`), `makeMethodFor`, `blockFitsExactOverload`, `placeMethodArguments`, `fillImportedDefaults` / `fillDefaultSlots`, `trailingMakeDefaults`, plus ~8 inline placement copies | Owner-scoped defaults/labels; strict/non-strict fallback; foreign receivers borrow nothing (#475); exact-arity overload pads nothing (#531) |

**H. Visibility gating + diagnostics:**

| Consumer | Site | Contract |
|---|---|---|
| resolve_pass | `enclosesCurrentModule`, `suggest` (edit distance), ambiguity / missing-`using` errors | Lexical enclosure test; typo hints |
| lowerer | `noteModuleScopedMake`, `makeMethodInScope` / `moduleInScope` (explicitly mirrors the walker), `gatedModuleTaking` (mirrors the walker's hint), `undefinedMethod` / `unknownNamedArgument` | Foreign-type module makes gated on `using`; out-of-scope clauses name the missing import |
| typechecker | `importedFunctionVisible`, `moduleMemberImported` | Import visibility for checking |

**I. Trust boundaries (outputs each consumer hands off):**

- Checker → lowerer: `m_resolvedCalls` → `resolvedCalls` (MethodCall
  winners), `typeMap()` → `expressionTypes` (~30 use sites: receiver/arg
  static types, trait dictionaries, guards, provider picks),
  `staticTypeOfCalls()` → `Type.of` literals. Verified present.
- Checker → interpreter/LSP only: `selectedCallSignatures()` is passed to
  `evaluator.setCallSignatures` (`main.cxx`) and read by the LSP server —
  never by any `lower*` entry. The lowerer re-derives call signatures from
  `fnParamNames` / `fnNamedClauses` / KexI tables. Closing this gap (a
  resolved-target channel for bare calls) is the migration's highest-value
  deletion.
- ResolvePass → DB: `SymbolInfo::references`, `completionScopes`,
  diagnostics. No other consumer reads its tables — migration-safe seam.
- Lowerer-internal tables (`moduleFunctions`, `moduleImports`,
  `externalImports`, `fieldAccessors`, `methodOwners`, `localTypeModules`,
  …) are built by the `lowerProgram` pre-pass (`preRecord` / `preFn` /
  `preType` / `preMake` / `preModule`); several encode resolution answers
  (`moduleReceiverNames` #230, `$free` renames #492, `collidingMethods`)
  that a shared module would compute once.

**J. Out of scope (scope box holds):** interpreter-internal resolution
stays out. The inventory shows the interpreter *consumes* the checker's
`selectedCallSignatures` rather than duplicating selection, so no
genuinely shared semantics force it in. (Its `resolveTraitName` use is
already-shared-helper use, not duplication.)

**Smallest consumer: `ResolvePass`** — 995 lines, one TU, narrow contract
(known/unknown + label check + references), no emission, no downstream
table readers. The checker is ~17k lines across three files; the lowerer
~13k across twelve. The vertical slice is ResolvePass's `resolveUsing` /
`isKnown` / `declaredLabels` onto the shared module.

### Design gate (proposed — needs review)

**Placement:** `src/semantic/resolve_core.{hxx,cxx}`. The shared code
needs `SemanticDB`, `ImportedInterfaces`, and AST types, all of which
live in `semantic/`; `src/ir` already includes both `semantic/` and
`common/` headers, so the dependency direction stays as-is. AST-free
primitives keep following the `common/named_arguments.hxx` precedent
(`resolveTraitName`, `namedArgumentsFit` already live there).

**Interface sketch** (names draft, contracts from the inventory tables):

- `resolveModulePath(db, imports, currentModule, usings, aliases, written)`
  — the single table-B implementation; the lowerer's
  `registerModuleAliases` mirror and the resolve_pass inline walk delete.
- `applyUsing(scope, module, alias, only, except)` — REVISED by the
  step-1 slice (cheap re-pass, as anticipated): a shared `applyUsing`
  is the wrong shape — the checker defers `using` selections without
  enumerating exports, so enumeration cannot be shared across all
  three. What is shared is the per-name merge DECISION:
  `resolve_core::mergeImportOrigin(scopes, name, origin)` (bare beats
  receiver-method, explicit beats whole-module, two bare claims =
  ambiguity, all from table A), with enumeration, sinks, and
  diagnostics staying per-consumer. Likewise `isKnown` stays in
  ResolvePass (its scope/db/lookup mix has no cross-consumer twin)
  and `declaredLabels` stays (sources differ per consumer; the shared
  primitive `namedArgumentsFit` already exists) — the slice moves the
  import-scope READS onto shared-populated scopes instead.
- `labelsFor(db, imports, scopes, name)` + `selectClause(candidates,
  labels, positional)` — the single table-G implementation behind
  `declaredLabels` / `namedClause` / `makeMethodFor`; the lowerer's ~8
  inline placement copies collapse onto it.
- `canonicalTypeName(db, imports, currentModule, usings, aliases, written)`
  — the single table-E implementation behind `resolveRecordName` /
  `canonicalRecordName`.
- Bare-call handoff: thread the checker's per-`FunctionCall` signature
  choice to the lowerer (extend the `resolvedCalls` channel or pass
  `selectedCallSignatures` into `lower*`) so `lowerFunctionCall` stops
  re-deriving from `fnParamNames` / KexI. Highest-value deletion; also
  the highest behavioral risk, so it migrates last within its slice with
  the fuzzer watching.
- Resolved-target trust: each table-D second-guess site
  (`localTypeShadows`, `stalePreludeTarget`, `localFieldAccessor`,
  `modulePathOwnsCall`, the two demotions) gets a recorded justification
  or is removed; removal is per-site, gated on the suites + fuzzer.

**Migration order (smallest first):**

1. Vertical slice (done): `src/semantic/resolve_core.{hxx,cxx}` with
   `resolveSourceModulePath` (table B, source-db rule),
   `mergeImportOrigin` + `ImportOrigin`/`ImportScope` (table-A merge
   kernel), consumed by `ResolvePass::resolveUsing` (both paths);
   `checkUsingBlock` and `applyUsingBindings` untouched. Pinned by
   `tests/resolve_core_test.cxx` (11 cases). Gate: `semantic_test`
   296 + spec + spec-beam + fuzz unchanged (results below).
2. Lowerer table-A/B copies onto the shared functions (in progress).
   Prep corrected the shape again: the lowerer cannot consume
   `mergeImportOrigin` (no origin tracking — ambiguity is check-time)
   or `resolveSourceModulePath` (it never resolves `using` paths).
   What it shares is the selection rule (`usingImportsNames` /
   `usingSelectsName`, also consumed by ResolvePass). The step's bulk
   is intra-lowerer: unifying the three copies (expression, top-level,
   module-level) onto one bindings core. That unification exposed a
   real walker↔BEAM divergence — top-level and module-level
   `using M do ... end` bodies are DROPPED on BEAM (walker runs them;
   no spec covered it) — plus drift (nested-only+overwrite aliasing
   and a `body.empty()` gate in the module copy vs full+try_emplace
   elsewhere; the checker's context-free `checkUsingBlock` says the
   drift is not deliberate). The step fixes the drop (scoped bindings
   + body exprs lowered immediately, emitted before main-phase steps
   in file order — elaboration-first, matching the walker's two-phase
   structure; file-order interleaving was tried first and abandoned
   when the walker proved to run using-bodies before all mains) and
   unifies the aliasing, pinned by new parity + scoping specs and a
   second `targetedCases` entry.
   Known limitation: cross-unit module-level bodies still cannot run
   on BEAM (no load hooks); in-unit order matches the walker.
   Step-2 gate results (working tree): `resolve_core_test` 13,
   `semantic_test` 296, ctest clean (2 known env), spec 634+13 env
   (same net set), spec-beam 547+13 env (same net set); fuzz
   400/400 + targeted 2/2. Done.
3. Checker table-A/B onto shared selection predicates (in progress).
   Prep corrected the shape: `checkUsingBlock` itself just records
   selections (deferred machinery stays); the shareable rules are
   the per-name `only:`/`except:` copies, which live in two checker
   lambdas (call-reachability `selected`, record `importedRecord`)
   plus step-1 leftovers in `ResolvePass::resolveUsing` (both
   paths) and a fourth copy in `Analyzer::moduleImportingName`
   (predicate-sharing only — the analyzer is outside the
   three-consumer migration scope). All five now call
   `resolve_core::usingSelectsName`. `resolveModulePath` internals
   (alias-prefix walk, `Kex.` canonicalization) stay per-consumer
   for step 5's table-F gap analysis.
   Step-3 gate results (working tree): `resolve_core_test` 13,
   `semantic_test` 296, ctest clean (2 known env), spec 634+13 env
   (same net set, eperm spot-confirmed), spec-beam 547+13 env
   (same net set, net-only rerun confirms); fuzz 400/400 +
   targeted 2/2. Done.
4. Table-G collapse (in progress). Prep corrected the shape and the
   count: the sketch's `labelsFor` + `selectClause` "single
   implementation behind `declaredLabels` / `namedClause` /
   `makeMethodFor`" does not survive contact. `declaredLabels`
   keeps per-consumer sources (decided step 1); `makeMethodFor`
   keeps its owner filter + skip rule + min-modulePath policy, and
   the checker selects signatures while ResolvePass selects
   nothing — so cross-consumer sharing in table G is already done
   (`namedArgumentsFit`). What collapses is intra-lowerer (the
   step-2 pattern): the inventory found 14 slot-placement loops,
   not ~8, plus 3 clause-selection loops (one a verbatim duplicate
   pair). Two pure helpers in `common/named_arguments.hxx`, beside
   the existing fit predicate: `selectClause` (first fitting
   clause, miss policy per site) behind `namedClause` + the two
   inline copies, and `placeArguments` (pure slot-index mapping:
   labels by name, positionals front-to-back, `firstSlot`
   reservation for a pre-placed receiver) behind all 14 placement
   loops — each site keeps its atomize flavor and order, miss
   policy (ignore / throw / diagnostic), and tail (defaults /
   none-fill / `continue`). Pinned by `tests/named_arguments_test`
   (7 cases) + the suites.
   Step-4 gate results (working tree): `named_arguments_test` 7,
   `resolve_core_test` 13, `semantic_test` 296, ctest clean
   (2 known env), spec 634+13 env (same net set), spec-beam
   547+13 env (same net set, net-only rerun confirms); fuzz
   400/400 + targeted 2/2. Done.
5. Table-E + table-F gap (in progress). Prep re-passed both halves.
   Table F first: `traitIdentityOf` already delegates to the shared
   `resolveTraitName` — the gap is only its imports input. Three
   real (narrow) divergence vectors vs the checker's
   `resolveTraitReference`: import ORDER (checker innermost-first,
   lowerer alphabetical `std::set`; `resolveTraitName` is
   first-match-wins), aliases (checker `throughAlias` fast path,
   lowerer none — `implement: G.Shape` silently inherits nothing
   on BEAM), and `only:`/`except:` (checker filters modules by
   bare trait name, lowerer does not — checker and lowerer can
   settle on DIFFERENT traits with checking on). Block-scoped
   usings are moot (`UsingBlock.body` holds expressions only; a
   make block is never inside one). Fix per inventory:
   `inheritedDefaults` consumes a resolved-trait channel
   (`TypeChecker::m_resolvedTraits`, threaded like `resolvedCalls`
   through `Analyzer` → `main.cxx` → `lower*` → `L`), falling
   back to today's path without analysis. Same-namespace keys
   both sides (`kex::traitIdentity`).
   Table E: the chains share shape but differ in stage policy and
   order — no `canonicalTypeName` whole-chain unification. The
   shared units are the two pure rules: `resolveInScopes`
   (innermost-first `scope.name` walk, first `exists` wins) and
   `matchSuffix` (unique / ambiguous / none tri-state over a
   candidate table), both in `resolve_core`, consumed by the two
   record chains plus the checker's nested-module walk. Also folds
   a step-3 leftover: `resolveTraitReference`'s own only/except
   copy → `usingSelectsName`.
   Verification caught a live one: `implement: Geo2.Shape`
   through `using Geo, as: Geo2` errored on BEAM (`Undefined
   method: describe for Circle` — forced-fallback binary) while
   the checker accepted it; with the channel BEAM inherits the
   default (`area=6.0`). Pinned by `spec/trait_alias_implement`
   (`# kex: run-beam`) + `.expected`. The same probe exposed a
   PRE-EXISTING walker gap (untouched by this step): the walker
   does not resolve the alias either and falls back to inspection
   output. Walker-internal resolution is out of scope (table J),
   so the walker side stays open: no `targetedCases` entry (it
   would fail the gate while the backends disagree), filed as
   kexhq/kex#627 alongside risk #1's filing (#626).
   Step-5 gate results (working tree): `resolve_core_test` 16,
   `named_arguments_test` 7, `semantic_test` 296, ctest clean
   (2 known env), spec 635+13 env (same net set, incl. the new
   trait-alias spec), spec-beam 548+13 env (same net set, net-only
   rerun confirms); fuzz 400/400 + targeted 2/2. Done.
6. Trust work last (in progress). Prep re-passed the handoff: the
   sketch ("`lowerFunctionCall` stops re-deriving") does not
   survive contact. `Signature` carries names but no DEFAULTS,
   and the checker keeps no clause back-pointer to fetch them —
   so the lowerer cannot consume the selection without either
   checker surgery (clause identity at selection time) or keeping
   its clause tables for defaults (in which case the "handoff"
   is a second selection path beside the --no-check fallback,
   not a deletion). No in-tree placement precedent either: the
   walker consumes `selectedCallSignatures` only for Showable
   display dispatch. And no observed divergence motivates the
   risk (suites + 400-case fuzzer green). The proportionate core
   is VERIFICATION, not deletion: a temporary experiment threads
   `selectedCallSignatures` into `Lowering` (single
   `lowerFunctionCall` caller keeps it cheap) and compares the
   checker's (paramNames, arity) against the selected local
   clause; spec + spec-beam + fuzz then prove agreement or
   surface live selection divergences to fix. The fuzzer remains
   the drift guard afterward (a selection split that changes
   output fails the differential gate; one that doesn't is
   harmless). Disposition follows the experiment outcome.
   Second-guess audit after: the six sites (`localTypeShadows`,
   `stalePreludeTarget`, `localFieldAccessor`,
   `modulePathOwnsCall`, local-beats-trait-default,
   trait-default→dispatcher demotion — all in
   `lowerMethodCallImpl`, each already justified in code with
   issue refs) get per-site force-off experiments through spec +
   spec-beam; removal needs gates green PLUS a dedicated probe
   of the cited scenario (a green gate alone cannot prove a
   latent issue trigger is gone), else the justification stands
   and is pointed at from here. (`capabilityOwnsCall` nearby is
   not a gate condition — out of the audit's set.)
   Experiment outcome: 22 comparisons across 12 specs
   (local named-arg calls with FunctionDef-named checker
   entries), 0 mismatches; the 400-case fuzzer adds no
   comparable calls. No live selection divergence — the
   handoff-as-deletion stays retired, the experiment is fully
   reverted (no `KEX_SELECTION_AUDIT` remnants), and the
   differential fuzzer remains the drift guard. Disposition:
   no committed code; agreement recorded here.
   Second-guess audit dispositions (all in
   `lowerMethodCallImpl`; firing map from a temporary
   stderr sweep over non-net specs, since reverted):
   1. `localTypeShadows` — KEEP. Force-off breaks
      `comparable_max_min.kex` + its BEAM twin; fires in 5
      specs. Justification stands (in-code: #143, #235, #408,
      `json_parser` shape).
   2. `stalePreludeTarget` — NARROWED. The prelude-self-build
      initializer (`preferExternalReceivers && ...`) was
      provably dead (no analyzed caller sets the flag; the
      two `true` sites pass null analysis) and is deleted.
      The two live accumulations (compiled-apart provider,
      erased receiver) stay: zero suite triggers, zero
      force-off failures, cited hazards unverifiable by
      removal — justification stands.
   3. `localFieldAccessor` — KEEP. Fires in 6 specs but
      force-off breaks none (fallthrough agrees
      everywhere); cited erased-value crash not
      reproducible in suite or probes — removal
      unverifiable. Justification stands.
   4. `modulePathOwnsCall` — KEEP. Zero suite triggers,
      zero force-off failures; cited #182 crash not
      reproducible (resolution now reaches the module on
      its own). Justification stands.
   5. local-beats-trait-default — KEEP. Zero suite
      triggers, zero force-off failures; cited failure is
      a SILENT wrong answer (`None` for `Just(-1)`) —
      the worst class to remove blindly. Justification
      stands.
   6. trait-default→dispatcher demotion — REMOVED (~20
      lines + comment, short pointer left). Fires in 28
      specs yet force-off spec-beam green, 400-case
      fuzzer green, and four targeted probes agree
      (literal/bound/typed/unpinned Optional
      interpolation, custom `Showable` instance); cited
      `Just(50)` shape no longer reproduces.
   Net audit effect: two deletions (dead initializer,
   orphaned demotion), four keeps with recorded
   justifications.
   Step-6 gate results (working tree): `resolve_core_test` 16,
   `named_arguments_test` 7, `semantic_test` 296, ctest clean
   (2 known env), spec 635+13 env (same net set), spec-beam
   548+13 env (same net set, net-only rerun confirms); fuzz
   400/400 + targeted 2/2. Done. **Phase 2 migration
   complete** — all six steps gated green.

Steps 1–3 unblock 3c/3d + Phase 4 (the gate is met at doc level now;
migration progress is not the gate). If the slice forces a design
change, re-pass here — a doc edit + review.

Step-1 gate results (working tree): `semantic_test` 296 green,
`resolve_core_test` 11 green; spec 632+13 env (same net set);
spec-beam 546+13 env (same net set, net-only rerun confirms);
fuzz 400/400 + targeted 1/1. Unchanged across the board.

**Stall guard:** the gate is doc-level and small by construction. If the
inventory shows a surface larger than the three-consumer scope, split the
migration into slices rather than widening the gate.

**Sequencing note:** Phase 3c/3d and Phase 4 perf work wait until the Phase 2
design gate is met — not until migration finishes. (Phase 0b/1/3a/3b are
exempt — they are small and unconditional.)

---

## Phase 3 — Compiler-pipeline speed (internally reordered)

### Done

- **Prelude interface cache** — `beam/interface_cache.{hxx,cxx}` serializes the
  checked `ImportedInterfaces` snapshot (ETF, versioned + magic-guarded) to
  `~/.cache/kex/prelude-interface/<source-hash>-<kex-version>.bin`. A cache hit
  replaces the whole prelude build. Result: **375 ms → 212 ms** (~163 ms saved
  per invocation; total wall ~0.69s → ~0.52s). Writes are atomic (unique staging
  file + rename) to avoid a concurrent-builder race.
- **File-list caching** — `standardLibraryModuleRoots` /
  `standardLibrarySourceFiles` / `standardLibraryArtifactSourceFiles` compute once
  per process instead of re-walking the stdlib tree per caller.

### 3a — Interface-cache correctness hardening (done)

- **Key on the compiler binary.** `kex::compilerBinaryStamp()` (next to
  `executablePath` in `src/common/prelude_loader.hxx`) reports the running
  binary's `size|mtime`; `kex::beam::interfaceCacheKey()` folds a 16-hex
  short hash of it into the key after the stdlib source hash + version.
  This deliberately differs from an earlier revision's "git SHA + dirty
  flag at CMake configure time": the binary stamp is simpler (no CMake
  machinery, no configure-time staleness), costs two stat calls per
  invocation, degrades cleanly to sources+version when the executable path
  is unknown (wasm), and follows the #372 run-cache precedent literally.
  Closes risk-register #2. Proven by `interface_cache_cli_test` (failed
  pre-fix with `1 -> 1`, passes post-fix).
- **Schema single point of truth.** All tuple shapes named once in a
  `schema` block in `src/beam/interface_cache.cxx` (the two unrelated `14`s
  are now `kFn` vs `kIfaces`); named shapes go through `checkedTuple` on
  write and explicit size checks on read; a `static_assert` on the `Type`
  variant count fails the build when an alternative is added without
  extending the codec. `kVersion` stays 1 (no wire change) with bump
  discipline documented at the constant. Closes risk #3 at the 3a level;
  full field tags only if 3c rewrites the format. Backstopped by the
  round-trip agreement test (deep equality over every member + all 16 Type
  alternatives), which also fixed a real divergence found along the way: a
  null type used to normalize to UnknownType on cache hit but not on miss;
  it now round-trips as `"null"` (old files unaffected).
- **Eviction** — `pruneInterfaceCacheDirectory` keeps the newest 5 keys on
  write, never the key just written; readers treat a missing file as a miss
  (the cache is best-effort), so unlinking older keys is safe. Closes #6.
- **Miss-reason reporting** — `deserializeImportedInterfaces` reports
  bad-magic | bad-version | corrupt via an out-param (the silent
  `catch → nullopt` now notes `corrupt`); the caller adds `absent` and
  prints `[cache] miss: <reason>` under `KEX_TIMINGS`. Stderr only, never
  stdout (golden-safe). `make bench` reports HIT/MISS from its warm
  timings dump — the value check on the cache's complexity. Closes #4.
- **Tests** — all in `tests/interface_cache_test.cxx` (16 cases: agreement,
  null, 3 reason cases, 4 key cases, 2 eviction, 1 race) plus
  `tests/interface_cache_cli_test.cxx` (stale-key end-to-end). Collateral
  fix: both mtime-backdating CLI tests now mutate a private binary copy
  instead of `build/kex`, which was flaking `separate_check_cli_test`
  under parallel ctest.

### 3b — Dedupe `analyze()` runs (done)

Traced count first — the earlier "~4×" was wrong in both directions. A
cold-kex `--run` (Compile mode, default flags) ran the analyzer **6×**: the
gating pre-check, two explicit discovery analyzes, the real check, plus two
*internal* re-analyzes inside `resolveBeamDeps`, which re-analyzed the entry
program on every call (the entry passes `source=nullptr`, so it never hits
the discovery cache — only dep files do). The Run/Check discovery sites are
other modes (2 runs each per invocation), not the same `--run`.

Redundancy proof: each internal re-analyze ran on the same program object
with the same interfaces as the caller's explicit analyze immediately
before it, and its result was unioned with that same result as the seed —
pure duplication. Fix: `resolveBeamDeps` takes the seed as
`optional<unordered_set<string>>` (7 of 8 call sites pass fresh sets,
unchanged via implicit conversion; REPL `/load` passes nullopt and keeps
the internal analyze) and trusts a provided seed — even an empty one —
sorting it into `referencedOrdered` exactly as the analyze path did.
Resolution order, hence merge order, is identical by construction. The
alternative of handing the pre-check's set to the real site was rejected:
distinct set objects can hash-iterate differently, and merge order is
#377-sensitive.

Result: 6 → 4 analyzes on the Compile path (2 → 1 discovery analyzes on
the Run/Check/seeded-REPL resolve paths). Measured cold-kex: both `N
dependency files` lines report `analyze 0.0 ms` for the entry on fib.kex
(was 2.2 + 2.0 ms); gating pre-check 7.3 → 4.7 ms, load dependencies 4.3 →
2.1 ms. On the 212-line mlp_xor.kex: pre-check internal 7.3 → 0.0 ms (the
remaining 5.2 ms at the real site is the correct, discovery-cached
dep-file analyze). The earlier "~45 ms, more with deps" estimate had no
measured basis and is withdrawn; the win scales with entry size.

### 3c — Faster cache hits: key-hash cost first, then the deserialize fork

The ~207 ms warm cache-hit phase is NOT mostly deserialization. Measured
(`make bench` at `f2e211b+dirty`, Darwin arm64): decode ~50 ms + walk ~27 ms =
~77 ms of the 207 ms `prelude interface cache` phase. The remaining ~130 ms is
key computation (`standardLibraryArtifactSourceFiles()` +
`preludeSourceHash()` — re-reading and re-hashing every stdlib source on each
invocation) plus file IO (`preludeSemanticInterfaces`,
`src/common/prelude_interfaces.hxx`). An earlier revision of this section
attributed the whole phase to the ETF decode/walk; that model was wrong.

Work order (measure at each step):

- **Step 0: commit the decode/walk instrumentation** — DONE (the
  "uncommitted" note was stale; verified present in `a75c569`).
- **Step 1: time the key computation** (in progress). Split the
  phase timer so `preludeSourceHash` + file read report separately
  from deserialize (done: `cache key + read` / `cache deserialize`
  phases; `bench.sh` parses only `[cache]` markers so the rename
  is safe). Measured 12 warm runs, same binary/session
  (`a75c569+dirty`, Darwin arm64, isolated `XDG_CACHE_HOME`):
  key+read median 94.0 (IQR 0.5), deserialize median 113.0
  (IQR 0.9) = decode 51.2 + walk 25.7 + **free 36.0** — the
  free slice (term-tree destruction) is new instrumentation
  (`root.reset()` + `free` readout on the `[cache]` line;
  `bench.sh` still matches). Total ~207 ms confirms the old
  phase number, but the old 130/77 key/deser model is wrong:
  key does NOT dominate (94 vs 113). Step 1 done.
  Decision: consumption-side fixes first. Decode vs walk is
  51.2 vs 25.7 (1.96× — inside the fork rule's 2× band either
  way), and the free slice belongs to the consumption side:
  walk+free (62) beat decode (51). An arena kills both with no
  format churn and no staleness risk; the key-hash lever (94,
  with its risk-#2-class trap) decides after re-measurement.
  (Step 1's "if key dominates" branch is retired by this
  measurement — key is the biggest single slice but not a
  majority, and the safe win goes first.)
- **Step 2: the deserialize fork** (in progress; remainder is
  ~113 ms, not ~77 — the free slice counts). Fork inputs:
  decode 51.2 / walk 25.7 / free 36.0.
  Prep re-passed the consumption side: the named arena is
  BLOCKED without type surgery. `TermPtr` is `shared_ptr`
  held in the nodes' own vectors, so an aliasing arena
  (nodes sharing one guard) cycles — internal refs keep the
  guard alive forever. Escapes all fail: per-node guards
  keep N control blocks (no win), weak/raw children ripple
  through every `asTuple`/`mapGet` user, intrusive counts
  can't mix heap (encode side) and arena (decode side) nodes
  under one `TermPtr`. Audited instead: all four decode
  callers walk-then-drop (no `TermPtr` persists past the
  walk — no `TermPtr` members anywhere), which is what makes
  the replacement safe: a SAX-style ETF reader that parses
  straight into `ImportedInterfaces` — no tree, no walk, no
  free. Same ETF wire + same `schema::k*` layout + same
  `kVersion` (old caches stay readable; no format churn),
  divergence guarded by the existing round-trip agreement
  test (it calls `deserializeImportedInterfaces`, so it
  covers the SAX path with zero test changes) and the
  `CacheMissReason` mapping stays. Expected: ~113 → ~35 ms.
  The tree codec stays for encode + the KexI path (off the
  warm path); the tree-reader twins (`functionFromTerm`,
  `typeFromTerm`, ...) go away with the swap so the two
  readers can't drift (risk #3).
  **Decision rule** (branches ordered, not exclusive):
  - **walk ≥2× decode** → walk-dominated: fix the consumption side
    (this SAX reader). No new format.
  - **decode ≥2× walk** → decode-dominated: direct binary format (no ETF, no
    intermediate term tree).
  - **within 2× either way** (this reading: 51.2 vs 25.7, plus
    free 36.0 on the consumption side) → consumption-side
    (SAX) first, re-measure, then decide on the format.
  Step 2 DONE. SAX reader landed (`SaxReader` in
  `src/beam/etf.{hxx,cxx}`; `*FromSax` twins in
  `src/beam/interface_cache.cxx`; tree readers deleted; timing
  line is now `[cache] sax N.Nms`, matched by `bench.sh`).
  12 warm runs, same protocol as step 1: key+read median
  93.6 (IQR ~1.0 — unchanged path, consistent with 94.0),
  sax median 17.5 (IQR ~0.35). Deserialize went 113.0 →
  17.5 (6.5×, beating the ~35 estimate 2× — no tree alloc
  at all beats even decode-alone's 51.2). Warm cache-hit
  phase: ~207 → ~111 ms (46% off). Gates green: round-trip
  16/16 through the SAX path, ctest clean (2 known env),
  spec 635+13 net, spec-beam 548+13 net, fuzz 400/400 +
  targeted 2/2.
- **Step 3: the key-hash decision** DONE. Key+read (93.6)
  decomposed before any memo design: raw `cat` of the 904 KB
  / 46-file stdlib takes 4 ms and SHA-256 ~2 ms, so ~88 ms
  of the 93.6 was `istreambuf_iterator` per-byte read
  overhead — in `preludeSourceHash` AND in the 902 KB cache
  file read. Fix: `readFileBytes` block-read helper
  (`src/common/file_io.hxx`), used at both warm-path sites
  (same bytes → same content hash; verified: new binary's
  cache key carries the identical `6bfe0257…` hash segment,
  only the rebuild stamp differs). 12 warm runs, same
  protocol: key+read median 10.8 (IQR ~0.35), sax median
  17.5 (IQR ~0.4, unchanged). Warm phase total: ~207 →
  ~28 ms (7.4×). The mtime-memo option (a) is retired
  without building it: remaining key cost is ~11 ms of
  genuine IO+hash, so a memo would trade staleness
  machinery for ~9 ms — bad trade. The other
  `istreambuf_iterator` uses (`evaluator.cxx`, `db.cxx`,
  …) are off the warm path and stay. 3c DONE: no format
  rewrite (17.5 ms needs none), no memo.
- **Schema:** the single point of truth already landed in 3a. If 3c rewrites
  the format, that rewrite uses field tags plus a `kVersion` bump as part of
  the change — not as a follow-up.

### 3d — Skip the runtime-beam copy on cache hits (deferred until 3c lands)

~16 ms against the 212 ms dominant cost — bad priority until 3c moves the
dominant number. Revisit then.

**Restore-ordering entanglement (entry paragraph, from the code):**
`tryLoadRunCacheFrom` (`src/main.cxx:3231`) couples the hit-check with
the restore — one call validates `manifest.txt` and, as a side effect,
copies every cached `.beam` into `outputDir`. There is no way to ask
"is this a hit?" without also performing the restore. The prebuilt
runtime-beam copy (`src/main.cxx:6626`, ~16 ms) therefore runs
unconditionally *before* the lookup: on a miss, erlc overwrites
same-named modules afterwards so the order is harmless, but on a hit
there is no erlc step, so the restore must land strictly *after* the
prebuilt copy — otherwise the toolchain's possibly-stale prebuilt copy
of a same-named stdlib module merged from source (`Kex.URI`, … via
`resolveBeamDeps`) would silently win (the kexhq/kex#27 `env.query`
shape). So the skip cannot be "check hit first, skip copy": the check
*is* the restore, and restoring before the copy is exactly the wrong
order. Untangling means splitting check from restore (validate
manifest + file presence first; restore after the copy decision) —
and even then a hit cannot skip the copy wholesale: the restore set
covers only the program's modules while execution still needs the
prebuilt runtime beams (`kex_prelude.beam` et al.), so only the
same-named subset the restore is about to overwrite can be skipped.
First measurement before any code: restore-set vs prebuilt-dir
overlap — if the program's modules are a small slice of the copy,
the win is small and 3d stays deferred.
  Measured (read-only, no code): copy set is 141 `.beam` files /
  1.5 MB; only `Kex.*` stdlib modules (76 files / 552 KB) can
  collide, since intrinsics/prelude/runtime have no mergeable
  source. Suite-wide survey: 3 of 644 specs merge an opt-in
  module from source (`using Kex.AST`, 1 module each);
  qualified references outside `using` add at most 1–2 more
  modules per program (`Kex.FileSystem`, …). A real
  run-cache manifest (trivial program) restores 1 entry
  module with 0 collisions. Generous upper bound (5 merged
  modules): ~5/141 file-copies ≈ 0.6 ms of the ~16 ms copy,
  hit-path only. Verdict: 3d STAYS DEFERRED — ceiling ~1 ms
  typical, 0 ms for programs that merge no stdlib source,
  against check/restore surgery beside a kex#27-shape
  hazard. No code written.

---

## Phase 4 — Codegen speed

### Done

- **Operator devirtualization** — `+` on provably-integer operands now emits
  `erlang:'+'` (was `kex_intrinsic_number:add/2`, a 12-clause cross-module call);
  `==` / `!=` on provably-`Integer`/`String`/`Char` operands now emit
  `erlang:'=:='` / `'=/='` (was the coercing `eq`/`neq`). Verified by the
  differential fuzzer. Optimized-build confirmation DONE: `KEX_NO_OP_DEVIRT=1`
  (new switch, `src/ir/lowering_core.cxx`, the `KEX_NO_DEVIRT` precedent —
  forces the three operator predicates to plead ignorance so every operator
  takes the checked path, the same path unchecked code takes today) gives a
  before/after lever. 12 runs/arm, bench `loop` scenario (compile+run, 100M
  while iterations), Darwin 27.2.0 arm64, `30680b5+dirty`, same session
  window — default build devirt 0.311 (0.005) vs fallback 1.096 (0.021),
  optimized (`-O3 -DNDEBUG`) devirt 0.261 (0.004) vs fallback 1.058 (0.027):
  the fallback is 3.5× / 4.1× slower, ranges disjoint, far above noise.
  The runtime delta is identical across builds (0.785 vs 0.797 s per 100M
  iterations — the win is in the emitted BEAM; abstract code from both
  binaries is byte-identical, the only `.beam` diff being `compile_info`'s
  source path). Fallback parity: fuzz 400/400 + targeted 3/3 green with the
  switch on; switched spec-beam 548 + 13 env + 1 understood — a benign
  caught-error rendering delta (`kex_load_beam`: the intrinsic bakes
  `file:line:col` into the message while the BIF `badarith` carries none
  through the `Kex.load` catch; the `# kex: run-beam` golden was recorded
  from the BIF path). The second switched diff at first (`type_pattern_match`:
  intrinsic raises where the BIF fails the clause softly) was risk #8's
  guard-order divergence, now fixed — it matches with the switch on too.
  Both were pre-existing dual-path properties the switch exposes, not
  switch bugs; all real gates run with the switch unset and stay green.

### Receiver-call devirtualization (landed)

The prelude emits mangled `name/Type` symbols (`to/Binary`,
`map/List`, …), but the KexI interface consumed by callers stores
the *plain* `name` (the runtime dispatcher). A first cut that
pointed the metadata at `name/Type` broke 67 `spec-beam` cases.

**Design (landed; renamed 2026-11-10: the checker records the receiver
type, `ResolvedCallTarget.staticReceiverHead`, and lowering spells
`name/Type` with `mangleReceiverImplementation`):** re-derive `name/Type` at the consumer side —
no interface format change, dispatcher stays the fallback. The
typechecker records `ResolvedCallTarget.importDispatchSpelling`
(new field, empty = dispatcher) at the imported-winner site in
`typechecker_expr.cxx`; lowering applies it in
`lowering_method_call.cxx` after the local path.
`backendFunction` itself stays plain everywhere, so the
`traitDefaultOwner` check, the line-299 plain-provider rule, and
`rewriteModuleCalls` see exactly what they see today.
Predicate (all interface-visible; `resolve_core` as
`importDispatchSpelling`, 52 unit tests): fire for an imported
receiver call with winner W, call-site receiver T, in module M,
iff (0) pure make-block winner with a receiver (foul calls take
their context through a registry keyed on the bare name, so a
mangled foul call would go out one argument short); (0b) T has
no `Unknown` anywhere (gradual receivers stay dynamic); (1) W's
written receiver (`signature.params[0]`) has a provable emitted
head — primitives (non-sized, non-unit, non-literal-atom),
List/Map kinds, nominals except `Block`/`Optional`; (1b) a
nominal head names a declared type — bare heads by last segment
in record/ADT/distinct tables, qualified heads by exact record
membership only (records qualify on both sides; aliases,
opaques, and local ADTs can emit bare while the interface
qualifies); (5) the (module, *source*-arity, head) bucket is
single-signature up to alpha-renaming with foul-equality
(source arity = beam arity minus dictionary slots, matching
the emitter's overload key; else only `name/Head,args…`
exists); (12) W's head is the ONLY head covering T at
(module, beam arity) — trait peers excluded (the dispatcher
tries them last); (13b) M has ≥2 distinct heads at this name
(all arities — collision is name-wide; else the definition
kept the BARE name); (DS) Range/Pid winners shadowed by
List/Task keep dispatcher behavior (their clauses are
dropped); plus two vetoes — `''` peers (optional-sugar,
tuple, func, structural-record targets bind the bare name and
merge ahead of the dispatcher) and covering/unprovable
module-function peers (the argument dispatcher runs first);
(10) no trait dictionary for argument 0, checked by lowering.
`KEX_NO_DEVIRT=1` disables firing (before/after measurement,
emergency use).
Why this shape instead of the draft's: interface receiver
types are *written*-derived (`convertTypeExpr`, not resolved),
so in the common cases the head IS the spelling and the
predicate's job is excluding the divergence classes — which
killed the draft's prelude-only restriction (per-module
grouping generalizes it), its record/qualified cuts (records
qualify on both sides; bare heads provably emit bare), and
its `Optional<X>` firing (decodes indistinguishably from
`T?`, which emits `''`). Soundness rests on three legs: the
predicate (each clause mirrors a documented emitter rule);
`make test-devirt` (`tools/devirt-correspondence.sh`,
CI-wired), which audits every applied spelling over the
spec corpus and asserts each exists in the defining beam's
exports via `beam_lib`; and the existing gates.
Regression found by spec-beam and fixed: the hookup first
fed the *winner's* receiver as T, which covers
tautologically for var-free winners — `None.blank?` became
`blank?/Bool` (`badarg`) and `None.to`/`Float.to` became
`to/Atom` (`Undefined method`). The fix feeds the call-site
receiver (`resolve(argTypes[0])`). `--no-check` degrades to
no-devirt without changing correctness.
Coverage (spec corpus, `KEX_DEVIRT_AUDIT`): 2663 firings,
87 distinct, all present in `kex_prelude` exports —
heads String/List/Char dominate (`bytes/String`,
`upperCase/Char`, `count/String`, `count/List`,
`first/List`, `join/List`, `length/String`, …).
Timings (12 runs, median (IQR); microbench: 10M loop
iterations × 4 devirted calls, `--run` on a prebuilt
beam for runtime, `--emit-core` of a 500-call file for
compile; same machine, `5943095+dirty`):
default-build runtime devirt 0.835 (0.008) vs dispatch
0.865 (0.018) = **−3.5%** (ranges disjoint); optimized
(`-O3 -DNDEBUG`) runtime 0.818 (0.012) vs 0.845 (0.011) =
**−3.2%**, confirming the win is in the emitted BEAM
(identical beams from both binaries). Compile cost of the
predicate: default build +30 ms on the 2000-call file
(+4.4%, ~15 µs/site — real files carry dozens of calls);
optimized build +1 ms, inside noise. The win is modest —
a dispatcher hop is one guard plus one jump — but real
and resolved on both builds. Deliberate v1 cuts (all
dispatcher-safe): sized numerics, unit, `Block`/`Optional`
nominals, unions, tuples/funcs, trait-flavored receivers,
gradual receivers, single-head names (already direct —
the definition kept the bare name), qualified ADT/distinct
heads, alias/opaque targets, foul, `''` peers, covering
module functions. v1.1 notes: an emitter-recorded
`writtenHead` per method (one string, format edit per the
schema policy) would close Optional-generic, Number/Float,
and sized heads with ground truth instead of proof.

**Alternative (rejected, quantified):** two interface entries per
colliding method (dispatcher + mangled). Measured on the baseline
prelude build (77 units): 504 dispatcher+mangled export pairs
(476 by-receiver + 28 signature-mangled) across the runtime
beams; the `.bin` holds 2915 function entries at ~253 B each
(902 KB). Dual entries add 504 entries (+17% entries, +44% of
the receiver table): ~127 KB (+14.1% `.bin`), ~70 KB (+22% of
the 322 KB KexI). Permanent, paid by every consumer on every
warm load (SAX deserialize +~2.5 ms/compile forever), plus
lookup ambiguity at every consumer and producer-side emission
+ ownership semantics. Against that: reconstruction costs
zero interface/load, lives in one place extending the proven
`localDispatchTypes` precedent (`lowering_method_call.cxx:690`),
and the taxonomy's line-299 finding keeps the plain entry
mandatory anyway — dual-entry removes no complexity, only
adds selection complexity. 14%/22% permanent bloat is
material next to one-spot logic: rejection KEPT. Middle
path noted for design (not decided): one "has mangled def"
bit per method (~108 B, `kVersion` bump, safe via the miss
path) instead of full second entries, if the existence
question below needs a signal.

**Prerequisites, in order:**

1. Failure taxonomy of the 67 broken cases — DONE. Re-ran the first
   cut over-broad (every concrete make-block receiver mangled in
   `receiverFunctionFromDef`; 546 methods) with risk #1 fixed: 169
   spec-beam failures = 13 baseline net + 156 induced. Every
   top-level `undef` stubbed via a temporary runner change (since
   reverted): 105/105 name a mangled symbol absent from the
   runtime beams' exports, 0 plain, 0 existing.
   - (a) lost fallback: 48 files. (a1) 12 × "Undefined method"
     (incl. serving crash report): same-unit dispatcher fallback
     requires a plain-named provider
     (`lowering_make.cxx:299`, `beamFunction == first.name`).
     (a2) 31 × render divergence (`Just(4)` vs `4`): Optional/
     Result/show dispatch goes structural (3 also crash later
     with (c)). Exact micro-mechanism (winner vs dictionary)
     still open — diff a baseline-vs-experiment `.core` for
     `static_alias_calls` during design.
     (a3) 5 × `function_clause`/`badarg` on EXISTING symbols
     (`showValue/Optional`, `element`, `concat`): direct-routed
     to an impl the receiver falls through.
   - (b) risk-#1 class: 0. All 156 passed at baseline; no
     rejection symptoms anywhere; `static_alias_calls` (the
     risk-#1 regression spec) computes correct values and
     fails only on (a2) rendering.
   - (c) over-broad-cut noise: 108 files (102 stubbed undef +
     5 in-test `undef` assertions + 1 rescued inner `undef`
     via missing `set?/Optional`, proven end to end).
     A faithful colliding-only cut would not mangle these.
   Design input: 48 files need the dispatcher/dictionary
   fallback; the dual-entry alternative satisfies line 299's
   plain-provider rule by construction. Open side observation
   (not taxonomy-blocking): `--run` vs `--compile` diverged
   on `backend_foreign_receiver` under mangled metadata
   (same kex, same sources); `--run` is the gate's ground
   truth and all taxonomy evidence is `--run` evidence.
2. Rejected-alternative quantification — DONE (see above; rejection
   kept: +504 entries / +14% `.bin` / +22% KexI is material).
3. Risk #1 fixed (failing spec case + fix + targeted differential
   case done; filed as kexhq/kex#626).

### Low priority / deferred

- **Capability empty-context** — `currentContext()` emits `maps:new()` per
  foul-target call from a pure caller; `maps:new/0` returns the singleton empty
  map so the win is marginal, and the Core Erlang map-literal input syntax is
  non-obvious.
- **Trait-dictionary hoisting** — dictionaries are per-call-site closure
  tuples; could become per-(trait,type) constants. Not started.

---

## Deferred

- **`::` → `.` mangling flip** — DONE 2026-11-12. Not the one line it was
  called: besides `kSeparator`, the walker spelled the separator out in 434
  string literals (`"File::read"`, `typeName + "::compare"`), all rewritten.
  No golden changed: the separator never reached a BEAM symbol or a message.
  `manglePath`, `headOf`, `tailOf` and `pathScopeOf` are deleted (a path is
  its own spelling now, and a name is split at its last separator).

---

## Verification

Every change is gated on:

- `spec` (walker) — 644 golden files, byte-exact
- `spec-beam` (BEAM) — 558 golden files
- `make fuzz` — 400-case differential walker↔BEAM (grown from 200 ahead
  of the Phase 2 migration, risk #5) + targeted cases
  (`targetedCases` in `tools/fuzz.kex`, one per divergence found)
- `semantic_test` (296), `codegen_test` (131), and the unit suites below
- `make test-devirt` — every applied devirt spelling exists in its beam
- `make test-wasm` where wasm-relevant

Caveat (risk #5): the golden suites preserve current behavior of each backend
independently — including known divergence — so cross-backend correctness
rests on the fuzzer alone.

**`make bench`** (Phase 0b): repeat-and-median benchmark script (≥10 runs,
median + IQR; scenarios per the methodology section; dumps `KEX_TIMINGS`
phases for one warm run and parses cache decode/walk + miss reasons; exits 0).
Informational pre-merge ritual, not CI. No automated perf gate beyond this.

### Unit tests added for new modules

| Suite | Covers |
|---|---|
| `tests/mangle_test.cxx` | `mangle` helpers |
| `tests/string_utils_test.cxx` | `lowered`, `snakeCased` |
| `tests/diagnostics_test.cxx` | `jsonEscape`, `extractHint`, `errorCountPhrase`, type printers, `colorizeMessage` |
| `tests/interface_cache_test.cxx` | `ImportedInterfaces` serialize/deserialize round-trip + corrupted-input rejection |
| `tests/interface_cache_test.cxx` (3a) | schema agreement, null round-trip, miss reasons, key/stamp, eviction, concurrent-writer race (16 cases) |
| `tests/interface_cache_cli_test.cxx` (3a) | stale-key end-to-end (binary-mtime → new entry) |
| `tests/resolve_core_test.cxx` (Phase 2 step 1) | merge 5-way table + module-path resolve/relative/miss (11 cases) |
| `tests/resolve_core_test.cxx` (Phase 4) | `writtenTargetHead` / `signaturesAlphaEqual` / `receiverCoversHead` / `importDispatchSpelling` clause matrix + `KEX_NO_DEVIRT` (36 cases; 52 total) |
| `tools/devirt-correspondence.sh` (`make test-devirt`, CI-wired) | every applied `name/Head` over the spec corpus exists in the defining beam's exports (2663 firings, 87 distinct) |

---

## State snapshot

Commit-anchored; re-verify cited symbols at each commit in this plan's scope,
and cite the commit in every future measurement table. (No standing rule to
update this section on every staging change — that toil never happens; the
anchor below names the last verification point.)

- HEAD is `91693ad` on `refactor/kex-bloat`, rebased onto `origin/main`
  `66a5838` (net-text-api merge) at the user's request; the 9-commit
  stack (grand refactor through the risk-#9 rescue-guard fix) replayed
  with 2 mechanical resolutions (main's capability block-argument
  routing ported into the split `lowering_method_call.cxx`;
  `TraitClaim.fullTypeName` kept coherent). Working tree clean;
  PR kexhq/kex#616 open on this branch with the full description.
- Gates on the rebased tree: resolve_core 52/52, semantic 296/296,
  test-spec-beam-runner 6/6, ctest 35/37 (2 known env), spec 638+21
  env, spec-beam 551+21 env (21 = 13 pre-existing + 8 new net specs,
  all sandbox-`eperm`, proven byte-identical under stock `main` —
  no rebase fallout), fuzz 400/400 + targeted 4/4. Slick `tey test`
  121/121 green on both this branch and stock `main` (stock pins;
  the user's `package.local.kex` override targets are stale —
  `/private/tmp` checkouts gone — and were set aside for the runs).
- Branch-vs-main bench (12 runs, median(IQR), same machine, default
  builds): warm compile 0.257 (0.003) vs 0.599 (0.006), fib 1.700
  (0.019) vs 2.098 (0.009), loop 0.308 (0.005) vs 1.015 (0.021);
  cold-cache rebuild +0.11 s is the 932 KB interface-cache write
  that stock `main` never performs (no disk cache there — every
  main run pays full rebuild), amortized after one warm hit.
  `make test-wasm` not run
  (no pinned emsdk here; CI-only). `build-opt/` scaffolding removed.
  This doc is untracked in the working tree. The four owed GitHub
  issues are filed: kexhq/kex#626 (risk #1), #627 (walker trait-alias
  gap), #628 (risk #8), #629 (risk #9).
