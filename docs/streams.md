# Streams, Feeds, and Enumerable

## Enumerable Hierarchy

`Range` and `[A]` (lists) implement the `Enumerable` trait —
structural trait membership via `make implement:`, not nominal type
inheritance (see `docs/types.md`'s Type Hierarchy / Enumerable Hierarchy
sections for the full pattern):

```kex
trait Enumerable do
  each : (This, A -> Void) -> Void
end

make Range implement: Enumerable do
  let each(f) = ...
end

make [A] implement: Enumerable do
  let each(f) = ...
end
```

`Stream` and `Feed` are not `Enumerable`: reducing an unbounded stream need
not terminate, and a feed can only be walked once. Both provide their own
`map`, `filter`, `take` and `each`.

Functions that accept any `Enumerable` work with all of these — the
parameter is constrained by the trait, not a concrete type:

```kex
let sumAll(xs: Enumerable) -> Int = ...
```

## Stream — Pure and Lazy

Immutable, reusable lazy sequences created with `Stream.Sequence(from:, step_fn)`:

```kex
let naturals = Stream.Sequence(from: 0) { |n| n + 1 }
let evens = Stream.Sequence(from: 0) { |n| n + 2 }
let powers = Stream.Sequence(from: 1) { |n| n * 2 }
```

A stream remembers the elements it produces, so walking one twice costs one
walk and `take(n)` is n steps rather than n².

Taking from a stream materializes elements into a list:

```kex
let first_ten = naturals.take(10)   # [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
let from5 = naturals.drop(5).take(5) # [5, 6, 7, 8, 9]

# Streams are reusable — naturals hasn't changed
let another_ten = naturals.take(10)
```

## Range

`1..10` creates a `Range<Int>`:

```kex
let r = 1..10
r.min    # 1
r.max    # 10
r.map { |x| x * 2 }
```

## Feed — One-Shot and Stateful

For an IO resource that can only be read once. `FS.File.feed` and
`handle.feed` answer one:

```kex
let lines = FS.File.feed("big.txt").or(Feed.empty)
```

Feeds are consumed — once you read from them, the data is gone:

```kex
main do
  let feed = FS.File.feed("log.txt").or(Feed.empty)
  feed.each do |line|
    IO.printLine(line) if line.contains?("ERROR")
  end
end
```

Taking twice walks forward rather than repeating, which is the whole
difference from `Stream`:

```kex
let feed = FS.File.feed("log.txt").or(Feed.empty)
feed.take(2)   # the first two lines
feed.take(2)   # the NEXT two
feed.spent?    # true once the source has run out
```

`map`, `filter` and `drop` answer a feed over the same cursor, so a pipeline
is a single pass and a file of any size costs the same memory to walk:

```kex
FS.File.feed("app.log").or(Feed.empty)
  .filter { |line| line.contains?("ERROR") }
  .take(10)
```

`collect` drains a feed into a list, and `toStream` answers a `Stream` that
remembers what it reads — replay, at the cost of holding it. Going the other
way, `Stream.toFeed` walks a long stream without retaining its start.

Feed methods are `let` rather than `foul`, because a feed over anything
outside the program can only come from a foul call (`FS.File.feed`,
`handle.feed`) — the effect is tracked where it enters.

## Summary

| Type | Pure? | Reusable? | Use Case |
|------|-------|-----------|----------|
| `Stream<A>` | Yes | Yes | Computed sequences, infinite lists |
| `Range<A>` | Yes | Yes | Numeric ranges, iteration |
| `[A]` | Yes | Yes | Materialized collections |
| `Feed<A>` | No | No | IO resources larger than memory, event sources |
