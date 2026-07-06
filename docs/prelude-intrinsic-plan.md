# Prelude + Intrinsic architecture

## Goal

Move the Kex standard library out of hand-written native code (the tree-walker's
`src/interpreter/stdlib/*.cxx` builtins and the BEAM emitter's UFCS `if`-ladder
in `src/codegen/core_erlang.cxx` / `src/ir/lower.cxx`) and **into a Kex prelude**
(`src/prelude/*.kex`), written on top of a small **intrinsic** primitive layer.

Two backends, one shared stdlib:

- **The prelude** (`src/prelude/*.kex`) is the typed stdlib, written in Kex.
  Types, dispatch, generics all live here. Each function is a thin wrapper over
  `Kex.Intrinsic.*` (the illustrative pure-Kex impls currently in `list.kex` are
  to be **discarded** — we want the optimized intrinsic-backed versions).
- **`Kex.Intrinsic.<Category>`** (`List`, `Map`, `String`, …) is the primitive
  boundary — the *only* place backend primitives are named. Implemented once
  **per backend**:
  - tree-walker: native C++ builtins (stay in C++).
  - BEAM: `runtime/src/kex_intrinsic_<cat>.erl`, thin wrappers over Core Erlang
    BIFs (`reverse(L) -> lists:reverse(L)`).
- The compiler/evaluator are **function-agnostic**: a call into `Kex.Intrinsic.*`
  is routed generically (module-path → cross-module call on BEAM; → native
  builtin dispatch on the walker). Adding a primitive = one runtime function
  (+ one C++ builtin if new). **Zero compiler changes per stdlib function.**

Why: Kex semantics are defined in Kex, not inherited from Erlang. We still get
BEAM's optimized BIFs as an implementation detail, without coupling Kex's meaning
to Erlang's. And the two emitters stop drifting — there's one stdlib.

## Validated (done)

The primitive boundary works end-to-end on **both** backends, function-agnostic:

- `Kex.Intrinsic.List.reverse([1,2,3])` → `[3,2,1]` on the walker (native
  builtin) and on `--ir` (→ `kex_intrinsic_list:reverse` → `lists:reverse`).
- `src/ir/lower.cxx`: `modulePath()` + generic rule mapping `Kex.Intrinsic.<Cat>.<fn>`
  → `call 'kex_intrinsic_<cat>':'<fn>'`.
- `src/interpreter/evaluator.cxx`: `MethodCall` dispatch recognizing the
  `Kex.Intrinsic.<Cat>.<fn>` module path → native builtin `<fn>`.
- `runtime/src/kex_intrinsic_list.erl` (currently just `reverse/1`).
- No regressions: `ctest` 10/10, `make spec` 106/106.
- The existing prelude files (`src/prelude/list.kex`, `algebra.kex`) already
  **lower cleanly under `--ir`** and type-check on the walker.

## Phase 0 + 1 done — the `reverse` vertical slice works

The full pipeline is proven end-to-end and green everywhere (`--ir` 92/106 with
zero invariant violations, `ctest` 10/10, `make spec` 106/106):

- `src/prelude/list.kex`: `let reverse = Kex.Intrinsic.List.reverse(this)`
  (illustrative O(n²) impl discarded).
- The whole prelude compiles into a shared **`kex_prelude`** module
  (`kex --build-prelude <dir>`), **prebuilt by CMake** into the runtime beam dir
  alongside the `.erl` beams, so BEAM **lazy-loads** it on first use. Per-run
  compile remains only as a fallback when the prebuilt beam is absent.
- User `x.reverse` routes to `call 'kex_prelude':'reverse'(x)` (via the
  `migratedPreludeFns()` set in main.cxx + the `preludeFns` route in
  `src/ir/lower.cxx`, with `reverse` removed from the emitter ladder).
