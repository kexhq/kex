# Purity and Side Effects

## The `foul` Keyword

Kex tracks side effects via the `foul` keyword. Everything is pure by default.

```kex
# Pure — no side effects
let compute(x: Int) = x * 2 + 1

# Foul — does IO
foul let readConfig(path: String) -> Result<String, IOError> do
  return IO.read(path)
end
```

## Rules

- Pure functions cannot call foul functions — compile error
- Foul functions can call anything
- `main` is implicitly foul

## Granularity

`foul` can be applied at three levels:

```kex
# Per function
foul let log(msg: String) = IO.printLine(msg)

# Per block
foul do
  let readConfig(path: String) = IO.read(path)
  let writeLog(msg: String) = Log.write(msg)
end

# Per module
foul module IO do
  let printLine(msg: String) = ...
  let getLine -> String = ...
end
```

## Process State

`var` that persists across `receive` cycles is foul (long-lived mutable state):

```kex
foul let counter = spawn do
  var state = 0       # foul — persists across receives
  loop do
    receive do
      :increment -> state = state + 1
    end
  end
end
```

## Build System Integration

The compiler infers requirements (IO, Process, etc.) from usage. The build config declares target capabilities:

```
# build config
target: wasm
capabilities: [IO]
```

Mismatch = compile error. Optional pragma for documentation:

```kex
#[Require Process, IO]
module MyServer do
  ...
end
```
