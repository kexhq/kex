# BEAM Codegen Plan

Goal: compile Kex source to BEAM bytecode via Core Erlang, producing `.beam` files that run on the Erlang VM with full OTP interop.

## Strategy

Core Erlang is a small (~15 node types), stable intermediate representation that the Erlang compiler accepts as input. Targeting it means we get OTP's optimizer, register allocator, and bytecode emitter for free. The alternative — emitting BEAM bytecode directly — gives more control but requires reimplementing all of that.

Pipeline:

```
.kex source → parse → semantic analysis → desugar → Core Erlang text (.core) → erlc → .beam
```

The **desugar** step is new: a pre-codegen pass that rewrites complex control flow (error propagation, loops, mutable variables) into a simplified AST that maps 1:1 to Core Erlang. This separates "what does this Kex construct mean?" from "how do I emit Core Erlang text?" and keeps the emitter mechanical.

## Prerequisites from Semantic Analysis

The emitter needs these resolved before it can emit correct code:

1. **Fully resolved names** — every Identifier mapped to its definition site (local binding, module function, imported name, make-block method)
2. **UFCS dispatch resolution** — `x.foo(y)` resolved to either a make-block method `Module:foo(x, y)` or a free function call. Requires type information (see below).
3. **Multi-clause function grouping** — `let factorial(0) = 1` and `let factorial(n) = ...` grouped into a single function definition with multiple clauses
4. **Pattern exhaustiveness** — Core Erlang requires exhaustive case expressions; the compiler rejects non-exhaustive matches. Requires type info to know all variants.
5. **Type information for representations** — which types are records (tagged tuples), sum types (tagged variants), optionals, etc.

### Why TypeCheck is NOT optional

The original plan understated the dependency on type checking. Without it:

- **UFCS is ambiguous** — `x.count()` could be a make-block method on x's type OR a free function taking x as first arg. Only the type of `x` resolves this.
- **Overloaded functions** — same name from different make blocks. Need receiver type to pick the right one.
- **Record field access** — `rec.x` requires knowing the record type to determine field position.
- **Sum type exhaustiveness** — need to know all variants of a type to verify pattern completeness.
- **Generic instantiation** — `List<Int>` vs `List<String>` may affect representation choices in the future.

**Minimum viable for codegen: Collect + Resolve + TypeCheck (at least for top-level signatures and UFCS resolution).** The type checker doesn't need to be complete — inference within function bodies can be partial — but it must resolve call targets and record types.

## Module Naming

### `Kex.` prefix (not `Elixir.`)

Kex modules use the prefix `Kex.` — e.g., `module Greeting` becomes BEAM module `'Kex.Greeting'`.

Rationale for NOT using `Elixir.`:
- Elixir modules assume specific conventions (structs have `__struct__` map key, protocols use `Elixir.Protocol.Type` dispatch atoms, GenServer expects specific callback forms). Pretending to be Elixir modules creates silent incompatibilities.
- A distinct prefix makes it obvious when you're crossing the language boundary.
- Interop with Elixir is still straightforward — Elixir can call `:'Kex.Greeting'.hello("world")` and Kex can call Elixir modules via their full atom name.

An explicit interop layer handles the gaps:
- Calling Elixir: `Elixir.SomeModule.function(args)` in Kex emits a direct cross-module call.
- Calling Kex from Elixir: standard BEAM cross-module call, no magic needed.
- OTP behaviours (GenServer, Supervisor): a `kex_genserver` bridge module that translates Kex process conventions to OTP callbacks. This is runtime library work, not compiler work.

### Nested Modules

```kex
module Foo do
  module Bar do
    let baz() = 42
  end
end
```

Becomes `'Kex.Foo.Bar'` — dot-separated, flattened to a single BEAM module. Each nested module is its own `.beam` file. No runtime nesting.

## Core Erlang Mapping

### Construct-by-Construct Mapping

