# calligrus.vim

Live Markdown and Org preview in a separate [Calligrus](../../README.md) window. Vim sends the buffer text and cursor line, Calligrus shows them and scrolls along; Ctrl+click in the preview moves your cursor to that spot in the source. Plain legacy Vimscript for Vim 8.2+ (needs jobs, channels and timers), no dependencies. For Neovim see [editors/nvim](../nvim/README.md).

## Install

Add this folder to the runtimepath, or use a plugin manager. In `vimrc`:

```vim
set runtimepath+=/path/to/calligrus/editors/vim
let g:calligrus_exe = '/path/to/calligrus'   " only if it is not on PATH
```

or with vim-plug:

```vim
Plug '/path/to/calligrus/editors/vim'
```

Or use Vim's native packages: put (or link) this folder under `~/.vim/pack/calligrus/start/`. Run `:helptags ALL` once if you want `:help calligrus`. All options are optional; the defaults are below.

```vim
let g:calligrus_exe = 'calligrus'                " program, or a list: ['program', 'arg', ...]
let g:calligrus_debounce = 150                   " while typing, send the text at most every this many ms
let g:calligrus_follow = 1                       " every Markdown/Org buffer you switch to is previewed
let g:calligrus_goto = 1                         " Ctrl+click in the preview moves the cursor
let g:calligrus_filetypes = ['markdown', 'org']  " buffers :CalligrusToggle accepts
```

## Commands

| Command | What it does |
|---|---|
| `:CalligrusToggle` | Preview the current buffer on/off. With `g:calligrus_follow` the first toggle is all you need: other buffers join in; toggling one off keeps it off. `:CalligrusToggle!` skips the filetype check. |
| `:CalligrusStop` | Quit the preview process; all previews turn off. |
| `:CalligrusOpenFile` | Just open the buffer's file in a new Calligrus window (no live link). |

Org buffers (`filetype=org`) are sent as Org, everything else as Markdown.

While a buffer is previewed: edits are sent (debounced), the preview scrolls to your cursor line, switching to another previewed buffer shows that one, and deleting a buffer or quitting Vim cleans up. If you close the preview window, previews turn off with a message; `:CalligrusToggle` starts it again.

Your search highlight shows in the preview too (with `'hlsearch'` on): after `/`, `?`, `*`, `#`, `n`, `N` the matches are highlighted there, and `:nohlsearch` clears them. Case follows `'ignorecase'`, `'smartcase'` and `\c`/`\C`. Only plain-text patterns (also `\<word\>`, `\V`) are shown; real regexes like `foo.*bar` aren't.

**Ctrl+click in the preview** jumps to that place in the source: Calligrus sends `{"type":"goto","doc":...,"line":N}` on its stdout and the plugin shows that buffer (in the window of the current tab page that already shows it, else in the current window), puts the cursor on line N (clamped) at the first non-blank character and opens any fold there. Set `g:calligrus_goto = 0` to ignore clicks. The preview window keeps the OS focus.

The preview shows your unsaved text (nothing is read from disk), with relative links and images resolved against the file's folder.

## Protocol

JSON lines on Calligrus's stdin (`open`, `text`, `cursor`, `search`, `close`, `quit`) and, back on its stdout, `goto`; see sections 15, 17 and 23 of `spec/SPEC.md`. Any editor can use it.

## Tests

`test/headless_test.vim` drives the plugin in headless Vim against `test/stub.py` (a fake Calligrus that logs its stdin and, for the `goto` tests, writes lines that the test appends to `$CALLIGRUS_STUB_OUT` to its stdout; the test sets that itself). It needs `python` on PATH (or `$CALLIGRUS_PYTHON`).

```sh
CALLIGRUS_STUB_LOG=/tmp/calligrus_stub.log vim -Nu NONE -es -S test/headless_test.vim
cat /tmp/calligrus_stub.log.report   # PASS/FAIL lines
```

It exits non-zero on failure.
