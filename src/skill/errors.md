# Errors in Kex

There are no exceptions. Failure is a value, and the type says which kind:

- `X?` is an optional, `Just(x)` or `None`: something may be missing and the
  reason does not matter.
- `X or! E` is a result, `Ok(x)` or `Error(e)`: something may fail and the
  caller needs to know why. Make `E` a variant type, not a `String`.

```kex
type ConfigError = MissingKey(String) | NotANumber(String)

# `.try` unwraps a success and leaves the function on a failure. Here a
# missing key leaves through `rescue`, which says what the function answers.
let port(settings: Map<String, String>) -> Integer or! ConfigError do
  let text = settings.get("port").try
  match Integer.parse(text) do
    Ok(n)    => Ok(n)
    Error(_) => Error(NotANumber(text))
  end
rescue return Error(MissingKey("port"))
end

let half(n: Integer) -> Integer? = n.even? then Just(n / 2) else None

# `foul` because it prints. Every case of the result is matched.
foul report(settings: Map<String, String>) -> Void do
  match port(settings) do
    Ok(n)                => IO.printLine("port ${n}")
    Error(MissingKey(k)) => IO.printError("no ${k}")
    Error(NotANumber(t)) => IO.printError("bad number ${t}")
  end
end

# `rescue |error|` catches whatever `.try` let go of. It shares the
# function's `end`.
foul firstLine(path: String) -> String do
  using FS
  let text = FS.File.read(path).try
  return text.lines.first.or("")
rescue |error|
  IO.printError("cannot read ${path}")
  return ""
end

# Steps that can each fail chain with `flatMap`; `map` changes the value
# inside a success. The first failure is the answer.
let doubledPort(settings: Map<String, String>) -> Integer or! ConfigError do
  port(settings)
    .flatMap { |n| n > 0 then Ok(n) else Error(NotANumber("${n}")) }
    .map { |n| n * 2 }
end

main do
  IO.printLine(doubledPort({ "port": "21" }).or(0))
  report({ "port": "8080" })
  report({ "port": "http" })
  report({})
  IO.printLine(half(10).map { |n| n + 1 }.or(0))
  IO.printLine(firstLine("/nonexistent"))
end
```

## Rules

- Leave an optional or a result once, at the end of the chain:
  `config.get("port").map(~clean).or("8080")`.
- `.or(default)` takes a plain value. `.or(None)`, `.or(Just(x))` and
  `.or { ... }` are all wrong: the argument is the answer itself, and it is
  never called.
- `.try` works on optionals and results alike, inside a function that can
  itself fail or that ends in a `rescue`, in a `make` block as anywhere else.
- `result.map { |x| ... }` changes a success and passes a failure on;
  `result.flatMap { |x| ... }` runs another step that can fail. Both exist on
  optionals too. `&.` shortens them: `stock.take("tea", 1).flatMap(&.take("mug", 1))`.
- `if let Ok(n) = Integer.parse(text)` is for one interesting case; `match` is
  for when both cases do work.
- `rescue` has three forms: `rescue |error|` followed by lines,
  `rescue return value` on one line, and arms such as
  `NotFound(path) => return "missing"`.
- There is no `rescue do |error| ... end`, no `try do`, no `raise` and no
  `begin`.
- `die("message")` ends the program. It is for a broken invariant, never for
  a failure a caller could handle.
