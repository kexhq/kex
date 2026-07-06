# Semantic Architecture Plan

Goal: restructure the semantic layer so it supports LSP, CLI linting, REPL completions, and the compiler — all from the same queryable index, with incremental re-analysis on edits.

## Problems with the current design

1. **No incremental analysis.** `Analyzer` and `TypeChecker` take a whole `Program` and walk it once. After the walk, scope information is discarded. Re-checking one function means re-analyzing the entire file.

2. **No persistent symbol index.** `SymbolTable` uses a push/pop scope stack during the walk. Once analysis finishes, the scopes are gone. There's no way to ask "what's defined at line 7?" or "where is `compute` referenced?"

3. **Fail-fast parser.** The parser throws `runtime_error` on the first syntax error. Tooling needs partial ASTs with error placeholders so completions and diagnostics work in broken files.

4. **Undefined names are silently ignored.** `checkCall` returns `Type::unknown()` for any unrecognized function to avoid false positives on forward/recursive references. This means typos like `fact(n-1)` instead of `compute(n-1)` go unreported.

5. **Type checker doesn't run in `--run` mode.** The interpreter executes programs without any semantic pre-flight, so errors only surface with explicit `--check`.

## Architecture overview

```
┌─────────────┐   ┌─────────────┐   ┌──────────────┐
│  LSP Server │   │  CLI Linter │   │  Compiler    │
└──────┬──────┘   └──────┬──────┘   └──────┬───────┘
       │                  │                  │
       └──────────┬───────┴──────────────────┘
                  │
         ┌────────▼────────┐
         │   Query Engine  │  symbolAt(), completionsAt(),
         │                 │  diagnosticsFor(), typeOf()
         └────────┬────────┘
                  │
         ┌────────▼────────┐
         │   Semantic DB   │  persistent index, survives analysis
         │                 │  file → symbols, refs, diagnostics
         └────────┬────────┘
                  │
    ┌─────────────┼─────────────┐
    │             │             │
┌───▼───┐  ┌─────▼─────┐  ┌───▼────┐
│Collect │  │  Resolve  │  │TypeChk │   (ordered passes, each reads/writes DB)
└───┬───┘  └─────┬─────┘  └───┬────┘
    │             │             │
    └─────────────┼─────────────┘
                  │
         ┌────────▼────────┐
         │  Parser         │  error-recovering, produces partial ASTs
         │  (per-file)     │  with ErrorNode placeholders
         └─────────────────┘
```

## Component details

### 1. Error-recovering parser

The parser currently throws on the first error. The new behavior:

- On unexpected token, record a `Diagnostic`, skip tokens until a sync point, insert an `ErrorNode` in the AST, and continue parsing.
- Sync points: `end`, newline at indent 0, `let`/`foul`/`module`/`type`/`main`/`record`/`make` (any definition-start keyword), EOF.
- The AST is always produced. A file with N syntax errors produces an AST with N `ErrorNode` placeholders and the rest of the tree intact.
- `ErrorNode` carries the source range and the diagnostic message.

Every AST expression/pattern/top-level variant gains `ErrorNode` as a possible alternative:

```cpp
struct ErrorNode {
    SourceRange range;
    std::string message;
};
```

### 2. SemanticDB — the persistent index

```cpp
struct SymbolInfo {
    std::string name;
    SymbolKind kind;           // Variable, Function, Type, Record, Module, TypeParam
    TypePtr type;              // resolved type (may be Unknown before pass 3)
    SourceLocation definition;
    std::vector<SourceLocation> references;
    std::string module;        // owning module, "" for top-level
    bool isFoul = false;
    bool isExported = true;
    // For functions: parameter names and types for signature help
    std::vector<std::pair<std::string, TypePtr>> params;
};

struct FileState {
    std::string path;
    uint64_t version;          // increments on every edit
    ast::Program ast;          // retained (not discarded after analysis)
    std::vector<Diagnostic> diagnostics;
    std::vector<SymbolInfo> symbols;
    // Sorted by start position for binary-search lookup
    std::vector<std::pair<SourceRange, size_t>> locationIndex; // index into symbols
};

class SemanticDB {
public:
    // Ingest
    auto updateFile(const std::string& path, std::string source) -> void;
    auto removeFile(const std::string& path) -> void;

    // Queries (const, callable from any thread with read lock)
    auto symbolAt(const std::string& file, Position pos) const -> const SymbolInfo*;
    auto completionsAt(const std::string& file, Position pos) const -> std::vector<CompletionItem>;
    auto diagnosticsFor(const std::string& file) const -> const std::vector<Diagnostic>&;
    auto referencesTo(const SymbolInfo& sym) const -> std::vector<SourceLocation>;
    auto typeOf(const std::string& file, Position pos) const -> TypePtr;
    auto signatureHelp(const std::string& file, Position pos) const -> std::optional<SignatureInfo>;
    auto allExportsFor(const std::string& moduleName) const -> std::vector<const SymbolInfo*>;

private:
    std::unordered_map<std::string, FileState> m_files;
    // Cross-file index: module name → exported symbols
    std::unordered_map<std::string, std::vector<SymbolInfo*>> m_moduleExports;
};
```

