# Modules and Visibility

> **Status.** Module declaration, nesting, `private do`, and qualified access
> all work in the interpreter. `using` — including `only:`, `except:`, and
> `as:` — is parsed and resolved during semantic analysis
> (`src/semantic/resolve_pass.cxx`), but the tree-walk interpreter still treats
> it as a **no-op** at runtime (names are not actually brought into scope — see
> `docs/testing.md`). A full module system across BEAM modules is still in
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

`using` brings public names from a module into scope. Bare, it takes everything
public:

```kex
using Html.Language do
  html do
    body do
      h1 "Hello"
    end
  end
end
```

### Selective imports

`only:` takes just the names listed; `except:` takes everything but them. The
two are mutually exclusive — using both is an error. `as:` binds the module to
a shorter name and imports nothing, leaving call sites qualified.

```kex
using Math, only: [square, cube]     # square and cube, nothing else
using Math, except: [cube]           # everything public but cube
using Pricing, as: P                 # no names imported; call it P.total
```

A name in an `only:`/`except:` list is a function, a type, or an operator in
parentheses:

```kex
using Vector, only: [magnitude, (+), (==)]
```

Prefer a short `only:` list to a bare `using`: it says what this scope borrowed,
and a name added to the module later cannot appear here without an edit. See
[STYLE.md](../STYLE.md) §7 for the full ladder — qualified call, `as:`,
`only:`, then bare `using`.

### Scope

Three placements, narrowest first:

```kex
let report(rows: [Row]) -> String do
  using Formatting                   # rest of this function body
  return rows.map(&.render).join("\n")
end

module App do
  using Http do                      # just these declarations
    let handleHome(req: Request) -> Response = Response.ok("Welcome!")
  end
end

module URI                           # rest of the file
using Parsing
```

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
