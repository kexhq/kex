# Tey — Package Manager for Kex

A package manager for Kex, written in Kex, with a Kex manifest. Git-resolved today,
registry-shaped for tomorrow. Per-project footprint is just a lockfile — sources live
in a global, content-pinned, read-only cache (Go-style), not under each project.

## Design decisions

| Decision | Choice | Rationale |
|---|---|---|
| Runtime | **BEAM (Core Erlang)** | Kex compiles to Core Erlang (`src/ir/emit_core.cxx`) and runs on BEAM. The tree-walk interpreter is a development fallback, not a target. Tey runs on BEAM. |
| Implementation language | **Kex** (bootstrap) | Matches the mix/shards precedent; the language is DSL-friendly. Depends on Kex→BEAM codegen reaching production quality and an Erlang FFI (§"Precursor"). |
| Manifest | **`package.kex`** (DSL) | "Everything is Kex." Read by shelling out to the already-built `kex`. |
| Dependency source | **Git now, registry later** | No infrastructure to stand up on day one. Git deps are source-only by policy (clone the pinned commit, compile locally — no source/binary gap). The manifest field shape lets `registry:` (and hub-only binary artifacts) slot in later without a breaking change. |
| Multi-file loading | **Out of scope** | Assumed to land in the compiler separately. In v1, `tey run`/`build`/`test` operate on a single entry file; multi-file dep wiring is the one explicitly inert area. |
| Dependency storage | **Global cache, no per-project tree** | Avoids the npm `node_modules` mistakes (see below). Same `<url>@<commit>` lives once on disk, shared by every project. |
| Build/distribution | **File-based compilation units** | A `.kex` file is a *program* — it can hold several `module` blocks plus top-level types/records/functions and `main`, not just one module. Each file compiles to one or more `.beam` modules; BEAM's code path loads the set. No concatenation linker (that was an interpreter-only stopgap; BEAM is multi-file native). |
| HTTP / hashing / json | **Idiomatic Kex wrappers over OTP** | `Process`, `Http`, `Json`, `Digest` are redesigned Kex APIs (`Result`, records, `String`) living in `src/prelude/`. Their implementations bottom out in `:httpc`/`:crypto`/ports/`:json` via a *private* Erlang FFI; no Erlang shapes leak into Kex code. |

## Why not `node_modules`

The npm sin is not "having a dependencies directory" — it is that `node_modules` is huge,
mutable, flat-hoisted, under-pinned, allows install scripts, and dedupes badly. Tey borrows
the Go/Cargo/hex model instead.

| npm problem | Tey mitigation |
|---|---|
| Massive per-project tree; the same library copied 10× across projects | **Global cache** keyed by git URL + commit. Same `<url>@<commit>` exists once on disk, shared by every project. |
| Flat hoisting → phantom dependencies | No flat namespace. Imports resolve through the cache root; the future loader gets one explicit `(url, commit)` table, not a hoisted pile. |
| Diamond deps → multiple copies of "react" causing subtle bugs | **Single-version unification** (shards model): if two deps request the same package, the resolver picks one version or fails. No nested copies. |
| `postinstall` scripts run arbitrary code on install | **No install scripts. Ever.** v1 packages are source trees; the future `.kexo`/`.kexi` binary mode is a sealed artifact, not an executable installer. hex and shards enforce the same rule. |
| `package-lock.json` doesn't truly pin / churns | `tey.lock` pins `<url, commit, sha256>` — the content hash is **mandatory**, not advisory. |
| `node_modules` freely editable, "works on my machine" | Cache entries are **read-only**. The only mutable state in a project is the lockfile. |

## Architecture and bootstrap

On BEAM, multi-file is native: a `.kex` file compiles to one or more `.beam` modules, and
BEAM's code path loads the resulting set. A file is a *program*, not a single module — it
may contain several `module` blocks plus top-level types/records/functions and `main`, so
the file→`.beam` mapping is many-to-many (one `.beam` per Kex `module` block, or one
`.beam` per file — a compiler decision). Either way, no concatenation linker is needed —
the tree-walk interpreter's single-file limitation (which originally motivated one) does
not apply on BEAM.

