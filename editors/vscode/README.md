<p align="center">
  <img src="https://github.com/kexhq/kex/raw/main/docs/assets/logo.png" alt="Kex" height="220" />
</p>

<h3 align="center"><a href="https://kex.run">Try Kex in your browser on kex.run</a></h3>

# Kex Language Support for VS Code

Syntax highlighting, type-aware hover, completion, navigation, and live
compiler diagnostics for [Kex](https://github.com/kexhq/kex).

Kex is a functional language with Ruby-like syntax, immutability by default, UFCS method chains,
and an Elixir-style process model. Try the language in your browser at [kex.run](https://kex.run).

The extension is a thin client. Everything it shows comes from the Kex compiler
itself, running as a language server (`kex --lsp`), so hover types and
diagnostics are the compiler's own answers rather than a separate model of the
language that could drift from it.

![Hover on an overloaded method, showing which overload was selected, the others available, and the documented example](https://github.com/kexhq/kex/raw/main/editors/vscode/images/hover.png)

![Completion after a dot in a builder chain, listing each method with its full signature](https://github.com/kexhq/kex/raw/main/editors/vscode/images/completion.png)

## Features

- **Type and declaration hover** — the declaration as written, plus any doc
  comments above it. On an overloaded method it names the overload chosen at
  that call site and lists the alternatives.
- **Diagnostics as you type** — syntax, type, and validation errors on the
  unsaved buffer, not just on save.
- **Completion** — members after `.`, and top-level names, with signatures and
  overload counts.
- **Go to definition** and **find references**.
- **Syntax highlighting** for Kex's full grammar.
- **Automatic server restart** when the configured compiler is rebuilt inside
  the workspace.

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
[repository README](https://github.com/kexhq/kex#readme) — then point
`kex.executablePath` at the binary.

## Settings

| Setting | Default | Description |
| --- | --- | --- |
| `kex.executablePath` | `kex` | Compiler used by the language server. Relative paths resolve from the workspace root, so `build/kex` works when developing Kex itself. |

## Commands

| Command | Description |
| --- | --- |
| **Kex: Restart Language Server** | Restarts the server without reloading the window. |

## Extension Development

1. Build Kex from the repository root: `cmake --build build --target kex`.
2. In this directory, run `bun install` and `bun run compile`.
3. Open this directory in VS Code and press F5 to launch an Extension
   Development Host.
4. Set `kex.executablePath` to `build/kex` when developing Kex itself, or
   install `kex` on `PATH`.

The server can also be driven by any LSP client:

```sh
kex --lsp
```

It communicates over standard input/output using LSP `Content-Length` frames.

When the configured executable is inside the workspace (for example
`build/kex`), rebuilding it automatically restarts the language server. You can
also run **Kex: Restart Language Server** from the command palette.
