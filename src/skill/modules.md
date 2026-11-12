# Modules in Kex

A module is a namespace. Its functions are called with the module's name in
front, or by their bare names after `using`.

```kex
module Geometry do
  let square(n: Float) -> Float = n * n

  let circleArea(radius: Float) -> Float = Math.PI * square(radius)

  private do
    let secret(n: Float) -> Float = n
  end
end

module Report do
  using Geometry, only: [circleArea]

  let line(radius: Float) -> String = "area ${circleArea(radius)}"
end

main do
  IO.printLine(Geometry.square(3.0))
  IO.printLine(Report.line(1.0))
end
```

## A program in several files

A file that starts with `module Name` is found by `using Name` from the files
beside it, by its file name: `module Inventory` lives in `inventory.kex`. A
file with no `module` line cannot be used from another file.

```kex inventory.kex
module Inventory

type StockError = UnknownItem(String) | NotEnough(String, Integer)

record Item do
  name  : String
  count : Integer = 0
end

record Stock do
  items : [Item] = []
end

make Stock do
  let add(name: String, count: Integer) -> This do
    New { items: [...@items, Item { name, count }] }
  end

  let take(name: String, wanted: Integer) -> This or! StockError do
    let item = @items.find { |item| item.name == name }.try
    return Error(NotEnough(name, item.count)) if item.count < wanted
    let rest = @items.map do |other|
      let left = other.count - wanted
      other.name == name then Item { ...other, count: left } else other
    end
    Ok(New { items: rest })
  rescue return Error(UnknownItem(name))
  end

  let total -> Integer = @items.reduce(0) { |sum, item| sum + item.count }
end

let empty -> Stock = Stock {}
```

```kex shop.kex
using Inventory                              # finds inventory.kex beside this file

main do
  let stock = Inventory.empty.add("tea", 5)  # qualified, or bare: both work
  match stock.take("tea", 9) do
    Ok(left)                  => IO.printLine(left.total)
    Error(NotEnough(name, n)) => IO.printLine("only ${n} ${name}")
    Error(UnknownItem(name))  => IO.printLine("no ${name}")
  end
end
```

```kex inventory.spec.kex
using Inventory                              # needed because inventory.kex is a module

describe("Inventory") do
  it("refuses more than there is") do
    let stocked = Stock {}.add("mug", 1)
    Assert.equal(stocked.take("mug", 3), Error(NotEnough("mug", 1)))
  end
end
```

## Rules

- A file that is one module starts with `module Name` on its own line, with no
  `do` and no `end`; the rest of the file is the module. Nested modules use
  `module Name do ... end`.
- File names are lower case with no separators: `module OptionParser` lives in
  `optionparser.kex`, `module Net.HTTP` in `net/http.kex`.
- `using M` brings in every public name; `using M, only: [f, g]` brings in
  those; `using M, as: Short` only shortens the prefix. Put `using` in the
  narrowest place that needs it: a function body may start with one.
- For two or three uses, write the qualified name (`Geometry.square`) and
  import nothing.
- Everything is public unless it sits in the module's `private do ... end`
  block, which goes after the public functions.
- Inside a module, call its own functions by their bare names.
- Standard-library modules outside the prelude need `using` too:
  `using FS`, `using JSON`, `using Regex`. `kex -s <Name>` says which.
