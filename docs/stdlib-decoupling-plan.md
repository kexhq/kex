# Standard library decoupling migration

## Goal

Make Kex source the owner of the standard library while reducing the
tree-walking interpreter and BEAM compiler to language implementation plus a
small private runtime ABI. Public stdlib behavior, types, dispatch, and
composition must not be repeated in semantic tables, evaluator registrations,
lowering ladders, and Erlang wrappers.

The interpreter remains a supported parity backend. Breaking public stdlib
changes require one comprehensive contract RFC and explicit approval before
implementation. Later substantial stdlib redesigns require the same explicit
discussion.

Non-breaking architecture work can proceed before that RFC is approved, but it
must preserve current public behavior.

## Scope control

This migration has one critical path:

1. Prove and remove the duplicate BEAM emitter.
2. Generate interfaces from semantic analysis.
3. Package and install the stdlib as ordinary Kex modules.
4. Resolve calls from interfaces instead of compiler name tables.
5. Move public behavior into Kex domain by domain.
6. Delete each replaced native and compiler implementation.

Effect inference, generalized pure guards, and selective-receive changes are
related language work, but they are separate workstreams. They may proceed in
parallel and do not block packaging or migrating functions that are not used in
guards. The final removal of legacy guard-specific stdlib lowering waits for
the guard workstream, not the rest of the migration.

### Deferred Parser and Evaluator modules

`Parser` and `Evaluator` are explicitly out of scope. They embed the C++
frontend and tree-walker and need a separate design for portable runtime
parsing, evaluation scope, and sandbox semantics.

For this migration:

- Preserve their existing interpreter behavior, while allowing mechanical
  changes required by the new package and loader structure.
- Do not implement them on BEAM and do not let them block in-scope backend
  parity, IR-backend retirement, or native-builtin deletion.
- Exclude them from automatic portable stdlib imports.
- Represent backend availability in generic module/package metadata. A BEAM
  compilation that references either module must produce a semantic
  "unavailable on this backend" diagnostic, never a runtime `undef`.
- Keep their native implementation quarantined from IR lowering and the
  private-intrinsic examples used by this migration.
- Verify that no in-scope stdlib module depends on either deferred module.
- Mark `Evaluator` as experimental and not sandbox-safe while `allow`,
  `maxSteps`, and `maxDepth` remain unenforced.
- Keep their interpreter tests running so representation and loader changes do
  not silently break them.

Each module gets a separately approved future design with explicit BEAM parity,
security, and removal criteria. Deferral is not evidence that the current API
or implementation is the eventual contract.

## Architectural boundaries

### Language implementation

The irreducible implementation owns parsing and evaluation rules, primitive
value representations, function application, control flow, pattern matching,
records and variants, generic module calls, and process/mailbox machinery.

Knowing how a list is represented belongs here. Knowing about `List.map`,
`List.count`, `Optional`, `Web.Server`, assertions, or filesystem mocks does
not. Generic ADT and record representation may remain native; knowledge of
particular stdlib ADTs and records must not.

### Standard library package

Public modules, functions, receiver extension functions, traits, ADTs, records,
documentation, and type signatures live in Kex. The stdlib uses the same
resolver, analyzer, KexI interface model, and module lowering as user packages.

Automatic imports and receiver-function providers are declared in package
metadata. They are not inferred from filenames or maintained as compiler name
sets. Typed receiver-call resolution handles ownership and ambiguity.

The installed distribution is an OTP-style `.ez` application archive containing:

- authoritative `.kex` sources for the interpreter and inspection under the
  application source tree;
- `ebin/*.beam`, each containing its existing embedded KexI chunk;
- the generated OTP `.app` application resource in `ebin`.

Package policy, dependency identities, automatic providers, backend
availability, source digest, compiler artifact version, and private runtime ABI
are compiled into the entry module's versioned KexI structural metadata. They
are not a parallel sidecar format.

This migration introduces no Kex-specific object file, interface file,
metadata binary, helper executable, or additional external runtime dependency.
Installed compiled content is ordinary BEAM code inside the standard `.ez`
container.

KexI remains embedded interface metadata, not a new file or executable format.
The interpreter executes source and may cache parsed/analyzed ASTs in memory by
source digest. A persistent interpreter executable cache is a separate design.

