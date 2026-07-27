# Kex Language Specification

Kex is a functional programming language with Ruby-like syntax, immutability by
default, Uniform Function Call Syntax (UFCS), an Elixir-style process model, and
an effect system that distinguishes pure code from side-effecting ("foul") code.
It compiles to BEAM (Core Erlang) and also runs on a tree-walking interpreter.

File extension: `.kex`.

---

## 1. Program Structure

A Kex program is a sequence of top-level declarations:

| Declaration | Keyword | Purpose |
|---|---|---|
| Function | `let` / `foul` | Named functions and multi-clause definitions |
| Type | `type` | Union types, type aliases, abstract types |
| Record | `record` | Named-field product types |
| Trait | `trait` | Method contracts with optional defaults |
| Make block | `make` | Attach methods to a type |
| Module | `module` | Namespace declarations |
| Compiled block | `compiled` | Group definitions evaluated at compile time |
| Using | `using` | Import module members into scope |
| Pragma | `#[...]` | Compile-time directives |
| Main | `main` | Program entry point |

Execution starts in `main`. Top-level expressions outside any declaration are
implicitly wrapped in a synthetic `main` block, so small scripts need no `main`:

```kex
IO.printLine("hello")          # runs directly
```

A full program:

```kex
let greet(name: String) -> String do
  return "Hello, ${name}!"
end

main do
  IO.printLine(greet("world"))
end
```

---

## 2. Lexical Structure

### Comments

Line comments start with `#` and run to end of line:

```kex
# This is a comment
let x = 42  # inline comment
```

There are no block comments.

### Identifiers

| Convention | Meaning | Example |
|---|---|---|
| `lowerCase` | Functions, variables, fields, atoms | `map`, `userName` |
| `UpperCase` | Types, modules, records, constructors | `List`, `Just` |
| trailing `?` | Predicate (returns `Bool`) | `even?`, `empty?` |
| trailing `!` | Mutating call (rebinds receiver) | `push!`, `filter!` |

Lowercase identifiers may end with a single `?` or `!`. Uppercase identifiers
consist of `[A-Z][A-Za-z0-9]*`.

### Literals

| Token | Example | Type |
|---|---|---|
| Integer | `42`, `1_000_000` | `Integer` (arbitrary precision, GMP) |
| Float | `3.14`, `2.718_28` | `Float` |
| String | `"hello"`, `"v: ${expr}"`, `` `raw` `` | `String` |
| Char | `'a'`, `'0'`, `'\n'` | `Char` |
| Bool | `true`, `false` | `Bool` |
| Atom | `:ok`, `:error` | `Atom` |
| None | `None` | `None` |

Underscores in numeric literals are ignored (`1_000 == 1000`).

### String Interpolation

Strings support `${expr}` interpolation. The expression is evaluated and
converted to its string representation:

```kex
let name = "world"
IO.printLine("Hello, ${name}!")      # Hello, world!
IO.printLine("2 + 2 = ${2 + 2}")     # 2 + 2 = 4
```

Escape sequences: `\n`, `\r`, `\t`, `\\`, `\$`, `\"`.

### Raw Backtick Strings

A backtick string is multiline and non-interpolating. Backslashes are always
literal, `${...}` is ordinary text, and two consecutive backticks inside the
body produce one literal backtick.

```kex
let pattern = `\d+`
let text = `the syntax ${name} stays unchanged`
let quoted = `a ``backtick```
```

When the opening backtick is followed immediately by a newline and the closing
backtick sits on its own line, the exact whitespace prefix before the closing
backtick is removed from every nonblank content line. The opening newline is
omitted; the newline before the closing line remains.

### Raw Tagged Literals

An adjacent lower-case identifier tags a raw backtick string:

```kex
let firstPart(parts: [String], values: [Any]) -> String do
  parts.first.or("")
end

let text = firstPart`raw body`
```

The tag is an ordinary function called as `tag(parts, values)`. A raw tagged
literal supplies one string in `parts` and an empty `values` list. Whitespace
between the identifier and opening backtick disables tagging, so
`` firstPart `raw body` `` is two separate expressions.

### Interpolating Backtick Strings

A `$` before the opening backtick enables `${expression}` holes:

```kex
let name = "Ada"
let greeting = $`Hello, ${name}!`
```

The parser stores the string parts and expression ASTs separately. Each value
is converted to a string only when an untagged interpolating literal is
evaluated. Write `$${` for literal `${`; backslashes remain fully literal.

The same marker works with a tag:

```kex
let captured = capture$`SELECT * FROM users WHERE id = ${userId}`
```

This calls `capture(["SELECT * FROM users WHERE id = ", ""], [userId])`.
Values are passed in their original types; the tag decides whether to escape,
bind, convert, or reject them.

### Compile-Time Tag Validation

A raw tag may have a pure companion named `validateTag`:

```kex
let query(parts: [String], values: [Any]) -> String =
  parts.first.or("")

let validateQuery(source: String) -> [TaggedValidation.Issue] do
  if source.blank? then
    [TaggedValidation.fatal("query must not be empty")]
  else
    []
  end
end

let users = query`SELECT * FROM users`
```

The convention is mechanical: `regex` uses `validateRegex`, `html` uses
`validateHtml`, and `myTag` uses `validateMyTag`. The companion must have the
pure signature `String -> [TaggedValidation.Issue]`. Fatal issues fail
the build; warnings continue it.

```kex
module TaggedValidation

type ByteSpan = At(Integer) | Between(Integer, Integer)
type Issue = Fatal(ByteSpan?, String) | Warn(ByteSpan?, String)
```

Byte spans use zero-based UTF-8 byte offsets in the cooked literal body.
`Between(start, end)` has an exclusive end and is rendered as an underlined
source range. JSON diagnostics include `end_line` and `end_column` when a range
is present. Invalid or reversed spans are compile errors produced by the
validator boundary. An absent span covers the whole literal. Issues are ordered
by their starting position, with whole-literal issues last.

`TaggedValidation` also provides `fatal`, `fatalAt`, `fatalBetween`,
`warn`, `warnAt`, and `warnBetween` constructors for validator implementations.

Only raw tagged literals are checked. Interpolating tags and ordinary function
calls remain runtime operations. Each validator invocation has a compiler-owned
one-second timeout.

### Keywords

```
after  break  compiled  do  elif  else  end  false  final  foul
if  let  loop  main  make  match  module  next  None  private
public  receive  record  return  spawn  static  then  this
trait  true  type  using  var  when  while
```

---

## 3. Types

### Primitive Types

