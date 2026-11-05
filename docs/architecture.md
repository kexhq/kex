# Compiler architecture

How a Kex program moves from source to a running program, pass by pass. The
restructure that produced this layout, and what is still unfinished, is
recorded in `docs/pass-restructure-plan.md`.

## The rule

Semantic analysis decides, and every later pass follows. Lowering and the
tree-walking interpreter both read one checked program (`hir::Program`), so
they do not each re-derive a decision the type checker already made. Two
ratcheted `make dup-audit` metrics watch for regressions:

- `lowering-rederives`: places lowering reads the raw type map to decide
  something itself. It is 0.
- `walker-rederives`: places the walker reads a static type itself. It is 1,
  where the walker builds a `Showable` dictionary from an argument's static
  type. That is type-directed by definition.

## Driver (`src/main.cxx`, `src/driver/pipeline.hxx`)

A build is a list of named passes over one `CompileUnit`
(`kex::driver::Pipeline`). `kex --print-passes` prints the list, and
`KEX_TIMINGS=1` times each pass.

```
prelude-interfaces → run-manifest → pre-check → output-dir → spec-base →
dependencies → runtime-beams → run-cache → expand → check → lower →
write-core → erlc → store-caches → attach-kexi → finish
```

A pass carries on (`kContinue`) or stops the build with an exit status.
`--emit-core` stops after `write-core`, and an error stops at the pass that
found it.

## Front end

- **Lexer and parser** (`src/lexer/`, `src/parser/`): source to `ast::Program`.
  The parser also rejects shapes that are wrong at any stage, such as `only:`
  beside `except:`.
- **Dependencies:** every module the program reaches through `using` or a
  qualified name is parsed and merged into the one program, as is a spec's
  base file.
- **Compile-time expansion** (`src/compiled/`): `compiled do ... end` blocks
  become ordinary AST (see `docs/compiled-status.md`).

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
  - IR passes run in `ir::PassManager` (`upgrade-points`, `verify`).
  - `emit_core` writes Core Erlang, and `erlc` compiles it.
  - The stdlib prelude is lowered with checker answers, as user code is.
- **Walker** (`src/interpreter/`): evaluates the same AST with the same
  `hir::Program` (`Evaluator::setProgram`). That covers operators and own
  methods of distinct types, slot casts, and `Type.of` answers. Under
  `--no-check` the analysis still runs, silently, on both backends.
- **Not yet on the HIR:** the walker REPL, the wasm REPL and compile-time
  evaluation. They keep the walker's run-time method lookup as their
  fallback.

## Checking it

```
make test-all     # unit tests, spec/, examples/, stdlib specs
make spec-beam    # every spec through the BEAM
make fuzz         # 400 generated programs + targeted ones, walker vs BEAM
make test-devirt  # devirtualized names exist in the emitted modules
make dup-audit    # duplication and re-derivation counts may only fall
make bench-slick  # performance on ../slick, a real application
```
