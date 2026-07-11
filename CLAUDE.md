# NEVER run `git commit` or `git push`. Never propose or suggest it either. Leave all changes in the working tree.

# Kex Language Compiler

Kex is a functional programming language with Ruby-like syntax, UFCS (Uniform Function Call Syntax), immutability by default, and an Elixir-style process model. File extension: `.kex`.

## Build

```
cmake -B build -G "Unix Makefiles"
cmake --build build
```

Run: `./build/kex <file.kex>`

### Emscripten/wasm build

Powers `web/index.html`'s in-browser REPL. Requires `emsdk` active, pinned to
**5.0.7** (not Homebrew's Emscripten — see `third_party/gmp-wasm/README.md`
for a real regression in newer versions), and a prebuilt
`third_party/gmp-wasm/{include,lib}` (gitignored, not vendored — same
README has the build recipe). CI builds and caches this from scratch; see
`.github/workflows/ci.yml`'s `wasm` job.

```
make build-wasm   # emcmake cmake -B build-wasm && cmake --build build-wasm
make test-wasm    # + runs the interpreter test suite via Node
```

## Project Structure

- `grammar.ebnf` — formal grammar specification (EBNF)
- `examples/` — example `.kex` source files covering all language features
- `src/` — compiler source (C++20)
  - `src/lexer/` — tokenizer (token.hxx/cxx, lexer.hxx/cxx)
  - `src/parser/` — recursive descent parser (parser.hxx/cxx)
  - `src/ast/` — AST node types (ast.hxx)
  - `src/semantic/` — semantic analysis: SemanticDB, collect/resolve passes, typechecker, traits
  - `src/interpreter/` — tree-walk interpreter, fiber/scheduler process runtime, stdlib (stdlib/)
  - `src/ir/` — lowering IR + IR→Core Erlang emitter (ir.hxx, lower, emit_core)
  - `src/codegen/` — AST→Core Erlang emitter (core_erlang.hxx/cxx)
  - `src/common/` — shared helpers (color, completion, prelude_loader)
  - `src/prelude/` — prelude `.kex` sources
  - `src/main.cxx` — CLI entry point

## Code Style

- C++20
- File extensions: `.hxx` (headers), `.cxx` (source)
- camelCase for function/method names
- `m_member` for class members
- `auto foo() -> ReturnType` trailing return style
- Namespace: `kex`

## Current Status

Lexer, parser, AST, semantic analysis, tree-walk interpreter, the Elixir-style process runtime (fibers/scheduler), and BEAM codegen (lowering IR → Core Erlang) are all implemented. The BEAM backend matches the interpreter at full spec parity (109/109). Type checker runs by default (`--no-check` to skip). Traits, currying (`~`), arbitrary-precision integers (GMP), and a rich stdlib are implemented. Next: the `.kexo` binary IR/distribution format, a full module system, and packaging.