| Type | Description |
|---|---|
| `Integer` | Arbitrary-precision integer (GMP) |
| `Float` | IEEE 754 double |
| `String` | UTF-8 text, interpolated |
| `Char` | Single character |
| `Bool` | `true` or `false` |
| `Atom` | Symbolic constant (`:ok`) |
| `None` | The unit/empty value |

### Type Expressions

| Syntax | Meaning |
|---|---|
| `Integer` | Named type |
| `List<X>` | Generic application |
| `X -> Y` | Function type |
| `(A, B)` | Tuple type |
| `[X]` | List type |
| `{K: V}` | Map type |
| `A \| B` | Union type |
| `T?` | Optional sugar — `Optional<T> = Just(T) \| None` |
| `T or! E` | Result sugar — `Result<T, E> = Ok(T) \| Error(E)` |
| `A or B` | Either sugar — `Either<A, B> = Left(A) \| Right(B)` |
| `Block<X>` | Block/lambda type |

Generic parameters are uppercase single letters: `<A>`, `<A, E>`.

### Union Types (Algebraic Data Types)

```kex
type Shape
  = Circle(Float)
  | Rectangle(Float, Float)
  | Triangle(Float, Float, Float)

type ParseError = InvalidFormat(String) | Overflow | EmptyInput
```

Union variants are constructors: `Circle(5.0)` constructs a `Shape` value.
Variants with no payload (`Overflow`, `EmptyInput`) are atoms.

Pattern matching destructures them:

```kex
make Shape do
  let area = match this do
    Circle(r)       -> Math.PI * r * r
    Rectangle(w, h) -> w * h
    Triangle(a, _, h) -> 0.5 * a * h
  end
end
```

### Optional (`T?`)

`T?` desugars to `Optional<T> = Just(T) | None`:

```kex
let findUser(users: [User], name: String) -> User? do
  return users.find { |u| u.name == name }
end

match result do
  Just(user) -> user.name
  None       -> "not found"
end
```

### Result

`Result<T, E> = Ok(T) | Error(E)`. The `or!` sugar is shorthand:

```kex
let parsePort(s: String) -> Int or! ParseError do
  return Error(EmptyInput) if s.empty?
  return Ok(s.to(Integer).or(0))
end
```

`Int or! ParseError` is equivalent to `Result<Int, ParseError>`.

### Either

`Either<A, B> = Left(A) | Right(B)`. The `or` sugar is shorthand:

```kex
let classify(n: Integer) -> String or Integer do
  ...
end
```

### Type Hierarchy (parsed, not yet enforced)

The parser accepts `<` for type inheritance, but the hierarchy is currently
stored without semantic effect — there is no subtype checking, no method
inheritance from parent types, and no enforcement that subtypes implement
required signatures. This syntax is reserved for future use.

```kex
type Comparable do
  compare :> This -> Int
end

type Number < Comparable do
  add :> This -> This
end
```

---

## 4. Collections

### Lists

Ordered, homogeneous. `[X]` is the list type.

```kex
let nums = [1, 2, 3, 4, 5]
let empty = []
let consed = [0 | nums]          # prepend → [0, 1, 2, 3, 4, 5]
```

### Tuples

Fixed-size, heterogeneous:

```kex
let pair = (1, "hello")
let triple = (true, 42, :ok)
```

### Maps

Key-value stores. `{K: V}` is the map type.

```kex
let scores = { alice: 95, bob: 72 }              # atom keys (:alice, :bob)
let config = { "host": "localhost", "port": 80 }  # string keys
```

### Ranges

`a..b` creates a `Range` — a bounded sequence. Ranges implement `Enumerable`,
so they support `map`, `filter`, `reduce`, `sum`, `contains?`, etc.

```kex
let r = 1..100
r.sum                         # 5050
r.map { |x| x * x }.take(3)  # [1, 4, 9]
(2..n).all? { |d| n.modulo(d) != 0 }
```

---

## 5. Variables and Bindings

### Immutable (`let`)

```kex
let x = 42
let name = "Alice"
let (a, b) = (1, 2)          # tuple destructuring
let { name, age } = user    # record destructuring
```

### Mutable (`var`)

```kex
var count = 0
count = count + 1
```

`var` may carry a type annotation:

```kex
var total : Integer = 0
var done  : Bool = false
```

Only `var` bindings can be reassigned; `let` bindings are immutable.

---

## 6. Functions

### Definition

Single-expression functions use `=`:

```kex
let double(n: Integer) = n * 2
let add(a, b) = a + b       # types inferred
```

Multi-line functions use `do...end`:

```kex
let greet(name: String) -> String do
  return "Hello, ${name}!"
end
```

`return` is required to yield a value from a `do...end` body. The last
expression without `return` is **not** an implicit return — use `return`.

### Multiple Clauses (Pattern Matching)

A function may have several clauses distinguished by parameter patterns. Clauses
are tried in order:

```kex
let factorial(0) = 1
let factorial(n: Integer) = n * factorial(n - 1)

let head([h | _]) = Just(h)
let head([])      = None
```

### Type Annotations

Parameter types follow `:`; return type follows `->`. All annotations are
optional (inferred when omitted) but enable compile-time checking:

```kex
let divide(a: Float, b: Float) -> Float do
  return a / b
end
```

### Named Arguments

Parameters may have default values and callers may pass them by name:

```kex
let connect(host: String, port: Integer, timeout: Integer = 5000) do
  # ...
end

connect(host: "localhost", port: 8080)
connect(host: "localhost", port: 8080, timeout: 1000)
```

### Foul Functions

Functions performing I/O, process operations, or calling foul functions must be
marked `foul` (see §14 Effect System):

```kex
foul readConfig(path: String) do
  return FS.File.read(path)
end
```

### Trailing Conditionals

Any expression may be followed by a trailing `if`:

```kex
return Error(EmptyInput) if s.empty?
count = count + 1 if line.contains?("ERROR")
break if done
```

---

## 7. Operators

### Arithmetic

`+  -  *  /  %` (modulo)

### Comparison

`==  !=  <  >  <=  >=`

### Logical

`&&  ||  !` (prefix not)

### Range

`..` — creates a `Range`: `1..100`.

---

## 8. Control Flow

### If / Elif / Else

```kex
if n > 0
  "positive"
elif n < 0
  "negative"
else
  "zero"
end
```

Single-line `if` uses `then` for the body but still requires `end`:

```kex
if x > 0 then "yes" else "no" end
if x > 0 then "big" elif x < 0 then "small" else "zero" end
```