| Kex | Core Erlang | Notes |
|-----|-------------|-------|
| `module Foo` | `module 'Kex.Foo'` | Kex prefix for clarity |
| `let f(x) = body` | `'f'/1 = fun (X) -> Body` | Single-clause function |
| multi-clause `let f(0)=...; let f(n)=...` | Single fun with `case` on args tuple | Pattern matching on parameters |
| `match x do ... end` | `case X of ... end` | Direct mapping |
| `if cond do a else b end` | `case Cond of 'true' -> A; 'false' -> B end` | Booleans are atoms |
| `record Vec2 { x, y }` | Tagged tuple `{'Vec2', X, Y}` | Field order from definition |
| `Vec2 { x: 1, y: 2 }` | `{'Vec2', 1, 2}` | Construction |
| `rec.x` | Pattern match on known record type | See Record Access below |
| `:ok`, `:error` | Atoms `'ok'`, `'error'` | Direct |
| `(a, b, c)` | `{A, B, C}` | Tuples are native |
| `[a, b \| rest]` | `[A, B \| Rest]` | Lists are native |
| `{key: val}` | `~{'key' => Val}~` | BEAM map syntax |
| `spawn do body end` | `call 'erlang':'spawn'(fun () -> Body end)` | Process creation |
| `receive do ... end` | `receive ... after Timeout -> ... end` | Message passing |
| `x.map { \|n\| ... }` | Resolved by UFCS to specific module call | Type-directed |
| pipeline `a.foo().bar()` | Nested calls (resolved targets) | Desugared before codegen |
| `expr?` (error propagation) | Desugared to explicit match (see below) | Pre-codegen rewrite |
| `String` | BEAM binary | UTF-8 encoded |
| `Integer` | BEAM bignum | Arbitrary precision |
| `Float` | BEAM float (64-bit) | |
| lambda `{ \|x\| x + 1 }` | `fun (X) -> X + 1 end` | Direct |
| `&.method` shorthand | `fun (X) -> 'method'(X) end` | Eta-expand |

### Multi-Clause Functions

```kex
let factorial(0) = 1
let factorial(n) = n * factorial(n - 1)
```

Becomes:

```erlang
'factorial'/1 = fun (_arg0) ->
    case _arg0 of
        0 -> 1;
        N -> call 'erlang':'*'(N, apply 'factorial'/1(call 'erlang':'-'(N, 1)))
    end
```

### UFCS / Make Blocks

```kex
record Vec2 do
  x : Float
  y : Float
end

make Vec2 do
  let add(other: Vec2) -> Vec2 do
    Vec2 { x: @x + other.x, y: @y + other.y }
  end
end
```

Emits a module for the make block:

```erlang
module 'Kex.Vec2' ['add'/2, '__record_info__'/1]
  attributes []

'add'/2 = fun (This, Other) ->
    let Tx = call 'erlang':'element'(2, This) in
    let Ox = call 'erlang':'element'(2, Other) in
    let Ty = call 'erlang':'element'(3, This) in
    let Oy = call 'erlang':'element'(3, Other) in
    {'Vec2', call 'erlang':'+'(Tx, Ox), call 'erlang':'+'(Ty, Oy)}

'__record_info__'/1 = fun (_arg0) ->
    case _arg0 of
        'fields' -> ['x', 'y'];
        'size' -> 3
    end
```

Then `a.add(b)` resolves (via semantic analysis + type info) to `call 'Kex.Vec2':'add'(A, B)`.

### Record Field Access

Field access uses compile-time knowledge of the record type to emit `erlang:element/2`:

```kex
let p = Vec2 { x: 1.0, y: 2.0 }
p.x   # field 'x' is at position 2 (position 1 is the tag)
```

```erlang
let P = {'Vec2', 1.0, 2.0} in
  call 'erlang':'element'(2, P)
```

**Stability:** Field positions are determined by the record definition's field order. Reordering fields in a record definition is a breaking change that requires recompilation of all dependents. The compiler tracks this via the module interface (see Separate Compilation below).

