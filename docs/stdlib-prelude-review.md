# Standard Library and Prelude Unification Review

## Status

The initial staged stdlib/prelude unification was structurally close to the
intended layout, but the review found semantic and backend regressions that
were not covered by the existing CTest suite. The fixes described below have
now been implemented and are guarded by regression tests.

The desired model is:

- all standard-library sources live under `src/stdlib`;
- `prelude.kex` selects the automatically visible subset;
- modules absent from the prelude, including `FS`, remain available through
  qualified names, but their members require `using` before they can be used
  bare;
- ADT constructors belong to their defining module and obey its visibility;
- the tree interpreter and BEAM backend resolve the same program identically.

No constructor, constant, or stdlib module spelling is special-cased by the
compiler. Resolution is driven by source declarations and imported interface
metadata. Runtime intrinsic adapters may translate declared values (for
example file modes) to host APIs, but they do not make those names visible to
the language.

## 1. Qualified uppercase names are misclassified as constructors

### Problem

The typechecker and BEAM lowering currently recognize a qualified nullary ADT
constructor using only the capitalization of its member:

```kex
FS.Read
```

Any zero-argument qualified member beginning with an uppercase letter is
therefore treated as a constructor. This also captures ordinary constants and
nonexistent names:

```kex
Math.PI
Console.GREEN
MissingModule.NotAConstructor
```

### Consequences

- `Math.PI` evaluates to the atom `PI` on BEAM instead of the floating-point
  constant.
- `Console.GREEN` is inferred as the type `Green` instead of `String`.
- nonexistent qualified names pass semantic checking;
- bare `Read` must not become visible merely because the FS interface is
  available;
- tree and BEAM execution disagree.

### Confirmed reproductions

```sh
build/kex --no-colors examples/io-math.kex
build/kex -R --no-colors examples/io-math.kex
```

The tree interpreter prints the numeric value of `Math.PI`; BEAM prints `PI`.

```sh
build/kex -R --no-colors spec/console.kex
```

This fails because `Console.GREEN` is inferred as `Green` rather than
`String`.

A program containing only `MissingModule.NotAConstructor` passes `--check`.
Tree execution rejects it, while BEAM lowers it to an atom and succeeds.

### Required correction

Remove capitalization-based handling from both the typechecker and IR
lowering. Qualified constructors must be resolved from authoritative module
interface metadata.

A nullary constructor should be represented as a real exported module value
or zero-arity accessor with its refined result type. Then normal namespace
resolution can distinguish:

- `FS.Read`, a known constructor;
- `Math.PI`, a known constant;
- `MissingModule.NotAConstructor`, an unknown member.

Qualification and import are independent: `FS.Read` is valid whenever the
`FS` module can be resolved, while bare `Read` is valid only in a scope that
has imported it with `using FS`.

## 2. Constructor visibility and exports are inconsistent

### Problem

The interpreter now scopes constructors under their module, but the module
export bookkeeping is incomplete.

Direct type definitions add their constructors to the public export set.
Type definitions inside `public` or `private` visibility blocks add only the
type name, not their constructors.

### Consequences

- a constructor declared in `public do` is not imported by `using Module`;
- a constructor declared in `private do` remains accessible as
  `Module.Constructor`;
- interpreter behavior does not match the declared visibility.

For example, the following public constructor is unavailable after import:

```kex
module Modes do
  public do
    type Mode = Read | Write
  end
end

using Modes

main do
  Read
end
```

Conversely, `Modes.Hidden` currently succeeds when `Hidden` belongs to a type
declared in a private block.

### Required correction

Use one shared type-member classifier when:

- registering constructor bindings;
- building module exports;
- recording private names;
- producing semantic interfaces;
- emitting BEAM module interfaces.

Public constructors must be exported, private constructors must be protected,
and both qualified and imported access must consult the same metadata.

## 3. Type aliases can accidentally export constructor names

### Problem

The interpreter export pass treats every RHS variant of a type definition as
a constructor. This includes aliases such as:

```kex
type FilePath = String
```

As a result, `FS` can export a runtime binding named `String`, even though
`String` is the target of the `FilePath` alias and not a constructor owned by
`FS`.

### Consequences

`using FS` can overwrite or conflict with the existing `String` binding.
Alias classification also differs between source-interface extraction and
runtime module execution.

### Required correction

Centralize alias-versus-ADT classification and reuse it everywhere. Alias RHS
names must never become constructor bindings or module exports.

The implementation should also document the language rule for ambiguous
single-RHS declarations so the parser, semantic layer, interpreter, and
backend cannot develop separate heuristics.

## 4. `prelude.kex` is not currently a valid or strictly checked manifest

### Problem

`src/stdlib/prelude.kex` looks like ordinary Kex source:

```kex
using Algebra
using Optional
using String
```

However, many referenced files do not define modules with those names.
The loader manually scans the file line by line and translates each `using`
name into a sibling filename.

Checking the file as normal Kex source produces missing-module warnings and
ambiguous-import errors. The manual scanner also silently ignores malformed
or unsupported lines.

### Consequences

- `prelude.kex` is not valid under the language semantics it appears to use;
- typos and unsupported syntax may be ignored rather than rejected;
- build logic and documentation can drift from the actual manifest;
- the `check-prelude` target still infers membership by excluding hard-coded
  filenames instead of using the manifest as its source of truth.

### Required correction

Formalize `prelude.kex` as a strict toolchain manifest:

- accept only comments, blank lines, and bare `using Module.Name` entries;
- reject malformed lines and duplicate entries with source locations;
- resolve every entry to exactly one stdlib source;
- make the `check-prelude` build target validate the expanded prelude;
- make prelude building, source hashing, WASM embedding, installed-layout
  checks, and `check-prelude` consume the same parsed manifest.

If `prelude.kex` is intended to be ordinary executable Kex instead, the
stdlib fragments must first become real importable modules. Mixing ordinary
syntax with bespoke silent interpretation should be avoided.

## 5. Tests do not cover the affected semantic boundary

### Problem

All 17 CTest suites pass despite the confirmed regressions. Current coverage
tests `FS.Read` primarily through the new successful path, but does not cover
the negative import case or existing uppercase module constants on BEAM.

### Required tests

Add tree and BEAM parity tests for:

- `Math.PI`;
- `Console.GREEN`;
- known qualified nullary constructors;
- nonexistent qualified uppercase members;
- `FS.Read` with `using FS`;
- `FS.Read` without `using FS`;
- bare `Read` before and after `using FS`;
- public and private module constructors;
- aliases inside modules;
- malformed and duplicate prelude-manifest entries.

The installed-layout test should copy the stdlib recursively so nested opt-in
modules are covered as well.

## 6. Packaging cleanup

The Makefile no longer defines `KEXLIBDIR`, but its uninstall message still
references that variable. The message should describe only the unified
stdlib destination.

## Proposed implementation sequence

1. Define shared metadata for aliases, ADTs, scoped constructors, constructor
   arities, and visibility.
2. Populate that metadata for local source modules, source-based stdlib
   interfaces, and prebuilt KexI interfaces.
3. Resolve qualified constructors through normal module member lookup.
4. Remove the uppercase shortcuts from the typechecker and IR lowering.
5. Update interpreter registration and BEAM module emission to use the same
   constructor metadata.
6. Replace the permissive manifest scanner with strict parsing and route all
   prelude consumers through it.
7. Add the regression tests listed above.
8. Fix packaging messages and installed-layout coverage.
9. Run the complete CTest suite, relevant spec suites on both backends,
   `git diff --check`, and `git diff --cached --check`.
