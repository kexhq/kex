# Random values and reproducible sequences

Import `Random` for ordinary random draws or an immutable seeded generator.
The operations have the same names in both forms.

```kex
using Random

let roll = Random.integer(1..6)
let weight = Random.float(-1.0..1.0)
let heads = Random.boolean
let simulateFailure = Random.chance?(0.01)
let selected = Random.choice(["red", "green", "blue"])
let hand = Random.sample(cards, count: 5)
let shuffled = Random.shuffle(cards)
```

Ordinary operations return values directly and are `foul`: their outcomes can
change between otherwise identical calls. Use them when reproducibility is
unnecessary. These are non-cryptographic generators, including the ordinary
operations; do not use their output for passwords, tokens, or other secrets.

## Secrets

`Random.secureBytes(count)` reads the operating system's cryptographically
secure generator, and `Random.token(bytes)` renders that many secure bytes as
lowercase hex. Use these for keys, session tokens and unguessable identifiers:

```kex
let key = Random.secureBytes(32)      # a Binary of 32 bytes
let session = Random.token(32)        # 64 hex characters
```

## Replay a sequence

`Random.seeded(42)` returns a `Random.Generator`. Every generator operation
returns **`(value, nextGenerator)`**, in that order. Keep the returned generator
to continue the sequence:

```kex
let generator = Random.seeded(42)
let (roll, afterRoll) = generator.integer(1..6)
let (weight, afterWeight) = afterRoll.float(-1.0..1.0)
let (hand, afterHand) = afterWeight.sample(cards, count: 5)
```

The generator is a value, not a mutable handle. Reusing the old value repeats
the draw; it does not silently advance global or process state:

```kex
let initial = Random.seeded(42)
let (first, advanced) = initial.integer(1..6)
let (replayed, _) = initial.integer(1..6)
Assert.equal(first, replayed)

let (second, _) = advanced.integer(1..6)
# The state advanced, but a repeated die roll is still possible.
```

Equal seeds and the same sequence of operations reproduce the same results
on interpreter and BEAM. Seeds are reduced modulo 2^64, so negative and
arbitrarily large integer seeds are accepted. `Random.fresh` creates a
similar generator seeded from host entropy when a sequence should vary
between runs.

## Bounds and edge cases

| Operation | Meaning |
| --- | --- |
| `integer(1..6)` | Both integer endpoints included |
| `float(-1.0..1.0)` | Lower endpoint included, upper endpoint excluded |
| `float` | Defaults to `[0.0, 1.0)` |
| `boolean` | Fair coin flip |
| `chance?(0.25)` | True with probability 0.25 |
| `choice(items)` | Uniformly chosen input position, wrapped in `Just`; `None` if empty |
| `sample(items, count: 5)` | Up to five input positions, without replacement, in random order |
| `shuffle(items)` | All positions in a random order |

Both endpoints must have the same type; use `1.0..6.0` for floating bounds,
not `1..6.0`. Integer bounds must be ascending and contain at most 2^64 integers. A
singleton range such as `7..7` returns 7 and consumes a draw. Float bounds
must be finite and strictly ascending. Probabilities must lie in `[0.0, 1.0]`;
both endpoint probabilities still consume one draw. Invalid arguments fail
with a descriptive runtime error.

Sampling a negative count fails. Sampling zero positions, choosing from an
empty input, or shuffling an empty input leaves the generator unchanged.
An oversized sample selects the whole input in shuffled order. Sampling
preserves duplicate values: selecting two positions from `["a", "a", "a"]`
returns `["a", "a"]`. Neither sampling nor shuffling mutates the input.

Range endpoints are retained without constructing an integer list, so large
random bounds do not imply a large allocation. Floating ranges represent
bounds, not an enumerable sequence; they have no implicit step size.

## Pass randomness through application code

Helpers should return their result alongside the updated generator. This
lets a caller reproduce an entire initialization sequence without hidden
state. See [the XOR model](../examples/mlp_xor.kex) for a complete example:
its hidden and output layers consume a single sequence seeded with 42.

[The dealing example](../examples/random/dealing.kex) demonstrates replaying
a hand, continuing the sequence, and then drawing a floating-point weight.
Each hand is sampled from the supplied full deck; keeping generator state
does not remove cards from that deck.

## Testing

Use an explicit seed, assert the desired outcome, and retain the returned
state when testing subsequent draws. Tests can also verify replay without
hard-coding an algorithm's output:

```kex
let initial = Random.seeded(42)
let (order, advanced) = initial.shuffle([1, 2, 3, 4])
let (replayed, replayState) = initial.shuffle([1, 2, 3, 4])
Assert.equal(order, replayed)
Assert.equal(advanced, replayState)
Assert.equal(order.sort, [1, 2, 3, 4])
```

The [library specs](../spec/stdlib/random.spec.kex) also verify deterministic
reference words, seed normalization, range bounds, and state advancement.

## Replacing the original API

Use `Random.Generator` instead of `Rng`, and construct it through
`Random.seeded` or `Random.fresh`. Replace `nextBounded(6)` with
`integer(0..5)`, `nextFloat` with `float`, and `nextBoolean` with `boolean`.
Replace `Random.between(1, 6)` with `Random.integer(1..6)` and supply sample
sizes as `count:`. Algorithm constants and raw-word generation are private.
