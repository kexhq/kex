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

## Units

`Measure` is the shared representation for every unit family. Time units are
available directly from the prelude and produce time measures stored
canonically in seconds:

```kex
let elapsed: Measure = 2.5.sec

elapsed.canonical                 # 2.5
elapsed.kind                      # :time
elapsed.to(String)                # "2.5 s"
```

`Duration` is a separate elapsed-span concept, used by `Date`, `Time`, and
`DateTime` (below); a value such as `5.sec` is a `Measure`, not a `Duration`.
The plural spellings build durations: `5.seconds`, `90.minutes`, `2.days`.

## Types as values

`Type.of(x)` answers what a value is, as a record you can hold, print, compare
and take apart. It is in the prelude.

```kex
Type.of(42).toString                  # "Integer"
Type.of([1, 2]).toString              # "[Integer]"
Type.of((1, "a")).toString            # "(Integer, String)"
Type.of(Date.of(2026, 7, 30))         # Result<Date, TimeError>
Type.of(x) == Type.named("Date")      # ordinary record equality
Type.of(point).fields                 # ["x", "y"]
Type.of(Light).constructors           # ["Light", "Dark"]
Type.returnedBy(Date.parse).toString  # "Result<Date, TimeError>"

"34".to(Type.of(234))                 # Just(34) — `.to` takes a Type value
5.to(String)                          # Just("5") — or a bare type name
```

The answer comes from the compiler where it has one: a checked expression
knows things a value cannot carry, such as the unused half of a `Result` or
that `Light` belongs to `Shade`. Where it does not — gradual code,
`--no-check`, a value arriving from another process — the value itself is
asked. That fallback is honest but lossy: `Type.of([])` is `[?]`, because an
empty list has no element to inspect.

A function's type is its signature — parameters, result, and whether calling it
is a side effect:

```kex
Type.of(helloWorld).toString    # "String -> String"
Type.of(shout).toString         # "foul String -> String"
Type.of(shout).pure           # true
```

`Type.returnedBy` is answered by the compiler alone, so it needs the NAME of a
function: a lambda or a function value carries no signature at runtime, and an
overloaded name has no single answer. Both are compile errors.

Either form can be written where a TYPE goes — in a declaration, an
annotation, or a binding:

```kex
type Row = Type.returnedBy(parseRow)     # a computed alias, usable as a type
answer : Type.of(42)                     # a standalone declaration
let answer = 7
let items: [Colour] = []                 # ordinary annotations, now on bindings
```

One limit: expressions inside a string interpolation are not type-checked, so a
`Type.of` written in `${...}` falls back to the value and reports a bare
`Function` for a function argument. Bind it outside the interpolation.

## Dates and times

Three civil record types, all in the prelude. `Date` is a calendar day with no
time and no zone, `Time` a time of day with no date and no zone, and
`DateTime` both plus a fixed offset from UTC.

```kex
let due = Date.of(2026, 7, 30).try   # Result<Date, Time.Error>
due.iso                                  # "2026-07-30"
due.weekday.name                         # "Thursday"
(due + 10.days).iso                    # "2026-08-09"
due.addMonths(1).iso                     # clamps into the target month

let moment = DateTime.parse("2026-07-30T14:03:00+02:00").try
moment.utc.iso                           # "2026-07-30T12:03:00Z"
moment.epochSeconds                      # 1785412980
moment.compareTo(other)                  # Less | Equal | Greater, by instant

DateTime.now()                               # foul: reads the host clock
```

Each type has a module of its own — `Date.of`, `Date.parse`, `Date.fromEpochDay`,
`Time.of`, `Time.parse`, `Time.midnight`, `DateTime.of`, `DateTime.parse`,
`DateTime.fromEpochSeconds` — and all three answer `now()` and `utcNow()`
(`Date.today()` reads better where you want a day). Shared calendar helpers
(`Time.daysInMonth`, `Time.leapYear?`, `Time.formatDate`, `Time.errorMessage`)
live in `module Time`. Values are then used through methods on the records.

Zones are fixed offsets only — UTC, an explicit `+02:00`, or the system zone's
offset at a given instant. Named IANA zones and DST rules are not modeled, so
`DateTime.now()` is correct for the present moment but cannot predict the offset
for a future local time.

Physical SI constructors and arithmetic are opt-in through `Units.SI`. `using`
brings its public names into scope, so qualification is only needed without an
import or to resolve an ambiguous name:

```kex
using Units.SI

let distance = 100.meter  # Measure
let speed = distance / 9.58.sec
distance.kilo.to(String)  # "0.1 km"
let energy = 31544.watt * 1.hour
energy.to(String, in: Kilo(Watt * Hour))  # Just("31.544 kWh")
```

`Kilo`, `Mega`, `Giga`, `Milli`, `Micro`, and `Nano` construct display units.
Combine one with a unit expression in `to(String, in:)` to format a measure
without changing its canonical value. Target formatting returns `String?`:
it is `None` when the target has a different measurement kind.

Decimal and binary information units are provided separately by `Units.Data`.
They produce the same `Measure` type; the uppercase names are conversion unit
values:

```kex
using Units.Data

let asset: Measure = 5.megabytes
let binary: Result<Measure, String> = asset.convertTo(MiB)
let cache = 2.gibibytes
asset.to(String, in: Mega)  # Just("5.0 MB")
```

## Foldable and Enumerable

`Range`, `[A]` (lists), `String`, and `Map<K, V>` implement both collection
traits through the same `trait`/`make implement:` mechanism as the Type
Hierarchy section above — not nominal inheritance:

```kex
trait Foldable do
  reduce :> A -> (A -> T -> A) -> A
  # defaults: each, all?, any?, find, count(predicate)
end

trait Enumerable do
  reduce :> A -> (A -> T -> A) -> A
  # defaults: map, filter, flatMap, collect
end

make Range, implement: Enumerable, Foldable do
  let reduce(acc, f) = ...
end

make [A], implement: Enumerable, Foldable do
  let reduce(acc, f) = ...
end
```

`Stream<A>` remains lazy and provides its own `map`, `filter`, `take`, and
`drop`; it is not `Foldable` because reducing an unbounded stream may not
terminate.

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
  let sum = this.reduce(0, ~(+))
end

make [String] do
  let unlines = this.join("\n")
end
```

The compiler picks the most specific match at the call site.
