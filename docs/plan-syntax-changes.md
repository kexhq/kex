# Plan: Language Syntax Changes

## Context

We're making six syntax changes to Kex based on design decisions from the effects/traits discussion:

1. **`foul` replaces `let` for effectful functions** — currently `foul let name(...)`, should become `foul name(...)`. `foul` becomes a standalone function keyword like `let`.
2. **Remove `do` from `if`/`elif`/`else`/`while`/`loop`** — currently `if cond do ... end`, should become `if cond\n body\nend`. Multiline conditions use parens.
3. **`when` replaces `if` in match guards** — currently `n if n > 1 ->`, should become `n when n > 1 ->`.
4. **`then`/`else` inline conditional expression** — ternary replacement: `cond then a else b`.
5. **Optional parens before `do` blocks** — `describe "test" do ... end` instead of requiring `describe("test") do ... end`.
6. **Drop `and`/`or`/`not`** — fully unreserved, use `&&`/`||`/`!` exclusively.

## Conditional constructs (three distinct forms)

```kex
# 1. Trailing if — statement guards / early returns only
return None if x > 0
return Error(:bad) if !valid?

# 2. Inline conditional expression (ternary replacement)
let z = x > 0 then x else y
let label = items.empty? then "none" else "${items.length} items"

# 3. Multiline if — no do keyword, parens for multiline conditions
if x > 0
  doSomething()
elif x == 0
  doOther()
else
  fallback()
end

# Multiline condition with parens
if (x > 0
    && y < 10
    && z != nil)
  doSomething()
end
```

No overlap, no ambiguity between the three forms.

