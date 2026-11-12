# Kex style

How Kex is laid out and named. The checker accepts other layouts, but this is
the form the formatter produces and the standard library is written in, so
write it this way from the start.

```kex
# A script starts with `main`, then its functions in the order a reader meets
# them. Nothing has to be declared before it is used.
main do
  let cart = Cart {}.add("tea", 450).add("mug", 1200)
  IO.printLine(receipt(cart))
end

# Two spaces per level. `end` sits under the line that opened the block.
# Fields are `name : Type`, one per line, with the colons and the defaults in
# columns.
record Cart do
  lines    : [Line] = []
  currency : String = "EUR"
end

record Line do
  item  : String
  cents : Integer
end

# Types and variants are PascalCase. A one-line list of variants while it
# fits; one per line under the `=` when it does not.
type Discount = NoDiscount | Percent(Integer) | Fixed(Integer)

make Cart do
  # A public function has a doc comment directly above it: one sentence
  # saying what the caller gets. Its types are in the `let`: `name: Type` for
  # each parameter and `-> Type` for the result.
  #
  # @param item [String] what is being bought
  # @param cents [Integer] its price in cents
  # @return [Cart] a new cart with the line added
  let add(item: String, cents: Integer) -> This do
    New { lines: [...@lines, Line { item, cents }] }
  end

  # Returns the price of everything in the cart, in cents.
  let total -> Integer = @lines.reduce(0) { |sum, line| sum + line.cents }

  # A predicate ends in `?` and is never `isEmpty` or `hasLines`.
  let empty? -> Bool = @lines.empty?

  # Helpers nobody else calls go in one `private do` block, last.
  private do
    let count = @lines.count
  end
end

# Functions and bindings are camelCase. A one-expression body is `=` on one
# line; anything longer is `do ... end`.
let euros(cents: Integer) -> String = "${cents / 100} EUR ${cents % 100}c"

# One-line definitions that follow each other line up on `=`, and the arms of
# a `match` on `=>`.
let discounted(cents: Integer, discount: Discount) -> Integer do
  match discount do
    NoDiscount  => cents
    Percent(by) => cents - cents * by / 100
    Fixed(off)  => cents - off
  end
end

let receipt(cart: Cart) -> String do
  # A guard is a `return` on its own line; the last expression is the result.
  return "nothing bought" if cart.empty?

  # A block on one line uses braces, with a space inside each.
  let rows = cart.lines.map { |line| "${line.item}: ${euros(line.cents)}" }

  # A block over several lines uses `do ... end`.
  let checked = cart.lines.map do |line|
    let price = discounted(line.cents, Percent(10))
    "${line.item} for ${euros(price)}"
  end

  # A chain that does not fit breaks before each dot, one level in.
  let summary = rows
    .map(&.upperCase)
    .join(", ")

  "${summary} / ${checked.join(", ")} / total ${euros(cart.total)}"
end
```

## Layout

- Two spaces per level, no tabs. `end` goes under the line that opened the
  block. No blank line straight after `do` or straight before `end`.
- Lines stay under 80 columns, and never over 100. A definition too long for
  one line breaks its parameters one per line inside the parentheses; a
  chain breaks before each dot.
- One space around every binary operator and `=`: `a + b`, `n >= low`. None
  around `..` (`1..10`) and none after a unary operator: `-count`, `!done?`,
  `~parse`, `&.even?`, `@radius`.
- No space inside `( )` or `[ ]` or before a call's parenthesis: `f(x)`,
  `[1, 2, 3]`. One space inside braces: `{ "name": "Kex" }`, `{ |n| n > 3 }`.
- A parameter is `name: Type`; a record field is `name : Type`. The
  difference is deliberate.
- Comments are `# ` with one space, on their own line above the code or two
  spaces after it.

## Naming

- `PascalCase` for modules, records, types, traits and variants. `camelCase`
  for functions, bindings and fields. Never `snake_case`.
- Type parameters are single capitals: `X`, `E`, `K`, `V`.
- A function or field that is a `Bool` ends in `?`, with no `is` or `has` in
  front: `empty?`, `admin?`.
- Name a function for what it gives back, not `getX` or `computeX`: `parent`,
  `count`, `total`. Keep verbs for effects: `write`, `send`.
- File names are lower case with no separators: `optionparser.kex`.

## Shape

- A function that fits on one line: `let f(x: T) -> R = expr`. Anything over
  one line, including one long expression: `let f(x: T) -> R do ... end`.
- One-line conditional: `cond then a else b`. `if ... end` only when a branch
  needs its own line.
- One-line block: `{ |x| ... }`. Several lines: `do |x| ... end`.
- `&.name` for a block that only calls a function on its argument,
  `~name` for one that only passes its argument to a function:
  `filter(&.even?)`, `map(~parse)`.
- Write `X?` and `A or! E`, not `Optional<X>` and `Result<A, E>`.
- Inside a `make`, write `This` for the type being made, not its name.
- Prefer a guard (`return x if cond`) to nesting, clauses to a `match` on a
  parameter, and `if let` to a `match` with one interesting arm.
- Types go in the `let`: `let name(arg: ArgType) -> RetType`. A separate
  declaration (`name : ArgType -> RetType`) is only for a function written as
  several pattern-matching clauses, and for a trait's requirements.
- A public function has a doc comment above it. The first line of the
  comment is one sentence ending in a period.
- No function is `foul` unless it performs an effect.
