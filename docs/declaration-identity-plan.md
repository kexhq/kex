# Declaration identity: the checker names declarations, backends name symbols

> **Stage 5, state 2026-11-11 (later):** the receiver, serving and
> distinct-type tables are keyed by identity (`declaredNames`,
> `declaredNameOf` and `ownerIdentities` are deleted; `state_type` in
> `kex_intrinsic_process.erl` takes the tag as it is), so the paragraph
> below that calls them written spellings is out of date. The `::` → `.`
> flip is done too (2026-11-12, `mangle::kSeparator`). Ordered open work:
> the review findings at the top of `docs/pass-restructure-plan.md`.

Status: in progress (2026-11-10). Stages 1–4 done: the checker gives each
record one identity, records the declaration of every record construction,
pattern, annotation and `make` target, and lowering reads those answers;
`canonicalRecordName` and lowering's `using`/alias record resolution are
deleted (stdlib Core Erlang identical, all gates green). Stage 5's name
resolution is done too: the checker records the function a bare call or
tagged literal names (`calledFunctionOf`) and the constant a bare name names
(`referencedConstantOf`), so lowering's `using` bindings (`moduleImports`,
`externalImports`, `applyUsingCore` and its kin) are deleted; and the
checker records the receiver type a devirtualized call runs
(`staticReceiverHead`) while lowering spells the BEAM symbol. A call to an
argument-overloaded `make` method is named from the declaration the checker
selected (`makeMethodSymbol`, the one rule its definition is named by too),
so lowering's reverse resolution of qualified names into written spellings
(`moduleLocalTypeSpelling`) is deleted. Stage 6: KexI records a record's
Kex identity (`KexiRecord::identity`) beside its BEAM tag, and the
interfaces the checker reads key records by the identity; `tagAtom` is the
BEAM backend's alone (layouts, display registration). BEAM symbols for
`make` methods are spelled from the declarations the checker resolved
(`methodSignatureTypes`: each parameter's annotation as its record
`Culture.Culture`, its alias's target), not from the written spellings, at
the one place each is named (definitions, the overload dispatchers, the
calls the checker bound). Stage 5 is NOT done: receiver implementations
(`mangleReceiverImplementation(name, owner)`), `upgrade/Type`, the serving
slot and distinct-type registrations are still spelled from the `make`
target as written (`SIPrefix`, not `Units.SI.SIPrefix`), and the runtime
expects that (`state_type` in `kex_intrinsic_process.erl` strips the
module). Lowering maps the checker's identities back to those names
(`declaredNames`, `declaredNameOf`). Follows Stage 3 of
`pass-restructure-plan.md`, which left lowering reading no expression types.

## Goal

The checker answers in Kex terms only: which **declaration** each name, call
and annotation means. Every backend (BEAM today; native and JavaScript later)
turns declarations into its own **representation** (a tagged tuple, an OTP
native record, a JS class) and its own **symbols** (mangled function names).
Nothing about a backend's representation moves up into the checker or the
language, and no backend resolves a name itself. Printing stays Kex-specific
(`showValue`), never a backend's raw value.

## What is wrong today

1. **Two identities for one record.** The stdlib interface readers register
   every record declared in a module under its qualified name *and* its bare
   name (`prelude_interfaces.hxx`, the `try_emplace((*record)->name, …)`
   lines; `kexi_registry.cxx`, the same for KexI). So the checker types the
   annotation `Culture` as `Culture` in some scopes and `Culture.Culture` in
   others: `pick(c: Culture) -> Culture` returns `Culture` while
   `Culture.DEFAULT` is `Culture.Culture`.
2. **Lowering has its own name resolver.** `Lowering::canonicalRecordName`
   (19 call sites in `src/ir`) re-resolves written record spellings through
   module scope, `using`s, aliases and suffix matching, to repair (1) and to
   find the tag. Over the stdlib and the 598 BEAM specs it changes a name 75
   times; where it does, the checker's answer is missing or different.
   Keyed by spelling it cannot be replaced: dispatchers are generated after
   the module walk, so lowering asks in scope `""` for a type written inside a
   module.
3. **Symbols come from written spellings.** `mangleReceiverImplementation` /
   `mangleReceiverSignature` (25 uses in `src/ir`) build `to/Time`,
   `*/Complex.Complex,Number` from the owner and parameter types as written,
   which lowering collects and resolves itself (`methodDispatchTypes`,
   `MakeMethod::owner`).
4. **The checker spells BEAM symbols.** `resolve_core::importDispatchSpelling`
   builds `name/Head` "spelled the way `mangleReceiverImplementation` does",
   stored in `ResolvedCallTarget::importDispatchSpelling` (7 uses).
5. **Interfaces key records by their BEAM tag.** KexI's
   `KexiRecord::tagAtom` is what `kexi_registry` uses as the record's
   identity for the checker.

## Design

- **Declaration identity** is the Kex qualified name: module path plus
  declared name (`Culture.Culture`, `Units.SI.Measure`), the name
  diagnostics and `Type.of` print. It is a language concept; that the BEAM
  tag spells the same string today is the BEAM backend's choice.
- **The checker records, per occurrence, the declaration it resolved:**
  - each record construction, record pattern and record named as a value;
  - each type annotation (`TypeExpr`) used by a parameter, a typed pattern
    or a `make` target;
  - each method call and operator: the method declaration chosen (local
    `make` clause, imported interface entry, or run-time dispatch over a
    named set of declarations).
  These are node-keyed `hir::Program` answers, persisted with the HIR like
  the call targets.
- **Interfaces carry declaration identities**, not BEAM spellings. A unit's
  KexI says "method `to` of `Time` with parameters (Type<String>)"; the BEAM
  backend derives the symbol from that, the same way in the defining unit and
  in every caller. `tagAtom` becomes BEAM-backend metadata, not identity.
- **The BEAM backend owns** the symbol scheme (today's mangling, now a pure
  function of the declaration) and the representation (today's tagged tuple;
  OTP native records are an option behind the same interface).

## Stages (each gated by `make test-all`, fuzz, devirt, dup-audit, and the
stdlib's Core Erlang compared before/after)

1. **One identity per record.** Drop the bare-name registrations from both
   interface readers; fix what then fails to resolve in the checker (bare
   spellings reach the qualified record through the normal scope / `using` /
   unique-suffix rules). Expected: diagnostics and `Type.of` print the
   qualified name for module records, as they already do for
   `Measure<Quantity.Time>`; spec expectations updated.
2. **Record occurrences.** The checker records the declaration of every
   record construction, pattern and record-as-value; lowering reads it at
   those sites (`lowerRecordConstruction`, record patterns, `fieldAccessIn`,
   the `UpperIdentifier` path).
3. **Owners by declaration.** `MakeMethod` and the owner tables carry the
   `make` target's declaration identity from the checker beside the written
   spelling; every owner comparison (`selectedMakeMethod`,
   `nextExtendingModule`, `takesPlainParameters`, `typeOwnsStaticFunction`)
   compares identities. `canonicalRecordName` is deleted.
4. **Guards by declaration.** Dispatcher guards are built from the checker's
   resolved parameter and receiver types (node-keyed), not from
   `typeText(…, Dispatch)` spellings; `discriminatingType`/`typeGuard` take a
   declaration, and the BEAM backend maps it to its run-time test.
5. **Symbols by declaration.** The BEAM mangling scheme becomes a function of
   the method declaration; `methodDispatchTypes` and the written-spelling
   inputs go. KexI carries declaration identities;
   `importDispatchSpelling` leaves the checker for the BEAM backend.
   Measured 2026-11-10 (stdlib, 598 BEAM specs, the separate-build tests):
   lowering's `moduleImports` binding of a bare call disagrees with the
   checker in four shapes. The checker records Dispatch, not the function,
   for a call to a pattern-dispatched or overloaded module function
   (`displaySymbol(unit)` in `Units.SI`, `retry?(result)`); it selects a
   clause lowering emits under a per-signature name (`Shapes.area`); and a
   symbol renamed after registration (`fetch$free`) has to be read from the
   final symbol table. So the checker first records, per call, the function
   declaration it resolved the name to (inside `checkCall`, where scope and
   imports are applied), whatever clause runs; lowering then maps that
   declaration to its symbol and `moduleImports`/`externalImports` go.
6. **`tagAtom` is backend metadata.** `kexi_registry` keys records by
   declaration identity.

`dup-audit` gets a metric per stage (`canonicalRecordName(`,
`importDispatchSpelling`, `typeText(` in `src/ir`) that may only fall.

## Risks

- Stage 1 changes printed type names in diagnostics and many `.expected`
  files; it also changes what a bare spelling means where two modules declare
  the same record name (the unique-suffix rule decides, and ambiguity becomes
  an error asking for a qualified name).
- Stage 5 changes emitted symbol names: interfaces and caches built before it
  are invalid (one cache version bump relative to `main`).
- Every stage keeps both BEAM and the walker green; the walker reads the same
  declaration answers.

## Done: written annotations (2026-11-11)

Lowering no longer reads an annotation's text. The checker answers what it
built from them: `declaredTypeOf` (a type as declared, the program's aliases
followed, for the reflection layouts), `expandDeclared` (a distinct type's
backing), `annotationSpelling` (an annotation as a dispatcher tests it: its
resolved record, else as written) and `structuralAliasBodies`. They keep what
programs see unchanged: option (b) below is the implemented behaviour, and
`Type.fields` keeps answering with a type as declared, aliases followed.
Answering with the checker's resolved types instead (option (a)) would be a
language change, made inside the checker alone if it is ever wanted. The `lowering-resolves` audit metric is 0 and may not rise: no name
resolution or annotation reading in `src/ir`, and no route to the checker's
machinery (no `src/semantic` include, type equality, trait satisfaction or
semantic type), so the checked program's answers through `hir/hir.hxx` are
all lowering can know about types and names.

What lowering used to take from an annotation's own text, rather than from
the checker's answer for it:

- the run-time reflection layouts `Type.fields` / `Type.constructors`
  answer with (`structuredTypeOfDeclared` + `expandDeclaredAliases`);
- the run-time dispatchers' alias bodies (`structuralTypeAliases`), the
  same-signature test between a function's clauses (`overloadedByParamType`),
  and whether a module function's first parameter is one of the module's own
  types (the receiver-method rule in `lowerModules`).

Measured against the checker's resolved type for each annotation
(`structuredTypeOf` of what `resolveTypeExpr` returned), over the stdlib
and the 598 BEAM specs: 490 differ. The checker names a module's types by
their identity (`Net.SupportValue` for `SupportValue`), follows aliases
(`Duration` is `Measure<Quantity.Time>`), spells some primitives its own way
(`Int`), and has no answer for primitive and type-variable annotations or
for constructor payloads. Since the layouts are what a program sees from
`Type.fields`, the checker answers them as declared, aliases followed
(option (b), the behaviour programs already had); option (a), the
checker's resolved types, would change program output and is not planned.

## Done: guards by the checker's reading (2026-11-11)

Stage 4's remaining half. Lowering no longer reads a type's form from its
spelling: the checker answers `typeTestOf(scope, written)`
(`semantic/type_test.hxx`), what a type written in a module says about a
value in Kex terms: any value (`Any`, a trait, a type variable), a function
of some arity, a block, a list (`[Char]` apart), a map, a tuple, or a named
type with its identity (`Net.HTTP.Client` for `Client` written in
`Net.HTTP`). Aliases are followed and type arguments dropped there. The BEAM
backend's `typeGuard`, `discriminatingType` and `runtimeShapes` map that
answer to run-time tests; `guardTag`'s scope walk, the alias table and
`functionTypeArity` left `src/ir`, and the make-target table holds each
target by the checker's identity too, so the suffix matching that bridged
the two spellings is gone. Stdlib Core Erlang identical.

Still keyed by a type's name as written, because the BEAM symbols and
runtime are (`state_type` in `kex_intrinsic_process.erl`, `upgrade/Type`,
`mangleReceiverImplementation`): `methodOwners`, `distinctTypes` and the
serving tables. Lowering reaches them from a checker identity through
`declaredNames` (filled from the checker's identities), not by cutting the
module off. Spelling those symbols from the identity is the rest of
Stage 5.
