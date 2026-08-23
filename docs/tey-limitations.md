# Tey limitations and follow-up work

This file records known constraints in the first working Tey implementation.
They are kept explicit so temporary workarounds do not become accidental API.

## Tey package-manager limitations

- A dependency may ask for a RANGE — `tag: "~> 0.2"`, `"^1.0"`, `">= 0.3"` —
  and Tey lists the repository's tags and locks the highest that satisfies it,
  so the manifest keeps the constraint and the lockfile keeps the answer. An
  exact tag is still taken as written.

  Resolution now walks the whole graph: a fetched dependency's own
  `package.kex` is read and its `tey(...)` lines join a breadth-first queue,
  keyed on package NAME. Two constraints on one package are intersected
  (`~> 0.1` and `>= 0.1.5` become `~> 0.1 >= 0.1.5`, which `satisfies` already
  evaluates as a clause list), and a name reached from two different
  repositories is an error rather than two packages.

  What remains is re-resolution. A constraint discovered LATER in the walk is
  checked against the version already pinned and reported if it conflicts; the
  package is not re-resolved to a version that would have satisfied both. So
  `~> 0.1` seen first pinning `0.1.2`, then `>= 0.1.5` arriving from a deeper
  package, reports a conflict that `0.1.6` would have solved. A fixpoint loop
  is the fix; intersect-and-report was chosen first because its failures are
  legible.
- OTP is declared, enforced and recorded: `otp(">= 26")` in the manifest,
  `Tey.Toolchain.runtimeOtpFloor` reading the compiling OTP out of
  `kex --info`, an effective floor of `max(package, toolchain)` refused before
  any resolving happens, and `"otp": {"requirement": ..., "release": N}` in
  `tey.lock`. What is NOT done is inheriting the requirement from
  dependencies — only the root package's `otp(...)` is consulted, so a library
  needing a newer Erlang than its consumer says nothing until the consumer's
  build fails. The intersection machinery to fix it exists
  (`Tey.Semver.intersect`); the walk does not feed OTP through it yet.
- `tey lock` currently fetches repositories while resolving them because the
  content digest is computed from `git archive`. Separating resolution from
  cache population needs a remote/archive strategy or a clearly renamed
  command contract.
- Cache checkouts are not made read-only, so nothing stops a build from
  writing into a fetched dependency. (Creation itself is transactional now:
  the clone lands in a sibling `<commit>.partial` directory and is renamed
  into place only once the checkout succeeds, so a failed or killed run
  leaves nothing reachable under the content-addressed name.)
- `tey run` and `tey test` run on the BEAM (`-R`), the same backend
  `tey build` compiles for, so a package is no longer built one way and tested
  another. `--interpret` is the way back for a backend gap or a debugging
  session. What is NOT solved is the cost: `-R` compiles before it runs, so
  Tey's own five spec files take ~10s where the interpreter takes ~1.4s, and
  nothing is cached between runs.
- Both call sites buffer the child through `Process.run` and print after it
  exits, so a long `tey test` shows nothing until it finishes and `tey run`
  cannot be interactive at all. This needs a streaming `Process` API rather
  than a change in Tey; the printing is in one place
  (`Tey.Commands.report`) so that it is one edit when there is one.
- Trailing arguments reach a program (`tey run one two`) or a package command
  (`tey fmt src`), but FLAGS do not: Tey's own option parser claims what it
  recognizes and rejects the rest, so `tey run --verbose` and `tey fmt --check`
  report an unknown option instead of passing it on. `--` stops Tey's parsing
  but what survives is then read by `kex` rather than by the program.
- Bare `tey` and `tey --help` print the help from `Tey.Cli.dispatch` rather
  than from `OptionConfig.run`, which reaches them through a `this.printHelp`
  the BEAM backend emits at the wrong arity (kexhq/kex#192). The text is the
  same; the early-out goes away when that call lowers correctly.
- `command(name, run:, description:)` in the manifest declares a
  project-specific command, run as `tey <name>`. A built-in name cannot be
  redeclared (the reader refuses it), and Tey registers the declared ones with
  the same option parser its own commands use, so they appear in `tey help`
  and in the unknown-command message. Names namespace with `:`
  (`command("db:migrate", ...)` → `tey db:migrate`). What is missing:
  composition (`run: ["fmt", "test"]`), an inline Kex block instead of a
  string, and any notion of a command that only makes sense in some
  environments. A shell line is also split by `sh`, so `run:` inherits
  whatever `sh` does with quoting.
- `tey run`, `tey build` and `tey test` put every fetched dependency on the
  compiler's module search path, so `using Greet` resolves to the cached
  checkout — and a dependency the lockfile names but the cache lacks is
  reported (`run \`tey install\``) rather than surfacing as a missing module.
  The whole resolved closure is on the path now, not only the direct
  dependencies — and the compiler can finally build it: a module that had a
  `using` of its own used to crash `kex` with SIGSEGV, which is why a
  dependency-of-a-dependency had never worked even when the resolver placed it
  correctly.
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