### Record Info Function

Every record module exports `__record_info__/1` which returns field names and tuple size. This enables:
- Runtime reflection when needed (debugging, serialization)
- Cross-module validation that a caller's field assumptions match the definition
- Dialyzer-style checking of record shapes

## Desugaring Pass

A dedicated AST-to-AST transform that runs after semantic analysis and before codegen. It rewrites complex constructs into a simplified form that maps directly to Core Erlang.

### Error Propagation (`?`) → Explicit CPS

The `?` operator is NOT handled inline by the emitter. Instead, a desugaring pass rewrites it before codegen.

**The problem:** `?` means "if this is an error, return early from the enclosing function." In an expression-oriented language, "return early" requires restructuring the entire continuation of the function into the success branch of a case.

**The transform:**

Input:
```kex
let doStuff(input: String) -> Result<Int, Error> do
  let parsed = Integer.parse(input)?
  let validated = validate(parsed)?
  parsed + validated
end
```

After desugaring:
```kex
let doStuff(input: String) -> Result<Int, Error> do
  match Integer.parse(input) do
    :ok(parsed) ->
      match validate(parsed) do
        :ok(validated) -> :ok(parsed + validated)
        :error(e) -> :error(e)
      end
    :error(e) -> :error(e)
  end
end
```

This is a mechanical right-fold: for each `?` in sequence, wrap everything after it in the success branch. The emitter then sees only `match` expressions, which it already knows how to emit.

**`?` inside lambdas:** Returns from the lambda, not the enclosing function. The desugaring scope resets at lambda boundaries.

**`?` inside if branches:** Each branch desugars independently. If only one branch has `?`, only that branch gets the case wrapper.

**`?` in complex positions (e.g., function arguments):**
```kex
foo(bar()?, baz()?)
```

Desugars to:
```kex
match bar() do
  :ok(tmp1) ->
    match baz() do
      :ok(tmp2) -> foo(tmp1, tmp2)
      :error(e) -> :error(e)
    end
  :error(e) -> :error(e)
end
```

Evaluation order is left-to-right, each `?` introduces a new nesting level.

### Loops → Tail-Recursive Functions

Loops desugar into local recursive functions before codegen.

**Simple while:**
```kex
var sum = 0
while n > 0 do
  sum = sum + n
  n = n - 1
end
```

Desugars to a local letrec:
```erlang
letrec 'while_0'/2 = fun (Sum, N) ->
    case call 'erlang':'>'(N, 0) of
        'true' ->
            let Sum1 = call 'erlang':'+'(Sum, N) in
            let N1 = call 'erlang':'-'(N, 1) in
            apply 'while_0'/2(Sum1, N1);
        'false' -> {Sum, N}
    end
in apply 'while_0'/2(Sum, N)
```

The loop function takes ALL mutable variables from the enclosing scope that are referenced inside the loop (both read and written). It returns a tuple of their final values. After the loop, the enclosing scope rebinds those variables to the returned values.

**`break` with a value:**
```kex
let result = loop do
  let item = next_item()
  if item.valid? do
    break item
  end
end
```

`break <value>` becomes the return value of the recursive function. Without `break`, the loop returns the final state tuple (for while) or `unit` (for infinite loops that must be broken out of).

```erlang
letrec 'loop_0'/0 = fun () ->
    let Item = apply 'next_item'/0() in
    case call 'Kex.Item':'is_valid'(Item) of
        'true' -> Item;          % break item → return value
        'false' -> apply 'loop_0'/0()
    end
in apply 'loop_0'/0()
```

**`next` (continue):**

`next` becomes a tail-call back to the loop function with current state:

```kex
while condition do
  if skip_this do
    next
  end
  do_work()
end
```

```erlang
% next → apply 'while_0'/N(current_state...)
% (skipping do_work, going straight to next iteration)
```

**Nested loops:**