### 3. Analysis passes

Split the single monolithic walk into ordered passes. Each pass reads the DB state left by prior passes and writes its own results back.

| # | Pass | Purpose | Granularity | Depends on |
|---|------|---------|-------------|------------|
| 1 | **Collect** | Register all top-level names, module exports, record shapes, type definitions, function signatures (from annotations) | Per-file, bodies not entered | Nothing (file-local) |
| 2 | **Resolve** | Resolve every name reference to a SymbolInfo. Flag undefined symbols. Build the references list. | Per-file | Pass 1 of ALL files (needs cross-file exports) |
| 3 | **TypeCheck** | Infer types for expressions, check call arg/return types, match exhaustiveness | Per-function | Pass 2 (names resolved) |
| 4 | **Lint** | Purity violations, unused variables/imports, shadowing warnings, style | Per-function | Pass 3 (types known) |

#### Incremental invalidation

On edit to file F:
1. Re-parse F (always — parser is fast).
2. Re-run pass 1 for F. Compare old vs new exports.
3. If exports unchanged: re-run passes 2–4 for F only.
4. If exports changed: re-run pass 2 for all files that import from F's module(s), then pass 3–4 for those files.

This is file-level granularity. Function-level is possible later (track which functions reference which symbols, only re-check functions whose dependencies changed) but file-level is sufficient for responsive LSP on typical Kex project sizes.

#### Pass 1: Collect (replaces the forward-reference problem)

This is the key insight that fixes the "undefined function" gap. Today, `checkCall` can't distinguish "forward reference" from "typo" because signatures are registered as functions are visited top-to-bottom. With a dedicated collect pass:

- ALL top-level and module-level function names (+ their annotated signatures if present) are registered BEFORE any body is entered.
- Pass 2 can then confidently report "undefined" for any name not in the global table.
- Recursive calls work because the function's own name is already collected.
- Mutually recursive functions work for the same reason.

```cpp
class CollectPass {
public:
    auto run(SemanticDB& db, const std::string& file) -> void;
private:
    auto collectTopLevel(const ast::TopLevelItem& item) -> void;
    auto collectModule(const ast::ModuleDef& mod) -> void;
    auto collectFunction(const ast::FunctionDef& def, const std::string& module) -> void;
    auto collectType(const ast::TypeDef& def) -> void;
    auto collectRecord(const ast::RecordDef& def) -> void;
};
```

#### Pass 2: Resolve

```cpp
class ResolvePass {
public:
    auto run(SemanticDB& db, const std::string& file) -> void;
private:
    auto resolveExpr(const ast::Expr& expr) -> void;
    auto resolveName(const std::string& name, SourceLocation loc) -> const SymbolInfo*;
    auto resolveQualified(const std::string& module, const std::string& name, SourceLocation loc) -> const SymbolInfo*;
};
```

When `resolveName("fact", loc)` finds nothing in the DB, it emits:
```
error: undefined function `fact`
  --> fact.kex:4:27
  |
4 |   let compute(n) = n * fact(n - 1)
  |                        ^^^^ not found
  |
  help: did you mean `compute`?
```

The "did you mean" suggestion uses edit-distance against all names in scope.

#### Pass 3: TypeCheck (mostly existing logic, reorganized)

The existing `TypeChecker` logic is largely correct — it just needs to:
- Read resolved symbols from the DB instead of maintaining its own scope stack
- Write inferred types back to `SymbolInfo.type`
- Operate per-function (called once per function body, not once per program)

