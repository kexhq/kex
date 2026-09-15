# Compositional, typed measures

Status: proposal for [issue #339](https://github.com/kexhq/kex/issues/339).
This document specifies a replacement; it does not describe shipped APIs.
Signatures and expressions below are design notation, not executable Kex.

## Decision

Keep measures for quantities whose units have a fixed multiplicative scale.
This includes physical quantities, elapsed amounts of time, and data sizes.
Remove the `Duration` type and constructor namespace entirely, with no
compatibility alias. Use `Measure<Dimensions.Time>` for elapsed time, which is
already a linear time quantity. `Dimensions.Time` names the dimension separately from
the existing `Time` clock-value type.

Use one dimension algebra for both static and dynamic quantities. Carry known
dimensions in types; expose runtime-defined dimensions through an explicitly
dynamic API. A runtime exponent vector alone does **not** provide static typing.

### Prelude boundary

The shared `Measure<D>` foundation, dimension arithmetic, time dimension, and
plural numeric time constructors are intended for the prelude alongside the
time API.
Timers and date operations can therefore accept `Measure<Dimensions.Time>`
without a units import. SI vocabulary stays behind `using Units.SI`; CSS stays
behind `using CSS`. Removing `Duration` must not make the prelude depend on
either opt-in vocabulary. The initial Kex algebra prototype remains an opt-in
`Dimensions` module until its prelude bootstrap cost is addressed. CSS is a
future extension, not part of this work.

Exclude affine points (such as Celsius readings), calendar periods, logarithmic
units, and context-dependent conversions from the core. Temperature differences
can use linear units. A future temperature-point API should distinguish
point minus point from point plus difference. Layout and currency APIs should
resolve their context explicitly before producing a linear quantity, where
appropriate; the core should not acquire an optional universal context argument.

## 1. Dimension identity and normalization

A dimension is a normalized finite map from nominal base-dimension identities
to integer exponents. Nominal means identity comes from a declaration, not its
spelling. Two modules declaring `Distance` create different base identities;
both must import the same declaration to interoperate. Derived dimensions are
structural: their normalized maps determine equality.

The standard basis contains length, mass, time, electric current, temperature,
amount of substance, luminous intensity, and information. Use metre, kilogram,
second, ampere, kelvin, mole, candela, and byte as canonical units respectively.
Allow extensions through fresh base declarations, without changing a central
vector width or maintaining a global registry of names. Persisted identities
will need stable module/declaration identities once packaging is defined;
process-local IDs must not become a public serialization format.

Normalization combines equal keys, removes zero exponents, and orders keys
deterministically. The empty map is dimensionless.

| Operation | Resulting dimension |
|---|---|
| Multiply | Add exponents |
| Divide | Subtract exponents |
| Integer power `n` | Multiply exponents by `n` |
| Add, subtract, compare, convert | Require equal normalized maps |

For example, pressure is `mass · length^-1 · time^-2`, obtained from
`Newton / Meter^2`. `Newton / Meter` has a different dimension. Cancellation
must make `(length / time) * time` identical to length, and multiplication
must normalize independently of operand order and parentheses.

Dimensions do not encode every semantic distinction. Energy and torque share
a vector. Angle and ordinary ratios are dimensionless here. Applications that
need to prohibit those combinations should wrap quantities in domain types;
unit symbols and optional descriptive names must never change compatibility.

## 2. Separate quantities from unit expressions

Conceptual representations:

```text
Unit<D>    = positive finite scale to canonical units + display expression
Measure<D> = canonical Float + preferred Unit<D>
```

Expose `value` as `canonical / unit.scale`; do not store a second independently
editable magnitude. Unit construction and measure updates must preserve the
dimension relationship. Typed constructors must not let callers attach an
arbitrary dimension parameter to an unrelated runtime vector. Retain `Float`
for this redesign; exact arithmetic and uncertainty propagation are separate
work. Validate scales after composition as well as initial definition, since
products and powers can underflow or overflow.

Units compose with the same algebra as quantities. Their scales multiply,
divide, or take integer powers along with their dimensions. A derived unit is
an expression with an optional display alias:

```text
Newton = named("N", Kilogram * Meter / Second^2)
Joule  = named("J", Newton * Meter)
Watt   = named("W", Joule / Second)
Pascal = named("Pa", Newton / Meter^2)
Liter  = named("L", scaled(Meter^3, 0.001))
Gram   = named("g", scaled(Kilogram, 0.001))
```

Adding Pascal requires one definition and, optionally, a numeric convenience
constructor. It requires no new arithmetic overloads. Prefixes are scaled
unit expressions; `Kilo(Watt * Hour)` has scale 3,600,000 relative to joules.
Explicit prefix nesting compounds scales, so `Milli(Kilogram)` equals a gram.
Formatting can preserve such an expression without inventing a new alias.

Keep display expressions structured so `(m/s)^2` cannot print as `m/s^2`.
Addition/subtraction preserve the left display unit; products and quotients
compose display units and their scales. Thus `2.newton * 3.meter` can display
`6.0 N·m`, and explicit conversion to Joule displays `6.0 J`.
Do not automatically select a named unit by dimension: several names can share
one dimension, and adding an import must not change existing output.

Equality and ordering compare canonical magnitudes after checking dimensions,
ignoring display units. Floating-point equality remains ordinary numeric
equality; approximate comparison is an explicit operation, with a tolerance of
the same dimension. Formatting rounding must not change stored values.

## 3. Static contract

The following signatures describe the required type rules. `D + E`, `D - E`,
and `nD` are dimension algebra, not proposed general-purpose Kex syntax:

```text
Measure<D> + Measure<D>       -> Measure<D>
Measure<D> - Measure<D>       -> Measure<D>
Measure<D> * Measure<E>       -> Measure<D + E>
Measure<D> / Measure<E>       -> Measure<D - E>
Measure<D> * Number           -> Measure<D>
Number * Measure<D>           -> Measure<D>
Measure<D> / Number           -> Measure<D>
Number / Measure<D>           -> Measure<-D>
Measure<D> ^ constant Integer -> Measure<nD>
Measure<D>.convertTo(Unit<D>) -> Measure<D>
```

Unit multiplication, division, prefixes, and powers preserve corresponding
static dimension information. A union of several time units can retain the
common time dimension; a heterogeneous unit collection requires erasure.

Incompatible static addition or conversion is a compile error. Compatible
operations return a measure directly, without a `Result` used only to report
dimension mismatch. Existing numeric domain failures retain numeric semantics;
static dimensional correctness does not prove nonzero divisors or finite results.

Keep dimensionless results as `Measure<Dimensionless>` and provide explicit
scalar extraction. This avoids return types changing between numbers and
measures after cancellation. Power zero produces dimensionless one under the
numeric power operation's domain rules. Negative integer powers are supported;
fractional powers are out of scope for the first implementation. A later root
operation can require exponents divisible by the root degree.

### Implement the algebra in Kex

Dimension normalization, unit composition, conversions, and quantity arithmetic
belong in the Kex standard library. Use ordinary maps, arbitrary-precision
integers, generic records, and compiled evaluation. Do not implement a parallel
dimension engine in C++ or hard-code the SI vocabulary into the compiler.

The first implementation is `src/stdlib/dimensions.kex`, exercised by
`spec/stdlib/dimensions.spec.kex` and `examples/dimensions.kex`. It uses `Type`
values obtained from dedicated marker values as base identities, so two
different nominal marker types remain distinct even when their short names
match. It currently supplies runtime algebra; integration into measures and
static rejection remain work in progress.

### Prove the static contract

Kex has generic records and phantom type parameters, but writing `Measure<D>`
does not supply dimension normalization or exponent-dependent result typing.
Compile-time evaluation alone does not establish these typing rules either.
Before promising static safety, prototype:

1. A representation of normalized dimensions in generic type arguments, using
   Kex declarations and compiled evaluation wherever possible.
2. Equality, substitution, and inference of dimension expressions through
   generic functions, aliases, overloads, and return annotations. Generic
   multiplication must retain symbolic dimensions until arguments are known.
3. Constant-integer exponent checking and clear diagnostics showing expected
   and actual dimensions, preferably with familiar unit expressions.
4. Preservation of dimension metadata through compiled evaluation and IR,
   with the same runtime arithmetic on interpreter, BEAM, and wasm.

Any necessary compiler change should support a demonstrated general language
requirement, with the dimension calculation itself staying in Kex. The static
representation remains a prototype decision. Completion requires both positive inference tests and negative type
tests; generating a finite table of popular derived types is insufficient.

### Replace Duration with time measures

Proposed examples:

```text
2.seconds                             # Measure<Dimensions.Time>
1.minutes + 30.seconds                 # 1.5 min
100.meters / 10.seconds                # 10.0 m/s
Task.sleep(500.milliseconds)
[1, 2, 3].second                       # Just(2), existing list accessor
```

Provide one numeric constructor per unit, using its plural name: `.seconds`,
`.minutes`, `.meters`, and so on. Do not add singular or abbreviated synonyms;
migrate existing `.sec`, `.minute`, and `.meter` calls to those spellings.
The spelling does not depend on magnitude: `1.seconds` and `0.5.seconds` are
valid. Unit values retain conventional names such as `Second` and `Meter`,
and display symbols remain `s` and `m`. Existing list/string accessors such as
`.second` keep their current meaning. Move time-specific helpers (`wholeSeconds`,
`wholeMilliseconds`, `shorterThan?`, etc.) onto the time specialization. The
numeric `.seconds` accessor on time measures returns the canonical second count;
`.value` is the magnitude in the selected display unit. For example,
`2.minutes.seconds` is 120.0 while `2.minutes.value` is 2.0.

Keep `Date`, `Time`, and `DateTime` as point types, and `Period` as a calendar
span. Their elapsed-span operations accept `Measure<Dimensions.Time>` directly.
A fixed day is
86,400 seconds; calendar-day arithmetic remains explicit through `Period`.
Negative durations remain valid quantities; timer APIs enforce their own
existing constraints. Squaring a duration produces time squared and therefore
cannot be passed to sleep. Display preferences never affect timer behavior.

This is an API and representation migration, not just a renamed declaration.
Replace every `Duration` annotation with `Measure<Dimensions.Time>` and remove
the record and module. Replace `Duration.seconds(n)` and raw
`Duration { seconds: n }` construction with `n.seconds`; use `0.seconds` for
zero. Move the UTC-offset factory into the time API and migrate its callers.
Retain numeric accessor
semantics, and remove duplicate duration arithmetic in favor of shared measure
operations. Audit formatting changes and equality across different display
units. BEAM task and networking intrinsics currently pattern-match
`{'Duration', Seconds}`; migrate those consumers and defaults together with
interpreter bridges. Test specialized measure method resolution, date
arithmetic, offsets, and timers across backends. Verify that no `Duration` type,
constructor namespace, or runtime record dependency remains. Both current
types store floating-point seconds, so the merge itself need not change numeric
precision. It also does not promise exact nanosecond storage.

## 4. Explicit dynamic boundary

Provide `DynamicUnit` and `DynamicMeasure`, carrying the same normalized maps
as runtime data. Parsing user-supplied units and heterogeneous quantities uses
these types. Dynamic multiplication/division still compose universally;
addition, comparison, and conversion return a typed dimension-mismatch error
when needed. Errors include both dimensions, not just a generic string.

```text
Measure<D>.erase()                         -> DynamicMeasure
DynamicMeasure.check(Unit<D>)              -> Result<Measure<D>, UnitError>
DynamicMeasure.convertTo(DynamicUnit)      -> Result<DynamicMeasure, UnitError>
DynamicMeasure.pow(Integer)                -> DynamicMeasure
```

`check` validates the dimension and selects the supplied display unit. Parsing
must resolve base identities through an explicit catalog; matching a symbol
does not establish dimension identity. Scale validation errors also belong in
`UnitError`. Dynamic numeric domain failures follow the same numeric rules as
static operations.

A nonconstant exponent requires explicit erasure before `pow`; it must not
silently discard static information. No implicit conversion from a dynamic
measure to a statically typed one is permitted.

## 5. Delivery and compatibility

### Extensibility example: CSS (out of scope)

Do not implement a CSS module, CSS constructors, expression evaluation, or
layout resolution as part of this redesign. The core must allow an external
module to declare its own base dimensions, compose typed quantities, and wrap
them in a separate expression type without modifying core arithmetic.

A future CSS library could add a typed expression layer for unresolved layout quantities:
`2.rem + 8.px` produces `CSS.Expr<CSS.Length>` and can serialize to
`calc(2rem + 8px)`. Use one constructor spelling per CSS unit. Adding seconds
to a CSS length is a type error. Resolved CSS lengths use
`Measure<CSS.Length>`, whose base identity is distinct from physical length.

CSS expressions can be emitted directly for browser evaluation, or resolved
with an explicit property/layout context, returning a measure or a resolution
error. Percentages retain their unresolved basis until a property supplies it;
they are not unconditionally lengths. For example, width can accept
`100.percent - 16.px`. Font-relative references also require the appropriate
property context, rather than one universal font-size field.

CSS absolute units have fixed internal ratios (`96px = 72pt`); converting
between them does not require device DPI. Mapping CSS coordinates to physical
measurements is a separate operation. See the
[CSS Values and Units specification](https://www.w3.org/TR/css-values-4/).
This extension reuses dimension checking without putting unresolved expressions
or layout context into the core `Measure` representation.

### Implementation sequence

First implement the shared runtime algebra and repair the existing API while
preserving its checked `Result` shape. This is a useful correctness milestone,
but does not resolve the issue's compile-time requirement. In parallel with
design work, establish feasibility of the static rules above before expanding
the public vocabulary.

The runtime milestone must fix all current representations together:

- Replace atom compatibility checks in core, SI, Data, and display helpers.
- Remove duplicated pairwise arithmetic and the fallback that keeps the left
  dimension for unknown combinations.
- Correct gram and litre canonical scales, squared dimensions, and pressure.
- Use the same implementation for operator and named `times`/`per` forms.
- Compose symbols with their scales; canonical magnitudes must never be
  labeled with an unadjusted noncanonical unit.

Then introduce the static API and explicitly named dynamic API together.
Migrate numeric constructors to preserve dimensions, all prefix/display
overloads, and consumers of `.kind`, `.value`, and raw record construction.
Remove dimension-related `.map`/`.try` at statically compatible call sites.
Keep plural time constructors in the prelude and explicit SI/Data imports; generic arithmetic
belongs in the shared units core and should not require importing SI.

Document these intentional breaking changes: derived output uses composite
symbols until explicitly converted, `.kind` is replaced by dimension inspection,
fractional measure powers are rejected, stored `value` becomes derived, and
statically compatible conversion/addition no longer returns `Result`.
Do not preserve incorrect results as compatibility behavior.

## Acceptance criteria

| Case | Required outcome |
|---|---|
| Seconds squared converted to Minute | Static rejection; dynamic mismatch error |
| Metres plus seconds | Static rejection, including through a generic helper |
| `1.kilogram` converted to Gram | 1000 grams |
| `1.liter` converted to `Meter^3` | 0.001 cubic metres |
| Force divided by area | Convertible to Pascal |
| Force divided by length | Cannot convert to Pascal |
| Newton times metre, in either order | Same dimension and canonical value |
| `(metres / seconds) * seconds` | Length, including across function boundaries |
| Kilometres divided by hours | Correct magnitude in both km/h and m/s |
| Watts times hours converted to joules or kWh | Correct composed scale |
| Unit divided by itself; powers 0, 1, -1 | Normalized dimensions and scales |
| Two unrelated base declarations with the same name | Incompatible |
| Imported alias of one base declaration | Compatible |
| Data size divided by time | Information/time without extra overloads |
| Dimensionless ratio extracted as scalar | Explicit operation, correct scale |
| Dynamic quantity checked against a static unit | Validated promotion or error |
| Runtime exponent on a static quantity | Diagnostic directing explicit erasure |
| Additional unit alias imported | Existing display output unchanged |

Exercise algebraic normalization independently of floating-point arithmetic.
Run quantity and display cases on interpreter, BEAM, and wasm; use appropriate
tolerances for numeric checks. Update existing units/time/data specs and add
compiler-error fixtures for the static cases. Issue #339 is complete only when
both compositional arithmetic and static rejection are delivered.
