# Type System

## Primitives

- `Integer` — arbitrary-precision integer (backed by GMP); the default for plain integer literals. `Int` is an alias for the fixed-width 64-bit form.
- `Float` — 64-bit floating point (the default for a plain float literal, e.g. `3.14`)
- `String` — UTF-8 string
- `Char` — a single character, written `'a'`, `'\n'`, etc.
- `Bool` — `true` or `false`
- `None` — absence of value (not falsy, must be pattern matched)

### `[Char]` is String — but `Char` is not

`[Char]` (a list of `Char`) *is* `String` — the same type, fully interchangeable for comparison, concatenation, and display. A single `Char`, however, is its own distinct type: `'a'` is not a 1-character `String`, and `==`/ordering between a `Char` and a `String` either return `false` or — for ordering — throw, the same as comparing any other two unrelated types. `Char` only compares/orders against another `Char`.

```kex
let c = "hello".at(1)        # Char: 'e' — String.at(i) returns the i'th
                              # element of the list, which is a Char
c == 'e'                     # true (Char == Char)
c == "e"                     # false — Char is not a String
'a' + 'b'                    # "ab" — concatenation still builds a String
"ab" + 'c'                   # "abc"
['h', 'i'] == "hi"           # true — [Char] IS String
IO.printLine(['h', 'i'])     # prints "hi", not "[h, i]"
```

Internally `Char` and `String`/`[Char]` stay separate runtime representations — only `String` and `[Char]` are the same type; `Char` is the distinct element type you get from indexing into one.

## Generics

Single-letter identifiers are generic type variables. Multi-letter identifiers are concrete types.

```kex
# A, B, K, V — generic
# Int, String, User — concrete
let identity(a: A) = a
let map(list: [A], f: A -> B) -> [B] = ...
```

## Traits

Traits declare type contracts. A trait lists required method signatures; default implementations use `let`. Types opt in via `make X implement: Trait do`.

```kex
trait Comparable do
  compare : This -> Comparison
end

trait Describable do
  let describe = "a ${this.to(String).or("")}"   # default — overridable
end

make Point implement: Comparable do
  let compare(other) -> Comparison do
    if @x != other.x then @x < other.x then Less else Greater
    elif @y < other.y then Less
    elif @y > other.y then Greater
    else Equal
    end
  end
end
```

- `This` — the concrete implementing type (type-level); `this` — the instance (value-level)
- `let name(...) = ...` inside a `trait` block is a default implementation, overridable in `make`
- A type can implement multiple traits: `make X implement: Comparable, Describable do`
- No diamond problem — trait membership is structural, not class-style inheritance

### Built-in Traits

| Trait | Satisfied by |
|-------|-------------|
| `Number` | Any integer or float type |
| `Integer` | `Integer` (arbitrary-precision) and all sized int types |
| `Float` | `Float32`, `Float64` |
| `Comparable` | Any type with `make X implement: Comparable` |
| `Equatable` | Any type with `make X implement: Equatable` |

### `Comparison` Type

`compare` returns a `Comparison` value — a built-in sum type:

```kex
type Comparison = Less | Equal | Greater
```

`sort`, `min`, and `max` all use `Comparison` dispatch for user types.

## Sum Types (Enums)

```kex
type Shape
  = Circle(Float)
  | Rectangle(Float, Float)
  | Triangle(Float, Float, Float)

type Optional<A> = Just(A) | None
type Result<A, E> = Ok(A) | Error(E)
```

## Union Types

Inline unions are allowed but discouraged in favor of declared types:

```kex
let handle(input: String | Int) = ...
```

## Optional

`String?` is sugar for `Optional<String>`. `None` is the empty optional value.

```kex
let email: String? = None
let name: String? = Just("Akos")
```

## Records

```kex
record User do
  name : String
  age : Int
  email : String? = None
end

let user = User { name: "Akos", age: 30 }
```

## Tuples

Fixed-size, heterogeneous, per-position types:

```kex
let point: (Int, Int) = (1, 2)
let (x, y) = point
```

## Lists

Variable-size, homogeneous:

```kex
let nums: [Int] = [1, 2, 3]
let [first | rest] = nums
```

## Maps

JSON-style syntax, bracket access:

```kex
let ages = { "alice": 30, "bob": 25 }
let age = ages["alice"]   # -> Int?
let { "alice": a } = ages
```

## Range

`1..10` is a `Range<Int>`, which is `Enumerable`. Has `.min` and `.max`.

```kex
let r = 1..10
r.max   # 10
r.map { |x| x * 2 }
```

## Enumerable Hierarchy

`Range`, `[A]` (lists), and `Stream` all implement the `Enumerable` trait,
the same `trait`/`make implement:` mechanism as the Type Hierarchy section
above — not nominal inheritance:

```kex
trait Enumerable do
  each : (This, A -> Unit) -> Unit
end

make Range implement: Enumerable do
  let each(f) = ...
end

make [A] implement: Enumerable do
  let each(f) = ...
end

make Stream implement: Enumerable do
  let each(f) = ...
end
```

(Required methods beyond `each` — whatever `.map`/`.min`/`.max` etc. need —
aren't fully enumerated here.)

## Atoms

Lightweight identifiers, separate from enum variants. Can appear in type positions:

```kex
let status: :ok | :error = :ok
type CounterMsg = :increment | :reset | (:get, Process<Int>)
```

## Block Type

`Block<A>` represents a `do...end` block:

- `Block<A>` — returns last expression
- `Block<[A]>` — collects each expression into a list

```kex
type HtmlFunction = String? -> [String] -> Block<[Element]> -> Element
```

## Specialized Make

`make` can target specialized generic types:

```kex
make [Int] do
  let sum = this.reduce(0, &.+)
end

make [String] do
  let unlines = this.join("\n")
end
```

The compiler picks the most specific match at the call site.
