# Compilation

## Target: WebAssembly First

Kex compiles to WASM as the primary target. This gives:
- Runs everywhere (browser, server via WASI, edge)
- Near-native performance
- Sandboxed by default

## Layered Backends

Future targets will be added as needed:
- WASM (bare): basic IO
- WASM + runtime: processes, scheduling
- BEAM: full OTP-style distribution (potential future)
- Native/LLVM: maximum performance (potential future)

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
2. Expands `compiled` blocks (pure computation + `Env.get`)
3. Type-checks everything (including generated code)

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
  codegen/     — AST→Core Erlang emitter
  common/      — shared helpers (color, completion, prelude loader)
  prelude/     — prelude .kex sources
  main.cxx     — CLI entry point
```
