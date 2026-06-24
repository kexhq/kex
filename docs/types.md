# Type System

## Primitives

- `Int` — integer (alias for `Int64`; arbitrary-precision `Integer` is the default for a plain literal — see Numeric Tower below)
- `Float64` — floating point (the default for a plain float literal, e.g. `3.14`)
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

## Type Hierarchy (Traits)

Type contracts are declared with `trait`, not nominal `type X < Y` blocks —
see `plan-effects-traits.md` for the full design. A trait lists required
method signatures (`:`); a type opts in by implementing it in a `make` block,
not by declaring a parent on the type itself:

```kex
trait Comparable do
  compare : This -> Comparison
end

trait Hashable do
  hash : This -> Int
end

make Point implement: Comparable, Hashable do
  let compare(other) -> Comparison = ...
  let hash -> Int = ...
end
```

Built-in numeric traits (`Number`, `Integer`, `Float`) work the same way, but
membership is structural/built-in rather than declared via `make implement:`
— every sized integer type (`Byte`, `Int8`..`UInt64`) and the
arbitrary-precision `Integer` type itself satisfy `Integer` (and
transitively `Number`); `Float32`/`Float64` satisfy `Float` (and `Number`)
the same way. `Integer` is unusual in that it's both the trait name *and*
one of its own concrete members — see `type-system-plan.md`'s Numeric Tower
section for the full tower (`Number` / `Integer` / `Float` / sized types /
the deferred `Rational`/`Decimal`):

```kex
Int8.is?(Integer)      # true
Int8.is?(Number)       # true
Float32.is?(Float)     # true
Float32.is?(Integer)   # false
Integer.is?(Integer)   # true
```

- Required methods (`name : type`) must be implemented; a `let name(...) = ...`
  inside the `trait` block itself is a default implementation, overridable
  by `make`.
- `This` resolves to the concrete implementing type, in both trait
  signatures and `make` blocks; `this` is the value-level instance.
- No nominal multiple-inheritance/diamond concern — trait membership is
  structural (`TraitRegistry.satisfies`), not class-style parent chains, so
  there's nothing to disambiguate when a type implements several traits.

## Sum Types (Enums)

```kex
type Shape
  = Circle(Float)
  | Rectangle(Float, Float)
  | Triangle(Float, Float, Float)

type Option<A> = Just(A) | None
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
