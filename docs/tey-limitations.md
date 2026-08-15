# Tey limitations and follow-up work

This file records known constraints in the first working Tey implementation.
They are kept explicit so temporary workarounds do not become accidental API.

## Tey package-manager limitations

- The manifest reader still recognizes declarations by their leading word
  rather than by a grammar. It now reports the line a problem is on, refuses a
  declaration it does not know, and joins a declaration whose arguments span
  several lines — but it does not understand nesting beyond `group`, and a
  malformed argument list is reported as an unknown declaration rather than as
  the specific mistake.

  The intended fix is NOT a hand-written grammar in Tey: `package.kex` is Kex
  source, so the compiler should hand its AST to Kex code and Tey should read
  that. Most of it already exists — the prelude's `Parser` module parses Kex
  into `Program`/`Node`/`Expression` values with locations. Three things stand
  in the way, in increasing order of size:

  1. `Parser` drops the `do ... end` block passed to a call, so
     `bundle "demo" do ... end` parses to `Call(Identifier("bundle"),
     [LitString("demo")])` with the manifest's entire contents missing.
  2. `Expression`'s `Call` has no representation for named arguments, which
     `tey("greet", git: ..., tag: ...)` depends on.
  3. `Parser` exists only in the tree walker. Under `-R` it raises `undef` —
     there is no `kex_intrinsic_parser.erl` — and Tey runs on BEAM. Closing
     this means either exposing the C++ parser to the BEAM runtime through a
     NIF/port, or a Kex-native parser (the self-hosting path). Either is a
     project of its own, and worth doing for tooling generally (formatter,
     linter, docgen, macros) rather than for this reader alone.
- A dependency may ask for a RANGE — `tag: "~> 0.2"`, `"^1.0"`, `">= 0.3"` —
  and Tey lists the repository's tags and locks the highest that satisfies it,
  so the manifest keeps the constraint and the lockfile keeps the answer. An
  exact tag is still taken as written. What remains is everything BEYOND one
  dependency: two constraints on the same package are not unified, a
  dependency's own manifest is never read (no transitive dependencies), and
  with no graph to walk there is no cycle detection either.
- `tey lock` currently fetches repositories while resolving them because the
  content digest is computed from `git archive`. Separating resolution from
  cache population needs a remote/archive strategy or a clearly renamed
  command contract.
- Cache checkouts are not made read-only, so nothing stops a build from
  writing into a fetched dependency. (Creation itself is transactional now:
  the clone lands in a sibling `<commit>.partial` directory and is renamed
  into place only once the checkout succeeds, so a failed or killed run
  leaves nothing reachable under the content-addressed name.)
- `tey run`, `tey build` and `tey test` put every fetched dependency on the
  compiler's module search path, so `using Greet` resolves to the cached
  checkout — and a dependency the lockfile names but the cache lacks is
  reported (`run \`tey install\``) rather than surfacing as a missing module.
  Only DIRECT dependencies are on the path; a dependency's own dependencies
  are not, which needs transitive resolution first.
- `tey install --without GROUP` omits a group's dependencies from the fetch
  while leaving them in the lockfile, so turning the flag off later needs no
  re-resolve. Orphan-only `tey clean`, targeted `tey update NAME`, and target
  selection for build/run remain incomplete.
- `tey kex list` reads released versions from the repository's tags on every
  call. There is no cache, so listing needs the network, and a pre-release
  suffix (`0.3.0-rc1`) parses as its release (`0.3.0`) and sorts as one.
- Lockfile merge-driver mode currently regenerates from the working-tree
  manifest; it does not inspect and merge the `%O`, `%A`, and `%B` inputs.
- Toolchain handling covers the whole lifecycle: `tey kex list` shows what is
  released and what is installed, `tey kex install <version>` clones that tag
  into the cache and builds it, `tey kex install <src> <version>` does the
  same from a checkout, `tey kex use` selects, and `tey kex uninstall`
  removes — clearing the selection when it removes the selected one. Installs
  stage into `<version>.partial` and only a complete tree is renamed into
  place, so a failed build never replaces a working compiler. What is still
  missing: no checksum over an installed tree, so corruption AFTER
  installation goes unnoticed; a cached source checkout is never pruned; and
  `tey kex install` of an already-installed version rebuilds it rather than
  reporting that it is there.
- The Homebrew formula is HEAD-only until the first tagged Tey/Kex release has
  a stable source URL and checksum. During bootstrap it follows the
  `package-management` branch and should switch back to `main` after merge.
