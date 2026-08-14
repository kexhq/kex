# Tey limitations and follow-up work

This file records known constraints in the first working Tey implementation.
They are kept explicit so temporary workarounds do not become accidental API.

## Kex compiler and module-system limitations

- `Process.run` keeps stdout and stderr separate in the tree walker, while the
  BEAM port backend currently combines stderr into stdout. Its public result is
  stable, but stream fidelity differs by backend.
- Everything is built under the root folder of the new package, instead of having an output folder and putting it there (maybe it should also be included in the `.gitignore`, too)

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
  a stable source URL and checksum. During bootstrap it follows the
  `package-management` branch and should switch back to `main` after merge.
