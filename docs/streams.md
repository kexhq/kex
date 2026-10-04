# Streams, feeds, and collections

Use a list for values already in memory, a range for consecutive integers or
characters, a stream for a lazy sequence you may revisit, and a feed for a
source that is consumed once.

## Collection traversal

Lists, strings, maps, and ranges implement `Foldable` and `Enumerable` by
providing `reduce`. `Foldable` supplies traversal methods such as `each`,
`find`, `all?`, and `any?`. `Enumerable` supplies transformations such as
`map`, `filter`, and `flatMap`.

```kex
[1, 2, 3].reduce(0) { |sum, n| sum + n }   # => 6
[1, 2, 3].map { |n| n * 2 }               # => [2, 4, 6]
[1, 2, 3].filter(~even?)                   # => [2]
```

Streams and feeds provide their own traversal methods. They do not implement
these collection traits: a stream may be infinite, and a feed changes as it
is read.

## Streams: lazy and reusable

`Stream.Sequence` creates an infinite sequence from a starting value and a
step function. `map`, `filter`, and `drop` return streams; `take` produces a
list with at most the requested number of elements.

```kex
let naturals = Stream.Sequence(from: 0) { |n| n + 1 }
naturals.take(5)                         # => [0, 1, 2, 3, 4]
naturals.map { |n| n * n }.take(4)       # => [0, 1, 4, 9]
naturals.filter(~even?).take(3)          # => [0, 2, 4]
naturals.drop(5).take(3)                 # => [5, 6, 7]
naturals.take(3)                         # => [0, 1, 2]
```

A stream remembers the elements already read, so reading it again starts at
the same position. Keeping the start of a stream also keeps its cached values
in memory. A filter may never produce a result if an infinite source has no
matching elements.

## Ranges

Integer and character ranges include both endpoints. Their collection methods
materialize the elements, so use care with large ranges. `min`, `max`, and
`first` return optionals because a range can be empty.

```kex
let numbers = 1..4
numbers.items                  # => [1, 2, 3, 4]
numbers.min                    # => Just(1)
numbers.max                    # => Just(4)
numbers.map { |n| n * 2 }      # => [2, 4, 6, 8]
(4..1).items                    # => []
('a'..'c').items                # => ['a', 'b', 'c']
```

Float ranges describe continuous bounds and cannot be enumerated with these
methods. To check continuous bounds, compare with both endpoints explicitly.

## Feeds: consumed once

Reading a feed advances its cursor. `Feed.Elements` lets you try that behavior
without opening a file:

```kex
let feed = Feed.Elements(["one", "two", "three"])
feed.take(2)   # => ["one", "two"]
feed.take(2)   # => ["three"]
feed.take(2)   # => []
feed.spent?    # => true
```

`map`, `filter`, and `drop` share the source cursor. Reading either the original
feed or a derived feed consumes that source. These transformations allow a
single pass without retaining every element.

For files, `FS.File.feed` returns `None` when the source cannot be opened.
Handle that case explicitly rather than treating a missing file as empty:

```kex
using FS

main do
  match FS.File.feed("app.log") do
    Just(lines) => do
      let errors = lines.filter { |line| line.contains?("ERROR") }.take(10)
      errors.each { |line| IO.printLine(line) }
    end
    None => IO.printError("Could not open app.log")
  end
end
```

`collect` drains the remaining feed into a list. Use it only when the source
is finite and fits in memory. `toStream` makes the values read from a feed
replayable by caching them. `Stream.toFeed` gives a consuming cursor over a
stream; release references to the stream's start when you do not need replay.

Opening a file-backed feed is `foul`. Consuming an existing feed uses ordinary
methods, but still advances its cursor.
