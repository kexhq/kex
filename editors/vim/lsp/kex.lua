-- Kex language server configuration for Neovim's vim.lsp.config (0.11+).
-- The server is the compiler itself, speaking LSP over stdio (`kex --lsp`) —
-- the same server the VS Code extension drives.
--
-- With this directory on your runtimepath the config is discovered
-- automatically; all that is left is enabling it (e.g. in init.lua):
--
--   vim.lsp.enable('kex')
--
-- To point at a specific build instead of the `kex` on PATH:
--
--   vim.lsp.config('kex', { cmd = { '/path/to/build/kex', '--lsp' } })
--   vim.lsp.enable('kex')

return {
  cmd = { 'kex', '--lsp' },
  filetypes = { 'kex' },
  root_markers = { 'package.kex', '.git' },
}
