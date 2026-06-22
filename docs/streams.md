# Streams, Feeds, and Enumerable

## Enumerable Hierarchy

```kex
type Enumerable<A>     # parent
type Range<A> < Enumerable<A>
type [A] < Enumerable<A>
type Stream<A> < Enumerable<A>
```

Functions that accept `Enumerable<A>` work with any of these.

## Stream — Pure and Lazy

Immutable, reusable lazy sequences created with `Stream.Sequence(from:, step_fn)`:

```kex
let naturals = Stream.Sequence(from: 0) { |n| n + 1 }
let evens = Stream.Sequence(from: 0) { |n| n + 2 }
let powers = Stream.Sequence(from: 1) { |n| n * 2 }
```

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

## Feed — Foul and Stateful

For IO resources that produce values over time:

```kex
foul let lines: Feed<String> = File.feed("big.txt")
foul let messages: Feed<Message> = socket.feed
foul let events: Feed<Event> = keyboard.feed
```

Feeds are consumed — once you read from them, the data is gone:

```kex
foul do
  let feed = File.feed("log.txt")
  feed.each do |line|
    IO.printLine(line) if line.contains?("ERROR")
  end
end
```

## Summary

| Type | Pure? | Reusable? | Use Case |
|------|-------|-----------|----------|
| `Stream<A>` | Yes | Yes | Computed sequences, infinite lists |
| `Range<A>` | Yes | Yes | Numeric ranges, iteration |
| `[A]` | Yes | Yes | Materialized collections |
| `Feed<A>` | No | No | IO resources, event sources |
