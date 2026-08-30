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
for a real regression in newer versions), plus two prebuilt static
dependencies: `third_party/gmp-wasm/{include,lib}` and
`third_party/pcre2-wasm/{include,lib}` (both gitignored rather than vendored;
each directory's README.md has its build recipe). CI builds and caches both
from scratch; see `.github/workflows/ci.yml`'s `wasm` job.

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
  - `src/common/` — shared helpers (color, completion, prelude_loader)
  - `src/stdlib/` — standard-library `.kex` sources; `prelude.kex` selects the automatically visible subset
  - `src/main.cxx` — CLI entry point

## Code Style

- C++20
- File extensions: `.hxx` (headers), `.cxx` (source)
- camelCase for function/method names
- `m_member` for class members
- `auto foo() -> ReturnType` trailing return style
- Namespace: `kex`

### Kex (`.kex` sources)
Use the `STYLE.md` file for concrete rules and examples for Kex code (`.kex` files).

## Current Status

Lexer, parser, AST, semantic analysis, tree-walk interpreter, the Elixir-style process runtime (fibers/scheduler), and BEAM codegen (lowering IR → Core Erlang) are all implemented. Type checker runs by default (`--no-check` to skip). Traits, currying (`~`), arbitrary-precision integers (GMP), and a rich stdlib are implemented.

Compile-time metaprogramming (`compiled do ... end`) is implemented: constants inlined at their use sites, `let %name` / `type %name` / `make %name` declaration generation, and builder-chain collapse that evaluates a fully-determined expression during compilation and reifies the result — carrying free runtime variables through as placeholders. `--collapse-report` says what collapsed and why the rest did not. See `docs/compiled-status.md`.

Next: a full module system and packaging. (The `.kexo` binary IR/distribution format is dropped.)
