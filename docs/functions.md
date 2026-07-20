# Functions

## Declaration

```kex
# Simple expression
let double(n: Int) = n * 2

# Block body
let greet(name: String) -> String do
  return "Hello, ${name}!"
end

# Multiple clauses (pattern matching)
let factorial(0) = 1
let factorial(n: Int) = n * factorial(n - 1)
```

## Type Annotations

Two forms:

- `:>` — implicit `This` as first param (used inside `make` blocks)
- `:` — full explicit signature (used at top level)

```kex
# Inside make Integer:
modulo :> This -> This    # full type: This -> This -> This

# Top level:
element : String -> [String] -> Element
```

## UFCS (Uniform Function Call Syntax)

`a.f(b)` desugars to `f(a, b)`. This enables:
- IDE code completion (type `.` to see available functions)
- Method-call syntax on any type
- Chaining: `input.parse.transform.format`

## Make Blocks

Group functions by their first parameter:

```kex
make Integer do
  let even? = this.modulo(2) == 0
  let odd? = !this.even?
end

5.even?   # false — calls even?(5)
```

- Open by default — can be extended from anywhere
- `make final: Type` — closed, no reopening allowed

## The `to` Convention (Type Conversion)

Universal conversion via `value.to(TargetType)`. A catch-all implementation
dispatches through the private `Kex.Intrinsic.Fun.convertTo` primitive,
supporting `String`, `Integer`, `Float`, and `List` targets. Types can add
specialized clauses through `make`:

```kex
make Vector2D do
  let to(String) -> String? do
    return Just("(${this.x}, ${this.y})")
  end
end

# Usage:
42.to(String)             # Just("42")
3.14.to(Integer)          # Just(3)
"nope".to(Integer)        # None
myVec.to(String).or("")   # "(3.0, 4.0)"
```

`to` returns `Optional<TargetType>` because conversion can fail; callers
explicitly unwrap, pattern-match, or propagate that result.

## Pattern Matching in Functions

```kex
# Match on this (@ prefix)
make [A] do
  let first(@[]) = Nothing
  let first(@[x | _]) = Just(x)
end

# Destructure record fields
make User do
  let greet({ name }) = "Hi, ${name}!"
  let adult?({ age }) = age >= 18
end

# Nested destructuring
make Line do
  let horizontal?({ start: { y: y1 }, end: { y: y2 } }) = y1 == y2
end
```

## Default Parameters

```kex
let createElement(name: String, id: String? = None, classes: [String] = []) do
  ...
end
```

## Named Arguments

```kex
div(id: "main", classes: ["container"]) do
  ...
end
```

## Static Functions (Constructors)

Records and types can have `static do...end` blocks for namespaced constructors and constants:

```kex
record Vector2D do
  x : Float
  y : Float

  static do
    let Polar(length: Float, angle: Float) -> Vector2D do
      return Vector2D { x: length * Math.cos(angle), y: length * Math.sin(angle) }
    end

    let Zero = Vector2D { x: 0.0, y: 0.0 }
    let UnitX = Vector2D { x: 1.0, y: 0.0 }
  end
end

# Usage — fully qualified:
let v = Vector2D.Polar(5.0, Math.PI / 4.0)
let origin = Vector2D.Zero
```

Convention:
- **Capitalized** — constructors and constants (`Polar`, `Zero`, `UnitX`)
- **Lowercase** — instance methods in `make` blocks (`add`, `length`, `normalize`)

Static functions have no `this` — they create or return values of the type.

## Predicate Functions

Functions ending in `?` return `Bool`:

```kex
let empty?(list: [A]) -> Bool = ...
let adult?({ age }) = age >= 18
```

## Currying and Partial Application

`~` creates a partially applied function. Bound arguments fill left-to-right; `_` is an explicit placeholder for a specific position. Chained `(args)` groups that fully saturate the function call it immediately.

```kex
let add(a, b) = a + b
let multiply(a, b) = a * b

let inc    = ~add(1)           # {|b| add(1, b)}
let double = ~multiply(2)      # {|b| multiply(2, b)}

[1, 2, 3].map(~multiply(10))           # [10, 20, 30]
[1, 2, 3, 4, 5].reduce(0, ~(+))        # 15 — operator as a two-arg function

let sub5 = ~(-)(_, 5)                  # {|a| a - 5}
sub5(20)                               # 15

~(+)(2)(3)                             # 5 — chained, fully applied inline
~add(3)(4)                             # 7
```

`~(op)` lifts any built-in operator into a function value. `_` can appear multiple times; each one becomes a positional parameter filled left-to-right.

## Closures

```kex
# Inline
arr.map { |x| x + 1 }

# Multi-line
arr.each do |x|
  process(x)
end

# Zero-arg
let thunk = { 42 }

# Shorthand
arr.map(&.name)           # { |x| x.name }
arr.filter(&.adult?)      # { |x| x.adult? }
arr.sort(&compare)        # passes named function

# Value capture (not reference)
var x = 10
let snap = { x }
x = 20
snap()   # 10
```
