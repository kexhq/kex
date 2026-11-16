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

A failure that nothing handles leaves the function with the failure as the
function's result. So a function that declares its result must be able to hold
one: `.try` is refused in a function whose declared result is neither a
`Result` nor an `Optional`, unless the function has a `rescue` or the `.try`
is inside `trying`.

```kex
# Refused: a caller would get an `Error` where a `User` was promised.
let registered(name: String) -> User do
  let user = find(name).try
  user
end

# The failure is the function's own result.
let registered(name: String) -> User or! String do
  let user = find(name).try
  Ok(user)
end

# Or the function answers for it.
let registered(name: String) -> User do
  find(name).try
rescue |reason|
  User { name: "nobody" }
end
```

A function that declares no result takes the failure into the result it
infers. `.try` on an `Optional` makes the result `T?`; `.try` on a
`Result<T, E>` makes it `T or! E`. The success comes back wrapped as `Just(v)`
or `Ok(v)`:

```kex
let initial(names: [String]) = names.first.try.upperCase   # String?
```

When the body already produces an `Optional` or a `Result`, that is the
result as it is. The checker refuses the inference in two cases: when one
function mixes `.try` on an `Optional` with `.try` on a `Result`, and when the
function also leaves early with `return`. Declare the result there instead.

A block is a function too, so the same holds for it. A block whose `.try`
can fail gives `T?` or `T or! E`, and `map` hands back each element's
outcome:

```kex
let parsed = ["1", "x"].map { |text| Integer.parse(text).try }
# [Ok(1), Error(ParseError { ... })]
```

Where the block is passed as a function that must give a plain value, the
call is refused like any other block of the wrong type. A block that mixes an
Optional's `.try` with a Result's keeps its own result type, because most
such blocks (`it`, `Task.start`) are not read.

Where a value cannot be missing, bind it by its pattern instead; a mismatch is
a fault, not a result:

```kex
let Just(first) = names.first
```
