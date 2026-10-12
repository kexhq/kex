# DSL Support

## Block<[A]> — Collection Blocks

When a function's last parameter is `Block<[A]>`, the compiler collects each expression in the `do...end` body into a list:

```kex
div classes: ["info"] do
  h1 "Title"      # collected as Element
  p "Content"     # collected as Element
end
```

The call may be qualified, through a module path or an `as:` alias, and keeps
the paren-less form:

```kex
Migration.define :add_owner, follows: :create_sessions do
  createTable(:owners) { |t| [t.string(:name)] }
  addIndex(:owners, [:name])
end
```

## Rules for Block<[A]>

1. Each expression contributes one `A` to the list
2. A guarded expression (`expr if cond`) contributes nothing when false
3. `...expr` spreads an `[A]` into the list

```kex
div do
  h1 "Always here"
  p "Warning" if showWarning?         # skipped if false
  ...items.map { |i| p(i.text) }     # spread list
end
```

## Block<P, [A]> — Builder Blocks

A block can be handed a value and still collect. The types the block receives
come first and its result last, so `Block<TableBuilder, [Column]>` is a
`do |t| ... end` whose statements are each a `Column`:

```kex
let createTable(name: Atom, columns: Block<TableBuilder, [Column]>) -> Table =
  Table { name: name, columns: columns(TableBuilder {}) }

createTable(:owners) do |t|
  t.string(:name, nullable: true)
  t.bigint(:credit)
  t.bigint(:legacy) if migrating?     # guards and spreads work as above
end
```

The column functions stay in the builder's `make` block, so they neither
compete with other modules for their names nor need to accumulate by side
effect.

Collection is decided by the declared type alone: a parameter written as a
plain function, `TableBuilder -> [Column]`, takes a block that returns its
last expression, and that expression has to be the list.

## Block<A> vs () -> A

- `Block<A>` — explicitly a `do...end` block, returns last expression
- `() -> A` — lambda/thunk, can be inline `{ expr }`
- `Block<[A]>` — collection mode
- `Block<P, A>` / `Block<P, [A]>` — the same two, for a block handed a `P`

## Using for Scoped DSL Imports

```kex
using Html.Language do
  html do
    head do
      title "My Page"
    end
    body do
      h1 "Hello"
    end
  end
end
```

`using Module do...end` brings all public names into scope for the duration of the block.

## Compiled Metaprogramming

Generate repetitive DSL functions at compile time:

```kex
module Html.Language do
  compiled do
    ELEMENTS = ["html", "head", "body", "div", "p", "h1", "h2", "h3"]

    ELEMENTS.each do |name|
      let %name(id = None, classes = [], block: Block<[Node]>) -> Element do
        let children = block()
        return Element { name: "%name", id: id, classes: classes, children: children }
      end
    end
  end
end
```

### Compiled Rules

- Same Kex syntax, pure only (+ `ENV.get`)
- `%variable` splices a string as an identifier
- Sees all declarations in scope (types, functions, etc.)
- Can produce: functions, types, constants, `make` blocks
- Runs before type-checking — generated code is checked normally after expansion
