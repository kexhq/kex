# Pattern Matching

## Match Expression

```kex
match value do
  pattern1 => result1
  pattern2 => result2
  _ => default
end
```

Arms take `=>`. So do bare `when` clauses, `rescue` clauses and `receive` arms:
`->` belongs to the type language alone (function types and return
annotations), so the line you are reading tells you which of the two you are
in.

## Pattern Types

### Literals

```kex
match n do
  0 => "zero"
  1 => "one"
  _ => "other"
end
```

### Variable Binding

```kex
match option do
  Just(x) => x
  None => default
end
```

### Wildcard

`_` matches anything without binding:

```kex
let first(@[x | _]) = Just(x)
```

### Constructor Patterns

```kex
match shape do
  Circle(r) => 3.14 * r * r
  Rectangle(w, h) => w * h
end
```

### Tuple Patterns

```kex
let (x, y) = point
match (a > 0, b > 0) do
  (true, true) => "both positive"
  _ => "not both"
end
```

### List Patterns

```kex
match list do
  [] => "empty"
  [x] => "single"
  [x, y] => "pair"
  [x | rest] => "at least one"
end
```

### Record Destructuring

```kex
let { name, age } = user
let { address: { city } } = profile
```

In function heads:

```kex
make User do
  let greet({ name }) = "Hi, ${name}!"
  let adult?({ age }) = age >= 18
end
```

### `@` — Match on This

Inside `make` blocks, `@` matches the structure of `this`:

```kex
make [A] do
  let first(@[]) = None
  let first(@[x | _]) = Just(x)
end
```

`@` is only needed at the top level. Inside nested patterns, normal pattern syntax applies:

```kex
let render({ children: [first | _] }) = ...
```

## Guards

Trailing `if` in match clauses:

```kex
match n do
  0 => "zero"
  n if n > 0 => "positive"
  _ => "negative"
end
```

## Multi-Clause Functions

Pattern matching directly in function definitions:

```kex
let factorial(0) = 1
let factorial(n: Int) = n * factorial(n - 1)

let zip(@[], _) = []
let zip(@_, []) = []
let zip(@[x | xs], [y | ys]) = [(x, y) | xs.zip(ys)]
```

Clauses are checked top-to-bottom, first match wins.
