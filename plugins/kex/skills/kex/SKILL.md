---
name: kex
description: Write, check and test Kex (.kex) source. Read before writing or editing any Kex code. Kex looks like Ruby but is statically typed, immutable and functional, so Ruby and Elixir habits produce code that does not compile.
---

# Writing Kex

Kex has Ruby's look (`do ... end`, blocks, `?` predicates) on a typed
functional core: values are immutable, there is no `nil` and there are no
exceptions, and every call is checked before the program runs. Most code a
model writes from Ruby or Elixir habit is rejected. Work in this loop instead:

1. **Look up, never guess.** `kex -s List` lists everything a type or module
   offers, `kex -s List.find` shows one function with examples, `kex -s find`
   finds a name on every type, and bare `kex -s` lists the whole standard
   library. A wrong guess prints the names that come close. In a shell, quote
   a name that ends in `?`, or leave the `?` off: `kex -s List.empty`.
2. **Check after every edit.** `kex -C file.kex` type-checks without running.
   Add `--json` for diagnostics as JSON. Fix every error before going on.
3. **Run.** `kex file.kex` runs on the BEAM (needs Erlang); `kex -R file.kex`
   runs the same program in the built-in interpreter. `kex name.spec.kex` runs
   tests.

No `kex` on the path? `brew install kexhq/tey/tey`, or
`curl -fsSL https://kex.run/install.sh | sh`, installs it.

Every example below is a whole program that this compiler checks and runs.
A `# not:` comment shows the habit that does not work, beside the line that
does.

## Functions

```kex
# not: def double(n) ... end
let double(n: Integer) -> Integer = n * 2

# A body with several lines is `do ... end`. The last expression is the
# result; `return` is only for leaving early.
let describeAge(age: Integer) -> String do
  return "invalid" if age < 0
  let decades = age / 10

  "${decades} decades"                       # not: "#{decades} decades"
end

# No parameters, no parentheses: here and at every call.
let greeting -> String = "hello"             # not: let greeting() = ...

# A function that answers Bool ends in `?`.
let adult?(age: Integer) -> Bool = age >= 18 # not: isAdult, is_adult

# Several clauses, one per pattern. Only then are the types written apart,
# in a declaration above. With one body they go in the `let`, as in `double`.
factorial : Integer -> Integer
let factorial(0) = 1
let factorial(n) = n * factorial(n - 1)

# `foul` marks a function which have side effects, including I/O. A function without it cannot call one (other than `IO.inspect`, but that is automatically removed in production builds).
foul announce(text: String) -> Void = IO.printLine(text)   # not: puts text

main do
  announce(greeting)                         # not: greeting()
  announce(describeAge(42))
  announce("${double(4)} ${factorial(5)} ${adult?(20)}")
  announce(4.double.to(String).or(""))       # a.f(b) is f(a, b), for any function
end
```

Your own names can collide with functions every program already has:
`describe`, `it`, `before`, `after`, `assert`, `inspect`, `to`, `die`.
`kex -s "(top level)"` lists them. Keywords cannot be names either: `return`,
`next`, `break`, `then`, `loop`, `match`, `when`.

## Values

```kex
main do
  let limit = 10                             # never changes; `limit = 11` is an error (immutable)
  var count = 0                              # `var` may change (mutable)
  count += 1

  var names = ["b", "a"]
  names.push!("c")                           # `!` rewrites (mutates) the var; only a var takes it
  let sorted = names.sort                    # an ordinary call answers a new list

  # No nil. A value that may be missing is `X?`: `Just(x)` or `None`.
  let first = names.first                    # String?, not String
  IO.printLine(first.or("nobody"))           # not: first || "nobody"
  if let Just(name) = first                  # not: if first
    IO.printLine(name.upperCase)             # not: upcase
  end
  IO.printLine(first.map(~capitalize).or(""))
  IO.printLine(first.set?)                   # not: first.nil?, first != nil

  # Text.
  let user = "Ada"
  IO.printLine("hello, ${user}")             # not: "hello, #{user}"
  IO.printLine("total: ${count + limit}")    # not: "total: " + count
  let initial = 'A'                          # single quotes are one Char
  IO.printLine(initial.upper?)
  let page = `
    several lines,
    kept as written
  `                                          # backticks for text over lines
  IO.printLine(`no ${holes} in here`)        # backticks are raw: this prints ${holes}
  IO.printLine($`but ${user} here`)          # a leading $ turns interpolation on
  IO.printLine(page.lines.count)

  # Conversion is `to(Type)` and answers an optional.
  IO.printLine("42".to(Integer).or(0) + 1)   # not: "42".to_i, toInteger, parseInt
  IO.printLine(count.to(String).or("") + "!")  # not: to_s, toString
  IO.printLine(Integer.parse("x").or(-1))    # parse answers Ok(n) or Error(e)

  # Numbers.
  IO.printLine(7 / 2)                        # 3
  IO.printLine(7 / 2.0)                      # 3.5
  IO.printLine(7 % 2)
  IO.printLine(3.7.round)                    # round takes no digits
  IO.printLine(sorted.join(", "))
end
```

