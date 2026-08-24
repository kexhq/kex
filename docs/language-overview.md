# Kex Language Overview

Kex is a functional programming language that combines Haskell's semantics with Ruby's syntax. It features UFCS (Uniform Function Call Syntax), immutability by default, a static type checker, currying, and first-class DSL support.

## Core Principles

- **Functional first** — immutable by default, pattern matching, algebraic types
- **UFCS** — `a.f(b)` is sugar for `f(a, b)`, enabling IDE code completion
- **Purity boundaries** — `foul` marks side effects, everything else is pure
- **Ruby-like syntax** — `do...end` blocks, closures, expressive DSLs
- **Typed processes and servers** — Elixir-style actors plus checked OTP-style `serving` protocols

## Quick Example

```kex
record User do
  name : String
  age : Int
end

make User do
  let greet({ name }) = "Hello, ${name}!"
  let adult?({ age }) = age >= 18
end

main do
  let user = User { name: "Akos", age: 30 }
  let msg = user.greet   # "Hello, Akos!"
end
```

`New { ... }` copies a record receiver with selected fields replaced;
`new.field = ...` performs repeated functional updates on an implicit local
copy. See [Records and Functional Updates](records-and-updates.md).

For state owned by a process, a `serving User do ... end` block declares typed
synchronous call and asynchronous cast slots. See [Typed Servers](serving.md).
