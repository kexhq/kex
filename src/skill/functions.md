# Functions in Kex

A function is a `let` with parameters. There is no `def`, no `fn` and no
`function`.

```kex
# The short form: one expression after `=`.
let square(n: Integer) -> Integer = n * n

# The block form: `do ... end`, for anything over one line. The last
# expression is the result.
let hypotenuse(a: Float, b: Float) -> Float do
  let sum = a * a + b * b
  Math.sqrt(sum)
end

# `return` leaves early. A guard is `return value if cond`.
let safeDivide(a: Integer, b: Integer) -> Integer? do
  return None if b == 0
  Just(a / b)
end

# No parameters: no parentheses, here or at the call.
let answer -> Integer = 42

# A default value, and a call that names its argument.
let greet(name: String, greeting: String = "Hello") -> String do
  "${greeting}, ${name}!"
end

# Types may be left out of a private helper and are inferred.
let twice(n) = n * 2

# Clauses: one function, one clause per pattern, tried top to bottom. Only
# then are the types written apart, in a declaration above the clauses; the
# clauses carry patterns alone.
fib : Integer -> Integer
let fib(0) = 0
let fib(1) = 1
let fib(n) = fib(n - 1) + fib(n - 2)

total : [Integer] -> Integer
let total([]) = 0
let total([first | others]) = first + total(others)

unwrap : Integer? -> Integer
let unwrap(Just(n)) = n
let unwrap(None) = 0

# A function that takes a function.
let applyTwice(f: Integer -> Integer, n: Integer) -> Integer = f(f(n))

# A function that takes a block: the block is the last parameter.
let repeated(times: Integer, action: Integer -> String) -> [String] do
  (1..times).map(action)
end

# A generic function: single capital letters stand for any type.
let pairUp(a: A, b: B) -> (A, B) = (a, b)

# `foul` marks I/O. A function without it cannot call a foul one.
foul log(message: String) -> Void = IO.printLine("[log] ${message}")

main do
  log("${square(4)} ${hypotenuse(3.0, 4.0)} ${safeDivide(1, 0).or(-1)}")
  log("${answer} ${twice(4)} ${fib(10)} ${total([1, 2, 3])} ${unwrap(None)}")
  log(greet("Ada"))
  log(greet("Ada", greeting: "Welcome"))

  # Any function can be called on its first argument.
  log("${4.square} ${10.fib}")

  # Functions as values: `~name`, with arguments fixed from the left.
  log("${applyTwice(~square, 3)} ${applyTwice(~twice, 3)}")
  let quadruple = ~applyTwice(~twice)
  log("${quadruple(1)}")

  # A lambda is a block; call it with parentheses.
  let increment = { |n| n + 1 }
  log("${increment(1)} ${applyTwice(increment, 1)}")
  log(repeated(2) { |i| "pass ${i}" }.join(", "))
  log("${pairUp(1, "one")}")
end
```

## Rules

- **Two forms.** `let name(arg: ArgType) -> RetType = expr` when the whole
  function fits on one line, `let name(arg: ArgType) -> RetType do ... end`
  whenever it takes more than one: several statements, or one expression too
  long for the line. Do not write `do return expr end` around an expression
  that fits.
- **Parameters** are `name: Type`, with no space before the colon. The return
  type follows `->`. This is where a function's types go whenever it has one
  body; do not write a separate declaration above a single `let`. A default
  is `name: Type = value`, and a caller may name an argument:
  `greet("Ada", greeting: "Welcome")`.
- **No parameters, no parentheses**, at the definition and at every call:
  `let answer -> Integer = 42`, then `answer`. `answer()` is wrong.
- **The result** is the last expression. `return` is for leaving early, most
  often as a guard: `return None if b == 0`.
- **Clauses.** Several `let`s with the same name are one function, matched top
  to bottom: most specific first, all of them together. This is the
  one case that takes a declaration: put the types in one line directly
  above (`fib : Integer -> Integer`) and only patterns in the clauses. A
  clause cannot carry a `when` guard; use `match` in the body for that.
- **Declarations** read `name : A -> B -> Result`: one arrow per parameter,
  then the result. A declaration is a contract for the name's type, which
  the `let` under it must keep. A parameter that is itself a function is
  parenthesized: `(Integer -> Integer) -> Integer -> Integer`. Inside a
  `make` a declaration is written `name :> B -> Result`, with the first
  parameter, the receiver, implied.
- **Calling.** `f(a, b)` and `a.f(b)` are the same call, for every function,
  so `4.square` works on a function you wrote a line earlier.
- **Functions as values.** `~square` is the function; `~add(10)` fixes its
  first argument; `{ |n| n + 1 }` is a lambda; `&.even?` is the lambda that
  calls `even?` on its argument. Call a function held in a variable with
  parentheses: `increment(1)`.
- **Blocks.** A function whose last parameter is a function can be called
  with a trailing block: `repeated(2) { |i| "pass ${i}" }`.
- **Generics.** Single capital letters are type parameters:
  `let pairUp(a: A, b: B) -> (A, B)`. Nothing is declared; using the letter is enough.
- **Effects.** A function that prints, reads, writes or talks to a process is
  `foul name(...)` instead of `let name(...)`. Only `main` and other `foul`
  functions may call it. Do not mark a pure function `foul`.
- **Names** are camelCase; a function answering `Bool` ends in `?`.
