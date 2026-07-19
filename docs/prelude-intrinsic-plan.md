# Prelude + Intrinsic architecture

## Goal

Move the Kex standard library out of hand-written native code (the tree-walker's
`src/interpreter/stdlib/*.cxx` builtins and the BEAM emitter's UFCS `if`-ladder
in `src/ir/lower.cxx`) and **into a Kex prelude**
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
to Erlang's. The shared Kex stdlib prevents backend drift.

## Validated (done)

The primitive boundary works end-to-end on **both** backends, function-agnostic:

- `Kex.Intrinsic.List.reverse([1,2,3])` → `[3,2,1]` on the walker (native
  builtin) and on the IR backend (→ `kex_intrinsic_list:reverse` → `lists:reverse`).
- `src/ir/lower.cxx`: `modulePath()` + generic rule mapping `Kex.Intrinsic.<Cat>.<fn>`
  → `call 'kex_intrinsic_<cat>':'<fn>'`.
- `src/interpreter/evaluator.cxx`: `MethodCall` dispatch recognizing the
  `Kex.Intrinsic.<Cat>.<fn>` module path → native builtin `<fn>`.
- `runtime/src/kex_intrinsic_list.erl` (currently just `reverse/1`).
- No regressions: `ctest` 10/10, `make spec` 106/106.
- The existing prelude files (`src/prelude/list.kex`, `algebra.kex`) already
  **lower cleanly under the IR backend** and type-check on the walker.

## Phase 0 + 1 done — the `reverse` vertical slice works

The full pipeline is proven end-to-end and green everywhere (the IR backend 92/106 with
zero invariant violations, `ctest` 10/10, `make spec` 106/106):

- `src/prelude/list.kex`: `let reverse = Kex.Intrinsic.List.reverse(this)`
  (illustrative O(n²) impl discarded).
- The whole prelude compiles into a shared **`kex_prelude`** module
  (`kex --build-prelude <dir>`), **prebuilt by CMake** into the runtime beam dir
  alongside the `.erl` beams, so BEAM **lazy-loads** it on first use. Per-run
  compile remains only as a fallback when the prebuilt beam is absent.
- User `x.reverse` routes to `call 'kex_prelude':'reverse'(x)` via
  `externalModules->receiverFunctions` (resolved from KexI interface at
  load time) or `resolvedCalls` (from semantic analysis).
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
verify parity. The `migratedPreludeFns()` set and `preludeFns` routing blocks
have been deleted; receiver calls route through imported interfaces generically.

## Decisions locked

- Prelude compiles once into a **shared `kex_prelude` BEAM module** (not inlined
  per user module). User modules call `kex_prelude:<fn>(...)`.
- Walker intrinsics stay in **C++**; prelude still calls them via `Kex.Intrinsic.*`.
- `Kex.Intrinsic.List` → runtime module name `kex_intrinsic_list` (lowercase,
  underscore-joined path).

## Phase status

### Phase 0 — make the prelude real runtime — DONE

The prelude compiles into `kex_prelude` (plus companion modules for explicit
`module` blocks). The walker executes prelude source at startup. Both backends
produce identical results.

### Phase 1 — the `reverse` vertical slice — DONE

Proved the full pipeline end-to-end.

### Phase 2 — rewrite the prelude as intrinsic wrappers — DONE

All prelude receiver functions and namespace modules call `Kex.Intrinsic.*`
instead of illustrative pure-Kex implementations.

### Phase 3 — fill in the intrinsics — DONE

20 intrinsic runtime modules (`kex_intrinsic_*.erl`) cover all primitives the
prelude needs. Walker native builtins back the same intrinsics.

### Phase 4 — delete the ladders — MOSTLY DONE

The `preludeFns`/`hofPreludeFns` routing blocks and `migratedPreludeFns()` are
deleted. Receiver calls route through `resolvedCalls` (semantic) or
`externalModules->receiverFunctions` (KexI). All namespace module hardcoded
dispatch removed except Supervisor, ENV, and Mock.FS (sub-module constructors).

Remaining inline lowerings (cannot be removed yet):
- Guard-safe BIF fallbacks (`even?`, `odd?`, `ok?`, `error?`, `none?`, `abs`,
  `alive?`, `in?`, `digit?`, `alpha?`, `space?`)
- File handle methods (`feed`, `readLine`, `writeLine`, `eof?`)
- Process methods (`send`, `link`/`unlink`, `await`)
- `.to(Type)` conversion
- `.or(default)` universal semantics

### KexI v3 — param names for named-arg reordering — DONE

`KexiExport.paramNames` carries parameter names through the binary interface.
The lowerer resolves named args for external module functions in all three
paths (method-call, bare-call, bare-name fallback). Stream.Sequence/Iterate
fully decoupled on both backends via this mechanism.

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
(`maps:from_list`). All green: the IR backend 92/106 zero violations, `ctest` 10/10,
`make spec` 106/106, walker == the IR backend on all map HOFs.

