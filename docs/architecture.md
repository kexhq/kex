# Compiler architecture

How a Kex program moves from source to a running program, pass by pass.

## The rule

Semantic analysis decides, and every later pass follows. Lowering and the
tree-walking interpreter both read one checked program (`hir::Program`), so
they do not each re-derive a decision the type checker already made. The
checker answers in Kex terms only: which declaration a name, call or
annotation means, named by its identity (module path plus declared name,
`Net.HTTP.Client`), and what form a written type says a value has
(`semantic/type_test.hxx`). Each backend turns those into its own
representation (BEAM: tagged tuples, guards) and its own symbols (mangled
function names); nothing about a backend's representation moves into the
checker. Lowering reads the checker only through `hir/hir.hxx`; it resolves
no name and reads no annotation itself.

## Passes (`src/pass/`), after LLVM's new pass manager

The compiler is a pipeline of typed passes over three IR levels:

| Level | Unit | LLVM |
|---|---|---|
| `program` | `build::Program`: the entry and every file it loads | `Module` |
| `unit` | `build::Unit`: one strongly connected group of files, built apart in build order | CGSCC |
| `module` | `ir::Module`: one emitted BEAM module | `Function` |

- A **pass** is a class (`PassInfoMixin`) with
  `run(Unit&, AnalysisManager<Unit>&) -> PreservedAnalyses`. A
  `PassManager<Unit>` runs a level's passes in order, and is a pass itself.
- An **analysis** (`AnalysisInfoMixin`) has a `Result` type and is asked for by
  type: `analyses.result<HirAnalysis>(unit)`. The `AnalysisManager` computes it
  once and drops it when a pass did not preserve it, or anything it lists in
  `Dependencies`.
- **Adaptors** run an inner level's pipeline over each inner unit
  (`unit(...)`, `module(...)`). Proxies give inner passes the outer level's
  results.
- **Instrumentation** wraps every pass: `--time-passes` (or `KEX_TIMINGS`),
  `--print-after=PASS`, `--print-after-all`. A unit a cache answered runs only
  its required passes.
- A **registry** per level names every pass, so a pipeline is text:
  `kex --print-passes` lists them, `--print-pipeline-passes` prints the one a
  build runs, and `--passes=...` runs another.

The build (`src/driver/build_ir.hxx`, `build_passes.cxx`):

```
spec-base,load-dependencies,run-cache,
unit(unit-cache,assemble,check,lower,load-order,companion-forwards,
     receiver-forwards,module(specialize-blocks,fuse-lists,guards,upgrade-points,verify),emit-core,
     describe,store-unit,return-items),
merge-dependencies
```

Unit analyses: `reach` (the files a unit uses), `below` (the units below it, as
a KexI registry), `available-interfaces`, `link` (what lowering knows of them),
`unit-cache-key`, and `hir`: the checker, whose answers lowering and
`describe` read.

## Driver (`src/main.cxx`, `src/driver/compile.cxx`)

Around the pipeline, and not passes: option parsing, the output directory,
copying the runtime, the run manifest and run cache (which answer through the
`run-cache` pass), writing `.core`, erlc, attaching KexI, and running the
program. Nothing expands the merged program: a `compiled do` block runs once,
in its own file's expansion.

## Front end

- **Lexer and parser** (`src/lexer/`, `src/parser/`): source to `ast::Program`.
  The parser also rejects shapes that are wrong at any stage, such as `only:`
  beside `except:`.
- **Dependencies:** every module the program reaches through `using` or a
  qualified name is parsed and merged into the one program, as is a spec's
  base file.
- **Compile-time expansion** (`src/compiled/`): `compiled do ... end` blocks
  become ordinary AST (see `docs/compiled-eval.md`).

## Semantic analysis (`src/semantic/`)

`runSemanticCheck` runs one sema over the merged program:

1. **Analyzer** (`Analyzer::analyze`):
   - The module rules (`module_rules.hxx`: what a `using` or `export` may
     take from another module).
   - The type checker (`typechecker*.cxx`), which is also the one place a
     name is resolved: an undefined name, constructor, tag or named-argument
     label is its report (`TypeChecker::reportUndefined`), and so is a bare
     name two `using`s bring in from two modules
     (`reportAmbiguousImports`). Its scopes record whether a name is a `var`,
     so it also reports assignment to a `let` and a name bound twice in one
     scope (`declareVar`).
   - Receiver conflicts, from the receivers the checker resolved.
   - Purity, over the function the checker says each call runs
     (`Analyzer::callsFoul`), then transitive effects. Its diagnostics are
     listed ahead of the checker's.
2. **Validation:** tagged literals.

The type checker is where calls are decided. Every call it understands gets a
decision: an imported target (`ResolvedCallTarget`), a selected signature, or
a `DecidedCall` of kind `Constructor`, `FieldRead`, `ModuleReference`,
`TraitMethod`, `ValueCall`, `Dispatch`, `Conversion`, `TypeReference`,
`Builtin`, `Operator`, `OwnMethod` or `ModuleFunction`.

`TypeChecker::ownerOfCall` and `ownTypeAnswers` are the one rule for who owns
a method call:
- the receiver's own type, through its `make` block, an inherited trait
  default, or its home module's function over it;
- the run-time value, when the receiver is erased and providers compete;
- or the overload winner.

