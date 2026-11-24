# Type checker: back to Hindley–Milner

Plan for bringing the type checker back to the Hindley–Milner design it was
built from, and for the features that go on top of it: traits as type classes,
kinds, higher-kinded traits and inferred capability sets. `Measure` dimensions
stay a library, built on overloads (§8.1).

Tracking issue: #808. Related: #415 (the branch join), #809 (`if` without
`else`), #651 (traits and bounds), #342 (type grammar), #142 (effect
polymorphism), #200 (contextual type parameters), #793, #642, #644, #646.

Every finding here was reproduced on `kex 0.4.0-rc.4 (4794703c)`.

The "Target" section (§4) is a **specification**, not a plan. When the work
below is done, it moves to a standing document and is never deleted as "done".
§3 explains why.

## Status (branch `hm-refactor`)

The refactor is in place, not a parallel core (§12): each step replaces the
code it supersedes and keeps every suite green.

Done:

- `Void` and `Never` named as the language names them (decision 9).
- One check for every condition and `when` guard (`checkCondition`): §11
  items 4 and 5.
- An `if` without `else`, and a trailing `x if c`, have no value (decision 8,
  #809), with KEXET-0506 where a value is needed.
- No type names compared as string literals in `src/semantic`, every name
  in `kex::names`, and the `check-type-names` guard with its allowlist (§7,
  rules 2 and 5). The integer and float name lists in `traits.cxx` are gone.
- One join for every conditional form, list elements, and a map's keys and
  values: different types make a union, and the wider of two that fit wins in
  either order (decision 3, §11 item 10). `commonTrait` and its list of trait
  names are deleted. A declared type is checked against each branch, with the
  error at the branch. A `match` arm's variable is narrowed to what earlier
  arms left.
- Decision 4 for one parameter (`tieTraitResult`): `[Integer].sum` is an
  Integer, and `twice(x: Number) -> Number` answers its argument's type (§11
  item 7). A body that answers a concrete type instead is an error for a
  declared contract, and is not tied for a checked one.
- A `Number` bound meets the promotion table: `x * 2.5` is a Float (§11
  item 8). The `units` functions that said `-> Number` to work around it say
  `-> Float`.