- Tey's *source* lives in `tey/src/*.kex`. By convention each file holds one `module`
  (`Semver.kex` → `module Semver`, …), but the language allows several modules per file and
  the build handles both.
- `make build` compiles each `.kex` to its `.beam`(s) under `tey/ebin/` via `kex --compile`.
- `make install` places `tey/ebin/*.beam` under `$(PREFIX)/lib/kex/tey/ebin/` and a shim at
  `$(BINDIR)/tey`:

  ```sh
  #!/bin/sh
  exec erl -noshell -pa "$PREFIX/lib/kex/tey/ebin" -s tey_cli start "$@"
  ```

  The exact launcher (raw `erl -pa`, an escript, or a `kex run` wrapper) is pinned in
  milestone 5.

  When run interactively (TTY present), `make install` also prompts to register Tey's git
  merge driver globally (`git config --global merge.tey.driver …`), so any repo carrying
  `tey.lock merge=tey` in its `.gitattributes` auto-resolves lockfile conflicts on merge.
  The prompt shells out to the canonical `tey setup merge-driver` command (see *Git merge
  driver* under *Lockfile spec*). Under `INSTALL_NONINTERACTIVE=1` (CI, package-manager
  builds) the prompt is skipped: package installers (Homebrew, AUR, apt) can't touch user
  dotfiles during install by policy and instead surface the one-liner via their post-install
  channels (`caveats`, `.install`, `postinst`). `make uninstall` removes the global entry
  for symmetry.

## Precursor: BEAM codegen maturity + Erlang FFI

Tey runs on BEAM and pulls HTTP, process, hashing, and JSON from OTP rather than from new
C++ natives. Two compiler-side prerequisites make this possible; Tey blocks on both.

### (a) Kex → Core Erlang reaches production quality

`src/ir/emit_core.cxx` already emits Core Erlang (`.core`), and `kex --emit-core`
is wired in `src/main.cxx`. The prerequisite is closing the gap between "emits a `.core`
for small programs" and "compiles and runs a multi-module program end-to-end on BEAM" —
enough to host Tey itself. This is a compiler workstream, tracked separately from Tey.

### (b) Erlang FFI in Kex — private plumbing only

A way for the standard library to reach OTP from Kex. The FFI is **internal**: only
`src/prelude/*.kex` uses it, exactly as C's `__builtin_*` is used by libc but not by
regular programs. Application code — and Tey — never sees `:httpc`, `:crypto`,
charlists, binaries, or `{:ok, _} | {:error, _}` tuples. The exact FFI syntax is a
compiler decision and is not part of Kex's public surface.

What Tey (and every Kex program) sees instead is a set of **redesigned, idiomatic Kex
APIs** in the prelude, each replacing an awkward Erlang return shape with the
corresponding Kex idiom:

```kex
foul module Process do
  run  : (String, [String]) -> Result<ProcessResult, Int>     # not :os.cmd's charlist goo
  exec : (String, [String]) -> Int
end
record ProcessResult do
  exit   : Int
  stdout : String
  stderr : String
end

foul module Http do
  get     : String -> Result<HttpResponse, HttpError>          # not :httpc's status tuple
  download: (String, String) -> Result<Unit, HttpError>
end
record HttpResponse do
  status  : Int
  headers : Map<String, String>
  body    : String
end
type HttpError = Timeout | Status(Int) | Net(String)

module Json do
  parse    : String -> Result<Value, String>                   # OTP 27 :json underneath
  stringify: Value -> String
end

module Digest do
  sha256     : String -> String                                # hex String, not a binary
  fileSha256 : String -> String?
end
```

"Better interface" is the whole point of these wrappers: `String` over charlist, hex
`String` over binary, records + `Result<T, E>` over `{:ok, _} | {:error, _}`. Zero new
C++ natives — the wrappers are pure Kex in `src/prelude/`. `git` remains a hard runtime
requirement; Tey shells out to it via `Process`.

## Cache layout