Tooling reads names from the checker too. When asked
(`Analyzer::setIndexesNames`; a build does not ask), the checker records
each expression that names something no scope binds and the names in scope
at each expression, and `name_index.cxx` turns that into the symbols'
references and the completion scopes. `symbol_queries.hxx` answers
definitions, references and completions over such an index. The language
server's documents (`src/lsp/workspace`) hold parsed files and their
symbols and analyze nothing themselves.

## The checked program (`src/hir/`)

`hir::Program` is built once from the AST and the analysis:
- `targetOf(call)` gives a `Target` with a kind, module, function, arity,
  owner, and `servingCast`.
- `typeOf(expr)` gives an expression's type.
- `receiverTypeOf(call)` gives the nominal type a method call was decided on.
- `callSites()` lists every call site.

`--dump-hir` prints every call site's target, and `--verify-hir` lists the
undecided ones. The HIR indexes the AST; it does not yet have node types of
its own.

## Backends

- **BEAM** (`src/ir/`):
  - Lowering translates the AST into the IR by reading the HIR's decisions.
  - The module passes (`specialize-blocks`, `fuse-lists`, `guards`,
    `upgrade-points`, `verify`) rewrite each lowered module
    (`src/ir/module_passes.hxx`).
  - `emit_core` writes Core Erlang, and `erlc` compiles it.
  - The stdlib prelude is lowered with checker answers, as user code is.
- **Walker** (`src/interpreter/`): evaluates the same AST with the same
  `hir::Program` (`Evaluator::setProgram`). That covers operators and own
  methods of distinct types, slot casts, and `Type.of` answers. Under
  `--no-check` the analysis still runs, silently, on both backends.
- Every walker entry point runs on a checked program: a file run, each REPL
  input (`keepChecked`, the wasm REPL too) and compile-time evaluation
  (`compiled do`). Only `--no-check` runs without one.

## Iteration on the BEAM

- A call that passes a block to a function that only applies it, recurses
  with it or hands it on is sent to a copy of that function with the block
  in place (`specialize-blocks`): a local loop with no closure call per
  element. The callee may be another module's when the standard library
  recorded it: `--build-prelude` writes the functions a copy may be made of
  into a `KxIR` chunk of `kex_prelude.beam` (`ir/serialize`), and
  `kex_prelude.beam` is part of the toolchain fingerprint. `List.map`,
  `filter`, `reduce`, `each`, `Integer.times` and `Range.each` recurse in Kex
  when compiled for this; the walker keeps its native loops, choosing by
  `Kex.Intrinsic.Kex.backend()`, which lowering makes a constant.
- A block that reassigns variables of the code around it takes their values
  after its own arguments and answers `{Value, State}`; its statement runs
  through `kex_intrinsic_fun:threadState`, which keeps the state for the
  call, and the variables are rebound to what it answers. Where the callee
  can be copied, the copy carries the state instead. No function is
  special: `each` and `times` are threaded as any callee is.
- A chain (`xs.map { … }.filter { … }.reduce(0) { … }`) walks its list once
  where every block in it is pure (`fuse-lists`). A block with an effect
  keeps the chain's order: every block of a stage before any of the next.
  Purity is the analysis's (`Analyzer::recordPureBlocks`); a pass recognises
  a copy by its shape, never by its name.

## Decisions

- The HIR is the AST plus the checker's answers: no separate node types.
  Lowering and the walker walk the AST and read every decision through
  `hir::Program`; no consumer re-derives one or reads a checker table
  directly.
- Every program the walker runs is checked first: the entry, each module
  loaded at run time (against its dependencies' interfaces, as the BEAM
  build checks it) and compile-time code.
- Separate compilation is the build: each unit is checked and lowered
  against the interfaces of the units below it, unchanged units come from
  the unit cache.
- Receiver devirtualization follows what the emitter recorded: a type's own
  implementation of a receiver function and the dispatcher's routes
  (`KexiMethod::implementation`), never a re-derivation from names.
- Visibility is decided at the declaration (`KexiMethod::foreignTarget`) and
  read by every consumer.
- Pseudo-functions are one intrinsic registry plus `Type<T>` signatures, not
  a `name == "..."` branch per subsystem. `Type<T>` is the reflection record:
  a type name used as a value is that record on both backends.
- One decision has one implementation: when a replacement serves the
  corpus, the old path is deleted in the same change. The size of `src/` is
  measured, not a gate.
- Breaking changes are fine before stability.

## Checking it

Every change: `cmake --build build` with no new warnings, `make test-all`
(not bare `ctest`, which misses the spec suites), and `make spec-tey` when
Tey or the standard library changes. A new spec at the top of `spec/` needs
its `.expected` golden (`make spec-orphans`); a `describe`/`it` suite goes
under `spec/stdlib/`. CI also runs `make test-wasm`, for the driver and the
stdlib loader.

```
make test-all     # unit tests, spec/, examples/, stdlib specs
make spec-beam    # every spec through the BEAM
make fuzz         # 400 generated programs + targeted ones, walker vs BEAM
make test-metamorphic   # program changes that must keep behaviour, or be refused
make metamorphic-search # those, plus spec programs broken the way a type error
                        # would: refused, or run alike without crashing (fresh seed)
make lsp-sweep     # the language server asked about every name of examples/;
                  # each answer held to the checker's (hover type, completion,
                  # diagnostics, definition)
make test-devirt  # devirtualized names exist in the emitted modules
```
