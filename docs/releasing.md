# Releasing Kex

Releasing is **one action**: run the Release workflow. It reads `VERSION`,
builds that version, tags it and publishes it.

**`VERSION` is the single source of truth for the next release's number.** The
workflow decides nothing about it — no form field, no arithmetic, no guessing
from commit messages. It releases what the tree says it is working toward.

Nothing is triggered by pushing a tag either. A tag is an *output* of a
release, not the request for one — a hand-pushed tag can land on a commit whose
suites never ran, and cannot be taken back once somebody has fetched it. Here
the tag is created by the last job, so **a tag in this repository always names
a version that was actually published**.

## Before 1.0.0

Kex is before 1.0.0 and **no backward compatibility is attempted**. The
language, the standard library, the package format and the on-disk layouts
change between releases without deprecation cycles. Every 0.x release says so
on its own release page — the `publish` job prepends it to the notes — and the
README says it too. Do not soften it; people who pin a version need to know
that is the only thing that will save them.

## VERSION names the next release

`VERSION` holds the version the tree is **working toward**, not the last one
shipped. `0.4.0-rc.2` in `VERSION` means "the next thing released from here is
0.4.0-rc.2" — a development build reports that number too, distinguished by the
git revision `kex --version` prints beside it.

Bumping it is an ordinary commit, reviewed like any other change:

```sh
echo "0.4.0-rc.2" > VERSION
```

Valid versions are `MAJOR.MINOR.PATCH`, optionally followed by `-` and one of
`prealpha`, `alpha`, `beta`, `rc`, each optionally with `.N`. Anything else
(`-RC1`, `-rc-1`, `-nightly`) is a typo, and a typo must not become a release,
so the workflow refuses it.

Releasing consumes that version, so afterwards the tree names something already
out. The `bump` job proposes the next number as a pull request — the smallest
step that cannot collide (`0.4.0` → `0.4.1`, `0.4.0-rc.1` → `0.4.0-rc.2`) — and
you edit or close it: a version that should be a minor, a major or a different
channel is exactly what a default gets wrong. Run with `bump_after: nothing` to
raise it by hand instead.

Until `VERSION` moves, running the workflow again fails on the existing tag
rather than rebuilding a published number.

## Running one

```
Actions → Release → Run workflow → ref: main
                                   bump_after: pull request
```

That is the whole input, and neither field is the version: the workflow reads
`VERSION` at that ref. There is nothing to type twice and nothing that can
disagree with the tree.

The channel follows from the version:

| VERSION        | GitHub release | Image tags              | Homebrew  | npm dist-tag |
| -------------- | -------------- | ----------------------- | --------- | ------------ |
| `0.4.0`        | stable         | `0.4.0`, `latest`       | updated   | `latest`     |
| `0.4.0-rc.1`   | pre-release    | `0.4.0-rc.1`, `rc`      | untouched | `rc`         |
| `0.4.0-beta.2` | pre-release    | `0.4.0-beta.2`, `beta`  | untouched | `beta`       |

The npm package (`@kexhq/kex`, the wasm interpreter) is published with the
plain `VERSION` as its version number, so every channel answers "which Kex is
this?" with the same number. CI covers the dev side in between releases: every
push publishes `<VERSION>-dev.<sha>` under the floating `next` dist-tag, which
a release never touches.

A pre-release is published with GitHub's `prerelease` flag, which is what keeps
`/releases/latest` meaning "the current stable Kex" without anyone maintaining
a second list — the endpoint kex.run reads, and what `tey kex install` resolves
to when given no version.

## Who may run it

`workflow_dispatch` is open to everyone with write access, which is wider than
the set of people who should publish a Kex, so the workflow gates itself twice:

- The `version` job runs in the **`release` environment**, which is configured
  with `@akoskovacs` as a required reviewer and `main` as the only allowed
  branch. A run waits for that approval before doing anything at all.
- It also asks the API for the caller's permission and refuses anyone below
  `maintain`, so an environment left unprotected is not a hole.

## Repository settings this depends on

Both are already set on kexhq/kex; they are written down because a restored or
forked repository will not have them.

