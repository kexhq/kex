# Tey limitations and follow-up work

This file records known constraints in the first working Tey implementation.
They are kept explicit so temporary workarounds do not become accidental API.

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
- Cache checkouts are not made read-only, so nothing stops a build from
  writing into a fetched dependency. (Creation itself is transactional now:
  the clone lands in a sibling `<commit>.partial` directory and is renamed
  into place only once the checkout succeeds, so a failed or killed run
  leaves nothing reachable under the content-addressed name.)
- `tey install --without GROUP`, orphan-only `tey clean`, targeted
  `tey update NAME`, target selection for build/run, and transitive compiler
  search paths remain incomplete.
- `tey kex list` reads released versions from the repository's tags on every
  call. There is no cache, so listing needs the network, and a pre-release
  suffix (`0.3.0-rc1`) parses as its release (`0.3.0`) and sorts as one.
- Lockfile merge-driver mode currently regenerates from the working-tree
  manifest; it does not inspect and merge the `%O`, `%A`, and `%B` inputs.
- Source toolchain installation builds compiler, runtime, and stdlib
  together, and stages them: CMake installs into `<version>.partial` and only
  a complete tree is renamed into place, keeping the previous installation
  until the new one lands. Still missing is a checksum over the installed
  tree, so a toolchain that was corrupted AFTER installation is not detected.
- The Homebrew formula is HEAD-only until the first tagged Tey/Kex release has
  a stable source URL and checksum. During bootstrap it follows the
  `package-management` branch and should switch back to `main` after merge.
