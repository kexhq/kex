# Modules and Visibility

> **Status.** Module declaration, nesting, `private do`, and
> qualified access all work in the interpreter. `using` is currently a **no-op**
> in the interpreter (names are not actually brought into scope — see
> `docs/testing.md`), and a full module system across BEAM modules is still in
> progress.

## Module Declaration

```kex
module Math do
  let pi = 3.14159
  let abs(n: Int) = ...

  module Trig do
    let sin(x: Float) = ...
  end
end
```

## Visibility

Functions are **public by default**. Use `private do...end` blocks for internal functions:

```kex
module Auth do
  private do
    let hashPassword(pw: String) = ...
    let validateLength(pw: String) = ...
  end

  # Public
  let authenticate(user: String, pw: String) = ...
end
```

Visibility scoping:
- `private` in a module — not exported
- `private` in a `make` block — only callable within that `make`
- Nested modules don't see parent's privates

## Using (Scoped Imports)

`using` brings all public names from a module into scope:

```kex
using Html.Language do
  html do
    body do
      h1 "Hello"
    end
  end
end
```

No selective imports — `using` brings everything public. If a module exports too much, split it.

## Effects in a Module

A module carries no effect of its own — there is no `foul module`. Each
function declares its own, so a `let` is pure wherever it appears:

```kex
module Logger do
  foul info(msg: String) = ...
  foul error(msg: String) = ...

  # Pure: formats a message without writing it anywhere.
  let format(level: Atom, msg: String) = ...
end
```

Calling `Logger.info` from a pure function is an error; calling
`Logger.format` is fine. See [purity.md](purity.md).

## Access

```kex
Math.pi
Math.Trig.sin(1.0)
Logger.info("hello")
```