Package installation is data movement only: verify the `.ez` hash, place the
archive in the immutable cache, and expose its application `ebin` path. There
are no preinstall, install, postinstall, build-on-install, activation, or other
lifecycle hooks. `package.kex` is parsed as declarative data and is never
evaluated as a Kex program.

### Private intrinsic ABI

`Kex.Intrinsic` is the only stdlib-to-runtime boundary. It contains operations
that cannot reasonably be expressed in Kex, such as OS access, VM primitives,
and selected efficient runtime operations.

Each intrinsic has a private declaration containing parameter types, return
type, arity, and effect. The interpreter registry keys implementations by full
intrinsic module, name, and arity. BEAM maps the same identity to runtime
modules through one generic convention. Neither checker nor lowering contains
per-intrinsic names.

Intrinsic access is a compilation capability assigned by the toolchain when it
loads the stdlib root from its installation/build configuration. It is not a
claim inside a user-editable package manifest, module name, or path heuristic.
User packages cannot shadow the reserved namespace or receive this capability.

Adding a normal stdlib helper changes Kex source and tests only. Adding a true
primitive changes its private declaration plus the interpreter and BEAM runtime
implementations, without adding semantic or lowering cases.

## Bootstrap and artifact model

The stdlib build must not require a previously compiled copy of itself:

1. The compiler starts with language primitive types and the private typed
   intrinsic declaration interface only.
2. It parses and analyzes all stdlib source modules as one dependency graph,
   resolving cycles through the normal declaration-before-body analysis.
3. The analyzed module interfaces are retained and emitted into their BEAM
   modules as KexI chunks.
4. User compilation loads these generated interfaces exactly as it loads other
   compiled package interfaces.
5. The interpreter loads the same source graph through the ordinary source
   resolver and caches its parsed/analyzed form for that process.

An interface hash detects dependency-contract changes, but not changed
function bodies. A cached stdlib BEAM is reusable only when all of these match:

- full digest of the authoritative stdlib sources and package metadata;
- compiler artifact-format version;
- private intrinsic/runtime ABI version;
- target backend/runtime version where representation depends on it.

A stale or missing development artifact is rebuilt explicitly by the build
pipeline. An installed toolchain with inconsistent artifacts fails with a
diagnostic rather than silently compiling the stdlib during every user run.

## Critical-path migration

### 1. Baseline and contract inventory

- Inventory every in-scope public function, type, receiver extension function, automatic
  import, mock, effect, error behavior, and backend discrepancy.
- Classify operations as language implementation, Kex stdlib, private
  intrinsic, or explicitly deferred.
- Build one public contract suite that runs unchanged on both supported
  backends and snapshot the current exported interface.
- Write the comprehensive stdlib contract RFC covering proposed removals,
  renames, behavior changes, and compatibility policy. Stop before its breaking
  portions until it is approved.

### 2. Retire the legacy BEAM emitter — completed

The string emitter, its dispatch ladder, backend-selection code,
`--legacy-emitter`, obsolete `--ir` switch, and emitter-only fixtures have been
removed. Useful structural and behavioral tests now exercise the production IR
pipeline. CI runs the supported BEAM surface exclusively through IR.

### 3. Generate interfaces from analyzed semantics

Progress: checked top-level, module, and receiver-function signatures now flow
directly from semantic analysis into KexI, including inferred return types.
Signatures are attached to their exact syntax declaration, so same-named
functions in separate modules and overload declarations cannot be confused by
the checker's unqualified call-resolution table. Unchecked compilation keeps
the syntax-based fallback. A stable manifest-supplied package identity is still
required before interfaces can drive package imports and call resolution.

KexI v2 now carries the first ownership layer explicitly: compilation-unit
identity, Kex-facing source module identity, backend module identity, and
emitted export names. The compiled-interface registry consumes these fields
instead of deriving them from `Kex.*` naming conventions. Version 1 artifacts
remain readable through conservative compatibility defaults. A future package
manifest can supply a stable package identity through the same unit field;
the standalone compiler currently uses the entry artifact identity.

- Make semantic analysis retain the checked public interface. KexI collection
  consumes it rather than reconstructing types syntactically from the AST.
- Represent exports, receiver functions, receiver types, generics, traits, records, ADTs,
  layouts, visibility, emitted ownership, and backend availability without
  stdlib-specific structures.
