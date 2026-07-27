# Sketch: `.try` / `rescue` / `trying`

## Context

Kex handles errors via `Result<T, E>` and `Option<T>` with explicit `match` unwrapping. The JSON parser (`examples/json_parser.kex`) shows the pain — every fallible call needs:
```ruby
let p = match this.expectLiteral(word) do
  Error(e) -> return Error(e)
  Ok(p)    -> p
end
```

## Design

Three constructs + one trait. No exceptions — pure control flow.

### `Tryable` Trait

`.try` works on any type implementing `Tryable<T, E>`. This is the extension point — user-defined types can opt in.

```ruby
trait Tryable<T, E> do
  unwrap :> Result<T, E>
end

# Built-in implementations
make Result<T, E>, implement: Tryable<T, E> do
  let unwrap = this
end

make Option<T>, implement: Tryable<T, None> do
  let unwrap = match this do
    Just(v) -> Ok(v)
    None    -> Error(None)
  end
end
```

User-defined types:
```ruby
type Validation<T, E> = Valid(T) | Invalid([E])

make Validation<T, E>, implement: Tryable<T, [E]> do
  let unwrap = match this do
    Valid(v)    -> Ok(v)
    Invalid(es) -> Error(es)
  end
end

# Now .try works on Validation
let name = validateName(input).try
```

### 1. `.try` — postfix unwrap

Calls `unwrap` on any `Tryable`. On `Ok(v)`, yields `v`. On `Error(e)`, jumps to the nearest `rescue` (in a `trying` block or function-level). If no `rescue` in scope, propagates via `return Error(e)`.

Compile error if the function doesn't return `Result` or `Option`.

```ruby
let parseLiteral(word: String, value: Json) -> (Json, Parser) or! ParseError do
  let p = this.expectLiteral(word).try
  Ok((value, p))
end
```

Chains naturally:
```ruby
let port = config.get("port").chain(~Integer.parse).try
```

### 2. `trying do ... rescue ... end` — scoped recovery

`.try` failures inside the block jump to `rescue` instead of propagating. `trying` without `rescue` is a compile error.

```ruby
let optional(f: Parser -> Result<(a, Parser), ParseError>) -> (a?, Parser) do
  trying do
    let (v, p) = f(this).try
    (Just(v), p)
  rescue
    _ -> (None, this)
  end
end
```

### 3. `rescue` — function-level recovery

Catches `.try` failures from the entire function body. Three sub-forms:

**Pattern matching:**
```ruby
foul setupDatabase(config: Config) -> Result<Connection, AppError> do
  let conn = Connection.open(config.dbUrl).try
  conn.execute("CREATE TABLE IF NOT EXISTS users (...)").try
  conn
rescue
  DbError(msg) -> Error(AppError("db: ${msg}"))
  e -> Error(AppError("unexpected: ${e}"))
end
```

**Pattern matching with guards:**
```ruby
rescue
  DbError(msg) when msg.contains?("timeout") -> retry(config)
  DbError(msg) -> Error(AppError(msg))
  e -> Error(AppError(e))
end
```

**Pattern matching with multi-line bodies:**
```ruby
rescue
  DbError(msg) -> do
    IO.printLine("db failed: ${msg}")
    Error(AppError(msg))
  end
  e -> Error(AppError(e))
end
```

**Catch-all block:**
```ruby
rescue do |e|
  IO.printLine("failed: ${e}")
  Error(AppError(e))
end
end
```

**Inline return** — shorthand when you don't need the error, just a fallback value. Indented with the body, not at block level:
```ruby
let findPort(config: Config) -> Int do
  config.get("port").chain(~Integer.parse).try
  rescue return 8080
end
```

Works in `trying` blocks too:
```ruby
let many(f: Parser -> Result<(a, Parser), ParseError>) -> ([a], Parser) do
  var results: [a] = []
  var p = this
  loop
    trying do
      let (v, p2) = f(p).try
      results.push!(v)
      p = p2
      rescue return (results, p)
    end
  end
end
```

## Desugaring

`.try` calls `unwrap` on the `Tryable` trait, then matches. No exceptions, no stack unwinding.

```ruby
# expr.try
# desugars to:
match expr.unwrap do
  Ok(v)    -> v
  Error(e) -> return Error(e)  # or jump to rescue
end
```

`rescue` wraps the function body in a match on its result:
```ruby
# the compiler transforms:
let foo() -> Result<Int, E> do
  bar().try + 1
rescue
  SomeError(msg) -> Error(Wrapped(msg))
end

# into:
let foo() -> Result<Int, E> do
  match __body() do
    Ok(v) -> Ok(v)
    Error(SomeError(msg)) -> Error(Wrapped(msg))
  end
end
```

