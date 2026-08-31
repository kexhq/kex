# @kexhq/kex

The [Kex](https://github.com/kexhq/kex) language interpreter compiled to
WebAssembly, packaged for embedding a Kex REPL in a web page or a Node
script. This is the same wasm binary built and tested in the main repo
(`src/wasm_repl.cxx`, `web/index.html`).

For more information, please visit our official site at [kex.run](https://kex.run).

Published to the public npm registry as [`@kexhq/kex`](https://www.npmjs.com/package/@kexhq/kex),
and mirrored to GitHub Packages for internal consumers already wired to that
path (e.g. kex.run's site repo).

## Status

**Pre-release.** The package's version follows the language's `VERSION` file:

- Published only on release, with the plain `VERSION` (`0.3.0-beta`) as its
  number and the same dist-tag rule as the container images: `latest` for a
  stable release, the channel name (`rc`, `beta`) for a pre-release. The
  `version` field in `package.json` is a placeholder — the number is stamped
  at publish time.

Expect breaking changes without notice until 1.0.0; pin a version if you need
reproducible builds.

**Known limitation:** none currently — the Asyncify/JS-interop bug that
used to cause duplicated output and lost state for `receive`,
`receive timeout:`, and `Task.await` is fixed.

## Installing

```
npm install @kexhq/kex
```

## Usage

```js
import { Kex } from "@kexhq/kex";

const session = await Kex.create();

// Evaluate one chunk of Kex source at a time — state (let/var bindings,
// spawned processes) persists across calls on the same session, exactly
// like a real REPL.
console.log(await session.eval("1 + 2"));
// => "=> 3 : Int\n"

await session.eval("let x = 5");
console.log(await session.eval("x + 10"));
// => "=> 15 : Int\n"

// Tab completion — see web/index.html in the main repo for a full
// reference client (word-break-character scanning, history, line editing).
console.log(session.complete("IO.printL", 0, "IO.printL"));
// => ["IO.printLine"]

session.destroy();
```

Ships with TypeScript definitions (`Kex`, `KexModuleOptions`) — no `@types`
package needed, and no `new Kex(...)`: the constructor is private, use
`Kex.create()`.

Multi-line input (`do ... end` blocks) needs to be accumulated into one
string before calling `eval` — this package doesn't do that for you (see
`web/index.html`'s `countBlocks()` for the exact logic the real REPL uses to
decide when a block is complete).

Output already contains ANSI color escape codes (matching the native CLI's
REPL exactly) — render it through a real terminal emulator (e.g.
[xterm.js](https://xtermjs.org/), as `web/index.html` does) rather than
stripping them, unless you specifically want plain text.

## For kex.run specifically

This is the package kex.run is expected to import as its in-browser
interpreter. Pin an exact released version (`@kexhq/kex@0.3.0-beta`) for a
deployed build.

## Building locally

From the main repo, with `emsdk` active (pinned to 5.0.7 — see
`third_party/gmp-wasm/README.md`):

```
cd packages/kex
npm run prepack   # builds build-wasm/ if needed, assembles dist/
npm pack          # produces a .tgz you can npm install locally to test
```
