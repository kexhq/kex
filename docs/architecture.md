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
     receiver-forwards,module(guards,upgrade-points,verify),emit-core,
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

1. **Names** (`resolveProgramNames`, `ResolvePass`): finds undefined names,
   constructors, tags and named-argument labels. It covers the program's own
   modules through `collectSymbols` and the prelude's through their imported
   interfaces.
2. **Analyzer** (`Analyzer::analyze`):
   - Phase 0 checks receiver conflicts and module rules (`module_rules.hxx`,
     the `using`/`export` visibility rules).
   - Then it checks scopes and purity, computes transitive effects, and runs
     the type checker (`typechecker*.cxx`).
3. **Validation:** tagged literals.

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

SemanticDB (`db.cxx`, `collect_pass.cxx`, `resolve_pass.cxx`) is the
language server's index. It runs the same collector, resolver and module
rules over the files it indexes. It is no longer part of a build.

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
  - The module passes (`guards`, `upgrade-points`, `verify`) rewrite each
    lowered module (`src/ir/module_passes.hxx`).
  - `emit_core` writes Core Erlang, and `erlc` compiles it.
  - The stdlib prelude is lowered with checker answers, as user code is.
- **Walker** (`src/interpreter/`): evaluates the same AST with the same
  `hir::Program` (`Evaluator::setProgram`). That covers operators and own
  methods of distinct types, slot casts, and `Type.of` answers. Under
  `--no-check` the analysis still runs, silently, on both backends.
- Every walker entry point runs on a checked program: a file run, each REPL
  input (`keepChecked`, the wasm REPL too) and compile-time evaluation
  (`compiled do`). Only `--no-check` runs without one.

## Checking it

```
make test-all     # unit tests, spec/, examples/, stdlib specs
make spec-beam    # every spec through the BEAM
make fuzz         # 400 generated programs + targeted ones, walker vs BEAM
make test-devirt  # devirtualized names exist in the emitted modules
make dup-audit    # duplication and re-derivation counts may only fall
make bench-slick  # performance on ../slick, a real application
```
