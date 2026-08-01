-- requires `vim.o.exrc = true`. Trust this file when Neovim asks.
local root = vim.fn.fnamemodify(debug.getinfo(1, "S").source:sub(2), ":p:h")

vim.lsp.config("luau_lsp", {
  cmd = { "luau-lsp", "lsp", "--definitions:@raylib=" .. root .. "/types/raylib.d.luau" },
  filetypes = { "luau" },
  root_dir = root,
  settings = { ["luau-lsp"] = { platform = { type = "standard" } } },
})
vim.lsp.enable("luau_lsp")
