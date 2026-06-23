# Type System

## Primitives

- `Int` — integer
- `Float` — floating point
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

## Type Hierarchy

Types use open inheritance with multiple parents:

```kex
type Comparable do
  compare :> This -> Int
end

type Number < Comparable do
  add :> This -> This
end

type Integer < Number, Hashable
type Float < Number
```

- Abstract types declare required functions — children must implement them
- `This` resolves to the concrete type in `make` blocks
- Compile error on diamond ambiguity (must resolve explicitly)

## Sum Types (Enums)

```kex
type Shape
  = Circle(Float)
  | Rectangle(Float, Float)
  | Triangle(Float, Float, Float)

type Option<A> = Just(A) | Nothing
type Result<A, E> = Ok(A) | Error(E)
```

## Union Types

Inline unions are allowed but discouraged in favor of declared types:

```kex
let handle(input: String | Int) = ...
```

## Optional

`String?` is sugar for `Optional<String>`. `None` is the empty value.

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

```kex
type Range<A> < Enumerable<A>
type [A] < Enumerable<A>
type Stream<A> < Enumerable<A>
```

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