#### Pass 4: Lint

Purity checking (currently in `Analyzer`) moves here, plus new checks:
- Unused variables (defined but never referenced — easy now that references are tracked)
- Unused imports/modules
- Shadowed variables (warning)
- Missing type annotations on exported functions (optional, configurable)

### 4. Query Engine

Thin facade over SemanticDB for consumer code:

```cpp
class QueryEngine {
public:
    explicit QueryEngine(SemanticDB& db);

    // LSP protocol operations
    auto hover(const std::string& file, Position pos) -> HoverResult;
    auto gotoDefinition(const std::string& file, Position pos) -> std::optional<Location>;
    auto findReferences(const std::string& file, Position pos) -> std::vector<Location>;
    auto completion(const std::string& file, Position pos) -> std::vector<CompletionItem>;
    auto signatureHelp(const std::string& file, Position pos) -> std::optional<SignatureInfo>;
    auto diagnostics(const std::string& file) -> std::vector<Diagnostic>;
    auto rename(const std::string& file, Position pos, const std::string& newName) -> std::vector<TextEdit>;
    auto documentSymbols(const std::string& file) -> std::vector<DocumentSymbol>;

    // CLI / compiler
    auto checkFile(const std::string& file) -> std::vector<Diagnostic>;
    auto checkAll() -> bool;  // true = no errors
};
```

### 5. LSP Server

The LSP server is a JSON-RPC transport layer. It manages:
- File open/change/close notifications → `SemanticDB::updateFile`
- Request dispatch → `QueryEngine` method → JSON-RPC response
- Diagnostic push (after every updateFile, push diagnosticsFor that file)

The protocol handling is mechanical — the interesting work is all in the DB and passes.

```
src/lsp/
  transport.hxx/cxx    — stdin/stdout JSON-RPC read/write loop
  protocol.hxx         — LSP message types (generated or hand-written)
  server.hxx/cxx       — lifecycle, dispatches requests to QueryEngine
```

### 6. Completion logic

Completions at a given position use context from the DB:

| Context | Completions offered |
|---------|-------------------|
| After `.` on a value | methods from the value's type (via UFCS lookup + make-block methods) |
| After `Module.` | exported symbols from that module |
| Bare identifier position | all in-scope variables + functions + modules |
| Inside a type annotation | type names + type params in scope |
| Pattern position | constructors of the expected type |
| Inside lambda `{ \|params\| ... }` | variables from enclosing scope |

### 7. Wiring into `--run` mode (immediate win)

Before full restructuring, a minimal change to `main.cxx`:

```cpp
// In mode == "run", after parsing, before evaluating:
kex::semantic::Analyzer analyzer;
bool ok = analyzer.analyze(program);
if (!ok) {
    for (const auto& diag : analyzer.diagnostics()) {
        // print errors to stderr
    }
    return 1;
}
// only then: evaluator.execute(program);
```

This gives users immediate feedback with the checks that already exist (purity, arity, type mismatches on known functions) while the larger restructuring proceeds.

## File layout

```
src/
  index/
    semantic_db.hxx/cxx
    query_engine.hxx/cxx
    completion.hxx/cxx
  semantic/
    passes/
      collect.hxx/cxx
      resolve.hxx/cxx
      typecheck.hxx/cxx
      lint.hxx/cxx
    types.hxx/cxx              (existing, unchanged)
    traits.hxx/cxx             (existing, unchanged)
    stdlib_signatures.hxx/cxx  (existing, unchanged)
  lsp/
    transport.hxx/cxx
    protocol.hxx
    server.hxx/cxx
  parser/
    parser.hxx/cxx             (add error recovery)
    error_node.hxx             (new)
  lexer/                       (unchanged)
  ast/                         (add ErrorNode variant)
  interpreter/                 (unchanged)
```

## Autoformatter

A canonical formatter for `.kex` source, available as `kex --fmt <file>`, `kex --fmt --check` (exit 1 if not formatted), and via LSP `textDocument/formatting`.

### Design

The formatter operates on the **CST (concrete syntax tree)**, not the AST — it must preserve comments, blank lines between logical groups, and trailing commas. The CST is a lossless representation of the source text (every token and trivia node accounted for).