- One conformance check for trait implementations and capability stand-ins
  (§10): a result the implementation writes is checked against the
  requirement (KEXET-0507, §11 item 2, #642), and a stand-in must take each
  required arity (KEXET-0508, #793). It found `Mock.Files` missing the block
  form of `FS.File.open`, which now fails the way its two-argument form does.
- A function value passed for a generic function parameter is checked
  against the parameter as the call made it (§11 item 3).
- An unknown field reports the field, and a trait default's bare requirement
  (`label`) is the requirement on `this`, typed and lowered on both backends
  (§11 item 9).
- A written `Any` is distinguishable from an unknown (UnknownType::written,
  `any` in KexI and the interface cache, cache version 7). It is a declared
  contract, so a call taking one still checks its other arguments: §11 item
  11 is closed.
- `Any` is the top type for calls: a function is called on an `Any` only when
  it takes any value (a generic, a bound, `Any`), as for a rigid type
  variable (KEXET-0509, "narrow it first"). `JSON.stringify` narrows with a
  type pattern where it used a value of a runtime kind. Three specs that test
  the backends' dispatch for an erased receiver run unchecked. An `Any` is no
  condition and no arithmetic operand either, and a field read on one is
  KEXET-0509 as well.
- Dimensions are a library (§8.1): Units.SI declares each product and
  quotient with the dimension it answers (and its mirror), with `times` and
  `per` as the same overloads spelled out, so §11 item 6 is closed and an
  undeclared product is a type error. `Quantity` gained `Speed`,
  `Acceleration` and `Pressure`. KEXET-0128 and KEXES-0004 tell overloads
  apart by the receiver's type arguments and the parameter types, so a program
  declares its own dimension's products (`spec/measure_user_dimension.kex`).
  The coherence rule (fix 2) is KEXET-0510: an overload of a name another
  module declares, told apart from it by its types, needs one of those types
  to be the program's own (`spec/error_orphan_overload.kex`). Fix 4 is done
  too: a generated function's signature splices a type (`-> Measure<%{dimension}>`,
  a type named as a value or a String spelling one), so Units.SI writes its
  prefixed constructors from one table (`spec/compiled_type_splice.kex`,
  KEXEC-0058/0059 in `spec/error_type_splice*.kex`).
  Fix 3 (no bare generic type) is done: a generic type written without its
  arguments is KEXET-0109/0111, as a wrong count is (#648), for imported
  types too (`ImportedInterfaces::typeArities`). A `make Measure` target still
  means every instantiation (`spec/error_bare_generic_type.kex`). `scaled`,
  `scale` and the conversions answer `This`. `^` changed the dimension, so
  (decided 2026-11-23) it is gone: the library declares `squared` (an `Area`)
  and `cubed` (a `Volume`, which converts to litres) on a length, and any
  other power goes through `Units.Dynamic`'s `pow`.
- A list literal with an expected type (a typed `let`, a declared result)
  checks each element against the element type, with the error at the
  element (decision 3).
- An integer literal is a Float where a Float is wanted (decision 2): `let x:
  Float = 1`, fields, defaults, arguments after overload selection, declared
  results, list elements. The checker records it (`noteFloatLiteral`, only
  where the target is final) and both backends emit a float
  (`hir::Program::literalIsFloat`).
- Capability requirements come from the checker's decided calls (§9 fixes 1
  and 2): every function, a `make` block's too, keyed by its definition, and
  each call followed to the clause the checker selected. A nested capability
  module is named qualified (`FS.File`). KexI exports and methods carry what a
  caller needs (`requiredCapabilities`, read back onto the imported
  `Signature`), and the stdlib read from source marks its capability modules'
  functions. All five rows of the §9 table now hover their requirement, and an inlay
  hint at the end of each function's head shows it without hovering
  (`textDocument/inlayHint`, fix 3).
- A conformance or function of `make Range<Integer>` holds for that
  instantiation only (it registered under the bare name before), and
  `make A is Discrete => Range<A>` (#651's bounded `make`) holds where `A`
  is Discrete: its functions (KEXET-0511) and conformances, through KexI,
  the interface cache and the source-built stdlib too. The stdlib declares
  ranges that way, so `floatRangeFunctions` and KEXET-0502 are deleted: a
  Float range has `contains?` and is not Enumerable because the library
  says so.
- An overload of a name another module declares belongs to a module that
  declares one of its types (KEXET-0510, §8.1 fix 2).
- #651's bounds on functions: `let larger(A is Comparable => a: A, b: A)` and
  `larger : A is Comparable => A -> A -> A`. Each bounded letter is one bound
  placeholder per clause, so inside the body it is the trait and at a call
  it binds to the argument (`larger(3, 7)` is an Integer). `is` is
  contextual: a bound only after a single capital and before `=>`, in the
  parser, the Vim and VS Code grammars, and LSP hover on the bound's trait.
  The `is?` type test (`value.is?(Discrete)`) followed; the highlighters
  leave `is?` an ordinary name.
- Higher-kinded traits, the first slice of §8.2 and #342: `TypeApp` (a type
  variable applied, `F<A>`), `applicationOf` (`[X]`, `X?`, `Map<K, V>` and
  `Box<A>` read as a head applied to arguments) and `applyConstructor`. A
  trait parameter writes its kind, `trait Functor<F<_>>`; a requirement on
  `F<A>` is a function on any `F<…>` its bound allows; `F is Functor =>`
  binds F to the caller's constructor (`spec/higher_kinded_traits.kex`).
  Type lambdas name the varying argument, `make Tagged<_, E>`. A bound's
  kind is checked: `F<A, B>` under `F<_>` is KEXET-0109. HKT signatures,
  type-lambda holes and trait kinds cross KexI and the interface cache, so it
  all works through separate compilation. `This<B>` in a make block (#651
  question 3) is its target applied again: a whole target takes new
  arguments, a type lambda has its holes filled (`spec/this_applied.kex`).
  The stdlib declares `Functor` and `Bindable` (`algebra.kex`), implemented
  by List, Optional and `Result<_, E>` (a block of its own, since `_` leaves
  the element unnamed: a conformance may now be met by functions another
  block of the type defines). Their requirements are candidates only for a
  receiver whose head is still a variable, so `[1].map` stays the list's own,
  and such a call is dispatched by the value through the prelude's
  dispatcher (`spec/stdlib_functor.kex`).
- §12 step 7, first part: a call through a bounded type variable
  (`A.identity` under `A is Monoid`), which has no value to dispatch on. The
  function is passed its types (type-passing: a type's atom serves as the
  dictionary, since dispatchers take it as a receiver). Only functions that
  need it get the hidden parameters: a body calling through a letter, or
  handing its letter to such a function. The checker records each call's
  types (`TypeArgument`), KexI exports a function's type parameters, the
  walker binds letters per call, and BEAM appends the atoms to the call
  (`spec/bound_type_passing.kex`). A requirement's `This` on a bounded
  receiver is now that receiver (`a.combine(b)` on an A is an A).
  A direct `String.identity` on BEAM goes to the prelude's function given the
  type's atom too (it failed in 0.4.0-rc.3).
  Second part: the types and the capability context are one kind of
  implicit argument, in one order for a definition and a call (explicit,
  dictionaries, types, context), appended at the central call builders. A
  foul bounded function takes both, and a `with` substitution still reaches
  it (`spec/bound_type_passing_capability.kex`); the types going after the
  context was a real mismatch the shared order fixed. The context still has
  its own gate (`threadsCapabilities`) and its own variable, but a
  definition builds one `ImplicitParameter` list (dictionaries, types,
  context) that both its arity and its clauses' parameters come from.
- Effects through signatures and data (decision 6, option a, #142): see
  decision 6. A program's own record or sum type now wins over an imported
  alias of the same bare name (`record Handler` beside Net.HTTP's alias).

- The coverage test (§10): every syntax form checks every child it has.
  `spec/check_every_child.kex` puts an ill-typed expression in each child
  slot of each form (calls, operators, literals, ranges, every conditional
  and loop, `match` scrutinee, arms and guards, bindings, assignments,
  lambdas, the `&.` shorthand, interpolation, `spawn`, `receive`, curry,
  `trying`/`rescue`, `return`, parameter defaults), and each is reported
  once.
- Rigid signature variables (#644, §4.1): `let same(a: A) -> A = 1` and a
  body answering another letter are KEXET-0127/0123. A body nobody calls is
  checked with fresh variables for its parameters, and a call on one whose
  candidates all answer one type has that type, so `let g(x) = x.length +
  "a"` is an error with no caller (decision 1).
- Decision 1's per-call check (`checkUnannotatedCalls`): an unannotated
  top-level function is checked again, on a copy, for each distinct set of
  settled argument types a call passes, the calls its body makes in turn.
  Each error is reported once, at the body, with a note naming the call
  (`spec/error_unannotated_per_call.kex`). `let shout(x) = x.upperCase + 1`
  is accepted by itself and rejected for `shout("a")` and `shout('c')`. A call
  already rejected where it is written, or whose argument misses a
  parameter the first check settled, is left to that error. The copies'
  answers are dropped, so the backends read the first check's.
  A call's type is that check's body type, computed at the call during
  inference (memoised by argument types, a recursive call left to its
  signature): `shout("hi")` is a String (`spec/error_unannotated_call_result.kex`).
- A trait default is typed: a requirement called on `this` answers its
  required result, and the default's result is what an implementor and a
  trait-typed value get, the pending result while its body is still to come
  (`spec/error_trait_default_result.kex`).
- The two escapes that let a call matching no candidate pass (§11 item 11)
  are deleted: `unreliableCandidates` (every candidate with an unannotated
  parameter) and its receiver-side twin (`anyUnknownParamCandidate` returning
  Unknown). With KexI giving every stdlib function its types, neither fired
  for anything but a mistake (`spec/error_any_not_narrowed.kex`).
- A written `Any` is the top type everywhere (decision 7), not only as a
  receiver: passed to a concrete parameter, bound by a typed `let`, or
  handed to a block (an element of `[Any]`) it must first be narrowed by a
  type pattern; a dot call on one says so (KEXET-0509). The stdlib had one
  such use (JSON's String arm), now narrowed, and the JSON and reflection
  specs narrow what `JSON.parse` and `Type.construct` give back. A call on
  an `Any` reaches its module's dispatcher, so the value decides on BEAM as
  on the walker (`spec/any_interpolation_parity.kex`: an interpolated None
  printed "None" on BEAM). `Type<T>` is a parameterised record: `Type.of(User)`
  is a `Type<User>`, `Type.of(value)` the type of the value as checked
  (`Type.of(Public)` a `Type<Access>`), and `construct`, `variant` and `parse`
  answer `Result<T, String>`; a type found by name at run time (`Type.named`) leaves `T` to
  the call. KexI writes a receiver function's annotation where its position
  holds one of the receiver's letters, which the checked signature cannot
  name.

- Failing closed on `Unknown` (§4.1, §10), first slices. Measured on the
  specs and examples: an unwritten Unknown reached a concrete parameter about
  4,600 times in 168 files, from a few producers. A call on a value not known
  yet now answers what every function it may reach answers when they agree
  on one concrete type and no record has a field of that name: `"${x}"` is a
  String (the largest producer, `showValue`, 187 sites, once every stdlib
  `showValue` declares its `String`). The candidates are those the other
  arguments fit, their letters bound by them, so `x.to(String)` is a
  `String?`. A trait's requirement is a candidate too, the value as its
  receiver: one answering `This` (`combine`) follows the value and leaves the
  call open. Three stdlib `to(String)` declarations now say `String?`. `break` and `next` are `Never` (decision 9). The
  stdlib's intrinsic constants (`Console.GREEN`, `System.BITWIDTH`) declare
  their types. A type the imported sources declare, named as a value, is its
  `Type<X>` (`to(Binary)`), as a program's own already was.
  The ENV capability's functions declare the types their docs state. A
  trait default's `This` parameter is the receiver's type at the call, so
  Comparable's `clamp(low: This, high: This) -> This` is typed
  (`spec/error_trait_default_this_param.kex`).
  `spec/error_unknown_receiver_common_result.kex`. Unknowns left
  in the specs and examples: 987 before, 718 now. Next producers:
  identifiers bound from them, `.to` whose selector reaches the call
  untyped, `.or`, `.get`, `.try`.

§12, where it stands:

- Step 1, the unification core. `unify` exists (`typechecker_unify.cxx`):
  structural, with an occurs check, binding inference variables only (a
  generic letter, an unknown and a trait bound match open), never to a type
  still holding an open one, and binding nothing when it fails. A call binds
  what its arguments' variables unify with (only the argument's: the
  callee's own are not the call's to bind), and `fixNestedVariables` is left
  for what a §4.5 rule accepted. A value flowing into a declared type (a
  typed `let`, a declared constant, an assignment) unifies with it once
  accepted, so what it leaves open takes the declared type. The other
  `argMatchesParam` sites (about 90) rank overload candidates, which a
  predicate must do without binding; what is left is moving their
  acceptance itself onto `unify` plus §4.5's rules, so there is one relation.
  Decided (2026-11-24): incrementally, each of `argMatchesParam`'s
  acceptances mapped to a §4.5 rule or removed. Done so far: the escape that
  took any two sized integers (or floats) as one type is gone; a literal
  adapts to a sized number (rule 2), a sized number widens without loss (to
  `Integer`, or a wider float), and nothing narrows silently
  (`spec/error_sized_number_narrowing.kex`). Unknowns left in the specs and
  examples: 479 (from 987) counted at any visit; counted by each
  expression's final type, leaving out module-path segments (`Utils.Strings`
  in `Utils.Strings.shout`), 311, and 292 after annotating Comparable's
  `max`/`min` and Foldable's `all?`, `any?`, `none?`, `one?`, `count`, and
  binding a trait function's `This` in the dotless form too (`clamp(15, 0,
  10)` is `15.clamp(0, 10)`; numbers join across the tower, so `max(1, 2.5)`
  is `Integer | Float`). A type's declared-only functions reach the
  checker too (Map's `find :>` and `map :>`, whose bodies are trait
  defaults): the KexI reader dropped every function without a body, and now
  drops only a trait's requirements, which the trait itself carries. What remains is a long
  tail: intrinsics and module namespaces typed unknown on purpose, calls
  whose candidates answer different types (`upperCase` on a String or a
  Char), functions of other files reached by name, and what those feed. The
  last `argMatchesParam` escape (a non-written Unknown matches anything)
  goes once these are typed.
  Tried and reverted: §4.5 rule 4's union of the members' results for a
  call on an unknown receiver. In an unannotated body it reports, at the
  definition, errors that hold only for some arguments (`shout(s) =
  s.upperCase` would be `String | Char`), which decision 1 forbids; such a
  call stays open and is typed per call instead. Decided (2026-11-24):
  decision 1 wins, and rule 4 now says so; these calls are open by design,
  not a hole. With that, every acceptance of `argMatchesParam` maps to a
  §4.5 rule, the non-written-Unknown one to decision 1's open calls and to
  what is unknown on purpose (intrinsics, module namespaces).
- Step 2, #342's grammar. Effect sets inside function types (§8.3, decided
  2026-11-24: inferred, shown only) are built: a block's `FuncType` carries
  the capabilities its body requires, computed with the requirements, and
  hover on what holds it shows them (`requires FS.File`); the definer
  already requires them, so no obligation is added. Inference variables and a
  signature's generic letters are two kinds: a `TypeVar` carries its
  `TypeVarKind`, made only by `Type::genericLetter` or
  `Type::inferenceVariable`, and `isGenericLetter` / `isInferenceVariable`
  read that kind. Their ids share one space so maps keyed by id stay
  unambiguous; an id kept bare (a serialized interface,
  `ConstrainedType::genericId`) is decoded in one place, `typeVarFromId`.

The long tail, where it ended: of the 292, what stays untyped is open by
design: foreign `BEAM.*` calls, `Kex.Intrinsic.*`, what follows an error in
an `error_*` spec, calls decision 1 keeps open (`shout(s) = s.upperCase`),
and what an `Any`-returning helper feeds (`opaque(x).name`). Calls into
other files by name are typed (their exports are annotated). Closed on the
way:

- A type's declared-only functions reach the checker (Map's `find :>` and
  `map :>`, whose bodies are trait defaults). The KexI reader dropped every
  function without a body; it now drops only a trait's requirements, which
  the trait itself carries (as functions too, Showable's `showValue :>
  String` stood beside the prelude's `showValue(value: Showable)` and made
  every interpolation ambiguous).
- Trait parameters (decided 2026-11-24, over an associated type and over
  `Foldable<F<_>>`, which String and Map's entries do not fit): `trait
  Foldable<T>`, claimed as `implement: Foldable<Char>`, `Foldable<X>`,
  `Foldable<(K, V)>`. A default written with the parameter answers in what
  the claim filled in (`"hey".find` is a `Char?`); each implementor's copy
  of the default reads its letters with the claim, and keeps what the
  clause writes when the body is checked. KexI carries it as a type-only
  function per implementor (`spec/trait_parameter_claim.kex`,
  `spec/error_trait_parameter_claim.kex`). A requirement written with its
  receiver (`fold : F<T> -> ...` in a higher-kinded trait) no longer counts
  that receiver twice.

The `is?` type test (decided 2026-11-24: now): `value.is?(T)` is a Bool,
whether the value is a `T` or, for a trait, of a type that implements it.
The checker decides it as an intrinsic (`CallKind::TypeTest`): a type tests
itself, a trait each type a `make` claims it for (`is?(Discrete)` is
`Integer, Char`), `Number` itself, and a structural trait (`Showable`) is
refused (KEXET-0516), as is a value where the type goes (KEXET-0515). Both
backends test as a type pattern does (`runtimeTypeMatches`, `typeGuard`).
It narrows nothing; a `match` with `x : T` does (`spec/is_type_test.kex`,
`spec/error_is_type_test.kex`).

After it (2026-11-24):

- Kind inference (§8.2): a trait parameter written without holes takes the
  kind its signatures apply it with (`trait Mappable<F>` with `F<A>` is
  `Mappable<F<_>>`), read by the parser so every consumer sees one kind;
  uses that disagree are KEXEP-0159 (`spec/trait_kind_inference.kex`).
- The Monad trait: `trait Monad<F>` with `wrap : A -> F<A>` and `flatMap`,
  implemented by List, Optional and `Result<_, E>`. `wrap`, which has no
  receiver, is called on the type (decided 2026-11-24): `List.wrap(1)` by
  name, `M.wrap(x)` through a bounded letter, M passed to the function as
  its type (§12 step 7, now for constructors too: `xs: M<Integer>` given a
  `[1]` passes List). Dispatchers of ADT receivers take the type's atom as a
  record's and a primitive's do. A function whose arguments do not say what
  M is cannot build one: the result stays `M<...>` (`spec/monad_trait.kex`,
  `spec/error_monad_trait.kex`).
- Clauses selected by a literal: a clause that writes its result and takes a
  literal Integer (`let ^(2) -> Measure<Quantity.Area>`) is chosen only by
  that literal, so `^` is back on lengths, overloaded on its exponent as `*`
  and `/` are on dimensions (`spec/measure_power_overload.kex`). A literal
  is its own type there (`PrimitiveType::literal`, which atoms already
  used); clauses that leave the result to the body keep their literals
  Integers (`fib(0) = 0`).
- Applicable (named so, decided 2026-11-24; its operation `with`, over
  `map2`): `trait Applicable<F>` with `wrap` and `with : F<A> -> F<B> -> (A
  -> B -> C) -> F<C>`, implemented by List (every pair), Optional, Result
  (the first failure) and `Validation<T, E> = Valid(T) | Invalid([E])`
  (every failure; Applicable but not a Monad). `traverse` and `sequence` in
  algebra.kex are written once over it. Passing a bounded letter's type now
  also reads it from the call's result (`traverse`'s `F<[B]>`, where only a
  block names F), the walker reads a one-letter receiver from its type frame
  in a prelude function too, a refused call no longer answers its
  unbound letters, the source-built prelude interface carries a claim's
  filled defaults as KexI does, and a block through `M<A>` takes its hints
  from the higher-kinded requirements (`spec/applicable_with.kex`,
  `spec/validation_signup.kex`, `spec/traverse_sequence.kex`, LSP and REPL
  tests).
- `into`, the monadic bind (decided 2026-11-24), is a plain function:
  `xs.into { |x| ... }` is `flatMap`, on List, Optional and Result (`spec/monad_into.kex`). It is not yet a `Monad`
  default: a default over a higher-kinded `F` does not tie `F` to its
  receiver. `.try` on Result and Optional is the checker's own
  (`tryPayload`): there is no Tryable trait.

Nothing in the plan is left undone.

---

## 1. Summary

Kex's type system was designed as **Hindley–Milner**: constraint-based
inference by unification, let-polymorphism and principal types, the design of
Haskell and OCaml. The implementation drifted from that design. It now infers
a type and then compares it, site by site, through escapes that accept whatever
a site does not know. It is fixed one hole at a time, and the holes keep
coming.

The plan:

1. Write the HM design down (§4) and bring the checker's core back to it.
2. Contain everything that does not fit HM in eight named rules, listed in one place (§4.5).
3. Remove type-name special cases from the compiler, leaving one registry of
   the dozen names the syntax needs (§7).
4. Keep `Measure` dimensions a library: products are overloads by type, and
   the language needs only general overload fixes (§8.1).
5. Build kinds on the core for higher-kinded traits and effect sets (§8.2,
   §8.3).
6. Infer capability requirements from the checker's answers and show them in
   the LSP, never in the syntax (§9).

---

## 2. Why point fixes do not converge

### 2.1 Evidence

Issues labelled `Typechecker`, by month opened:

| Month | Opened |
|---|---|
| 2026-08 | 21 |
| 2026-09 | 16 |
| 2026-10 | 21 |
| 2026-11 (to the 23rd) | 49 |

107 in total, 29 open. Since October, 167 commits touched
`src/semantic/typechecker*.cxx`, and 77 of them name a hole, a wrong accept or
reject, or a fix. #649's first batch of local fixes is closed, yet a round of 78
probe programs found ten new holes (§11).

A caution: the November rise coincides with fuzzing, metamorphic search and
probe rounds, so part of it measures more searching rather than a worse
checker. The structural argument below does not rest on the counts.

Fixes land on the repro rather than the class. #642 is closed and its repro is
rejected, but annotating the implementation's own return type gets the same
program through:

```kex
trait Shape do
  area :> Float
end

record Square do
  side : Float
end

make Square, implement: Shape do
  let area -> String = "not a number"   # accepted: the annotation overrides the requirement
end
```

#793 (a capability stand-in with fewer parameters) is the same class.

### 2.2 How the checker is built

An HM checker has three properties this implementation lost:

1. **One typing rule per syntax form, constraining every child.** Here a form
   infers its children and compares where someone wrote a comparison. Where
   nobody did, nothing is checked: `when` guards (`typechecker_expr.cxx:2842`,
   `:2898`) and `elif` conditions (around `:2757`) call `inferExpr` and drop the
   result.
2. **An expected type that flows down.** In bidirectional checking, `check(e, T)`
   pushes `T` into `e`. Here there are 74 `inferExpr` calls, no check-mode entry
   point, and `m_branchExpectation` as a partial stand-in.
3. **One way types are related.** In HM that is unification. Here there are 103
   `argMatchesParam` call sites, often tried in both directions, plus 56
   "permissive" flags and 69 `UnknownType` checks that switch a comparison off.
   Each site has its own version of numeric promotion, literal coercion and
   optional wrapping, and every new feature (Measure, capabilities, atom literal
   types, distinct types) has to be taught to every site.

The escapes **fail open**: a site that does not know a case accepts it, so gaps
surface as programs that wrongly pass. Two consequences:

- `VoidType` is the bottom type `Never` (`types.hxx:97-106`; it prints as
  `Never`), and an `if` with no `else` is typed with it
  (`typechecker_expr.cxx:2818`), so it fits any declared result (#809).
- Domain knowledge sits in the general rules: the declared-return check looks
  for `:new`/`:state`/`:stop` keys in map literals to recognise serving
  transitions (`typechecker.cxx`, `servingTransitionMatches`).

#342 describes the representation side: `semantic::Type` is a flat variant
grown one case per feature.

---

## 3. How the HM design was lost

The design was written down once, in `docs/type-system-plan.md` (1139 lines),
and that file was deleted on 2026-06-26 as a "done plan" (3bbea2f9). Recover it
with `git show 3bbea2f9^:docs/type-system-plan.md`. It said:

> User-defined functions: each `FunctionDef` clause's param/return types are
> inferred (Hindley-Milner style: assign type vars, unify across clauses and
> call sites) rather than required as annotations

> 2. Infer each argument's type, unify against the corresponding param type
> (respecting `Trait` constraints, not just nominal equality).

The checker was built in five phases on 2026-06-25, and the drift happened
inside them:

1. **Phase 4 (bd047b14) checked stdlib calls by comparison, not unification.**
   It introduced `argMatchesParam`, whose first lines let `UnknownType` and
   `TypeVar` pass everything, because user-defined functions were "not checked
   yet (phase 5)". That stub grew into 103 call sites.
2. **Unification came afterwards, as a side mechanism.** Phase 5a added
   `resolve`/`unifyVar` to push constraints from operators and unique overload
   matches into parameter type variables. It sat beside `argMatchesParam`
   rather than replacing it.
3. **Generalisation was never built.** The plan's own phase 5 status note says
   "first-writer-wins monomorphic unification … the generalization half remains
   below as the future 5a-gen sub-phase". It lists 5a-gen (generalisation and
   instantiation), 5b (call-graph SCCs) and 5c (overload tie-break) as **not**
   implemented. Call-site re-checking (`examples/typesystem_audit/03`) later
   stood in for let-polymorphism.
4. **The plan was deleted as done** one day later, with phase 5 marked "SCOPED,
   PARTIALLY DONE". From then on, nothing an implementor read said "HM".
5. **Every later feature extended the comparison sites.** Rows, intersections,
   the numeric tower, distinct types, atom literal types, `Measure` and
   capabilities all did. Counts on `main`:

| Date | `argMatchesParam(` | `UnknownType` checks | "permissive" | `typechecker*.cxx` lines |
|---|---|---|---|---|
| 2026-06-30 | 31 | 29 | 33 | 2,117 |
| 2026-08-31 | 67 | 65 | 50 | 7,674 |
| 2026-10-31 | 79 | 76 | 59 | 11,122 |
| 2026-11-23 | 104 | 69 | 56 | 19,047 |

Unification calls went from 19 to 27 over the same period.

The deleted plan predicted the failure. Its "Sanity check / risks" section
warns that the three "don't fully know the type" representations "must not
collapse into one permissive bypass", and that `typesEqual`, trait
satisfaction and numeric widening "are three different relations". Both
happened.

Two of its decisions differ from proposals here, and should be weighed, not
silently overridden (§6.3):

- `Float` was designed as an umbrella trait over `Float32`/`Float64`.
- `Int` was designed as an alias for `Int64`. Today `Int` means `Integer`,
  while `Int64` prints as `Int`.

---

## 4. Target: Hindley–Milner with traits

The design the new core implements. Anything that does not fit it is a
decision to raise on #808 first, not a special case to add.

### 4.1 The core is HM

- Every expression generates constraints, and **unification** solves them.
  Unification is the only way two types are related. It replaces the
  `argMatchesParam` sites, the "permissive" flags and the `UnknownType` escapes.
- **Annotated functions** get one type scheme, checked once. The exception is a
  bare trait result with several parameters of that trait, which is open
  (§4.5, rule 7).
- **Unannotated functions are checked at each call site** (decision 1, §5).
  Re-checking a `let`-bound function at each use is a standard formulation of
  let-polymorphism: checking `let f = e in body` is equivalent to checking
  `body` with `e` substituted.
- A type variable in a **written signature is rigid** (#644): `let same(a: A) ->
  A` cannot be specialised by its body.
- An unsolved variable is either generalised (in an annotated signature) or an
  error. It is never a silent "accept", and there is no defaulting (decision 2).
- Mutable `var`s are monomorphic (the value restriction, as in ML).
- The textbook formulation to follow is HM(X) / OutsideIn-style constraint
  generation and solving.

### 4.2 Traits are type classes

Qualified types, after Jones:

- A bound such as `Comparable` on a type variable is a constraint, carried in
  the type scheme and discharged by instance resolution.
- `make … Box<A>, implement: Pretty` with a condition on `A` is an instance with
  a context (#651).
- Instances are coherent, with overlap and orphan rules, because separate
  compilation depends on it.
- Lowering passes dictionaries (#651).
- What a bare trait name means in each position is decision 4 and decision 5
  (§5).

### 4.3 Bidirectional checking is layered on HM

As in GHC and OCaml, check mode does not replace inference:

- `checkExpr(e, expected)` pushes an annotation or a known expected type down,
  into lambdas, branches, literals and arguments.
- Errors are then reported at the expression that is wrong, not at the end of a
  chain.
- Literals and the joins see their target (§4.5).
- Inference stays unification. Check mode only decides where constraints are
  generated and where errors are reported.
- Every syntax form has one rule, and every child is either checked against a
  type or inferred into a constraint. Guards and conditions are checked against
  `Bool`, bodies against their declared result types, and arguments against
  parameter types.

### 4.4 Extensions

Each is a known extension of HM that keeps principal types:

| Feature | Extension | Reference |
|---|---|---|
| Record rows (`spec/row_polymorphism.kex`) | row-polymorphic unification | Rémy; Leijen |
| Higher-kinded traits | kinds and kind inference; type variables in head position | Jones' constructor classes; Haskell 98 |
| Inferred capability sets | effect rows, unified like record rows | Koka (Leijen) |

### 4.5 What does not fit HM, and how it is contained

This is the **complete** list of rules that go beyond plain HM unification.
Each is one rule in one place. This section is normative: §5 gives the
evidence and the reasoning for each decision, and where the two differ, this
section wins. A new rule is added here first, or it does not exist.

1. **Numeric promotion.** `Integer` op `Float` gives `Float`, as one table in
   the operator rule. It is resolved when both operand types are known, and
   deferred as a constraint until then. It is never a subtype relation.
2. **Literals.** A literal takes the type expected of it: a declared type
   (`let b: UInt8 = 255`), a distinct type (`foul first() -> BookId = 1`), or
   the other operand of an arithmetic operator (`x + 1` with `x: Float`).
   Unconstrained, `1` is `Integer` and `2.5` is `Float` (decision 2).
3. **The join** (decision 3, #415) for branches and list elements: equal types
   stay, sibling constructors join into their ADT, a branch that never returns
   drops out, and different types join into their **union**. The compiler never
   picks a trait for a join. A join happens only where no type is expected,
   and only over types already known (rule 6 makes sure of that).
   **Unification never forms a union.** It may bind a type variable to a union
   that already exists: `twice(x)` with `x : Integer | Float`, or `map` on an
   `[Integer | String]`, binds the variable to that union. What it never does
   is make one by joining two types, when `A` meets `Integer` in one place and
   `String` in another. That is a mismatch, as in plain HM, and only rule 3
   makes unions.
4. **Overload sets.** Kex chooses functions by type: same-name functions with
   different parameter types, functions on a receiver (`x.label` on `Dog` or on
   `Robot`), and `to(T)`. Every overload declares what it takes and returns, so
   an overloaded name has a fully known type: the **intersection** of its
   signatures, `(Integer -> String) & (String -> String)`.
   - A call with a known argument type resolves to the one matching member, and
     its result is that member's declared result.
   - A call whose argument is still unknown constrains it to the **union** of
     the members' parameter types. Its result is what every matching member
     answers when they agree; when they do not, the call stays open in the
     definition and is typed at each call with that call's types (decision 1,
     which wins here, decided 2026-11-24). A receiver whose type is a union
     already gets the union of the members' results.
   - A type variable is never solved by picking one member, which was #643's
     bug.

   This is what the checker already does: with `describe` overloaded on
   `Integer` and `String`, `let show(x) = describe(x)` is typed
   `Integer | String -> String`, and `show(2.5)` is rejected. `let tag(x) =
   x.label` accepts a `Dog` and a `Robot` and runs each correctly. Overloads
   stay overloads. They are not turned into type classes.
5. **Intersections** are kept, as the type of overload sets. Whether a program
   may also write intersection types itself is part of #342's type grammar.
6. **Unannotated functions are checked per call** (decision 1). They have no
   type scheme of their own. Each call checks the body with the argument types
   it receives, memoised by those types. This is the substitution formulation
   of let-polymorphism, not generalisation, and it gives up principal types for
   unannotated code.
7. **A bare trait as a result type follows the parameters** (decision 4). With
   one parameter of that trait, the result is that parameter's type, which is
   ordinary HM with an implicit type variable. With several parameters it would
   be a type function of the arguments (`join(A, B)`), which HM has no place
   for. That case is an open decision (§14).
8. **Trait types are existentials** (decision 5). A value of type `Shape` or
   `Number` carries its implementation, and calls on it dispatch at run time.
   A value becomes one only where a trait type is **written**: a parameter, a
   typed `let`, a return type, an element type (`let xs: [Shape] = …`). A join
   never makes one (rule 3), and unification never does.

Anything else that passes today through `argMatchesParam`'s directional checks
(optional wrapping, `Any`, matching tolerant of unknowns) has to be justified as
one of these eight or become an error.

---

## 5. Decisions

The evidence and reasoning behind the rules in §4.5. Each was checked against
today's compiler before deciding. The rules themselves live in §4.5, and this
section does not restate them as a second specification.

### Decision 1: unannotated functions are checked at each call site

```kex
let f(c: Bool, x) = c then x else 1

f(true, 5)      # Integer
f(true, "s")    # String | Integer
```

- A branch whose type depends on an unannotated parameter is joined within each
  call's check, once the argument's type is known. No type scheme ever carries a
  pending join.
- Overload sets (§4.5, 4) rely on the same property.
- **Recursion:** a recursive unannotated function is checked monomorphically
  within its own body, or needs an annotation.
- **Module boundaries:** an exported function is annotated, so KexI carries its
  signature rather than its body. This matches the style rule that single-body
  functions carry inline types.
- **Hover** on an unannotated function shows its type at the call under the
  cursor.

Costs that the rule must handle, measured on today's compiler:

- **A function nobody calls must still be checked.** Today `let g(x) =
  x.upperCase + 1` with no caller gives "No errors found". Every body is
  checked at least once with fresh variables for its parameters, so an error
  that holds for every argument type (here, `String + Integer`) is reported at
  the definition.
- **Errors are reported once per definition.** Today a wrong body called twice
  reports the same error twice. Errors from per-call checks are deduplicated by
  location, listing the calls that triggered them.
- **Memoisation.** Nested unannotated calls re-check multiplicatively, the
  template-instantiation problem. Each check is memoised by the tuple of
  argument types.
- **Tooling has no type to show at the definition.** `kex -s`, KexI and hover
  at the definition need a display. Show the constraints the body puts on its
  parameters (`x: has upperCase`), or ask for an annotation.
- **"Exported functions are annotated" breaks existing code.** How much has not
  been counted. That count is needed before the rule is adopted (§14).

### Decision 2: number literals adapt only where a type is expected

- They are not polymorphic in the Haskell sense: no defaulting rules, no
  "ambiguous type variable" errors.
- Unconstrained, `1` is `Integer` and `2.5` is `Float`.
- A literal takes the expected type, including from the other operand of an
  arithmetic operator.
- `let b: UInt8 = 255` works today. `let x: Float = 1` is rejected today ("expected
  Float, got Integer") and becomes accepted.

### Decision 3: the join

One rule for `if`/`elif`/`else`, `<cond> then <a> else <b>`, `match`, list
elements, and a map's keys and values (`{ "ada": 36, "bob": "old" }` is a
`{String: Integer | String}`; maps widened to `Any` before, #771). An
atom-keyed map still takes its first value's type until #200, because serving
transitions are read as named fields (decision 7):

| Joined | Result |
|---|---|
| equal types | that type |
| sibling constructors (`Ok`/`Error`) | their ADT (`Result<…>`) |
| a branch that never returns | drops out |
| different types | their **union** |

```kex
[Circle {…}, Square {…}]           # [Circle | Square]
c then Circle {…} else Square {…}  # Circle | Square
[1, 2.0]                           # [Integer | Float]
c then n else x                    # Integer | Float, for n: Integer, x: Float
[1, "a"]                           # [Integer | String]
let shapes: [Shape] = [Circle {…}, Square {…}]   # [Shape]: a trait type only when written
```

- **The compiler never picks a trait for a join.** An earlier version of this
  decision joined types that share a trait to that trait (`[Circle, Square]` to
  `[Shape]`, `[1, 2.0]` to `[Number]`). Types share more traits than that
  suggests: `Integer` and `String` are both `Blankable`, `Truthyable` and
  `Monoid`, so it needed a list of traits that must never be picked, or new
  syntax to mark them. It also made a mixed list an error, at a distance, once
  someone added an `implement:` that gave its types a second shared trait.
  Unions have none of those problems.
- **A union loses nothing.** A union has a trait when every member has it
  (`TraitRegistry::satisfies`), so a shared function works on it: `s.area` on a
  `Circle | Square` checks, and runs on both backends today. `Integer | Float` is
  a `Number` wherever one is needed. Anything only one member has is reached
  through a `match` with type patterns (`c: Circle => …`), as written unions
  already are (`type Key = Integer | String` works today).
- **Where a type is expected, nothing is joined.** Each branch or element is
  checked against it, so `let t(c: Bool) -> String = c then 1 else "a"` reports
  the `1`, and `let shapes: [Shape] = [Circle {…}, Square {…}]` checks each
  element against `Shape`. A list literal takes the element type it is expected
  to have, so `sum([1, 2.5])` against `[Integer | Float]` is accepted (today it
  is rejected: "list elements must be the same type").
- **A join only sees known types.** Rule 6 checks an unannotated function per
  call, so `let f(c, x) = c then x else 1` joins `x`'s actual type at each call.
  No union is ever formed over an unsolved variable, and unification never
  creates one, so the HM core is untouched.
- **Promotion stays an operator rule.** `n + x` is `Float`. A branch join gives
  `Integer | Float` and does not promote.
- **Proposed, not yet decided (§14): a union of types the run time cannot tell
  apart is an error.** A union is used by telling its members apart at run time,
  in a `match` with type patterns or a call dispatched on the value. That works
  for records and ADTs, which are tagged, and for the primitives. It fails for a
  distinct type over a primitive: in `BookId | Integer`, both are integers at
  run time. Unions are inferred, so this matters more for them than for written
  trait types (decision 5), which have the same problem.
- **The cost.** In unannotated code, a branch that produces the wrong type is
  reported where the union is used, not at the branch. Plain HM (branches must
  agree) would report it at the branch. Annotating the result moves the error
  back to the branch.

Today's behaviour, which this replaces:

| Program | Today |
|---|---|
| `if c … 1 elif d … "a" else :b end` | error KEXET-0444 |
| `match n do 0 => 1; _ => "many" end` | error KEXET-0446 |
| `c then 1 else "a"` | error KEXET-0459 |
| `c then n else x`, `n: Integer, x: Number` | accepted as **`Integer`** |
| the same with branches swapped | accepted as **`Number`**: the result depends on branch order |
| `let xs: [Number] = [1, 2.0]` | error: "list elements must be the same type" |
| `[Circle {…}, Square {…}]` | accepted, inferred `[Shape]` |

The element join is `TraitRegistry::commonTrait` (`traits.cxx`). It returns
the *first* shared trait in iteration order, excludes a hard-coded list of
trait names (`kBuiltin`), and `joinElementType` falls back to `Unknown` when
nothing matches. That guess also gets in the way: a `[Circle {…}, Square {…}]`
literal passed to a `[Circle | Square]` parameter is rejected today, because it
was already guessed to be `[Shape]`. Under this decision `commonTrait` and its
list are deleted, not replaced. The block `if`, the inline conditional and
`match` are three separate pieces of code that disagree, and become one join.

### Decision 4: a bare trait as a result type is the parameters' type

```kex
let twice(x: Number) -> Number = x * 2
twice(3).even?      # Integer (today: "`even?` expects Integer, but got Number")
twice(2.5).round    # Float

let add(a: Number, b: Number) -> Number = a + b
add(1, 2)           # Integer
add(1, 2.5)         # open: see below
```

- **Decided:** with one parameter of that trait, the result is that parameter's
  type.
- **Open (§14):** with several parameters (`add` above). Making the result the
  join of what they received is a type function, `add : A -> B -> join(A, B)`.
  Checking the body once against it means proving that `promote(A, B)` fits
  `join(A, B)` for rigid `A` and `B`, which this plan does not specify. That
  contradicts rule 6's "annotated functions are checked once". Options:
  - a. Several parameters with the same bare trait and a bare trait result
    must name the type, and a bare result is an error saying so.
  - b. Such signatures are checked per call, like unannotated functions.
  - c. Specify the symbolic rule for promotion against the join.
- **A surprise to document:** `let avg(x: Number) -> Number = x / 2.0` is an
  error for `avg(3)`, because the body gives `Float` while the result must be
  `Integer`. Such a function is written `-> Float`.
- The trait may sit at any depth of a parameter. On `make [Number]`,
  `sum :> Number` returns the element type, so `[Integer].sum` is `Integer`
  with no new syntax.
- No type-variable syntax is needed for these cases. An opaque "some number the
  callee chooses" is no longer something a signature can say.

### Decision 5: a trait is a type

- A trait type is a type you **write**: `let xs: [Shape] = …`, `x: [Number]`.
  A list of `Shape`s is homogeneous in `Shape`, as `[Integer]` is in `Integer`,
  so §23.6's "collections stay homogeneous" holds. The compiler never infers a
  trait type (decision 3): an unannotated `[1, 2.0]` is `[Integer | Float]`, and
  §23.6's wording changes so that it is no longer an error.
- A value of a trait type carries its implementation (an existential, or trait
  object). Calls on it dispatch at run time through that implementation. That
  is the one place the backends do not follow a checker-chosen implementation.
- A trait type satisfies its own trait (like `dyn Trait: Trait` in Rust). A
  `Number` value can be passed where `x: Number` is expected, and decision 4
  then gives `Number` back.
- A bare trait as a parameter's whole type (`x: Shape`) is generic: each
  occurrence is its own type, fixed by the call. A trait inside a type
  (`[Shape]`, `Shape?`) is the trait type. #651's syntax rules should state both.
- Today `let xs: [Number] = [1, 2.0]` is rejected ("list elements must be the
  same type") while mixed record lists are accepted. With the expected type
  pushed into the literal (decision 3), both are accepted.
- **Open (§14, question 4): types the run time cannot tell apart.** Dispatch on
  a `[Shape]` element works from the value's own run-time type, which fails for
  a distinct type over a primitive (a `BookId` is an `Integer` at run time). It
  is the same question as for unions (decision 3), and is decided with it.

### Decision 6: effects are inferred through written signatures

- Every function type in a written signature gets a hidden effect variable. The
  stdlib's `map : [A] -> (A -> B) -> [B]` is pure when its block is pure and
  `foul` when its block is, with the text unchanged.
- A `foul` block passed through a pure `runTwice(f: Block<Void>)` is rejected.
  Today it is accepted and runs the effect twice. This closes #142 **for
  function values passed through signatures**.
- **Open (§14): effects inside data.** Function values also live in record
  fields. The stdlib's own `Mock.Clock { onNow: Just({ DateTime.now() +
  3.day }) }` stores a `foul` block in one. A hidden effect variable on
  signatures does not reach a field, so a `foul` block can still escape through
  data, and #142 does not fully close. Options:
  - a. A type with a function-typed field gets a hidden effect parameter, as in
    Koka. That means hidden parameters on user types, shown in hover.
  - b. Calling a function read from a field is treated as possibly `foul`, so a
    pure function cannot call one. This is conservative but simple.
  - c. Accept the gap and state that #142 closes for signatures only.
  Decided (2026-11-23): a. Implemented in the analyzer (`checkEffectFlow`):
  a block that runs something foul, a capture of a foul function, and a
  record, list or tuple built with one carry a foul effect, the record's
  hidden effect parameter instantiated by its value. A function that calls a
  parameter, or a block read out of one, is effect-transparent in it, through
  other functions too, and hover says `foul when 'h' carries an effect`. A
  guard may not hand an effect to such a parameter (KEXEF-0010), nor contain a
  foul block or capture at all (KEXEF-0009), since what it hands one to may
  run it.
- `foul` stays the written declaration and is checked against the inferred
  effects. A function inferred to have effects but written `let` is an error, as
  today.

### Decision 7: places for today's special cases

- **Serving transitions:** the general fix is #200, a type parameter filled in
  from context, so `Reply<Integer>` inside `serving Counter` knows its state is
  `Counter`. Until then, the `:new`/`:state`/`:stop` recognition is one named
  rule, not part of the general return check.
- **`Any`:** the top type. Anything can be passed as `Any`, and nothing can be
  done with one until it is narrowed by a type pattern, like TypeScript's
  `unknown`. It is never a silent escape (#764).
- **Distinct types** (`BookId`): nominal types. A literal enters one only
  through the literal rule (§4.5, 2).
- **Atom literal types** (`:ok`): singleton types. Different atoms join into a
  union (`:ok | :error`), and are accepted as `Atom` where `Atom` is expected.
- **Sized numbers:** concrete types that implement `Number`. Literals adapt to
  them.

### Decision 8: a conditional without `else` has no value (#809)

- As a statement it is allowed: `if ready? … end`, `if let Just(x) = y … end`,
  `IO.printLine(x) if ready?`.
- Where a value is expected (a declared result, a typed `let`, an argument, an
  operand) it is an error: "`if` without `else` has no value; add an `else`".
- There is no implicit wrapping into `T?`. An optional is written out:
  `n > 5 then Just(n) else None`.
- Both backends produce the same `Void` value. Today the walker yields `None`,
  BEAM yields `:ok`, and the checker types the `if` as `Never`.
- The inline form already requires its `else` (`c then 1` alone is a parse
  error, KEXEP-0063).

### Decision 9: `Void` and `Never` are the language's two types

- `Void` is "produces nothing useful" (`-> Void`, `.each`, `IO.printLine`).
  `Never` is the bottom type of an expression that does not return.
- They never stand in for each other.
- Inside the checker the names are swapped today: `PrimitiveType::Unit` is the
  source's `Void`, and `struct VoidType` is `Never`. They are renamed to match
  the language. No `Unit` type is added.

---

## 6. Numbers

### 6.1 What stays

- `Number` is already a trait inside the checker (`traits.cxx:323`).
- `1 + 2.5`, `0 == 0.0`, ordering across numbers, and integer-literal patterns
  matching floats stay. They are operator and pattern rules, not subtyping.
- The runtime numeric tower (the four backend sites that make `0` and `0.0` one
  number) is untouched.

### 6.2 What `Number` means in each position

| Written | Meaning |
|---|---|
| `x: Number` as a parameter | generic: any number, fixed by the call (decision 5) |
| `-> Number` as a result | the type the `Number` parameters received (decision 4) |
| `[Number]`, `Number?` written as a type | the trait type: values of any number type (decision 5). Never inferred: an unannotated `[1, 2.0]` is `[Integer \| Float]` (decision 3) |

Signatures that change with decision 4, each now returning the caller's type:

- `List.sum` and `List.product`, both forms (`list.kex:37`, `:47`, `:621`, `:763`)
- `Math.abs : Number -> Number` (`math.kex:315`)
- `units.kex:571` (`scaledValue`) and `units/si.kex:295` (`prefixScaled`)
  become `-> Float`. Their comments say `-> Number` only works around
  `Integer / Float` being typed `Integer` (§11, item 8).
- `examples/wc.kex`, `spec/generic_alias.kex`, `spec/number_comparable.kex`,
  `spec/result_annotation_precedence.kex`

The 45 stdlib parameters typed `Number` are unaffected.

### 6.3 `Integer` and `Float` (proposed, to be weighed against the original design)

Today the two are not the same kind of thing:

| Written | Is today | Prints as |
|---|---|---|
| `Integer`, `Int` | the concrete arbitrary-precision type (`types.cxx:213`) | `Integer` |
| `Int64` | a sized type | **`Int`** (`types.cxx:316`), the name that, written, means `Integer` |
| `Float` | a **trait bound** over `Float32`/`Float64` (`types.cxx:244`, `traits.cxx:90`) | `Float` |
| `2.5` | `Float64` | `Float64` |

`Integer` is also a trait name (`traits.cxx:87`), satisfied by every sized
integer. The asymmetry caused #766 (`count * price` on `price: Float` typed
`Integer`, patched in 7eb0d7ac by teaching `isFloat` about the bound) and is
part of why `Integer / Float` is typed `Integer` in some contexts.

Proposal: no name is both a type and a trait.

- `Number` is the only numeric trait.
- `Integer` is the concrete arbitrary-precision type. `Int` stays its alias
  (249 uses).
- `Float` becomes the concrete 64-bit float, the same type as `Float64`. BEAM
  floats are 64-bit, so a trait over `Float32` buys nothing at run time.
  `Float32` stays a concrete sized type for binaries and FFI.
- Sized integers (`Int8` … `UInt64`, `Byte`) are concrete types implementing
  `Number`, and stop satisfying a trait called `Integer`.
- `Int64` prints as `Int64`.
- Promotion is one table over the concrete types, so the special case in
  `isFloat` goes.

The cost is that a `Float32` value no longer passes where `Float` is expected
without a conversion. No stdlib or spec code relies on it beyond the
`Float32`/`Float64` constant modules in `number.kex` and
`spec/numeric_bounds.kex`.

The original design (§3) made `Float` a trait on purpose, and `Int` an alias for
`Int64`. Both choices are to be weighed here, not dropped silently.

---

## 7. No type names in the compiler

### 7.1 Today

Decisions taken by comparing a type's name as a string (`== "Optional"`):

| Where | Comparisons |
|---|---|
| `src/semantic` (checker) | 96 |
| `src/interpreter` (walker) | 86 |
| `src/ir` (lowering) | 12 |
| `src/lsp` | 10 |
| `src/common` | 4 |
| **Total** | **~208** |

By kind:

| Kind | Sites | Names | Replacement |
|---|---|---|---|
| Built-in ADTs | 61 | `Optional`, `Just`, `None`, `Result`, `Ok`, `Error` | Ordinary stdlib declarations. The sugar expands once (§7.2), `.try` goes through `Tryable`, exhaustiveness is the generic ADT check, and the walker builds values through constructor handles from the registry |
| Numeric | 41 | `Integer`, `Float`, `Number`, sized types | Membership in `Number`, read from `implement:` declarations; one promotion table |
| Built-in types | 47 | `Any`, `Void`, `Never`, `Block`, `Tuple`, `Atom`, `String`, `Range`, … | `Void`, `Never`, tuples and functions become kinds in the type grammar (#342). `Range` is a stdlib type (see below) |
| Serving | 22 | `Server`, `Reply`, `New`, `:new`/`:state`/`:stop` | #200; after it, `Reply` is an ordinary record |
| Walker, other | 24 | `This`, file modes, `Regex.Regex`, `Binary`, `Stream`, `Less`/`Greater` | Value kinds as an enum, and stdlib constructors through registry handles |
| Modules | 5 | `Elixir`, `Erlang`, `Prelude` | Registry entries |
| Built-in traits | 4 | `Showable`, `Comparable`, … | Two of the four go with `commonTrait`, which decision 3 deletes; the rest become a "derived for every type" property of the registry |

Other special cases in the checker:

- **Hard-coded name lists:**
  - `kIntNames` and `kFloatNames` (`traits.cxx:72`, `:81`)
  - `kBuiltin` (`traits.cxx`), the traits excluded from joins: deleted with `commonTrait` under decision 3, not replaced
  - (done) `floatRangeFunctions` (`typechecker_expr.cxx:2388`): stdlib *function*
    names allowed on a `Range<Float>`. That is library knowledge written into
    the checker. The stdlib should declare it: `make Range<Integer>,
    implement: Enumerable` rather than on `Range<X>`, so a `Range<Float>` simply
    has no `sum`.
- **18 comparisons against function names**: `new`/`state`/`stop`/`reply`/
  `within` (serving), and `inspect` (`IO.inspect`'s purity exemption).
- **One-rule helpers**: `literalCoercesTo`, `siblingConstructorJoin`,
  `moreKnownBranch`, `deferIfPending`/`answerPending`, and
  `servingTransitionMatches`/`servingCastTransitionMatches`.

The walker's 86 comparisons need sorting before they are counted against it.
Many concern **run-time value representation**: which native value to build,
or how to print one. That is legitimate in an interpreter. The ones that
matter for the single-source-of-truth rule are those where the walker decides
something the checker already answered, such as choosing an implementation or
coercing a value by its static type. Appendix A lists every site, and each has
to be classified as one or the other.

Appendix A lists every site.

### 7.2 What must remain: the syntax's own vocabulary

| Syntax | Needs |
|---|---|
| `T?` | `Optional<T>` (`Just(T) \| None`) |
| `A or B` | `Either<A, B>` (`Left(A) \| Right(B)`) |
| `T or! E` | `Result<T, E>` (`Ok(T) \| Error(E)`) |
| `[T]` | `List<T>` |
| `{ "k": v }`, `Map<K, V>` | `Map<K, V>` |
| conditions, `true`/`false` | `Bool` |
| `"…"`, `'c'`, interpolation | `String`, `Char` |
| `1`, `2.5` | `Integer`, `Float` |
| `:x` | `Atom` |

Tuples `(A, B)`, functions `A -> B`, unions `A | B`, `Void` and `Never` belong to
the type grammar and need no name.

### 7.3 Rules

1. **Each sugar expands in exactly one place**, the type-expression resolver,
   into an ordinary application of the declared stdlib type. After that,
   `Optional<Integer>` is a type like `Box<Integer>`, and nothing downstream
   tests the name.
2. **Each name lives once, in one registry header**, used by reference. The
   declarative-intrinsics work (`names::TestBuiltin`) is the precedent.
3. **Behaviour comes from declarations, not names**: traits (`Tryable`,
   `Number`), stdlib functions (`or`), and the generic ADT rules.
4. **Fix stdlib declarations first** where they are the real cause: `Range`'s
   implementations, `sum`'s result, the derived-trait property.
5. **A CI guard** fails the build when a new type-name comparison appears in
   `src/`. A text search alone would also match diagnostic text and printing, so
   it works from an **allowlist**: the registry, the resolver, and each
   remaining site classified as value representation, recorded with its reason.
   A new site fails until it is classified. The guard is written in C++ or Kex.

---

## 8. Dimensions, and kinds

### 8.1 Dimensions are a library

Nothing about dimensions is built into the language. `Measure<D>`, the
`Quantity` records, units and products stay ordinary stdlib code, and users
extend them with the same tools.

**The hole.** `Measure<D>` already gets `+`, `-`, comparisons and `Duration`
right. The gap is `*`, `/` and `per` (`units/si.kex:326`, `:345-346`), which
return a bare `Measure`, and a bare `Measure` reads as `Measure<anything>`:

```kex
using Units.SI

main do
  let speed = 100.meter.per(10.second)
  IO.printLine(speed + 5.second)      # accepted today, prints "15.0 m/s"
  let t: Duration = 2.meter * 3.meter # accepted today
end
```

**The design: products are overloads by type.** Each meaningful product is
declared with the dimension it returns, so the overload-set rule (§4.5, 4)
types it:

```kex
make Measure<Quantity.Power> do
  let *(other: Measure<Quantity.Time>) -> Measure<Quantity.Energy> = Measure { … }
end

make Measure<Quantity.Voltage> do
  let *(other: Measure<Quantity.Current>) -> Measure<Quantity.Power> = Measure { … }
end
```

- **The dimension is in the type, the unit is in the value.** The type is
  chosen by the overload above. The display unit is chosen at run time by
  patterns on unit constructors, as `make SIUnit do let *(@Watt, @Hour) ->
  UnitDefinition` already does (`units/si.kex:228`). So `2.watt * 3.hour` is
  typed `Measure<Quantity.Energy>` and displays as `6.0 Wh`, while
  `2.watt * 3.meter` is a type error.
- **Each overload builds its result as a record** (`Measure { canonical: …,
  value: …, unit: … }`), so its declared result fixes `D`. The private `product`
  helper that returns a bare `Measure` goes.
- **No generic `*` or `/` on bare `Measure`.** The ones at `units/si.kex:345-346`
  and `per`/`times` become declared products. An unlisted product is a type
  error, and code that really needs one goes through `Units.Dynamic`
  explicitly.
- **Derived dimensions stay named records** (`Area`, `Speed`, `Volume`,
  `Charge`), as `Quantity` has them today. `Time * Power` needs its own
  overload beside `Power * Time`.

A probe confirms the checker already types this correctly: with products
declared on `make Qty<Dim.Length>`, `meters(2.0) * meters(3.0)` is a
`Qty<Dim.Area>`, and both a wrong result type and a wrong operand are rejected
with readable errors.

**Users add dimensions the same way:**

```kex
module Money do
  record Currency do
  end
  record Rate do
  end

  make Measure<Currency> do
    let /(other: Measure<Quantity.Time>) -> Measure<Rate> = Measure { … }
  end
end

let pay: Measure<Money.Rate> = Money.euro(3600.0) / 1.hour
```

They declare a record for the dimension, define units with `UnitDefinition`
(or the `Unit` trait for a unit ADT, like `TimeUnit`), and declare the
products that make sense. `+`, `-`, comparisons and display come from the
stdlib's generic `Measure<D>`.

**What the language needs.** These are general fixes, needed beyond units:

1. **Overload conflicts compare full signatures.** Today the module above is
   rejected: "`/` is defined twice for receiver `Measure`: here and in the
   prelude" (KEXET-0128). The prelude's `/` is `Duration / Number`
   (`duration.kex:39`), which has a different receiver type argument and a
   different parameter. The conflict check (`receiver_conflicts.cxx`) compares
   only the receiver's type name. It must compare the receiver with its type
   arguments, and the parameter types. This is §4.5's overload-set rule, and
   §7's move away from type names.
2. **A coherence rule for overloads across modules.** It applies only to
   **adding an overload to a name declared in another module**, such as `/` on
   `Measure<Currency>` beside the prelude's `/`. Such an overload may be declared
   only in a module that declares one of its operand types. `Money` can define
   `Currency / Time`, but no module other than the stdlib can define
   `Power * Time`, so two libraries cannot give the same product different
   results. This is the orphan rule, applied to overloads.

   It does **not** restrict new names. A module adding its own function to a
   stdlib type (`make String do let shout …`) is ordinary Kex: 24 files in
   `examples/`, `spec/` and `../slick` do it. Two modules adding the same new
   name to the same type are already caught by KEXES-0004.
3. **No bare generic type.** `Measure` written without its argument must not
   silently mean `Measure<anything>`. It is the escape behind §11 item 6, and the
   same class as #648 (type-argument arity).
4. **Done:** `compiled do` able to splice a *type* into a signature
   (`-> Measure<%{dimension}>`). `units/si.kex` already notes that "a template
   cannot splice a type, only a name". With it, a stdlib generator could write a
   product, its mirror and its inverses (`Energy / Time = Power`,
   `Energy / Power = Time`) from one line, such as
   `Units.product(Power, Time, Energy)`.

**Rejected alternative: dimensions in the type system.** Kennedy's units of
measure, as in F#, give every product automatically: a dimension kind,
unification modulo abelian groups, and dimension expressions such as
`Length / Time` and `Length^3`. It is the most expressive option, and it would
allow code that is generic over dimensions (`let square(m: Measure<D>) ->
Measure<D * D>`). It is rejected because it builds a units feature into the
type system: a solver, new type syntax and new error forms, for what a library
can do with overloads. If the boilerplate of declared products ever outweighs
that, it can be reconsidered.

### 8.2 Higher-kinded traits

A goal, on the HM core. They need kinds (`*`, `* -> *`) and kind inference.
Design points:

- `[X]`, `X?` and `Map<K, V>` must be applications of `List`, `Optional` and `Map`
  (#342), so a type variable can stand in head position (`F` for `List` or
  `Optional`).
- Decide which argument of a multi-argument constructor varies: Haskell's
  last-argument order or type lambdas. That decides the parameter order of
  `Result` and `Map`.
  Decided (2026-11-23): type lambdas. An implementation names the hole,
  `make Result<_, E>, implement: Functor` or `make Map<K, _>`, so neither
  `Result` nor `Map` is reordered.
- How `This<B>`-style signatures (#651 open question 3) interact with calls
  chosen by receiver type (§4.5, 4).
- Under HM there is no variance question: `F<Integer>` and `F<Number>` are
  related only by unifying `F` and the arguments.
- Motivation: one `Functor`/`Monad` in place of the `map`/`flatMap` copies in
  `optional`, `list` and `result`, and the typing of the planned `Bindable`/
  `within`. `examples/typesystem_audit/08_hkt_attempt.kex` becomes its spec.

### 8.3 Effect sets

The kind for inferred capability requirements (§9 and decision 6).

---

## 9. Capabilities: inferred, never written, shown by the LSP

Capability requirements stay out of the syntax. They are computed like types,
as a checker answer every consumer reads.

Hover already prints `requires Clock` with the `← … uses Clock` chain
(`lsp/server.cxx:2009`), but the inference (`analyzer.cxx:570-670`) is a
syntactic call graph keyed by bare names, built before type checking. Over a
real LSP session:

| Function | Uses Clock via | Hover before | Hover now |
|---|---|---|---|
| `direct` | `Clock.now()` | `requires Clock` | `requires Clock` |
| `Stamper.stamp` (in a `make` block) | `Clock.now()` | nothing | `requires Clock` |
| `viaMake` | `Stamper {…}.stamp` | nothing | `requires Clock` via `stamp` |
| `viaStdlib` | `FS.File.read` | nothing | `requires FS.File` |
| `viaTime` | `Time.now` | nothing | `requires Time.Clock` (compiled stdlib) |

The cause: only top-level and module functions are walked (never `make`
blocks), `x.f` cannot be followed before the checker resolves it, imported
interfaces record only `isCapability` and not per-function requirements, and
names defined twice are dropped.

To fix:

1. Build requirement sets from the checker's decided calls (the HIR,
   `--dump-hir`), keyed by the resolved function. That covers `make` blocks,
   calls with a receiver, overloads and names defined twice in a unit.
2. Export each function's set in its KexI interface, so imported and stdlib
   functions carry their requirements to callers like their types do.
3. Show them where types show: hover today, and inlay hints or a code lens on
   function heads later.

The same per-function set, carried on function values, closes #142 for function values passed through signatures (decision 6). Effects inside data are open.
The dictionaries #651 needs in lowering and the hidden capability context are
both implicit parameters, one resolved statically and one scoped by `with`, so
they should share one lowering mechanism. Sandboxing (#183, #185) then needs
nothing more from the checker: `with only: []` is a block whose effect set is
empty.

Also open in this area: #793, a stand-in with fewer parameters passing, which
the single conformance check (§10) fixes.

---

## 10. Structure of the new core

- **The HM core** (§4): constraint generation, unification, rigid signature
  variables, traits as constraints, and check mode for annotations and error
  placement.
- **One conformance check** for `implement:`, capability stand-ins and #651's
  conditional implementations. It instantiates the requirement's type scheme
  and unifies it with the implementation's, for parameters, result and `foul`.
  This fixes #793, the #642 annotated case and §11 item 2 together.
- **One join** (decision 3) for every conditional form and for list elements.
- **Fail closed:** `Void` and `Never` kept apart (decision 9), and `Unknown`
  only after an error has been reported.
- **A coverage test:** every AST node kind constrains every child it has.
- **No type names** outside the registry (§7).

---

## 11. Known holes

On the branch every item below is fixed, each with a spec (see Status): 1
(KEXET-0506), 2 (KEXET-0507), 3 (a function value checked against the
element type), 4 and 5 (`checkCondition`), 6 (dimensions as a library), 7
and 8 (decision 4, the promotion table), 9 (KEXET-0452 names the record's
fields), 10 (one join) and 11 (a written `Any` kept apart from an unknown,
and the `unreliableCandidates` escape deleted). The list stays as the
record of what the plan was written against.

Accepted by `kex -C`, then misbehaving at run time:

1. **`if` without `else` is typed `Never`** (#809, decision 8).
2. **A trait implementation's return type is unchecked when annotated** (the
   #642 case, §2.1). Calling `x.label.upperCase` through a trait-typed
   parameter crashes.
3. **A function value passed to `map` is not checked against the element type.**
   `["a"].map(~inc)` is rejected, but `let f = ~inc; ["a"].map(f)` and
   `["a"].map(~add(1))` are accepted, and the latter crashes with `cannot add
   Integer and String`.
4. **A `when` guard need not be Bool.** `m when m => …` with `m: Integer` is
   accepted, and the walker treats it as true.
5. **An `elif` condition need not be Bool.** `elif n` with `n: Integer` is
   accepted and runs as true.
6. **`Measure` dimensions are lost through `*`, `/` and `per`** (§8.1).

Wrong in the other direction, or in the message:

7. **`[Integer].sum` is `Number`** (`list.kex:37`), so
   `let total(xs: [Integer]) -> Integer = xs.sum` is rejected, and so is
   `[1.5].sum.round`. The same holds for `product` and `Math.abs`. Fixed on the
   branch by decision 4 (`tieTraitResult`).
8. **`Integer / Float` is typed `Integer` in some contexts**; `units.kex` works
   around it. Fixed on the branch: a `Number` bound meets the promotion table,
   so `x * 2.5` is a Float. The `units` workarounds can now become `-> Float`.
9. **Misleading messages:** an unknown field (`Person { age: 1 }.name`) reports
   "`name` expects argument 1 to be Weekday". A trait default with a type error
   (`let shout -> Integer = label.upperCase`) reports "undefined name: `label`".
10. **The inline conditional's type depends on branch order** (decision 3).
    Fixed on the branch: one join for every conditional form, and the wider of
    two fitting types wins.
11. **A call that matches no candidate can pass silently.** When every
    candidate has a parameter without a written type, and a `make` function's
    receiver counts as one, the call is accepted without a check
    (`unreliableCandidates` in `typechecker_expr.cxx`). On a bare `Type`
    receiver, `Type.named("User").construct(5)` passes `kex -C` and is left
    undecided, so the BEAM lowering fails with KEXEB-0038. Found when maps
    became unions: `spec/type_construct.kex` passed a `Just("admin?")` key to
    a `Map<String, Any>` parameter, which `Any` keys had hidden. That case is
    removed from the runtime spec. The checker should reject it, and step 1's
    unification replaces this path.

Also still reproducing: #142 (decision 6), #793 (§10), and #646 (needs #651's
conditional implementations). The original repro of #644 is now rejected.

Item 1 is filed as #809. Items 2–10 are listed only here and in #808, and should
be filed.

---

## 12. Staging

**Open (§14): a parallel core or an in-place refactor.** Decided
(2026-11-23): in place, incrementally. #342 starts with a uniform view of a
type as a head applied to arguments (`TypeApp`, a variable head allowed), and
consumers move onto it case by case. The decisions keep much
of today's behaviour: per-call checking, overload sets, unions at joins,
trait-typed values. So the new core is less new than a rewrite.

| | Parallel core | In-place refactor |
|---|---|---|
| How | build it beside the old checker, like the IR lowering, and switch form by form | replace `argMatchesParam` with unification plus the §4.5 rules site by site, behind the existing answer surface |
| Pro | a clean core that is not shaped by the old code | ships continuously, the answer surface stays put, and no harness comparing two checkers is needed |
| Con | must reproduce the whole answer surface before anything switches, and needs a comparison harness whose oracle (today's specs) encodes today's holes | the old structure can survive under the new rules, and the drift of §3 can repeat |

The steps below hold either way.

1. **HM core** (§4, §10): unification, rigid signature variables, call-site
   checking of unannotated functions, check mode, the eight rules of §4.5, one
   conformance check, one join, `Void`/`Never`, fail-closed `Unknown`, the
   coverage test. Closes §11 items 1–5 and 10, #793, and the #642 annotated
   case.
2. **#342, the type grammar:** uniform type application, nominal vs structural,
   inference placeholders kept apart from resolved types, effect sets inside
   function types.
3. **No type names** (§7): the registry, the stdlib declaration fixes, and the
   CI guard. This can start alongside step 1.
4. **Dimensions as a library** (§8.1). This needs the general fixes there
   (overload conflicts by full signature, the coherence rule, no bare generic
   type), then the stdlib's products declared as overloads. It closes §11 item 6
   and does not wait for kinds.
5. **Kinds, then higher-kinded traits** (§8.2).
6. **Independently, once the core exists:**
   - #651's traits and bounds, plus decisions 4 and 5 (fixes §11 item 7)
   - inferred capability sets in KexI and the LSP (§9), then #142
7. **Lowering:** #651 dictionaries and the capability context as one mechanism.

Until the switch, point fixes are still worth making for holes that bite users,
but each covers its class, not its repro.

---

## 13. Risks

- **Scope.** HM with type classes, rows, kinds, effect rows, unions and
  overload sets is more than any single production language combines (Koka
  has effect rows without type classes). Keeping dimensions a library (§8.1)
  takes one extension out. Each extension is established on its own, and the
  staging keeps them apart, but interactions will surface. Each is raised on
  #808 when found.
- **The design is not plain HM, and must not claim to be.** §4.5 has eight
  rules beyond unification, three of them from the decisions (per-call
  checking, results that follow parameters, trait types). Keeping §4.5 the one
  complete list is what stops a ninth from appearing unwritten, the way the
  original design was lost (§3).
- **With a parallel core, the real cost is the answer surface, not
  inference.** The walker, lowering, the LSP and KexI consume the checker's
  answers (decided calls, addressing by owner, interface types). A parallel
  core must produce all of them before any form switches, and its oracle is
  weak: the spec suite encodes today's behaviour, holes included, so every
  disagreement between the two checkers has to be classified by hand. The IR
  strangler worked because Core Erlang diffs cleanly, and diagnostics and
  answers do not.
- **With an in-place refactor, the risk is that the old structure survives**
  under the new rules. The CI guard (§7.3) and the node-coverage test (§10)
  are what make the change visible.
- **Principality is given up**, on purpose, for unannotated functions (checked
  per call, rule 6) and for overload calls with unknown arguments (unions,
  rule 4).
- **Errors arrive later in unannotated code:** a branch of the wrong type makes a
  union, reported where the union is used, not at the branch (decision 3).
  An annotation moves it back to the branch.
- **Runtime dispatch stays** for values of a trait type (rule 8), and decision
  5's open question decides whether that dispatch can always be done from the
  value alone.
- **A long stretch with no visible progress**, with a parallel core. Point
  fixes and §7's registry work continue meanwhile.

---

## 14. Open questions

**Decisions needed before the core work starts:**

1. **A parallel core or an in-place refactor** (§12).
2. **A bare trait result with several parameters** (decision 4, §4.5 rule 7):
   Decided (2026-11-24): b, checked per call.
   Built: the call is the body's type at its arguments, as for an
   unannotated function (`combine(1, 2.5)` is a Float),
   `spec/error_trait_result_several_params.kex`.
   - a. name the type
   - b. check per call
   - c. specify promotion against the join
3. **Effects inside data** (decision 6):
   - a. hidden effect parameters on types with function fields
   - b. a call through a field counts as possibly `foul`
   - c. #142 closes for signatures only
4. **Types the run time cannot tell apart, in a union or a trait type**
   (decisions 3 and 5). For example, `BookId | Integer`, or a `BookId` in a
   `[Shape]`. Proposed: an error. The alternative is values that carry their
   type or implementation explicitly, which is a second dispatch mechanism.
   Decided (2026-11-24): an error.
   Built: a union of a distinct type and what backs it (or two distinct types
   backed alike) is KEXET-0512, written or joined; a distinct type stored as
   a trait it implements in its own `make` (a typed `let`, a list or optional
   of the trait, or a trait-typed parameter) is KEXET-0513
   (`spec/error_indistinguishable_types.kex`). A `Showable` parameter is the
   exception: the checker routes the value through its own `showValue`, which
   both backends honour.
5. **"Exported functions are annotated"** (decision 1): count what it breaks,
   then adopt it or drop it.
   Decided (2026-11-24): count first, then decide. Counted, then decided
   (2026-11-24): required. The stdlib's are annotated first, then an
   unannotated exported function is an error; metro and slick follow.
   Built: KEXET-0514 for a public function of a file module (`module Name`
   as a file's header), or of a module in a file other than the entry, that
   leaves a parameter or its result untyped with no standalone signature;
   private functions and a program file's own modules are exempt
   (`spec/error_exported_unannotated.kex`). Counted again with standalone
   signatures recognised: 14 in the stdlib (now annotated), and none in
   harmat, metro or slick. Diagnostics print a file under the working
   directory relative to it.
   Counted: unannotated exported functions are 100 in the stdlib (FS 43, Net
   about 37, Assert 8, URI 6, Control.Retry 3), 0 in harmat, 39 in metro and
   90 in slick.

**Other open questions:**

6. **The core's deliverables:**
   - an inventory of the answer surface
   - the KexI format change for type schemes with constraints, kinds and effect
     rows, and its effect on the caches
   - with a parallel core, a harness that compares the two checkers
   - an exit criterion per syntax form
7. **`Float` and `Int`** (§6.3): the concrete-`Float` proposal against the
   original design's umbrella trait, and `Int` as an alias of `Integer` or of
   `Int64`.
   Decided (2026-11-24): nearly the rule `Integer` follows. A float is a
   `Float`, and `Float64` shows only where a program wrote it. Built so far:
   a literal's type prints `Float` (the registry keys floats by size), and a
   written `Float` is that same concrete type, not a bound over the float
   types, in programs and imported signatures alike.
8. **Written intersection types** (§4.5, 5): allowed in programs, or kept as the
   type of overload sets only.
   Decided (2026-11-24): allowed in programs.
   Built: `A & B` parses, checks (a value must be every member) and runs on
   both backends, and record intersections merge their fields
   (`spec/intersection_written.kex`, `spec/error_intersection_written.kex`,
   language spec §3).
9. **HKT argument order** (§8.2): Haskell's last-argument convention or type
   lambdas.
10. **Type splicing in `compiled do`** (§8.1, fix 4): decided and built
   (2026-11-23). It was a question of whether to add it, so a
   stdlib generator can declare a product with its mirror and inverses from
   one line.
11. **Process:**
   - file §11 items 2–10
   - file the KEXET-0128 conflict (§8.1, fix 1) as a bug: it blocks
     user-defined overloads on any generic type's instances today
   - one issue per stage, each with an exit criterion
   - move §4 and §5 to a standing specification once the core switches
12. **#415 and #808 on GitHub** do not yet record decisions 1–9, or that
   dimensions stay a library. This document is ahead of them.

---

## 15. Housekeeping

- `examples/typesystem_audit/06` and `07` are now rejected with correct
  non-exhaustive-match errors. The audit README still lists them as open, so
  they can graduate to specs.
- `make check-examples` currently fails on two untracked local files
  (`examples/brainfuck.kex`, `examples/isitme.kex`), not on committed code.

---

## Appendix A: type-name comparisons

Every `== "Name"` / `!= "Name"` against a capitalised name in `src/semantic`,
`src/interpreter`, `src/ir`, `src/lsp` and `src/common`, as of 4794703c. A
grep count overstates a little, because some comparisons only print or look up
a declaration. Regenerate with:

```
grep -rnoE '(==|!=) *"[A-Z][A-Za-z0-9.]*"' src/semantic src/interpreter src/ir src/lsp src/common
```


### Built-in ADTs (61)

| Site | Name |
|---|---|
| `src/common/prelude_interfaces.hxx:152` | `Optional` |
| `src/common/prelude_interfaces.hxx:161` | `Result` |
| `src/interpreter/evaluator.cxx:4292` | `Ok` |
| `src/interpreter/evaluator.cxx:4295` | `Error` |
| `src/interpreter/evaluator.cxx:4298` | `Just` |
| `src/interpreter/evaluator.cxx:4301` | `Error` |
| `src/interpreter/evaluator.cxx:4301` | `None` |
| `src/interpreter/evaluator.cxx:5433` | `Optional` |
| `src/interpreter/evaluator.cxx:5434` | `Just` |
| `src/interpreter/evaluator.cxx:5434` | `None` |
| `src/interpreter/evaluator.cxx:5436` | `Optional` |
| `src/interpreter/evaluator.cxx:5517` | `Just` |
| `src/interpreter/evaluator.cxx:5517` | `None` |
| `src/interpreter/evaluator.cxx:5519` | `Optional` |
| `src/interpreter/evaluator.cxx:5977` | `None` |
| `src/interpreter/evaluator.cxx:6499` | `Just` |
| `src/interpreter/evaluator.cxx:6505` | `None` |
| `src/interpreter/stdlib/file.cxx:135` | `Just` |
| `src/interpreter/stdlib/file.cxx:1544` | `None` |
| `src/interpreter/stdlib/kex.cxx:223` | `None` |
| `src/interpreter/stdlib/net.cxx:283` | `Just` |
| `src/interpreter/stdlib/net.cxx:816` | `Ok` |
| `src/interpreter/stdlib/test.cxx:93` | `Error` |
| `src/interpreter/stdlib/test.cxx:93` | `Result` |
| `src/interpreter/stdlib/type.cxx:89` | `Option` |
| `src/interpreter/stdlib/type.cxx:89` | `Optional` |
| `src/interpreter/stdlib/type.cxx:93` | `Result` |
| `src/interpreter/stdlib/type.cxx:94` | `Ok` |
| `src/interpreter/stdlib/type.cxx:299` | `Option` |
| `src/interpreter/stdlib/type.cxx:417` | `Option` |
| `src/interpreter/value.cxx:148` | `None` |
| `src/ir/lowering_helpers.cxx:274` | `Option` |
| `src/semantic/declared_type.hxx:41` | `Optional` |
| `src/semantic/resolve_core.cxx:164` | `Optional` |
| `src/semantic/resolve_core.cxx:204` | `Optional` |
| `src/semantic/typechecker.cxx:2126` | `Optional` |
| `src/semantic/typechecker.cxx:4547` | `Optional` |
| `src/semantic/typechecker.cxx:4549` | `Result` |
| `src/semantic/typechecker.cxx:4591` | `Optional` |
| `src/semantic/typechecker.cxx:5086` | `Just` |
| `src/semantic/typechecker.cxx:5089` | `Result` |
| `src/semantic/typechecker.cxx:5091` | `Error` |
| `src/semantic/typechecker.cxx:5091` | `Ok` |
| `src/semantic/typechecker.cxx:5092` | `Ok` |
| `src/semantic/typechecker.cxx:5240` | `Just` |
| `src/semantic/typechecker.cxx:5245` | `Error` |
| `src/semantic/typechecker.cxx:5245` | `Ok` |
| `src/semantic/typechecker.cxx:5247` | `Result` |
| `src/semantic/typechecker.cxx:5944` | `Result` |
| `src/semantic/typechecker_expr.cxx:2412` | `Result` |
| `src/semantic/typechecker_expr.cxx:2417` | `Error` |
| `src/semantic/typechecker_expr.cxx:2417` | `Just` |
| `src/semantic/typechecker_expr.cxx:2417` | `Ok` |
| `src/semantic/typechecker_expr.cxx:2438` | `None` |
| `src/semantic/typechecker_expr.cxx:2438` | `Result` |
| `src/semantic/typechecker_expr.cxx:4720` | `None` |
| `src/semantic/typechecker_expr.cxx:9430` | `Optional` |
| `src/semantic/typechecker_expr.cxx:9430` | `Result` |
| `src/semantic/typechecker_expr.cxx:9449` | `Optional` |
| `src/semantic/typechecker_expr.cxx:9501` | `Optional` |
| `src/semantic/typechecker_expr.cxx:9501` | `Result` |

### Numeric (41)

| Site | Name |
|---|---|
| `src/interpreter/evaluator.cxx:5299` | `Float` |
| `src/interpreter/evaluator.cxx:5299` | `Integer` |
| `src/interpreter/evaluator.cxx:5299` | `Number` |
| `src/interpreter/evaluator.cxx:5410` | `Float` |
| `src/interpreter/evaluator.cxx:5410` | `Integer` |
| `src/interpreter/evaluator.cxx:5410` | `Number` |
| `src/interpreter/evaluator.cxx:5423` | `Int` |
| `src/interpreter/evaluator.cxx:5423` | `Integer` |
| `src/interpreter/evaluator.cxx:6274` | `Integer` |
| `src/interpreter/evaluator.cxx:6299` | `Integer` |
| `src/interpreter/evaluator.cxx:6321` | `Byte` |
| `src/interpreter/evaluator.cxx:6328` | `Float` |
| `src/interpreter/value.cxx:1171` | `Float` |
| `src/interpreter/value.cxx:1171` | `Int` |
| `src/interpreter/value.cxx:1171` | `Integer` |
| `src/interpreter/value.cxx:1186` | `Int` |
| `src/interpreter/value.cxx:1186` | `Integer` |
| `src/interpreter/value.cxx:1188` | `Float` |
| `src/ir/lowering_expr.cxx:583` | `Float` |
| `src/ir/lowering_expr.cxx:583` | `Integer` |
| `src/ir/lowering_expr.cxx:583` | `Number` |
| `src/semantic/declared_type.hxx:33` | `Byte` |
| `src/semantic/declared_type.hxx:33` | `Int` |
| `src/semantic/declared_type.hxx:38` | `Float32` |
| `src/semantic/declared_type.hxx:38` | `Float64` |
| `src/semantic/resolve_core.cxx:189` | `Integer` |
| `src/semantic/traits.cxx:87` | `Integer` |
| `src/semantic/traits.cxx:90` | `Float` |
| `src/semantic/traits.cxx:93` | `Number` |
| `src/semantic/traits.cxx:125` | `Float` |
| `src/semantic/traits.cxx:125` | `Integer` |
| `src/semantic/traits.cxx:125` | `Number` |
| `src/semantic/traits.cxx:147` | `Float` |
| `src/semantic/traits.cxx:147` | `Integer` |
| `src/semantic/traits.cxx:147` | `Number` |
| `src/semantic/typechecker_expr.cxx:3720` | `Float` |
| `src/semantic/typechecker_internal.hxx:242` | `Float` |
| `src/semantic/typechecker_internal.hxx:242` | `Integer` |
| `src/semantic/typechecker_internal.hxx:242` | `Number` |
| `src/semantic/types.cxx:244` | `Float` |
| `src/semantic/types.cxx:244` | `Number` |

### Built-in types (53)

| Site | Name |
|---|---|
| `src/common/prelude_interfaces.hxx:117` | `Any` |
| `src/common/prelude_interfaces.hxx:165` | `Block` |
| `src/interpreter/evaluator.cxx:2627` | `String` |
| `src/interpreter/evaluator.cxx:2631` | `Atom` |
| `src/interpreter/evaluator.cxx:3955` | `This` |
| `src/interpreter/evaluator.cxx:5400` | `Any` |
| `src/interpreter/evaluator.cxx:5484` | `Char` |
| `src/interpreter/evaluator.cxx:6242` | `Type` |
| `src/interpreter/evaluator.cxx:6267` | `String` |
| `src/interpreter/evaluator.cxx:6289` | `Atom` |
| `src/interpreter/evaluator.cxx:6348` | `String` |
| `src/interpreter/value.cxx:1182` | `String` |
| `src/interpreter/value.cxx:1190` | `Bool` |
| `src/interpreter/value.cxx:1192` | `Atom` |
| `src/interpreter/value.cxx:1194` | `Char` |
| `src/interpreter/value.cxx:1198` | `Tuple` |
| `src/interpreter/value.cxx:1202` | `Range` |
| `src/ir/lowering_function.cxx:68` | `Char` |
| `src/ir/lowering_make.cxx:470` | `Atom` |
| `src/ir/lowering_make.cxx:639` | `Char` |
| `src/ir/lowering_make.cxx:763` | `Char` |
| `src/ir/lowering_method_call.cxx:764` | `String` |
| `src/ir/lowering_method_call.cxx:769` | `Atom` |
| `src/ir/lowering_method_call.cxx:1190` | `This` |
| `src/lsp/server.cxx:4047` | `This` |
| `src/lsp/server.cxx:4056` | `This` |
| `src/lsp/server.cxx:4656` | `This` |
| `src/lsp/server.cxx:4663` | `This` |
| `src/semantic/declared_type.hxx:96` | `Atom` |
| `src/semantic/declared_type.hxx:96` | `Atom` |
| `src/semantic/resolve_core.cxx:164` | `Block` |
| `src/semantic/resolve_core.cxx:190` | `Char` |
| `src/semantic/resolve_core.cxx:191` | `String` |
| `src/semantic/resolve_core.cxx:192` | `Bool` |
| `src/semantic/resolve_core.cxx:193` | `Atom` |
| `src/semantic/typechecker.cxx:1091` | `Unknown` |
| `src/semantic/typechecker.cxx:4420` | `Never` |
| `src/semantic/typechecker.cxx:4423` | `Void` |
| `src/semantic/typechecker.cxx:4428` | `Any` |
| `src/semantic/typechecker_expr.cxx:2394` | `Range` |
| `src/semantic/typechecker_expr.cxx:4538` | `Block` |
| `src/semantic/typechecker_expr.cxx:4665` | `Tuple` |
| `src/semantic/typechecker_expr.cxx:4757` | `Void` |
| `src/semantic/typechecker_expr.cxx:4763` | `String` |
| `src/semantic/typechecker_expr.cxx:6778` | `Block` |
| `src/semantic/typechecker_expr.cxx:8514` | `Any` |
| `src/semantic/typechecker_expr.cxx:8799` | `Map` |
| `src/semantic/typechecker_expr.cxx:9306` | `Any` |
| `src/semantic/typechecker_intrinsics.cxx:240` | `Atom` |
| `src/semantic/types.cxx:247` | `Void` |
| `src/semantic/types.cxx:259` | `Any` |
| `src/semantic/types.cxx:259` | `Tuple` |
| `src/semantic/types.cxx:260` | `Block` |

### Serving (22)

| Site | Name |
|---|---|
| `src/interpreter/evaluator.cxx:3955` | `New` |
| `src/interpreter/evaluator.cxx:3975` | `New` |
| `src/interpreter/scheduler.cxx:322` | `Reply` |
| `src/ir/lowering_method_call.cxx:1174` | `New` |
| `src/lsp/server.cxx:2700` | `Reply` |
| `src/lsp/server.cxx:4047` | `New` |
| `src/lsp/server.cxx:4048` | `New` |
| `src/lsp/server.cxx:4056` | `New` |
| `src/lsp/server.cxx:4535` | `Reply` |
| `src/lsp/server.cxx:4542` | `Reply` |
| `src/semantic/typechecker.cxx:521` | `Server` |
| `src/semantic/typechecker.cxx:3048` | `Reply` |
| `src/semantic/typechecker.cxx:3082` | `Reply` |
| `src/semantic/typechecker.cxx:3250` | `Reply` |
| `src/semantic/typechecker.cxx:5696` | `Reply` |
| `src/semantic/typechecker.cxx:5963` | `Reply` |
| `src/semantic/typechecker.cxx:8111` | `Server` |
| `src/semantic/typechecker_expr.cxx:2331` | `Server` |
| `src/semantic/typechecker_expr.cxx:3055` | `New` |
| `src/semantic/typechecker_expr.cxx:3084` | `New` |
| `src/semantic/typechecker_expr.cxx:6348` | `Server` |
| `src/semantic/typechecker_expr.cxx:7787` | `Server` |

### Ordering (6)

| Site | Name |
|---|---|
| `src/interpreter/stdlib/list.cxx:620` | `Equal` |
| `src/interpreter/stdlib/list.cxx:749` | `Less` |
| `src/interpreter/stdlib/list.cxx:770` | `Less` |
| `src/interpreter/stdlib/list.cxx:780` | `Less` |
| `src/interpreter/stdlib/list.cxx:803` | `Greater` |
| `src/interpreter/stdlib/list.cxx:813` | `Greater` |

### Built-in traits (4)

| Site | Name |
|---|---|
| `src/semantic/traits.cxx:105` | `Inspectable` |
| `src/semantic/traits.cxx:105` | `Showable` |
| `src/semantic/traits.cxx:154` | `Equatable` |
| `src/semantic/traits.cxx:154` | `Showable` |

### Modules (5)

| Site | Name |
|---|---|
| `src/semantic/typechecker_expr.cxx:1704` | `Elixir` |
| `src/semantic/typechecker_expr.cxx:1704` | `Erlang` |
| `src/semantic/typechecker_expr.cxx:1705` | `Gleam` |
| `src/semantic/typechecker_expr.cxx:1710` | `Elixir` |
| `src/semantic/typechecker_expr.cxx:6217` | `Prelude` |

### Other (16)

| Site | Name |
|---|---|
| `src/interpreter/evaluator.cxx:6034` | `True` |
| `src/interpreter/evaluator.cxx:6038` | `False` |
| `src/interpreter/evaluator.cxx:6384` | `Binary` |
| `src/interpreter/stdlib/file.cxx:27` | `Write` |
| `src/interpreter/stdlib/file.cxx:30` | `Append` |
| `src/interpreter/stdlib/file.cxx:33` | `ReadWrite` |
| `src/interpreter/stdlib/file.cxx:36` | `Read` |
| `src/interpreter/stdlib/file.cxx:217` | `Write` |
| `src/interpreter/stdlib/file.cxx:222` | `Append` |
| `src/interpreter/stdlib/process.cxx:601` | `From` |
| `src/interpreter/stdlib/regex.cxx:82` | `Regex.Regex` |
| `src/interpreter/stdlib/regex.cxx:228` | `Regex.Regex` |
| `src/interpreter/value.cxx:1184` | `Binary` |
| `src/interpreter/value.cxx:1204` | `Stream` |
| `src/interpreter/value.cxx:1206` | `Feed` |
| `src/semantic/typechecker_expr.cxx:4763` | `FilePath` |
