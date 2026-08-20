-- Neovim only: wire omni-completion and tag lookup to the Kex language
-- server once it attaches to this buffer. Vim ignores .lua ftplugins; the
-- guard keeps a Lua-enabled Vim build quiet if it ever sources this anyway.
if not (vim and vim.api) then
  return
end

vim.api.nvim_create_autocmd('LspAttach', {
  buffer = 0,
  callback = function(args)
    local client = vim.lsp.get_client_by_id(args.data.client_id)
    if client and client.name:lower():find('kex', 1, true) then
      vim.bo.omnifunc = 'v:lua.vim.lsp.omnifunc'
      vim.bo.tagfunc = 'v:lua.vim.lsp.tagfunc'
    end
  end,
})