However, the formatter input should be **pluggable** — it doesn't hard-code "parse from text." This matters because:
- Today we format `.kex` source files (text → CST → formatted text).
- Later, the IR (`.kexo` binary format) will carry enough structure to regenerate formatted source. The formatter should accept IR as an alternative input without duplicating layout logic.

### Pluggable input via `FormattableTree`

```cpp
// Abstract interface — anything that can be formatted
class FormattableTree {
public:
    virtual ~FormattableTree() = default;
    virtual auto root() const -> const FormatNode& = 0;
};

// Concrete: parsed from .kex source text (lossless CST)
class SourceTree : public FormattableTree { ... };

// Future: reconstructed from .kexo IR
class IrTree : public FormattableTree { ... };
```

The formatter itself (`Formatter::format(const FormattableTree&) -> std::string`) only sees the abstract tree. Input providers handle parsing/decoding.

### Formatting rules

| Construct | Rule |
|-----------|------|
| Indentation | 2 spaces per nesting level |
| `do`/`end` blocks | `do` on same line as header, `end` on own line at parent indent |
| Function clauses | No blank line between clauses of the same function |
| Method chains | One UFCS call per line when chain exceeds line limit |
| Binary operators | Spaces around all binary ops |
| Trailing commas | Allowed in multi-line lists/tuples/maps; formatter adds them |
| Line length | Soft limit 100 chars. If exceeded, break at natural points (after `,`, after `->`) |
| Match arms | Aligned on `->`, one arm per line |
| Module body | One blank line between definitions |
| Comments | Preserved in place; `#` comments get one space after `#` |
| String interpolation | No formatting inside `${}` beyond what the expression rules dictate |

### File layout

```
src/
  fmt/
    format_tree.hxx          — FormattableTree interface + FormatNode types
    source_tree.hxx/cxx      — CST from .kex source (lossless parse)
    formatter.hxx/cxx        — layout engine (operates on FormattableTree)
    rules.hxx                — configurable style options (if we ever allow overrides)
```

### CLI integration

```
kex --fmt file.kex           # format in place
kex --fmt --check file.kex   # exit 1 if file would change (CI use)
kex --fmt --stdin            # read stdin, write formatted to stdout
kex --fmt .                  # format all .kex files recursively
```

### LSP integration

The LSP handler for `textDocument/formatting` calls `Formatter::format(SourceTree(fileContents))` and returns the text edits. Same code path as CLI, no duplication.

## Implementation order

1. **Error-recovery parser** — everything downstream benefits from always having an AST. Without this, a single typo means no completions anywhere in the file.

2. **Pass 1: Collect** — register all names before checking bodies. This immediately fixes the "undefined function not reported" bug. Can be done as a pre-pass in the existing TypeChecker without the full DB yet.

3. **Wire checker into run mode** — minimal change, immediate UX improvement. Users see type errors without remembering to pass `--check`.

4. **SemanticDB + location index** — the persistent store. Migrate existing Analyzer/TypeChecker to write into it instead of their own transient state.

5. **Pass 2: Resolve** — name resolution as a separate pass, populating references. Enables go-to-definition and find-references.

6. **Query Engine** — the facade. At this point, CLI `--check` and a hypothetical LSP both call the same code.

7. **LSP transport** — JSON-RPC over stdin/stdout. Mostly boilerplate. Use the `lsp-framework` pattern (dispatch table mapping method names to handler functions).

8. **Completions** — context-aware suggestions using the DB. This is where UFCS and module exports make Kex completions especially useful.

9. **Autoformatter** — CST-based formatter with pluggable input (source text today, IR later).

## LLM integration

The goal: an LLM can write Kex code, validate it, understand what went wrong, and fix it — in a tight loop without human intervention.

### 1. Machine-readable diagnostics

```
kex --check --json file.kex
```

Output: one JSON object per diagnostic with file, line, col, severity, message, and — critically — **a fix hint** when the checker knows the answer:

```json
{
  "file": "fact.kex", "line": 4, "col": 27,
  "severity": "error",
  "message": "undefined function `fact`",
  "hint": "did you mean `compute`?",
  "candidates": ["compute"]
}
```

The hint and candidates fields give the LLM enough to self-correct without re-reading the whole file. The checker should produce actionable messages: not just "type mismatch" but "expected Integer, got String — argument 2 of `compute`."

