# NEVER run `git commit` or `git push`. Never propose or suggest it either. Leave all changes in the working tree.

# Kex Language Compiler

Kex is a functional programming language with Ruby-like syntax, UFCS (Uniform Function Call Syntax), immutability by default, and an Elixir-style process model. File extension: `.kex`.

## Build

```
cmake -B build -G "Unix Makefiles"
cmake --build build
```

Run: `./build/kex <file.kex>`

## Project Structure

- `grammar.ebnf` — formal grammar specification (EBNF)
- `examples/` — example `.kex` source files covering all language features
- `src/` — compiler source (C++20)
  - `src/lexer/` — tokenizer (token.hxx/cxx, lexer.hxx/cxx)
  - `src/parser/` — recursive descent parser (parser.hxx/cxx)
  - `src/ast/` — AST node types (ast.hxx)
  - `src/main.cxx` — CLI entry point

## Code Style

- C++20
- File extensions: `.hxx` (headers), `.cxx` (source)
- camelCase for function/method names
- `m_member` for class members
- `auto foo() -> ReturnType` trailing return style
- Namespace: `kex`

## Current Status

Lexer, parser, AST, semantic analysis, and tree-walk interpreter are complete. Type checker runs by default (`--no-check` to skip). Traits, currying (`~`), arbitrary-precision integers (GMP), and a rich stdlib are implemented. Next: codegen (BEAM/WASM) or process runtime.
