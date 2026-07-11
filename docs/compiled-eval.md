# Compile-Time Expression Evaluation

## Summary

Modules can define methods inside `compiled do...end` blocks, marking them as compile-time evaluable. When user code calls `.emit()` (or `.compile()`) on a method chain where that method was defined inside a `compiled` block, the compiler evaluates the entire chain at compile time. The result is spliced into the program as a literal value. Free variables from the enclosing runtime scope are auto-detected and become parameterized placeholders.

## Mechanism

### Authoring (library side)

A module defines its builder methods inside a `compiled do...end` block:

```kex
module SQL do
  record QueryBuilder do
    fields     : [Symbol] = []
    table      : Symbol = :none
    conditions : [(String, Any)] = []
  end

  record Query do
    text   : String
    params : [Any] = []
  end

  compiled do
    let select(fields) -> QueryBuilder do
      QueryBuilder { fields: fields }
    end

    make QueryBuilder do
      let from(table: Symbol) -> QueryBuilder do
        QueryBuilder { fields: @fields, table: table }
      end

      let where(conditions) -> QueryBuilder do
        QueryBuilder { fields: @fields, table: @table, conditions: @conditions + conditions }
      end

      let emit() -> Query do
        # ... build SQL string from accumulated state ...
        Query { text: sql, params: params }
      end
    end
  end
end
```

The `compiled` block is the only marker needed. No special annotations, no new keywords.

### Consumption (user side)

User code looks completely normal:

```kex
let q = SQL.select(:all).from(:users).where(id: eq(userId)).emit()
```

This is a regular method chain. The compiler recognizes that `.emit()` was defined in a `compiled` block and triggers compile-time evaluation.

### What the compiler produces

At compile time, the chain evaluates to:

```kex
Query { text: "SELECT * FROM users WHERE id = $1", params: [userId] }
```

At runtime, `q` is just a record literal — no builder objects, no method dispatch, no allocations beyond the final result.

## Free Variable Detection

When evaluating a compile-time chain, the compiler distinguishes:

- **Compile-time values**: literals (`:all`, `:users`, `"John"`, `10`), atoms, other constants
- **Free variables**: identifiers from the enclosing runtime scope that don't exist in the compile-time environment

Free variables are automatically treated as parameterized placeholders. The compile-time evaluator tracks them and emits them in the result's params list (or wherever the `.emit()` implementation places them).

```kex
let userId = get_user_id()  # runtime value

let q = SQL.select(:all).from(:users).where(id: eq(userId)).emit()
#                                                  ^^^^^^
#                     userId is not a compile-time constant → becomes $1
```

The compiler sees that `userId` isn't available at compile time, so it:
1. Assigns it placeholder index `$1`
2. Evaluates the chain with the placeholder flowing through
3. The `.emit()` body builds `"... WHERE id = $1"` and puts `userId` in the params list
4. At runtime, the original `userId` identifier is referenced in the params array

## Trigger Convention

`.emit()` is not a language keyword — it's a naming convention. The actual rule is:

> If a method was defined inside a `compiled do...end` block, and the entire chain from the root can be evaluated at compile time (modulo free variables), then calling that method triggers compile-time evaluation.

In practice, DSL authors use `.emit()` for query-like builders and `.compile()` for things that produce functions (like routers). The compiler doesn't distinguish — any method in the compiled block can be the trigger.

### Partial chains stay runtime

Without calling a compiled-block method as the terminal:

```kex
let builder = SQL.select(:all).from(:users)
# ^ This is a normal runtime value (QueryBuilder record).
# No compile-time evaluation happens because the chain
# doesn't terminate with a compiled-block method that
# produces a final result.
```

Wait — actually all the methods (`select`, `from`, `where`) are also in the compiled block. The distinction is:

> Compile-time evaluation is triggered when the **entire chain is evaluable** — all arguments are either compile-time constants or free variables (which become placeholders). If the chain is used as an intermediate value that gets further method calls with dynamic arguments, it stays runtime.

The practical rule: a complete expression statement or binding like `let q = ...chain....emit()` where the chain terminates, triggers evaluation. An intermediate variable that gets more calls later doesn't.

## Routing Example

For frameworks that generate dispatch logic:

```kex
module Router do
  record Route do
    method  : Symbol
    path    : String
    handler : (Request) -> Response
  end

  record RouteTable do
    routes : [Route] = []
  end

  compiled do
    let new() -> RouteTable = RouteTable {}

    make RouteTable do
      let get(path: String, handler: (Request) -> Response) -> RouteTable do
        RouteTable { routes: @routes ++ [Route { method: :get, path: path, handler: handler }] }
      end

      let post(path: String, handler: (Request) -> Response) -> RouteTable do
        RouteTable { routes: @routes ++ [Route { method: :post, path: path, handler: handler }] }
      end

      let compile() -> (Request) -> Response do
        # Analyze all routes at compile time.
        # Generate an optimized dispatch function:
        # - static paths → direct pattern match
        # - parameterized paths (/users/:id) → segment extraction
        # - overlapping prefixes → nested match (trie-like)
      end
    end
  end
end
```

Usage:

```kex
let dispatch = Router.new()
  .get("/", &index)
  .get("/users", &listUsers)
  .get("/users/:id", &getUser)
  .post("/users", &createUser)
  .compile()
```

At compile time, the router knows all routes statically. `.compile()` produces a multi-clause function with BEAM-optimal pattern matching — no route table traversal at runtime.

## Constraints

### Purity

Code inside `compiled do...end` must be pure (no IO, no process spawning, no mutable global state). The only permitted side effect is `Env.get()` for reading environment variables at compile time.

### Totality

The compile-time evaluator must terminate. Infinite loops or unbounded recursion at compile time are a compile error (with a configurable step limit).

### Type safety

The result of `.emit()` must have a concrete type known at compile time. The type checker validates the spliced result against the expected type at the call site.

### Error reporting

If compile-time evaluation fails (type mismatch, assertion, step limit), the error points back to the original call site in user code, not into the framework's compiled block internals (unless `--verbose` is passed).

## Relationship to Existing `compiled` Features

This extends the existing `compiled do...end` mechanism which currently supports:

- **Compile-time constants**: `MAX_RETRIES = 3`
- **Declaration generation**: `TYPES.each do |name| let %name?(...) = ... end`
- **Type generation**: `type %name = ...`

The new capability adds:

- **Module method definitions**: methods defined in `compiled` are compile-time evaluable
- **Expression evaluation**: `.emit()` triggers collapse of a complete chain
- **Free variable detection**: runtime values flow through as placeholders

All three features share the same `compiled do...end` syntax and the same compile-time evaluator.

## See Also

- `examples/compiled_sql.kex` — full SQL builder example
- `examples/compiled_router.kex` — full routing framework example
- `examples/compiled.kex` — declaration generation examples (existing)