### Then-Else (Ternary)

A standalone ternary expression — no `if`, no `end`. Both branches are required:

```kex
let sign = n > 0 then "positive" else "negative"
```

Nesting another `then/else` requires parentheses.

### If-Let

Pattern match in an `if` condition; the body runs only if the match succeeds:

```kex
if let Just(v) = maybeValue
  IO.printLine("got: ${v}")
end
```

### Match

```kex
match shape do
  Circle(r)       -> 3.14 * r * r
  Rectangle(w, h) -> w * h
  _               -> 0.0
end
```

Match clauses support guards with `when`:

```kex
match n do
  0              -> "zero"
  n when n > 0   -> "positive"
  _              -> "non-positive"
end
```

A bare `when` (no pattern) acts as a catch-all guarded clause:

```kex
match value do
  when value > 100 -> "large"
  when value > 0   -> "small"
  _                -> "non-positive"
end
```

A named match variable binds the scrutinee:

```kex
match expr do |result|
  Ok(v)     -> handle(v)
  Error(e)  -> retry(result)
end
```

### While / Loop

```kex
while condition
  body
end

loop
  # runs forever until break
  break if done
end
```

`break` exits the nearest enclosing `while`/`loop`. `next` skips to the next
iteration. Using `break`/`next` outside a loop, or inside a closure passed to a
function (e.g. an `each` block), is a compile error — they only apply to
explicit `while`/`loop` constructs.

---

## 9. Pattern Matching

Patterns appear in `match`, function heads, `let` destructuring, and `if let`.

| Pattern | Matches |
|---|---|
| `42`, `"hi"`, `true`, `:ok` | Literal equality |
| `x` | Variable binding (any value) |
| `_` | Wildcard (any value, discarded) |
| `(a, b)` | Tuple |
| `[h \| t]` | List head/tail |
| `[]` | Empty list |
| `[a, b, c]` | Fixed-length list |
| `1..5` | Range (inclusive) |
| `Circle(r)` | Constructor (union variant) |
| `{ name, age }` | Record destructuring (shorthand) |
| `{ x: x1, y: y1 }` | Record field binding |
| `{ "key": val }` | Map string-key destructuring |
| `@Circle(r)` | Match on `this` (inside make blocks) |

### Record Destructuring

```kex
record User do
  name : String
  age  : Integer
end

record Address do
  city    : String
  country : String = "Hungary"
end

record Profile do
  user    : User
  address : Address
end

main do
  let user = User { name: "Alice", age: 30 }
  let addr = Address { city: "Budapest" }
  let profile = Profile { user: user, address: addr }

  # Shorthand — binds `name` and `age` from matching fields
  let { name, age } = user
  IO.printLine("${name} is ${age}")              # Alice is 30

  # Nested — reach into a record's inner records
  let { address: { city, country } } = profile
  IO.printLine("${city}, ${country}")            # Budapest, Hungary

  # Rename bindings with field: variable
  let { name: n, age: a } = user
  IO.printLine("${n} is ${a}")                   # Alice is 30
end
```

### List Patterns

```kex
match list do
  []           -> "empty"
  [x]          -> "single: ${x}"
  [x, y]       -> "pair: ${x}, ${y}"
  [x | rest]   -> "head: ${x}, tail length: ${rest.count}"
end
```

---

## 10. Lambdas and Blocks

### Inline Lambda

`{ |params| expr }`:

```kex
let inc    = { |x| x + 1 }
let add    = { |a, b| a + b }
let thunk  = { 42 }              # zero-arg
```

### Multi-Line Block

`do |params| ... end`:

```kex
list.each do |item|
  IO.printLine(item)
end
```

Rule: `{ |params| expr }` for one-liners, `do |params| ... end` for multi-line.

### Shorthand Lambda (`&`)

`&.method` expands to `{ |x| x.method }`:

```kex
list.filter(&.even?)              # { |x| x.even? }
list.map(&.to(String).or(""))    # { |x| x.to(String).or("") }
list.map(&double)                # { |x| double(x) }
```

### Currying (`~`)

`~f(arg)` produces a partial application — a lambda that supplies `arg` as the
first parameter of `f`:

```kex
let add(a, b) = a + b
let inc = ~add(1)                # { |b| add(1, b) }

[1, 2, 3].map(~add(10))          # [11, 12, 13]
(1..100).reduce(0, ~(+))         # 5050

let sub5 = ~(-)(_, 5)            # { |a| a - 5 }  (_ = placeholder)
```

Operators can be curried directly: `~(+)`, `~(-)`, `~(*)`. Use `_` as a
placeholder to fix the non-leading argument.

### Higher-Order Functions

```kex
let applyTwice(f, x) = f(f(x))
applyTwice(~add(1), 5)           # 7
```

---

## 11. Uniform Function Call Syntax (UFCS)

Any function `f(x, ...)` can be called as `x.f(...)`. The receiver becomes the
first argument. This enables method-chaining pipelines and erases the
distinction between free functions and methods:

```kex
let result = requests
  .filter { |req| req.path.startsWith?("/admin") }
  .map { |req| req.path }

# equivalent to:
let result = map(filter(requests, { |req| req.path.startsWith?("/admin") }), { |req| req.path })
```

Free functions, make-block methods, and trait methods are all callable via UFCS.

---

## 12. Mutating Calls (`!`)

The `!` suffix rebinds a `var` to the result of a method call. It is syntactic
sugar — `list.push!(6)` is equivalent to `list = list.push(6)`:

```kex
var list = [1, 2, 3]
list.push!(6)                    # list is now [1, 2, 3, 6]
list.filter!(&.even?)            # list is now [2, 6]
list.map! { |x| x * 10 }        # list is now [20, 60]
```

Using `!` on a `let` binding is a compile error:

```kex
let frozen = [1, 2, 3]
# frozen.push!(4)  # ERROR: push! requires a var binding
```

---

## 13. Records

Records are immutable product types with named fields, optional defaults, and
optional type annotations:

```kex
record User do
  name  : String
  age   : Integer
  email : String? = None        # default value
end

record Address do
  street  : String
  city    : String
  country : String = "Hungary"  # default value
end
```

Construction uses `Type { field: value }`. Fields with defaults may be omitted:

```kex
let user = User { name: "Alice", age: 30 }
let addr = Address { street: "Main St", city: "Budapest" }
```

Fields use explicit `field: value` syntax in construction:

---

## 14. Make Blocks

`make` attaches methods to a type. Inside a make block, `this` refers to the
receiver and `@field` is shorthand for `this.field`.