`trying` works the same way but scoped to the block.

## Grammar

```ebnf
try_postfix = expr DOT TRY ;

trying_block
  = TRYING DO body RESCUE rescue_body END    (* block rescue *)
  | TRYING DO body rescue_inline END            (* inline rescue *)
  ;

rescue_inline = RESCUE RETURN expr ;

rescue_body
  = match_clause+                                  (* pattern matching *)
  | DO PIPE param PIPE body END                    (* catch-all block *)
  ;

(* rescue at function level replaces the final END *)
function_clause
  = LPAREN params RPAREN DO body RESCUE rescue_body END    (* block rescue *)
  | LPAREN params RPAREN DO body rescue_inline END        (* inline rescue *)
  ;
```

`TRY`, `TRYING`, and `RESCUE` are new keywords.

## Full Example — Expression Parser

```ruby
type ParseError = Unexpected(String, Int) | NoMatch(Int)

type Expr
  = IntLit(Int)
  | FloatLit(Float)
  | Ident(String)
  | BinOp(String, Expr, Expr)
  | Call(String, [Expr])

record Parser do
  input : String
  pos   : Int = 0
end

make Parser do
  let peek   = @pos < @input.count then Just(@input.at(@pos)) else None

  let advance = Parser { input: @input, pos: @pos + 1 }
  let advanceBy(n: Int) = Parser { input: @input, pos: @pos + n }
  let remaining = @input.slice(@pos, @input.count)

  let char(expected: Char) -> (Char, Parser) or! ParseError do
    match this.peek do
      Just(c) when c == expected -> Ok((c, this.advance))
      Just(c) -> Error(Unexpected("${c}", @pos))
      None    -> Error(Unexpected("EOF", @pos))
    end
  end

  let charWhen(pred: Char -> Bool) -> (Char, Parser) or! ParseError do
    match this.peek do
      Just(c) when pred(c) -> Ok((c, this.advance))
      Just(c) -> Error(Unexpected("${c}", @pos))
      None    -> Error(Unexpected("EOF", @pos))
    end
  end

  let keyword(word: String) -> (String, Parser) or! ParseError do
    let p = this.ws
    let slice = p.remaining.slice(0, word.count)
    if slice == word
      Ok((word, p.advanceBy(word.count)))
    else
      Error(Unexpected(slice, p.pos))
    end
  end

  let ws() -> Parser do
    var p = this
    loop
      match p.peek do
        Just(c) when c.whitespace? -> p = p.advance
        _ -> return p
      end
    end
  end

  # --- Combinators ---

  let many(f: Parser -> Result<(a, Parser), ParseError>) -> ([a], Parser) do
    var results: [a] = []
    var p = this
    loop
      trying do
        let (v, p2) = f(p).try
        results.push!(v)
        p = p2
        rescue return (results, p)
      end
    end
  end

  let some(f: Parser -> Result<(a, Parser), ParseError>) -> ([a], Parser) or! ParseError do
    let (first, p) = f(this).try
    let (rest, p) = p.many(f)
    Ok(([first, ...rest], p))
  end

  let sepBy(item: Parser -> Result<(a, Parser), ParseError>,
            sep: Parser -> Result<(Parser, Parser), ParseError>) -> ([a], Parser) do
    trying do
      let (first, p) = item(this).try
      let (rest, p) = p.many { |p| let (_, p) = sep(p).try; item(p) }
      ([first, ...rest], p)
      rescue return ([], this)
    end
  end

  let choice(alts: [Parser -> Result<(a, Parser), ParseError>]) -> (a, Parser) or! ParseError do
    var i = 0
    loop
      break if i >= alts.count
      trying do
        let result = alts.at(i)(this).try
        return Ok(result)
      rescue
        _ -> i = i + 1
      end
    end
    Error(NoMatch(@pos))
  end

  let optional(f: Parser -> Result<(a, Parser), ParseError>) -> (a?, Parser) do
    trying do
      let (v, p) = f(this).try
      (Just(v), p)
      rescue return (None, this)
    end
  end

  # --- Parsers ---

  let float() -> (Expr, Parser) or! ParseError do
    let p = this.ws
    let (int, p) = p.some(~charWhen(&.digit?)).try
    let (_, p) = p.char('.').try
    let (frac, p) = p.some(~charWhen(&.digit?)).try
    let n = Float.parse(int.join("") + "." + frac.join("")).try
    Ok((FloatLit(n), p))
  end

  let integer() -> (Expr, Parser) or! ParseError do
    let p = this.ws
    let (digits, p) = p.some(~charWhen(&.digit?)).try
    let n = Integer.parse(digits.join("")).try
    Ok((IntLit(n), p))
  end

  let ident() -> (Expr, Parser) or! ParseError do
    let p = this.ws
    let (first, p) = p.charWhen(&.lowercase?).try
    let (rest, p) = p.many { |p| p.charWhen { |c| c.letter? || c.digit? || c == '_' } }
    Ok((Ident(([first, ...rest]).join("")), p))
  end

  let primary() -> (Expr, Parser) or! ParseError do
    this.choice([~float, ~integer, ~ident])
  end

  let expression() -> (Expr, Parser) or! ParseError do
    let (left, p) = this.ws.primary().try
    trying do
      let (op, p) = p.ws.choice([
        { |p| p.keyword("+") },
        { |p| p.keyword("-") },
        { |p| p.keyword("*") },
      ]).try
      let (right, p) = p.expression().try
      Ok((BinOp(op, left, right), p))
      rescue return Ok((left, p))
    end
  end
end

main do
  let p = Parser { input: "3.14 + x * 2" }
  let (expr, _) = p.expression().try
  IO.printLine("${expr}")
rescue do |e|
  IO.printLine("Parse error: ${e}")
end
end
```

