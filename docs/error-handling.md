# Error Handling

## No Exceptions

Kex has no exceptions. Errors are values.

## Two Error Types

- `Optional<A>` (aka `A?`) — value might not exist
- `Result<A, E>` — operation might fail with an error

## Optional

```kex
let find(list: [A], f: A -> Bool) -> A? do
  ...
end

# Handle with pattern matching
match findUser(users, "alice") do
  Just(user) -> "Found ${user.name}"
  None -> "Not found"
end

# Or with map/flatMap
findUser(users, "alice")
  .map(&.name)
  .unwrapOr("Unknown")
```

## Result

```kex
type ParseError = InvalidFormat(String) | Overflow | EmptyInput

let parseInt(s: String) -> Result<Int, ParseError> do
  return Error(EmptyInput) if s.empty?
  ...
end
```

## Checking Without Unwrapping

`ok?`/`error?` (on `Result`) and `present?`/`none?` (on `Optional`) ask "did this succeed" without a `match`:

```kex
if parseInt(input).ok? do
  ...
end
```

## Combining

Chain `Result`/`Optional` computations with `flatMap`:

```kex
foul let getUserEmail(id: Int) -> Result<String, AppError> do
  fetchUser(id).flatMap { |user| match user.email do
    Just(email) -> Ok(email)
    None -> Error(AppError(:no_email))
  end}
end
```
