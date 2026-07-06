# Atom Disentanglement Plan

Goal: split the single `AtomValue`/`AtomLiteral` representation — currently doing quadruple duty — into distinct IR concepts, while keeping BEAM codegen working at every step.

## The conflation (current state)

`AtomValue{name}` is the sink for four unrelated concepts:

1. **Genuine symbolic atoms** (`:foo`, `:read`) — the only true atom case.
2. **Zero-arg ADT variant tags** (`None`, `Less`, `Nothing`, `Fizz`) — reach `AtomValue` via the `UpperIdentifier` fallback at `src/interpreter/evaluator.cxx:1108-1114`. `Less`/`Equal`/`Greater` are also *registered* as atoms directly (`src/interpreter/stdlib/adt.cxx:30-37`), while `Ok`/`Just`/`Error` are `RecordValue`s — three different representations for "ADT variant".
3. **Module / type-name references** (`Math`, `String`, `File`) — same `UpperIdentifier` fallback; explicit in `src/codegen/core_erlang.cxx:248-250`.
4. **Runtime type tags** — `to(String)` produces `atom("String")` purely so pattern matching can compare names (`src/interpreter/evaluator.cxx:1514-1516`).

**Symptom that exposes it:** `src/interpreter/evaluator.cxx:653-658` uses `atom->name` as a type name for method dispatch. That's only meaningful for case 2, but it runs for every `AtomValue`, so a genuine `:foo` or stray module atom gets misread as a type.

## Design principle

The disentanglement happens **in the IR**. The BEAM lowering is a per-concept emitter that re-converges on atoms deliberately and traceably. Erlang's runtime genuinely uses atoms for all these roles; what's broken is that Kex collapses them at the value layer, below the type checker and dispatch.

```
                ┌──────────── Kex IR (distinct concepts) ────────────┐
                │  Atom          Variant         Module       TypeTag │
                │  :foo          Ok(x)/Less      Math         String  │
                └──────────────────────────┬──────────────────────────┘
                                           │
                              lowering (per concept)
                                           │
                ┌──────────── BEAM Core Erlang ───────────────────────┐
                │  'foo'         {'Ok',X} / 'Less'   'math'    (none) │
                │  true/false    tagged tuple / bare atom              │
                └─────────────────────────────────────────────────────┘
```

The reverse direction (Erlang values entering Kex via FFI) goes through a wrapping layer that re-classifies a bare atom as Variant/Module/genuine-Atom based on declared Kex types.

## Phase 1 — `VariantValue` (highest value, self-contained)

Add to `src/interpreter/value.hxx`:

```cpp
struct VariantValue {
    std::string tag;         // "Ok", "Less", "Nothing", "Just"
    std::string parentType;  // "Result", "Ordering", "Option", "" if unknown
    std::vector<ValuePtr> args;  // payload, empty for zero-arg
};
```

Add to the `Value` variant. Migrate:

- **One-arg constructors** (`Ok`/`Just`/`Error`/`Left`/`Right`) currently registered in `adt.cxx:14-26` as `RecordValue{typeName, {{"0", ...}}}` → `VariantValue{tag, parentType="Result"|"Option"|..., args=[x]}`.
- **Zero-arg constructors** (`Less`/`Equal`/`Greater`) currently atoms-registered in `adt.cxx:30-37` → zero-arg `VariantValue{parentType="Ordering"}`.
- **User-declared variants** from `type X = Foo(...) | Bar | ...` — currently lower to the same `RecordValue` shape via `execTopLevel` TypeDef handling — emit `VariantValue`.
- **`UpperIdentifier` fallback** at `evaluator.cxx:1114`: when the identifier names a *known variant* (looked up against a registry populated by TypeDef evaluation), construct `VariantValue{tag=node.name, parentType=<looked-up>}`. If it names a known module, see Phase 3. Otherwise it's a genuine error (no more silent atom fallback).

`parentType` is known when construction goes through a registered constructor; for the bare-`UpperIdentifier` case it's resolved from the variant registry built during TypeDef evaluation.