### 2. Stdin validation (snippet checking)

```
echo 'compute(n - 1)' | kex --check --stdin --context src/
```

Accepts a code fragment on stdin and checks it against the project's definitions (loaded from the manifest). Returns diagnostics as above. This lets an LLM validate a generated snippet before inserting it, without writing to disk.

### 3. Compact context for code generation

When an LLM needs to understand the project to write correct code, it doesn't need full source — it needs signatures and types. The toolchain provides:

```
kex --summary src/          # all modules' public signatures, one line per symbol
kex --summary --module Http  # just one module
```

Output is Kex syntax (not JSON), readable in a prompt:

```
module Factorial do
  compute : Integer -> Integer
end

module Http do
  get : String -> Handler -> Route
  post : String -> Handler -> Route
end
```

This is auto-generated from pass 1 (collect) — no bodies, just the API surface. An LLM reading 50 module summaries gets full project context in minimal tokens.

### 4. Formatter as normalizer

LLMs produce inconsistently formatted code. `kex --fmt` normalizes it:
- The LLM doesn't need style rules in its prompt — write correct code, formatter handles the rest.
- `kex --fmt --check` (exit 1 if not formatted) gives a binary signal for CI or LLM loops.

### 5. Error design philosophy

Every diagnostic should answer three questions for the LLM:
1. **What** went wrong (the error message)
2. **Where** (file:line:col)
3. **How to fix it** (hint, candidates, or suggested replacement)

This means the checker does more work upfront (edit-distance suggestions, type-based candidate filtering) but each error is self-contained — no need to re-analyze context to understand the fix.

## Pluggable input — general principle

The formatter isn't the only component that will eventually consume IR instead of (or in addition to) source text. The same pattern applies to:

- **SemanticDB** — today it calls the parser to get an AST. Later it could ingest pre-analyzed IR (e.g. from a compiled dependency's `.kexo` that carries type info but no source).
- **Formatter** — formats from CST today, from IR-reconstructed tree later.
- **Code generation** — today from AST, later directly from IR.

The abstraction boundary is: each consumer declares what tree shape it needs (AST, CST, or a subset of IR metadata), and an adapter layer converts the available input into that shape. We don't build this adapter infrastructure upfront — just keep the consumers decoupled from "how the tree was obtained" so adding an IR input path later is a new adapter, not a rewrite.

## Build system / manifest

A project manifest (e.g. `kex.toml` or similar) defines what belongs to a project. The toolchain — compiler, LSP, formatter, linter — all read the manifest to discover files, dependencies, and build targets.

### What the manifest provides

- **Source roots** — which directories contain `.kex` source files.
- **Targets** — what gets built (library, BEAM app, WASM module, CLI tool). Each target declares its entry point(s) and backend.
- **Dependencies** — other Kex packages (with version constraints), resolved from a registry or local paths.
- **Module mapping** — which files/directories map to which modules (or inferred by convention: `src/http/router.kex` → `Http.Router`).
- **Build profiles** — debug vs release settings, optimization level, whether to emit IR or go straight to codegen.
- **Tool configuration** — formatter style overrides, lint rule severity, enabled/disabled checks.

### How each component uses it

| Component | What it reads from manifest |
|-----------|---------------------------|
| **SemanticDB** | Source roots → knows which files to index. Dependencies → loads their interface files for cross-package resolution. |
| **LSP** | Source roots → file watcher scope. Module mapping → resolves `using` declarations to files. |
| **Formatter** | Tool config → style overrides (if any). Source roots → `kex --fmt .` knows what to format. |
| **Compiler** | Targets → knows what to build, which backend to invoke. Dependencies → links against compiled packages. |
| **LLM tools** | Source roots + module mapping → generates accurate file summaries. Dependencies → includes dependency signatures in context. |

### Dependency resolution and the SemanticDB

When file A uses module B from a dependency:
1. The manifest tells the build system where package B lives (registry, local path, or git).
2. The build system ensures B's interface file (`.kexi` — generated by pass 1 on B's source) is available.
3. SemanticDB loads B's interface file into its cross-file index, just like a local file's pass-1 output.
4. Pass 2 (resolve) for A can now resolve references to B's exports.

This means dependencies don't need to be re-analyzed from source — their interface files are pre-computed artifacts, fast to load.

### Implications for tooling