### `then`/`else` rules
- Both `then` and `else` are required (always returns a value, like `?:`)
- Lowest expression precedence (like C's `?:`)
- Nesting requires parens: `x > 0 then (y > 0 then a else b) else c`
- Must be on the same line (unless the whole expression is parenthesized)
- `else` at line start always belongs to `if` — never to `then`

## `do` usage philosophy

`do ... end` means "passing a block" — it's what makes user functions extensible like language constructs:

```kex
# Built-in
receive do
  :msg -> handle()
end

# User-defined — same shape
describe "my test" do
  it "works" do
    assert(true)
  end
end

transaction(db) do
  db.insert(record)
end
```

**Keeps `do`:** `receive`, `match`, function bodies (`let`/`foul`), `module`, `make`, block arguments
**Drops `do`:** `if`, `elif`, `else`, `while`, `loop` (control flow — no block passing)

## Changes

### 1. `foul` as standalone function keyword

**Files:** `src/parser/parser.cxx`, `grammar.ebnf`, examples

**Parser changes (`src/parser/parser.cxx`):**
- Line 96-98: Change from checking `Foul + Let` to just `Foul` followed by an identifier. Call `parseFunctionDef(true)` without expecting `Let`.
- Line 156-158: Same change in module body parsing.
- Also handle `foul` inside `private do ... end` and `public do ... end` visibility blocks.
- `parseFunctionDef()`: When `isFoul=true`, skip the `Let` expectation — `foul` was already consumed and next token is the function name.
- Handle `foul` + operator overloads in `make` blocks (e.g. `foul +(other) do ... end`).

**Grammar (`grammar.ebnf`):**
- Line 109-112: Change `FOUL? LET` to `(FOUL | LET)` — either keyword introduces a function.

**Examples:** Update all files containing `foul let` to just `foul`.

### 2. Remove `do` from `if`/`elif`/`else`/`while`/`loop`

**Files:** `src/parser/parser.cxx`, `grammar.ebnf`, all examples

**Parser changes (`src/parser/parser.cxx`):**
- `parseIfExpr`: Remove `expect(TokenType::Do, ...)` after if/elif/else. After parsing the condition, just `skipNewlines()` — newline is the delimiter.
- `parseWhileExpr` (if exists): Same — remove `do` expectation after condition.
- `parseLoopExpr` (if exists): Remove `do` after `loop` keyword.
- The `m_noDoBlocks` flag may no longer be needed for these constructs. Still need to prevent the condition parser from consuming `elif`/`else`/`end` tokens — may need flag or stop-tokens.
- Multiline conditions require parens: `if (cond1\n && cond2)\n body\nend`
- Trailing `if` remains unchanged (already doesn't use `do`).

**Grammar (`grammar.ebnf`):**
```
if_expr    = IF expr NEWLINE body (ELIF expr NEWLINE body)* (ELSE NEWLINE body)? END
while_expr = WHILE expr NEWLINE body END
loop_expr  = LOOP NEWLINE body END
```

**Examples:** All files with `if ... do`, `elif ... do`, `else do`, `while ... do`, `loop do` — many occurrences across ~15+ files.

### 3. `when` for match guards + match subject binding

**Files:** `src/lexer/lexer.cxx`, `src/lexer/token.hxx`, `src/parser/parser.cxx`, `src/ast/ast.hxx`, `grammar.ebnf`, examples

**Lexer:** ✅ Done — `When` added to `TokenType` enum, keyword map, and `tokenTypeName`.

**Parser (`src/parser/parser.cxx`):**
- Line 1414: Change `match(TokenType::If)` to `match(TokenType::When)` in `parseMatchClause()`.
- Add optional `|name|` binding after `do` in `parseMatchExpr()` — same syntax as block params.
- Support bare `when cond -> ...` clauses (no pattern) — desugar to `_ when cond -> ...`.

**AST (`src/ast/ast.hxx`):**
- Add optional `subjectBinding` field to `MatchExpr`:
  ```cpp
  struct MatchExpr {
      ExprPtr subject;
      std::optional<std::string> subjectBinding; // |n|
      std::vector<MatchClause> clauses;
  };
  ```

**Grammar:**
```
match_expr   = MATCH expr DO (PIPE LOWER_IDENT PIPE)? match_clauses END
match_clause = pattern (WHEN expr)? ARROW expr
             | WHEN expr ARROW expr   (* bare when = _ when *)
```

**Examples:**

```kex
# Without binding — just patterns
match result do
  Ok(val) -> use(val)
  Error(e) -> handle(e)
end

# With binding — enables guards on the whole subject
match user.age do |n|
  100 -> "wow"
  when n >= 65 -> "senior"
  when n >= 18 -> "adult"
  _ -> "minor"
end

# Patterns + guards combined
match response do |r|
  Ok(val) when val > 0 -> use(val)
  Error(e) when r.retryable? -> retry()
  Error(e) -> fail(e)
end
```

**Rules:**
- `|n|` is optional — binds the match subject for use in guards
- `when cond -> ...` without a pattern is sugar for `_ when cond -> ...`
- Patterns and guards can be combined: `pattern when guard -> ...`
- `|n|` binds the fully evaluated expression (after `?` unwrapping etc.)
- Same `|name|` pattern works in `receive do |msg| ... end` — consistent with closures
- In all contexts, `do |name|` means "bind whatever arrives" — closures, match, receive

**Design note:** `|n|` in destructuring clauses (e.g. `Ok(val) when r.status == 200`) means `|r|` is the whole subject and `val` is destructured from the pattern. Two scopes of binding in one clause — the subject binding is the "outer" name, pattern variables are "inner" names.

**Examples to update:** All match guards from `if` to `when`, update examples using `n if n > x` pattern to use `|n|` binding style.

### 4. `then`/`else` inline conditional expression

**Files:** `src/parser/parser.cxx`, `src/ast/ast.hxx`, `grammar.ebnf`

**Lexer:** ✅ Done — `Then` added to `TokenType` enum, keyword map, and `tokenTypeName`.

**AST (`src/ast/ast.hxx`):**
- Add `ThenElseExpr` variant:
  ```cpp
  struct ThenElseExpr {
      ExprPtr condition;
      ExprPtr thenExpr;
      ExprPtr elseExpr;
  };
  ```

**Parser:**
- Parse at lowest expression precedence: after parsing a full expression, if the next token is `Then`, consume it, parse the then-expression (stopping at `Else`), expect `Else`, parse the else-expression.
- No nesting without parens — inside a `then`/`else`, don't look for another `then` unless parenthesized.

**Grammar:**
```
then_else_expr = expr THEN expr ELSE expr
```

**Examples:** New construct — add usage to `basics.kex` or `stdlib_demo.kex`.

### 5. Optional parens before `do` blocks

**Rule:** When a function call is immediately followed by a `do` block, parentheses around arguments are optional. Everything between the function name and `do` is comma-separated arguments.

```kex
# With parens (always valid)
describe("my test") do ... end
make Option, implement: Monad do ... end

# Without parens (valid because do follows)
describe "my test" do ... end
make Option, implement: Monad do ... end
worker Database, args: [config.port] do ... end

# Without do block — parens required
let result = describe("my test")
```

Consistent with existing `make Something, implement: Other do ... end` pattern.

**Parsing rule:** First `do` at bracket depth 0 terminates the arguments and starts the trailing block. If ambiguous, use parens.

**Inline closures use `{ }`, not `do...end`:** `do...end` is always multiline/block-level. Inline closures use braces. This means a `do` inside arguments basically can't happen in idiomatic Kex.

```kex
# ✅ Inline closure — braces
items.map { |x| x + 1 }

# ✅ Block — do...end
describe "test" do
  ...
end

# ❌ Never — do...end inline
items.map do |x| x + 1 end
```

**Parser:** When parsing a call expression at statement position, if after consuming the function name the next token is NOT `(`, check if we can parse arguments terminated by `do`. If `do` is found, treat everything between name and `do` as comma-separated arguments.

**Grammar:**
```
call_with_block = IDENT arg_list? DO body END
               | IDENT bare_args DO body END
bare_args      = expr (COMMA expr)*
```

### 6. Drop `and`/`or`/`not` — use `&&`/`||`/`!` only

**Rule:** `and`, `or`, `not` are fully removed as keywords. They become regular identifiers (valid for function/variable names). All logic uses `&&`, `||`, `!`.

**Files:** `src/parser/parser.cxx`, `grammar.ebnf`, examples

**Lexer:** Remove `and`, `or`, `not` from the keywords map — they'll lex as `LowerIdent` instead of keyword tokens.

**Token enum:** Remove `And`, `Or`, `Not` from `TokenType` (and `tokenTypeName`).

**Parser:** Remove any code paths that handle `And`/`Or`/`Not` tokens.

**Examples:** Replace all `and` → `&&`, `or` → `||`, `not` → `!` in example files.

## Example: all new syntax in action

```kex
module UserService do
  record User do
    name : String
    age : Int
    email : String?
  end

  # Pure function — let
  let adult?(user: User) -> Bool = user.age >= 18

  # Foul function — standalone foul keyword
  foul fetchUser(id: Int) -> Result<User, AppError> do
    let response = Http.get("/users/${id}")?
    Json.parse(response.body)
  end

  # Match with when guards + subject binding
  let classify(user: User) -> String do
    match user.age do |n|
      100 -> "centenarian"
      when n >= 65 -> "senior"
      when n >= 18 -> "adult"
      _ -> "minor"
    end
  end

  # if without do, then/else ternary
  foul greet(id: Int) do
    let user = fetchUser(id)?
    let title = user.age >= 18 then "Mr/Ms" else "Young"
    let greeting = "${title} ${user.name}"

    if user.email
      Email.send(user.email, greeting)
    else
      IO.printLine("No email for ${user.name}")
    end

    # Trailing if for early return
    return None if !adult?(user)

    # while without do, break/next with trailing if
    var attempts = 0
    while attempts < 3
      next if skip?(user)
      match Notification.send(user) do
        Ok(_) -> break
        Error(_) -> attempts = attempts + 1
      end
    end
  end

  # loop without do
  foul listen do
    loop
      receive do
        (:fetch, id, sender) -> sender.send(fetchUser(id))
        :stop -> return
      end
    end
  end
end

# Optional parens before do
describe "UserService" do
  it "fetches users" do
    let result = UserService.fetchUser(1)
    assert result.ok?
  end
end

# Logical operators: &&, ||, ! only
let valid?(user: User) -> Bool =
  user.age > 0 && user.name.length > 0 && !user.name.empty?
```

## Test files needed

### Passing tests (should parse successfully)

**`spec/foul_standalone.kex`**
- `foul` at top level
- `foul` inside module
- `foul` inside `make` block
- `foul` inside `private do ... end`
- `foul` inside `public do ... end`
- `foul` with operator overload: `foul +(other) do ... end`
- `foul` one-liner: `foul printHello = IO.printLine("Hello!")`
- `foul` with return type: `foul fetch(id: Int) -> Result<User, Error> do ... end`
- Mixed `let` and `foul` in same module

**`spec/if_no_do.kex`**
- Basic if/end
- if/else/end
- if/elif/else/end
- Multiple elif chains
- Nested if inside if
- if inside function body (do...end block)
- if inside loop
- Multiline condition with parens: `if (a\n && b)\n ... end`
- Single-line condition with complex expression: `if a > 0 && b < 10`
- if as expression (assigning result): `let x = if cond\n a\n else\n b\n end`

**`spec/while_loop_no_do.kex`**
- Basic while/end
- while with break
- while with next
- while with break + trailing if
- while with next + trailing if
- loop/end
- loop with break
- loop with receive inside
- Nested while inside loop
- Nested loop inside while

**`spec/when_guards.kex`**
- Basic guard: `pattern when cond -> ...`
- Guard with complex expression: `pattern when a > 0 && a < 100 -> ...`
- Guard with function call: `pattern when s.length > 0 -> ...`
- Multiple patterns with guard: `(:ok, val) when val > 0 -> ...`
- Subject binding: `match x do |n| when n > 0 -> ... end`
- Bare when (no pattern): `when n >= 18 -> ...` (sugar for `_ when ...`)
- Mixed literal patterns and guards: `100 -> ...\n when n > 50 -> ...`
- Pattern + guard + subject binding: `Ok(val) when val > 0 -> ...`
- Nested match with guards
- Guard referencing multiple bound variables

**`spec/then_else.kex`**
- Basic: `let x = a > 0 then a else b`
- Precedence: `let x = a + b > 0 then c + 1 else d - 1`
- Nested with parens: `let x = a then (b then c else d) else e`
- Nested in else: `let x = a then b else (c then d else e)`
- Inside function args: `foo(x > 0 then a else b)`
- In assignment RHS
- With method calls: `let x = list.empty? then "none" else list.first`
- Parenthesized multiline:
  ```
  let x = (condition
    then longValue
    else otherValue)
  ```
- Inside if body (else on same line doesn't conflict):
  ```
  if x > 0
    let y = z > 1 then a else b
  else
    other()
  end
  ```

**`spec/optional_parens_do.kex`**
- String arg: `describe "test" do ... end`
- Keyword arg: `worker Database, restart: :always do ... end`
- Multiple args: `foo a, b, c do ... end`
- UpperIdent arg: `make Option, implement: Monad do ... end`
- Nested do blocks: `describe "outer" do it "inner" do ... end end`
- Mixed with parens: `foo(a, b) do ... end` (still works)
- Arg with brackets: `foo items: [1, 2, 3] do ... end`
- Arg with braces: `foo handler: { |x| x + 1 } do ... end`

**`spec/logical_operators.kex`**
- `&&` basic: `a && b`
- `||` basic: `a || b`
- `!` prefix: `!a`
- Chained: `a && b || c`
- With comparison: `x > 0 && y < 10`
- In if condition: `if a && b`
- In while condition: `while !done?`
- In trailing if: `return None if !valid?`
- Precedence: `!a && b || c` = `((!a) && b) || c`
- With parens: `!(a && b)`

**`spec/break_next.kex`**
- `break` in while
- `break` in loop
- `next` in while
- `next` in loop
- `break if condition` (trailing if)
- `next if condition` (trailing if)
- `break` inside receive inside loop (exits loop)
- `break` inside match inside while (exits while)
- Nested loops with break (breaks inner only)

### Error tests (should produce compile errors)

**`spec/errors/foul_errors.kex`**
- Pure function calling foul function: `let foo = foulFunc()` → error
- Foul closure in pure context: `let foo(xs) = xs.map { |x| IO.printLine(x) }` → error

**`spec/errors/break_next_errors.kex`**
- `break` outside loop → error
- `next` outside loop → error
- `break` inside closure inside loop → error: `loop\n items.each do |i| break end\n end`
- `next` inside closure inside loop → error

**`spec/errors/then_else_errors.kex`**
- `then` without `else` → error: `let x = a > 0 then b`
- Nested `then` without parens → error: `let x = a then b then c else d else e`

**`spec/errors/if_errors.kex`**
- `if` with `do` → error (or at least should not work anymore)
- `while` with `do` → error
- `loop do` → error

**`spec/errors/logical_errors.kex`**
- Using `and` as operator → parse error (it's now an identifier)
- Using `or` as operator → parse error
- Using `not` as prefix operator → parse error

### 7. `break` and `next` for loops

**Rule:** `break` exits the loop, `next` skips to the next iteration. Works with trailing `if`.

```kex
while attempts < 3
  next if skip?(item)
  break if done?
  process(item)
end

loop
  receive do
    :stop -> break
    msg -> handle(msg)
  end
end
```

**Lexer:** Add `Break` and `Next` to `TokenType` enum, keyword map, and `tokenTypeName`.

**AST:** Add `BreakExpr` and `NextExpr` variants (no value — just control flow).

**Parser:** Parse as standalone statements. They're valid inside `while` and `loop` blocks only (compiler error elsewhere — can be checked during parsing or later in semantic analysis).

**Scoping rules:**
- `break`/`next` apply to the nearest enclosing `while` or `loop` — NOT to `each` blocks or other closures.
- Using `break`/`next` inside a closure (do-block passed to a function) is a compile error.
- `break` inside `receive` inside `loop` exits the `loop`, not the `receive`.

**Grammar:**
```
break_expr = BREAK
next_expr  = NEXT
```

## Order of Implementation

1. ✅ **Lexer** — `when` and `then` keywords added
2. **Match guards** — swap `if` to `when` in parser + examples
3. **`foul` keyword** — parser refactor + examples
4. **Drop `and`/`or`/`not`** — remove from parser + update examples
5. **`if`/`while`/`loop` without `do`** — parser change + all examples
6. **`then`/`else` expression** — new AST node, parser, grammar
7. **Optional parens before `do`** — parser enhancement
8. **`break`/`next`** — add loop control keywords

## Verification

1. `cmake --build build` — must compile cleanly after each step
2. `./build/kex examples/<file>.kex` — run each modified example
3. Run all 29 examples to check for regressions

## Key implementation risks

- **`if` without `do`:** Condition parsing must stop at newline (respecting paren/brace depth). The `m_noDoBlocks` flag logic needs rethinking.
- **`then`/`else` vs `if`'s `else`:** Inside a multiline `if` body, `else` on its own line closes the `if`. Inside an expression `x then a else b`, `else` belongs to `then`. Parser must distinguish by context (expression vs statement position / same-line vs new-line).
- **Optional parens:** Must not interfere with existing paren-required calls. Only applies when `do` follows.
- **`foul` in visibility blocks:** `private do ... end` wraps function definitions — parser currently looks for `Let` there, must also recognize `Foul`.