Each loop gets a unique name (`while_0`, `loop_1`, etc.). `break` and `next` always target the innermost enclosing loop — Kex has no labeled breaks. Breaking from an outer loop requires restructuring into a helper function (this is a language-level constraint, not a codegen limitation).

**Loops inside lambdas:**

The recursive function is defined inside the lambda's body. No special handling — Core Erlang's `letrec` is lexically scoped.

```kex
items.each { |item|
  var count = 0
  while count < item.size do
    process(item, count)
    count = count + 1
  end
}
```

The lambda body contains the letrec. The lambda captures nothing extra from the loop.

### Mutable Variables (SSA Transform)

SSA renaming runs as part of desugaring, not in the emitter. This is because loops need to know which variables to thread as parameters.

Rules:
1. Each `var x = expr` introduces version `x_0`
2. Each `x = expr` increments to `x_N+1`
3. At loop boundaries: current versions of all mutated variables become loop function parameters
4. After a loop: variables get new versions bound from the loop's return tuple
5. At branch merge points (if/else both assign to same var): a phi variable is introduced after the branch

**Branch merge example:**
```kex
var x = 0
if condition do
  x = 1
else
  x = 2
end
IO.printLine(x)
```

```erlang
let X0 = 0 in
  let X1 = case Condition of
      'true' -> 1;
      'false' -> 2
  end in
  call 'Kex.IO':'print_line'(X1)
```

Since if/else is an expression in both Kex and Core Erlang, the branch result IS the new variable value. The desugarer recognizes "both branches assign to the same variable as their last statement" and lifts it to a binding of the if-expression result.

## Separate Compilation

### Compilation Units

Each Kex source file compiles to one or more BEAM modules independently. The unit of compilation is the file, not the program.

### Module Interfaces (.kexi files)

When compiling a file, the compiler emits an **interface file** alongside the `.beam`:

```
src/greeting.kex → ebin/Kex.Greeting.beam + ebin/Kex.Greeting.kexi
```

The `.kexi` file contains:
- Exported function signatures (names, arities, type annotations)
- Record field definitions (names, types, order)
- Sum type variant definitions
- Type aliases
- Re-exported names

This is analogous to Erlang's `.hrl` headers or OCaml's `.cmi` files.

```
# Kex.Greeting interface (machine-readable, not shown to users)
module Kex.Greeting
  export hello/1 : (String) -> String
  export goodbye/1 : (String) -> String
  record Vec2
    fields: [x: Float, y: Float]
    size: 3
```

### Cross-Module References

When module A uses module B:
1. Compiler looks for `B.kexi` in the interface search path
2. If found: uses the interface for type checking and resolution
3. If not found: compiles B first (topological sort of dependencies)

### Circular References

Kex does NOT support circular module dependencies. The compiler detects cycles during dependency resolution and reports an error. Workaround: extract shared types into a third module.

This is a deliberate constraint. Circular dependencies are rare in practice and complicate incremental compilation significantly. If a pattern emerges where they're needed, we can add forward declarations later.

### Recompilation Tracking

The compiler maintains a dependency graph:
- File F depends on modules M1, M2, ... (those it imports)
- When recompiling M1: check if M1's interface changed
  - If unchanged: dependents don't need recompilation
  - If changed: recompile all files depending on M1

"Interface changed" means: any exported signature, record field order, or type definition differs from the previous `.kexi`.

### Record Field Stability

Since records are positional tuples, changing a record's field order or inserting a field changes the tuple layout. The interface file locks this down:
- The `.kexi` stores the field list in order
- When a dependent is compiled, it reads the `.kexi` to get field positions
- If the record definition changes, the `.kexi` changes, triggering recompilation of dependents

This is the same strategy Erlang uses with `-record` definitions in `.hrl` files — change the record, recompile everything that includes the header.

## Tail Call Optimization

### BEAM's TCO Model

BEAM automatically optimizes calls in tail position. Any call that is the last thing a function does before returning is a tail call — no annotation needed, no limit on recursion depth.

