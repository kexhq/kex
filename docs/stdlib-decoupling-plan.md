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

KexI v4 adds trait definitions alongside the existing generic ADT constructor
sets in structural metadata. The compiled-interface registry now projects both
into the backend-neutral semantic snapshot. Match exhaustiveness no longer seeds
`Optional`, `Result`, `Comparison`, or `Either` by name in the typechecker;
those closed constructor sets come from the prebuilt stdlib interface. Imported
trait definitions retain required method names, signatures, and foulness, so a
dependent package can validate `make ..., implement:` without reparsing the
provider's source. `Optionable`, `Resultable`, and `Eitherable` plus their ADT
conformances now live in `optional.kex`; their transitional named-ADT registry
bridge has been deleted. Primitive structural trait membership remains part of
the language implementation. The shared `either.kex` contract test exercises
the source-owned constructors and matching on both the walker and BEAM. The
walker no longer registers `Left` or `Right` natively; loading `optional.kex`
is their only interpreter definition. Native `ok?`, `error?`, `some?`,
`present?`, and `none?` registrations have also been deleted; ordinary walker
calls now execute the same pattern-matching Kex methods as BEAM. The universal
raw-value behavior of `.or(default)` remains an explicitly tracked fallback.
Native `even?`, `odd?`, and `empty?` registrations have likewise been removed:
ordinary walker calls now use the Kex definitions in `number.kex`, `list.kex`,
`string.kex`, and `map.kex`. Guard-specific BEAM lowering remains tracked
separately until the generalized pure-guard work is complete. List and String
`second`, `third`, and `rest` are now explicit Kex wrappers composed from the
private `List.first` and `List.drop` intrinsics; their public native
registrations have been deleted while preserving String/[Char] behavior and
making both receiver families visible in generated interfaces.