- Version KexI additions compatibly and treat absent required information
  conservatively.
- Feed the same interface registry to checking, completion, dispatch, record
  layout lookup, lowering, and the REPL.
- Remove the hardcoded stdlib signature table only after generated interfaces
  reproduce its accepted calls and diagnostics.

### 4. Turn the prelude into the installed stdlib package

Progress: the backend-independent package policy model now separates stable
package identity from compiled-unit identity. A package explicitly owns one or
more unit IDs and declares its automatic-import modules and receiver-function
provider modules. Declarations are validated against loaded source-module
identities; duplicates, missing modules, foreign units, and ambiguous ownership
are rejected. The compiled-interface registry searches receiver functions only
inside declared providers, so loading a module no longer enrolls it in a global
name-based extension pool. The source declaration belongs to `package.kex`;
the compiled copy will live in the entry module's versioned KexI structural
metadata. Entry-module KexI now serializes that policy directly, and the
compiled-interface registry validates and registers it when loading the unit.
No additional installed metadata file is required.

The prelude build now embeds `kex.stdlib` package policy in the entry module's
KexI structural metadata, declaring `"Prelude"` as the receiver-function
provider. The compiled-interface registry picks this up at load time, so
`buildExternalModules` and `buildSemanticInterfaces` populate receiver
functions through the standard package-declared path rather than a global
name-based fallback.

The prelude source dependency graph has been verified as a DAG with no
cycles, enabling tiered compilation:

- Tier 0 (no deps): algebra, console, errorable, io, math, optional,
  process, range, stream, system, test
- Tier 1 (Tier 0 only): blankable, env, http, number, parser, string,
  truthyable, web\_server
- Tier 2: enumerable (optional, range, stream); evaluator (optional,
  stream); file (stream)
- Tier 3: list (enumerable, optional, range); map (blankable, enumerable)

Splitting the merged prelude into separately-compiled tiers is the key
prerequisite for removing the remaining inline lowering section in
`lowerMethodCall` (Step 5). Currently all prelude receiver functions land
in `localMethods` because the prelude compiles as one unit, which gates
the `externalModules->receiverFunctions` path. Once tiers compile
separately, cross-tier receiver calls go through imported interfaces and
the inline lowerings become dead code.

- Split the merged prelude into ordinary modules with explicit public/private
  exports and a declared dependency graph.
- Represent receiver extension functions through KexI ownership and declared
  automatic providers, never a global function-name fallback.
- Build and validate cached BEAMs once through the artifact model above, then
  assemble the application as one `.ez` archive.
- Load source through the ordinary resolver for the interpreter and cached
  modules/interfaces through the compiled-module path for BEAM.
- Resolve installation and development package roots from toolchain
  configuration. Do not compile an absolute checkout path into `kex_lib`.
- Once both paths work, delete `KEX_PRELUDE_DIR`, merged-prelude compilation,
  AST lifetime/path special cases, prelude record/layout collectors,
  `migratedPreludeFns`, and per-run prelude compilation fallbacks.

### 5. Make call resolution interface-driven

Progress: the registry-to-IR snapshot now keeps ordinary module exports
separate from receiver functions. Only receiver functions owned by a
package-declared provider module are exposed to receiver-call lowering;
loading an ordinary export with the same name no longer grants it receiver
semantics. Arity filtering and deterministic ambiguity rejection happen at
this transitional boundary.

Semantic analysis now resolves prelude receiver function calls through the
imported interface and records their backend routing in `resolvedCalls`.
The lowerer consumes resolved targets before reaching any hardcoded
fallback, so calls that the typechecker covers go through a generic
interface-driven path with no hardcoded function names. KexI method
`paramTypes` now exclude the receiver (stored separately in `receiverType`),
fixing a double-receiver arity mismatch. Trait-constrained receiver types
in imported signatures widen to Unknown for type checking until the trait
conformance system is fully wired to the static checker. The stdlib
signature table still shadows imported sigs at matching arities to preserve
hand-tuned overload diagnostics; removing it requires KexI signatures to
reach parity with the stdlib table.