- **Environment `release`** — required reviewer, `main`-only branch policy, and
  the home of `HOMEBREW_TAP_TOKEN`. An environment secret is reachable only
  from a job that names the environment, so nothing else in the repository can
  reach the tap.
- **"Allow GitHub Actions to create and approve pull requests"** (Settings →
  Actions → General) — the `bump` job opens its pull request with the workflow
  token. Default workflow permissions stay *read*; every job that needs more
  asks for it explicitly.

Still to do by hand, once: create `HOMEBREW_TAP_TOKEN` — a token with contents
and pull-requests write on kexhq/homebrew-tey — and store it in the
environment:

```sh
gh secret set HOMEBREW_TAP_TOKEN --repo kexhq/kex --env release
```

Without it, only the `formula` job fails, and only on a stable release.

## What a run does

1. **`version`** — checks the caller, reads `VERSION`, refuses a version that
   is already tagged, and resolves `ref` to one commit. Every later job builds
   *that commit*, so a push to `main` mid-release cannot change what is being
   released. The workflow writes nothing to the repository.
2. **`build`** (3 runners) — builds, runs the full suites (unit, spec, prelude,
   stdlib, spec-beam), and packages one archive per platform:

   ```
   kex-<version>-linux-x86_64.tar.gz    kex-<version>-macos-arm64.tar.gz
   kex-<version>-linux-arm64.tar.gz
   ```

   each with a `.sha256` beside it. A red suite means no release.

   The Linux x86_64 runner additionally packages **`tey-<version>.tar.gz`**.
   Tey is compiled to BEAM modules, which are architecture-independent, so one
   archive serves every platform — that is what lets Homebrew install Tey
   without being able to build Kex at all.

   Both halves of the Homebrew formula come from these four archives: the keg
   installs Tey from the `tey-` one and the compiler for the installing machine
   from the matching `kex-` one. A platform whose archive is missing is a
   platform that cannot `brew install tey`, which is the other reason a red
   suite stops the release. Intel macOS currently publishes no archive (see
   "Known gaps").
3. **`wasm`** — builds the npm package (`@kexhq/kex`) from the same commit, on
   the same pinned toolchain as CI's wasm job (the cache keys match, so this
   restores rather than rebuilds). Runs the wasm unit and spec suites, stamps
   the package with the release's `VERSION`, and uploads the tarball for the
   `npm` job. Built here rather than repackaged from a CI dev build so the
   binary's `built` date is the release commit's, like the archives.
4. **`image`** (3 variants × 2 architectures) — each built on a runner of its
   own architecture, not under QEMU, because the image build runs the spec
   suites inside itself. Pushed to GHCR by digest.
5. **`manifest`** — stitches each variant's two digests into one multi-arch
   tag, so `docker pull` picks the right architecture:

   ```
   ghcr.io/kexhq/kex:<version>           glibc, BEAM included
   ghcr.io/kexhq/kex:<version>-alpine    musl, BEAM included
   ghcr.io/kexhq/kex:<version>-slim      musl, OTP pruned to what Kex loads
   ```

   A stable release also moves `latest`, `latest-alpine`, `latest-slim`; a
   pre-release instead gets its channel name (`rc`, `beta`, …).
6. **`publish`** — compiles the release log, then **creates the tag** (via
   `--target`) together with the GitHub release, attaches the platform
   archives, and marks a pre-release as such.

   The release page *is* the changelog: there is no `CHANGELOG.md` to keep in
   step with the tags. The notes are built from the commits between the
   previous tag and this one (merges dropped, since their subjects only repeat
   branch names), followed by install commands, the pre-1.0 warning on any 0.x
   release, and GitHub's own generated notes for the pull requests and new
   contributors.    Re-running a release rewrites the notes as well as the assets,
   so a fixed run is not stuck with the first attempt's text. That flag is what keeps `/releases/latest` meaning "the current stable
   Kex" — the endpoint kex.run reads, and what `tey kex install` resolves to
   when given no version.
7. **`npm`** — publishes the wasm package the `wasm` job built as
   `@kexhq/kex@<version>` on GitHub Packages, with the same dist-tag rule as
   the images: `latest` for a stable release, the channel name for a
   pre-release. The one job that cannot replace on a re-run — npm versions
   are immutable, so a re-run of an already-published version skips instead.
