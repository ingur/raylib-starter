-- Needs `vim.o.exrc = true` in your own config. Neovim will ask you to trust
-- this file once. Everything here is scoped to this project.
local root = vim.fn.fnamemodify(debug.getinfo(1, "S").source:sub(2), ":p:h")

vim.lsp.config("luau_lsp", {
  cmd = { "luau-lsp", "lsp", "--definitions:@raylib=" .. root .. "/types/raylib.d.luau" },
  filetypes = { "luau" },
  root_dir = root,
  -- luau-lsp defaults to roblox, which injects roblox globals and wants a sourcemap
  settings = { ["luau-lsp"] = { platform = { type = "standard" } } },
})
vim.lsp.enable("luau_lsp")