- The LSP reads the manifest on startup to determine project scope. No need for the user to configure include paths or workspace folders manually.
- `kex --check` without a file argument checks the entire project (all targets).
- `kex --fmt .` formats all source files in all source roots.
- `kex --summary` uses the manifest to scope its output — includes all source roots and dependency signatures.

## UFCS resolution ordering

UFCS creates a dependency cycle between resolution and type checking: `x.foo(y)` desugars to `foo(x, y)`, but if multiple modules export `foo`, we need the type of `x` to pick the right one — and types aren't known until pass 3 (TypeCheck), which runs after pass 2 (Resolve).

### Solution: two-phase resolution

Pass 2 (Resolve) handles the easy cases and defers ambiguous ones:

1. **Unambiguous** — only one `foo` is in scope (either local, or brought in by a single `using`). Resolve it immediately.
2. **Qualified** — `Module.foo(x, y)` is always unambiguous. Resolve immediately.
3. **Ambiguous UFCS** — multiple candidates for `foo`. Record all candidates as a `PendingResolution` node (not an error yet).

Pass 3 (TypeCheck), when it encounters a `PendingResolution`, uses the inferred type of the receiver to select the correct candidate. If no candidate matches, or multiple still match, emit the error at this point.

```cpp
struct PendingResolution {
    std::string name;
    std::vector<const SymbolInfo*> candidates;
    SourceLocation loc;
};
```

This means TypeCheck both infers types AND finalizes resolution for UFCS calls. The cost is minor — it's the same place where argument type checking happens anyway.

### Why this works

In practice, ambiguous UFCS is rare — most method-style calls are either:
- Qualified (`Module.func(...)`)
- Unique in scope (only one module exports that name)
- On a receiver whose type is already annotated or trivially inferred from a literal/constructor

The deferred resolution handles the remaining edge cases without complicating the common path.

## Multi-clause function unification

Kex functions can have multiple clauses with pattern matching:

```kex
let compute(1) = 1
let compute(n) = n * compute(n - 1)
```

These are separate `FunctionDef` AST nodes that need to be treated as a single logical function for type checking.

### How each pass handles this

**Pass 1 (Collect):**
- Groups consecutive `FunctionDef` nodes with the same name (within the same scope/module) into a single `SymbolInfo` with `clauseCount > 1`.
- The function's signature is the union of all clause signatures: all clauses must agree on arity; the parameter types are the union (widest) of per-clause parameter types; the return type is the union of per-clause return types.
- If clauses disagree on arity, emit an error here.

**Pass 2 (Resolve):**
- No special handling — each clause body is resolved independently, all referencing the same `SymbolInfo`.

**Pass 3 (TypeCheck):**
- Checks each clause body independently, but unifies return types across all clauses.
- Verifies that pattern arguments in each clause are subtypes of the declared parameter type.
- Checks exhaustiveness: do the patterns across all clauses cover the full input space? (Already partially implemented in `checkMatchExhaustiveness`.)

### Edge cases

- **Clauses with type annotations:** If any clause has a type annotation, all clauses must be compatible with it. The annotation wins over inference.
- **Clauses in modules:** All clauses of a function must be in the same module. A clause for `compute` in module A and another in module B are separate functions, not multi-clause.
- **Guard expressions (if we add them):** Each guarded clause is a separate branch within one pattern, not a separate clause for unification purposes.

## Error recovery — Kex-specific strategy

Generic "skip to sync point" isn't enough. Here's how recovery works for Kex's specific grammar constructs:

### Sync points (ordered by priority)

1. **Top-level keywords at indent 0:** `let`, `foul`, `module`, `type`, `record`, `make`, `main`, `using`, `compiled`. If we're lost inside a function body and hit one of these at the start of a line, we've clearly exited the current definition.

2. **`end` keyword:** Closes the nearest open `do` block. The parser tracks a `do`/`end` depth counter. On error, skip forward until `end` balances the current block, then resume after it.

3. **Newline + dedent:** If we're inside an expression and hit a newline at the same or lesser indentation as the enclosing definition, treat it as statement boundary and try to parse the next statement fresh.

### Recovery strategies per production

