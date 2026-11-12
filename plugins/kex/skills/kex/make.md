# `make`: functions on a type

Kex has no classes and no methods. A `record` or `type` only describes data,
and a `make` block declares functions whose first argument is a value of that
type, the receiver. `v.length` is the function `length` called on `v`, exactly
as `length(v)` is. A `make` can be written for any type: a record, a variant
type, a built-in or a generic one. Nothing is constructed, nothing is
inherited, and nothing is changed in place.

```kex
record Vector do
  x : Float = 0.0
  y : Float = 0.0
end

# `make Type do` declares functions that take a `Vector` first. Inside, `this`
# is that value, the receiver, and `@x` is its field.
make Vector do
  # Nothing but the receiver: no parentheses, called as `v.length`.
  let length -> Float = Math.sqrt(@x * @x + @y * @y)

  # The types go in the `let` itself. The receiver is the first argument and
  # is not written, so the type of `scaled` is `This -> Float -> This`. `This`
  # is the type being made; nothing here names Vector.
  let scaled(factor: Float) -> This = New { x: @x * factor, y: @y * factor }

  # `New { ... }` copies the receiver with some fields replaced.
  let withX(x: Float) -> This = New { x: x }

  # Call another function on the receiver through `this`.
  let unit -> This = this.scaled(1.0 / this.length)

  # An operator is a function named by the operator.
  let +(other: This) -> This = New { x: @x + other.x, y: @y + other.y }

  # A predicate ends in `?`.
  let zero? -> Bool = @x == 0 && @y == 0

  private do
    let squared -> Float = @x * @x + @y * @y
  end
end

# A trait names what a type must provide, and may give defaults built on it.
trait Shape do
  area :> Float
  name :> String

  let describe = "${name} with area ${area}"
end

record Circle do
  radius : Float
end

record Square do
  side : Float
end

# `implement:` claims a trait; the block must define what the trait requires.
make Circle, implement: Shape do
  let area = Math.PI * @radius * @radius
  let name = "circle"
end

make Square, implement: Shape do
  let area = @side * @side
  let name = "square"
end

# Variant types work the same way. Match the receiver with `@` in the
# clause head.
type Tree = Leaf | Node(Tree, Integer, Tree)

make Tree do
  # Several clauses share one declaration, directly above them. `:>` is `:`
  # with the receiver left out: this line says `size : This -> Integer`.
  size :> Integer
  let size(@Leaf) = 0
  let size(@Node(left, _, right)) = left.size + 1 + right.size

  insert :> Integer -> This
  let insert(@Leaf, value) = Node(Leaf, value, Leaf)
  let insert(@Node(left, here, right), value) do
    return Node(left.insert(value), here, right) if value < here
    Node(left, here, right.insert(value))
  end
end

# Built-in types take a `make` too.
make Integer do
  let doubled -> Integer = this * 2
end

make [Integer] do
  let evens -> [Integer] = this.filter(&.even?)
end

# A generic record names its parameters in the `make` as well.
record Box<A> do
  value : A
end

make Box<A> do
  let map(f: A -> B) -> Box<B> = Box { value: f(@value) }
end

# A parameter typed by a trait takes anything that implements it.
foul report(shape: Shape) -> Void = IO.printLine(shape.describe)

main do
  let v = Vector { x: 3.0, y: 4.0 }
  IO.printLine("${v.length} ${v.scaled(2.0).x} ${v.withX(9.0).x} ${v.unit.length}")
  IO.printLine((v + Vector { x: 1.0 }).x)
  IO.printLine(Vector {}.zero?)

  report(Circle { radius: 1.0 })
  report(Square { side: 2.0 })
  let shapes: [Shape] = [Circle { radius: 1.0 }, Square { side: 2.0 }]
  IO.printLine(shapes.map(&.name))

  let tree = Leaf.insert(5).insert(2).insert(8)
  IO.printLine(tree.size)
  IO.printLine("${21.doubled} ${[1, 2, 3, 4].evens}")
  IO.printLine(Box { value: 2 }.map { |n| n + 1 }.value)
end
```

## `this`, `This`, `New` and `new`

Four names exist only where there is a receiver: inside a `make`, a `trait`
or a `serving` block. They differ by a letter or its case:

- `this` is the receiver, a value: `this.length`, `others.reduce(this)`.
- `This` is the receiver's type. In a `make Money` it is `Money`; in a trait
  it is whichever type implements the trait. Inside a `make`, write `This`
  rather than the type's own name: renaming the type, or moving the block to
  another one, then changes one line instead of every declaration.
- `New { ... }` is a copy of the receiver with some fields replaced.
- `new` is a copy of the receiver whose fields are assigned one by one,
  `new.balance = ...`, and which is then answered. Use it when several fields
  change; `kex --skill processes` shows it.