## Lists, maps and tuples

```kex
main do
  let words = ["pear", "fig", "apple"]
  IO.printLine(words.count)                              # not: size (length also works)
  IO.printLine(words.filter { |w| w.count > 3 })         # not: select
  IO.printLine(words.reduce(0) { |sum, w| sum + w.count })  # not: inject
  IO.printLine(words.contains?("fig"))                   # not: include?
  IO.printLine(words.mapIndexed { |w, i| "${i}:${w}" })  # not: each_with_index
  IO.printLine(words.take(2))                            # not: first(2)
  IO.printLine(words.sortBy(&.count))                    # not: sort_by(&:count)
  IO.printLine(words.at(9).or("none"))                   # at answers an optional
  IO.printLine([...words, "plum"])                       # a new list with one more
  IO.printLine(["kiwi" | words])                         # a new list with one in front
  IO.printLine((1..5).map { |n| n * n }.sum)

  let text = "  Hello, World  "
  IO.printLine(text.trim)                                # not: strip
  IO.printLine(text.lowerCase)                           # not: downcase
  IO.printLine(text.trim.startsWith?("Hello"))           # not: start_with?
  IO.printLine(text.trim.split(", "))
  IO.printLine(text.trim.replace("World", "Kex"))        # not: gsub, sub

  # A map. String keys are quoted; a bare key is an atom.
  let ages = { "ada": 36, "alan": 41 }                   # not: { "ada" => 36 }
  IO.printLine(ages.get("ada").or(0))                    # not: ages["zed"] || 0
  let more = ages.put("grace", 45)                       # a new map; `ages` is unchanged
  IO.printLine(more.keys)
  IO.printLine(more.has?("grace"))                       # not: key?, has_key?
  more.each { |name, age| IO.printLine("${name} is ${age}") }
  let byYear = Map.from([(1990, 3), (2000, 1)])          # not: { 1990: 3 }; a literal
  IO.printLine(byYear.get(1990).or(0))                   # takes string and atom keys only

  # A tuple is taken apart by pattern. It has no `.first`.
  let pair = ("x", 1)
  let (label, value) = pair                              # not: pair[0], pair.first
  IO.printLine("${label}=${value}")
  let pairs = [("a", 2), ("b", 1)]
  IO.printLine(pairs.sortBy { |_, n| n })                # not: { |(_, n)| n }
end
```

## Conditions and matching

`kex --skill control` has the whole of it, with loops.

```kex
type Shape = Circle(Float) | Rect(Float, Float)

let sign(n: Integer) -> String = n < 0 then "neg" else "pos"   # not: n < 0 ? "neg" : "pos"

let grade(score: Integer) -> String do
  if score >= 90                             # not: if score >= 90 then
    "A"
  elif score >= 80                           # not: elsif, else if
    "B"
  else
    "C"
  end
end

let area(shape: Shape) -> Float do
  match shape do                             # not: case shape
    Circle(r)  => Math.PI * r * r            # not: when Circle(r) then ...
    Rect(w, h) => w * h                      # every variant must be covered
  end
end

let size(n: Integer) -> String do
  match n do
    0            => "zero"
    n when n < 0 => "negative"               # a guard is `when`, not `if`
    _            => "positive"
  end
end

main do
  IO.printLine("${sign(-1)} ${grade(85)} ${area(Rect(2.0, 3.0))} ${size(-4)}")
  let items = [1, 2, 3]
  IO.printLine("has items") if !items.empty?  # not: unless items.empty?; not: if items
  IO.printLine(items.count > 2 && items.contains?(3))   # not: and, or, not
end
```

## Blocks

