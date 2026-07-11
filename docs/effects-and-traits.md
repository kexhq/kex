# Effects and Traits

## Effect System: `foul`

Kex tracks effects without monads. Functions are either pure or foul.

### Rules

- `let` — pure function. Compiler enforces no effects.
- `foul` — effectful function (replaces `let`, not a modifier on it).
- `main` — implicitly foul.
- Pure functions cannot call foul functions (compile error).
- Foul functions can call pure functions freely.

### Examples

```kex
# Pure
let parse(input: String) -> Config =
  ...

# Foul
foul readConfig(path: String) -> Result<Config, AppError> do
  IO.readFile(path)?.parse()
end

foul startApp do
  Supervisor.start(strategy: :one_for_one) do
    worker(Database)
  end
end

# One-liner
foul printHello = IO.printLine("Hello!")

# Entry point
main do
  IO.printLine("Hello!")
end
```

### `using` is for imports

`using` is a namespace import mechanism (not effects):

```kex
module App do
  using Http do
    let router = Router.Config {}
      .get("/", &handleHome)
  end
end
```

### `IO.inspect` — debug escape hatch

`IO.inspect` is a special IO call that the compiler ignores for purity checking. Pure functions can use it freely.

```kex
let parse(input: String) -> Config =
  let tokens = tokenize(input)
  IO.inspect(tokens)  # doesn't make this foul
  buildConfig(tokens)
```

- Always available, even in pure functions
- Compiler never treats it as a foul call
- Prints to stderr, not stdout
- Fully eliminated (argument included) in release builds — don't put side effects inside `IO.inspect()`
- Compiler flag (`--inspect=keep`) to retain in release for production debugging

### Relationship to processes

Effects are visible in the code through service calls (`IO.readFile`, `Net.fetch`, etc.). The compiler infers which services a foul function uses — no manual capability annotations needed. Tooling (LSP, docs) can display inferred capabilities on hover.

### Foul closures

Closures are never annotated as foul. Instead, the enclosing function's purity determines what's allowed:

```kex
# Fine — enclosing function is foul
foul process(nums: [Int]) -> [Int] do
  nums.map { |n| IO.printLine(n); n * 2 }
end

# Error — enclosing function is pure, closure calls foul
let process(nums: [Int]) -> [Int] =
  nums.map { |n| IO.printLine(n); n * 2 }  # compile error
```

No changes to `map`'s signature needed — it accepts any closure. Purity is checked at the context level, not the closure level.

### Foul trait methods

Traits can declare foul methods:

```kex
trait Serializable for: This do
  foul save : (path: String) -> Result<Unit, Error>
  foul load : (path: String) -> This
end
```

A trait can mix pure and foul methods. Implementors must match the foul marker.

### Granular capabilities

Not in the syntax. The compiler infers capabilities from the body. If granular restrictions are needed later, they can be added as optional annotations without breaking existing code.

---

## Traits

Traits declare type contracts. They use type signatures (`:`) for required methods and `let` for default implementations.

### Syntax

```kex
trait Printable for: This do
  toString : () -> String
end

trait Monad for: F<_> do
  wrap : (value: A) -> F<A>
  flatMap : (f: A -> F<B>) -> F<B>
  let map(f: A -> B) -> F<B> =
    this.flatMap { |x| wrap(f(x)) }
end
```

- `name : type` — required, implementor must provide.
- `let name(...) = ...` — default implementation, can be overridden.

### Implementation via `make`

```kex
make Option implement: Monad do
  let wrap(value: A) -> Option<A> = Just(value)
  let flatMap(f: A -> Option<B>) -> Option<B> =
    match this do
      Just(x) -> f(x)
      None -> None
    end
end

make Result implement: Monad do
  let wrap(value: A) -> Result<A, E> = Ok(value)
  let flatMap(f: A -> Result<B, E>) -> Result<B, E> =
    match this do
      Ok(x) -> f(x)
      Error(e) -> Error(e)
    end
end
```

### `this` and `This`

- `this` — the instance (value-level). Used inside `make` blocks and trait signatures.
- `This` — the implementing type (type-level). Used in trait signatures to refer to the concrete type.

```kex
trait Cloneable for: This do
  clone : (this) -> This
end

make MyRecord implement: Cloneable do
  let clone -> MyRecord = MyRecord { ...this }
end
```

---

## Higher-Kinded Types (HKTs)

Kex supports abstracting over type constructors via `F<_>`.

- `Option` is a type constructor — it has a "hole" (`Option<_>`).
- `Int` is a complete type — no hole, can't be used where `F<_>` is expected.
- `F<_>` in a trait means "this trait applies to things that take a type parameter."

This enables traits like Monad/Functor that are generic over *any* container type.

### Type parameter convention

Single uppercase letters (`A`, `B`, `C`) are type parameters, not concrete types.

---

## Shared State

No global mutable variables. If you need shared mutable state, use a process.

```kex
# Module-level `let` is fine (immutable constant)
let MAX_RETRIES = 3

# Mutable shared state = a process
foul startCounter -> Process<CounterMessage> do
  spawn do
    var state = 0
    loop do
      receive do
        :increment -> state = state + 1
        (:get, sender) -> sender.send(state)
      end
    end
  end
end
```

Module-level `var` does not exist. This avoids concurrency bugs and keeps the process model as the single mechanism for shared state.

---

## Design Rationale

- **No mandatory monads.** The process model handles effects at runtime (isolation, sequencing, failure) — no need to encode them in the type system.
- **`foul` gives signature-level effect information** without monadic ceremony (no lifting, no transformer stacks). Compiler infers granular capabilities.
- **Monads are opt-in.** Available as a trait for those who want parser combinators, validation chains, etc. — but never required.
- **HKTs are opt-in.** Most code won't need `F<_>`. It exists for library authors who want full abstraction.
- **No global mutable state.** Shared state lives in processes — the single concurrency primitive.