The `preludeFns` parameter and all four of its routing blocks in
`lowerMethodCall` have been removed from the lowering API. The
`resolvedCalls` path (which carries full backend routing from semantic
analysis) handles all checked calls. The `externalModules->receiverFunctions`
path (which checks arity) and the `externalModules->exportToBeamFn` path
handle the same calls as fallbacks in unchecked (`--no-check`) mode. Codegen
tests now exercise the `externalModules` path directly. The
`migratedPreludeFns()` function and the `routedFunctions` member of the
prelude interface cache have been deleted as dead code.

The `preferExternalReceivers` flag in the lowerer bypasses the
`localMethods` gate during prelude self-compilation, routing internal
receiver calls through `kex_prelude:method/N` self-calls instead of inline
lowerings. This eliminates `lists:foreach`, `lists:map`, `lists:filter`,
and `lists:foldl` from `kex_prelude.core` entirely.

Removed inline lowerings:
- Block/HOF section (`each`, `map`, `filter`, `reduce`, `find`, `mapValues`,
  `mapKeys`, `reject`, `flatMap`, `count` with blocks) and `hof2Name`
- `calls[]` prelude entries (`product`, `sum`, `sort`, `uniq`, `abs`, `sqrt`,
  `inspect`, `upperCase`, `lowerCase`, `trim`, `at`)
- Standalone prelude lowerings: `modulo`, `push`, `contains?`, `indexOf`,
  `rest`, `even?`/`odd?` (non-guard), `ok?`/`error?` (non-guard), `alive?`
  (non-guard), `.or` (match-based)
- Dead safety nets: `count`/`length`/`size`, `first`, `none?`, `get` (1-arg
  and 2-arg), `empty?` (all had `!localMethods` guards, unreachable with
  external dispatch)

Remaining inline lowerings that cannot be removed yet:
- `.or()` hardcoded (`kex_intrinsic_fun:or_else`) — universal semantics
  for non-Optional/non-Result receivers that the prelude's typed `or/2`
  doesn't cover
- Guard-safe BIF fallbacks (`even?`, `odd?`, `ok?`, `error?`, `none?`,
  `abs`, `alive?`, `in?`, `digit?`, `alpha?`, `space?`) — external dispatch
  is skipped in guards, so BIF-based lowerings are required
- File handle methods (`feed`, `readLine`, `writeLine`, `eof?`) — not
  prelude receiver functions
- Process methods (`send`, `link`/`unlink`, `await`) — not prelude
  receiver functions
- `.to(Type)` — not a prelude receiver function; argument is a type name,
  not a value

HOF performance intrinsics: after removing the inline block/HOF lowerings,
List HOFs (`map`, `filter`, `each`, `find`, `flatMap`, `reject`, `all?`,
`any?`, `count`) fell back to the Enumerable trait's `reduce`-based
defaults. Fixed by adding BIF-backed intrinsics in
`runtime/src/kex_intrinsic_list.erl` and direct overrides in
`src/prelude/list.kex`'s List make block, bypassing the trait path
entirely for lists.

Math module fully decoupled: prelude `module Math` now has `let`
implementations that call `Kex.Intrinsic.Math.*`; `kex_intrinsic_math.erl`
has all BIF wrappers; interpreter `math.cxx` has bare-name intrinsic
aliases. Case-insensitive aliases (`pi`/`PI`, `power`/`pow`) removed —
names are strictly case-sensitive. Constants `PI` and `E` are literal
floats in Kex source. The `flattenModules` flag is now set during prelude
metadata collection so module function exports appear in the KexI chunk.
Hardcoded namespace dispatch removed from `lower.cxx` for all decoupled
modules (IO, System, Math, Console, Process, Task, Http, File, Directory,
Integer, Float, Number). Namespace calls now route through companion
module exports via `externalModules->exportToBeamFn`. The `nsCall` lambda
is deleted. Codegen tests updated to verify companion module routing
(`Kex.IO:printLine` etc.) instead of direct runtime calls
(`kex_io:print_line`). Only Supervisor and ENV retain hardcoded dispatch.

Native-wins guard in the interpreter: `execModule` now skips prelude
`let` definitions when a native builtin already exists for that
`Module::function` key in `m_intrinsicEnv`. This lets the prelude carry
Kex.Intrinsic.* implementations for BEAM without overwriting the
interpreter's native builtins (which may be variadic or capture evaluator
state).