- `kex_prelude:reverse` → `Kex.Intrinsic.List.reverse` → `kex_intrinsic_list:reverse`
  → `lists:reverse`. Walker: same prelude, intrinsic → native C++ builtin.

Enabling fixes made along the way (general, not reverse-specific):
- make-block/function grouping now keys on **(name, arity)** — the prelude
  overloads by arity (`count/1` vs `count/2`, `join/1`/`join/2`).
- duplicate function definitions (same name+arity) are dropped — the prelude
  legitimately repeats methods across make blocks (`identity`/`combine` for
  Integer under both Monoid and Group in algebra.kex).
- `compilePreludeCore` probes each prelude file and omits ones that don't lower
  yet (only `file.kex`, which uses nested `module`-in-`module`).

**Migrating another method now = 3 lines:** add its prelude wrapper (calling an
intrinsic), add its intrinsic (`kex_intrinsic_*.erl` + native builtin if new),
add its name to `migratedPreludeFns()` and remove it from the ladder. Verify
parity.

## Decisions locked

- Prelude compiles once into a **shared `kex_prelude` BEAM module** (not inlined
  per user module). User modules call `kex_prelude:<fn>(...)`.
- Walker intrinsics stay in **C++**; prelude still calls them via `Kex.Intrinsic.*`.
- `Kex.Intrinsic.List` → runtime module name `kex_intrinsic_list` (lowercase,
  underscore-joined path).

## Remaining phases

### Phase 0 — make the prelude real runtime (the prerequisite)

Today `loadPrelude` only feeds the SemanticDB (name/type checking). Neither
backend executes/compiles the prelude — `5.combine(3)` and prelude-`reverse`
don't run anywhere. Wire it in:

- **BEAM**: compile `src/prelude/*.kex` (merged) into a `kex_prelude` module.
  Reuse `ir::lowerProgram(preludeProgram, "prelude")` with module name
  `kex_prelude`; build it into `runtime/beam/` like the other runtime modules.
  Then user-module codegen routes UFCS calls whose name is a prelude export to
  `call 'kex_prelude':'<fn>'(args)` instead of a local `apply`. Needs the set of
  prelude-exported function names available at compile time.
- **Walker**: load + execute the prelude Program into the evaluator's global env
  at startup (before running the user file), so prelude functions are defined.

Constraint: must not break the 106 specs. The prelude must produce **identical**
results to today's native builtins before any builtin is removed. So the order
within a method is: prelude wrapper + intrinsic land first (both backends,
parity green) → *then* remove the old native builtin / emitter handler.

### Phase 1 — the `reverse` vertical slice (proves Phase 0)

One method, all the way through: prelude `let reverse = Kex.Intrinsic.List.reverse(this)`
→ intrinsic (both backends) → `[1,2,3].reverse` resolves to the prelude → old
C++ `reverse` builtin and `--ir` `.reverse` table entry removed → parity green on
both backends and the spec/example suites. The rest is repetition of this slice.

### Phase 2 — rewrite the prelude as intrinsic wrappers

Replace the illustrative pure-Kex impls in `src/prelude/*.kex` with
`Kex.Intrinsic.*` calls (keep the `:>` type signatures). Add prelude files for
`map`, `string`, etc. as needed.

### Phase 3 — fill in the intrinsics

Implement every primitive the prelude needs, both backends:
`kex_intrinsic_list.erl` / `kex_intrinsic_map.erl` / `kex_intrinsic_string.erl`
(BEAM) and the corresponding native builtins (walker — mostly already exist).
Includes low-level tests (`is_list?`, `is_map?`) for the polymorphic stdlib fns
(`contains?`, list-vs-map `map`, Optional-wrapping `get`).

### Phase 4 — delete the ladders

Remove the native builtin stdlib and the emitter UFCS `if`-cascades
(`core_erlang.cxx` + the `src/ir/lower.cxx` value-receiver method handlers) as the
prelude covers them, verifying parity at each removal. Endgame: the emitters know
only language constructs + the generic intrinsic/prelude-call rules; all stdlib
lives in Kex.

