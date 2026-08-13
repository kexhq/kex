# Kex Language Support for VS Code

This extension provides Kex syntax highlighting, compiler-backed completion,
type/declaration hover, and live syntax/type/validation diagnostics through the
language server built into the `kex` compiler.

## Development

1. Build Kex from the repository root: `cmake --build build --target kex`.
2. In this directory, run `bun install` and `bun run compile`.
3. Open this directory in VS Code and press F5 to launch an Extension
   Development Host.
4. Set `kex.executablePath` to `build/kex` when developing Kex itself, or
   install `kex` on `PATH`.

The server can also be used directly by any LSP client:

```sh
kex --lsp
```

It communicates over standard input/output using LSP `Content-Length` frames.

When the configured executable is inside the workspace (for example
`build/kex`), rebuilding it automatically restarts the language server. You can
also run **Kex: Restart Language Server** from the command palette.