Parameterless make methods automatically receive `this`, giving access to
`@field`:

```kex
record User do
  name : String
  age  : Integer
end

make User do
  let greet -> String do
    "Hi, ${@name}!"
  end

  let adult? -> Bool = @age >= 18

  let summary -> String = "${@name} is ${@age}"
end

main do
  let u = User { name: "Alice", age: 30 }
  IO.printLine(u.greet)          # Hi, Alice!
  IO.printLine(u.adult?)         # true
  IO.printLine(u.summary)        # Alice is 30
end
```

> **Note:** make methods that take **explicit parameters** beyond the implicit
> `this` are dispatched with the caller's receiver as `this`. This is supported
> for prelude types (e.g. `[X].push(x)`, `Map.put(k, v)`) but, for **user
> records**, `@field` access in parameterized make methods is not yet supported —
> use `match this do ... end` or a parameterless method in those cases.

### `@` Shorthand

Inside make blocks, `@field` is shorthand for `this.field`, and `@method(args)`
is shorthand for `this.method(args)`:

```kex
make Circle do
  let area = Math.PI * @radius * @radius
end
```

### Pattern Matching on `this` (`@` patterns)

Make methods can pattern-match on `this` directly using `@`:

```kex
make [A] do
  let isEmpty?(@[])  = true
  let isEmpty?(@_)   = false

  let head(@[x | _]) = Just(x)
  let head(@[])      = None
end
```

### Final Make

`make final: Type` seals the make block — it cannot be reopened:

```kex
make final: Bool do
  let to(String) do
    match this do
      true  -> Just("true")
      false -> Just("false")
    end
  end
end
```

### Operator Overloading

Operators can be overloaded inside make blocks:

```kex
record Vec2 do
  x : Float
  y : Float
end

make Vec2 do
  let +(other: Vec2) -> Vec2 = Vec2 { x: @x + other.x, y: @y + other.y }
  let ==(other: Vec2) -> Bool = @x == other.x && @y == other.y
end
```

---

## 15. Traits

Traits declare method contracts. Required methods use `:>` (implicit `this` as
first parameter). Default implementations may call other trait methods via
`this`:

```kex
trait Shape do
  area      :> Float          # required
  perimeter :> Float          # required

  let describe = "area=${this.area}"   # default implementation
end
```

A type implements a trait with `make Type, implement: Trait`. The type must be a
record (so `@field` works) or provide its own `this` handling:

```kex
record Circle do
  radius : Float
end

record Rectangle do
  width  : Float
  height : Float
end

make Circle, implement: Shape do
  let area      = Math.PI * @radius * @radius
  let perimeter = 2.0 * Math.PI * @radius
  # describe inherited from the trait default
end

make Rectangle, implement: Shape do
  let area      = @width * @height
  let perimeter = 2.0 * (@width + @height)
  let describe  = "Rectangle ${@width}x${@height}"   # override default
end

main do
  let c = Circle { radius: 5.0 }
  let r = Rectangle { width: 3.0, height: 4.0 }
  IO.printLine(c.describe)       # area=78.539816
  IO.printLine(r.describe)       # Rectangle 3.0x4.0
end
```

A trait may require foul methods:

```kex
trait Logger do
  foul write :> String -> Unit
end
```

### Trait-Constrained Functions

Function parameters may be constrained to a trait:

```kex
foul printShape(s: Shape) do
  IO.printLine(s.describe)
end
```

---

## 16. Modules

Modules namespace functions, records, types, traits, and nested modules:

```kex
module Geometry do
  let square(n) = n * n
  let cube(n)   = n * n * n

  module Constants do
    let pi  = Math.PI
    let tau = 2.0 * Math.PI
  end
end
```

### Qualified Access

```kex
Geometry.square(5)
Geometry.Constants.tau
```

### Using (Import)

```kex
using Math, only: [sqrt]          # selective import
using Math, except: [tan]         # exclusion import
using Math, as: M                 # alias
using Math, as: M, only: [sqrt]   # alias + selective
```

Scoped `using` block:

```kex
using Math, only: [sqrt] do
  IO.printLine(sqrt(16.0))
end
```

### Privacy

```kex
module StringUtils do
  private do
    let repeat(s, n) = # ...   # only visible inside this module
  end

  let padLeft(s, width) = # ... # can call repeat internally; public
end
```

`public do ... end` blocks mark explicit public sections.

### Export (Re-export)

```kex
module Shop do
  export Pricing, only: [subtotal, standardTotal]
end
```

### Nested Visibility Rules

Inside `private`/`public` blocks, the parser accepts: functions (`let`/`foul`),
type annotations, `make` blocks, `type`, `record`, and `using` declarations.
Nested modules, traits, and compiled blocks are not allowed inside visibility
sections.

---

## 17. Effect System (Pure / Foul)

Kex distinguishes **pure** functions from **foul** (side-effecting) ones.
Functions are pure by default. The `foul` keyword marks functions that perform
I/O, use processes, call other foul functions, or otherwise have observable
side effects:

```kex
let compute(x: Integer) = x * 2 + 1     # pure

foul readConfig(path: String) do
  return FS.File.read(path)
end
```

Rules:

- A pure function **cannot** call a foul function (compile error).
- `main` is implicitly foul.
- Modules may be marked `foul module`, making every member foul.
- Guards in `match` must be pure — foul calls in guards are a compile error.

`IO`, `FS.File`, `FS.Directory`, `Http`, `System`, `Task`, and `Process`
operations are foul.
Reading global `ENV` is foul because it is ambient process input. `main` also
receives the immutable environment snapshot as an explicit parameter; passing
that map to a helper preserves purity.

---

## 18. Processes

Kex has an Elixir-style process model: lightweight processes, message passing,
links, monitors, and supervision trees.

### Spawn and Send

`spawn do ... end` starts a new process and returns a `Pid`:

```kex
foul startCounter -> Pid do
  return spawn do counterLoop(0) end
end
```

`pid.send(msg)` sends a message (any value) to a process:

```kex
counter.send(Increment)
```

### Receive

`receive do Pattern -> body end` blocks until a matching message arrives:

```kex
foul counterLoop(n: Integer) do
  receive do
    Increment       -> counterLoop(n + 1)
    Reset           -> counterLoop(0)
    Get(sender)     -> do
      sender.send(n)
      counterLoop(n)
    end
  end
end
```

### Receive with Timeout

```kex
receive timeout: 5000 do
  (:ok, value) -> handle(value)
after -> IO.printLine("timed out")
end
```

