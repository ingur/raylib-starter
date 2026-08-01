-- Neovim project config for luau-lsp. Requires `vim.o.exrc = true` in your own
-- config, and Neovim will ask you to trust this file once.
--
-- Two things have to happen that are not defaults: Neovim maps *.lua to the
-- `lua` filetype, so luau-lsp never attaches to game scripts, and the server
-- only loads type definitions from its command line. The definitions path must
-- be absolute because luau-lsp resolves relative ones against its own cwd.

local root = vim.fn.fnamemodify(debug.getinfo(1, "S").source:sub(2), ":p:h")
local definitions = root .. "/types/raylib.d.lua"

vim.filetype.add({ pattern = { [root .. "/game/.*%.lua"] = "luau" } })

vim.lsp.config("luau_lsp", {
  cmd = { "luau-lsp", "lsp", "--definitions:@raylib=" .. definitions },
  filetypes = { "luau" },
  root_dir = root,
  settings = {
    ["luau-lsp"] = {
      -- the server defaults to the roblox platform, which injects roblox
      -- globals and warns about a missing sourcemap
      platform = { type = "standard" },
      sourcemap = { enabled = false },
      -- duplicated from the command line on purpose: this copy is what stops
      -- the server from type checking types/raylib.d.lua as ordinary source
      types = { definitionFiles = { ["@raylib"] = definitions } },
    },
  },
})
vim.lsp.enable("luau_lsp")