`reduce` itself is now intrinsic-backed (`Kex.Intrinsic.List.foldl` →
`lists:foldl`), the first discard of an illustrative pure-Kex impl. Every
Enumerable HOF routes through it, so this is iterative on both backends (the
old `1 + xs.count`-style recursion was non-tail on the walker → C++ stack risk
on large lists; BEAM's TCO masked it there). Kex's reducer is acc-first
`f(acc, elem)`; Erlang's `lists:foldl` is elem-first `fun(elem, acc)`, so the
intrinsic swaps the argument order at the boundary.

The IR backend's map `find`/`map`/`each` are deterministic
(sorted-canonical), matching the walker. The retired string emitter used raw
Erlang map order; removing it eliminated that backend discrepancy.

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

## Migrated intrinsics

### Receiver functions (UFCS methods)

- **List** (`kex_intrinsic_list`): reverse, sort, uniq, flatten, take, drop, zip,
  push, sum, product, indexOf, at, foldl (backs Enumerable.reduce), min, max,
  length (backs List.count), join/1,2, map, filter, each, find, flatMap, reject,
  all?, any?, count (with block), as_list
- **Map** (`kex_intrinsic_map`): keys, values, entries, merge, has?, put, delete,
  size (backs Map.count), fromEntries, mapValues, mapKeys
- **String** (`kex_intrinsic_string`): upperCase, lowerCase, trim, split/1,2,
  startsWith?, endsWith?, replace, replaceAll, contains?, chars, count, at,
  reverse, repeat, padLeft, padRight
- **Char** (`kex_intrinsic_char`): is_digit, is_alpha, is_space
- **Integer** (`kex_intrinsic_integer`): modulo; even?/odd? expressed in Kex
- **Number** (`kex_intrinsic_number`): to_integer, to_float
- **Range** (`kex_intrinsic_range`): items
- **Stream** (`kex_intrinsic_stream`): make, generate, take, drop, map, filter
- **Fun** (`kex_intrinsic_fun`): applyItem (auto-splat), or_else

### Namespace modules (companion modules on BEAM)

All hardcoded namespace dispatch removed from the lowerer. Calls route through
`externalModules->exportToBeamFn` (populated from KexI companion interfaces):

- **IO** (`kex_intrinsic_io`): printLine, print, putLine, put, inspect, getLine,
  get, printError, warn, warning, exit
- **Math** (`kex_intrinsic_math`): all math functions, PI/E as Kex constants
- **Console** (`kex_intrinsic_console`): color constants and functions
- **Process** (`kex_intrinsic_process`): self, exit, register, whereis
- **Task** (`kex_intrinsic_task`): start, awaitAll
- **Http** (`kex_intrinsic_http`): get/post/put/patch/delete/head/options (1+2 arity)
- **Stream**: Sequence, Iterate (via generate); named-arg reordering through
  KexI v3 param names
- **System** (`kex_intrinsic_system`): exit
- **File** (`kex_intrinsic_file`): all file operations
- **Directory** (`kex_intrinsic_directory`): all directory operations
- **Integer/Float/Number**: parse, parsePrefix functions
- **Mock.IO/Mock.Http**: fully decoupled via companion modules
- **ENV** (`kex_intrinsic_env`): get, getWithDefault, has?, keys, values,
  count, each, entries; interpreter keeps ENV as a plain MapValue with UFCS

Still hardcoded: **Supervisor** (IR-level map construction),
**Mock.FS** (sub-module constructors).

### Routing

Receiver calls route through `externalModules->receiverFunctions` (populated
from KexI interfaces at load time) or `resolvedCalls` (from semantic analysis).

Bare uppercase function calls (e.g. `Sequence(from: 0)`) resolve through
module registry exports on the interpreter, and through external module param
name fallback on BEAM.

**Guard-safety rule:** the external receiver dispatch path is gated on
`!m_inGuard`. A cross-module `call 'kex_prelude':…` is illegal inside a Core
Erlang guard, so guarded methods fall through to BIF-based lowerings.

## Maps: unordered with a canonical (sorted) order — DONE

Decision: **Map is unordered.** Erlang's map key order is non-deterministic
across VM invocations (identical Core Erlang printed both `[:m,:a,:z]` and
`[:a,:m,:z]`), so there is no stable native order to match. `keys`/`values`/
`entries` therefore expose a deterministic **canonical order: sorted by key**,
applied in all three backends so they agree:

- walker: `sortedEntries` in `src/interpreter/stdlib/map.cxx` (matches Erlang
  term order for the common homogeneous key types),
- `kex_intrinsic_map.erl`: `lists:sort(maps:keys(M))` etc.,
- BEAM IR: keys/values/entries delegate to `kex_intrinsic_map`.

Migrated map ops: keys, values, entries, merge, has?, put, delete. No `.expected`
changed (spec map data was already sorted-compatible); the IR backend had
92/106 zero violations, with no walker/BEAM divergence in the migrated cases.

Map HOFs (each/map/filter/reduce/find) now iterate in canonical sorted order
on both backends: the walker uses `sortedEntries`, and BEAM routes through
Enumerable's `reduce`-based defaults which call `kex_intrinsic_map:entries`
(sorted). A separate insertion-ordered map *type* is future work.

## Verification invariant (every step)

The IR backend and tree-walker must agree on every `spec/*.kex` (identical
pass/fail set) and every runnable `examples/*.kex`
(identical output, modulo opaque `#Fun`/`#PID` ids). Current: `ctest` 12/12,
`make spec` 125/125, prelude spec 292/292, spec-beam 120/123 (3 pre-existing
diffs). Anything the pipeline compiles is correct or fails loudly — never
silently wrong.
