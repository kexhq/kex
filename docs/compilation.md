# Compilation

## Backends today

Kex runs two ways today:

- **Tree-walk interpreter** (default) — `kex file.kex`.
- **BEAM** — a lowering IR (`src/ir/`) emits Core Erlang, which `erlc` turns
  into `.beam`. `kex -c` compiles, `kex -R` runs on BEAM, `kex -e` emits
  `.core` without invoking `erlc`. This backend is at full spec parity with the
  interpreter.

The interpreter is *also* compiled to WebAssembly via Emscripten to power the
in-browser REPL (`web/index.html`) — note that this is the interpreter running
in wasm, **not** Kex source compiled to wasm.

## Layered backends (potential future)

Directions considered but not built:
- Compiling Kex source directly to WASM (bare: basic IO; + runtime: processes, scheduling)
- Full OTP-style distribution on BEAM (multi-node)
- Native/LLVM for maximum performance

> Capability inference and pragmas below describe intended design and are **not
> yet implemented** — `foul` tracking is enforced at the semantic pass, but
> capability declarations are not read. Compiled metaprogramming IS built; see
> `docs/compiled-status.md`.

## Capability Requirements

The compiler infers what capabilities each module needs:

- `IO` — file, network, stdin/stdout
- `Process` — spawn, receive, supervision
- `Distribution` — multi-node processes
- `Env` — environment variables
- `FFI` — foreign function calls

The build config declares what the target supports:

```
target: wasm
capabilities: [IO]
```

If a module in the dependency graph needs `Process` but the target only supports `IO`, that's a compile error.

## Optional Pragma

For documentation or as an intentional constraint:

```kex
#[Require Process, IO]
module MyServer do
  ...
end
```

The compiler would error if you accidentally add IO to a module you intended to keep pure.

## Compiled Metaprogramming

The `compiled` block runs at compile time before type-checking:

1. Compiler collects all declarations
2. Expands `compiled` blocks (pure computation), hoisting what survives into
   the enclosing scope so no `compiled` block reaches any later stage
3. Type-checks everything, including generated code

Step 2's hoist is what makes step 3 true: the type checker walks past a
`compiled` block, so before it existed nothing inside one was ever checked.

`ENV.get` is the one permitted compile-time effect, and it works: the variable
is read while compiling and the value inlined, leaving no `ENV` reference in the
emitted module. Note the spelling — the module is `ENV`, not `Env`.

## Compiler Implementation

The compiler is written in C++20, with a tree-walk interpreter and a Core Erlang (BEAM) codegen backend, plus an Emscripten/WASM build of the interpreter. Structure:

```
src/
  lexer/       — tokenizer
  parser/      — recursive descent parser
  ast/         — AST node types
  semantic/    — SemanticDB, collect/resolve passes, typechecker, traits
  interpreter/ — tree-walk interpreter, fiber/scheduler process runtime, stdlib
  ir/          — lowering IR + IR→Core Erlang emitter
  common/      — shared helpers (color, completion, prelude loader)
  prelude/     — prelude .kex sources
  main.cxx     — CLI entry point
```
