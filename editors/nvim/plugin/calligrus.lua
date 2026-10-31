-- Commands only; the work is in lua/calligrus/init.lua (loaded on first use).
if vim.g.loaded_calligrus then
  return
end
vim.g.loaded_calligrus = true

vim.api.nvim_create_user_command("CalligrusToggle", function(opts)
  require("calligrus").toggle(opts.bang)
end, { bang = true, desc = "calligrus: toggle the live preview of this buffer (! = ignore 'filetype')" })

vim.api.nvim_create_user_command("CalligrusStop", function()
  require("calligrus").stop()
end, { desc = "calligrus: quit the preview window; all previews turn off" })

vim.api.nvim_create_user_command("CalligrusOpenFile", function()
  require("calligrus").open_file()
end, { desc = "calligrus: open this file in a separate calligrus window (no live link)" })