### Tasks

`Task.start { expr }` spawns a background computation and returns a `Task`:

```kex
let t = Task.start { slowCompute(42) }

match t.await(5000) do
  Ok(result)  -> IO.printLine("got: ${result}")
  Error(_)    -> IO.printLine("task failed or timed out")
end
```

`Task.awaitAll([tasks])` awaits a list of tasks.

### Process Introspection

```kex
let me = Process.self            # caller's Pid
pid.alive?                       # is the process alive?
pid.link                         # link caller to pid (bidirectional exit)
pid.monitor                      # returns a Reference; demonitor with ref.demonitor
Process.register(pid, :name)     # register pid under an atom
Process.whereis(:name)           # Pid? for a registered name
Process.exit(pid, reason)        # send an exit signal
```

### Supervision

`Supervisor.start` starts a supervisor over a list of worker specs returned by
its block. Each worker wraps a zero-argument block that should `spawn` the child
and return its pid:

```kex
foul startCounter(name: String) do
  spawn do counter(name, 1) end
end

main do
  let result = Supervisor.start(restart: :only_crashed) do
    [worker { startCounter("counter-A") }]
  end
  match result do
    Ok(pid)    -> IO.printLine("supervisor started: ${pid}")
    Error(msg) -> IO.printLine("supervisor error: ${msg}")
  end
end
```

> **Note:** `worker` and `Supervisor.start` are runtime intrinsics not known to
> the semantic checker — use `--no-check` if the checker reports `worker` as
> undefined, or run on the BEAM backend.

> **Backend note:** the interpreter supports only `restart: :only_crashed`. The
> `:all` and `:crashed_and_newer` strategies require the BEAM backend
> (`kex -R`) and produce an `Error` on the interpreter.

---

## 19. Streams

Lazy, potentially infinite sequences. Unlike ranges (which are bounded),
streams evaluate elements on demand:

```kex
let naturals = Stream.Sequence(from: 0) { |n| n + 1 }
let firstTen = naturals.take(10)        # [0, 1, 2, ..., 9]

let primes = Stream.Sequence(from: 2) { |n| n + 1 }
  .filter do |n|
    (2..n - 1).all? { |d| n.modulo(d) != 0 }
  end

primes.take(5)                          # [2, 3, 5, 7, 11]
```

`Stream.Iterate(seed, step)` is an alias for `Sequence`. Stream methods:
`take(n)` → list, `drop(n)` → stream, `map(f)` → stream, `filter(pred)` →
stream.

---

## 20. Compiled Blocks

`compiled do...end` groups definitions that are available as module members and
evaluated at compile time. This is used to build zero-overhead DSLs:

```kex
module SQL do
  record Query do
    text   : String
    params : [Any] = []
  end

  record QueryBuilder do
    fields     : [Atom] = []
    table      : Atom   = :none
    conditions : [(String, Any)] = []
  end

  compiled do
    let select(fields) -> QueryBuilder do
      QueryBuilder { fields: fields }
    end

    make QueryBuilder do
      let from(table) -> QueryBuilder do
        QueryBuilder { fields: @fields, table: table, conditions: @conditions }
      end
      # ...
    end
  end
end

main do
  let q = SQL.select([:all]).from(:users)
  IO.printLine(q)                 # QueryBuilder { ... }
end
```

See `examples/compiled_sql.kex` and `examples/compiled_css.kex` for full
working examples.

---

## 21. Pragmas

Pragmas are compile-time directives placed before declarations:

```kex
#[Pure]
let compute(x) = x * 2
```

Pragmas are currently parsed and stored as metadata; they do not alter code
generation. `#[Pure]` documents intent but is not enforced beyond the existing
effect system.

The comment pragma `# kex: no-check` at the top of a file disables the type
checker for that file.

---

## 22. Testing

Kex ships a testing DSL in the prelude: `describe`, `it`, `assert`, plus
`before`/`after` hooks and an `Assert` helper module.

```kex
describe("Integer") do
  describe("even?/odd?") do
    it("identifies even numbers") do
      assert(4.even?)
      assert(0.even?)
      assert(!3.even?)
    end
  end
end
```

`assert` takes a `Bool` and fails the current test if false. An optional message
may be supplied: `assert(x > 0, "x must be positive")`.

`Assert` helpers: `Assert.equal(a, b)`, `Assert.notEqual(a, b)`,
`Assert.truthy(v)`, `Assert.falsy(v)`, `Assert.some(opt)`, `Assert.none(opt)`,
`Assert.ok(result)`, `Assert.error(result)`.

`before` / `after` run per-test by default; `before(:all)` / `after(:all)` run
once per group.

Test files use the `.spec.kex` convention — `foo.spec.kex` auto-loads
`foo.kex`'s declarations without needing to re-declare them.

---

## 23. Standard Library (Prelude)

The prelude is loaded automatically. Prelude methods are **sealed** — user code
can extend types with new methods but cannot redefine prelude methods.

The prelude is built on several core traits. `Enumerable` provides `map`,
`filter`, `each`, `reduce`, `find`, `any?`, `all?`, `flatMap`, `collect`, and
`count(pred)` to any type that supplies a `reduce`. `Blankable` provides
`blank?`/`present?`. `Truthyable` provides `truthy?`/`falsy?`.

### 23.1 String

String implements `Enumerable` (over characters) and `Blankable`.

| Method | Signature | Description |
|---|---|---|
| `count`, `length` | `-> Integer` | Character count |
| `empty?` | `-> Bool` | No characters |
| `at(i)` | `-> Char?` | Character at index |
| `chars` | `-> [Char]` | List of characters |
| `rest` | `-> String` | All but first char |
| `split(sep)` | `-> [String]` | Split on separator |
| `trim` | `-> String` | Strip leading/trailing whitespace |
| `upperCase` | `-> String` | Uppercase |
| `lowerCase` | `-> String` | Lowercase |
| `reverse` | `-> String` | Reverse |
| `contains?(sub)` | `-> Bool` | Substring test |
| `startsWith?(pre)` | `-> Bool` | Prefix test |
| `endsWith?(suf)` | `-> Bool` | Suffix test |
| `map(f)` | `(Char -> B) -> [B]` | Map chars |
| `filter(pred)` | `(Char -> Bool) -> String` | Keep matching chars |
| `uniq` | `-> String` | Remove duplicate chars |

### 23.2 Char

| Method | Description |
|---|---|
| `upperCase` / `lowerCase` | Case conversion (returns `Char`) |
| `digit?` | `0`–`9` |
| `alpha?` | Alphabetic |
| `space?` | Whitespace |
| `in?(range)` | Membership in a `Range<Char>` |

