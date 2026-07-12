# Kex

[![CI](https://github.com/kexhq/kex/actions/workflows/ci.yml/badge.svg)](https://github.com/kexhq/kex/actions/workflows/ci.yml)

<p align="center">
<img src="./docs/assets/logo.png" height="340" style="align: center" />
</p>

Kex is a small functional programming language with Ruby-like syntax, immutable data by default, UFCS method chains, type-directed `make` blocks, pattern matching, and explicit side-effect boundaries.

It is designed for code that reads like a scripting language without giving up typed records, sum types, pure functions, and predictable dispatch.

```rb
record Vector2D do
  x : Float
  y : Float
end

make Vector2D do
  let +(other: This) -> This do
    return Vector2D { x: @x + other.x, y: @y + other.y }
  end

  let *(factor: Float) -> This do
    return Vector2D { x: @x * factor, y: @y * factor }
  end

  let near?(other: This) -> Bool do
    (@x - other.x).abs < 0.01 && (@y - other.y).abs < 0.01
  end

  let to(String) -> String do
    "(${@x}, ${@y})"
  end
end

main do
  let position = Vector2D { x: 3.0, y: 4.0 }
  let velocity = Vector2D { x: 1.0, y: -0.5 }

  let arrived = position + velocity * 2.0
  IO.printLine("next position: ${arrived.to(String).or("")}")
  IO.printLine("arrived? ${arrived.near?(Vector2D { x: 5.0, y: 3.0 })}")
end
```

## Why Kex

Kex tries to make the common path feel light:

- Functions compose through method syntax with UFCS: `items.filter(...).map(...).take(10)`.
- Domain behavior lives near the type through `make`, without classes or inheritance-heavy hierarchies. `@field` is shorthand for `this.field` inside those blocks, and operators (`+`, `==`, ...) can be overloaded the same way.
- Pattern matching works in function clauses, `match` expressions, and receiver patterns.
- Effects are visible: functions are pure unless marked `foul`, and `main` is the effect boundary.
- Records, sum types, optional values, result values, ranges, streams, maps, and lists are built into the language model.

## Language Tour

## The Good Parts In Code

### Pipelines Over Real Data

Uniform Function Call Syntax means `value.f(arg)` is equivalent to `f(value, arg)`. You get fluent pipelines while keeping functions as plain functions.

```rb
let suspicious = requests
  .filter { |req| req.path.startsWith?("/admin") }
  .reject(&.authenticated?)
  .map { |req| SecurityEvent.from(req) }
  .take(20)

# Equivalent forms; pick whichever reads better locally.
requests.map(&normalize)
map(requests, &normalize)
```

### Type-Directed Behavior Without Classes

`make` blocks attach behavior to a type. The same method name — even an operator — can exist for different receiver types; dispatch is based on the receiver. Inside a `make` block, `@field` is shorthand for `this.field`.

```rb
record Vector2D do
  x : Float
  y : Float

  static do
    let Zero = Vector2D { x: 0.0, y: 0.0 }
    let UnitX = Vector2D { x: 1.0, y: 0.0 }
  end
end

record Vector3D do
  x : Float
  y : Float
  z : Float
end

make Vector2D do
  let +(other: This) -> This do
    return Vector2D { x: @x + other.x, y: @y + other.y }
  end

  let *(factor: Float) -> This do
    return Vector2D { x: @x * factor, y: @y * factor }
  end

  let dot(other: This) -> Float do
    return @x * other.x + @y * other.y
  end

  let to(String) -> String do
    return "(${@x}, ${@y})"
  end
end

make Vector3D do
  let +(other: This) -> This do
    return Vector3D { x: @x + other.x, y: @y + other.y, z: @z + other.z }
  end

  let to(String) -> String? do
    return Just("(${@x}, ${@y}, ${@z})")
  end
end

let unitX = Vector2D { x: 1.0, y: 0.0 }
let shot = (unitX * 2.0 + Vector2D { x: 0.0, y: 5.0 }).to(String).or("")
# "(2.0, 5.0)" — `*` resolves through Vector2D::*, `+` through Vector2D::+
```

### Pattern Matching As Control Flow

Kex supports multi-clause functions, `match` expressions, destructuring, and `@pattern` to match on the receiver itself (separate from the `@field` shorthand shown above).

```rb
let fizzBuzz(n: Int) -> String do
  match (n.modulo(3), n.modulo(5)) do
    (0, 0) -> "FizzBuzz"
    (0, _) -> "Fizz"
    (_, 0) -> "Buzz"
    (_, _) -> n.to(String).or("")
  end
end

main do
  (1..100)
    .map(&fizzBuzz)
    .each { |s| IO.printLine(s) }
end
```

### Records, Sum Types, Optional, Result

Data modeling is direct: records for product types, `type` for unions, `?` for optional values, and `Result` for fallible flows.

```rb
type ParseError = InvalidFormat(String) | Overflow | EmptyInput

let parseInt(s: String) -> Result<Int, ParseError> do
  return Error(EmptyInput) if s.empty?
  match Integer.parse(s) do
    Ok(n)    -> Ok(n)
    Error(_) -> Error(InvalidFormat(s))
  end
end

let parsePort(s: String) -> Result<Int, ParseError> do
  let n = parseInt(s)?
  return Error(Overflow) if n > 65535
  return Ok(n)
end

foul loadConfig(path: String) -> Result<Config, AppError> do
  let content = IO.readFile(path)?
  let parsed = Config.parse(content)?
  let port = parsePort(parsed.get("port"))?
  return Ok(Config { port: port, host: parsed.get("host") })
end
```

### Purity Is the Default

Functions are pure unless marked `foul`. Pure code cannot call foul code, so side effects stay visible in the type of the program.

```rb
let compute(x: Int) = x * 2 + 1

foul readConfig(path: String) do
  return File.read(path)
end

# Compile error: pure code cannot call foul code.
let bad(path: String) = readConfig(path)
```

`main` is implicitly foul, so programs still have a natural place for IO.

### Local Mutation With `var` and `!`

Bindings are immutable by default. `var` opts into local mutation, and `!` is reassignment sugar for methods that return an updated value — it rebinds the variable rather than mutating the underlying value in place, so aliases never see the change.

```rb
let frozen = [1, 2, 3]
# frozen.push!(4)
# runtime error: Cannot use '!' on immutable binding: frozen

var list = [1, 2, 3, 4, 5]
list.push!(6)
list.filter!(&.even?)
list.map! { |x| x * 10 }
# list is now [20, 40, 60]
```

### Lazy Streams and Ranges

Ranges and streams share collection-style operations. Infinite streams stay lazy until consumed.

```rb
let naturals = Stream.Sequence(from: 0) { |n| n + 1 }

let primes = Stream.Sequence(from: 2) { |n| n + 1 }
  .filter do |n|
    (2..n - 1).all? { |d| n.modulo(d) != 0 }
  end

let firstTenPrimes = primes.take(10)

let squaresOfMultiplesOfThree = (1..100)
  .filter { |x| x.modulo(3) == 0 }
  .map { |x| x * x }
  .take(5)
```

### Currying and Partial Application

`~` creates a partially applied function. Bound arguments fill left-to-right; `_` marks an explicit open slot for a specific position. Chained `(args)` groups that fully saturate the function call it immediately — no extra wrapping.

```rb
let add(a, b) = a + b
let multiply(a, b) = a * b

let inc = ~add(1)
let double = ~multiply(2)

main do
  let multipled = [1, 2, 3].map(~multiply(10))
  IO.printLine(multipled)

  let summed = (1..100).reduce(0, ~(+))
  IO.printLine(summed)
end
```

### Traits

Traits declare required methods and can provide default implementations. Implementing types get the defaults for free.

```rb
trait Shape do
  area      : () -> Float
  perimeter : () -> Float

  let describe = "area=${this.area}, perimeter=${this.perimeter}"
end

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
end

make Rectangle, implement: Shape do
  let area      = @width * @height
  let perimeter = 2.0 * (@width + @height)
  let describe  = "Rectangle ${@width}x${@height} (area=${this.area})"  # override default
end

let shapes = [
  Circle { radius: 5.0 },
  Rectangle { width: 4.0, height: 6.0 },
]

shapes.each { |s| IO.printLine(s.describe) }
# area=78.53981633974483, perimeter=31.41592653589793
# Rectangle 4.0x6.0 (area=24.0)
```

### DSL-Friendly Builders

Kex is intended to make embedded DSLs feel native. Block arguments and `do |x| ... end` syntax let library code read like built-in language constructs.

```rb
let app = Http.routes do
  get "/" do |req|
    Response.ok("Welcome")
  end

  get "/users/:id" do |req|
    match UserService.find(req.params.id) do
      Just(user) -> Response.json(user)
      None -> Response.notFound("user not found")
    end
  end

  post "/users" do |req|
    let user = UserService.create(req.body)
    return Response.created(user) if user.ok?

    return Response.error("error while creating user")
  end
end
```

### Static Constructors and Constants

Records can define type-level constructors and constants in `static do` blocks. Constructors are capitalized `let`s that build an instance; constants are capitalized `let`s that just hold a value.

```rb
record Temperature do
  celsius : Float

  static do
    let Celsius(value: Float) -> Temperature do
      return Temperature { celsius: value }
    end

    let Fahrenheit(value: Float) -> Temperature do
      return Temperature { celsius: (value - 32.0) * 5.0 / 9.0 }
    end

    let Freezing = Temperature { celsius: 0.0 }
  end
end

make Temperature do
  let to(String) -> String? do
    return Just("${@celsius}C")
  end
end

let boiling = Temperature.Fahrenheit(212.0).to(String).or("")  # "100.0C"
let zero = Temperature.Freezing.to(String).or("")              # "0.0C"
```

## Try It

Build the compiler:

```sh
make build
```

Run a file:

```sh
make run F=examples/fizzbuzz.kex
build/kex examples/vectors_advanced.kex
```

Start the REPL:

```sh
make repl
```

Install the binary somewhere on your `PATH`:

```sh
make build
sudo make install
```

By default, `make install` copies the existing `build/kex` to `/usr/local/bin/kex`. It intentionally does not build as root. To install elsewhere:

```sh
make install PREFIX=$HOME/.local
```

## Commands

```sh
make build          # Build the compiler
make test           # Run C++ unit tests
make spec           # Run executable language specs
make parse          # Parse all examples
make repl           # Start the interactive REPL
make run F=file     # Run a .kex file (type-checks first; use --no-check to skip)
make check F=file         # Run semantic analysis only
make check-prelude        # Type-check all src/prelude/*.kex files
make install              # Install build/kex to /usr/local/bin/kex
make uninstall      # Remove the installed binary
make clean          # Remove build artifacts
```

Requires CMake 3.20+ and a C++20 compiler. Readline is optional.

## Examples

Good starting points:

- `examples/basics.kex` - core syntax
- `examples/traits.kex` - Shape trait, required/default/overridden methods, multiple traits
- `examples/vectors_advanced.kex` - records, static constructors, UFCS methods
- `examples/streams.kex` - lazy ranges and streams
- `examples/html_dsl.kex` - DSL-oriented blocks
- `examples/error_handling.kex` - `Result`, optional values, and `?`
- `spec/type_dispatch.kex` - dispatch through `make` and receiver patterns
- `spec/operator_overloading.kex` - overloading `+`, `*`, `==` per receiver type
- `spec/at_field_shorthand.kex` - `@field`/`@method(...)` inside `make` blocks
- `spec/mutating_calls.kex` - `var`/`let` and `!` mutation semantics
- `spec/static_namespacing.kex` - static constructors/constants stay namespaced under their type
- `spec/math.kex` - the `Math` module (`Math.PI`, `Math.sqrt`, trig, logs, ...)
- `spec/testing_dsl.kex` - the `describe`/`it`/`assert` testing DSL itself
- `spec/json_parser.spec.kex` - a spec for `examples/json_parser.kex` using `describe`/`it`/`assert` (see "Specs for example files" in `docs/testing.md`)

## Project Structure

```text
src/
  lexer/        Tokenizer
  parser/       Recursive descent parser
  ast/          AST node types
  semantic/     Scope resolution, purity checking, type checking
  interpreter/  Tree-walk interpreter
  prelude/      Kex-language specs for the built-in prelude (documented with RDoc-style comments)
  main.cxx      CLI entry point

examples/       Language showcase files
spec/           Runnable programs with expected output
tests/          C++ unit tests
docs/           Language reference documentation
grammar.ebnf    Formal grammar specification
```

## Compiler Development

The compiler is written in C++20 with no external dependencies beyond the standard library and optional readline.

Source flows through:

```text
Lexer -> Parser -> AST -> Analyzer -> Evaluator
```

The current evaluator is a tree-walk interpreter. There is no bytecode or IR yet.

Key implementation choices:

- AST nodes and runtime values use `std::variant`.
- `make Vec2 do let add(...) end` registers behavior as `Vec2::add`.
- UFCS resolves by checking the receiver type and looking up the mangled method name.
- Purity is enforced in a semantic pass before evaluation.

## Status

Working today:

- Lexer, parser, AST, semantic analysis, and tree-walk evaluation
- Records, sum types, functions, lambdas, pattern matching, destructuring
- Lists, maps, ranges, streams, strings, chars, numbers, optional values, result values
- UFCS, `make` dispatch, `to` conversion convention, operator overloading
- `@field`/`@method(...)` shorthand for `this` inside `make` blocks
- `foul` purity boundaries, and local `var` mutation enforced at runtime
- Traits: `trait ... do end`, `make X implement: Trait do end`, `Comparable`, `Equatable`
- Currying and partial application with `~func(args)`, `~(op)`, and `_` placeholders
- Type checker active by default (`kex run` gates on type checking; `--no-check` to skip)
- Stdlib type signatures, trait-constrained overload resolution, lambda type inference
- Arbitrary-precision `Integer` (GMP-backed), numeric tower, match exhaustiveness checking
- Rich stdlib: `uniq`, `partition`, `indexOf`, `flatMap`, `contains?`, `min`/`max` returning `Optional`, char predicates (`digit?`, `alpha?`, `space?`), and more
- REPL with optional readline support

Planned or incomplete:

- Bytecode VM or WASM codegen
- Full process/actor runtime
- `is?` / `as` type introspection and explicit conversion
- Namespace/import resolution beyond `using` blocks
- Compiled metaprogramming beyond the parser/design sketch

## License

MIT