A `serving` block is the third place a record gets behaviour: `make` declares
functions on a value, `serving` declares what a process holding that value
answers. Both see the same `@field`, `this`, `New` and `new`. A `serving` block adds
one separator, `::>`, for declaring a slot that answers its caller.

`This` is what lets a trait say "the same type as me". `merge :> This -> This`
takes another value of the implementing type and answers one, so for `Money`
it is `Money -> Money` and for `Tags` it is `Tags -> Tags`:

```kex
# In a trait, `This` is whichever type implements it.
trait Mergeable do
  merge :> This -> This

  # A default, written once and typed for every implementer. A default has no
  # declaration: in a trait, a declaration means "required".
  let mergeAll(others: [This]) -> This do
    others.reduce(this) { |sum, other| sum.merge(other) }
  end
end

record Money do
  cents : Integer = 0
end

record Tags do
  names : [String] = []
end

# Here `This` is Money: `merge` takes a Money and answers a Money.
make Money, implement: Mergeable do
  let merge(other) = New { cents: @cents + other.cents }

  # In a `make`, `This` is Money. Prefer it to writing Money again.
  let doubled -> This = this.merge(this)
end

# Here `This` is Tags.
make Tags, implement: Mergeable do
  let merge(other) = New { names: [...@names, ...other.names] }
end

main do
  let others = [Money { cents: 10 }, Money { cents: 20 }]
  let total = Money { cents: 5 }.mergeAll(others)
  IO.printLine(total.cents)
  IO.printLine(Money { cents: 4 }.doubled.cents)
  IO.printLine(Tags { names: ["a"] }.merge(Tags { names: ["b"] }).names)
end
```

## Rules

- **Shape.** `make Type do ... end`. The `let`s inside it are ordinary
  functions that take the receiver implicitly. There is no `class`, no
  `initialize`, no `self` and no instance variable to assign. Never put a
  function inside the `record` itself.
- **The receiver** is `this`. Its fields are `@field`, not `this.field` and
  not `self.field`. Use `this` to pass the whole receiver or to call another
  function on it (`this.length`).
- **Types go in the `let`** when a function has one body:
  `let scaled(factor: Float) -> This = ...`. Do not put a declaration above
  a single `let`.
- **`name :> Type` is a declaration**, the same as `name : Type` with the
  receiver implied: `insert :> Integer -> This` is
  `insert : This -> Integer -> This`, so the caller passes one argument. The
  same holds for a typed `let`: `let scaled(factor: Float) -> This` has the
  type `This -> Float -> This`. Write a declaration in two places only: above
  a function with several pattern-matching clauses, which it types once for
  all of them, and in a trait, where a declaration with no `let` is a
  requirement on implementers.
- A function of the receiver alone has no parentheses: `let length -> Float`,
  called as `v.length`.
- **Changing a record** means answering a new one: `New { x: 1.0 }` is the
  receiver with `x` replaced and every other field kept. Do not restate the
  type with all its fields, and never assign to `@x`. Outside its own `make`,
  copy a record with a spread: `Vector { ...other, x: 1.0 }`.
- **Matching the receiver.** In a clause head `@Pattern` stands for the
  receiver: `let size(@Leaf) = 0`, `let size(@Node(left, _, right)) = ...`.
  Further parameters follow it: `let insert(@Leaf, value) = ...`.
- **Traits.** `trait Name do` lists required functions as declarations and
  may define defaults with `let`. A default carries its types in the `let`
  itself and has no declaration above it: a declaration in a trait always
  means the implementer must provide that function.
  `make Type, implement: Name do` claims a trait, and several are separated
  by commas: `implement: Shape, Describable`. A missing required function is
  an error. The trait already types them, so the
  implementing `let` needs no declaration of its own.
- **A trait is a type.** `foul report(shape: Shape)` accepts every type that
  implements `Shape`, and `[Shape]` is a list of any of them.
- **Operators** are functions named by the operator:
  `let +(other: This) -> This = ...`.
  Override only the existing ones, with their usual meaning.
- **`This`** is the receiver's type. Use it for the type being made in every
  declaration inside a `make`, and in a trait wherever the type is "whoever
  implements this": `compare :> This -> Ordering`. A generic `make` that
  answers a different instantiation still names it:
  `let map(f: A -> B) -> Box<B>`. It is the type
  `:>` leaves out: `merge :> This -> This` is `merge : This -> This -> This`.
- **Generic types** repeat their parameters: `make Box<A> do`.
- **Built-ins and lists** take a `make` too: `make Integer do`,
  `make [Integer] do`.
- **Private functions** go in one `private do ... end` block at the end of the
  `make`.
- Several `make` blocks for one type are fine; each adds to it.
