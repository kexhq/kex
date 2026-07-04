# @kexhq/kex

The [Kex](https://github.com/kexhq/kex) language interpreter compiled to
WebAssembly, packaged for embedding a Kex REPL in a web page or a Node
script. This is the same wasm binary built and tested in the main repo
(`src/wasm_repl.cxx`, `web/index.html`) — see `docs/fiber-process-plan.md`
there for the full design/testing history.

Published to GitHub Packages, not the public npm registry — see
"Installing" below.

## Status

**Pre-release.** Published under the `next` dist-tag as `0.0.0-dev.<sha>`
versions on every push to the `wasm-emscripten` branch — there is no
`latest`/stable release yet. Expect breaking changes without notice until a
real version is cut.

**Known limitation:** none currently — the Asyncify/JS-interop bug that
used to cause duplicated output and lost state for `receive`,
`receive timeout:`, and `Task.await` is fixed (see `docs/fiber-process-plan.md`).

## Installing

This package is published to **GitHub Packages**, so `npm install` needs to
be told to fetch `@kexhq` packages from there instead of the public
registry. Add to your project's `.npmrc`:

```
@kexhq:registry=https://npm.pkg.github.com
```

Then:

```
npm install @kexhq/kex@next
```

(GitHub Packages requires *reading* an npm package to be authenticated too,
even for public repos — set up an `NODE_AUTH_TOKEN`/`.npmrc` `_authToken`
with a GitHub personal access token that has `read:packages` scope. See
[GitHub's docs on installing packages](https://docs.github.com/en/packages/working-with-a-github-packages-registry/working-with-the-npm-registry).)

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
interpreter. Since there's no stable release yet, pin to a specific
`0.0.0-dev.<sha>` version (rather than floating on `next`) if you need
reproducible builds — every push to `wasm-emscripten` overwrites what
`next` points to.

## Building locally

From the main repo, with `emsdk` active (pinned to 5.0.7 — see
`third_party/gmp-wasm/README.md`):

```
cd packages/kex
npm run prepack   # builds build-wasm/ if needed, assembles dist/
npm pack          # produces a .tgz you can npm install locally to test
```