**`None` stays `NoneValue`.** It already has its own representation and that's fine; pattern matching already special-cases it. Optionally unify it as `VariantValue{tag="None", parentType="Option"}` later — a follow-up, not required for this plan.

### Pattern matching changes

`matchPattern` in `evaluator.cxx`:
- `ConstructorPattern{name="Ok", args=[p]}` matches `VariantValue{tag="Ok", args=[v]}` by recursing `p` vs `v`.
- `ConstructorPattern{name="Less", args=[]}` matches zero-arg `VariantValue{tag="Less"}`.
- The current `pat.name == "Atom"` guard for atoms (evaluator.cxx:1531) is no longer reached by variants.

### Dispatch fix (this is the payoff)

`evaluator.cxx:653-658` becomes:

```cpp
} else if (auto* var = std::get_if<VariantValue>(&receiver->data)) {
    receiverType = var->parentType.empty() ? var->tag : var->parentType;
}
```

`AtomValue` is removed from the dispatch chain entirely. Calling a method on `:foo` is now a type error rather than silently mis-dispatching on the atom's name.

## Phase 2 — `ModuleValue` for module references

Add:

```cpp
struct ModuleValue { std::string name; };  // "Math", "IO", "File"
```

`UpperIdentifier` that resolves to a known module produces `ModuleValue`, not an atom. `Module.method(...)` dispatch already works via namespace qualification; the standalone-module-as-value case (rare) now has a proper representation.

**Open question Q1** (see below): if modules never need to be first-class values in Kex, this can stay compile-time-only and skip the runtime value entirely. Recommend starting with the runtime value for simplicity, dropping it later if unused.

## Phase 3 — Remove the type-tag-via-atom hack

`to(String)` / `as(...)` / `is_a?(...)` currently materialize `atom("String")` so patterns can compare by name. Replace with one of:

- **(a) `TypeTagValue{typeName}`** — a real value type. Minimal change to current shape, still materializes a value.
- **(b) Primitive type-test ops** — `x.is_a?(String)` and `case x of String => ...` lower to a runtime type check that never constructs a "type value". Cleaner; recommended.

Either way, `atom("String")` stops appearing in the evaluator.

## Phase 4 — Restrict genuine atoms

After Phases 1–3, `AtomValue` is used **only** for genuine symbolic atoms (`:foo`, `:read`, `:write`, map-key sugar `host: ...` → `:host`). Tighten:

- The type `Atom` is a distinct nominal type (not a union of tags).
- Pattern matching on `:foo` only matches atoms.
- Atoms are equal only to atoms with the same name.
- The `ident:` map-key sugar at `src/parser/parser.cxx:2174-2179` is unchanged.

## Phase 5 — BEAM lowering (per concept, explicit)

This is the only place atoms re-converge, and it's deliberate. Update `src/codegen/core_erlang.cxx`:

| Kex IR concept                         | Core Erlang emission                       |
| -------------------------------------- | ------------------------------------------ |
| `Atom "foo"` (genuine)                 | `'foo'`                                    |
| `Variant{tag="Ok", args=[x]}`          | `{'Ok', X}` (tagged tuple, tag is atom)    |
| `Variant{tag="Less", args=[]}` (0-arg) | `'Less'` (bare atom — Erlang convention)   |
| `Module "Math"`                        | `'math'` (module atom, used in `call`/`apply`) |
| `Bool true` / `false`                  | `'true'` / `'false'`                       |
| `None`                                 | `'none'`                                   |

Two implementation rules in the emitter:

1. Each IR value kind has its own `emitX` branch calling `erlAtom` with a comment explaining *why* an atom is the right BEAM representation for that concept.
2. The `erlAtom` helper itself is *only* called from those per-concept branches — never inline at an expression emission site. That makes the mapping auditable.

**Reverse direction (FFI):** when an Erlang term enters Kex (e.g. return value from a `call 'erlang':'...'`), the wrapping layer re-classifies a bare atom as Variant/Module/genuine-Atom based on the *declared Kex type* of the call site. Without a declared type, a bare atom stays an `Atom` — the safe default.

## Phase 6 — Tests & prelude migration

