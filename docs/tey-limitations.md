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

  There is a middle option that is available TODAY: the stdlib's `Parsing`
  combinators. Unlike `Parser` they run on BEAM — `spec/stdlib/parsing.spec.kex`
  is part of the opt-in BEAM suite — so a combinator grammar for `package.kex`
  would give real positions, real error messages, and nesting, without waiting
  on any of the three blockers above. What it does not give is the guarantee
  the AST path does: a hand-written grammar can drift from what the compiler
  actually accepts, and then a manifest that Kex parses is one Tey rejects.
  Worth taking if the reader needs to grow much further before the AST path is
  reachable; not worth taking to save the line reader as it stands.
- A dependency may ask for a RANGE — `tag: "~> 0.2"`, `"^1.0"`, `">= 0.3"` —
  and Tey lists the repository's tags and locks the highest that satisfies it,
  so the manifest keeps the constraint and the lockfile keeps the answer. An
  exact tag is still taken as written. What remains is everything BEYOND one
  dependency: two constraints on the same package are not unified, a
  dependency's own manifest is never read (no transitive dependencies), and
  with no graph to walk there is no cycle detection either.
  `docs/tey-resolver-plan.md` is the committed design for closing this, together
  with the OTP gap below — they share a constraint-intersection step, and doing
  either first without the other means writing it twice.
- Nothing records which Erlang/OTP a package needs or was built with. A `.beam`
  cannot be loaded by an OTP older than the `erlc` that produced it, and while
  `kex` now diagnoses that before spawning `erl` (`KEX_RUNTIME_OTP_FLOOR`), a
  manifest cannot declare an OTP requirement and `tey.lock` does not record the
  OTP that resolved it — so a locked, reproducible checkout can still behave
  differently on two machines. Planned in `docs/tey-resolver-plan.md`.
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
  call. There is no cache, so listing needs the network. Pre-releases are
  understood now — `0.4.0-rc.1` parses, sorts below `0.4.0` and above
  `0.4.0-beta`, and is hidden from the list unless `--pre` is passed — but a
  version is still discovered only by asking git for tags, so a tag with no
  published release looks installable and only fails at install time.
  `tey kex install --pre` installs the newest version of any channel, and a
  pre-release named outright installs without the flag; bare `tey kex install`
  stays on the newest stable.
- Lockfile merge-driver mode currently regenerates from the working-tree
  manifest; it does not inspect and merge the `%O`, `%A`, and `%B` inputs.
- Toolchain handling covers the whole lifecycle: `tey kex list` shows what is
  released and what is installed, `tey kex install <version>` downloads the
  published archive for the machine (falling back to building the tag from
  source when there is none), `tey kex install <src> <version>` builds from a
  checkout, `tey kex use` selects, and `tey kex uninstall` removes — clearing
  the selection when it removes the selected one. Installs stage into
  `<version>.partial` and only a complete tree is renamed into place, so a
  failed install never replaces a working compiler. What is still missing: no
  checksum over an INSTALLED tree, so corruption after installation goes
  unnoticed; neither a cached source checkout nor a downloaded archive is ever
  pruned; and `tey kex install` of an already-installed version reinstalls it
  rather than reporting that it is there.
- A downloaded archive is verified against the `.sha256` published beside it,
  and a mismatch fails the install outright rather than falling back to a
  source build. But the checksum comes from the same host as the archive, so
  it proves transfer integrity, not provenance — signing, or a checksum
  recorded somewhere other than the release, is the missing half.
- Downloading and hashing shell out: `curl` (then `wget`), `tar`, and
  `sha256sum` (then `shasum`). Tey does no HTTP of its own, shows no progress
  on a several-tens-of-megabytes download, and cannot resume an interrupted
  one — it re-fetches from the start. A machine with none of those binaries
  gets a clear error and the source-build fallback, which needs a compiler
  toolchain instead.
- Nothing checks that a downloaded Kex can actually RUN before selecting it.
  The macOS and Linux archives link GMP, PCRE2, Boost and readline
  dynamically, against whatever the release runner had; a machine missing them
  (or holding incompatible versions) gets a dynamic-loader error out of the
  first compile rather than a diagnosis from `tey kex install`.

## Tey and the packaging that installs it

- Homebrew installs Tey and puts no `kex` BINARY on PATH: two compilers under
  two managers drift apart on the first upgrade of either. What lands on PATH
  is `tey` and Tey's own dispatcher, `tey/bin/kex`, which runs whatever Tey has
  selected. The compiler itself sits in the keg's `libexec` — downloaded as a
  brew resource for a release, built from source for `--HEAD` — and is used
  where it lies until `tey kex install` takes it into the Tey home. So a fresh
  install is runnable immediately, writes nothing outside the keg, and every
  version after the first comes from Tey alone.
- The release path of the formula is UNPROVEN: there is no tagged release yet,
  so what has actually been exercised is the archive install against a
  `file://` release and the tarball unpack by hand. The first real release run
  (`docs/releasing.md`) is also the first test of the `formula` job that
  rewrites the tap.
- Once `tey kex install` has copied the bundled compiler into the Tey home,
  `brew upgrade tey` — which replaces the one in `libexec` — leaves the copy
  installed and selected. The copy is deliberate (a symlink would silently
  change what a version directory contains), but nothing tells the user a newer
  Kex arrived with the upgrade. Before that copy exists, an upgrade does move
  what `kex` runs, since the dispatcher falls through to the keg.
- Tey cannot yet manage Tey. It is installed and upgraded by whatever
  installed it, and there is no `tey self update` — so the toolchain manager is
  the one thing on the machine that is not under the toolchain manager.
- Installed WITHOUT a package manager — the release tarball unpacked by hand —
  Tey still needs its `bin` directory on PATH, which it cannot do for the user.
  It says so after an install. The Homebrew path no longer has this problem,
  since brew links `tey` and `kex` itself.

## Compiler problems this work surfaced

- A module's own private function loses to a prelude function of the same
  name, even when called by its qualified name: `Tey.Toolchain.sha256(path)`
  resolved to the prelude's `Digest.sha256` and typechecked as `String`,
  producing a `.try` error on a call that was plainly declared
  `Result<String, String>` a few lines below. Renaming to `checksumOf` was the
  workaround. Qualified names should not be resolvable to another module's
  function at all.
- A function-valued record field cannot be called through the field:
  `spec.handler(options)` fails on both backends, so `OptionParser`'s command
  dispatch binds `let handler = spec.handler` first and calls that.
