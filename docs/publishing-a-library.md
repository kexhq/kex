# Publishing a Kex library

A Kex library is published by **tagging a Git repository**. There is no
registry to upload to and no account to hold: Tey resolves a dependency by
cloning the repository the manifest names and checking out a commit, so
publishing is making the right commit reachable under the right tag.

`https://github.com/kexhq/greet` is the worked example throughout — the first
Kex package, kept deliberately small so the whole path can be tested on it.

## Make the package

```sh
tey new greet --lib      # a new directory
tey init --lib           # or: adopt the directory you are already in
```

`--lib` means no `entrypoint` and no `target`: a library is something other
packages compile against, not something that runs. Both commands produce the
same tree.

```
greet/
  package.kex          the manifest
  README.md            how to depend on it, and how to release it
  src/greet.kex        module Greet
  spec/greet.spec.kex  a spec that already passes
  .gitignore           ebin/ and erl_crash.dump
  .gitattributes       the tey.lock merge driver
```

The README matters more for a library than it looks: there is no registry
rendering a description somewhere else, so the repository page IS the
distribution. The generated one already carries the dependency line and the
tag convention — the two things an author is least likely to write from
memory.

`tey init` names the package after the directory it is standing in. Pass a
name when that is wrong — a checkout of `kexhq/greet` sitting in `greet-main`
is still `greet`:

```sh
tey init greet --lib
```

The module name follows the package name in PascalCase, and the FILE follows
the module: `hello-world` gives `module HelloWorld` in `src/helloworld.kex`,
because the resolver finds a module by lowercasing its name.

## Fill in the manifest

```kex
# The first Kex package, kept small on purpose.
package "greet" do
  version("0.1.0")
  description("Greets a user, or the whole world")
  kex(">= 0.3.3")
  license("MIT")
end
```

`package.kex` is Kex source rather than JSON so it can carry comments — on
their own line, after a declaration, or inside one spread over several lines.
Use them.

`version` is the number you will tag. `kex` is the compiler requirement, and
Tey refuses to resolve the package on a compiler that does not satisfy it, so
name the oldest version you have actually tested. `description` is one line
saying what the package is for; it is optional, but a name alone never says
enough.

The reader is strict: a declaration it does not recognize is an error, not a
comment. That is deliberate — a typo in a manifest must not be silently
dropped — and it means a misspelled field fails loudly at `tey install` rather
than going missing.

## Check it before anyone depends on it

```sh
tey build     # compiles src/ into ebin/
tey test      # runs spec/*.spec.kex, on the BEAM
```

Both see the package's own `src/` and every fetched dependency, and both mean
the BEAM: what `tey build` produces is what ships, so a spec suite that proves
anything has to run the same way. `tey test --interpret` (and `tey run
--interpret`) tree-walks the source instead — for a backend gap, or a
debugging session.

`tey test` also takes the specs to run — `tey test spec/greet.spec.kex` while
you are working on one — and, for a tool rather than a person, `--json`,
`--list` and `--only <name>`: one JSON record per case with source locations,
the cases discovered without running any, and a run narrowed to one case. That
is what the VS Code test explorer drives; `docs/testing.md` has the record
shape.

## Name the commands your package needs

The other things a package needs run — the formatter, the migration, the
release script — go in the manifest rather than in a `Makefile` beside it:

```kex
package "greet" do
  version("0.1.0")

  command("fmt", run: "kex --format src", description: "Format the sources")
  command("db:migrate", run: "script/migrate.kex")
  command("release", run: "script/release.kex")
end
```

`tey fmt` runs it, and trailing arguments are appended (`tey fmt src`). A name
is one word; group related ones with a colon — `db:migrate`, `assets:build` —
the separator other package managers already use, and one that needs no
quoting in a shell. A
`run:` whose first word ends in `.kex` is run through the toolchain Tey
selected, with the package's source roots already in place — the same
environment `tey run` gives, so a release script can `using` the package it is
releasing. Anything else is a shell line, so pipes and `&&` work, with Tey's
own `bin` directory first on its `PATH`.

`tey help` lists them under the built-in ones, which is where someone looks
who has just cloned the repository. A built-in name (`test`, `build`, …)
cannot be redeclared: `tey test` means the same thing in every checkout, and a
manifest that tries to change that is an error at read time.

**`tey test`
exits non-zero when a spec fails** — so a CI job that runs it is a CI job that
can actually fail. (Before Kex 0.3.5 it did not, which made every downstream
CI green regardless of what the specs said. If you are pinning an older
compiler, check the output rather than the status.)

## Wire up CI

```yaml
name: CI
on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: kexhq/kex/.github/actions/setup-tey@v0.3.5
      - run: tey install
      - run: tey build
      - run: tey test
```

`setup-tey` installs Tey and the compiler from the release archives and caches
both — see `.github/actions/setup-tey/README.md`. Do not reach for
`brew install tey` in CI: on a Linux runner it costs two and a half minutes to
deliver 252 KB.

## Tag the release

```sh
git tag v0.1.0
git push origin v0.1.0
```

The tag is the release. Tey lists a repository's tags to resolve a version
range, so an untagged repository can only be depended on by branch or by
commit — which is not a published library, it is a moving target.

Use `vMAJOR.MINOR.PATCH`, matching the `version` in the manifest. A leading `v`
is optional as far as Tey is concerned (it strips one when parsing), but be
consistent inside one repository. Pre-release identifiers — `v0.2.0-rc.1` —
parse and sort below their release, and a version range never resolves to one.

## Depend on it

In the consuming package:

```sh
tey add greet --git https://github.com/kexhq/greet --tag "~> 0.1"
```

which appends to its `package.kex`:

```kex
tey("greet", git: "https://github.com/kexhq/greet", tag: "~> 0.1")
```

and writes `tey.lock`:

```json
"greet": {
  "git": "https://github.com/kexhq/greet",
  "resolved": "refs/tags/v0.1.1",
  "commit": "22c6db55eb807504d9a6d0eee9af44eb52230833",
  "sha256": "1d43746e5b4a31987e2776767860d7965d46783f08f8fcefc97859bda0554923",
  "groups": []
}
```

The manifest keeps the QUESTION (`~> 0.1`) and the lockfile keeps the ANSWER
(`v0.1.1`, at that exact commit, with a digest over its tree). A build is
therefore exact while the constraint stays open: re-running `tey update` picks
up `v0.1.2` when it exists, and nothing else moves until it does.

Then in the source:

```kex
using Greet, only: [hello]

main(args) do
  hello()
end
```

`--tag "~> 0.1"` is the shape to reach for. The alternatives are worth knowing
and worth avoiding in anything published:

| Selector | Means | Use when |
| --- | --- | --- |
| `tag: "~> 0.1"` | highest tag satisfying the range | ordinary dependency on a released library |
| `tag: "v0.1.0"` | exactly that tag | pinning deliberately |
| `branch: "main"` | that branch's tip, pinned by commit in the lockfile | the library has no tags yet |
| `ref: "<sha>"` | one commit | reproducing something specific |

`tey.lock` **is committed**. It is what makes a checkout build the same code
everywhere, and `.gitattributes` installs a merge driver so two branches
adding dependencies do not conflict on it by hand:

```sh
tey setup merge-driver
```

## What is not there yet

Transitive dependencies. Tey reads the manifest of the package you are
building, not the manifests of what it fetches — so a library's own
dependencies do not come along, and two constraints on the same package are
not unified. Keep published libraries dependency-free until that lands;
`docs/tey-limitations.md` tracks it, along with the rest.