## Enumerable trait (polymorphic HOFs, one definition) — DONE for map/filter/each

`src/prelude/enumerable.kex` defines the HOFs **once** in terms of `reduce` (the
universal fold). List, Range, and Map all get them:
- `make [X], implement: Enumerable` — `reduce` folds elements; ranges are lists
  at runtime so they inherit automatically.
- `make Map<K,V>, implement: Enumerable` — `reduce` folds **sorted** `entries`
  (canonical order).
- Dispatch is by runtime type (`is_list`/`is_map`) via the primitive-type
  dispatcher; the same default body serves both.
- **Auto-splat**: `Kex.Intrinsic.Fun.applyItem(f, item)` (`kex_intrinsic_fun.erl`)
  spreads a 2-tuple item into a two-arg block, so `list.map { |x| }` and
  `map.map { |k,v| }` both work.

Enabling fixes: `simpleTypeName` now names list types "List" (so list-vs-map
methods collide and dispatch); the walker's map HOFs (`map`/`each`/`filter`/…)
iterate `sortedEntries` to match the BEAM canonical order; and the migrated
map/list/string ladder handlers were **removed** (Phase 4) so the prelude's own
definitions aren't shadowed during prelude compilation.

Routed HOFs: `map`, `each`, `all?`, `any?`, `find` (uniform, from Enumerable
defaults) plus `filter`, `reject`, `mapValues`, `mapKeys` where **Map overrides**
the Enumerable default to return a Map (à la Ruby) — it enumerates sorted
`entries`, transforms, and rebuilds via `Kex.Intrinsic.Map.fromEntries`
(`maps:from_list`). All green: `--ir` 92/106 zero violations, `ctest` 10/10,
`make spec` 106/106, walker == `--ir` on all map HOFs.

