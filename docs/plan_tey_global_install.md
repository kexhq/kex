# Tey program installs

## Context

Tey installs *dependencies* into the cache and, since targets landed, also
installs a package's *own* targets: `tey install` inside a package that
declares `target("name")` builds each target as an escript and copies it into
`$TEY_HOME/bin` (`Tey.Commands.installMemberTargets`). What is missing is the
`gem install` / `npm install -g` / `cargo install` half: installing a program
someone else wrote, from outside any project, without cloning it by hand first.

Almost every piece already exists:

- `Tey.Cache.fetch` clones and verifies a Git source at a commit.
- `Tey.Snapshot` turns a directory into an immutable, content-addressed copy.
- `Tey.Workspace.discover` / `select` find a package, or a member of a
  workspace, inside a tree.
- `buildTargetsAt` compiles each target into a self-contained escript (runtime
  and prelude embedded), so an installed program does not depend on the
  selected toolchain once built.
- `$TEY_HOME/bin` is already on PATH for anyone who can run `kex`, because the
  `kex` shim lives there.

So this feature is mostly orchestration plus one new piece of state: a record
of what was installed, so it can be listed, updated and removed.

The "no install scripts, ever" rule is unchanged. Installing a program means
fetching source and compiling it with the selected Kex; nothing from the
package runs at install time except plugins it declares, which go through the
existing approval flow (see §6).

## 1. Command surface

Two command words, no flags or groups, reading the way `gem install` /
`gem uninstall` do:

```sh
tey install                                                 # unchanged: this project
tey install https://github.com/kexhq/rodolfo.git            # default branch HEAD
tey install https://github.com/kexhq/rodolfo.git --tag v0.4.0
tey install <url> --branch main | --ref <sha>
tey install <url> --package rodolfo-cli                     # workspace member
tey install <url> --target rodolfo                          # only this target
tey install ./path/to/checkout                              # like cargo install --path
tey install rodolfo                                         # already installed: update it
tey uninstall rodolfo
tey list                                                    # + an "Installed programs" section
```

`tey install <something>` can always mean "install this program", inside a
project or out, because adding a dependency is already its own verb
(`tey add`). There is none of npm's ambiguity to resolve, and the
merge-driver's positional arguments only apply behind `--merge-driver`, so
they do not collide.

The argument is classified as:

- a **path** when it starts with `.`, `/` or `~`;
- a **Git URL** when it contains `://` or looks like `user@host:path`
  (`--git <url>` is also accepted, so `tey add` muscle memory works);
- otherwise a **name**, looked up in the receipts (§3). An installed name
  re-installs from the source its receipt records — that is what updating is
  (§7). An unknown name is an error saying registry names are not supported
  yet and asking for a Git URL or path.

`--tag` / `--branch` / `--ref` mean exactly what they mean on `tey add`, and
the parsing is shared with it. Several names may be given at once
(`tey install rodolfo kexfmt`); sources are one per invocation.

Bare `tey install` in a package with targets keeps doing what it does today,
but now also writes a receipt (§5), so it shows up in `tey list` and can be
removed with `tey uninstall`.

`tey list` gains an "Installed programs" section (name, version, source,
commit or tag, targets, and a "needs rebuild" mark, §8). Outside a project it
shows only that section instead of failing for lack of a `package.kex`.

Registry names as *first* installs (`tey install rodolfo` with nothing
installed) wait for registry sources, which are already deferred in
`plan_tey_workspaces_plugins.md`.

## 2. Layout under the Tey home

```
$TEY_HOME/
  bin/                     # already exists: the kex shim + installed targets
    kex
    rodolfo
  programs/
    rodolfo/
      receipt.json         # what was installed, from where, owning which bins
      build/               # snapshot the program was built from (kept, see below)
```

The unit is the *package*, keyed by package name, not by binary: one package
may install several targets, and reinstall and `uninstall` act on all of them.

The build tree is a private copy (`Tey.Snapshot` of the fetched checkout),
never the cache checkout itself: the cache entry is verified by content, and
building in it would write `ebin/` into something Tey later re-verifies.
Keeping the snapshot makes re-installing by name a no-op check when the commit has
not moved and lets a failed rebuild leave the working install untouched.

## 3. The receipt

```json
{
  "name": "rodolfo",
  "version": "0.4.0",
  "source": {"kind": "git", "url": "https://…/rodolfo.git",
             "selector": "tag", "requested": "v0.4.0",
             "commit": "3f9c…", "subdir": "", "package": ""},
  "targets": ["rodolfo"],
  "kex": "0.5.1",
  "otp": "28",
  "locked": true,
  "installedAt": "2026-09-26T12:00:00Z"
}
```

For a path source `source` records the absolute path and the snapshot sha256
instead of a URL and commit.

The receipt is the only authority for ownership: a file in `$TEY_HOME/bin` that
no receipt lists is not Tey's to overwrite or delete. JSON rather than a Kex
manifest because nothing about it is authored by hand, and `:json` is already a
runtime requirement.

## 4. Install flow

1. **Resolve the source.** Git: resolve the selector to a commit the same way
   the resolver does for a dependency, and `Tey.Cache.fetch` it. Path:
   `Tey.Snapshot.create` it.
2. **Find the package.** `Tey.Workspace.discover` on the fetched tree; with a
   workspace, `--package` picks the member and its absence is an error listing
   the members that declare targets.
3. **Refuse libraries early.** No targets → error: "`name` declares no
   targets; it is a library. Add it to a project with `tey add`." (No fallback
   to `entrypoint`: a package that wants to be installable says so with
   `target(...)`, which is also what `tey install` in the project already
   requires.)