### Kex's Approach

Kex does NOT require explicit tail-call annotations. BEAM handles it transparently. However:

**Compiler warning for non-tail recursion in likely-recursive functions:**

If a function calls itself but NOT in tail position, the compiler emits a warning:

```
warning: recursive call to `sum` at line 5 is not in tail position
  hint: this will consume stack proportional to input size
  hint: consider using an accumulator pattern
```

This catches the classic mistake:
```kex
let sum([]) = 0
let sum([x | rest]) = x + sum(rest)   # WARNING: + happens after recursive call
```

vs the correct:
```kex
let sum(list) = sumAcc(list, 0)
let sumAcc([], acc) = acc
let sumAcc([x | rest], acc) = sumAcc(rest, acc + x)   # tail position, no warning
```

The warning is non-fatal. Some non-tail recursion is intentional (tree traversals, etc.).

### Loop Transform Guarantees

All loops desugared to recursive functions are guaranteed tail-recursive by construction — the recursive call is always the last expression in the true-branch. This is a structural property of the transform, not something the programmer needs to think about.

## Hot Code Reloading

### BEAM's Model

BEAM supports hot code upgrade at the module level. The VM can hold two versions of a module simultaneously — processes running in the old version continue until they make an external call, at which point they switch to the new version.

### Kex's Approach

Since each Kex module maps to exactly one BEAM module, hot reloading works at Kex module granularity. No special compiler support is needed — `code:load_file/1` or the standard OTP release handler works.

**Implications for module design:**
- Small, focused modules are preferable for granular hot-reloading
- A single large module means reloading everything when any function changes
- This is a documentation/best-practices concern, not a compiler concern

**State migration:** OTP's `code_change/3` callback pattern applies. Kex processes that hold state should implement a version-aware receive loop. The `kex_genserver` bridge (in the runtime library) can handle this for processes that opt into OTP supervision.

### Dev-mode REPL

The REPL compiles each entered expression/definition as a temporary module, loads it, evaluates, and unloads. This naturally uses BEAM's hot loading. No special support beyond "compile a module and load it."

## Runtime Library Design

### Architecture

```
runtime/
  src/
    kex_runtime.app.src   — OTP app descriptor
    kex_io.erl            — IO.printLine, IO.readLine, IO.print
    kex_string.erl        — String methods (split, trim, contains, etc.)
    kex_list.erl          — List operations beyond stdlib
    kex_map.erl           — Map helpers
    kex_result.erl        — Result protocol: unwrap, map, flatMap
    kex_process.erl       — Process helpers, registry patterns
    kex_enum.erl          — Enumerable protocol: map, filter, reduce, each
    kex_genserver.erl     — Bridge: Kex process conventions ↔ OTP GenServer
```

### How the Compiler Knows the Mappings

The compiler has a built-in **stdlib signature registry** — a table mapping Kex standard library calls to their BEAM targets. This is compiled into the compiler, not loaded at runtime.

```cpp
// In runtime_mappings.hxx
struct StdlibMapping {
    std::string kexModule;      // "IO"
    std::string kexFunction;    // "printLine"
    int arity;                  // 1
    std::string beamModule;     // "kex_io"
    std::string beamFunction;   // "print_line"
};

inline const std::vector<StdlibMapping> STDLIB_MAPPINGS = {
    {"IO", "printLine", 1, "kex_io", "print_line"},
    {"IO", "readLine", 0, "kex_io", "read_line"},
    {"Integer", "parse", 1, "kex_string", "parse_integer"},
    // ...
};
```

**User-defined methods that shadow stdlib names:** Resolved by the type checker. If `x` has type `MyRecord` and `MyRecord` has a make-block method `count()`, then `x.count()` calls the make-block method. The stdlib mapping only applies to calls on stdlib types or unresolved UFCS on stdlib module names (e.g., `IO.printLine`).

### Versioning

