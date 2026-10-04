# Error handling

Kex uses values to represent missing data and expected failures:

- `A?` is shorthand for `Optional<A>`: `Just(value)` or `None`.
- `A or! E` is shorthand for `Result<A, E>`: `Ok(value)` or `Error(reason)`.

Use an optional when absence is enough information. Use a result when callers
need to know why an operation failed.

## Handling an optional

Supply a default with `or`, or match when each case needs different work:

```kex
let names = ["Ada", "Grace"]
names.first.or("unknown")   # => "Ada"
[].first.or("unknown")      # => "unknown"

match names.first do
  Just(name) => IO.printLine("Hello, ${name}")
  None => IO.printLine("No names supplied")
end
```

`map` transforms a present value and leaves `None` unchanged:

```kex
["Ada"].first.map(~upperCase).or("UNKNOWN")   # => "ADA"
```

Use `set?` and `none?` to check presence without extracting the value.

## Handling a result

Match to retain the error, or use `or` when discarding it is intentional:

```kex
match Integer.parse("42") do
  Ok(number) => IO.printLine(number)
  Error(reason) => IO.printError("Invalid number: ${reason}")
end

Integer.parse("invalid").or(8080)   # => 8080
```

`ok?` and `error?` check which case a result contains. They do not unwrap it.

## Chaining fallible steps

Use `map` when the next step returns a plain value. Use `flatMap` when it
returns another result or optional, to avoid nested wrappers:

```kex
Integer.parse("21").map { |n| n * 2 }   # => Ok(42)

Integer.parse("21").flatMap { |n|
  n > 0 then Ok(n) else Error("must be positive")
}   # => Ok(21)

Just("42").flatMap { |text| Integer.parse(text).optional }   # => Just(42)
```

An `Error` or `None` skips the remaining callbacks in its chain.

## Unwrapping with `try`

`.try` extracts a value from `Just` or `Ok`. On `None` or `Error`, it raises a
failure; it does not supply a default. Use `trying` and `rescue` to handle that
failure around a sequence of operations:

```kex
main do
  trying do
    let number = Integer.parse("invalid").try
    IO.printLine(number * 2)
  rescue
    reason => IO.printError("Could not parse: ${reason}")
  end
end
```

Use `match`, `or`, or `flatMap` when handling a single missing value or failure
is clearer at the call site.
