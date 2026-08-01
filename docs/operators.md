# Operators

## Fixed Set

Kex has a fixed set of operators. No custom operators allowed — they make code unreadable.

No pipe operator (`|>`) either — UFCS covers chaining: `input.parse.transform.format`.

## Arithmetic

| Op | Meaning |
|----|---------|
| `+` | Addition / String concatenation |
| `-` | Subtraction |
| `*` | Multiplication |
| `/` | Division |
| `%` | Modulo |

## Comparison

| Op | Meaning |
|----|---------|
| `==` | Structural equality |
| `!=` | Not equal |
| `<` | Less than |
| `>` | Greater than |
| `<=` | Less or equal |
| `>=` | Greater or equal |

## Boolean

| Op | Meaning |
|----|---------|
| `&&` | Logical AND |
| `\|\|` | Logical OR |
| `!` | Logical NOT (prefix) |

## Special

| Op | Meaning |
|----|---------|
| `..` | Range (`1..10`) |
| `...` | Spread into a list, a map, or a `Block<[A]>` body |
| `!` (suffix) | Mutating call on var |
| `~` (prefix) | Capture / partial application (`~func`, `~func(args)`, `~Mod.func`, `~(op)`) |
| `&.` (prefix) | Receiver shorthand (`&.method` = `{ \|x\| x.method }`) |

`~` and `&.` are easy to confuse. `~name` captures the function `name` and
passes it along; `&.name` builds a lambda that calls `.name` **on its
argument**. Each has exactly one spelling: `&` is always followed by `.` and an
identifier, and every operator capture goes through `~`. `&name` and `&.+` are
both invalid, and the parser points you at `~name` and `~(+)`.

Every binary operator can be captured with `~(op)`, plus `~(!)` for unary
negation. See [functions.md](functions.md#operators) for the full list and the
short-circuiting caveat on `~(&&)` / `~(||)`.

## Overriding

Types can override operator behavior in `make` blocks:

```kex
make Vector do
  let +(other: This) -> This do
    return Vector { x: this.x + other.x, y: this.y + other.y }
  end

  let ==(other: This) -> Bool do
    return this.x == other.x && this.y == other.y
  end
end
```

## String Concatenation

`+` works for strings:

```kex
"hello " + "world"   # "hello world"
```

Prefer interpolation for complex cases:

```kex
"${greeting}, ${name}!"
```

## Equality

`==` is structural equality by default. Override in `make` for custom behavior.
