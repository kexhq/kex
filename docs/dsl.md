# DSL Support

## Block<[A]> — Collection Blocks

When a function's last parameter is `Block<[A]>`, the compiler collects each expression in the `do...end` body into a list:

```kex
div classes: ["info"] do
  h1 "Title"      # collected as Element
  p "Content"     # collected as Element
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

## Block<A> vs () -> A

- `Block<A>` — explicitly a `do...end` block, returns last expression
- `() -> A` — lambda/thunk, can be inline `{ expr }`
- `Block<[A]>` — collection mode

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