- `tests/codegen_test.cxx`: the "emits true as atom 'true'" / "emits false" / "emits none" tests stay valid as *lowering* tests. Add new tests: `Ok(5)` lowers to `{'Ok', 5}`; zero-arg `Less` lowers to `'Less'`; `:foo` lowers to `'foo'`; module call `Math.sqrt(2)` lowers to `call 'math':'sqrt'(2)`.
- `tests/interpreter_test.cxx`: replace `assertEqual(std::get<AtomValue>(result->data).name, "Less")` (line ~1201) and the `Nothing` assertion (~789) with `VariantValue` checks.
- `tests/semantic_test.cxx:750` (`traits.satisfies(Type::atom(), "Showable")`) stays — atoms still exist, just genuinely.
- `src/interpreter/stdlib/adt.cxx`: register all variants uniformly as `VariantValue`; the `Ok`/`Just`/`Error` record-vs-atom inconsistency disappears.
- `src/interpreter/stdlib/file.cxx:10-13`: the `:Write`/`:Append`/`:ReadWrite` checks stay — those are genuine atoms.
- `src/interpreter/stdlib/list.cxx:55, 446`: the atom branch in ` sortBy`/comparators needs review — if it was catching `Less`/`Equal`/`Greater`, switch to `VariantValue`.

## Ordering & risk

Phases are incremental; BEAM works at every intermediate step because Phase 5's lowering changes land in lockstep with each IR split.

- **Phase 1** is the bulk of the work and the biggest payoff (kills the dispatch hack). Self-contained: touches `value.hxx`, the variant construction sites, `matchPattern`, and dispatch. BEAM change is just "emit `VariantValue` as tagged tuple / bare atom" in `core_erlang.cxx`.
- **Phase 2** is small and isolated.
- **Phase 3** touches pattern matching but in a localized way.
- **Phase 4** is mostly deletion of now-dead code paths.
- **Phase 5** runs alongside each phase; only its final consolidation is standalone.
- **Phase 6** runs incrementally with each phase.

## Open questions (decide before starting)

**Q1 — Zero-arg variant on BEAM:** bare atom `'Less'` (Erlang/OTP convention, dialyzer-friendly) or empty tagged tuple `{'Less'}` (more uniform with the N-arg case)? Recommend bare atom.

**Q2 — First-class module values:** does Kex need `Math` as a passable value, or only as a syntactic receiver prefix? If only the latter, `ModuleValue` may not need to exist at runtime — pure compile-time namespace resolution is simpler. Recommend starting with the runtime value and dropping it if no use case appears.

**Q3 — Type tags:** option (a) materialize as `TypeTagValue`, or (b) make type-test ops primitive and never materialize? Recommend (b).

**Q4 — Unify `None` as `VariantValue{tag="None", parentType="Option"}`?** Cleaner but a breaking change to the value layer's special-case handling. Recommend deferring to a follow-up; `NoneValue` staying distinct doesn't block this plan.

## Files touched (summary)

- `src/interpreter/value.hxx` / `.cxx` — add `VariantValue`, (optionally) `ModuleValue`, (optionally) `TypeTagValue`; update `toString`/`toRepr`/`inspect`/`typeName`/`valuesEqual`.
- `src/interpreter/evaluator.cxx` — `UpperIdentifier` branch, dispatch chain, `matchPattern`, `to(...)` type-tag construction.
- `src/interpreter/stdlib/adt.cxx` — uniform variant registration.
- `src/interpreter/stdlib/list.cxx` — comparator atom branch review.
- `src/parser/parser.cxx` — no change (parser already produces distinct AST nodes; the conflation is downstream).
- `src/ast/ast.hxx` — no change.
- `src/semantic/typechecker.cxx` — tighten `AtomLiteral` typing to the genuine `Atom` type only; add variant type checking.
- `src/codegen/core_erlang.cxx` — per-concept emission branches; centralize `erlAtom` calls.
- `tests/interpreter_test.cxx`, `tests/codegen_test.cxx`, `tests/lexer_test.cxx`, `tests/semantic_test.cxx` — update expectations.
