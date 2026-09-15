# Dimension algebra

The first part of the [typed measures redesign](units-design.md) is implemented
in Kex, in the opt-in `Dimensions` module at `src/stdlib/dimensions.kex`. This is the algebra foundation; existing
`Measure`, `Unit`, and `Duration` APIs have not yet been migrated, and dimension
mismatches are not yet compile-time errors.

## Declare a base

Use a dedicated nominal marker type for each independent base dimension:

```kex
using Dimensions

record DistanceMarker do
end

record ElapsedMarker do
end

let distance = Dimensions.base(DistanceMarker {})
let elapsed = Dimensions.base(ElapsedMarker {})
```

The marker's type identifies the base. Field values do not affect identity.
Separate modules may use the same short marker name without sharing a base;
import and reuse the same marker declaration to share one. Use dedicated,
nongeneric markers rather than primitives, containers, or display names.

`Dimensions.Dimension` contains a map from reflected `Type` identities to Kex
integer exponents. It uses existing map equality and arbitrary-precision
integer arithmetic. There is no C++ dimension implementation and no fixed
number of base dimensions.

## Compose dimensions

```kex
let speed = distance / elapsed
let acceleration = distance / (elapsed ^ 2)

(speed * elapsed) == distance              # true
(distance * elapsed) == (elapsed * distance) # true
(distance / distance).dimensionless?       # true
```

Multiplication adds exponents, division subtracts them, and integer powers
multiply them. Each operation removes zero powers. `Dimensions.one` is the
dimensionless identity; negative powers represent inverse dimensions.

```kex
(elapsed ^ 2).exponentOf(ElapsedMarker {})   # 2
(elapsed ^ -1).exponentOf(ElapsedMarker {})  # -1
distance.exponentOf(ElapsedMarker {})        # 0
```

To import an existing map, call `Dimensions.fromPowers` so explicit zero
entries are removed. Constructing the record directly bypasses normalization.
Reflection records can be constructed manually too; this runtime API is not
proof that a future static dimension parameter matches its value.

## Compile-time evaluation

The same Kex code can run in a `compiled do` block. The specs exercise both
runtime calculation and reification of a computed dimension as a constant.
This allows normalization to be reused during compilation, but does not on
its own implement generic dimension inference or static measure checking.

## Example and verification

Run `examples/dimensions.kex` to derive force, energy, power, and pressure
from three marker types. It prints:

```text
true
-1
true
```

`spec/stdlib/dimensions.spec.kex` covers nominal identity, exponent arithmetic,
normalization, physical formulas, custom information dimensions, and compiled
evaluation. `spec/type_of_empty_records.kex` pins runtime reflection of empty
nominal records, including the BEAM representation as an atom.

## Remaining migration

The agreed redesign still requires replacing atom-based unit compatibility,
removing pairwise SI arithmetic, introducing statically typed measures and an
explicit dynamic boundary, removing `Duration`, and migrating constructors,
callers, specs, and examples. CSS is an extensibility requirement only; it is
not being implemented.