```kex
let add(a: Integer, b: Integer) -> Integer = a + b

main do
  let numbers = [1, 2, 3, 4]
  IO.printLine(numbers.map { |n| n * 2 })    # one line: braces

  numbers.each do |n|                        # several lines: do ... end
    let square = n * n
    IO.printLine(square)
  end

  IO.printLine(numbers.filter(&.even?))      # &.even? is { |n| n.even? }; not: &:even?
  IO.printLine(numbers.map(&.pow(2)))        # &. takes arguments too
  IO.printLine(numbers.map(~add(10)))        # ~add(10) is { |n| add(10, n) }
  IO.printLine(numbers.reduce(0, ~add))      # ~add is the function itself
  IO.printLine(numbers.map { |n| Just(n) })  # not: ~Just; `~` takes no constructor

  # Chains read left to right with dots. There is no |> operator.
  let result = numbers
    .filter(&.odd?)
    .map { |n| n * n }
    .sum
  IO.printLine(result)
end
```

## Types, records and traits

```kex
# Variants, each with its own data. Not a class hierarchy.
type Payment = Cash | Card(String) | Transfer(String, String)

# With one variant only, keep a leading bar.
type Failure = | Boom(String)

# Too many for one line: one per line, the bars under the `=`.
type OrderError = OutOfStock(String)
                | UnknownProduct(String)
                | PaymentDeclined(String, Integer)

# A record has named fields. Fields are `name : Type`.
record User do
  name   : String
  email  : String
  admin? : Bool = false
end

# A trait: required functions as declarations, defaults with a body.
# `:>` is `:` with the receiver implied: `label : This -> String`.
trait Describable do
  label :> String

  let shout = label.upperCase
end

# Functions on a type go in a `make` block, never inside the record. It is
# not a class: `ada.label` is the function `label` called on `ada`.
make User, implement: Describable do
  let label = "${@name} <${@email}>"         # @name is this user's field; not: self.name

  # `This` is the type being made, User. `New` copies with a field changed.
  let promote -> This = New { admin?: true }
end

# Spellings: [X] list, X? optional, A or! E result, Map<K, V>, (A, B) tuple,
# A -> B function. A type parameter is a single capital letter.
# A name for a type that already exists is an alias:
type Scores = Map<String, Integer>

let firstOr(items: [X], fallback: X) -> X = items.first.or(fallback)

let describePayment(payment: Payment) -> String do
  match payment do
    Cash               => "cash"
    Card(number)       => "card ${number}"
    Transfer(from, to) => "transfer ${from} to ${to}"
  end
end

main do
  let ada = User { name: "Ada", email: "ada@example.com" }   # not: User.new(...)
  IO.printLine(ada.label)
  IO.printLine(ada.promote.admin?)
  let bob = User { ...ada, name: "Bob" }     # another record's fields, one replaced
  IO.printLine(bob.label)
  IO.printLine(ada.shout)
  IO.printLine(firstOr([], 7))
  IO.printLine(describePayment(Card("1234")))
end
```

## Files, input and other modules

`IO`, `List`, `Map`, `String`, `Math`, `Process` and the rest of the prelude
need nothing. Everything else needs `using`; `kex -s <Name>` says which. Your
own files work the same way: see `kex --skill modules`. A program of more
than a few files should be a package: see `kex --skill tey`.

```kex
using FS                                     # not: require, import
using JSON

foul firstLine(path: String) -> String do
  let text = FS.File.read(path).try          # .try leaves through rescue on failure
  return text.lines.first.or("")
rescue return ""
end

main(args) do                                # command-line arguments, as [String]
  let path = args.first.or("notes.txt")
  FS.File.write(path, "one\ntwo\n")
  IO.printLine(firstLine(path))
  FS.File.delete(path)

  IO.printLine(JSON.stringify({ "ok": true, "count": 2 }))
  match JSON.parse(`{"name": "kex"}`) do
    Ok(data)  => IO.printLine(data)
    Error(_)  => IO.printError("bad json")
  end
  IO.printLine(ENV.get("HOME").set?)
end
```

## More

Each of these is one worked program followed by its rules. Read as an
installed skill, each topic is also a file beside this one, named for the
topic: `control.md`, `make.md` and so on.

- `kex --skill functions` covers the forms of a function, clauses,
  declarations, defaults, lambdas and `~`.
- `kex --skill make` covers functions on a type, `@field`, `New`, traits and
  operators.
- `kex --skill control` covers conditions, `match`, patterns and loops.
- `kex --skill errors` covers results, optionals, `.try` and `rescue`.
- `kex --skill modules` covers modules, `using`, `private` and programs
  in several files.
- `kex --skill processes` covers state that lives in a process: `serving`,
  which is a GenServer with a declared protocol.
- `kex --skill testing` covers `describe` / `it` / `assert` and spec files.
- `kex --skill style` covers layout, naming and which form to pick.
- `kex --skill tey` covers packages: creating one, where files go, running,
  testing and dependencies.