The walker resolves private intrinsics by category-qualified identity. List,
String, Range, and Stream publish qualified registry entries, so overlapping
names such as `drop`, `take`, `map`, and `filter` no longer depend on
registration order or on one category's implementation emulating another
category. The general bare intrinsic compatibility bridge has been eliminated;
the bare names that remain are intentional language, callback, testing DSL, or
documented scheduler-fallback surfaces. The walker resolves bare UFCS calls
generically through the receiver's Kex method when no free function exists.
Range explicitly implements Enumerable via
an `items`-backed `reduce`, while String now owns its Enumerable conformance and
string-preserving `map`/`filter` wrappers in Kex source. Neither receiver family
therefore depends on native bare-name dispatch. All operations captured under
the qualified `List::*` private intrinsic registry now have their bare native
registrations removed, including the collection folds, predicates, ordering,
selection, and transformation helpers. Public receiver and bare-UFCS calls
route through Kex methods; their native implementations are reachable only
through qualified private intrinsic identities. The runtime now installs those
identities directly rather than snapshotting temporary bare globals; obsolete
native `reduce` and `collect` implementations have been deleted because their
source implementations already own those APIs. Universal `to` and `inspect`
remain intentional public compatibility functions. With migrated domains now
registering privately from the outset, the evaluator's bulk post-registration
erasure list has also been deleted. Range's established numeric
`sum` and `product` surface is now implemented directly from its Kex `reduce`
primitive rather than inherited accidentally from bare List dispatch. Map's
primitive registry is likewise category-qualified and its bare aliases are
removed; String's established `length` compatibility spelling is now an
explicit Kex method backed by the private List length intrinsic, and its
string-preserving `uniq` behavior is likewise source-owned. String-only
primitive aliases (`chars`, containment, prefix/suffix tests, splitting, and
trimming) have been removed from the bare registry. These String primitives,
plus the shared List/String `at` and `reverse` operations, now register directly
under their qualified private identities rather than passing through temporary
bare globals. Range containment is now a
Kex method over `items`, and Char predicate intrinsics have explicit
`Char::is_*` private identities while retaining their intentional bare callback
surface. Those two identities are now registered explicitly rather than by
copying the temporary public binding. Numeric receiver primitives now have explicit `Integer::*` and
`Number::*` private identities; bare aliases for modulo, absolute value, square
root, repetition, range membership, and rounding have been removed in favor of
their Kex receiver methods. The source-owned numeric primitives register
directly under those private identities instead of being copied from temporary
bare globals. Walker receiver classification now includes Char,
so Char membership and predicate calls dispatch to the source-owned `Char::*`
methods instead of depending on bare native fallthrough.
Math's interpreter registry now exposes only its qualified `Math::*` intrinsic
identities. The duplicated bare trigonometric/logarithmic aliases are removed,
and `Math.abs`, `Math.floor`, and `Math.ceil` explicitly share the qualified
Number implementations rather than relying on the retired bare numeric bridge.
Stream construction and Range item materialization no longer retain bare
`generate` or `items` aliases. Collection `reduce` dispatches through the
source-owned receiver implementations, List's optimized `find` and `flatMap`
paths now have qualified private identities and explicit Kex wrappers, and
`collect` uses the shared Enumerable source implementation. Their transitional
bare aliases have all been removed.
Map primitives likewise register directly under private `Map::*` identities;
the runtime no longer builds those entries by overriding shared bare List and
String functions and copying the resulting wrappers.
Char now owns `upperCase` and `lowerCase` Kex methods that reuse the qualified
String intrinsic ABI while preserving Char results. This removes the last bare
String case-conversion aliases, including their former use by shorthand mapping
callbacks.
Mock IO and HTTP control primitives now register only as qualified `IO::ioMock*`
and `Http::mock*` intrinsic identities. Public fixed-arity `Mock.IO` and all
`Mock.Http` calls execute source wrappers; only Mock.IO's unbounded variadic
`input` compatibility binding remains public-native. Transitional bare control
functions are gone.
Kex feature probes now use qualified `Kex::featureHas?` and
`Kex::featureList` intrinsic identities. The walker also classifies native Pid
values for source method resolution; `send`, `link`, `unlink`, and `alive?`
now enter the runtime only through qualified `Process::*` identities, with
their bare native aliases removed. The `timeout` keyword is accepted as a
parameter/value name outside receive-clause syntax, so both source-owned Task
overloads survive parsing and own BEAM dispatch. The walker retains its
isolated bare `await` fallback: awaiting yields the scheduler while a shared
evaluator environment frame is active, so routing that backend through the Kex
wrapper still requires process-local evaluator environments.
`Process.self`, `Task.start`, and `Task.awaitAll` remain public walker-native
scheduler fallbacks. Even a non-suspending wrapper may run in one of several
cooperatively scheduled processes and mutate the evaluator's shared environment
frame. Process exit/register/whereis remain BEAM-only capabilities; the
interpreter's feature probe reports Process unavailable.
The Enumerable callback adapter is now registered solely as the qualified
`Fun::applyItem` intrinsic. No public or prelude code depended on its former
bare registry name.
The interpreter-only Web server error boundary is likewise registered as
`Web::serve`; `Server.start` remains the sole public source entry point.
The interpreter now has a direct `defineIntrinsic` registration path. Private
Fun, Web, feature-probe, and mock-control implementations use it instead of
being inserted into the public environment and copied out later, beginning the
physical separation of primitive capabilities from public stdlib bindings.
Qualified receiver primitives for List, Map, String, Char, Range, Stream,
Number, Integer, and Process now use that direct registry as well. Cross-domain
sharing (notably Stream's List-backed map/filter implementation) reads from the
intrinsic environment rather than treating public bindings as a staging area.
This separation exposed List's historical `length` spelling as an accidental
public lookup of a private binding; List now declares the compatibility method
explicitly in Kex source.
The transitional `definePublicIntrinsic` dual-registration path has been
deleted after its final callers moved to source wrappers. Private primitives
now enter only the intrinsic registry. Intentional public-native compatibility
functions are registered explicitly by their owning domain and cannot
accidentally acquire intrinsic capability through a shared helper.
Ordinary function lookup no longer falls back to that private registry either;
category-qualified primitives are reachable only through the evaluator's
dedicated `Kex.Intrinsic.*` dispatch. A contract test verifies that an identity
such as `FS::file` is not callable as an ordinary namespace function. Toolchain
capability enforcement for direct `Kex.Intrinsic.*` syntax remains part of the
installed-stdlib/package phase.
Intrinsic registration now rejects identities without a category separator,
making the removal of the bare compatibility bridge an enforced invariant
rather than a convention.
Fully source-owned namespace domains no longer pre-install native `ModuleValue`
shells. Math, Console, Kex, HTTP, File, Directory, System, Mock, and Stream
namespace paths are published only by ordinary prelude module loading; their private
registrations contain functions, not public namespace state. IO and
Process/Task retain native shells solely for their documented public-native
compatibility operations.
FileHandle likewise relies on receiver-type method dispatch rather than a
native namespace placeholder, and Mock.IO's nested module identity comes from
the source declaration instead of a native function returning `ModuleValue`.
The remaining native `ModuleValue` entries are intentional: primitive type
tokens used as runtime values (`Integer`, `List`, `String`, and peers), deferred
Parser/Evaluator, and IO/process/supervisor namespaces that still own explicit
walker compatibility behavior.
IO/System, HTTP, Process/Task, and numeric parsing now explicitly publish their
dual public/intrinsic identities as well. Their unrelated public helpers are no
longer implicitly treated as intrinsic capabilities by domain registration.
File, FileHandle, and Directory operations now use the same explicit dual
registration path, so filesystem dispatch no longer depends on the
constructor's blanket public-environment clone.
With all source-backed intrinsic domains registered deliberately, evaluator
construction no longer copies the entire public environment into the private
intrinsic registry. Public-only native modules, test helpers, constructors, and
scheduler fallbacks therefore remain outside the intrinsic capability surface.
Native-versus-source module collision handling now recognizes public native
bindings directly as well as explicit qualified intrinsics. Public-only helpers
therefore preserve native ownership without gaining intrinsic reachability.
Module namespace installation likewise preserves an existing public non-module
binding directly; the `ENV` Map no longer relies on an accidental copy in the
intrinsic registry to survive loading the source `ENV` namespace declarations.

- Make semantic analysis retain the checked public interface. KexI collection
  consumes it rather than reconstructing types syntactically from the AST.
- Represent exports, receiver functions, receiver types, generics, traits, records, ADTs,
  layouts, visibility, emitted ownership, and backend availability without
  stdlib-specific structures.
- Version KexI additions compatibly and treat absent required information
  conservatively.
- Feed the same interface registry to checking, completion, dispatch, record
  layout lookup, lowering, and the REPL.
- Completed: the hardcoded stdlib signature table is gone. `TypeChecker`
  obtains namespace, automatic-import, and receiver candidates from the shared
  `ImportedInterfaces` snapshot built from prebuilt KexI metadata. Remaining
  name-specific checks implement language semantics (for example typed process
  messages), not duplicate stdlib declarations.

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

CLI compilation and semantic tests now share `common/prelude_interfaces.hxx`
for loading the prebuilt entry artifact and building the semantic snapshot.
The duplicate CLI registry loader and its separate error path have been
removed. Interface names, semantic checking, lowering routes, and record
layouts are all derived from one immutable, validated registry cached for the
process rather than reloading the BEAM independently for each view.

Prelude source discovery is now centralized in `common/prelude_loader.hxx`.
The interpreter, CLI semantic fallback, and native REPL consume the same sorted
source set and native-to-WASM fallback order instead of independently walking
`KEX_PRELUDE_DIR`. This isolates the remaining compiled development root behind
one function ahead of replacing it with installation-aware package roots.
Prelude compilation uses that shared file set as well, the interpreter no
longer compiles out source loading when the development macro is absent, and
sealed-method exemptions use exact discovered-file membership instead of an
absolute-path substring check. CLI prelude compilation and sealed-method
validation are no longer conditionally compiled behind `KEX_PRELUDE_DIR`; a
missing runtime source root now produces an explicit build-prelude diagnostic.
Runtime discovery also accepts `KEX_STDLIB_DIR` as its highest-priority source
root, providing an installation/toolchain override ahead of the temporary
compiled development fallback and embedded WASM path. Native discovery now
also checks executable-relative installed (`share/kex/prelude`) and development
(`src/prelude`) layouts. With those paths validated, the absolute checkout
`KEX_PRELUDE_DIR` definition has been removed from `kex_lib`; no source-tree
path is compiled into the runtime.

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
fixing a double-receiver arity mismatch.

The trait conformance system is now wired to the static checker: KexI
structural metadata carries `trait_conformances` collected from
`make ... implement:` declarations (with ADT constructors expanded from the
ADT's conformance), `buildSemanticInterfaces` exposes them through
`ImportedInterfaces::traitConformances`, and the typechecker registers them
in its `TraitRegistry` (including compound List/Map/Optional receivers).
Trait definitions themselves also cross the KexI v4 boundary, allowing the
checker to resolve imported trait names and validate their required methods.
Trait-constrained receiver types in imported signatures are no longer
widened to Unknown — `argMatchesParam` checks them through the registry.
Generic type variables in KexI signatures map to sequential negative ids
per signature, so diagnostics render them as A, B, ... like the hand-tuned
table did. Vacuous full matches (unannotated trait default methods) no
longer mask concrete receiver-matching signature mismatches, and duplicate
imported/stdlib candidates are listed once. The CLI Analyzer now receives
the prelude semantic interfaces, so plain `kex file.kex` checks against
imported signatures.

The stdlib signature table no longer shadows imported sigs at matching
arities; imported and table candidates merge (deduplicated) and drive
checking together. Removing the table entirely still requires auditing
that generated interfaces reproduce every accepted call and diagnostic it
covers.

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
  (non-guard), Task `await` (including named `timeout:` calls), `.or`
  (match-based)
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
- `.to(Type)` — not a prelude receiver function; argument is a type name,
  not a value

Compiled receiver interfaces now retain source parameter names (KexI v5),
excluding the receiver itself. Both registry-driven and semantically resolved
receiver calls reorder named arguments through that metadata. Consequently
Task `await` and Pid `send`, `link`, and `unlink` all route through their source
receiver methods and no longer have ordinary-call lowerings.

HOF performance intrinsics: after removing the inline block/HOF lowerings,
List HOFs (`map`, `filter`, `each`, `find`, `flatMap`, `reject`, `all?`,
`any?`, `count`) fell back to the Enumerable trait's `reduce`-based
defaults. Fixed by adding BIF-backed intrinsics in
`runtime/src/kex_intrinsic_list.erl` and direct overrides in
`src/prelude/list.kex`'s List make block, bypassing the trait path
entirely for lists.

Math module fully decoupled: prelude `module Math` now has `let`
implementations that call `Kex.Intrinsic.Math.*`; `kex_intrinsic_math.erl`
has all BIF wrappers; interpreter `math.cxx` exposes qualified private
identities only. Case-insensitive aliases (`pi`/`PI`, `power`/`pow`) removed —
names are strictly case-sensitive. Constants `PI` and `E` are literal
floats in Kex source, and their obsolete private runtime functions have been
deleted from both backends. The `flattenModules` flag is now set during prelude
metadata collection so module function exports appear in the KexI chunk.
Hardcoded namespace dispatch removed from `lower.cxx` for all decoupled
modules (IO, System, Math, Console, Process, Task, Http, File, Directory,
Integer, Float, Number). Namespace calls now route through companion
module exports via `externalModules->exportToBeamFn`. The `nsCall` lambda
is deleted. Codegen tests updated to verify companion module routing
(`Kex.IO:printLine` etc.) instead of direct runtime calls
(`kex_io:print_line`). Only Supervisor retains syntax-specific hardcoded
dispatch.

Native-wins guard in the interpreter: `execModule` skips prelude `let`
definitions only when an explicit public native builtin already exists for
that `Module::function` key. A private intrinsic with the same qualified name
no longer suppresses its public Kex wrapper. This lets source ownership advance
without renaming the private ABI, while preserving intentional variadic or
evaluator-state public compatibility bindings.

Module-function fallback in the interpreter: `callFunction` now searches
`m_moduleRegistry` exports when a bare uppercase name (e.g. `Sequence`)
isn't found in `m_env`/`m_intrinsicEnv`. This lets bare calls like
`Sequence(from: 0) { ... }` resolve to prelude module functions
(`Stream::Sequence`) without requiring separate global aliases.

Namespace modules with prelude implementations:
- **Math**: fully decoupled (see above)
- **IO**: `let` implementations calling `Kex.Intrinsic.IO.*`;
  `kex_intrinsic_io.erl` wraps `kex_io`. Walker namespace functions remain
  public-native compatibility bindings: print families are variadic, and all
  IO calls may occur in concurrently scheduled processes before evaluator
  environments are process-local. Each compatibility function and alias now
  declares its public and intrinsic identities together; there is no trailing
  public-environment copy pass.
- **System**: source-owned `exit(code)` calls the private
  `Kex.Intrinsic.System.exit` primitive on both backends
- **Console**: all color constants and functions as `let` definitions
  calling `Kex.Intrinsic.Console.*`; walker calls execute those source wrappers
- **Process**: `module Process` added with `self`, `exit`, `register`,
  `whereis` calling `Kex.Intrinsic.Process.*`; `Process::self` native wins
  on interpreter. Walker-native Process/Task scheduler fallbacks now declare
  their public and intrinsic identities explicitly instead of copying them
  from the public environment after registration
- **Kex**: `backend` and nested `Feature.has?`/`Feature.list` execute their
  Kex wrappers on the walker over private backend/feature intrinsics

- **Http**: all 7 HTTP methods (get/post/put/patch/delete/head/options)
  with 1- and 2-arity overloads calling `Kex.Intrinsic.Http.*`;
  `kex_intrinsic_http.erl` wraps `kex_http`; interpreter request operations
  are private-only, so public walker calls execute the Kex wrappers

- **Task**: `start` and `awaitAll` call `Kex.Intrinsic.Task.*`; both receiver
  `await` overloads are source-owned, including named `timeout:` dispatch;
  `kex_intrinsic_task.erl` wraps `kex_task`; interpreter natives win

- **File/Directory**: top-level `foul module File` and `foul module Directory`
  live alongside the `module FS` declaration block; `kex_intrinsic_file.erl`
  and `kex_intrinsic_directory.erl` wrap `kex_file`. Walker filesystem
  operations are private-only, so public calls execute the Kex wrappers.
- **Integer/Float/Number**: namespace blocks in `number.kex` own parse and the
  Integer/Float parsePrefix forms through `Kex.Intrinsic.*`; all walker parsing
  primitives are private-only. `kex_intrinsic_float.erl` supplies the Float
  BEAM boundary, while Number uses the shared numeric intrinsic module.

Mock decoupling status:
- **Mock.IO**: decoupled at the runtime boundary — prelude foul functions in http.kex's
  consolidated `module Mock` block dispatch through `Kex.Intrinsic.IO.*`;
  companion `Kex.Mock.IO` module generated; hardcoded lowerer dispatch
  removed. Walker `start`, `stop`, `output`, and `clear` execute the source
  wrappers. Source overloads cover one through four input arguments on BEAM,
  including scalar and list forms. The walker's unbounded variadic `input(...)`
  remains the sole public-native compatibility binding until Kex declarations
  can represent variadic parameters.
- **Mock.Http**: fully decoupled — companion `Kex.Mock.Http` generated
  from prelude foul functions; walker public calls also execute those source
  wrappers, with only mock-state controls retained as private intrinsics
- **Mock.FS**: fully decoupled — source `File`, `Directory`, and `clear`
  functions dispatch through `Kex.Intrinsic.FS.*`; the interpreter registers
  private FS primitives and BEAM uses `kex_intrinsic_fs`, so the dedicated
  lowerer dispatch block has been removed

The direct `spec/fs.spec.kex` contract now passes all 36 cases on both the
walker and BEAM. FileHandle's documented `readLine`, `writeLine`, `read`,
`write`, `eof?`, and `close` methods are source-owned and backed by a qualified
private intrinsic module. BEAM mock copy/rename, mocked-directory deletion,
and mocked `File.open` now share the walker's in-memory behavior.

Stream decoupling status:
- **Interpreter**: fully decoupled — prelude `let Sequence(from, step)` and
  `let Iterate(seed, step)` in stream.kex dispatch through
  `Kex.Intrinsic.Stream.generate()`; native `streamMake` uses type-sniffing
  for argument order flexibility. The runtime registers `generate`, `take`,
  and `drop` directly under private `Stream::*` identities, while the public
  `Stream` namespace is created exclusively by the source module
- **BEAM**: fully decoupled — KexI v3 carries param names in
  `ExternalModules.exportParamNames`; the lowerer resolves named args for
  external module functions (method-call, bare-call, and bare-name-fallback
  paths); hardcoded Stream dispatch removed

ENV decoupling:
- **BEAM**: fully decoupled — prelude `module ENV` dispatches through
  `Kex.Intrinsic.Env.*`; `kex_intrinsic_env.erl` wraps `kex_io:env_map()`
  with map operations; hardcoded ENV dispatch removed from lowerer
- **Interpreter**: ENV remains a plain MapValue (env.cxx); UFCS handles
  `.get`/`.keys`/`.has?`/etc. via Map builtins; `execModule` guard
  preserves the MapValue when the prelude's `module ENV` is loaded
- `main(args, env)` parameter binding to `kex_io:env_map()` retained
  (language-level, not namespace dispatch)

Namespace modules that remain hardcoded (cannot be simple intrinsic calls):
- **Supervisor**: `start` needs named-arg destructuring and map
  construction at the IR level

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
- The interpreter contract suite rejects ordinary namespace calls to private
  ABI-only names across FS, List, Map, Char, Stream, and IO, while allowing
  source-owned receiver methods to retain their normal static-call form.
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
