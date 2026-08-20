# Kex support for Vim and Neovim

Runtime files for the [Kex](https://github.com/kexhq/kex) language: filetype
detection, syntax highlighting, indentation, `%` matching on `do`/`end`
blocks, comment settings — and a language-server client for Neovim.

## Install

Put this directory on your `runtimepath`. As a Vim/Neovim package:

```sh
# Neovim
mkdir -p ~/.config/nvim/pack/kex/start
cp -r editors/vim ~/.config/nvim/pack/kex/start/kex

# Vim 8+
mkdir -p ~/.vim/pack/kex/start
cp -r editors/vim ~/.vim/pack/kex/start/kex
```

Or, while working on Kex itself, one line in your config:

```vim
set rtp+=/path/to/kex/editors/vim
```

Neovim users on a plugin manager can load it as a plain directory plugin
(`lazy.nvim`: `{ dir = '/path/to/kex/editors/vim' }`).

## Language server (Neovim)

The compiler speaks LSP over stdio — the same server the VS Code extension
uses:

```sh
kex --lsp
```

The `lsp/kex.lua` here is a `vim.lsp.config` definition, picked up
automatically from the runtimepath. On Neovim 0.11+ enable it with:

```lua
vim.lsp.enable('kex')
```

That gives you hover (`K`), go to definition (`gd`), references (`gr`), and
omni completion (`CTRL-X CTRL-O`) once a server attaches; `:checkhealth
vim.lsp` shows its state. Requires a `kex` on PATH (`brew install
kexhq/tey/tey`); to point elsewhere:

```lua
vim.lsp.config('kex', { cmd = { '/path/to/build/kex', '--lsp' } })
vim.lsp.enable('kex')
```

On older Neovim, start it yourself:

```lua
vim.api.nvim_create_autocmd('FileType', {
  pattern = 'kex',
  callback = function(args)
    vim.lsp.start({
      name = 'kex',
      cmd = { 'kex', '--lsp' },
      root_dir = vim.fs.root(args.buf, { 'package.kex', '.git' }),
    }, { bufnr = args.buf })
  end,
})
```

## What's in the box

| File | Purpose |
| --- | --- |
| `ftdetect/kex.vim` | `*.kex` → `filetype=kex` |
| `syntax/kex.vim` | Full grammar: strings (raw, interpolated, char), regex literals, atoms, types, declarations, numbers, operators, doc-comment tags |
| `indent/kex.vim` | `do`/`end` blocks (including `do |params|`), `if`/`while` headers without `do`, branch heads, chain steps, wrapped brackets |
| `ftplugin/kex.vim` | `#` comments, `using` includes, matchit words for `%` |
| `after/ftplugin/kex.lua` | Neovim: sets `omnifunc`/`tagfunc` when the server attaches |
| `lsp/kex.lua` | Neovim `vim.lsp.config` entry for `kex --lsp` |
| `test/` | The test suite, and the fixture it indents |

The syntax file is a port of the VS Code grammar
(`editors/vscode/syntaxes/kex.tmLanguage.json`); keep the two in sync when
the grammar changes.

## Tests

`test/indent.kex` is a fixture written the way `=` should indent it: the test
reindents the whole file and fails on any line that moves. The same run checks
the filetype and ftplugin settings, and the syntax group a dozen constructs
land in.

```sh
nvim --headless -u editors/vim/test/run.vim
vim -es -u NONE -N -c 'source editors/vim/test/run.vim'
```

Both exit nonzero on failure and print what moved where.

## Indentation, in short

- `do`/`end` blocks, `do |params|` among them, and the `do`-less headers
  (`if cond`, `while cond`) indent their bodies one level.
- `end`, `else`, `elif`, `rescue`, and `after timeout:` line up with the line
  that opened their block, found by matching, so a block hung off a chain step
  lands correctly.
- A chain step on its own line (`.map { … }`) or a type variant (`| Square`)
  indents one level under what it continues, and the steps after it line up
  with the first.
- A wrapped bracketed expression indents one level; once a continuation line
  exists, later ones keep its column, so alignment under an opening paren
  survives a reindent.
- Lines inside a multi-line string are left exactly as written.