There is no per-project source directory. Sources live in a global cache, keyed by git URL
+ commit (a commit is already an immutable tree, so the address is stable and dedup is
automatic).

```
$TEY_CACHE/                               # default: per-OS cache dir /tey
  src/<host>/<owner>/<repo>/<commit>/     # checkout pinned to exact commit
    …                                     # e.g. src/github.com/kexlang/json/a1b2c3d7…/
```

- `$TEY_CACHE` defaults to `~/Library/Caches/tey` (macOS), `~/.cache/tey` (Linux),
  `%LOCALAPPDATA%\tey` (Windows); overridable via `TEY_CACHE` env. Consistent with the
  project already using `~/.config/kex/` for REPL history (`src/main.cxx:510`).
- Entries are created read-only after hash verification.
- A project's on-disk footprint is **just `tey.lock`**.

### What `tey install` does

1. Read `tey.lock`.
2. For each entry: skip if `--without` names a group in the entry's `groups`. Otherwise, if
   `$TEY_CACHE/src/<host>/<owner>/<repo>/<commit>/` exists and its tree hash matches the
   lockfile `sha256`, skip. Otherwise `git clone --filter=blob:none` +
   `git checkout <commit>` into the cache, verify the hash, mark read-only.
3. Done. No copy into the project. No symlinks. The future loader honors the same exclusion
   when activating modules, so `--without development` deps are absent at run time too.

### Coupling with the future loader

The compiler's future loader reads `tey.lock` and resolves imports against
`$TEY_CACHE/src/...`. That is the only coupling, and it is cleaner than a per-project path:
Tey hands the loader a `(url, commit)` table, and the loader maps
`import "json/..."` → `src/github.com/kexlang/json/<commit>/...`.

## Manifest spec — `package.kex`

Idiomatic Kex DSL. Tey injects a manifest-prelude defining `bundle`, `tey`, `target`,
evaluates `package.kex` via `Process.run("kex", […])`, and reads canonical JSON from stdout.

```kex
bundle "myapp" do
  version("0.1.0")
  kex(">= 0.1.0")                  # required compiler version (semver range)
  license("MIT")
  entrypoint("src/main.kex")

  target("myapp", entrypoint: "src/main.kex")   # `tey build` compiles each target

  tey("json",   git: "https://github.com/kexlang/json",   tag: "v0.3.0")
  tey("logger", git: "https://github.com/kexlang/logger", branch: "main")
  tey("legacy", git: "https://github.com/foo/legacy",     ref: "abc1237")

  group :development do
    tey("test_utils", git: "https://...", tag: "v1.0.0")
  end

  # Reserved for the registry future — accepted today, ignored by v1:
  # tey("argon", registry: "kexhub", version: ">= 1.2")
end
```

`tag`/`branch`/`ref` are mutually exclusive; `tag` preferred. Unknown keys are ignored, so
future fields don't break old Tey.

### Why parens on non-block declarations

Kex supports paren-free calls only when a `do…end` block follows the arguments
(`src/parser/parser.cxx:1485`) or when the call is all-named-args
(`src/parser/parser.cxx:1461`). A paren-free call *without* a block
(`version "0.1.0"`, `tey "json", git: …`) is not supported — by design, to avoid
Ruby's `foo bar` statement-vs-call ambiguity. So block-bearing declarations
(`bundle`, `group`) stay paren-free, matching the HTML DSL idiom
(`examples/html_dsl.kex`), while scalar/target/dep declarations use parens.

## Lockfile spec — `tey.lock`

JSON. Deterministic, sorted, committed to git. `sha256` is mandatory and verifies the tree
at the pinned commit.

```json
{
  "version": 1,
  "kex": "0.1.0",
  "deps": {
    "json":       { "git": "https://github.com/kexlang/json",
                    "resolved": "v0.3.0", "commit": "a1b2c3d...", "sha256": "...", "groups": [] },
    "logger":     { "git": "https://github.com/kexlang/logger",
                    "resolved": "main",   "commit": "deadbeef...", "sha256": "...", "groups": [] },
    "test_utils": { "git": "https://github.com/kexlang/test_utils",
                    "resolved": "v1.0.0", "commit": "cafebabe...", "sha256": "...", "groups": ["development"] }
  }
}
```