8. **`formula`** — **stable only**. Points `kexhq/homebrew-tey`'s
   `Formula/tey.rb` at the new release and opens a pull request on the tap. It
   fills two marked regions and nothing else: `# <<STABLE-TEY` with the Tey
   archive's `url`/`sha256`, and `# <<STABLE-KEX` with one `resource` per
   platform, so brew downloads the compiler for the machine doing the
   installing. Checksums are read from the `.sha256` files published beside the
   assets rather than recomputed. Nothing pushes to a branch people install
   from — a formula that cannot build is a broken `brew install` for everyone —
   so **a release reaches Homebrew when a maintainer merges that pull
   request**.
9. **`bump`** — opens a pull request raising `VERSION` to the next number, on a
   `bump/v<next>` branch. It does nothing if `VERSION` has already moved past
   the released version while the run was going. Needs "Allow GitHub Actions to
   create and approve pull requests" in the repository's Actions settings.

Pre-releases are reachable on purpose, just never by accident:

```sh
tey kex list --pre          # show them
tey kex install --pre       # newest of any channel
tey kex install 0.4.0-rc.1  # by name, no flag needed
docker run ghcr.io/kexhq/kex:rc
npm install @kexhq/kex@rc   # GitHub Packages — see packages/kex/README.md
```

## If something fails

Re-run the workflow. Nothing needs undoing first: no tag was created unless
`publish` reached the end, and every job is idempotent — release assets are
replaced with `--clobber`, image digests are overwritten, and the formula
rewrite commits nothing when it changes nothing. The one exception is `npm`,
which skips a version the registry already holds: npm versions are immutable
and cannot be replaced, so the first publish of a number is the one that
sticks.

- **`v0.4.0 already exists. Bump VERSION and run again`** — that version was
  published. Raise `VERSION` to the next one and merge that first; do not
  delete a tag people may have fetched.
- **`<actor> has 'write' permission; releasing needs admin or maintain`** — ask
  a maintainer to run it.
- **A suite fails on one platform** — nothing is published, and nothing is
  left behind: no tag, no release, no change to the repository. The other
  platforms still finish building (the matrix does not cancel siblings, so one
  run shows every failure), but `publish` needs the whole `build` job, and a
  release missing a platform is not a release.
- **`formula` failed** — the release itself is complete; only the tap is
  behind. Re-running updates the same pull request rather than opening another.
- **`npm` failed** — the release itself is complete; only the npm package is
  missing. Re-running publishes it, unless it had already succeeded: npm
  versions are immutable, so a re-run skips an already-published one rather
  than replacing its bytes.
- **`bump` failed** — nothing is wrong with the release; raise `VERSION` by
  hand. The likely cause is the Actions setting that permits creating pull
  requests.
- **Images pushed but the release failed** — GHCR holds digests for a version
  with no release. A re-run overwrites them; nothing consumes an image tag that
  `manifest` has not created.

## Deliberately not automated

- **The version.** Deciding that what comes next is `0.4.0` rather than
  `0.3.5` is a judgement. `VERSION` is one line in a reviewed commit, and the
  `bump` job only ever *proposes* the smallest next step.
- **Prose release notes.** The generated commit list is the default; write
  something better into the release afterwards when a version deserves it.
- **Anything extra for a pre-release.** No tap update, no `latest` image, no
  `/releases/latest`. That is the whole point of the channel.

## Known gaps

The first run of this workflow is also its first real test; the `formula` job
in particular has only been exercised against a simulated release.
`docs/tey-limitations.md` tracks that and the related packaging gaps.

**No Intel macOS archive.** GitHub has retired its Intel macOS runners, so a
`macos-13` build job queues forever — the first real release hung on exactly
that. The platform is dropped until the x86_64 build is cross-compiled on an
arm64 runner (or self-hosted). Until then: no `kex-<version>-macos-x86_64`
asset, the Homebrew formula carries no `kex` resource for Intel macOS, and
`tey kex install` on an Intel Mac falls back to a source build (the resolver
already treats a missing archive as "not published for this platform").