The runtime library version is encoded in the compiler:
- Compiler version N expects runtime version >= N
- The runtime exports a `kex_runtime:version/0` function
- At startup, compiled code calls `kex_runtime:check_version(RequiredVersion)` which crashes with a clear error if incompatible
- Backwards-compatible changes (adding new functions) don't bump the required version
- Breaking changes (renamed functions, changed arities) bump the required version

This is the simplest thing that works. A more sophisticated version constraint system (semver ranges) can come later if needed.

## Invoking erlc

### Phase 1: Shell Out (Prototype)

```cpp
auto cmd = fmt::format("erlc +from_core +clint0 -o {} {}", outDir, corePath);
auto result = std::system(cmd.c_str());
if (result != 0) {
    // Parse erlc stderr for error locations (best-effort)
    reportErlcFailure(corePath);
}
```

Limitations: no structured errors, platform-dependent path handling, requires `erlc` on PATH.

### Phase 2: Erlang Port (Production)

A long-running Erlang node that the Kex compiler talks to over a port (stdin/stdout protocol or TCP):

```
kex compiler ←→ [port protocol] ←→ erlang node running compile:forms/2
```

Benefits:
- Structured error output (the Erlang compiler returns error tuples, not text)
- No process spawn overhead per file (the Erlang node stays warm)
- Can map Core Erlang source locations back to Kex source locations via a source map
- Platform-independent

The port protocol is simple: send Core Erlang source + options, receive either `{ok, BeamBinary}` or `{error, [{Line, Module, Desc}]}`.

### Source Location Mapping

The emitter maintains a source map: Core Erlang line → Kex source location. When `erlc` reports an error at a Core Erlang line, the compiler looks up the corresponding Kex location and reports it to the user. Without this, errors in generated code are incomprehensible.

```cpp
struct SourceMap {
    // Core Erlang line number → original Kex location
    std::unordered_map<int, SourceLocation> coreToKex;
};
```

## File Layout (Full)

```
src/
  codegen/
    core_erlang.hxx          — CoreErlangEmitter class declaration
    core_erlang.cxx          — Implementation
    runtime_mappings.hxx     — Stdlib function name mappings
    source_map.hxx           — Core Erlang line ↔ Kex source location
  desugar/
    desugar.hxx              — Desugaring pass interface
    desugar.cxx              — Implementation
    error_prop.cxx           — ? operator rewriting
    loops.cxx                — Loop → recursive function transform
    ssa.cxx                  — Mutable variable SSA renaming
runtime/
  src/
    kex_runtime.app.src
    kex_io.erl
    kex_string.erl
    kex_list.erl
    kex_map.erl
    kex_result.erl
    kex_process.erl
    kex_enum.erl
    kex_genserver.erl
  test/
    kex_io_tests.erl
    kex_result_tests.erl
```

## Emitter Interface

