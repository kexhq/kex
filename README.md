# Kex

A functional programming language with Ruby-like syntax, UFCS, immutability by default, and typed processes.

```rb
record User do
  name : String
  age : Int
end

make User do
  let greet({ name }) = "Hi, ${name}!"
  let adult?({ age }) = age >= 18
end

main do
  let user = User { name: "Alice", age: 30 }
  print(user.greet)       # "Hi, Alice!"
  print(user.adult?)      # true
end
```

## Key Features

### UFCS (Uniform Function Call Syntax)

`a.f(b)` is sugar for `f(a, b)`. This gives you method-call ergonomics and IDE code completion while keeping functions composable and free:

```rb
let nums = [5, 3, 1, 4, 2]

nums.sort.reverse.take(3)          # [5, 4, 3]
nums.filter(&even?).map { |x| x * 2 }  # [4, 8, 2]

# These are equivalent:
nums.map { |x| x + 1 }
map(nums) { |x| x + 1 }
```

### Type Dispatch via `make`

Group functions by their receiver type. Same method name, different types — the compiler resolves based on the receiver:

```rb
make Vec2 do
  let add(other: Vec2) -> Vec2 do
    return Vec2 { x: this.x + other.x, y: this.y + other.y }
  end

  let to(String) = "(${this.x}, ${this.y})"
end

make Vec3 do
  let add(other: Vec3) -> Vec3 do
    return Vec3 { x: this.x + other.x, y: this.y + other.y, z: this.z + other.z }
  end

  let to(String) = "(${this.x}, ${this.y}, ${this.z})"
end

# No conflict — dispatches by receiver type
let v2 = Vec2 { x: 1.0, y: 2.0 }.add(Vec2 { x: 3.0, y: 4.0 })
let v3 = Vec3 { x: 1.0, y: 2.0, z: 3.0 }.add(Vec3 { x: 4.0, y: 5.0, z: 6.0 })
```

### Pattern Matching

Multi-clause functions with pattern matching on arguments and `this`:

```rb
let factorial(0) = 1
let factorial(n: Int) = n * factorial(n - 1)

make [A] do
  let first(@[]) = None
  let first(@[x | _]) = x

  let empty?(@[]) = true
  let empty?(@_) = false
end
```

Match expressions with guards:

```rb
let grade = match score do
  n if n >= 90 -> "A"
  n if n >= 80 -> "B"
  n if n >= 70 -> "C"
  _ -> "F"
end
```

### Purity with `foul`

Everything is pure by default. Side effects must be marked:

```rb
# Pure — no IO allowed
let compute(x: Int) = x * 2 + 1

# Foul — does IO
foul let readConfig(path: String) do
  return File.read(path)
end

# Pure can't call foul — compile error:
let bad(path: String) = readConfig(path)  # ERROR
```

`main` is implicitly foul. The compiler enforces the boundary.

### Immutability and `!`

All bindings are immutable by default. `var` opts into local mutation. The `!` operator is sugar for reassignment:

```rb
let x = 5
x = 10          # compile error

var list = [1, 2, 3]
list.push!(4)   # sugar for: list = list.push(4)
list.filter!(&even?)
# list is now [2, 4]
```

The compiler can optimize `!` calls to in-place mutation when it can prove sole ownership.

### Lazy Streams

Infinite lazy sequences with familiar collection methods:

```rb
let naturals = Sequence(from: 0) { |n| n + 1 }
let primes = Sequence(from: 2) { |n| n + 1 }
  .filter(&isPrime?)

primes.take(10)  # [2, 3, 5, 7, 11, 13, 17, 19, 23, 29]

(1..100)
  .filter { |x| x.modulo(3) == 0 }
  .map { |x| x * x }
  .take(5)       # [9, 36, 81, 144, 225]
```

### The `to` Convention

Universal type conversion via pattern matching on the target type:

```rb
make Vec2 do
  let to(String) = "(${this.x}, ${this.y})"
  let to(List) = [this.x, this.y]
end

make Integer do
  let to(String) = ...
  let to(String, base: Integer) = ...
  let to(Float) = ...
end

42.to(String)             # "42"
42.to(String, base: 16)   # "2a"
myVec.to(String)          # "(3.0, 4.0)"
```

### Static Constructors

Records can have named constructors in `static do` blocks:

```rb
record Vector2D do
  x : Float
  y : Float

  static do
    let Polar(length: Float, angle: Float) -> Vector2D do
      return Vector2D { x: length * cos(angle), y: length * sin(angle) }
    end

    let Zero = Vector2D { x: 0.0, y: 0.0 }
    let UnitX = Vector2D { x: 1.0, y: 0.0 }
  end
end

let v = Vector2D.Polar(5.0, angle: Math.PI / 4.0)
let origin = Vector2D.Zero
```

### DSL Support with `Block<[A]>`

When a function's last parameter is `Block<[A]>`, each expression in the block is collected into a list:

```rb
using Html.Language do
  html do
    head do
      title "My Page"
    end
    body do
      h1 "Hello"
      p "Welcome" if showWelcome?
      ...items.map { |i| li(i.name) }
    end
  end
end
```

### Processes (Planned)

Elixir-style typed actors with supervision:

```rb
type CounterMsg = :increment | :reset | (:get, Process<Int>)

foul let counter: Process<CounterMsg> = spawn do
  var state = 0
  loop do
    receive do
      :increment -> state = state + 1
      :reset -> state = 0
      (:get, sender) -> sender.send(state)
    end
  end
end
```

## Building

```
make build       # Build the compiler
make test        # Run unit tests
make spec        # Run spec programs (verify output)
make parse       # Parse all examples (syntax check)
make repl        # Start interactive REPL
make run F=file  # Run a .kex file
make check F=file  # Semantic analysis
```

Requires: CMake 3.20+, C++20 compiler (Clang/GCC).

## Project Structure

```
src/
  lexer/        Tokenizer
  parser/       Recursive descent parser
  ast/          AST node types
  semantic/     Scope resolution, purity checking, type checking
  interpreter/  Tree-walk interpreter
  main.cxx      CLI entry point

examples/       Language showcase (.kex files — tested for parsing)
spec/           Runnable programs with expected output
tests/          C++ unit tests
docs/           Language reference documentation
grammar.ebnf    Formal grammar specification
```

## Compiler Development

The compiler is written in C++20 with no external dependencies beyond the standard library and optional readline.

**Code style:**
- `.hxx` headers, `.cxx` sources
- `camelCase` functions, `m_member` fields
- `auto foo() -> ReturnType` trailing return style
- Namespace: `kex`

**Architecture:**

Source → `Lexer` → tokens → `Parser` → AST → `Analyzer` (scope/purity/types) → `Evaluator` (tree-walk)

The interpreter evaluates directly from the AST. No bytecode or IR yet — the next step is either WASM codegen or a bytecode VM.

**Key design decisions:**
- `std::variant` for AST nodes and runtime values (no inheritance hierarchies)
- Function dispatch via name mangling: `make Vec2 do let add(...) end` registers as `Vec2::add`
- UFCS resolved at call time by checking receiver type → mangled name lookup
- `Block<[A]>` collection semantics determined by the type system, not special syntax
- Purity enforced in a separate semantic pass before evaluation

**Test suite:**
- 6 C++ test suites (lexer, parser, semantic, interpreter, examples, REPL)
- 8 spec programs with expected output verification
- All 20 example files tested for successful parsing

## Status

Working: lexer, parser, semantic analysis (purity + type checking), tree-walk interpreter with stdlib (collections, strings, streams, ranges), REPL with readline support.

Next: WASM codegen, proper namespace resolution (`Stream.Sequence`, `Vector2D.Polar`), generic type inference.

## License

MIT
