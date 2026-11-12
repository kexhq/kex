# Control structures in Kex

Every control structure is an expression: it has a value, and the value of
the last one in a function is what the function returns. There is no
statement that must be wrapped in `return`.

```kex
type Shape = Circle(Float) | Rect(Float, Float) | Dot

record User do
  name : String
  age  : Integer
end

# One line: `cond then a else b`. It is an expression and has a value.
let sign(n: Integer) -> String = n < 0 then "negative" else "positive"

# Several lines: `if` / `elif` / `else` / `end`, with no `then`. The whole
# `if` is the function's last expression, so its branches are the result.
let grade(score: Integer) -> String do
  if score >= 90
    "A"
  elif score >= 80
    "B"
  else
    "C"
  end
end

# A guard leaves early: `return value if cond`.
let ratio(a: Float, b: Float) -> Float do
  return 0.0 if b == 0

  a / b
end

# `match` picks by shape. Arms are `pattern => result`; a guard is `when`.
let describeNumber(n: Integer) -> String do
  match n do
    0            => "zero"
    1, 2, 3      => "small"
    n when n < 0 => "negative"
    _            => "large"
  end
end

# Patterns take variants, tuples, lists and records apart.
let area(shape: Shape) -> Float do
  match shape do
    Circle(r)  => Math.PI * r * r
    Rect(w, h) => w * h
    Dot        => 0.0
  end
end

let summary(items: [Integer]) -> String do
  match items do
    []               => "empty"
    [only]           => "just ${only}"
    [first | others] => "${first} and ${others.count} more"
  end
end

let fizzbuzz(n: Integer) -> String do
  match (n % 3, n % 5) do
    (0, 0) => "FizzBuzz"
    (0, _) => "Fizz"
    (_, 0) => "Buzz"
    _      => "${n}"
  end
end

let greeting(user: User) -> String do
  match user do
    { name: "root" } => "hello, admin"
    { name, age }    => "hello, ${name} (${age})"
  end
end

# An arm that is only `when` tests a condition and ignores the shape.
let temperature(degrees: Integer) -> String do
  match degrees do
    when degrees < 0  => "freezing"
    when degrees < 20 => "cool"
    _                 => "warm"
  end
end

main do
  IO.printLine("${sign(-2)} ${grade(85)} ${ratio(1.0, 0.0)}")
  IO.printLine("${describeNumber(2)} ${describeNumber(-9)} ${area(Rect(2.0, 3.0))}")
  IO.printLine("${summary([])}, ${summary([7])}, ${summary([1, 2, 3])}")
  IO.printLine((1..5).map(~fizzbuzz).join(" "))
  IO.printLine(greeting(User { name: "Ada", age: 36 }))
  IO.printLine(temperature(12))

  # `if let` runs its body when the pattern matches, and binds what it found.
  if let Just(n) = [4, 5].first
    IO.printLine("first is ${n}")
  else
    IO.printLine("nothing")
  end

  # Repetition is a function that takes a block.
  [1, 2, 3].each { |n| IO.printLine(n) }
  (1..3).each { |n| IO.printLine(n * n) }
  3.times { |i| IO.printLine("pass ${i}") }
  ["a", "b"].eachIndexed { |item, i| IO.printLine("${i}: ${item}") }

  # `while` and `loop` need a `var` to change. `break` leaves, `next` skips.
  var total = 0
  var n = 0
  while n < 10 do
    n += 1
    next if n.odd?
    total += n
  end
  IO.printLine(total)

  loop do |i|
    break if i >= 2
    IO.printLine("loop ${i}")
  end

  # A statement can carry its own condition.
  IO.printLine("big") if total > 20
end
```

## Choosing

- **One line:** `cond then a else b`. No `if`, no `end`, no `? :`.
- **Several lines:** `if cond` / `elif cond` / `else` / `end`. No `then` after
  the condition, and `elif` rather than `elsif` or `else if`.
- **Leaving early:** `return value if cond`. Any statement can carry a
  trailing `if`. There is no `unless`; write `if !cond`.
- **`if let pattern = value`** runs its body when the pattern matches and
  binds its names there. Use it when one case matters; it may have an `else`.
- Conditions must be `Bool`. A number, a string or an optional is not a
  condition: write `list.empty?`, `name != ""`, `found.set?`.
- `&&`, `||` and `!` are the logical operators. `and`, `or` and `not` are not:
  `or` is a function and a type (`a.or(b)`, `A or B`).

## Matching

- `match value do`, then arms `pattern => result`, then `end`. An arm whose
  result needs several lines is `pattern => do ... end`.
- Patterns: a literal (`0`, `"yes"`, `:ok`), several separated by commas
  (`1, 2, 3`), a variant (`Circle(r)`, `Just(x)`, `None`), a tuple (`(0, _)`),
  a list (`[]`, `[only]`, `[first | rest]`), a record (`{ name, age }`,
  `{ name: "root" }`), a name that binds anything, and `_` for anything.
  Patterns nest to any depth: `Just({ name, age: 36 })`, `Ok([first | _])`.
- A guard is `when`: `n when n < 0 => "negative"`. An arm that is only
  `when cond => result` tests the condition alone.
- Every case must be covered. The checker names the ones that are missing:
  `Non-exhaustive match on Light: missing case(s) Amber`. Cover them, and keep
  `_` for a real "everything else".
- A function whose whole body would be a `match` on its own parameter is
  written as clauses instead:
  `let area(Circle(r)) = ...` then `let area(Rect(w, h)) = ...`.
- Do not `match` on `true` and `false`; that is a conditional.

## Repeating

- Reach for a function first: `each`, `map`, `filter`, `reduce`, `find`,
  `any?`, `all?`, `eachIndexed`, `mapIndexed`, on a list, a map, a string or a
  range such as `1..10`. `3.times { |i| ... }` counts.
- A block takes a tuple apart by naming its parts: `{ |word, count| ... }`
  over a list of pairs or a map. `{ |(word, count)| ... }` is an error.
- Recursion with one clause per case is the normal way to walk a structure.
- `while cond do ... end` and `loop do ... end` are for work driven by a
  `var`. `loop do |i|` counts passes from 0. `break` leaves the loop and
  `next` starts the following pass; both work only inside `while` and `loop`,
  never inside a block passed to `each`.
- There is no `for`, no `until` and no `do ... while`.
- `next`, `break`, `match`, `when`, `then`, `loop` and `while` are reserved, so
  none of them can name a function or a variable.