```cpp
namespace kex::codegen {

class CoreErlangEmitter {
public:
    CoreErlangEmitter(const semantic::SemanticDB& db);

    // Emit an entire Kex module as Core Erlang source text + source map
    auto emitModule(const ast::ModuleDef& mod) -> EmitResult;

    // Emit the top-level program (main block + top-level functions)
    auto emitProgram(const ast::Program& program) -> EmitResult;

    struct EmitResult {
        std::string coreErlang;    // The generated source text
        SourceMap sourceMap;        // Line mapping for error reporting
    };

private:
    // Top-level
    auto emitFunction(const ast::FunctionDef& fn) -> std::string;
    auto emitExports(const std::vector<ast::FunctionDef>& fns) -> std::string;

    // Expressions (operates on desugared AST)
    auto emitExpr(const ast::ExprPtr& expr) -> std::string;
    auto emitBinaryOp(const ast::BinaryOp& op) -> std::string;
    auto emitMethodCall(const ast::MethodCall& call) -> std::string;
    auto emitFunctionCall(const ast::FunctionCall& call) -> std::string;
    auto emitRecordConstruction(const ast::RecordConstruction& rec) -> std::string;
    auto emitLambda(const ast::Lambda& lam) -> std::string;
    auto emitMatch(const ast::MatchExpr& match) -> std::string;
    auto emitIf(const ast::IfExpr& ifExpr) -> std::string;
    auto emitReceive(const ast::ReceiveExpr& recv) -> std::string;
    auto emitSpawn(const ast::SpawnExpr& spawn) -> std::string;
    auto emitLetChain(std::span<const ast::ExprPtr> body) -> std::string;
    auto emitLetrec(const LoopFunction& loop) -> std::string;

    // Patterns
    auto emitPattern(const ast::PatternPtr& pat) -> std::string;

    // Helpers
    auto freshVar(const std::string& hint = "") -> std::string;
    auto recordFieldIndex(const std::string& recordType, const std::string& field) -> int;
    auto resolveCall(const ast::MethodCall& call) -> ResolvedCall;

    // Source mapping
    auto mapLine(const ast::Node& node) -> void;

    const semantic::SemanticDB& m_db;
    int m_varCounter = 0;
    int m_currentLine = 1;
    SourceMap m_sourceMap;
};

} // namespace kex::codegen
```

## Implementation Order (Revised)

### Milestone 1: Hello World on BEAM

1. Implement Collect pass (register top-level function names, arities, record definitions, type definitions)
2. Implement Resolve pass (resolve names, flag undefined, group multi-clause functions)
3. Implement minimal TypeCheck (resolve UFCS targets for stdlib calls — enough to know `IO.printLine` → `kex_io:print_line`)
4. Build CoreErlangEmitter skeleton — modules, function defs, integer/string literals, atoms
5. Write `kex_io.erl` (just `print_line/1`)
6. Wire up `--compile` flag in main.cxx (shell out to erlc)
7. Get `hello.kex` → `.core` → `.beam` → running on `erl`

### Milestone 2: Basic Language

8. Arithmetic operators (map to `erlang:+`, `erlang:-`, etc.)
9. Let bindings → Core Erlang `let`
10. If/else → case on true/false
11. Multi-clause functions → case on args
12. Basic pattern matching (literals, variables, wildcards, tuples)
13. Lists and list patterns
14. Lambdas and higher-order functions
15. String interpolation → iolist construction

### Milestone 3: Records and UFCS

16. Record definitions → tagged tuple representation + `__record_info__`
17. Record construction and field access (positional, type-directed)
18. Make blocks → module with receiver-first functions
19. UFCS resolution using type info (type checker must resolve receiver types)
20. `.kexi` interface file generation
21. Separate compilation with interface lookup

### Milestone 4: Desugaring Complex Control Flow

22. SSA transform for mutable variables
23. While/loop → letrec recursive functions
24. `break` → return value from loop function
25. `next` → tail-call to loop function with current state
26. Nested loops (unique naming, inner-only break/next)
27. Error propagation (`?`) → nested match rewriting
28. `?` in compound positions (arguments, lambdas, branches)

### Milestone 5: Sum Types and Exhaustiveness

29. Sum type variants → tagged tuples
30. Pattern matching on sum types
31. Exhaustiveness checking (all variants covered)
32. Wildcard injection when non-exhaustive (with compiler warning)
33. Guards in patterns → Core Erlang `when`

### Milestone 6: Concurrency

34. spawn → `erlang:spawn/1`
35. send/receive → `erlang:send` + receive expression
36. After timeout in receive
37. Process linking basics
38. `kex_genserver` bridge for OTP interop

### Milestone 7: Full Language

39. Range expressions
40. Map literals and operations
41. Shorthand lambdas (`&.method`, `&function`)
42. Trailing if → case wrapper
43. Non-tail-recursion warnings
44. Source map for error reporting
45. Erlang port for structured compilation (replace system() shell-out)

### Milestone 8: Ecosystem