Namespace modules with prelude implementations:
- **Math**: fully decoupled (see above)
- **IO**: `let` implementations calling `Kex.Intrinsic.IO.*`;
  `kex_intrinsic_io.erl` wraps `kex_io`; interpreter natives win
- **System**: `let exit(code)` calling `Kex.Intrinsic.IO.exit`
- **Console**: all color constants and functions as `let` definitions
  calling `Kex.Intrinsic.Console.*`; interpreter natives win
- **Process**: `module Process` added with `self`, `exit`, `register`,
  `whereis` calling `Kex.Intrinsic.Process.*`; `Process::self` native wins
  on interpreter

- **Http**: all 7 HTTP methods (get/post/put/patch/delete/head/options)
  with 1- and 2-arity overloads calling `Kex.Intrinsic.Http.*`;
  `kex_intrinsic_http.erl` wraps `kex_http`; interpreter natives win

- **Task**: `start` and `awaitAll` calling `Kex.Intrinsic.Task.*`;
  `kex_intrinsic_task.erl` wraps `kex_task`; interpreter natives win

- **File/Directory**: top-level `foul module File` and `foul module Directory`
  added alongside `module FS` declaration block; `kex_intrinsic_file.erl` and
  `kex_intrinsic_directory.erl` wrap `kex_file`; interpreter natives win
- **Integer/Float/Number**: `module Integer`, `module Float`, `module Number`
  blocks added to `number.kex` with parse/parsePrefix functions calling
  `Kex.Intrinsic.*`; `kex_intrinsic_float.erl` created; camelCase aliases
  added to existing BEAM modules; interpreter natives win via guard

Namespace modules that remain hardcoded (cannot be simple intrinsic calls):
- **Supervisor**: `start` needs named-arg destructuring and map
  construction at the IR level
- **ENV**: not a module — the lowerer constructs `kex_io:env_map()` and
  inlines map operations with Just/None wrapping

- Resolve module calls and UFCS receiver functions during semantic analysis using receiver
  types, imports, visibility, and interface ownership.
- Carry the resolved module, emitted function, arity, and receiver substitution
  into typed IR.
- Lower resolved calls generically. Remove searches and special sets for
  migrated receiver functions, HOFs, Web routes, Mock modules, and record helpers.
- Diagnose ambiguous ownership, unknown receivers, unavailable modules, and
  private intrinsic access before lowering.

Existing guard-only lowering may remain temporarily in a clearly isolated
compatibility component. It must not participate in ordinary call resolution,
and it is removed when the pure-guard workstream reaches parity.

### 6. Migrate and delete by vertical slice

Separate intrinsic registration from the interpreter's public environment so
Kex wrappers cannot shadow primitive lookup. Load stdlib sources as normal
package dependencies. Preserve generic native representations while removing
knowledge of named stdlib APIs and ADTs.

Migrate foundations first: ADTs, truth/blankness, numbers, collections,
strings, ranges, traits, and enumeration. Then migrate effectful domains: IO,
environment, files/directories, processes, streams, math, HTTP/Web, testing,
and mocks. Parser and Evaluator remain excluded.

For every domain:

1. Implement the approved public contract and types in Kex.
2. Add only primitives that cross a genuine backend boundary.
3. Add intrinsic contract tests and shared public parity tests.
4. Route all ordinary public use through the Kex implementation.
5. Delete replaced C++ public builtins, semantic entries, ordinary IR cases,
   Erlang wrappers, compatibility aliases, and obsolete tests in the same
   slice.
6. If a receiver function still has a temporary guard lowering, record that sole remaining
   reference in the guard workstream and prevent it from serving ordinary
   calls.

`Mock.FS` and `Mock.Http` become normal Kex APIs over private runtime state
primitives while preserving the approved behavior. Web routing and response
composition live in Kex; native code retains networking and server lifecycle
primitives. Neither checker nor ordinary lowering knows these public names.

### 7. Critical-path dead-code gate

- Remove empty forwarding files rather than retaining shells.
- Search for deleted flags, stdlib names, prelude paths, dispatch sets, and old
  runtime entry points.
- Use compiler warnings, reference searches, clean linking, and dead-code
  reporting where available.
- Delete transitional tests only after backend-neutral contract or IR tests
  represent their behavior.