## Error Type Unification

The compiler infers the union of all error types from `.try` calls in the function body. The function's return type must cover them all:

```ruby
# Return type declares the error union
foul startApp(config: Config) -> Result<App, DbError | HttpError | ConfigError> do
  let conn = Connection.open(url).try           # DbError
  let data = Http.get(endpoint).try             # HttpError
  let port = config.get("port").try             # ConfigError
  App { conn: conn, data: data, port: port }
rescue
  DbError(msg)     -> Error(DbError(msg))
  HttpError(code)  -> Error(HttpError(code))
  ConfigError(msg) -> Error(ConfigError(msg))
end
```

Without `rescue`, the error types propagate — the return type must still cover the union:

```ruby
let loadAndParse(url: String) -> Data or! DbError | HttpError do
  let conn = Connection.open(url).try
  let raw = Http.get(conn.endpoint).try
  parse(raw)
end
```

The compiler checks that `rescue` patterns are exhaustive across the inferred error union. A catch-all `e -> ...` covers everything; without one, all error types from `.try` calls must have a matching clause.

## Resolved Questions

1. **`foul` interaction**: `.try` on a `foul` call requires the caller to be `foul` too — same rule as any other `foul` call
2. **`.try` on Option**: `None` jumps to `rescue` — `rescue` receives `None` (via `Tryable`'s `unwrap` returning `Error(None)`)
3. **Nested `trying`**: Inner `rescue` catches first; unmatched errors bubble to outer `rescue`. `return` always exits the enclosing function, not the `trying` block
4. **Error type unification**: Compiler infers the union of all `.try` error types; return type must cover the union; `rescue` patterns checked for exhaustiveness

## Future: Monadic Generalization

`Tryable` is deliberately shaped to grow into a monad-like system later. The path:

### Step 1 (now): `Tryable`
```ruby
trait Tryable<T, E> do
  unwrap :> Result<T, E>
end
```
`.try` calls `unwrap`. Covers error propagation.

### Step 2 (later): `Bindable`
```ruby
trait Bindable<T> do
  chain : (T -> Bindable<U>) -> Bindable<U>
  wrap  : T -> Bindable<T>
end
```
The monad interface. Types like `Result`, `Option`, `List`, `Future`, `Parser<T>` can implement it for different composition patterns.

### Step 3 (later): `within` blocks
Syntactic sugar for `chain` — operates within a monadic context:

```ruby
within Parser do
  let _ <- keyword("if")
  let cond <- expression()
  let _ <- keyword("end")
  wrap(IfExpr(cond))
end

# Desugars to:
keyword("if").chain { |_|
  expression().chain { |cond|
    keyword("end").chain { |_|
      wrap(IfExpr(cond))
    }
  }
}
```

Works with any `Bindable`:
```ruby
# List comprehension
within List do
  let x <- [1, 2, 3]
  let y <- [10, 20]
  x + y
end
# => [11, 21, 12, 22, 13, 23]

# Result chaining
within Result do
  let port <- config.get("port")
  let n <- Integer.parse(port)
  n
end
```

### Design Decisions

- **`Tryable` and `Bindable` are independent traits** — types implement whichever they need
- `Result`, `Option` implement both — `.try` for error propagation, `within` for monadic composition
- `List` implements only `Bindable` — no error concept, no `.try`
- Simple types like `Validation` implement only `Tryable` — no need for `chain`/`wrap`
- `.try` + `rescue`/`trying` is for error handling; `within` is for general monadic composition
- They overlap on `Result` — that's fine, use whichever reads better for the situation
- Explicit parser state threading with `.try` works now; `within Parser` with hidden state comes later via `Bindable`
