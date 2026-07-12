  # KexI BEAM Metadata and Compiled Module Loading

  ## Summary

  Implement the module plan's Phase 5 metadata:

  - KexI is a single versioned ETF custom BEAM chunk containing both exported type signatures and module structural metadata.
  - BEAM module names remain Kex.BinaryTree, Kex.Http.Router, etc.
  - /load file.kx.beam reads the entry metadata, loads its exact companion modules, and imports their interfaces into the BEAM REPL.
  - BEAMs without a KexI chunk are rejected by /load with a "recompile with current kex" diagnostic.

  ## Metadata and Compiler Changes

  - Add a shared BEAM metadata subsystem that:
      - Reads and rewrites IFF/BEAM chunks while preserving existing chunks and padding.
      - Encodes and decodes the supported ETF subset.
      - Replaces existing KexI chunks when recompiling.
      - Rejects unsupported newer schema versions with a clear compatibility error.
      - Reads older schema versions: a v2 compiler reads v1 chunks by defaulting any absent v2 fields.  Newer compilers always read older chunks without requiring a recompile of dependencies.

  - Define KexI version 1 containing two sections within a single ETF term:

    **Type interface section:**
      - Public function names, BEAM arities, foulness, parameters, and return types.
      - Public constants as zero-arity exports.
      - Exported types, generic parameters, aliases, and constructors.
      - make method signatures, receiver type, arity, and foulness.
      - Structural type terms rather than formatted signature strings, including primitives, named/generic types, functions, tuples, lists, maps, optionals, unions, constrained types, Never, and unknown/dynamic types.

    **Structural metadata section:**
      - Module identity, BEAM atom, foul flag, and public/private export metadata.
      - Module role: either entry (with companion manifest) or companion (with entry back-pointer — the BEAM atom of the entry module this companion belongs to).
      - Submodule and re-export relationships.
      - Record names and ordered field layouts.
      - ADT names, type parameters, constructor tags, payload arities, and ownership.
      - ADT display metadata: for each constructor, its human-readable name and the ordered field names (for record-style constructors) or positional arity (for tuple-style constructors), so the display system can render values without executing module code.
      - make method ownership and emitted BEAM function mapping.
      - For entry modules, the companion manifest: for each companion, its BEAM atom, relative path (may include subdirectories, e.g. http/router.beam), and expected interface hash recorded at compilation time.  A single-file module with no submodules is an entry with an empty companion manifest.

  - Collect metadata from the analyzed AST and SemanticDB, not from emitted Core text.
      - Preserve the analyzed interface after semantic checking for compilation.
      - With --no-check, emit a KexI chunk with an empty type interface section (no functions, no type exports).  The structural metadata section is still populated so that record layouts, ADT ownership, companion manifests, and /load's module graph remain available.  Downstream typed access fails with "module was compiled with --no-check; recompile to enable type information" rather than silently using unknown everywhere.
      - Attach the KexI chunk after each erlc +from_core invocation.
      - Attach the compilation-unit companion manifest to the renamed entry .kx.beam.

  ## Build Integrity

  Every KexI chunk contains an interface hash (a SHA-256 truncated to 128 bits).  The hash is computed by serializing the full ETF payload *without* the hash field, hashing that byte sequence, then inserting the resulting hash into the final serialized chunk.  This avoids dependence on the hash field's byte position within the ETF encoding.

  The entry module's companion manifest records, for each companion, the interface hash that was current at compilation time.  At /load time, the loader reads each companion's actual chunk hash and compares it against the manifest.  A mismatch means the companion was recompiled independently and is out of sync with the entry; /load rejects the entire unit with a diagnostic naming the stale companion(s).

  For future incremental compilation: a downstream compilation unit can record the interface hashes of its dependencies.  When recompiling, if a dependency's hash has not changed, its dependents do not need re-lowering.  The chunk schema supports this from version 1 even though the compiler does not yet implement incremental builds.

  ## Loading and REPL Integration

  - Replace the abandoned directory-scanning/loadset approach.
  - For /load compiled.kx.beam:
      1. Read its KexI chunk.  If absent, reject with "recompile with current kex."
      2. Check the module role.  If it is a companion, reject with "this is a companion module of <entry atom>; load the entry module instead."
      3. Resolve companion paths relative to the entry BEAM's directory (paths may include subdirectories, e.g. http/router.beam, but may not escape the entry directory via .. or absolute paths).
      4. Validate that every declared companion exists, its embedded module identity matches the manifest, and its interface hash matches the manifest's recorded hash.  On mismatch, reject the entire load and name the stale companion(s).
      5. Hot-load companions in manifest order (the compiler writes the manifest in dependency order — a companion that another companion calls at BEAM module-load time appears earlier).  Load the entry last.
      6. Import all KexI interfaces into a compiled-interface registry used by semantic analysis, lowering, completion, display, and subsequent REPL inputs.

  ### Reloading and Unloading

  - **/load of an already-loaded compilation unit** replaces the previous version:
      1. Remove all registry entries for the previous version's modules.
      2. Invalidate any REPL type environment entries whose type references a module being replaced — those bindings become untyped (the value is still accessible but dispatch treats the type as unknown until the user rebinds).
      3. Proceed with the normal /load sequence.  BEAM's code server handles the hot-code swap of the modules themselves.

  - There is no /unload command in version 1.  Reloading covers the recompile-and-retry workflow.  Full unloading (removing modules from the code server, clearing registry entries, invalidating bindings) can be added later without schema changes.

  - Extend lowering with external compiled interfaces:
      - BinaryTree.fromList(...) lowers directly to 'Kex.BinaryTree':fromList(...).
      - Receiver calls such as tree.inOrder use KexI method ownership plus receiver types.
      - Record access and ADT rendering use chunk-provided layouts and constructor ownership.

  ### Method Dispatch with Loaded Modules

  When a REPL expression uses UFCS on a binding whose type is known (e.g. tree.inOrder where tree has type Tree(a)), dispatch proceeds in this order:

  1. **Exact receiver match.**  Search KexI interfaces of all loaded modules for a make method whose receiver type unifies with the binding's type.  If exactly one module owns a matching method, emit a call to that module's BEAM function.
  2. **Supertype / trait match.**  If the receiver type implements a trait that a loaded module's method requires, and no exact match exists, use the trait-constrained method.  Ambiguity (two loaded modules both define .foo for the same trait) is a compile error at the REPL input, not a silent fallback.
  3. **Prelude / builtin fallback.**  Only if no loaded module claims the method, fall through to List/Map/String/prelude dispatch as today.

  Dispatch never searches by method name alone.  If the receiver type is unknown (untyped binding from a --no-check module, or a REPL binding whose type was lost), the call is rejected with "cannot resolve method .foo on untyped value; add a type annotation or recompile the source module with type checking enabled."

  For union receivers (e.g. Tree(a) | Nil), every variant must resolve to the same method in the same module, or the call is rejected.  Generic receivers unify against the method's declared receiver type parameter.

  ### REPL Type Environment

  The REPL maintains a type environment (a map from binding name to its inferred type term) parallel to the process-dictionary value store.

  - **On let binding:** after evaluating `let tree = BinaryTree.fromList(...)`, the compiler's return-type annotation from KexI is stored as tree's type in the environment.
  - **On rebinding:** `let tree = otherTree.merge(tree2)` overwrites the previous type entry for tree with the new return type.  The old type is discarded, not stacked.
  - **On destructuring:** `let (a, b) = pair` stores the projected component types for a and b if the source type is a known tuple or record.
  - **On subsequent input:** before compiling the next REPL expression, inject all live bindings as typed external declarations so that method dispatch, field access, and completion resolve correctly.
  - **Type loss:** if a binding's value comes from a raw BEAM call (e.g. :erlang.element(1, x)) or a --no-check module, its type is unknown.  The environment records this explicitly so dispatch can reject ambiguous calls rather than guessing.

  ### Display of Loaded ADTs and Records

  When companions are loaded, their KexI ADT display metadata is registered with the value formatter:

  - For each ADT constructor, the chunk provides the constructor's human-readable name (e.g. "Node", "Leaf"), its tag atom as emitted on BEAM, and the ordered field names (for record-style constructors like `Node(value:, left:, right:)`) or positional arity (for tuple-style constructors like `Some(value)`).
  - For records, the chunk provides the record name and ordered field names/types.
  - The formatter pattern-matches the BEAM representation (tagged tuples) against registered constructor metadata to produce Kex-syntax output (e.g. `Node(value: 5, left: Leaf, right: Leaf)` rather than `{'Node', 5, 'Leaf', 'Leaf'}`).
  - If a value's tag is not registered (e.g. from a raw BEAM module), it falls through to the existing tuple/term display.
  - Registration happens at load time from chunk data alone — no module code is executed, no main block runs, no side effects occur.

  ## Test Plan

  - Chunk codec tests:
      - ETF round trips every supported type form.
      - BEAM rewriting preserves standard chunks and replaces the KexI chunk deterministically.
      - OTP beam_lib:chunks/2 can read the custom chunk.
      - Corrupt payloads and unsupported versions report clean errors.
      - Interface hash is stable across identical payloads and changes when any field changes.

  - Compilation tests:
      - A multi-module source emits unchanged Kex.* module atoms and filenames.
      - Every companion contains a correct module-local KexI with role companion and correct entry back-pointer.
      - The entry KexI has role entry and its manifest lists exactly the generated companions with correct hashes.
      - Private exports are excluded from the type interface section but represented appropriately in the structural metadata section.
      - --no-check emits KexI with an empty type interface section; structural metadata is still present and valid.
      - Recompiling a single companion without recompiling the entry produces a hash mismatch that /load detects.
      - ADT display metadata in the chunk matches the source-declared constructors and field names.

  - Dispatch tests:
      - Exact receiver match: tree.inOrder resolves to BinaryTree's method, not a same-named prelude method.
      - Trait match: a loaded module's trait-constrained method is found when the receiver implements the trait.
      - Ambiguity: two loaded modules defining .foo for the same receiver type produce a compile error, not a silent pick.
      - Unknown receiver: UFCS on an untyped binding is rejected with a clear diagnostic.
      - Union receiver: .foo on Tree(a) | Nil requires both variants to resolve to the same method.
      - Prelude fallback: list.map still works when no loaded module claims .map for List.

  - Reload tests:
      - /load of an already-loaded unit replaces registry entries and invalidates stale bindings.
      - After reload, new bindings use the updated types and dispatch.
      - Bindings whose types were invalidated by reload are reported as untyped on next use.

  - Display tests:
      - Loaded ADT values render with Kex constructor syntax, not raw BEAM tuples.
      - Record values render with field names in declaration order.
      - Values from unregistered tags fall through to tuple display.
      - Display metadata survives reload (new metadata replaces old).

  - REPL type environment tests:
      - Binding type persists across inputs: let x = ... in one input, x.method in the next resolves correctly.
      - Rebinding overwrites the type: let x = 1 then let x = "hello" gives x type String, not Int.
      - Destructuring assigns component types.
      - Type loss from raw BEAM calls is recorded as unknown and dispatch rejects ambiguous methods.

  - Native and BEAM module tests:
      - Cross-module calls, aliases, selective imports, re-exports, foulness, records, custom ADTs, generic signatures, and colliding make methods resolve from chunks without source.

  - Exact REPL acceptance case:
      - Compile examples/binary_tree.kex.
      - Start kex -i.
      - /load ./binary_tree.kx.beam.
      - let a = BinaryTree.fromList([4,1,33,5,3,7,34,3412,11,2]) returns and displays as Tree with constructor syntax (not raw tuples).
      - a.inOrder, a.size, a.hasValue?(33), a.transform(...), and a.fold(...) all work.
      - Loading does not execute the example's main.
      - /load companion.beam directly is rejected with a "load the entry instead" diagnostic.
      - Missing companion, mismatched identity, hash mismatch, corrupt chunk, and newer schema cases produce their specified diagnostics.

  - Adversarial cases:
      - Circular module references between companions within a compilation unit.
      - Method name collisions across two independently loaded compilation units.
      - A module with zero public exports (KexI is present but type interface section is empty; /load succeeds).
      - Large type signatures (deeply nested generics, wide unions) round-trip without ETF size issues.
      - Concurrent /load of two compilation units sharing a companion BEAM atom name (second load detects the conflict and rejects).
      - Companion path containing .. or an absolute path is rejected at compile time.
      - A companion whose file exists but contains a different module's identity is rejected at load time.

  ## Assumptions

  - ETF is the canonical chunk encoding.
  - The chunk begins at schema version 1.
  - The four-byte chunk ID is exactly KexI.
  - Every KexI chunk carries a module role: entry or companion.  Companions carry a back-pointer to their entry module's BEAM atom.
  - The compiler writes the companion manifest in dependency order so that /load can hot-load in manifest order without a separate topological sort.
  - Companion paths in KexI are relative to the entry BEAM's directory.  They may include subdirectories (e.g. http/router.beam) but may not use .. or absolute paths.  Validation is enforced at compile time and again at load time.
  - Interface hashes use SHA-256 truncated to 128 bits, stored as a 16-byte binary in the ETF payload.  The hash is computed over the payload serialized without the hash field, then inserted into the final form.
  - A single-file module with no submodules is an entry with an empty companion manifest — every .kx.beam is an entry.
  - Schema evolution is additive: newer compilers always read older chunk versions by defaulting absent fields.  Only chunks from a *newer* schema than the compiler understands are rejected.
  - Chunk metadata is an interface and dispatch description, not executable code and not a replacement for normal BEAM exports.
  - BEAMs without a KexI chunk are rejected by /load rather than loaded in a degraded mode.  This avoids a parallel code path that every downstream feature must handle.
  - --no-check produces valid .kx.beam files with structural metadata but no type information.  This is intentional — it supports the development workflow where you want fast compilation and structural loading without type checking.  Distributed binaries should always be compiled with type checking enabled.