46. Runtime library completion (kex_runtime OTP app)
47. Mix compiler plugin for mixed Kex/Elixir projects
48. Package manifest (kex.toml) with dependency resolution
49. REPL backed by BEAM (compile + hot-load per expression)
50. Hot code reload documentation and best practices

## Design Decisions

### Why `Kex.` module prefix (not `Elixir.`)?

- Avoids false promises of Elixir protocol/struct/behaviour compatibility
- Makes language boundaries explicit in stack traces and `:observer`
- Interop is still trivial (cross-module calls work regardless of prefix)
- Can add an `@elixir_compat` pragma later if someone genuinely needs Elixir struct layout

### Why tagged tuples for records (not maps)?

- Pattern matching on tuples is fast and well-optimized on BEAM
- Field access by position is O(1)
- Maps are available separately for Kex's `{key: val}` syntax
- Sum type variants need tags anyway; tuples give a uniform representation
- Tradeoff: field reordering is breaking. Mitigated by recompilation tracking via `.kexi` interfaces.

### Why NOT maps (like Elixir structs)?

Elixir uses maps for structs to get field-name-based access without positional fragility. We choose tuples because:
- Kex is statically typed — the compiler always knows field positions, so positional access is safe
- Tuple pattern matching is faster than map pattern matching on BEAM
- We have `.kexi` files to track record layout changes across modules
- If interop with Elixir structs is needed, a conversion function (`to_elixir_struct/1`) is a one-liner

### Why SSA for mutability (not process dictionary)?

- Process dictionary is global mutable state — breaks reasoning about functions
- SSA is a compile-time transform with zero runtime cost
- The generated code is idiomatic Core Erlang (no mutation anywhere)
- Easier to optimize (dead variable elimination, etc.)
- Composes cleanly with the loop transform (mutable vars become loop params)

### Why a desugaring pass (not inline in the emitter)?

- **Separation of concerns:** "what does `?` mean?" is a language semantics question; "how do I print a case expression?" is a codegen question
- **Testability:** can unit-test desugaring (AST → AST) without generating text
- **Reuse:** the desugared AST could target other backends (WASM) without re-implementing control flow transforms
- **Composability:** SSA, loops, and `?` interact (a `?` inside a loop body). Doing them as separate sub-passes in a defined order is cleaner than handling all combinations in the emitter.

### Why no circular module dependencies?

- Simplifies compilation order (topological sort always works)
- Incremental recompilation is straightforward (DAG, not general graph)
- In practice, circular deps indicate a design problem — shared types should be extracted
- Can revisit with forward declarations if real-world usage demands it

### Loops: why letrec (not named recursive functions at module level)?

- `letrec` in Core Erlang is lexically scoped — the loop function is only visible inside the function that contains the loop
- No namespace pollution (no `while_0` in the module's export list)
- Multiple loops in the same function each get their own `letrec`
- BEAM compiles `letrec` into a tight tail-call loop — equivalent performance to a module-level function

## Open Questions

1. **Trait dispatch at runtime:** If a function takes `impl Comparable`, how does the compiled code dispatch? Options: dictionary passing (like Haskell), monomorphization (like Rust), or vtable (like Go interfaces). Deferred until traits are implemented.

2. **Generic specialization:** Does `List<Int>` have a different representation than `List<String>`? On BEAM, probably not — everything is already boxed. But if we ever target WASM/native, this matters. For now: uniform representation, no specialization.

3. **Binary/bitstring operations:** Erlang's bit syntax is powerful for protocol parsing. Does Kex expose it? Probably yes (important for the BEAM ecosystem), but the syntax needs design work.

4. **NIFs and ports:** How does Kex call native code? Probably through Erlang's existing NIF mechanism, with a Kex wrapper module. Low priority but needed for performance-critical paths.

5. **Dialyzer integration:** Should compiled Kex code carry type specs for Dialyzer? The type information exists — emitting `-spec` annotations in the generated code is straightforward and enables Dialyzer to check Kex↔Erlang boundaries.
