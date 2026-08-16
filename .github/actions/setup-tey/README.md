# `setup-tey`

Installs Tey and a Kex toolchain from the published release archives, puts them
on `PATH`, and caches both.

```yaml
- uses: kexhq/kex/.github/actions/setup-tey@v0.3.5
- run: tey install
- run: tey build
- run: tey test
```

That is the whole thing. A complete workflow for a Kex package:

```yaml
name: CI
on:
  push:
    branches: [main]
  pull_request:

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

## Why not `brew install tey`

Because it costs two and a half minutes to install three megabytes. Measured on
`kexhq/greet`, on `ubuntu-latest`:

| Step | Before | After |
| --- | --- | --- |
| Install Tey and Kex | 2m 31s | ~2s cold, ~0.2s cached |

Homebrew on a Linux runner resolves and pulls a dependency chain for a machine
that is deleted three minutes later. The release publishes exactly what is
needed — `tey-<version>.tar.gz` is 252 KB — and Tey's own `tey kex install`
verifies the checksum and stages the compiler properly. This action does that.

## Inputs

| Input | Default | Meaning |
| --- | --- | --- |
| `kex-version` | `latest` | `0.4.0`, or `latest` for the newest stable release |
| `cache` | `true` | Cache the toolchain and the package cache between runs |
| `erlang` | `true` | Install Erlang/OTP; turn off if the image already has it |
| `repository` | `kexhq/kex` | Where releases are published, for forks and mirrors |
| `token` | `github.token` | Used to read the release list |

Outputs: `kex-version`, `tey-version`, `cache-hit`.

## What is cached, and why separately

```
~/.local/share/tey     the toolchain      keyed on the Kex + Tey version
~/.cache/tey           dependencies       keyed on tey.lock
```

Two caches rather than one, because they turn over on different events. A Kex
bump must not throw away every fetched dependency, and a lockfile edit must not
serve yesterday's compiler. The package cache also carries `restore-keys`, so
changing one dependency re-fetches one dependency rather than all of them.

The toolchain key uses the RESOLVED version, not the input — so `kex-version:
latest` starts filling a new cache the day a release lands, instead of pinning
whatever `latest` meant the first time the workflow ran.

## Pinning

`@v0.3.5` pins the action to a release tag, which is why it lives in the `kex`
repository rather than one of its own: the tags already exist and already mean
something. `@main` tracks the tip if you would rather.

## Other CI systems

There is deliberately no GitLab/CodeBuild/Woodpecker equivalent of this file.
Those all take an image plus a script, so the published container is the answer
there rather than a per-platform config to keep in step:

```yaml
image: ghcr.io/kexhq/kex:0.3.5
script:
  - tey install && tey build && tey test
```

All three image variants carry `tey` alongside `kex`, with `TEY_KEX` already
pointing at the compiler inside the image so nothing reaches for the network.
The image's `ENTRYPOINT` is `kex`, which CI systems taking an `image:` and a
`script:` replace themselves; from a plain `docker run`, reach Tey with
`--entrypoint tey`.
