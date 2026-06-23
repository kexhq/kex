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

Universal conversion via `value.to(TargetType, ...options)`:

```kex
make Integer do
  let to(String) = BuiltIn.intToString(this)
  let to(String, base: Integer) = BuiltIn.intToStringBase(this, base)
  let to(Float) = BuiltIn.intToFloat(this)
end

make Vector2D do
  let to(String) -> String do
    return "(${this.x}, ${this.y})"
  end
end

# Usage:
42.to(String)             # "42"
42.to(String, base: 16)   # "2a"
3.14.to(Integer)          # 3
myVec.to(String)          # "(3.0, 4.0)"
```

The first argument is the target type (pattern matched). Additional arguments are options for the conversion. Every type that supports conversion implements `to` in its `make` block.

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
