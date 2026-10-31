# calligrus.nvim

Live Markdown preview in a separate [Calligrus](../../README.md) window. Neovim
sends the buffer text and cursor line, Calligrus shows them and scrolls along;
Ctrl+click in the preview moves your cursor to that spot in the source. Works in Neovim and Neovide (no terminal-specific code). Needs Neovim 0.10+.

## Install

Add this folder to the runtimepath, e.g. with lazy.nvim:

```lua
{ dir = "D:/path/to/calligrus/editors/nvim",
  config = function()
    require("calligrus").setup({ exe = "D:/path/to/calligrus.exe" })
  end }
```

or in plain `init.lua`:

```lua
vim.opt.rtp:append("D:/path/to/calligrus/editors/nvim")
require("calligrus").setup({ exe = "calligrus" })
```

`setup()` is optional; the defaults are below.

```lua
require("calligrus").setup({
  exe = "calligrus",              -- program, or a list { "program", "arg", ... }
  debounce = 150,              -- while typing, send the text at most every this many ms
  filetypes = { "markdown", "org" },  -- buffers :CalligrusToggle accepts ("org" is sent as Org)
  follow = true,               -- while the window is open, every Markdown buffer you
                               -- switch to is previewed (false: only toggled ones)
  goto = true,                 -- Ctrl+click in the preview moves the cursor (false: ignore it)
})
```

## Commands

| Command | What it does |
|---|---|
| `:CalligrusToggle` | Preview the current buffer on/off (with `follow`, the first toggle is all you need: other Markdown buffers join in; toggling one off keeps it off). Starts the shared `exe --editor` process if needed. `:CalligrusToggle!` skips the filetype check. |
| `:CalligrusStop` | Quit the preview process; all previews turn off. |
| `:CalligrusOpenFile` | Just open the buffer's file in a new Calligrus window (no live link). |

While a buffer is previewed: edits are sent (debounced), the preview scrolls
to your cursor line (when it is outside the middle 60 % of the view), switching
to another previewed buffer shows that one, and deleting a buffer or quitting
Neovim cleans up. If you close the preview window, previews turn off with a
message; `:CalligrusToggle` starts it again.

Your search highlight shows in the preview too: after `/`, `?`, `*`, `#`, `n`,
`N` the matches are highlighted there, and `:nohlsearch` clears them. Case
follows 'ignorecase'/'smartcase' and `\c`/`\C`. Only plain-text patterns
(also `\<word\>`, `\V`) are shown; real regexes like `foo.*bar` aren't.

**Ctrl+click in the preview** jumps to that place in the source: Calligrus sends
`{"type":"goto","doc":...,"line":N}` on its stdout and the plugin shows that
buffer (in the window of the current tabpage that already shows it, else in the
current window), puts the cursor on line N (clamped to the buffer) at the first
non-blank character and opens any fold there. The normal `cursor` message that
follows is harmless: the preview doesn't scroll when the spot is already in the
middle of its view. Set `goto = false` to ignore clicks. The preview window
keeps the OS focus (raising Neovim/Neovide is not attempted).

The preview shows your unsaved text (nothing is read from disk), with relative
links and images resolved against the file's folder.

## Protocol

JSON lines on Calligrus's stdin (`open`, `text`, `cursor`, `search`, `close`,
`quit`) and, back on its stdout, `goto`; see sections 15, 17 and 23 of
`spec/SPEC.md`. Any editor can use it.

## Tests

`test/headless_test.lua` drives the plugin in headless Neovim against
`test/stub.py` (a fake Calligrus that logs its stdin and, for the `goto` tests,
writes lines that the test appends to `$CALLIGRUS_STUB_OUT` to its stdout; the
test sets that itself):

```
set CALLIGRUS_RTP=<this folder>
set CALLIGRUS_STUB_LOG=%TEMP%\calligrus_stub.log
nvim --headless -n -u NONE --cmd "lua vim.opt.rtp:append(os.getenv('CALLIGRUS_RTP'))" -c "luafile test/headless_test.lua"
```

It prints PASS/FAIL lines and exits non-zero on failure.
