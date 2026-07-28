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

`foul` can be applied at three levels:

```kex
# Per function
foul log(msg: String) = IO.printLine(msg)

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
foul counter = spawn do
  var state = 0       # foul — persists across receives
  loop do
    receive do
      :increment -> state = state + 1
    end
  end
end
```

## Build System Integration

Planned: the compiler will infer IO/Process requirements from usage and let the build config declare target capabilities (e.g. `target: wasm, capabilities: [IO]`), producing a compile error on mismatch. Not yet implemented — currently `foul` tracking is enforced at the semantic pass but capability declarations are not read.
