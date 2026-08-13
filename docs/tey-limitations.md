# Tey limitations and follow-up work

This file records known constraints in the first working Tey implementation.
They are kept explicit so temporary workarounds do not become accidental API.

## Kex compiler and module-system limitations

- A qualified reference such as `Tey.Git.execute(...)` does not currently make
  the cross-file BEAM compiler load and emit `tey/git.kex`. Tey must also write
  `using Tey.Git, as: Git` to establish the source dependency. Qualified module
  references should become sufficient dependency edges.
- Cross-file dependency discovery currently stops before all transitive Tey
  modules are emitted as companions of `main.kx.beam`. The Tey Makefile
  compiles every lowercase `src/tey/*.kex` file explicitly, and the launcher
  places the complete ebin directory on the BEAM code path.
- `using Module, only: []` can force dependency loading without importing
  members, but it is an implementation-shaped workaround and is not used by
  Tey. Explicit named imports establish dependency edges until qualified
  references load source files by themselves.
- Record/type resolution can confuse same-named nested records and same-named
  fields imported from sibling modules. Tey therefore uses distinctive names
  such as `ManifestDependency` and `LockedDependency`. Nested type identity
  should be fully module-qualified so `Tey.Manifest.Dependency` and
  `Tey.Lockfile.Dependency` can coexist.
- Named record patterns in function parameters currently generate invalid BEAM
  field accessor calls for Tey's imported records. Resolver functions accept a
  typed parameter and destructure it immediately in the body until parameter
  pattern lowering is fixed.
- An `if`/`elif` chain inside the mutable `text.lines.each` manifest fold
  currently lowers only its first branch on BEAM. The bootstrap reader uses
  independent, mutually exclusive `if` blocks until `elif` lowering in captured
  mutable folds is fixed.
- Compiling a nested source file directly chooses that file's directory as the
  module root. For example, compiling `tey/src/tey/commands.kex` searches for
  `Tey.Lockfile` below `tey/src/tey/tey/`. Building from `tey/src/main.kex`
  works. The compiler needs an explicit source-root option for tools and build
  systems.
- `Process.run` keeps stdout and stderr separate in the tree walker, while the
  BEAM port backend currently combines stderr into stdout. Its public result is
  stable, but stream fidelity differs by backend.

## Tey package-manager limitations

- The manifest reader currently recognizes the supported declarations one line
  at a time. It does not yet use the `Parsing` combinators for the full grammar,
  report source locations, reject unknown declarations, or handle declarations
  split across lines.
- Git tags are pinned exactly. The package-level Kex requirement is checked,
  but dependency semver-range tag discovery, highest-compatible
  selection, duplicate constraint unification, transitive manifests, and cycle
  detection are still to be implemented.
- `tey lock` currently fetches repositories while resolving them because the
  content digest is computed from `git archive`. Separating resolution from
  cache population needs a remote/archive strategy or a clearly renamed
  command contract.
- Cache checkout creation is not yet transactional or made read-only. A failed
  clone can leave a partial directory that a later run must detect and repair.
- `tey install --without GROUP`, orphan-only `tey clean`, targeted
  `tey update NAME`, target selection for build/run, and transitive compiler
  search paths remain incomplete.
- Lockfile merge-driver mode currently regenerates from the working-tree
  manifest; it does not inspect and merge the `%O`, `%A`, and `%B` inputs.
- Source toolchain installation builds compiler, runtime, and stdlib together,
  but does not yet stage and atomically rename the completed installation.
- The Homebrew formula is HEAD-only until the first tagged Tey/Kex release has
  a stable source URL and checksum.
