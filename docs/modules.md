# Modules and Visibility

> **Status.** Module declaration, nesting, `private do`, `foul module`, and
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

## Foul Modules

```kex
foul module Logger do
  let info(msg: String) = ...
  let error(msg: String) = ...
end
```

All functions in a `foul module` are implicitly foul.

## Access

```kex
Math.pi
Math.Trig.sin(1.0)
Logger.info("hello")
```