4. **Check the toolchain.** Validate the package's `kex(...)` requirement
   against the *selected* Kex (`Tey.Toolchain.ToolchainInfo`, never
   `Kex.VERSION`). On mismatch, fail with the `tey kex install <version>`
   command that would satisfy it; do not switch toolchains behind the user's
   back.
5. **Lock.** Honour the program's committed `tey.lock` by default — the same
   dependency versions its author tested, and what `cargo install --locked`
   gets right. `--fresh` re-resolves instead. With no lockfile, resolve and
   record `"locked": false`. The resolved lock is written into the snapshot
   only, never back to the source.
6. **Fetch dependencies and approve plugins** exactly as `Tey.Commands.install`
   does, against the snapshot's workspace context.
7. **Build** with `buildTargetsAt(package, snapshotRoot)`; `--target`
   restricts which ones.
8. **Check collisions** (§5), then **publish atomically**: copy each escript to
   `bin/.<name>.new`, `chmod +x`, rename into place, and only then write the
   receipt (again via a temp file + rename). A failure at any step before the
   renames leaves the previous install fully intact.
9. Print the installed paths and the existing "Add … to PATH" hint when
   `$TEY_HOME/bin` is not on PATH.

Reinstalling an already-installed package replaces it in place (steps 1–8 with
the old receipt's bins released only after the new ones land), and says
"Replaced rodolfo 0.3.2 → 0.4.0".

## 5. Collisions

A target name is refused when it is:

- `kex` — reserved for the shim that lives in `bin/`, never overwritten,
  `--force` or not. (`tey` is not reserved: Tey's own launcher is installed
  elsewhere, and Tey's own package declares a `tey` target.)
- owned by another package's receipt — error names the owner; `--force`
  transfers ownership and rewrites the other receipt to drop it (removing the
  receipt when it is left owning nothing);
- an existing file that no receipt owns and Tey did not write — refused unless
  `--force`.

An unowned file that *is* a Tey-built escript (its emulator line names
`kex_main`) is adopted without `--force`: it is what a project-local
`tey install` left there before receipts existed, and refusing it would make
every existing installation demand `--force` on its next reinstall.

All names are checked before anything is built, so a refused install costs
nothing and changes nothing. Targets the previous receipt owned that the
package no longer declares are removed on reinstall.

Implemented (step 1): project-local `tey install` writes a receipt with
`"source": {"kind": "path", …}` (`Tey.Programs`), so every route into `bin/`
obeys the same ownership rule and `tey list` shows everything in there.

## 6. Plugins and non-interactive installs

A program's direct plugin bindings need approval like any workspace's. On a
TTY the existing prompt runs. Without one (CI, Dockerfiles), installation
fails listing the bindings, and `--approve-plugins` explicitly authorises them
for this install. Approvals are stored in the snapshot's lock, so re-installing
re-prompts only when a plugin's fingerprint changes — the invalidation rule
that already exists.

## 7. Update and uninstall

There is no update command: `tey install <name>` re-installs from the
receipt's source, keeping its selector.

- tag and `--ref` installs are pinned; report "pinned at v0.4.0" and skip.
  Passing a new selector (`tey install rodolfo --tag v0.5.0`) moves the pin;
- branch/HEAD installs re-resolve; unchanged commit → "up to date";
- path installs re-snapshot; unchanged sha256 → "up to date";
- anything else rebuilds through the §4 flow and replaces in place.

A failed rebuild leaves the old program installed and runnable. With several
names, each is attempted and the summary lists which failed.

`tey uninstall <name>` removes exactly the bins its receipt lists (skipping,
with a warning, any that were since replaced by something the receipt does not
own), then `programs/<name>/`. Cache entries are left to `tey clean --cache`.
Without a name it is an error pointing at `tey list`.

## 8. Runtime compatibility

The escript's shebang runs `escript` from PATH, and a `.beam` cannot be loaded
by an OTP older than the one that compiled it. The receipt records the OTP
release that built each program; `tey list` marks programs built by a newer
OTP than the one now on PATH as "needs rebuild", and `tey install <name> --rebuild`
rebuilds regardless of source movement (also the answer after
switching to a Kex with a compiler fix the user wants).

Changing the selected Kex does not affect installed programs: the runtime and
prelude are embedded. That is a property worth stating in `tey kex use`'s
output only if users turn out to expect otherwise.

## 9. Implementation order

1. *(done)* Receipts for the existing project-local target install, plus the §5
   ownership rule and the `tey list` section. Useful alone and exercises the state.
2. `tey install <path>`: snapshot,
   build, publish atomically.
3. Git sources with `--tag/--branch/--ref`, `--package`, `--target`, locked by
   default with `--fresh`.
4. `tey uninstall`, re-install by name, `--rebuild`.
5. Non-interactive plugin approval (`--approve-plugins`).

## Verification

Pure specs: argument classification (URL vs path vs name), receipt round trip,
collision decisions (reserved / owned / foreign / `--force`), update decisions
per selector kind.

Temporary-repository integration tests with `TEY_HOME` and `TEY_CACHE` pointed
at a scratch directory: install from a local Git repo by tag and by branch;
install a workspace member; library rejected with the `tey add` hint; `kex(...)`
mismatch rejected; reinstall replaces bins and receipt; a build failure leaves
the previous version runnable; two packages exporting the same bin name;
`uninstall` leaves unowned files alone; re-install by name no-ops on an unmoved branch
and rebuilds after a new commit; the installed escript runs with a different
Kex selected.

## Deliberately deferred

- Registry names for first installs, with registry sources generally
- Bulk update of every installed program in one command
- Prebuilt/binary downloads — every install compiles locally
- Per-project tool pinning (a `tools do … end` block run via `tey exec`)
- Installing into a prefix other than `$TEY_HOME/bin` (`--root`)
- Shell completions or man pages shipped by installed programs
