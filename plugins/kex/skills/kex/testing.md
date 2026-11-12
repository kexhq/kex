# Testing Kex

A spec is a file named `<name>.spec.kex` beside `<name>.kex`, with no `main`.
It sees everything `<name>.kex` declares without importing it.

A file that starts with `module Name` is different: do not name its spec
after it. Give the spec a name of its own and start it with `using Name`, as
`stock.spec.kex` does for `inventory.kex` in `kex --skill modules`. A spec
named after a module file loads that file twice, and on some systems the
checker then reports every function as defined twice.

Run a spec with `kex <name>.spec.kex`, or `kex -R <name>.spec.kex` where there
is no Erlang; the exit status is non-zero when a case fails.

```kex stack.kex
record Stack do
  items : [Integer] = []
end

make Stack do
  let push(item: Integer) -> This = New { items: [item | @items] }

  let top -> Integer? = @items.first

  let depth -> Integer = @items.count
end
```

```kex stack.spec.kex
# A fixture is an ordinary function at the top of the spec.
let filled -> Stack = Stack {}.push(1).push(2)

describe("Stack") do
  it("starts empty") do
    assert(Stack {}.depth == 0)
    assert(Stack {}.top == None)
  end

  it("puts the newest item on top") do
    Assert.equal(filled.top, Just(2))        # on failure, prints both values
    Assert.equal(filled.depth, 2)
  end

  it("keeps the old stack unchanged") do
    let bigger = filled.push(3)
    Assert.equal(bigger.depth, 3)
    Assert.equal(filled.depth, 2)
    Assert.some(bigger.top)                  # passes for any Just
  end
end
```

## Rules

- `describe("unit") do ... end` groups cases; `it("does something") do ... end`
  is one case; `assert(condition)` fails the case when the condition is false.
  `assert(condition, "message")` adds a message.
- A failed `assert` says only that it failed. Prefer `Assert.equal(actual,
  expected)`, which prints both values, and its relatives: `Assert.notEqual`,
  `Assert.some`, `Assert.none`, `Assert.ok`. `kex -s Assert` lists them all.
- A case may print and may call `foul` functions.
- `before do ... end` and `after do ... end` run around each case of their
  `describe`.
- Compare values, not their printed form: `assert(result == Ok(3))`.
- One `describe` per file, named for what is tested. One behaviour per `it`.
- `kex --test-only "Stack > starts empty" stack.spec.kex` runs one case,
  `--test-list` lists the cases without running them, and `--test-json`
  reports one JSON record per case.
- `describe`, `it`, `before`, `after` and `assert` are ordinary functions that
  every program has, so do not give your own functions those names.