- Update build, installation, and architecture documentation to describe only
  supported paths and explicitly deferred capabilities.

## Parallel language workstream: effects and pure guards

This workstream removes the final guard-specific stdlib coupling without
blocking the package migration.

- Compute effects transitively across the call graph, including recursive
  strongly connected components and imported interfaces.
- Extend KexI exports and receiver functions with versioned effect summaries. An absent or
  unknown effect is conservatively foul in a guard.
- Permit any proven non-foul expression or function call in a Kex guard.
- Reject direct and transitive foul calls during semantic analysis.
- Lower BEAM-eligible typed IR operations as native guards only as an
  optimization.
- Lower other pure guards to a decision tree or generated clause continuation
  that evaluates the subject once, preserves bindings, and reaches remaining
  clauses without duplicating their code.
- Audit whether runtime errors propagate or fail the clause and put any change
  to that behavior through the language/contract approval process.
- Delete the isolated compatibility guard lowerings once parity tests cover all
  former cases.

The backend eligibility pass recognizes typed IR operations and primitive
properties, never public stdlib names.

## Parallel language workstream: selective receive

General pure guards for ordinary match/function clauses do not wait for receive
redesign. Arbitrary non-foul predicates in selective receive require a
persistent logical queue because consuming and re-sending a BEAM message changes
ordering.

- Generated receive clauses provide a pure matcher returning no match or the
  selected clause data.
- The runtime scans retained messages before reading new BEAM mailbox messages.
- Unmatched messages retain order; new arrivals append after them; a successful
  match removes only the selected message.
- Timeout accounting uses one deadline across scanning and predicate
  evaluation.
- Raw messages sent to a Kex process enter the logical queue in BEAM arrival
  order.
- The interpreter and BEAM run the same ordering, timeout, and false-predicate
  contract tests.

Native receive guards remain an optimization only when proven equivalent to
the logical queue. Until this workstream is complete, selective-receive guards
retain their existing supported subset with an explicit diagnostic for other
pure calls.

## Automated architecture tests

- Compile a temporary stdlib package fixture containing a previously unknown
  module function and receiver extension function; both backends must use it without any
  compiler registration.
- Compile a fixture adding a new private intrinsic through the declared ABI;
  semantic and IR sources must not change to recognize its name.
- Reject a user package that declares `Kex.Intrinsic`, copies stdlib metadata,
  or requests the intrinsic capability.
- Reject backend-unavailable modules during semantic analysis.
- Change only a stdlib function body and prove that the cached BEAM is rejected
  even though its KexI interface hash is unchanged.
- Build and run an installed layout outside the source checkout.
- Enforce dependency layering so compiler targets cannot include stdlib source
  files or native public builtin registries through anything except the generic
  package and intrinsic interfaces.
- Maintain an allowlisted source audit for temporary guard compatibility names;
  the list must shrink and reach zero when the guard workstream completes.

## Completion criteria

### Critical path

- Interpreter and BEAM pass the same in-scope semantic, contract, spec, example,
  module, REPL, process, Web, testing, filesystem-mock, and HTTP-mock suites.
- Parser/Evaluator tests remain green on the interpreter, while BEAM references
  receive a compile-time availability diagnostic.
- A clean installed build works outside the checkout. Changing a stdlib body
  invalidates its cached BEAM even when the public interface is unchanged.
- Adding a Kex-only helper changes no compiler or backend runtime code.
- Adding an intrinsic changes its declaration and two runtime implementations
  without adding semantic or lowering cases.
- User packages cannot access or spoof intrinsic identity or capability.
- No hardcoded in-scope public signatures, layouts, ordinary calls, Web routes,
  Mock operations, or migrated-function sets remain in the compiler.
- The legacy emitter, selection flags, baked paths, duplicated native public
  implementations, and replaced compatibility paths are absent.

### Full decoupling

- Direct and transitive foul calls are rejected in guards; all other Kex guards
  have backend parity.
- No public stdlib names remain in guard lowering.
- Selective receive preserves unmatched messages, arrival order, timeouts, and
  raw-message interoperability for arbitrary pure predicates.
- The temporary guard compatibility allowlist is empty and deleted.

Measure compiler size, compilation time, interpreter startup, and cached stdlib
load time before and after each workstream. Material unexplained regressions
block completion even when behavior tests pass.
