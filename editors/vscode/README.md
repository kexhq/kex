<p align="center">
  <img src="https://github.com/kexhq/kex/raw/main/docs/assets/logo.png" alt="Kex" height="220" />
</p>

<h3 align="center"><a href="https://kex.run">Try Kex in your browser on kex.run</a></h3>

# Kex Language Support for VS Code

Syntax highlighting, type-aware hover, completion, navigation, and live
compiler diagnostics for [Kex](https://github.com/kexhq/kex).

Kex is a functional language with Ruby-like syntax, immutability by default, UFCS function chains,
and an Elixir-style process model. Try the language in your browser at [kex.run](https://kex.run).

![Hover on an overloaded function, showing which overload was selected, the others available, and the documented example](https://github.com/kexhq/kex/raw/main/editors/vscode/images/hover.png)

![Completion after a dot in a builder chain, listing each function with its full signature](https://github.com/kexhq/kex/raw/main/editors/vscode/images/completion.png)

## Features

- **Type and declaration hover** — the declaration as written, plus any doc
  comments above it. On an overloaded function it names the overload chosen at
  that call site and lists the alternatives.
- **Diagnostics as you type** — syntax, type, and validation errors on the
  unsaved buffer, not just on save.
- **Completion** — functions after `.`, and top-level names, with signatures and
  overload counts.
- **Go to definition** and **find references**.
- **Test explorer** — every `describe`/`it` in a `*.spec.kex` file in the
  Testing view, runnable per case, with a failure marked on the line that
  produced it.
- **Package commands as tasks** — whatever this package's `package.kex`
  declares, offered where VS Code offers tasks.
- **Syntax highlighting** for Kex's full grammar.

## Requirements

The extension needs a `kex` executable. Kex ships with **Tey**, its toolchain
manager, and installing Tey installs a working `kex`:

```sh
brew install kexhq/tey/tey
kex --version
```

The `kex` on your PATH is Tey's dispatcher: it runs whichever toolchain Tey has
selected, so there is only ever one compiler command.

```sh
tey kex list [--pre]        # released and installed versions
tey kex install [<version>] # newest stable, or the one you name
tey kex use <version>       # switch what `kex` runs
```

Not on macOS or Linux with Homebrew? Build from source — see the
[repository README](https://github.com/kexhq/kex#readme) — then pick it with
**Kex: Select Toolchain → Choose a Kex binary…**.

## Choosing a toolchain

The extension resolves its compiler through Tey, so it runs the same Kex your
shell does. **Kex: Select Toolchain** lists the versions Tey has installed,
marks the one Tey itself has selected, and adds two entries: *Use Tey's
selection*, which follows `tey kex use` from then on, and *Choose a Kex
binary…*, for a compiler you built yourself.

The choice is saved per workspace, so a Kex checkout can run its own
`build/kex` while every other window keeps the machine-wide selection. The
status bar shows which compiler actually started — `Kex 0.4.0-alpha`, or
`Kex 0.4.0-alpha (build/kex)` for a binary of your own — and turns into a
restart button if the server goes down.

Left alone, the extension follows the package you have open: a Tey package
declares the Kex it builds against (`kex(">= 0.3.4")` in `package.kex`,
resolved to an exact version in `tey.lock`), and that version is the one the
editor uses, so diagnostics come from the same compiler as `tey build`. Pick
*Use Tey's selection* to ignore the lock file.

In full, the compiler is the first of:

1. `kex.executablePath`, when you have set it — an escape hatch that overrides
   everything below.
2. The version pinned by `kex.toolchain` (what the picker writes).
3. The version this package's `tey.lock` names, when it is installed.
4. Tey's own selection, from `tey kex which`.
5. A bare `kex` on PATH.

## Settings

| Setting | Default | Description |
| --- | --- | --- |
| `kex.toolchain` | *(empty)* | Toolchain the language server runs: empty follows this package's `tey.lock` and then Tey's selection, `tey` follows Tey's selection alone, a version such as `0.4.0` pins one Tey has installed, an absolute path runs your own build. Set it with **Kex: Select Toolchain**. |
| `kex.executablePath` | `kex` | Compiler used by the language server, overriding Tey and `kex.toolchain`. Relative paths resolve from the workspace root. |
| `kex.sourceRoots` | *(empty)* | Extra directories searched for `using` modules, on top of the workspace's own `lib`/`src`. Relative paths resolve from the workspace root. Tey dependencies need nothing here — the server reads those from `tey.lock`. |
| `kex.testExplorer.backend` | `auto` | Which backend the Testing view runs on: `auto` follows the runner (the BEAM under Tey, the interpreter otherwise), or say `walker`/`beam` outright. Both report identical results. |

### The Testing view

Open the Testing view and every `*.spec.kex` file in the workspace is listed;
expanding one asks the runner what is inside it — a discovery pass that runs
the file's `describe` blocks but no `it` body — so a spec that generates its
cases in a loop is listed exactly as it will run. ▶ on a case runs that one
case; ▶ on a `describe` or a file runs everything under it. A failure is marked
on the line that produced it.

In a Tey package the runner is **Tey**, from the package root:

```
tey test spec/greet.spec.kex --list
tey test spec/greet.spec.kex --json --only "outer > inner > case"
```

because only Tey knows the package's source roots — its own `src/` and every
locked dependency's — and `tey test` is how the suite is run everywhere else.
Outside a package it is the compiler directly, with the same flags under their
compiler spelling (`--test-list`, `--test-json`, `--test-only`).

Either way the flags belong to the tools, not to this extension: run those
lines in a terminal and you get the same records. `--only` in particular is
worth knowing by hand when one case in a long file is failing.

### Modules from dependencies

A package's dependencies resolve on their own: the server finds the nearest
`package.kex`, reads its `tey.lock`, and searches each locked dependency's
`src/` in the Tey cache — the same roots `tey build` passes the compiler. Run
`tey install` and a dependency's modules hover, complete and navigate like
your own. `kex.sourceRoots` is for the layouts no lock file describes:
vendored trees, monorepo siblings, a checkout that is not a Tey package.

## Commands

| Command | Description |
| --- | --- |
| **Kex: Select Toolchain** | Pick which Kex the language server runs, and restart it on the new one. |
| **Kex: Restart Language Server** | Restarts the server without reloading the window. |

## Other editors

Vim and Neovim have their own runtime files in
[`editors/vim`](../vim). The syntax file there is a port of this extension's
TextMate grammar (`syntaxes/kex.tmLanguage.json`) — when the grammar changes,
change both.

## Extension Development

1. Build Kex from the repository root: `cmake --build build --target kex`.
2. In this directory, run `bun install` and `bun run compile`.
3. Open this directory in VS Code and press F5 to launch an Extension
   Development Host.
4. Run **Kex: Select Toolchain → Choose a Kex binary…** and pick `build/kex`
   when developing Kex itself, or install `kex` on `PATH`.

The server can also be driven by any LSP client:

```sh
kex --lsp
```

It communicates over standard input/output using LSP `Content-Length` frames.

Rebuilding the compiler automatically restarts the language server, wherever
that binary lives — a workspace `build/kex`, a Tey toolchain, or `kex` on
PATH. You can also run **Kex: Restart Language Server** from the command
palette, or click the Kex status bar item when the server is down.

## Tests

```sh
bun run test              # resolution logic, no editor involved
bun run test:integration  # the extension inside a real VS Code
bun run test:all
```

Neither suite needs a Kex. `test/fixtures/kex-stub.mjs` is a compiler that
answers `--info`, `--version` and a minimal `--lsp`, laid out in a throwaway
`TEY_HOME` of fake toolchains, so the tests say the same thing on a machine
with four toolchains installed and on a CI runner with none.

- **`test/unit`** covers `src/toolchain.ts`: resolution order, `tey.lock`,
  `$TEY_HOME` listing, version ordering. It runs in under a second.
- **`test/integration`** downloads a VS Code and drives the extension in it —
  that the server starts on the right compiler, comes back after that compiler
  is rebuilt or killed, and that the picker writes what it says it writes.
  Point it at another build with `KEX_TEST_EXTENSION=/path/to/extension`.