### 23.3 List

List implements `Enumerable`, `Blankable`. Numeric lists (`[Number]`) have
`sum`/`product`/`min`/`max`; `[String | Char]` lists have `join`.

| Method | Signature | Description |
|---|---|---|
| `first`, `second`, `third`, `last` | `-> X?` | Positional access |
| `rest` | `-> [X]` | All but first |
| `count`, `length` | `-> Integer` | Length |
| `empty?` | `-> Bool` | |
| `at(i)`, `get(i)` | `-> X?` | Index access |
| `get(i, default)` | `-> X` | Index with default |
| `contains?(x)` | `-> Bool` | Membership |
| `indexOf(x)` | `-> Integer?` | First index |
| `map(f)` | `(X -> Y) -> [Y]` | |
| `filter(pred)` | `(X -> Bool) -> [X]` | |
| `reject(pred)` | `(X -> Bool) -> [X]` | Inverse of filter |
| `flatMap(f)` | `(X -> [Y]) -> [Y]` | Map + concat |
| `each(f)` | `(X -> Unit) -> Unit` | Side-effect loop |
| `find(pred)` | `(X -> Bool) -> X?` | First match |
| `any?(pred)`, `all?(pred)` | `(X -> Bool) -> Bool` | Quantifiers |
| `collect(f)` | `(X -> Y?) -> [Y]` | Filter+map via Optional |
| `reduce(init, f)` | `(A, A -> X -> A) -> A` | Left fold |
| `take(n)`, `drop(n)` | `Integer -> [X]` | Prefix/suffix |
| `push(x)` | `X -> [X]` | Append |
| `reverse` | `-> [X]` | |
| `sort` | `-> [X]` | Natural order |
| `sort(cmp)` | `(X -> X -> Bool) -> [X]` | Custom comparator |
| `flatten` | `-> [X]` | One level |
| `zip(other)` | `[Y] -> [(X, Y)]` | Pairwise |
| `partition(pred)` | `(X -> Bool) -> ([X], [X])` | Split |
| `uniq` | `-> [X]` | Deduplicate |
| `join(sep)` | `String -> String` | (on `[String\|Char]`) |
| `sum`, `product` | `-> Number` | (on `[Number]`) |
| `min`, `max` | `-> X?` | (on `[Number]`) |
| `min(f)`, `max(f)` | `(X -> Y) -> X?` | By key |

### 23.4 Map

Map implements `Enumerable` (over `(key, value)` pairs), `Blankable`.

| Method | Signature | Description |
|---|---|---|
| `get(key)` | `-> V?` | Value or `None` |
| `get(key, default)` | `-> V` | Value or default |
| `put(k, v)` | `-> Map<K,V>` | New map with entry |
| `delete(k)` | `-> Map<K,V>` | New map without key |
| `has?(k)` | `-> Bool` | Key exists |
| `keys` | `-> [K]` | All keys |
| `values` | `-> [V]` | All values |
| `entries` | `-> [(K,V)]` | All pairs |
| `count` | `-> Integer` | Entry count |
| `merge(other)` | `Map<K,V> -> Map<K,V>` | Union; other wins |
| `mapValues(f)` | `(V -> W) -> Map<K,W>` | Transform values |
| `mapKeys(f)` | `(K -> J) -> Map<J,V>` | Transform keys |
| `filter(pred)` | `((K,V) -> Bool) -> Map<K,V>` | Keep matches |
| `reject(pred)` | `((K,V) -> Bool) -> Map<K,V>` | Drop matches |
| `each(f)` | `((K,V) -> Unit) -> Unit` | Iterate pairs |
| `map(f)` | `((K,V) -> R) -> [R]` | Map to list |
| `find(pred)` | `((K,V) -> Bool) -> (K,V)?` | First match |
| `any?(pred)`, `all?(pred)` | `((K,V) -> Bool) -> Bool` | |
| `blank?` | `-> Bool` | No entries |

### 23.5 Optional / Result / Either

```
Optional<X> = Just(X) | None        # sugar: X?
Result<X, E> = Ok(X) | Error(E)     # sugar: X or! E
Either<L, R> = Left(L) | Right(R)   # sugar: L or R
```

**Optional:**

| Method | Description |
|---|---|
| `none?` | `True` iff `None` |
| `present?` | `True` iff `Just` (from `Blankable`) |
| `or(default)` | Unwrap or `default` |
| `map(f)` | `Just(x).map(f) = Just(f(x))`; `None` passes through |
| `flatMap(f)` | Chain Optional-returning functions |

**Result:**

| Method | Description |
|---|---|
| `ok?`, `error?` | Variant tests |
| `or(default)` | Unwrap `Ok` or `default` |
| `map(f)` | Apply to `Ok` value; `Error` passes through |
| `flatMap(f)` | Chain Result-returning functions |
| `toOptional` | `Ok(x)` → `Just(x)`; `Error(_)` → `None` |

**Either:** marker type; no methods beyond construction and pattern matching.

### 23.6 Integer / Float / Number

**Integer** (implements `Blankable`, `Monoid`, `Group`):

| Method | Description |
|---|---|
| `even?`, `odd?` | Parity |
| `abs` | Absolute value |
| `sqrt` | Square root (returns `Float`) |
| `modulo(n)` | Mathematical modulo |
| `in?(range)` | Membership in inclusive range |
| `times(f)` | Call `f` with `0..self-1` |
| `floor`, `ceil`, `round` | No-ops on Integer (return `Integer`) |

**Float** (implements `Blankable`, `Truthyable`):

| Method | Description |
|---|---|
| `abs` | Absolute value |
| `sqrt` | Square root |
| `in?(range)` | Membership in inclusive range |
| `floor`, `ceil`, `round` | Nearest `Integer` |
| `toInteger` | Truncate toward zero |

**Module-level parsing** (`Integer`, `Float`, `Number`):

| Function | Returns | Description |
|---|---|---|
| `Integer.parse(s)` | `Result<Integer, ParseError>` | Parse full string |
| `Integer.parsePrefix(s)` | `(Integer, String)?` | Parse prefix, return value + rest |
| `Float.parse(s)` | `Result<Float, ParseError>` | |
| `Float.parsePrefix(s)` | `(Float, String)?` | |
| `Number.parse(s)` | `Result<Number, ParseError>` | Integer or Float |

### 23.7 Math

Constants: `Math.PI`, `Math.E`.