`deps` is an **object keyed by dep name**, not an array. This is deliberate: it minimizes git
merge conflicts (each dep is one stable line, so adding/removing one touches only its own
entry) and structurally enforces the resolver's single-version-per-name invariant
(*Dependency resolution* — "no nested copies — one version per package"). Keys are written
sorted alphabetically; readers ignore order. The array form was rejected because every
add/remove edit landed in the same bracket-delimited region, producing whole-block conflicts.
Forward-compatibility holds across every planned extension: binary mode adds fields to an
entry (`source_sha256`, `kexo_sha256`, `attestations`, …) without touching keying; registry
deps are still keyed by name; aliases/multi-major side-by-side would key by alias and remain
unique — but that mode is already rejected by policy in v1.

`commit` is the immutable pin. `sha256` verifies the tree at that commit. `groups` records
every group the dep was declared under (empty = production); a dep reachable through multiple
groups lists all of them. `tey install --without development` skips cache entries whose
`groups` intersect the exclusion list, and the future loader does the same when activating
modules — so dev-only deps never ship in a production build.

`tey install` is a no-op when `tey.lock` is up to date and the relevant cache entries are
present and hash-verified.

### Git merge driver

`tey.lock` is a deterministic function of `package.kex` (resolution is single-version,
highest-wins, no backtracking), so lockfile conflicts are never hand-merged — they're
regenerated. Two mechanisms make that automatic:

1. **`.gitattributes`** (written by `tey init`, committed, portable):
   ```
   tey.lock merge=tey
   ```
2. **The driver itself** — registered in the user's global `~/.gitconfig`:
   ```ini
   [merge "tey"]
       name = Tey lockfile merger
       driver = tey lock --merge-driver %O %A %B
   ```
   `tey lock --merge-driver` unions the deps from both sides, re-resolves against the merged
   `package.kex`, and writes the canonical lockfile. If `package.kex` itself conflicted
   (git 3-way-merges files independently), the driver exits non-zero and lets git surface
   it — that's the *real* conflict, and it belongs to the human.

Global registration is gated behind the user's consent — silent writes to `~/.gitconfig`
are rude. The canonical entry point is `tey setup merge-driver` (add) / `--remove` (remove),
and every install surface routes through it:

| Surface | Behavior |
|---|---|
| `make install` (TTY) | prompt "Register Tey's git merge driver in ~/.gitconfig? [Y/n]"; yes → `tey setup merge-driver` |
| `make install` (`INSTALL_NONINTERACTIVE=1`, CI) | skip the prompt, print the hint |
| Homebrew tap | formula `caveats` prints the one-liner (sandbox can't touch dotfiles) |
| AUR / Debian | post-install message equivalents |
| Any time later | `tey setup merge-driver` / `--remove` |

Optional polish (later milestone): a one-time hint on the first `tey` invocation in a repo
whose `.gitattributes` references `merge=tey` but whose `~/.gitconfig` lacks the driver,
gated by a marker under `~/.config/kex/` (already used for REPL history, `src/main.cxx:510`).
Catches users who installed via a package manager and skipped the caveats.

## Project layout (`tey init`)

```
myapp/
  package.kex
  tey.lock              # written after first `tey install`
  src/
    main.kex
  spec/                 # *.spec.kex, runnable via `tey test`
  .gitattributes        # tey.lock merge=tey (auto-merge; see Lockfile spec)
  .gitignore            # created empty; nothing per-project to ignore
```

`tey init <name>` scaffolds the above. `tey init --lib` drops `src/main.kex` and produces a
library layout (no `target`). Note: unlike npm/shards, there is **no** `tey_modules/` or
`lib/` to gitignore — the cache is global and the project carries only its lockfile.

## Dependency resolution (git + semver)

Deliberately simple — no PubGrub in v1. Single-version unification per package name.

1. Parse `package.kex` → deps with version sources (`tag`/`branch`/`ref`).
2. For each dep: `git ls-remote --tags <url>` → collect `vX.Y.Z` tags → filter by semver
   range (only `tag:` accepts ranges like `>= 0.2`, `~> 1.0`, `^1.2`; exact `tag:`/`ref:`/
   `branch:` skip resolution).
3. **Conflict detection**: on the same package requested twice, pick the highest version
   satisfying both ranges; fail with a clear message if the intersection is empty. No
   backtracking (documented limitation). No nested copies — one version per package.
4. For each resolved ref: clone into `$TEY_CACHE/src/<host>/<owner>/<repo>/<commit>/`,
   verify `sha256` over `git archive` output.
5. Recurse into each dep's `package.kex` (transitive). Cycle detection by visited-set on
   `name`. **Group propagation**: a transitive dep's `groups` is the union of every group
   through which it is reachable — reached only via `:development`, it's dev-only; reached
   via both production and development edges, it lands in both.
6. Write `tey.lock` (including `groups` per dep).

Semver is a ~60-line Kex module implementing parse, compare, and the `~>` `^` `>=` `<` `=`
operators. Tag convention `vMAJOR.MINOR.PATCH` is enforced; non-conforming tags are ignored.

## Command surface (v1)

```
tey init [name] [--lib]        scaffold a project
tey add <name> [--git URL] [--tag|--branch|--ref V]
                               add a dep to package.kex, resolve, write lock
tey install                   populate/verify the cache from tey.lock (resolve if absent)
tey update [name]              re-resolve; with name, bump just that dep
tey lock                       (re)write tey.lock from package.kex without fetching
tey lock --merge-driver %O %A %B
                               internal: git's `merge=tey` driver target (see Lockfile spec)
tey list                       print resolved dep tree
tey run [target]               exec `kex <entrypoint>`; v1: single file
tey build [target]             compile target to .beam via `kex --compile`
tey test                       run spec/*.spec.kex via `kex`; aggregate exit codes
tey clean [--cache]            by default: remove orphaned cache entries not in any lockfile
                               --cache: wipe $TEY_CACHE entirely
tey setup merge-driver [--remove]
                               register/remove the global git merge driver in ~/.gitconfig
                               (interactive `make install` prompts to run this)
tey --version / --help
```

`run`/`build`/`test` shell out to the installed `kex` (itself BEAM-backed). **In v1 they
ignore the cache** because the compiler can't yet wire transitive deps into a program —
this is the explicitly inert area. The moment the loader lands, `tey run` resolves the
entrypoint's imports against `$TEY_CACHE` via a `--path`-style flag the loader exposes.

## Tey's own module structure

```
tey/
  src/
    Semver.kex        # parse/compare/range — pure
    Git.kex           # foul: wraps Process.run("git", …) ; ls-remote, clone, archive
    Manifest.kex      # read/parse package.kex via subprocess; Package record
    Lockfile.kex      # read/write tey.lock; idempotent compare; hash verification
    Cache.kex         # $TEY_CACHE layout, read-only population, orphan cleanup
    Resolver.kex      # the resolution algorithm; returns a resolved dep graph
    Commands.kex      # one function per command
    Cli.kex           # arg parsing, dispatch, exit codes
    main.kex          # entry: dispatch on argv
  manifest_prelude.kex # the bundle/tey/target DSL definitions
  Makefile            # build: kex --compile each src/*.kex -> ebin/*.beam ; install
  spec/               # *.spec.kex tests for Tey itself
```

## Testing strategy

- **Pure modules** (`Semver`, `Lockfile` parsing): tested with the existing
  `describe`/`it`/`assert` DSL (`docs/testing.md`), runnable via the `.spec.kex` auto-load
  mechanism (`src/main.cxx:342`) until the loader lands, then via `tey test`.
- **Side-effecting modules** (`Git`, `Cache`, `Commands`): fixture git repos created under a
  temp `$TEY_CACHE` by spec setup; assertions on cache layout, hash verification, and
  lockfile contents.
- **End-to-end**: `tey init` → `tey add` → `tey install` → `tey run` against a throwaway temp
  project with a throwaway temp cache, asserting exit code and cache layout.
- The new prelude modules (`Process`, `Http`, `Json`, `Digest`) get their own `*.spec.kex`
  suites alongside `src/prelude/`, asserting against fixed outputs (a known sha256, a
  `:json` round-trip, a fixed subprocess exit code) and type-checked via
  `cmake --build build --target check-prelude` (`CMakeLists.txt:92`).

## Milestones

0. **BEAM codegen + Erlang FFI (compiler track, parallel)** — Kex → Core Erlang runs a
   multi-module program end-to-end; an FFI surface lets Kex call
   `:httpc`/`:crypto`/`:json`/ports. Blocks everything in Tey.
1. **FFI wrapper modules** — `Process`, `Http`, `Json`, `Digest` in Kex over the FFI, with
   specs. The pure-OTP layer Tey stands on.
2. **Semver + Lockfile** modules + specs. Pure foundation.
3. **Git + Cache + Manifest** modules + specs (uses `Process`). Can read `package.kex`,
   `git ls-remote`, and populate/verify the cache.
4. **Resolver** — `tey install`/`lock`/`add`/`update`/`list`/`clean` working end-to-end against
   real git URLs.
5. **`tey init`** scaffolding + `Makefile` (`kex --compile` → `ebin/*.beam`) + install shim.
6. **`tey run`/`build`/`test`** — shell-out wrappers, functional for single-file projects.
7. **Tey dogfoods itself**: Tey's own `package.kex` and `tey install` work on Tey's repo (no real
   deps yet, but the path is proven). This is also the first real stress-test of Kex → BEAM.
8. **Docs**: this file, README section, `tey --help` polish.

Estimated ~2–3k lines of Kex. The compiler-side cost (BEAM codegen maturity + Erlang FFI)
is tracked separately on the compiler track.

## Future: `.kexo` / `.kexi` binary distribution (hub-only)

Tey's two dependency sources carry different trust models, and the policy splits along
that line:

- **Decentralized (git) deps — source required.** v1 is git-only and stays source-only by
  policy: a `git:` dependency can never request a binary artifact. Tey clones the pinned
  commit, hashes the tree, and compiles locally. The integrity anchor is the source
  `sha256` — there is no binary to disagree with the source, and no injection gap to close.
- **Central hub (future) — may ship binaries.** When the registry lands, hub packages may
  publish `.kexo`/`.kexi` for speed and encapsulation. *There* the source/binary binding
  machinery below is mandatory; the decentralized path never needs it.

The interface/body split itself:

- **`.kexi`** — a Kex interface file: public type and function signatures only, no bodies.
  The existing `--summary` output (`src/main.cxx:1032`) already produces exactly this shape.
- **`.kexo`** — a Kex object file: the compiled artifact (a `.beam` or a sealed bundle),
  consumed directly without recompilation.

Why it fits:

- **Smaller, faster installs** — consumers fetch a `.kexo` + `.kexi`, not source; no
  per-project recompile.
- **Encapsulation** — the `.kexi` is the contract; the body is opaque, so authors can hide
  implementation details (closer to Modula-3/Ada `spec`/`body` than to npm's "everything
  public").
- **Incremental compilation** — consumers recompile only when a `.kexi` they depend on
  changes; a body bump with no interface change needs no downstream rebuild.
- **Integrity** — a `.kexo` is a fixed blob, so the lockfile pins it by content hash
  directly. (Decentralized git deps, by contrast, pin the *source* tree and compile locally
  — no binary integrity question arises.)

Design implications kept compatible with the v1 choices:

- **Cache** gains a parallel tier `$TEY_CACHE/obj/<name>/<version>/<kexi-hash>/` alongside
  `src/`. v1 only populates `src/`; the loader is the future consumer of both.
- **Lockfile** records `sha256` over source today; in binary mode the same field pins the
  `.kexo` (and a `kexi_sha256` pins the interface), with a full source-binding attestation
  chain — see *Binding binaries to source* below. The schema is additive.
- **Manifest** gains a per-dep preference — but only for hub deps, since git deps are
  source-only: `tey "json", registry: "kexhub", version: ">= 1.2", artifact: binary`.
  Source remains the default; requesting `artifact: binary` on a `git:` dep is an error.
- **No install scripts** still holds — a `.kexo` is a sealed artifact, not an executable
  installer. That part of the npm model is rejected regardless of source vs. binary.

### Binding binaries to source (no injection)

A published `.kexo` whose bytes don't match the public source is the classic supply-chain
attack — clean source as cover, malicious code in the artifact. The defense is a chain of
hashes that makes any mismatch detectable:

1. **Reproducible builds** (compiler-track requirement). Kex → Core Erlang → BEAM must be
   deterministic for a fixed input + fixed compiler version: stable map/key ordering, no
   timestamps, no absolute paths or random IDs leaking into the output. Without this,
   independent rebuilds can't be compared at all.
2. **Source-bound attestation in the lockfile.** A binary-mode entry carries the full chain:

   ```json
   // A binary-mode entry under "deps" (keyed by name, as in v1):
   "json": {
     "git":            "...",            // source anchor; same shape as v1
     "resolved":       "v0.3.0",
     "commit":         "...",
     "source_sha256":  "...",            // hash of the published source tree
     "kexo_sha256":    "...",            // hash of the published .kexo blob
     "built_with":     { "kex": "0.3.1", "kex_sha256": "..." },
     "attestations": [                   // independent rebuilds: "I rebuilt source_sha256
       { "by": "kexhub-bot",             //   with built_with and got kexo_sha256"
         "rebuild_sha256": "...",
         "signature": "..." } ],
     "groups":         []
   }
   ```

   `source_sha256` binds the artifact to the public code; `kexo_sha256 == rebuild_sha256`
   proves the binary is what that source compiles to; `built_with` pins the compiler so a
   malicious rebuild can't hide behind a different `kex`.
3. **Verification policy.** `tey install` in strict mode rebuilds the source locally with
   the pinned `kex` and asserts its hash equals `kexo_sha256`. In trust mode it accepts the
   published binary if a quorum of `attestations` (or a trusted key) already asserts the
   same equality — the Debian/reproducible-builds rebuilder model.
4. **Publisher signatures** are orthogonal: they prove *who* shipped the artifact, not
   what's in it. They layer on top of the source-binding chain, not replace it.

Out of scope for v1; flagged here so the v1 schema (lockfile, cache, manifest) doesn't paint
into a corner.

## Risks and open items

- **Loader dependency is real.** Until multi-file loading lands in `kex`, Tey fetches and
  caches but can't wire deps into a program. `run`/`build`/`test` are explicitly limited to
  single entry files in v1.
- **No backtracking resolver** — rare for small git-tagged graphs, but diamond conflicts on
  ranges can fail where PubGrup would succeed. Documented; upgradeable later.
- **BEAM codegen must scale to Tey** — Tey is one of the first real Kex programs. Any gaps
  in Kex → Core Erlang (large functions, deep pattern matches, closures over loops) surface
  as Tey-blocking compiler bugs. Mitigation: Tey lands incrementally alongside codegen,
  dogfooding from milestone 4 onward.
- **Security**: `git clone` of arbitrary URLs. Decentralized (git) deps are **source-only by
  policy** — Tey always compiles from the pinned commit, so there is no source/binary gap to
  exploit. The mandatory `sha256` pins the tree; enforcement hooks (rejecting unverified
  trees) come with the registry. Binary distribution and its source/binding machinery are
  hub-only — see *Binding binaries to source* under the `.kexo`/`.kexi` section.
- **BEAM runtime requirement** — users need Erlang/OTP 27+ installed (for the `:json`
  module and modern `:httpc`). Heavier than the current C++-only binary, but standard for
  BEAM-hosted languages (Elixir, LFE, Alpaca all assume it). Tey is part of that story.
- **Cache pruning policy**: `tey clean` (no flags) removes cache entries unreferenced by any
  lockfile found under `$HOME`. Conservative; a future `--since`/`--dry-run` can refine it.