`reduce` itself is now intrinsic-backed (`Kex.Intrinsic.List.foldl` →
`lists:foldl`), the first discard of an illustrative pure-Kex impl. Every
Enumerable HOF routes through it, so this is iterative on both backends (the
old `1 + xs.count`-style recursion was non-tail on the walker → C++ stack risk
on large lists; BEAM's TCO masked it there). Kex's reducer is acc-first
`f(acc, elem)`; Erlang's `lists:foldl` is elem-first `fun(elem, acc)`, so the
intrinsic swaps the argument order at the boundary.

Note: `--ir` map `find`/`map`/`each` are now *deterministic* (sorted-canonical),
matching the walker; the **default string emitter's** map HOFs still use Erlang's
non-deterministic map order, so they can differ from `--ir` on contrived inputs
(not exposed by the spec/example suites, and the default emitter is being
retired anyway). `--ir` matches the ground-truth walker — the right direction.

## Primitive-type dispatch (unblocks polymorphic methods) — DONE

The colliding-method dispatcher (`makeDispatcher`) used to tag-match
`element(1, arg)` — fine for records/variants, but crashes on primitive-type
receivers (a raw number/list has no tag). Rewrote it to **guard-dispatch on
runtime type**: primitives use the matching `is_*` BIF (`is_integer`,
`is_float`, `is_map`, `is_list`, …), records use `is_tuple(V) and element(1,V)
=:= 'T'` (all guard-safe, arity-agnostic). This let `abs` (defined on both
Integer and Float) migrate, and is the mechanism that will let the list-vs-map
HOFs dispatch by `is_list`/`is_map`. Existing record/variant dispatch
(type_dispatch, operator_overloading, traits) unchanged.

## Migrated so far (list + string + numeric)

- **List** (`kex_intrinsic_list`): reverse, sort, uniq, flatten, take, drop, zip,
  push, sum, product, indexOf, at, foldl (backs Enumerable.reduce), min, max
  (Just/None-wrapped, None-on-empty), length (backs List.count), join/1,2
  (backs [String|Char].join — was non-tail `x + sep + xs.join(sep)`). (Also moved
  list_get/index_of/list_product out of kex_io into here.)
- **Map** (`kex_intrinsic_map`): keys, values, entries, merge, has?, put, delete,
  size (backs Map.count, O(1) vs the old entries-build-then-count).
- **String** (`kex_intrinsic_string`): upperCase, lowerCase, trim, split/1,2
  (arity-1 with sep via `string:split`, arity-0 into individual chars via
  singleton-list wrap — was bodyless in the prelude, errored under `--ir`),
  startsWith?, endsWith? (the last two closed a real BEAM gap — they worked on
  the walker via native builtins but errored under `--ir` as "not yet ported";
  now `lists:prefix`/`lists:suffix` since Kex strings are charlists).
- **Char** (`kex_intrinsic_char`): is_digit, is_alpha, is_space (moved the
  method form to the prelude's `make Char`; the guard-inlined range checks in
  lower.cxx stay as the guard fallback; kex_io's copies remain for the default
  emitter's `&digit?` shorthand-lambda path).
- **Integer** (`kex_intrinsic_integer`): modulo; even?/odd? expressed in Kex on
  top of modulo.

Names live in `migratedPreludeFns()` (main.cxx); the value-receiver path in
lower.cxx routes them to `kex_prelude:<fn>` early, before the ladder.

**Guard-safety rule:** both routing branches (`preludeFns` and `hofPreludeFns`)
are gated on `!m_inGuard`. A cross-module `call 'kex_prelude':…` is illegal
inside a Core Erlang guard (guards only allow the guard-safe BIF subset), so a
migrated method used in a guard (`when s.count > 0`, `if x.isEmpty?`, …) must
fall through to its guard-safe ladder form (`erlang:length`, etc.) instead. A
real, reproduced regression otherwise: `count`'s arity unification made
`when_guards.kex`'s `s.count > 0` route to `kex_prelude:count` and erlc/runtime-
rejected the guard (`--ir` 92 → 91). `count`'s migration now exercises this live:
arity-0 `count` routes to the prelude (→ `Kex.Intrinsic.List.length` /
`Kex.Intrinsic.Map.size`) outside guards, and falls back to the ladder's
`erlang:length`/`maps:size` inside them.

## Maps: unordered with a canonical (sorted) order — DONE

Decision: **Map is unordered.** Erlang's map key order is non-deterministic
across VM invocations (identical Core Erlang printed both `[:m,:a,:z]` and
`[:a,:m,:z]`), so there is no stable native order to match. `keys`/`values`/
`entries` therefore expose a deterministic **canonical order: sorted by key**,
applied in all three backends so they agree:

- walker: `sortedEntries` in `src/interpreter/stdlib/map.cxx` (matches Erlang
  term order for the common homogeneous key types),
- `kex_intrinsic_map.erl`: `lists:sort(maps:keys(M))` etc.,
- default emitter: keys/values/entries delegate to `kex_intrinsic_map`.

Migrated map ops: keys, values, entries, merge, has?, put, delete. No `.expected`
changed (spec map data was already sorted-compatible); `--ir` 92/106 zero
violations, walker/`--ir`/default all agree, examples zero divergence.

Caveat: the map **HOFs** (each/map/filter/reduce/find) still iterate in raw
(walker-insertion / BEAM-native) order, not the canonical sort — a latent
inconsistency not exposed by the suite. Canonicalize them too when those get
migrated. A separate insertion-ordered map *type* is future work.

## Verification invariant (every step)

`--ir` and the default string emitter and the tree-walker must agree on every
`spec/*.kex` (identical pass/fail set) and every runnable `examples/*.kex`
(identical output, modulo opaque `#Fun`/`#PID` ids). `ctest` 10/10 and
`make spec` 106/106 stay green. Anything the pipeline compiles is correct or
fails loudly — never silently wrong.