| Production | On error | Recovery |
|-----------|----------|----------|
| Top-level item | Unexpected token after consuming `let`/`module`/etc. | Skip to next top-level keyword at indent 0. Insert `ErrorNode` for the failed item. |
| Function body (`do...end`) | Bad expression inside body | Skip to next newline at body indent level. Insert `ErrorNode` as one expression in the body. Continue parsing remaining expressions. |
| Match clause | Malformed pattern or missing `->` | Skip to next `\n` + indent matching the match body. Insert `ErrorNode` clause. Try next clause. |
| Parenthesized expression | Missing `)` | If we hit a newline or a token that clearly starts a new statement, insert synthetic `)` and emit "missing `)` " diagnostic. Don't skip. |
| `do`/`end` block | Missing `end` | If we hit a top-level keyword or EOF, insert synthetic `end`, emit diagnostic, and let the outer production resume. |
| List/tuple literal | Missing `]` or `)` or bad element | Skip to closing bracket (tracking nesting). If not found before newline at outer indent, insert synthetic close and emit diagnostic. |

### The `|` ambiguity

`|` is overloaded in Kex (union types, list cons, lambda params, match receive blocks). Error recovery handles this by using context:

- **In a type annotation** (`name :` or `:>` context): `|` is union type. On error, skip to next `,` or `)` or newline.
- **In a list literal** (`[` context): `|` is cons. On error, skip to `]`.
- **After `do` or `{`** with no preceding expression: `|` starts lambda params. On error, skip to matching `|`, then parse body.
- **In a match/receive block:** `|` is never valid here (match clauses use pattern `->` body). If seen, it's a syntax error — likely the user confused match with lambda syntax.

### Cascading error suppression

After emitting an error and inserting an `ErrorNode`, suppress further errors on the same line (or within the same `ErrorNode` span). This prevents a single typo from producing 5 cascading errors that obscure the real problem.

## REPL integration

The REPL currently lives in `main.cxx` with its own parsing logic (clause accumulation, wrapping expressions in `main do...end`). It should use the same infrastructure.

### Architecture

```
┌─────────────────┐
│   REPL Loop     │  readline, clause accumulation, :commands
└────────┬────────┘
         │
┌────────▼────────┐
│  SemanticDB     │  single "virtual file" representing REPL state
│  (repl session) │  accumulates definitions across inputs
└────────┬────────┘
         │
┌────────▼────────┐
│  Evaluator      │  executes, same as today
└─────────────────┘
```

### How it works

- The REPL maintains a **virtual file** in the SemanticDB that grows as the user enters definitions.
- Each input line/block is appended to this virtual file, then passes 1–4 run incrementally on the new content.
- Diagnostics are printed immediately (type errors, undefined names).
- If no errors, the evaluator runs the new expression/definition.
- Completions: when the user hits tab (if readline supports it), call `completionsAt` on the virtual file at the cursor position.

### What changes from today

- Clause accumulation logic stays (it's REPL UX, not semantic infrastructure).
- The expression-wrapping-in-`main do...end` hack goes away — the SemanticDB can handle bare expressions in REPL mode (or the REPL marks them as such).
- `:set types` uses the TypeChecker's inferred type from the DB, not a separate mechanism.
- Definitions entered in the REPL are visible to subsequent inputs via the DB, same as if they were in one file.

## Open questions

- **Manifest format:** TOML is the likely choice (familiar from Cargo, pyproject.toml). Exact schema TBD.
- **Module-to-file mapping:** Convention-based (directory structure = module path) vs. explicit in manifest vs. hybrid. Convention-based is less boilerplate; explicit handles edge cases.
- **Dependency order:** If module A imports module B, pass 2 for A needs pass 1 for B to be done. With the manifest declaring dependencies, a topological sort is straightforward. For local modules within a project, we either sort by import graph or use the lazy "analyze-on-demand" approach (resolving a cross-file name triggers collection of that file).
- **Concurrency model for LSP:** Single-threaded with cooperative scheduling (process one request at a time, re-parse on next idle) vs. reader-writer lock on the DB (multiple queries concurrent, exclusive lock for updates). Single-threaded is simpler and sufficient for single-file edits at human speed.
- **Bignum Integer:** The type system plan specifies arbitrary-precision Integer as the default. The semantic layer assumes this (PrimitiveType::Integer) but the interpreter uses int64_t. This is orthogonal to the architecture work but should be addressed before the type system can make meaningful claims about integer overflow.
