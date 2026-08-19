# Purity and Side Effects

## The `foul` Keyword

Kex tracks side effects via the `foul` keyword. Everything is pure by default.

```kex
# Pure — no side effects
let compute(x: Int) = x * 2 + 1

# Foul — does IO
foul readConfig(path: String) -> Result<String, IOError> do
  return IO.read(path)
end
```

## Rules

- Pure functions cannot call foul functions — enforced at semantic analysis (compile-time error)
- Foul functions can call anything
- `main` is implicitly foul

## Granularity

`foul` marks one function. There is no wider form — no foul block, and no
`foul module` — so a function's effect is always declared on the function:

```kex
module IO do
  foul printLine(msg: String) = ...
  foul getLine -> String = ...

  # Pure, and callable from pure code, despite the effectful company.
  let escape(msg: String) = ...
end
```

A module carries no effect of its own. Whether a call is legal from pure code
depends only on the function it names, so `let` always means pure and reading
one definition is enough to know (kexhq/kex#130).

## Process State

`var` that persists across `receive` cycles is foul (long-lived mutable state):

```kex
foul counter = spawn do
  var state = 0       # foul — persists across receives
  loop do
    receive do
      :increment => state = state + 1
    end
  end
end
```

## Build System Integration

Planned: the compiler will infer IO/Process requirements from usage and let the build config declare target capabilities (e.g. `target: wasm, capabilities: [IO]`), producing a compile error on mismatch. Not yet implemented — currently `foul` tracking is enforced at the semantic pass but capability declarations are not read.