All functions take `Number` (Integer or Float) and return `Float` unless noted.

| Function | Description |
|---|---|
| `sqrt`, `cbrt` | Roots |
| `sin`, `cos`, `tan` | Circular trig (radians) |
| `asin`, `acos`, `atan` | Inverse trig |
| `atan2(y, x)` | Two-arg atan |
| `sinh`, `cosh`, `tanh` | Hyperbolic trig |
| `log(x)`, `log(x, base)` | Natural / base-N logarithm |
| `log2`, `log10` | Base-2 / base-10 log |
| `exp` | `e^x` |
| `pow(x, y)` | `x^y` |
| `abs(x)` | Absolute value (returns `Number`) |
| `floor(x)`, `ceil(x)` | → `Integer` |
| `hypot(x, y)` | `sqrt(x² + y²)` |

### 23.8 IO (`foul module IO`)

| Function | Description |
|---|---|
| `printLine(x)` | Write `x` + newline to stdout |
| `printLine` | Blank line |
| `print(x)` | Write `x` (no newline) |
| `putLine`, `put` | Aliases for `printLine`/`print` |
| `inspect(x)` | Pretty-print to stderr, return `x` |
| `getLine` | Read a line from stdin → `String?` |
| `get` | Read one char from stdin → `String?` |
| `printError(x)` | Write to stderr |
| `warn`, `warning` | Aliases for `printError` |

### 23.9 Range

`a..b` constructs a `Range`. Implements `Enumerable`.

| Method | Description |
|---|---|
| `reduce(init, f)` | Left fold (Enumerable primitive) |
| `sum`, `product` | Aggregate |
| `contains?(x)` | Membership (inclusive) |
| `items` | Materialize as a list |

All Enumerable methods (`map`, `filter`, `each`, etc.) are inherited.

### 23.10 Stream

| Constructor / Method | Description |
|---|---|
| `Stream.Sequence(from, step)` | Infinite stream from `from` |
| `Stream.Iterate(seed, step)` | Alias for `Sequence` |
| `take(n)` | First `n` elements as a list |
| `drop(n)` | New stream offset by `n` |
| `map(f)` | Lazy map |
| `filter(pred)` | Lazy filter |

### 23.11 Console

ANSI styling constants (all become `""` under `--no-colors`): `Reset`, `Bold`,
`Dim`, `Italic`, `Underline`, `Red`, `Green`, `Yellow`, `Blue`, `Magenta`,
`Cyan`, `White`, `Gray`, etc.

| Function | Description |
|---|---|
| `Console.colorize(text, color)` | Wrap text in color + reset |
| `Console.enabled?` | Whether styling is active |

### 23.12 ENV

An immutable `Map<String, String>` snapshot of the process environment. Access
through the global `ENV` namespace is **foul** because it is an implicit input:
the value can change between process launches without appearing in a function's
arguments.

`main(args, env)` receives the same snapshot as an ordinary map. Operations on
that explicit value are pure, so pure helpers should accept `env` as a
parameter.

| Function | Description |
|---|---|
| `ENV.get(key)` | `String?` |
| `ENV.get(key, default)` | `String` |
| `ENV.has?(key)` | `Bool` |
| `ENV.keys`, `ENV.values`, `ENV.entries` | Collections |
| `ENV.count` | Entry count |
| `ENV.each(f)` | Iterate `(k, v)` pairs |

### 23.13 File / Directory (`foul`)

Filesystem operations are available through the `FS.File` and `FS.Directory`
modules. Paths use the `FilePath` type.

**File:**

| Function | Description |
|---|---|
| `FS.File.read(path)` | `String?` — whole file or `None` |
| `FS.File.write(path, content)` | `Bool` — overwrite |
| `FS.File.append(path, content)` | `Bool` — append |
| `FS.File.lines(path)` | `[String]?` — lines |
| `FS.File.feed(path)` | `Stream<String>?` — lazy lines |
| `FS.File.exists?(path)`, `file?`, `directory?` | `Bool` |
| `FS.File.size(path)` | `Integer?` — bytes |
| `FS.File.delete(path)`, `copy(src, dst)`, `rename(src, dst)` | `Bool` |
| `FS.File.open(path, mode)` | Mode-specific `Result<FileHandle<R, W>, FileError>` |
| `FS.File.basename`, `dirname`, `extension`, `join`, `absolute` | Path ops |

`open` refines the handle capabilities from its mode:

| Mode | Result |
|---|---|
| `Read` | `Result<FileHandle<CanRead, CannotWrite>, FileError>` |
| `Write` | `Result<FileHandle<CannotRead, CanWrite>, FileError>` |
| `Append` | `Result<FileHandle<CannotRead, CanWrite>, FileError>` |
| `ReadWrite` | `Result<FileHandle<CanRead, CanWrite>, FileError>` |

Use `.try` inside `trying` to unwrap the result without losing the capability
information:

```kex
trying do
  let file = FS.File.open("notes.txt", Read).try
  let first = file.readLine
  file.close
  first
rescue
  OpenFailed(_) -> None
end
```

Read methods are only available on `FileHandle<CanRead, W>`, and write methods
are only available on `FileHandle<R, CanWrite>`. For example, calling
`readLine` on a handle opened with `Write` is a compile-time error. Capabilities
do not currently track whether a handle has been closed.

**Directory:**

| Function | Description |
|---|---|
| `FS.Directory.create(path)`, `delete(path)`, `deleteAll(path)` | `Bool` |
| `FS.Directory.list(path)`, `files`, `directories` | `[String]?` |
| `FS.Directory.exists?`, `current`, `home` | Existence / paths |

**FileHandle** methods:

- Read-capable: `getLine`, `get`, `readLine`, `read`, `eof?`, `atEnd?`, `feed`.
- Write-capable: `printLine`, `print`, `writeLine`, `write`.
- All handles: `close`.

### 23.14 Http (`foul`)

Types (defined in the prelude):

- `HttpResponse` — fields: `status: Integer`, `body: String`, `headers: Map<String, String>`
- `HttpError` — fields: `kind: NetworkError`, `message: String`
- `HttpOptions` — fields: `headers: Map<String, String> = {}`, `timeout: Integer = 30000`
- `NetworkError = ConnectionRefused | Timeout | DnsError | SslError | NotImplemented | MockEmpty | Unknown`

Each verb (`get`, `post`, `put`, `patch`, `delete`, `head`, `options`) has a
1-arg and a 2-arg (with `HttpOptions`) overload, returning
`Result<HttpResponse, HttpError>`:

```kex
match Http.get("https://example.com") do
  Ok(res)   -> IO.printLine(res.status)
  Error(e)  -> IO.printLine("failed: ${e.message}")
end
```

### 23.15 Web Server

Types (defined in the prelude):

- `Request` — fields: `method`, `path`, `queryString`, `query: Map`, `headers: Map`, `body: String`
- `Response` — fields: `status: Integer = 200`, `headers: Map = {}`, `body: String = ""`

Build a server immutably, then `start`:

```kex
main do
  let server = Web.Server.new(8080)
    .get("/") { |req| Web.Response.text("hello") }
    .get("/api") { |req| Web.Response.json("{\"ok\":true}") }
  match server.start() do
    Ok(_) -> IO.printLine("serving")
    Error(e) -> IO.printLine("failed: ${e}")
  end
end
```

`Web.Response` builders: `text(body)`, `textWithStatus(body, status)`,
`html(body)`, `json(body)`, `redirect(location)`, `notFound`.

Route methods on `Web.Server`: `get`, `post`, `put`, `patch`, `delete`,
`mount(path, handler)` — each returns a new `Server`.

### 23.16 System (`foul`)

| Function | Description |
|---|---|
| `System.exit(code)` | Terminate the process with exit code |

### 23.17 Process / Task / Supervisor (`foul`)

See §18 for the full process API. Summary:

- `Process.self`, `Process.exit(pid, reason)`, `Process.register(pid, name)`,
  `Process.whereis(name)`
- `Pid.send(msg)`, `Pid.link`, `Pid.unlink`, `Pid.monitor`, `Pid.alive?`
- `Reference.demonitor`
- `Task.start { expr }`, `task.await(timeout)`, `Task.awaitAll([tasks])`
- `Supervisor.start(restart: atom) do [worker { spawnFn }] end`

### 23.18 Parser (Introspection)

Runtime access to the Kex parser itself:

| Function | Returns | Description |
|---|---|---|
| `Parser.parse(source)` | `Result<Program, ParseError>` | Parse a program |
| `Parser.parse(source, filename)` | `Result<Program, ParseError>` | With filename |
| `Parser.parseFile(path)` | `Result<Program, ParseError>` | Read + parse |
| `Parser.parseType(source)` | `Result<TypeRef, ParseError>` | Parse a type |
| `Parser.parseExpression(source)` | `Result<Expression, ParseError>` | Parse an expr |

The returned AST types (`TypeRef`, `PatternRef`, `Expression`) model the full
Kex syntax tree as values. See `src/prelude/parser.kex` for the complete ADT.

### 23.19 Evaluator (Sandboxed)

Run Kex source in an isolated evaluator. The options record has fields:

- `allow : [Atom] = [:Math, :List, :String, :Integer, :Map, :Stream]`
- `modules : Map = {}`
- `maxSteps : Integer = 1_000_000`
- `maxDepth : Integer = 256`

| Function | Returns |
|---|---|
| `Evaluator.run(source)` | `Result<Any, String>` |
| `Evaluator.run(source, opts)` | `Result<Any, String>` |
| `Evaluator.runExpression(source)` | `Result<Any, String>` |
| `Evaluator.runExpression(source, opts)` | `Result<Any, String>` |

### 23.20 Kex (Backend / Feature Introspection)

```kex
type Backend = Interpreter | Beam
type Feature = Http | FS | Process | WebServer
```

| Function | Description |
|---|---|
| `Kex.backend` | Active backend (`Interpreter` or `Beam`) |
| `Kex.Feature.has?(f)` | Whether a feature is available on this backend |
| `Kex.Feature.list` | All available features |

### 23.21 Test Doubles (Mock)

| Module | Functions |
|---|---|
| `Mock.FS` | `File(path, content)`, `Directory(path)`, `clear()` |
| `Mock.Http` | `start()`, `respond(status, body)`, `respond(status, body, headers)`, `stop()` |
| `Mock.IO` | `start()`, `stop()`, `output()`, `clear()`, `input(lines)` |

### 23.22 Conversion

The universal `to(value, Type)` method converts between types, returning an
`Optional`:

```kex
42.to(String)                    # Just("42")
"hello".to(String)               # Just("hello")
42.to(String).or("")             # "42"
"123".to(Integer)                # Just(123)
(1..5).to(List)                  # Just([1, 2, 3, 4, 5])
```

---

## 24. Traits (Prelude)

The prelude defines several reusable traits:

### Enumerable

Provides all higher-order traversal methods in terms of a single `reduce`.
Conformances: String, List, Map, Range, Stream.

```
trait Enumerable do
  reduce :> A -> (A -> T -> A) -> A     # required
  # defaults: map, filter, each, find, any?, all?, flatMap, collect, count(pred)
end
```

### Blankable

Rails-style "blankness". Conformances: Bool, Integer, Float, String, Optional,
List, Map.

```
trait Blankable do
  blank? :> Bool          # required
  present? = !this.blank? # default
end
```

### Truthyable

Crystal-style truthiness (`false`, `None`, `()`, `NaN` are falsy).

```
trait Truthyable do
  truthy? :> Bool         # required
  falsy? = !this.truthy?  # default
end
```

### Algebraic

- `Semigroup` — requires `combine :> This -> This`
- `Monoid` — requires `identity :> This`, `combine :> This -> This`
- `Group` — requires `identity :> This`, `combine :> This -> This`, `inverse :> This`

`Integer` implements `Monoid` + `Group`; `String` and `[A]` implement `Monoid`.

### Errorable

```
trait Errorable do
  message :> String
end
```

---

## 25. Backends

Kex targets two backends:

- **Tree-walking interpreter** (default) — runs directly: `kex file.kex`
- **BEAM (Core Erlang)** — compiles to `.core` then `.beam`:
  `kex -c file.kex` (compile) or `kex -R file.kex` (compile + run on BEAM)

Both backends support the full language. The BEAM backend provides real
Erlang-level concurrency, distribution, and all supervisor restart strategies;
the interpreter supports `restart: :only_crashed` only.

### CLI

```
kex file.kex            # run on interpreter (default)
kex -c file.kex         # compile to BEAM (.beam)
kex -R file.kex         # run on BEAM
kex -i                  # interactive REPL
kex -C file.kex         # semantic check only
kex -n file.kex         # skip semantic check
kex -p file.kex         # print AST
kex -l file.kex         # print token stream
kex -e file.kex         # emit Core Erlang (.core)
kex -s file.kex         # print public API signatures
kex --no-colors         # disable ANSI colors
```
