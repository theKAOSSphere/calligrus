# Calligrus specification

Calligrus is a native Markdown and Org viewer: one executable with its own TeX math engine,
Mermaid renderer, syntax highlighting (tree-sitter) and PDF export. This document is the behaviour
specification the code and its tests follow. Section numbers are referred to from source
comments and test fixtures (for example "spec 20").

Related files in this repository:
- `themes/`: the built-in themes, embedded in the executable.
- `grammars/`: the vendored tree-sitter grammars, queries and `theme.txt` / `languages.txt`.
- `spec/tests/`: shared test inputs (config files, theme files, fixtures) with their expected
  results; the console tests in `src/*_test.cpp` and the visual suite in `spec/tests/visual/` run them.
- `editors/emacs`, `editors/nvim`: editor plugins that drive `calligrus --editor` (sections 15 to 18, 23).

In the sections below, `<root>` stands for the repository folder.

---

## 1. Zoom

- `Ctrl+=` / `Ctrl++` (also numpad +): zoom in. `Ctrl+-`: zoom out. `Ctrl+0`: reset to 1.0.
- `Ctrl` + mouse wheel: zoom (one notch = one step) instead of scrolling.
- Steps of 0.1, clamped to 0.5 ... 3.0. Applies to the document (text, math,
  diagrams, images) and the outline; the toolbar scales too.
- C++: every font size must stay whole-pixel (`Fonts::px()` rounds); math is
  re-rendered at the new size (the cache is keyed by size already).
- Keep the reading position: the block at the top of the view stays at the top
  after a zoom change.
- The zoom level is saved to the config (`zoom`), see §5.
- Android (§35): the zoom applies to the document only (the toolbar and widgets keep their size), and a
  pinch zooms around the fingers.

## 2. Vim-style keys

Active when `vim_keys = true` (default true) and no text field has focus
(find bar, settings fields). Standard keys keep working regardless.

| Keys | Action |
|---|---|
| `j` / `k` (and `Down` / `Up`) | scroll down / up by 3 body lines (3 × 1.55 × body size) |
| `d` / `u`, `Ctrl+d` / `Ctrl+u` | half a page down / up |
| `f` / `b`, `Space` / `Shift+Space`, `PageDown` / `PageUp` | a page down / up (page = view height − 2 lines) |
| `g g` / `G`, `Home` / `End` | top / bottom |
| `]` `]` / `[` `[` | next / previous heading (scroll it to the top, like clicking it in the outline) |
| `/` | open the find bar, vim style (§7): Enter searches and returns the keys to the document |
| `Ctrl+F` | open the find bar, browser style (§7): Enter = next match, the field keeps focus |
| `n` / `N` | next / previous find match (when there is a query; after `/` Enter they step through the matches) |
| `o`, `Ctrl+B` | toggle the outline panel |
| `r` | reload (`F5` is slides on / off, §26) |
| `t` | open the theme picker (§6.1); in slides mode `t` is the presenter clock instead (§26). `g t` / `g T` stay next / previous tab (§34); `T` does nothing |
| `Esc` | close find bar (clears the highlights) / cancel pending keys / clear selection |

- Counts: digits before a motion repeat it (`5j`, `3]]`). `0` alone is not a
  motion (it starts nothing); max count 999. A pending count or `g`/`[`/`]`
  prefix expires after 1.5 s or on any other key.
- All scrolling goes through the existing smooth-scroll animation. Key scrolls (`j`/`k`/`d`/`u`/
  `f`/`b`, `Ctrl+E`/`Ctrl+Y`, `gg`/`G`) add to where a scroll under way is heading, so quick
  presses (or a held key) never lose distance.

## 3. Middle-click autoscroll (browser style)

- Middle button pressed over the document → autoscroll starts with the
  *anchor* at the press point. Draw an anchor marker there: a 26 px circle
  (theme `weak` outline, `bg` fill at 85%) with small up/down triangles.
- While active: velocity (px/s) = sign(dy) × max(0, |dy| − 10)^1.5 × 0.6,
  where dy = mouse y − anchor y (dead zone 10 px). Cap at 6000 px/s. Applied
  every frame (time-based, not frame-based); keep rendering while active.
- Mode: if the button is released within 250 ms and the mouse moved < 6 px →
  "toggle" mode: stays active until any mouse button press, a wheel event, or
  `Esc`. Otherwise "hold" mode: ends when the button is released.
- Mouse cursor: a vertical-resize / all-scroll style cursor while active.
- Middle-clicking a link does nothing special (no new window); autoscroll wins.

## 4. Code blocks

- Background `code_bg`, 1 px border `code_border`, corner radius 6 px (scaled).
- Language label: top-right, small italic `weak` text (as now).
- **Copy button**: when the mouse is over the block, a small button appears at
  the top-right (left of the label), text `Copy`; click copies the block's
  exact source text to the clipboard and shows `Copied` for 1.5 s.
- **Long lines**: no wrapping. A block wider than the column scrolls
  horizontally on its own: `Shift`+wheel over the block, or dragging a thin
  (4 px) scrollbar that appears under the block when it overflows (hover
  makes it 6 px). Text selection still works on the visible part.
- **Line numbers**: when `code.line_numbers = true`, a right-aligned gutter in
  `weak` colour, 1 ch gap, not included in copied text.
- **In a PDF export** (§29, §33; pages and slides, `Renderer::export` / `render_set_export`) there
  is no scrolling: code lines wider than the column **wrap** after as many columns as fit;
  continuation lines are indented by 2 columns with a small `↪` (`weak` colour) in the indent, and
  carry no line number. The text layer has the wrapped lines. Scroll bars (code, wide tables and
  formulas) are never drawn in an export. The on-screen app keeps its horizontal scrolling.

## 5. Config file

Location: `$CALLIGRUS_CONFIG` (a file path), if set; otherwise `config.toml` in a folder
`calligrus` (`~` = `%USERPROFILE%` on Windows):
1. A `calligrus` folder that already exists wins, checked in this order: `$XDG_CONFIG_HOME/calligrus`,
   `~/.config/calligrus`, `%APPDATA%\calligrus` (Windows). So the location never moves under the user,
   e.g. when another tool creates `~/.config` later.
2. Otherwise (first launch): `$XDG_CONFIG_HOME/calligrus` if `XDG_CONFIG_HOME` is set; else
   `~/.config/calligrus` if a `~/.config` folder exists (on Linux always, even if it doesn't exist
   yet); else `%APPDATA%\calligrus` (Windows without a `~/.config`).

The file is **created** there (settings panel, zoom, "Open config
file") by the first setting change. User themes live in `themes/` next to the config file in use,
and the window / scroll / recent-files state in `state.toml` in the same folder (`$CALLIGRUS_STATE`
overrides that one file). The old `MDVIEW_*` names of these variables are still read when the
`CALLIGRUS_*` one is not set.

**Migration.** The program used to be called mdview and kept its files elsewhere:
`%APPDATA%\mdview\` on Windows, `~/.config/mdview/` (or `$XDG_CONFIG_HOME/mdview/`) on Linux, with
`state.toml` possibly in `~/.local/state/mdview/` (`$XDG_STATE_HOME/mdview/`). When the new folder
does not exist at start-up (and `CALLIGRUS_CONFIG` is not set), `config.toml`, `state.toml` and
`themes/` are **copied** into the folder chosen by rule 2 from the first old location that has them. Nothing is moved or deleted,
and an existing new folder is never touched. The old built-in theme names `mdview-dark` and
`mdview-light` are accepted as aliases of `calligrus-dark` and `calligrus-light` in `theme`,
`dark_theme` and `light_theme`.

A missing file = all defaults (don't create it until a setting changes). Unknown keys are
ignored (kept in the file). Invalid values fall back to the default for that key (and log/print a
warning); a broken file never prevents start-up.

```toml
# calligrus configuration
theme = "auto"            # "auto" = follow the system light/dark setting, or a theme name
dark_theme = "calligrus-dark"   # used by "auto" when the system is dark
light_theme = "calligrus-light" # used by "auto" when the system is light
zoom = 1.0                # 0.5 .. 3.0
body_size = 16            # body text size in logical pixels at zoom 1.0 (12 .. 28); exports use 0.75 pt per px
max_width = 860           # widest reading column in points at zoom 1.0 (400 .. 2400)
show_outline = true       # outline panel visible at start-up
vim_keys = true
system_fallback = true    # installed fonts fill characters the bundled ones lack (§30)

[fonts]
prose = ""                # path to a TTF/OTF for body text and headings; "" = built-in IBM Plex Sans
prose_bold = ""           # optional; "" = built-in bold if prose is built-in, else the regular file
prose_italic = ""         # optional, same rule
prose_bold_italic = ""    # optional, same rule
mono = ""                 # path for code; "" = built-in Cascadia Code

[code]
line_numbers = false
```

- Paths may use `~` (home) and environment variables like `%USERPROFILE%`.
- A font file that fails to load → fall back to the built-in face, show a
  short non-blocking message in the existing error bar.
- **Writing** (settings panel, zoom changes): edit *in place*: replace the value
  on the line that defines the key (keeping the line's trailing comment and
  everything else in the file byte-for-byte), or append the key at the end of
  its table (create the `[fonts]` / `[code]` header if missing). Never rewrite
  the whole file. Write atomically (temp file + rename). Debounce writes
  (300 ms after the last change).
- **Reading**: at start-up, and again whenever the file changes on disk (watch
  it like the document; ignore the change notification caused by our own write).
- Shared tests (`spec/tests/`): `config-*.toml` inputs with
  `config-*.expected` (one `key = value` per line, fully resolved defaults) and
  `edit-*.toml` + `edit-*.ops` + `edit-*.expected-toml` for the in-place writer.
  `calligrus-config-test` runs all of them.

## 6. Themes

A theme is a TOML file. Built-ins: everything in `themes/`, embedded
in the executable. User themes: `<config dir>/themes/*.toml` (a user theme with the
same `name` replaces a built-in). The theme picker lists all, by `name`.

```toml
name = "calligrus-dark"         # identifier used in config.toml
display_name = "calligrus Dark"  # shown in the picker
dark = true                  # dark UI chrome and dark text coverage curve

[ui]                         # toolbar, outline, popups, widgets
panel = "#1b1b1b"            # toolbar / outline background
border = "#3c3c3c"           # separators, widget outlines
widget = "#3c3c3c"           # button background
widget_hover = "#464646"
widget_active = "#373737"
text = "#b4b4b4"             # widget text
weak = "#5f5f5f"             # disabled / hint text
field = "#0a0a0a"            # text-field background
selection = "#005c80"        # selected toggle fill and text-selection highlight
selection_text = "#c0deff"   # text on a selected toggle
accent = "#5aa0ff"           # focus outline, find "current match" outline

[doc]                        # the document
bg = "#1b1b1b"
text = "#e0e0e0"
weak = "#969696"             # quotes, captions, labels, line numbers
link = "#6ea8fe"
heading = "#e0e0e0"          # optional, default = text
# heading_1 .. heading_6     # optional per-level colours (§22); default = heading, h6 = weak
code_bg = "#262626"          # code blocks and inline code background
code_border = "#333333"
code_fg = "#ebbe8c"          # inline code text and unhighlighted code
rule = "#3a3a3a"             # horizontal rules, table borders, heading underline
quote_bar = "#4a4a4a"
table_head_bg = "#262626"
zebra = "#202020"            # alternate table rows
error = "#ff7878"
find_match = "#6b5a1e"       # background of find matches
find_current = "#a07f14"     # background of the current match

[syntax]                     # code highlighting, keyed by the entry names of grammars/theme.txt
comment = "#65737e italic"   # "#rrggbb" plus optional words: bold, italic
keyword = "#b48ead bold"
string = "#a3be8c"
# ... a missing entry falls back along its dotted prefix (function.builtin → function);
# no match at all → plain: doc.text in code blocks (today's look); doc.code_fg is
# only for inline `code` spans
```

- Colours: `#rrggbb` or `#rrggbbaa`.
- Missing `[ui]` / `[doc]` keys: take the value from the built-in
  `calligrus-dark` (if `dark = true`) or `calligrus-light`, so partial user themes
  work.
- **Highlighting change**: `grammars/theme.txt` keeps the list of entry names
  and which captures are "plain" (`-`), but its colour columns are dropped (or
  ignored). The highlighter resolves a capture to an *entry name* (as the
  golden files already record); the colour/bold/italic come from the active
  theme's `[syntax]` table at draw time. Golden files stay valid.
- Switching themes is live (no restart). The toolbar's theme control becomes
  a dropdown of all themes plus "Auto (system)".
- Built-in themes (files in `themes/`): `calligrus-dark`,
  `calligrus-light` (the default look), `leuven` (light), `bearded-vivid-black`,
  `gruvbox-dark`, `nord`, `catppuccin-mocha`, `tokyo-night`, `solarized-dark`,
  `solarized-light`, `dracula`, `print-dark` (near-black page for dark PDFs, after
  a browser print stylesheet). The code must not hard-code this list: embed
  whatever `*.toml` files the folder contains at build time.

### 6a. Resolved details (`spec/tests/README.md` has the full rules)

- `doc.heading` missing → the theme's own `doc.text` (not calligrus-dark's).
- A user theme with **no `[syntax]` table at all** inherits the `[syntax]` table of
  its base (calligrus-dark if `dark = true`, else calligrus-light). A theme that has a
  `[syntax]` table uses only that table (dotted-prefix fallback, then plain).
- `dark` missing or not a boolean → `false`.
- Syntax lookup is two-stage: capture → entry name via `grammars/theme.txt`
  (longest dotted prefix; `-`/unlisted/`_`-prefixed → plain), then entry name →
  style via the theme's `[syntax]` (dotted-prefix fallback, then plain).
- Config: invalid or out-of-range values fall back to the key's default (never
  clamped); the writer keeps a trailing comment in its column when the gap is
  spaces. Everything else as in `spec/tests/README.md`.

### 6b. More resolved details

- Plain/unhighlighted code in code blocks: `doc.text`. `doc.code_fg`: inline code only.
- Vim: a key that doesn't complete a pending prefix (`g` then `j`) cancels the
  prefix and its count, then acts on its own; a digit after a prefix cancels it
  and starts a new count. Counts also apply to standard keys (`3 PageDown`);
  `G`, `End`, `Home` ignore counts.
- `Esc` does one thing, first applicable of: cancel pending keys → stop
  autoscroll → close find bar → clear selection.
- Autoscroll "moved < 6 px" = maximum distance from the anchor while held.
  Only the left button selects text.
- Find: an inline equation is one unmatchable character; a hard line break is
  `\n`; matches never span paragraphs, table cells or code lines. A new query
  (Ctrl+F) starts at the first match in/below the top visible block; with `/` at the first match
  at or after the view position where `/` was pressed (§7). Current match:
  `find_current` fill + 1 px `ui.accent` outline.
- Copy button copies the block text without trailing newlines.
- Theme picker: sorted by `name`, showing `display_name`.
- The label grey (not in theme files) = mix of `ui.panel` and `ui.text`
  (113/153 of the way in dark themes, 168/188 in light).

### 6.1 Theme picker, `t`

`t` opens the toolbar's theme dropdown from the keyboard, already open, with the current theme
highlighted (`Action::ThemePicker` in `navigation.cpp`; the dropdown itself is `theme_picker` in
`settings.cpp`).

- **Gating**: like `j` / `k`: vim keys on, normal reading mode (not slides mode, where `t` stays
  the clock; not the page export), no text field focused, no other popup open. A count before `t`
  is dropped. After a pending `g`, `t` is still next tab. Opening the dropdown with the mouse works as before.
- **Rows**: "Auto (system)" first, then the themes sorted by name (the dropdown's own list).
- **While open**: `j` / `Down` and `k` / `Up` move the highlight, **without wrapping** (it stops at the
  first / last row, like the recent-files popup). The document **previews the highlighted theme
  live** (the config value is changed in memory only: nothing is written to the config file while
  the picker is open). The list scrolls to keep the highlight visible. Mouse hover does **not**
  preview (only the keyboard highlight does).
- **Enter** keeps the highlighted theme: exactly what clicking it does, so the normal
  debounced config write (`theme = ...`) happens, and nothing is written when it is the theme
  that was active before `t`. **Esc** closes the picker, restores the theme active before `t` and
  saves nothing. **Clicking a row** keeps it (as without the picker); **clicking outside** counts as Esc.
  Leaving the toolbar's mode (slides, page export) while it is open also reverts.
- **No key leaks** to the document while it is open (no scrolling, no `/`, no marks; Esc does not
  also close the find bar). Keyboard navigation inside a mouse-opened dropdown is not added.
- Test: `theme-picker.mdscript` (visual suite: `t j Down` preview, Esc reverted, `t j Enter` kept; every
  shot shows the top of the page); key parsing in `calligrus-nav-test`.

## 7. Find in page

- `Ctrl+F` or `/` opens a find bar at the top of the document view (below the
  toolbar): a text field, a match counter `3/17` (or `no matches`), ↑ / ↓
  buttons, and a close button. `n` / `N` (vim keys) work whenever a
  query exists and no text field has focus. How the bar was opened decides what Enter and Esc do:
  - **Opened with Ctrl+F (browser style)**: `Enter` = next match, `Shift+Enter` = previous, the
    field keeps focus; `Esc` closes the bar and clears the highlights.
  - **Opened with `/` (vim style)**: the search starts at the view position where `/` was
    pressed (the first match at or after the top of the view, wrapping to the first match of the
    document; not the position the view has scrolled to while typing). `Enter` runs the search
    now and **gives the keys back to the document**: the field loses focus, the query and the
    highlights stay, the current match is visible. Then `n` / `N` step through the matches (and
    all other vim keys work). `Esc` (in the document, or still in the field) does one thing,
    closes the bar and clears the highlights (§6b order: pending keys, autoscroll, find bar,
    selection). `/` again reopens the bar (after Esc) with the previous query back in the field,
    or (bar still open) puts the keys in the field again; in both cases the whole text is
    selected, so typing replaces it, and the next Enter searches again from the view position
    of that `/`. A `/` typed to open the bar is not inserted into the field.
  - Ctrl+F while the field has focus keeps (or makes) the bar browser style; `*` / `#` (§28) open the bar
    with the keys on the document and no vim mode (Enter in the field = next match).
- Matching: plain substring over the document's text runs (the selection
  model's runs: paragraphs, headings, list items, table cells, code lines).
  **Smartcase**: case-insensitive unless the query contains an uppercase
  letter. Math is not searched (its runs are atomic).
- All matches get a `find_match` background, the current one `find_current`;
  the view scrolls so the current match is visible (centred if it was off
  screen). Search updates as you type, **debounced**: the search runs 150 ms after the last edit
  of the field (the counter shows `…` meanwhile), at once on Enter, at once when the query
  becomes empty; script mode never waits.
- Matches must be found in off-screen blocks too (search the document text,
  not only what was painted this frame).
- **Cost**: the searchable text (the groups of select.h: each group's characters, their selection
  positions, and each run's line box) is **built once per layout** (document version, column width, font
  size, columns / styles; `FindIndex` in find.h, kept in the tab's find state and dropped when no query
  needs it), not per keystroke; a search is then only a substring search over the prepared (case-folded)
  group texts. The editor-search highlight (§17) uses the same text. (Developer switch for measuring:
  a file `find_legacy` in the home folder, or the environment variable `CALLIGRUS_FIND_LEGACY`, prepares the text
  again at every search; timings are logged with `CALLIGRUS_TRACE=1`, on Android with tag `calligrus`.)
- Tests: `find-vim.mdscript` (visual: `/`, `fox`, Enter, `n`, `n n`, `N`, Esc, `/` and `dog` replacing the
  selected query), using the script command `expect-find closed | field|doc MATCHES CURRENT` (the bar
  is closed / open with the keys in the field / open with the keys on the document, and the counter) to check
  the focus; `find.mdscript` covers Ctrl+F.

## 8. Fonts: embedded defaults + custom prose/mono

- Embed all default fonts in both exes (IBM Plex Sans ×4, Cascadia Code, Hack, Noto Sans Symbols 2, the emoji /
  icon fallbacks, Latin Modern Math, which is also the very last fallback of every font chain, §27). The C++ app must no longer need an
  `assets/` folder (keep reading `assets/demo.md` only as an optional demo;
  better: embed demo.md too).
- Custom fonts from `[fonts]` (§5). C++: the size factor must come from the
  font's own metrics, not the hard-coded `kPlexPx = 1.3` / `kMonoPx = 1.164`:
  factor = (ascender − descender) / units_per_em from the font file (the
  metrics ImGui/FreeType use for sizing), so 1 em renders at the requested size.
- Changing fonts applies live (C++: rebuild the atlas, as the theme switch does).

### 8a. Fonts by family name

- A `[fonts]` value that contains no `/` or `\` and doesn't end in
  `.ttf`/`.otf`/`.ttc` (case-insensitive) is a **family name**, e.g.
  `prose = "Inter"`, `mono = "Cascadia Code"`. Anything else is a path.
- Font index: scan the system font folders (Windows: `%WINDIR%\Fonts` and
  `%LOCALAPPDATA%\Microsoft\Windows\Fonts`; Linux later: fontconfig's dirs) and
  read each file's `name` table (typographic family/subfamily, IDs 16/17, else
  1/2), `OS/2` weight class and italic bit (fsSelection bit 0, or subfamily
  containing "Italic"/"Oblique"), and monospace (`post.isFixedPitch` != 0, or
  OS/2 panose proportion = 9). For `.ttc` index every face (face index kept and
  passed to the loader). Build it in the background at start-up (it may take
  ~a second); cache nothing on disk. Family match is case-insensitive.
- Picking faces for a family: regular = non-italic face with weight closest to
  400; bold = non-italic closest to 700 (must be ≥ 600, else none); italic =
  italic face closest to 400; bold-italic = italic closest to 700 (≥ 600). Ties:
  the lower weight, then the file path order. A missing style uses the §5 rule
  (fall back to the family's regular). Explicit `prose_bold`/… values
  (paths or names) override the automatic pick for that style.
- Width: index each face's `OS/2` `usWidthClass` (1–9, 5 = normal; missing or
  out of range → 5). When picking any style, faces closest to width 5 win
  first; weight rules apply among those (so "Arial" picks Arial, not Arial
  Narrow). Fixture face lines get an optional 7th field `width` (default 5).
- Unknown family → built-in face + error-bar message (`font "X" not found`).
- Variable fonts: load the default instance only (note it in the README).
- Settings panel: the prose and mono fields become a searchable dropdown of
  installed families (type to filter; mono list shows monospace families only,
  with a "Show all" checkbox), plus `Browse…` for files and `Reset`. Selecting a
  family writes its name to the config.
- Shared test: `spec/tests/fontpick-*.txt` (a fake index: one face per line
  `family | subfamily | weight | italic | mono | file#face`, then queries and
  expected picks; format in `spec/tests/README.md`). The resolver must
  pass it, separately from the real scanner.

## 9. Settings panel

- Toolbar button `⚙` opens a settings window (an ImGui
  window) styled like the rest of the UI. Contents, top to bottom:
  - Theme: dropdown (Auto + all themes); when Auto: dark/light theme dropdowns.
  - Text size (body_size slider 12–28), Zoom (slider + reset).
  - Fonts: prose / mono path fields with `Browse…` (native file dialog,
    filter *.ttf;*.otf) and `Reset` buttons; the optional bold/italic fields
    under a collapsible "More".
  - Toggles: Vim keys, Show outline at start-up, Code line numbers.
  - Buttons: `Open config file` (opens it in the default editor, creating it
    from the current values if missing) and `Open themes folder`.
- Every change applies live and is written to the config (§5 writer).

## 10. Static linking (single-file executable)

- vcpkg triplet `x64-windows-static` and `CMAKE_MSVC_RUNTIME_LIBRARY` `MultiThreaded` (/MT);
  `build.ps1` passes the triplet. Result: `calligrus.exe` runs with no DLLs and (with §8) no
  assets folder. Check with `dumpbin /dependents`: only system DLLs (kernel32, user32,
  gdi32, opengl32, shell32, comdlg32, dwmapi, ...).
- On Linux the libraries are static too (triplet `x64-linux`); the executable only links the
  system's libc, libstdc++ and X11/GL.

## 11. Icon, keys, links, history, state file

- **Icon**: `assets/icon/icon.svg` (white ∫ + amber Markdown ↓ on a `#141417`
  tile with a faint white rim; older variants in `assets/icon/variants/`).
  Built assets: `icon.ico` (16–256 px) for the exe resource, `icon-256.png`
  for the window/taskbar icon. Regenerate with the `svgpng` example + Pillow.
- **Keys** (additions to §2): `Ctrl+e` / `Ctrl+y` scroll one body line
  (1 × 1.55 × body size; counts; vim_keys only). `q` quits (vim_keys, no text
  field focused, no pending prefix). `F11` toggles borderless fullscreen on the
  current monitor (always active; `Esc` keeps its old meanings). `zz/zt/zb`
  deferred until the editor integration gives us a "current line"; `H/M/L`
  (cursor motions) dropped, `H/L` reused for history.
- **Links**: `#anchor` → scroll to the heading with that GitHub-style slug
  (unique: `foo`, `foo-1`, …). Relative path (no scheme), optional `#anchor`,
  resolved against the current file's folder: existing `.md`/`.markdown` →
  open in Calligrus, then jump; other existing files → OS default program;
  missing → error bar. URLs with a scheme → OS default handler.
- **History**: locations (file, scroll). Following an in-app link or opening a
  file (dialog/drop) pushes the current location; new navigation after going
  back truncates forward; cap 100. Back/forward: `Alt+←/→`, mouse X1/X2
  (always), `H`/`L` (vim_keys, counts). Reload / on-disk change don't push.
- **State file** (separate from config): `$CALLIGRUS_STATE`, else
  `state.toml` next to `config.toml` (`~/.config/calligrus/state.toml`, §5).
  Atomic writes; missing/broken → defaults.
  ```toml
  # calligrus state (written by the app; safe to delete)
  [window]
  x = 100          # outer position, physical pixels
  y = 80
  width = 1100     # client-area size, physical pixels
  height = 1300
  maximized = false

  [[scroll]]
  path = 'C:\Share\me\notes\a.md'   # absolute
  offset = 1234.5                  # document px at zoom 1.0
  ```
  Window: saved at exit (pre-fullscreen rect when fullscreen); restored only if
  the top 40 px strip (≥ 100 px wide) intersects a monitor's work area; else the
  default placement. Config key `remember_window = true` (+ settings checkbox
  "Remember window size and position"); false → [window] not read/written.
  Scroll: record a file's offset when it's closed/replaced or at exit (move to
  front, max 50, Windows paths case-insensitive); restore on open (not on
  back/forward or reload).

## 12. Front matter, callouts, footnotes

- **Front matter**: a first line (after an optional BOM) exactly `---` with a
  later line exactly `---` or `...` → that block is removed before parsing.
- **Callouts**: a blockquote whose first line is exactly `[!NOTE]`, `[!TIP]`,
  `[!IMPORTANT]`, `[!WARNING]` or `[!CAUTION]` (case-insensitive, trailing
  spaces ok): marker removed, 4 px left bar + bold title (`Note`, …) in the
  kind's colour. Dark/light: note `#4493f8`/`#0969da`, tip `#3fb950`/`#1a7f37`,
  important `#ab7df8`/`#8250df`, warning `#d29922`/`#9a6700`, caution
  `#f85149`/`#cf222e`. Other `[!FOO]` = ordinary blockquote.
- **Footnotes**: `[^label]: text` (+ indented continuation) definitions;
  numbered by first reference; references → `[n]` link (0.8× size, raised
  ~0.35 em) to `#fn-n`; unknown labels stay literal. At the end: a rule, then
  `n. text` paragraphs with anchors `fn-n` (not in the outline); optional ` ↩`
  to `#fnref-n`. Clicks go through the normal link path (history).
- C++ `\tag`: flush right at the column edge on the formula baseline.

## 13. Recent files, "Open with", leftovers

- **Recent files** (state file, §11): a top-level array before `[window]`:
  ```toml
  recent = ['C:\Share\me\notes\a.md', 'D:\b.md']   # newest first, max 10
  ```
  A file goes to the front when it's opened (command line, dialog, drop, link
  to another .md, back/forward into it); Windows paths case-insensitive, no
  duplicates. Not on reload. Unknown keys elsewhere in the file must survive
  (keep [window] and [[scroll]] as in §11).
  - UI: a small `▾` button right after `Open…` in the toolbar opens a popup
    list: file name (normal text) + folder (weak text, elided in the middle if
    long), newest first; files that no longer exist are shown disabled with a
    `(missing)` suffix. Click → open (same path as the dialog: pushes history,
    restores scroll). Last row: `Clear recent` (empties the list). Empty list →
    one disabled row `No recent files`.
  - `Ctrl+Shift+O` (always active) opens the same popup with keyboard
    navigation: `Up`/`Down` (and `j`/`k`), `Enter` opens, `Esc` closes.
- **"Open with" registration** (Windows only): command-line flags, no window.
  - `--register`: per-user (HKCU, no admin). ProgId = `<exe stem>.markdown`
    (so builds with different exe names coexist):
    `HKCU\Software\Classes\<ProgId>` default = `Markdown document`,
    `\DefaultIcon` default = `"<exe>",0`,
    `\shell\open\command` default = `"<exe>" "%1"`;
    `HKCU\Software\Classes\.md\OpenWithProgids` and `...\.markdown\OpenWithProgids`
    get an empty string value named `<ProgId>`;
    `HKCU\Software\Classes\Applications\<exe file name>\SupportedTypes` gets
    empty values `.md`, `.markdown` and `FriendlyAppName` = `calligrus` on the `Applications\<exe file name>` key. Then
    `SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr)`.
    It does NOT change the default program (Windows doesn't allow that
    silently); the user picks it in "Open with → Choose another app".
  - `--unregister`: removes exactly what `--register` added (the ProgId key,
    the two `OpenWithProgids` values, the `Applications\<exe>` key), then the
    same SHChangeNotify. Missing keys are fine.
  - Output: a one-line result (`Registered <exe> for .md/.markdown` or the
    error). Both exes are GUI-subsystem: `AttachConsole(ATTACH_PARENT_PROCESS)`
    and print if that works, else a MessageBox. Exit code 0/1.
  - Code takes the root key path as a parameter so tests can use
    `HKCU\Software\calligrus-test\Classes` (and delete it afterwards). Never touch
    the real `Software\Classes` from tests.
- **Footnote leftovers**:
  - Footnote marks are raised (0.8× size, baseline raised ~0.35 em).
  - Back-links: the FIRST reference to footnote n gets the anchor `fnref-n`;
    each footnote paragraph ends with ` ↩` linking to `#fnref-n` (normal link
    path → history). The anchor scrolls the reference's block into view (same
    as headings: block top to the top of the view).
- **Readable current find match**: text drawn over the `find_current` fill
  uses black or white, whichever contrasts more with `find_current` (relative
  luminance > 0.4 → `#000000`, else `#ffffff`). Other matches unchanged.
- **Code-block language label**: drawn on an opaque pill (`code_bg`, radius
  4 px scaled, 4 px horizontal padding) so it stays readable over code that
  scrolls under it. Same when the Copy button shows.
- A table directly after a `> [!KIND]`
  line: md4c can't interrupt a paragraph with a table, so `model.cpp`
  `split_callout_markers` inserts an empty quote line after marker lines.
- **Binary files** (every way of opening a file: dialog, recent, drop, command line, links, tabs,
  reload; `md::looks_binary`): content that is binary (a NUL byte in
  the first 8 KB, a signature `%PDF` / PNG / JPEG / `GIF8` / `PK`, or more than 30 %
  invalid UTF-8 there) is not parsed. The document area shows "NAME is not a text file (looks like
  a PDF). Calligrus shows Markdown and Org files." and an "Open with default app" button (the same
  hand-over to the OS as links to non-Markdown files). Fixture `binary-note`; unit tests in `calligrus-model-test`.

## 14. Headless script mode

Goal: check the app visually and in automated tests without a visible window (while the user works
on the only screen, with the PC locked, or on a CI machine), with the SAME script giving the same
output every time.

- **Trigger**: `--script <file>` (or env `CALLIGRUS_SCRIPT=<file>`). Config and
  state lookups are as usual (`CALLIGRUS_CONFIG`, `CALLIGRUS_STATE` still apply), but
  in script mode the state file AND the config file are never WRITTEN (zoom,
  theme etc. change only in memory), and nothing is
  registered/opened externally (links to URLs/other files are logged, not
  launched).
- **No visible window, ever**: no window shown, no taskbar entry, no focus
  change, nothing drawn on screen. Rendering goes through the normal pipeline
  (GLFW + OpenGL3), into an offscreen target if a
  hidden window's default framebuffer isn't reliable (FBO + `glReadPixels`).
  Must also work while the workstation is locked. DPI scale = 1.0 in script
  mode unless `scale` says otherwise, so output doesn't depend on the monitor.
- **Script**: UTF-8 text, one command per line, `#` comments, blank lines ok.
  Paths relative to the script's folder, except `shot` files: relative to
  `--out DIR` (env `CALLIGRUS_SCRIPT_OUT`) when given, else the script's folder.
  Missing folders for shots are created. Commands:
  | Command | Meaning |
  |---|---|
  | `size W H` | client size in pixels (default 1200 900) |
  | `scale S` | DPI scale (default 1.0) |
  | `open PATH` | open a file (as from the dialog: history, recent, scroll restore) |
  | `keys TOKENS` | key presses, space-separated tokens: a letter/digit/symbol (`j`, `G` = shift+g, `/`, `[`), named keys (`esc enter tab space up down left right pageup pagedown home end f5 f11 backspace`), modifiers joined with `+` (`ctrl+f`, `ctrl+shift+o`, `alt+left`, `ctrl+plus`, `ctrl+minus`, `ctrl+0`) |
  | `type TEXT` | text input (rest of the line) into whatever has focus |
  | `click X Y [left\|right\|middle\|x1\|x2]` | press + release at client coords (window pixels, whatever the zoom or scale) |
  | `move X Y` | move the mouse (hover) |
  | `drag X1 Y1 X2 Y2` | left drag (selection) |
  | `wheel DY [X Y]` | mouse wheel, DY in notches (+ = down), at X Y (default centre) |
  | `settle` | run frames until nothing animates / no repaint pending (max 300 frames) |
  | `frames N` | run exactly N frames |
  | `shot FILE.png` | save the client area (RGBA, exactly W×H×scale) |
  | `export FILE` | export the slides as a PDF to FILE (relative to the output folder, like `shot`) as the PDF button does, without the dialog, and wait for it (fails after 120 s; §27; after an `editor` `open`: the editor's text) |
  | `echo TEXT` | print to stdout |
  | `expect-title TEXT` | fail if the window title differs |
  | `expect-find closed` / `expect-find field\|doc MATCHES CURRENT` | fail unless the find bar is closed / open with the keys in its field / open with the keys on the document, with that many matches and that (1-based, 0 = none) current one (§7) |
  | `quit` | exit 0 (also implied at end of file) |
  - A `keys` token that is a symbol without a key of its own (e.g. `*`, `#`) sends only its
    character (no key press).
  - After every input command the app runs frames until settled (as `settle`).
  - Time: animations use real time, `settle` simply waits for them; no clock faking.
  - Errors (unknown command, missing file, bad args, `expect-*` failure) →
    message on stderr with the line number, exit code 1. Each `shot` prints
    `shot FILE WxH` on stdout.
  - Stdout/stderr: both exes are GUI-subsystem; in script mode attach to the
    parent console (`AttachConsole(ATTACH_PARENT_PROCESS)`) if any, and ALSO
    append every line to `<script>.log` next to the script.
- **Shared examples**: `spec/tests/script/` — `smoke.mdscript` (open demo-ish
  doc, shots at top / after `G` / after `ctrl+f` find / after `ctrl+plus`),
  plus the test docs it needs. It must produce same-sized PNGs on every OS;
  small pixel differences between OSes (font anti-aliasing) are expected.
- Exit code 0 on success. A run should take seconds, not minutes.

## 15. Editor live preview

One-way: the editor drives Calligrus; Calligrus never talks back (yet). One shared
Calligrus window per editor session shows whichever previewed buffer is current.

- **Launch**: the editor starts `calligrus --editor`
  with stdin and stdout as pipes. In this mode Calligrus reads protocol messages
  from stdin; stdin EOF (editor quit/crashed) → Calligrus exits. No file is
  opened from the command line; until the first `open` the view shows a short
  "Waiting for the editor…" line. The window behaves normally otherwise
  (state.toml window geometry, config, themes, keys, find, zoom...).
- **Protocol**: UTF-8 JSON, one object per line (`\n`), field `type`:
  | type | fields | meaning |
  |---|---|---|
  | `open` | `doc` (string id), `path` (absolute file path or ""), `text` | show this document now (create or replace it) |
  | `text` | `doc`, `text` | new full text of `doc` (ignored if unknown doc) |
  | `cursor` | `doc`, `line` (1-based source line) | scroll-sync to that line (only if `doc` is shown) |
  | `close` | `doc` | the buffer is gone; if it's shown, keep the last text but drop live updates |
  | `quit` | — | exit |
  Unknown types/fields are ignored; a malformed line is reported on stderr and
  skipped. Messages are applied on the UI thread; if several `text` messages
  for the shown doc are waiting, only the newest is parsed (coalesce).
- **Editor documents**: the text comes from the editor, NOT the disk — no file
  watching for them, reload (`r`) re-renders the last received text.
  Relative links/images resolve against `path`'s folder (or the cwd if path is
  empty). The title/toolbar shows the path (or `doc`) followed by ` (live)`.
  History: switching docs via `open` does not push history. Links that open
  another .md file still work as usual (they leave live mode for that view;
  the next `open`/`text` for the editor doc brings it back). Scroll positions
  of editor docs are not saved in state.toml; recent files: not added.
- **Scroll sync** (`cursor`): every rendered top-level block knows its source
  line range [first, last] (1-based, in the text after front matter is
  stripped — add the stripped line count back). Target = the block containing
  `line` (else the last block starting before it); y = block top +
  (line − first) / (last − first + 1) × block height. If that y is already
  within the middle 60 % of the view, do nothing; else scroll (smooth, as
  usual) so y sits at 1/3 of the view height from the top. After a `text`
  update, keep the last synced line at the same view position (re-apply the
  last `cursor` without animation).
  - md4c has no positions;- **Script mode** (§14) gets `editor JSON` (rest of the line is one protocol
  message, applied as if read from stdin), so all of this is testable headless.
- **Neovim plugin** (`editors/nvim`, a standard plugin layout:
  `lua/calligrus/init.lua`, `plugin/calligrus.lua`, `README.md`):
  - `require("calligrus").setup({ exe = "calligrus", debounce = 150, filetypes = { "markdown" } })`
  - Commands: `:CalligrusToggle` (preview on/off for the current buffer; starts
    the shared process if needed), `:CalligrusStop` (quit the process),
    `:CalligrusOpenFile` (level 1: just launch `exe <file>` detached, no link).
  - Sends: `open` on toggle-on and on BufEnter of a previewed buffer;
    `text` on TextChanged/TextChangedI (debounced `debounce` ms, per buffer);
    `cursor` on CursorMoved/CursorMovedI (debounced ~50 ms, only the line);
    `close` on BufDelete/BufWipeout of a previewed buffer; `quit` on VimLeavePre.
    Uses `vim.fn.jobstart(cmd, { stdin = "pipe" })` + `vim.fn.chansend`,
    `vim.json.encode`, `nvim_buf_get_lines(buf, 0, -1, false)` joined with \n.
    If the process exits, previews turn off with a notification; toggling
    again restarts it.
  - Works in nvim and Neovide (no terminal-specific code).

## 16. Cursor marker and sync accuracy dump

- **Cursor marker** (editor docs only, once a `cursor` message has arrived):
  - A bar in the left margin next to the block that contains the cursor line
    (the same target block §15's scroll sync uses): 3 px wide (scaled),
    `accent` colour at 60 % alpha, from the block's top to its bottom,
    8 px (scaled) left of the reading column's left edge.
  - If the block spans more than one source line, also a tick at the
    interpolated line position (the §15 y): 8 px × 2 px (scaled), `accent` at
    100 %, centred on the bar.
  - Updated on every `cursor` and after every `text` (re-mapped), drawn under
    nothing else, never selectable, not part of find. Nothing for normal
    (non-editor) documents. Callouts/quotes/lists: the bar is still in the
    margin of the top-level block (no nesting for now).
- **Script command `sync-dump FILE`** (for checking line mapping):
  writes a TSV for the shown document: header `line\tfirst\tlast\ty`, then one
  row per source line 1..N (N = number of lines of the text): `first`/`last` =
  the source line range of the target block for that line (as §15), `y` = the
  §15 target y in document units at zoom 1.0, 1 decimal, measured from the
  top of the document content (a fixed origin). Relative to
  `--out`. Works for editor docs and normal docs alike.

## 17. Editor search highlight

- **Protocol** (§15 table gets one more type):
  `search` — fields `doc`, `text` (plain text; "" = clear), `case` (bool,
  true = case-sensitive). Ignored if `doc` isn't shown.
- **App**: highlight every match of `text` in the shown editor doc with the
  find colours (`find_match`; no "current" match), using the same text
  matching as find-in-page (§7: same searchable text, same case rule but
  driven by `case`). No find bar, no scrolling, no count shown. Kept across
  `text` updates (re-matched); cleared by `search` with "" and by `open` of a
  different doc. If the user opens the app's own find bar (Ctrl+F or `/`),
  that takes over while it is open; closing it shows the editor search again.
- **Script mode**: nothing new (`editor {"type":"search",…}`).
- **Neovim plugin**: send `search` for the current previewed buffer when the
  search highlight changes: after `/` or `?` (CmdlineLeave with cmdtype / or ?),
  `*` `#` `n` `N` (check on CursorMoved: compare `@/` and `v:hlsearch` with
  what was last sent), `:nohlsearch` (CmdlineLeave for `:` + the same
  comparison), and on BufEnter/open. Send "" when `v:hlsearch` is 0 or
  'hlsearch' is off. Pattern → text: strip `\<` `\>` `\c` `\C` `\V` `\v`(only
  if nothing else magic follows); if what's left still contains regex magic
  (`.*[]~^$\{}+=?|()` after `\v`, or backslash classes like `\d \s \w \+ \=`)
  send "" (can't show it faithfully). `case`: `\C` → true, `\c` → false,
  else `ignorecase` off → true; `smartcase` on and the text has an uppercase
  letter → true; otherwise false.

## 18. Subtler marker, item-level sync, `editor_marker`

- **Marker look** (replaces §16's numbers): bar 2 px wide (scaled), `accent`
  at 35 % alpha; tick 8 × 2 px, `accent` at 80 %. Position unchanged (8 px
  left of the column).
- **Item-level targets**: the sync target (scroll sync, marker, sync-dump —
  one shared computation, as in §16) goes one level into containers:
  - list → the list ITEM containing the line (top-level list's items only;
    a nested list inside an item belongs to that item),
  - blockquote / callout → the child block containing the line (paragraph,
    code, list…; again one level only),
  - everything else as before (top-level block).
  The bar spans only that target; `first`/`last` in sync-dump are the target's
  source lines; y interpolates inside the target. The tick shows only if the
  target spans more than one source line AND isn't a single paragraph/list
  item (paragraph lines don't map linearly to wrapped visual lines, so a tick
  there would mislead): i.e. code blocks, display math, tables, and items or
  quote children that contain several blocks.
  - A line that falls between items/children (blank line) → the previous one.
  - Callout marker line `> [!NOTE]` → the callout's title (top of the callout).
- **Config** `editor_marker = true` (boolean, default true; settings checkbox
  "Show the cursor marker in editor previews", next to the other view
  options). false → no marker; scroll sync unchanged. Shared fixtures in
  `spec/tests` are already updated (check.py, README, config-full.toml,
  all `*.expected`).

## 19. Linux

- Calligrus builds and runs on Linux x86_64 with the same features, config/state paths (§5)
  and headless `--script` mode as on Windows.
- Windows-only features compile out cleanly on Linux with a clear message:
  `--register`/`--unregister` ("Windows only"), dark title bar, console attach.
- Platform pieces with Linux versions (`src/platform_linux.cpp`): monitor work areas (window
  restore check), the Open dialog (zenity, falling back to "drop a file or pass a path"), the system
  font index for the font picker (the standard font directories), opening URLs/files with the OS
  (`xdg-open`), clipboard (GLFW).
- Headless script mode works under Xvfb (`xvfb-run`); software GL is fine for tests. When `DISPLAY` /
  `WAYLAND_DISPLAY` point at a real display (WSLg for example), headless runs should unset them and use
  `env -u DISPLAY -u WAYLAND_DISPLAY xvfb-run -a` so no window appears.
- The shared scripts in `spec/tests/script/` run on Linux and produce same-sized PNGs; pixel differences
  against Windows (font rasterisation) are expected and are why the visual suite keeps per-OS references.
- Build: `build.sh` (see `BUILDING.md`); `scripts/check-linux.sh` builds, runs every test and the visual suite.

## 20. Visual regression suite

- `spec/tests/visual/`: one `.mdscript` per feature area + the docs they open;
  `run.py` runs them with the built executable (Linux: under xvfb-run), compares each shot to
  `refs/<os>/<script>/<shot>.png` (tolerance: per-pixel channel diff > 24 counts as different; a shot
  fails if > 0.1 % of pixels differ, both configurable), and writes `report/index.html` (per shot:
  reference | new | diff heat-map). `--update` accepts current output as the new references; `--exe`,
  `--only <pattern>` to narrow. Exit code 0 = all within tolerance.
- Runs use a fixed config (`spec/tests/visual/config.toml`, built-in theme, embedded fonts, zoom 1) and
  a fixed empty state file, so a developer's own config never leaks into references.
- **Neutral paths.** `run.py` sets `CALLIGRUS_DISPLAY_ROOT=<root>`. With it set, every path the UI
  shows (the toolbar's file path, the recent-files popup, tab tooltips, "could not read" and "link target
  not found" messages) is shown relative to that folder with forward slashes when the file is below it
  (`spec/tests/visual/pages.md`), so references do not depend on where the repository is checked out.
  Without the variable nothing changes. It is a test switch, not a setting.

## 21. Look and layout details
The look of the window, fixed so that the visual suite is meaningful:
- **Vertical scrollbar of the document**: floating. Invisible while the view
  is still; it appears while the view scrolls (about 1 s after the last movement) or
  while the pointer is within 16 pt of the right edge of the window, then goes away
  again. Never a permanent bar. (A code block that is wider than the column keeps its own
  always-visible thin bar, spec 4.)
- **Code-block scrollbar**: 4 pt thick (6 pt while it is used), in 6 pt of extra room under
  the code (so a scrollable block is 6 pt taller than a short one); the thumb is the UI text
  colour at 60 % (idle), 80 % (pointer over the block), solid black/white (dark theme) while
  pointed at or dragged. The code is clipped at the frame's inner margin (11 pt: 10 + the
  1 pt stroke), not at the frame.
- **Copy button** (code blocks): shown while the pointer is over the block and for 1.5 s
  after copying ("Copied"). Drawn as a button on an opaque `code_bg` underlay: fill
  `ui.widget` (`ui.widget_hover` when pointed at), a 1 pt outline only when pointed at, label
  in the UI text colour.
- **Layout numbers** (points, scale 1): toolbar row 25 px tall including its
  bottom rule; document content starts 8 + 16 below the toolbar; panels' rules are whole
  pixels; combo box (theme picker) width = max(100, text + 26); toolbar separators take a
  6 pt slot with the line 3 pt in; "Open…" and the recent-files "▾" are 8 pt apart; the
  settings "⚙" is a plain button; the outline panel is 230 pt wide; the find field is 240 pt
  wide, 21 pt tall, with a 1 pt outline (`selection_text` colour while focused) and the cursor
  at the end of the text after Enter (no selection); the document scrolls 32 pt past the last
  block (64 pt of space inside the 8 pt panel margin); a heading jump (`]]`, outline
  click) puts the heading's text box 18 px below the top of the document view.
- **Text metrics**: line boxes split a font's height: total = whole-pixel
  size, descent = round(size x descender share), ascent = the rest; glyph advances are exact
  (unrounded) and each glyph is placed on a whole pixel. Tables and underlines/strike-through
  are drawn on whole pixel rows/columns; images have square corners; list item content is
  indented by `floor(marker width)` pt.
- **`sync-dump` y** counts from the top of the scroll area (the first block starts at
  y = 16); a source line after the last line of its target (a blank line) maps to
  the target's bottom edge (fraction clamped to 0..1, not to the last line).
- **Theme "Auto" on Linux**: the app asks GSettings: `org.gnome.desktop.interface
  color-scheme` (`prefer-dark` = dark, `prefer-light` = light), else the `gtk-theme` name
  (contains "dark"/"Dark" = dark, else light), and when `gsettings` cannot be run: dark.
  (`src/platform_linux.cpp`.) Windows uses the registry setting.
- **Long words wrap** (paragraphs, list items, table cells): a word or inline-code span without
  spaces that does not fit the line may break after one of `/ \ _ - . :` (the last such point that
  fits; none fits: per character), as browsers do for long paths. Only when it does not fit; normal
  wrapping is unchanged. Selection, find and copy keep the original characters (the pieces are
  separate runs; copying adds no space between them). Table columns may shrink to the widest piece.
  (Fixture `wrap-long`.)


## 22. Theme colours from the themes' own ports

Each built-in theme follows the colours of its Neovim port / VS Code theme.

- Optional `[doc]` keys `heading_1` .. `heading_6`: the colour of each heading level.
  Missing or invalid -> `heading` for levels 1-5, `weak` for level 6 (h6 has always been
  drawn weak). Never inherited from the base theme, so themes (and user themes) without
  them keep a single heading colour. Not in the settings UI; theme files only.
  The outline panel keeps using `heading`.
- Headings inside quotes and callouts keep their heading colour (only body text is dimmed
  to `weak`).
- Built-in themes with per-level colours, where the theme's official sources agree:
  catppuccin-mocha (red, peach, yellow, green, sapphire, lavender; `heading` is now blue
  #89b4fa, the port's @markup.heading, instead of mauve, which no upstream uses), nord
  (nord9, 15, 14, 13, 12, 11), solarized-dark/light (blue, violet, yellow, orange, red,
  magenta; light's yellow darkened to #ad8200 for 3:1 contrast). Also built in:
  dracula (dracula.nvim: foreground, pink, cyan, green, purple, orange; `heading` stays the VS Code
  purple for the outline) and tokyo-night (tokyonight.nvim: blue, yellow, green, teal, magenta,
  purple). All others (bearded, github, gruvbox, leuven, calligrus-*, imgui-dark): one colour.
- Inline code: bearded-vivid-black is purple #a95eff (every Bearded port), not greenAlt.
  solarized-light keeps magenta: the ports' cyan is only 2.6:1 on its code background.
- check.py: a new `h_levels` contrast column (minimum 3:1) covers the per-level colours;
  theme dumps (`theme-*.expected`) now have 32 lines, including `doc.heading_1..6`.
- **Bold, italic and quote colours**: optional `[doc]` keys `bold`, `italic`,
  `quote`, same rules as `heading_N` (never from the base theme; missing or invalid ->
  `text`, `text`, `weak`, i.e. exactly the old look). `bold`/`italic` colour a span only in
  plain body text: not in links, code, headings, table headers or callout titles (any run
  whose base is bold or not the body colour). A span that is both takes `bold`. Inside quotes
  and callouts the body colour is `quote`; `bold`/`italic` left at `text` follow it there,
  set ones keep their own colour. Only dracula sets them (its spec: bold orange #ffb86c,
  italic yellow #f1fa8c); the other themes keep the old look (their ports not yet checked
  for emphasis colours).
  check.py: column `emph` (minimum 3:1); theme dumps now have 35 lines.

## 23. Two-way sync: Ctrl+click in the preview moves the editor

Gesture: **Ctrl+click** (double-click keeps selecting words, and a plain click keeps its normal
meaning, so Ctrl+click adds the jump without changing anything else).

- **When**: only while an editor document is shown (`--editor` mode, the live view). Outside
  editor mode, or after a link left live mode, Ctrl+click does nothing new (no selection change,
  no link opening). Ctrl+click on a link sends `goto` instead of opening the link. Plain click,
  double/triple-click, shift+click and drags behave exactly as before.
- **Message** (app -> editor, on stdout, one JSON object + `\n`, flushed at once):
  `{"type":"goto","doc":"<the shown doc id>","line":N}`, N 1-based in the editor's buffer
  (the stripped front-matter line count added back, as for `cursor`). Nothing else is ever
  written to stdout in editor mode (logs go to stderr / the log file).
- **Reverse mapping** (the inverse of §15/§18's forward rule, from the same shared target list):
  1. Take the click's document y. Target = the deepest sync target (§18: top-level blocks,
     list items, quote/callout children, the callout title) whose [top, bottom) contains y;
     if y falls in a gap, the nearest target above (before the first target: the first).
  2. Line within the target:
     - code block: the fence line + 1 + the visual code line under y (exact; clamp to the
       code's lines; a click on the fence padding/label -> the first code line);
     - table: header row -> its first line; body row k (0-based) -> first + 2 + k
       (skipping the delimiter row);
     - display math, image, Mermaid, rule: the target's first line;
     - anything else (paragraph, heading, list item text, quote child...):
       first + floor((y - top) / height x (last - first + 1)), clamped to [first, last]
       (exact inverse of the forward formula, so goto -> cursor -> marker lands on the
       clicked spot).
  3. The app does not scroll on Ctrl+click. The editor answers with a normal `cursor`
     message and the marker/scroll follow the usual rules (middle-60 % means: no jump).
- **Script mode (§14)**: `click X Y ctrl` (modifier words after the coordinates: `ctrl`,
  `shift`, `alt`; also for `move`/`drag` if cheap). Messages the app would send are appended
  to `<out>/editor-out.jsonl` (one per line, exactly what stdout would get); `expect-editor
  JSON` fails the script unless the newest line written since the last `expect-editor` equals
  JSON (compared as parsed objects).
- **Resolved details:**
  - "Only while an editor document is shown" means shown AND still live: after `close` for that
    doc (text kept, no updates) Ctrl+click does nothing, there is nobody to tell.
  - Code: a click above the first code row (top padding, label) gives the first code line; below
    the last row (bottom padding, scrollbar room) the last code line. The fence line itself is never
    the answer. A block without fence lines (indented code) has as many source lines as rows, so
    there is no `+ 1`: the app uses the `+ 1` only when the block's line count exceeds its row count.
    Code inside a quote/callout is a target of its own and follows the same rules.
  - Table rows follow the rendered rows (a wrapped cell makes a tall row); header -> first line,
    body row k -> first + 2 + k, clamped to the table's last line.
  - Gaps: every block's measured height includes the space after it, so a click between blocks is
    inside the block above. Items and children inside a container: the last one starting at or above
    the click (the first one for a click above all of them).
  - Ctrl+click: any left button click with Ctrl held and the pointer moving at most 6 px between
    press and release. Press-time effects (selection, link follow) are
    suppressed; Ctrl+right/middle click and Shift/Alt+click do nothing new. In script mode the
    modifier is held by key events around the click.
  - Script: `expect-editor none` is also accepted: it fails if the app wrote anything since the
    last `expect-editor`. `expect-editor JSON` fails when nothing was written since the last
    check. A script that right-clicks leaves ImGui's context menu open and swallows the next
    click; the shared test avoids it.
  - Output: stdout is written only through the process's own stdout handle (the handle taken at
    start, `WriteFile`, one call per line). The app does not attach to a parent console for this, so a
    pipe given by the editor receives every line; `script_log` only attaches / redirects when
    stdout/stderr are not redirected.
  - Also in `spec/tests/script/editor-goto.mdscript`: the shared test; the app writes the
    28 lines to `editor-out.jsonl`.
- **Plugins**: read the app's stdout, split into lines (partial lines buffered), parse JSON,
  ignore unknown types/malformed lines. On `goto` for a doc they own: show that buffer (its
  window if one shows it, else the current window), set the cursor to (line, first
  non-blank), clamp to the buffer, open folds there. Option to turn it off: Neovim
  `setup({ goto = true })`, Emacs `calligrus-goto` (defcustom, t).
- **Focus back to the editor** : right before writing a `goto`, the app calls
  `AllowSetForegroundWindow(ASFW_ANY)` on Windows (otherwise Windows refuses another process
  taking the foreground and only flashes its taskbar button); no-op on Linux. Emacs then raises
  the frame of the window it used (`select-frame-set-input-focus`, graphical frames only),
  defcustom `calligrus-goto-focus` (t). Neovim does nothing: a terminal Neovim cannot raise its
  terminal window, so focus stays on the preview there.

## 24. Frame pacing and idle redraws

- Pacing (`FramePacer` in main.cpp): if 10 frames in a row come under 4 ms apart, vsync is not
  working (remote desktop, software OpenGL); from then on (sticky) frames are kept at least
  1/60 s apart by sleeping. With working vsync this never triggers (a 144 Hz screen stays 144).
  Not in script mode (headless runs draw as fast as they can).
- The floating scrollbar (§21) does not force a redraw every frame while visible: the idle wait wakes
  once when the bar must hide (`bar_wake`).
- Idle cost: the app wakes every 0.5 s (live reload / config writer); under 5 % of a core.

## 25. Org files

Org documents render with the same block model, renderer, outline, find, selection, sync and
themes as Markdown. Only the parser is new: a line-based Org parser in each app that produces
the existing `Block`s plus the same line targets (§15/§18/§23 Kinds). Not a translation to
Markdown text (line numbers must stay exact). Unsupported syntax shows as plain text.

- **Which parser**: a file whose extension is `.org` (any case); an editor `open` with the new
  optional field `"format": "org"` (values `"markdown"` | `"org"`; missing: by `path`
  extension; unknown: markdown). The format of an editor doc is fixed at `open`. Open dialog
  and "Open with" registration accept `.org`. Reload/watch as for Markdown.
- **Hidden lines** (belong to no target, like blank lines): `#+KEYWORD: ...` lines other than
  `#+TITLE` (AUTHOR, OPTIONS, STARTUP, CAPTION, NAME, RESULTS, TBLFM, ATTR_*, ...); comment
  lines (`#` then space or end of line); `#+BEGIN_COMMENT`..`#+END_COMMENT`; drawers
  (`:PROPERTIES:` / any `:NAME:` line .. `:END:`); planning lines (`SCHEDULED:`, `DEADLINE:`,
  `CLOSED:`) right after a headline. Keywords and block names are case-insensitive.
- **Title**: `#+TITLE: x` -> a level-1 heading at that line.
- **Headlines**: `*`..`*` + space at column 0 -> heading, level = number of stars (max 6). A
  leading TODO keyword (`TODO`, `DONE`, `NEXT`, `WAIT`, `CANCELLED`) stays as a bold span; a
  priority `[#A]` stays as text; trailing tags (`:a:b:` after whitespace) are dropped.
- **Paragraphs**: consecutive non-blank lines not starting another element; joined with
  spaces like Markdown soft breaks; `\` at line end = hard break.
- **Inline** (within one paragraph line; Org's rules: the opening marker follows start of
  line, whitespace or one of `-('"{`, the closing marker is followed by end of line,
  whitespace or one of `-.,;:!?')}"\`, the body neither starts nor ends with whitespace):
  `*bold*`, `/italic/`, `+strike+`, `=verbatim=` and `~code~` -> inline code, `_underline_` ->
  plain text (markers dropped; no underline style). Nested emphasis allowed (`*/x/*`).
  Links: `[[target][desc]]`, `[[target]]`, bare `http(s)://` URLs. `[[#id]]`,
  `[[*Heading text]]` -> link to that heading's slug (`#slug`, same navigation as Markdown).
  `file:` prefix stripped. A link without description whose target ends in
  png/jpg/jpeg/gif/svg/webp/bmp -> image; a paragraph of only images -> `Images`.
  Math: `$x$` (same rules as Markdown), `\(x\)` inline; `\[x\]` and `$$x$$` display (may span
  lines); a `\begin{env}`..`\end{env}` at column 0 (after indentation) -> display `Math` with
  the whole environment as its TeX. Footnotes: `[fn:name]` reference; a line starting
  `[fn:name] text` (column 0) is the definition; numbered by first reference, rendered as the
  Markdown footnotes.
- **Lists**: `- `, `+ `, and indented `* ` bullets; `1.` / `1)` ordered (start from the first
  number); nesting by indentation; an item continues over more-indented lines. Checkboxes
  `[ ]`, `[X]`, `[-]` -> like Markdown task items (`[-]` as unchecked). Description items
  `- term :: text` -> `term` bold, then ` — `, then text.
- **Blocks** (`#+BEGIN_X` .. `#+END_X`, case-insensitive, may be indented): `SRC lang` -> code
  block with that language (`mermaid` -> diagram, as in Markdown); `EXAMPLE` -> code block, no
  language; `QUOTE` -> quote; `VERSE`, `CENTER` -> their content as paragraphs;
  `NOTE`/`TIP`/`IMPORTANT`/`WARNING`/`CAUTION` -> callout of that kind; any other name -> its
  content as normal blocks. `#+BEGIN_EXPORT`: hidden. Lines starting `: ` (or a lone `:`)
  -> code block, no language (fixed-width).
- **Tables**: lines starting `|` (after indentation). Rule lines `|-...` separate the header:
  rows above the first rule line are the header; no rule line -> no header row (renderers
  draw a table with an empty header without the header band). Rows with only `<l>`/`<c>`/
  `<r>`/`<N>` cookies are hidden; the first such cookie per column sets its alignment.
- **Rule**: a line of 5 or more `-` only -> `Rule`.
- **Line targets**: each block's source lines as in Markdown (headline = its line; a block
  `#+BEGIN..#+END` = all its lines; table header row/body rows as §23). Cursor on a hidden line
  = nearest target above, as for blank lines.
- **Plugins**: Emacs sends `"format":"org"` for `org-mode` buffers (and lists `org-mode` in
  `calligrus-modes`); Neovim does the same for `filetype=org`.
- **Tests**: `spec/tests/visual/org.org` (+ `org.mdscript`, shots `top`, `bottom`) in the
  visual suite; `spec/tests/script/org.mdscript` writes `org/sync.tsv` .

## 26. Slides mode

A presentation view of the shown document (Markdown or Org; normal or editor doc). Same
renderer: a slide is a range of top-level blocks drawn alone, scaled to the window.

- **On/off**: `F5` toggles; toolbar button "Slides"; CLI flag `--slides` starts in it; `Esc`
  leaves (when find is closed). Leaving scrolls to the current slide's first block. Entering
  starts at the slide containing the top of the current view. The toolbar, outline panel and
  scrollbar are hidden in slides mode. `F11` toggles fullscreen (also outside slides).
- **Splitting** (top-level blocks only): if the document has any top-level `Rule`, slides are
  the runs between rules (rules not drawn; empty runs skipped). Otherwise a slide starts at
  every top-level heading whose level is the smallest heading level present; blocks before the
  first such heading form a slide of their own if any. No headings and no rules: one slide.
- **Layout**: the slide is laid out at a reading width of 700 units with the normal fonts,
  then drawn at zoom z = min(b, H / (h + 80)) with b = min(W / 760, 3), W, H the window size
  and h the slide's laid-out height (both in units at zoom 1), but never below 0.6 * b; centred
  horizontally and vertically (if it still doesn't fit at the floor: top-aligned and the mouse
  wheel scrolls within the slide). Background: doc `bg`. User zoom (Ctrl +/-) is ignored in
  slides mode and kept for normal mode.
- **Counter**: `n / N` at the bottom-right corner, 14 px (scaled by window, not zoom), doc
  `weak`, 16 px from both edges.
- **Keys**: next = Right, Down, Space, PageDown, `l`, `j`, `n`, Enter; previous = Left, Up,
  Backspace, PageUp, `h`, `k`, `p`; Home/`g` first; End/`G` last; `t` clock, `N` notes, `o`
  overview, `s` presenter window (below). Mouse clicks keep their usual
  meaning (links, selection, Ctrl+click goto). Find (Ctrl+F) works and jumps to the slide of
  the current match.
- **Editor**: while in slides mode a `cursor` message shows the slide containing that line (no
  marker drawn); `text` updates re-split and keep the current slide index (clamped).
- **Script mode**: `keys f5` etc. as usual; visual suite `slides.mdscript` on `slides.md`.
- **Reload**: re-split, keep the index (clamped).
- **Pauses**: a line holding only `<!-- pause -->` (Markdown,
  indented by at most 3 spaces) or `# pause` (Org comment line, not indented), outside fenced
  blocks (``` / ~~~; Org `#+begin_` ... `#+end_`), splits a slide into steps. Pandoc's `. . .`
  works too in Markdown when it is a paragraph of its own (blank line or file edge on both
  sides); that paragraph is then not shown, in either mode. The step starts at
  the next top-level block; a pause with no block of the same slide before or after it is
  ignored (so pauses inside lists or quotes do nothing). Step k shows the slide's blocks before
  its (k+1)-th pause; later blocks keep their space (the slide is laid out and zoomed whole,
  nothing moves when a step appears) but are not drawn. Next reveals a step before going to the
  next slide; previous hides one, and going back onto a slide shows all of its steps. Home/End
  and other jumps to a slide show its first step. The editor `cursor` shows the steps up to
  the cursor's block; a find match in a hidden block reveals the steps up to it. The counter is
  unchanged (`n / N` slides). Normal mode ignores pauses (comments are not drawn).
  Visual suite: `pauses.mdscript`.
- **More presenterm comments**, found like pauses (Org: `# name` lines):
  - `<!-- end_slide -->`: when a document has any, the slides are the runs between them (a
    slide ends before the first block below each one) and `---` rules are drawn as rules.
  - `<!-- incremental_lists: true -->` / `false`: top-level lists after `true` on the same slide
    (until `false`; each slide starts with it off) show one more item per step: the first item
    with what comes before, then a step per item. Later blocks wait for the last item. Like a
    column directive, a switch applies from the next block of a slide below it. Hidden items
    keep their space. The cursor / find reveal a list whole.
  - `<!-- jump_to_middle -->`: accepted, no effect: short slides are already centred
    vertically (the whole slide, not just what follows the comment).
- **Progress bar**: a 3 px bar (UI accent colour) along the bottom edge of the window in slides
  mode, `(n / N)` of the window width. Visual suite: `incremental.mdscript`.
- **Text directives** (presenterm), found like pauses; each applies from the next block of a
  slide below it, and each slide starts plain:
  - `<!-- new_lines: n -->` (also `newlines:`; 1 to 50) / `<!-- newline -->` (`new_line`): n
    empty body lines (1.55 x body size each) above the next block. Full-width blocks only (not
    inside a column). The space is part of the slide (counted when centring it).
  - `<!-- alignment: left|center|right -->`: top-level paragraphs and headings from here to the
    slide's end are lined up that way (h1/h2 rules stay full width; other blocks unchanged).
  - `<!-- font_size: n -->` (1 to 7): top-level blocks from here to the slide's end draw their
    text n times the body size (everything derived from it scales: headings, gaps, code...).
  - Malformed values are ignored. Visual suite: `presenter.mdscript`.
- **Speaker notes**: `<!-- speaker_note: text -->` on one line, or presenterm's long form
  (`<!-- speaker_note: |`, the text lines, a `-->` line; lines trimmed); Org `#+begin_notes` ..
  `#+end_notes` (never drawn, in either mode). Outside fences / other blocks. A note belongs to
  the slide of the block at or above it (just below a slide-ending rule: the next slide);
  several on a slide are joined by a blank line.
  - `N` (slides mode) shows / hides the current slide's notes on a panel along the bottom of the
    window (UI panel colour, border, 15 px text, 12 px padding, 40 px above the bottom).
  - Editor documents: on every slide change, and when the shown slide's notes change, the app
    sends `{"type":"notes","doc":..,"slide":n,"total":N,"notes":"..","line":a,"end":b}`
    (1-based, notes maybe ""; lines: see "Editor follows slides"). The Emacs plugin (`calligrus-notes`, default on) shows them in `*calligrus notes*`, popped
    up (not selected) the first time a slide has notes. Script test: `editor-notes.mdscript`.
- **Presenter clock**: `t` (slides mode) shows / hides the time since slides mode was entered,
  `m:ss` (`h:mm:ss` from an hour), bottom left, same font and colour as the counter. F5 restarts it.
- **Columns**, slides mode only. Directive lines, recognised
  like pauses (Markdown: the whole line is the comment, indented by at most 3 spaces, outside
  fences; Org: a `# ` comment line, not indented, outside `#+begin_` blocks):
  - `<!-- column_layout: [a, b, ...] -->` / `# column_layout: [a, b, ...]`: starts a layout of
    as many columns as weights (positive integers, 2 to 4 of them; anything else: the line is
    ignored). Spaces inside the brackets are optional.
  - `<!-- column: n -->` / `# column: n`: the following top-level blocks go into column n
    (0-based) of the current layout. Out of range, or no layout open: ignored.
  - `<!-- reset_layout -->` / `# reset_layout`: closes the layout; the following blocks are
    full width again. The end of a slide also closes it.
  - Blocks after `column_layout` but before the first `column:` are full width (above the
    columns). A directive applies from the next top-level block on (like pauses): a block
    belongs to the directive state in force at its first line. A rule between slides is in no
    slide, so directives just above `---` apply to the next slide's first block. Malformed
    `column_layout` lines are ignored (an open layout stays open); `column: n` out of range or
    without a layout is ignored (the current column stays). Ctrl+click in the gap between
    columns goes to the nearer column.
  - Layout: within the 700-unit slide column, a layout is a row: column i is
    (700 - 32 (n - 1)) * w_i / sum(w) wide, columns 32 units apart. Each column stacks its
    blocks from the row's top (blocks laid out at the column's width, normal top/bottom block
    spacing); the row is as tall as its tallest column; what follows starts below it. Several
    layouts per slide are fine. The slide height used for the zoom includes the rows.
  - Pauses work inside layouts: hiding is by block index (blocks at or after the step's block
    are invisible but keep their space), not by y.
  - Ctrl+click, the editor cursor (slide + step), find highlights and selection work on blocks
    in columns. Selection/copy order is block (source) order.
  - Normal mode ignores the directives entirely (one column, nothing changes).
  - Visual suite: `columns.mdscript` on `columns.md`.
- **Background pictures**, slides mode only: a top-level paragraph of
  only pictures whose alt texts are all `bg ...` is not drawn in the text; the slide's first
  such picture is its background. Words after `bg`, in any order: `left` / `right` (a strip on
  that side, 50% wide) or `left:NN%` / `right:NN%` (clamped to 10..90%); `contain` / `fit` (the
  whole picture, centred, with bars) or `cover` (default: fills its area, cropped in the
  middle); Marp filters (below). Any other word: ignored. `bg` alone: behind the whole
  window. A strip splits the slide like a two-column layout of weights [100 - p, p] (`right`) or
  [p, 100 - p] (`left`): the slide's full-width blocks go in the text column; the picture covers
  the window from the middle of the gap to that window edge, full height. Remote pictures are
  not fetched (no background). Normal mode: an ordinary picture. Visual suite:
  `backgrounds.mdscript`.
  - **Several pictures**: every `bg` picture of a slide is drawn. The first one
    that names a side sets the strip (later sides are ignored); without one they share the
    whole window. The area is split into equal cells side by side, or stacked top to bottom if
    any of them says `vertical`; each picture fills (or fits) its own cell.
  - **Filters**: `blur`, `brightness`, `contrast`, `grayscale`,
    `hue-rotate`, `invert`, `opacity`, `saturate`, `sepia`, as `name` or `name:value`, applied
    in the order written. Without a value: blur 10px, brightness 1.5, contrast 2, grayscale /
    invert / sepia 1, opacity 0.5, saturate 2, hue-rotate 180deg. Amounts may be percentages
    (`50%` = 0.5); blur takes an optional `px`, hue-rotate an optional `deg` (and may be
    negative). A negative or unreadable value drops that filter; grayscale, invert, sepia and
    opacity above 1 count as 1. Colour filters use the CSS filter matrices on the
    8-bit sRGB values (rounded); opacity scales alpha. Blur is three passes of an integer box
    blur (the sizes from the standard Gaussian approximation) with sigma = px * picture width /
    1280, at most 200 picture pixels. Applied once when the picture is loaded (cached by path
    and filters). Visual suite: `bg-filters.mdscript`.
- **Overview**: `o` (slides mode) shows every slide (all steps) as a picture in a grid filling
  the window: in the main window, or in the presenter window while that is open (the audience
  keeps seeing the current slide until one is picked; keys work from either window, clicks in
  the window showing the grid). Cells shaped like the main (audience) window, 24 px apart, slide numbers (13 px, doc `weak`) under
  each; the column count is the one that makes cells biggest. Each picture is the slide drawn
  as slides mode would draw it in this window at its own zoom, shrunk into its cell (not
  clickable inside). The picked cell has a 3 px accent outline (others 1 px border colour).
  Arrows / `hjkl` move (up/down by a row, stopping at the edges), Home/`g`, End/`G`; Enter,
  Space or `o` show the picked slide (all of it); a click shows the clicked one; Esc goes back
  to the slide that was on. In the main window, counter, bar and notes are hidden meanwhile.
  Visual suite: `overview.mdscript`, `speaker-overview.mdscript`.
  - **Long decks**: cells are never narrower than 160 px. When the best grid's
    cells would be, the grid has as many columns of cells at least 160 px wide as fit (at least
    one), starts 24 px below the top, and scrolls: the mouse wheel moves it 40 px a notch (not
    with Ctrl), and whenever the pick moves (and when `o` opens it) it scrolls just enough to
    show the picked row with 24 px around it. Visual suite: `overview-long.mdscript`.
- **Presenter window**: `s` (slides mode) opens / closes a second native window (title "Calligrus
  presenter", 1000 x 640 px): top bar 56 px with "Slide n / N" (and "step s / S" when the slide
  has steps; 18 px) on the left, the wall clock (local time `HH:MM`, 18 px, `weak`)
  in the middle and the clock (time since F5, 28 px) on the right; below, the
  current slide as the audience sees it (current step) on the left, as big as fits in 62% of
  the width; on the right the next step's picture (the same slide one step on, or the next
  slide's first step; "End of the slides" after the last) at most 40% of the height, and the
  speaker notes (20 px, wrapped, "(no notes)" when none) below it. Labels "Now" / "Next" /
  "Notes" (13 px, `weak`) above each box; 16 px padding; UI panel background. Pictures have the
  main window's shape. Keys typed in it act as in the main window; closing it or leaving
  slides mode turns it off. Script mode has no second window: the view replaces the main
  window's content, the clock shows 0:00 and the wall clock 12:00. Visual suite: `speaker.mdscript`.
- **Export**: `calligrus --export OUT FILE [--size WxH] [--scale S]` (headless; defaults
  1280x720, scale 1.5) draws every slide, all steps shown, as slides mode does in a window of
  that size, and writes `OUT` as a PDF (if it ends in `.pdf`: one page per slide, the picture
  RGB/Flate, page size = window size at 96 px per inch) or into the folder `OUT` as
  `slide-001.png`... Uses the normal config (theme, fonts). `--base-dir DIR` (also for the pages
  export, §29): relative pictures and links resolve from DIR instead of FILE's folder. Exit codes: 0, 1 (cannot
  read / write), 2 (bad command line). Script command `slide N` shows slide N (1-based, all
  steps), switching slides mode on if needed.
- **Editor follows slides**: the `notes` message also carries the slide's source lines,
  `"line"` (its first block's first line) and `"end"` (the line before the next slide's first
  block, or the last line). The Emacs plugin (`calligrus-follow-slides`, default on) moves point
  to `line` when the slide changes unless point is already within `line`..`end`; it does not
  echo a `cursor` message for that move.
- **Pictures reload**: a picture whose file changed (or appeared) on disk is loaded again,
  checked once a second while pictures are shown (both modes).

## 27. Fonts, column width, copy LaTeX, zoom

- **Cache invalidation rule** (code ligatures, shaped prose, drawn glyph pictures): everything that depends on the loaded
  fonts is keyed by, or dropped with, the font generation. `shaping::set_font` and `prose_shaping::set_chains` (both called
  by every `load_fonts`) clear their caches and bump `generation()`; `shaped_draw` compares the two generations on every
  glyph and drops its FreeType faces, glyph pictures and textures when either changed; `text_capture` does the same for
  the PDF's fonts. A new cache of fonts' output must do likewise, or be cleared in `load_fonts`; the document's layout
  caches are reset by main.cpp right after an atlas rebuild (`cache.key = 0`).
- **Glyphs come from the right size** (ImGui's FreeType loader): ImGui keeps one "baked" font per size and its FreeType
  loader skips selecting a baked's `FT_Size` when it believes that baked is the one selected last, remembering it by
  address. That memory can go stale (bakeds are discarded and their pool moves); glyphs were then loaded at another
  baked's size, and code came out with wrong advances and overlapping or wrong glyphs (seen with the system-font search
  on, which looks characters up at 16 px between frames; fixture `code-sysfallback`). `load_glyph_with_curve` (fonts.cpp)
  therefore selects the baked's `FT_Size` itself before every glyph. Dev switch `CALLIGRUS_FT_LEGACY=1` (Android: file
  `ft_legacy` in the app home) leaves the loader's own check alone, to see the old behaviour.
- **Font fallback order**: prose / UI: the face,
  Ubuntu-Light, Noto Emoji, emoji-icon-font, Cascadia Code (box drawing), Hack (symbols Cascadia lacks: ⇒ ⇔ ∀ ∃), Noto Sans Symbols 2 (✗ ✘ ☒..., below); code: the face,
  Cascadia Code (when the face is a custom one), Hack, Ubuntu-Light, the two emoji fonts, Noto Sans Symbols 2. A character no font has is drawn as
  `◻` (U+25FB). Visual suite: `symbols.mdscript`.
  **Noto Sans Symbols 2**: SIL OFL 1.1, unmodified static Regular (1.23 MB, 2955
  codepoints, 1688 of them in none of the other bundled fonts: ✗ ✘ ☒, many more dingbats and
  symbol blocks). It sits right after Hack in the prose chain and at the end of the
  code chain, before the installed fonts of §30 (the §30 check counts it) and Latin Modern Math.
  Fixture `symbols`. It is TrueType, so the PDF subsetter embeds its glyphs as real text (§33).
  **Last of all**: Latin Modern Math (the TeX engine's font, already embedded) ends every
  chain, after Hack and after the installed fonts of §30, for maths symbols nothing else has (⊨ ⋈ ⟹ ♭
  ⊻ ∖ ⟺ ⪯...). It has CFF outlines (ImGui/FreeType draws them fine), which the PDF subsetter
  cannot embed (§33): such glyphs stay in the picture layer of an exported PDF. The §30 "does any font
  have this character" check ignores it, so an installed font is still picked first.
- **Column width**: config `max_width` (points at zoom 1.0, 400 .. 2400, default 860; out of
  range: a config error, the default kept) is the widest reading column; Settings has a "Column width" slider (step 20).
  Slides mode keeps its own 700-unit column. Visual suite: `max-width.mdscript`.
- **Link targets**: hovering a link shows its URL in a tooltip.
- **Copy LaTeX**: right-clicking a formula (inline or display) adds "Copy LaTeX" at the top of
  the context menu; it copies the formula's source written as `$..$` (inline) or `$$..$$`
  (display), whatever delimiters the document used. A toast says `Copied <source>` (shortened to 60 characters
  with `…`).
- **Toast**: 14 pt UI text on a UI-panel-coloured pill (1 px border, radius 6, padding 10 x 6),
  centred, its bottom 24 pt above the bottom of the view, shown for 1.6 s. It does not count as
  an animation (script mode does not wait for it).
- **Click to zoom**: a click on a picture (one that is not a link) or a Mermaid diagram shows it
  over everything: the window dimmed (black at 80%), the picture centred on a card of the page's
  background (radius 6, 12 pt padding), as big as fits inside a 40 pt margin (less a 32 pt
  caption line when the picture has alt text), at most 4x its natural size (diagrams 3x, and
  drawn again at that size in half steps so they stay sharp). The alt text (15 pt, light grey)
  is centred under the card. A click anywhere or Esc closes it; meanwhile it takes every key.
  Pictures and diagrams show the hand cursor. Works in both modes. Visual suite:
  `zoom-copy.mdscript`.
- **Export button**: toolbar button "PDF" right after "Slides" (enabled when a file
  or an editor document is shown and no export is running; tooltip "Export the slides as a PDF (e in slides mode)");
  `e` in slides mode does the same. A save dialog (PDF filter, starts at `<doc stem>.pdf` in the
  document's folder; asks before overwriting) then the app runs its own exe as a child process
  `--export OUT DOC` (no console window, stdin empty, stdout + stderr captured, on a background
  thread), so the PDF is exactly the CLI's (1280x720, scale 1.5, the current config's theme and
  fonts). **In editor mode** (the text arrives over the protocol, §15/§17; the `open` message's
  `path` and `format` say where it lives and whether it is Org) the button and `e` export exactly
  what the preview shows, unsaved edits included: the current text is written to a temporary file
  in the system temp folder (`.md` or `.org` by the format), the same child export runs on it with
  `--base-dir` set to the real document's folder (the folder of `path`, else the working folder),
  so relative pictures resolve as in the preview, and the temp file is deleted when the child has
  finished, whether it succeeded or not. The save dialog starts in the real document's folder with
  `<stem of path>.pdf`; a buffer without a path (never saved) starts in the working folder with
  `untitled.pdf`. Both Slides... and Pages... work; the toasts are unchanged. (A file that a link
  led to is a normal file: exported as such.) While it runs the
  toast says "Exporting to NAME…" (refreshed); then "Exported N slides to NAME" (N from the
  child's `exported N slides to ...` line; "1 slide") or "Export failed: MSG" (MSG from its
  `export: ...` line, else its last line, else "no answer from the export"; "cannot start the
  export" if it could not be launched). Script mode: the dialog is suppressed (log line); the
  script command `export FILE` (§14) starts the export without the dialog and waits for it
  (fails after 120 s; in editor mode it exports the editor's text, as above). Visual suite:
  `export.mdscript`, `export-editor.mdscript` (an `open` with unsaved text, then `export-pages` and
  `export`, with `# expect-pdf-text:` checks that the unsaved text is in the PDFs). Unit tests:
  `script::export_args` for `--base-dir`, and the temp-file / base-folder / dialog-name helpers
  (`editor_temp_file`, `editor_base_dir`, `editor_pdf_name`).

## 28. Marks

Vim keys only (`vim_keys` on), normal mode (not slides mode).

- **Keys**: `m` + letter sets a mark, `'` + letter goes to it, `''` = back in the history (same
  as `H`), `*` / `#` find (below). A non-letter after `m` / `'` drops the prefix and that key is
  handled on its own; the prefixes time out like `g`.
- **A mark** = the top-level block at the top of the view and how far down into it (0..1, kept to
  3 decimals) + the block's key: its searchable text (find's groups: spans, code lines, table
  cells, a callout's title; inline maths as U+FFFC; mermaid, pictures, rules and display maths
  have none) with runs of white space as one space, cut to 40 characters (a cut may leave a
  trailing space). The key is written to `state.toml` (test in `calligrus-model-test`).
- **Scope**: `a`-`z` per file, `A`-`Z` across files (setting one moves it; going to it opens its
  file). The built-in demo has no file: toast "Marks need a file (this is the built-in demo)".
- **Going to a mark**: same file: the current place goes into the history; another file: opened
  like a link (history too). The block is found again: the same index if its key is unchanged,
  else the nearest block with that key, else the mark is dropped with the toast "Mark X is gone:
  its text changed". A mark on a block without text only matches at its own index on a block
  that still has no text. A moved mark is saved at its new index. Unset: "Mark X is not set";
  a global mark whose file is missing: "Mark X: PATH is gone". The view is put there at once (no
  animation).
- **`state.toml`**: `[[mark]]` tables after the `[[scroll]]` ones, newest first, at most 300:
  `name = 'a'`, `path = '...'` (absolute, like scroll paths), `block = N`, `frac = 0.250` (3
  decimals), `text = "..."` (TOML basic string, escapes as in config strings). Bad entries are
  skipped one by one; a second mark with the same name (and file, for `a`-`z`) is ignored.
  Written only by the normal app (not in script mode).
- **Margin**: the shown file's marks (local, and global ones set in it) whose block is found,
  as their letters (several at one spot: `a`-`z` then `A`-`Z`, e.g. "aB"), in the document's
  `weak` colour, 0.75 x body size, right-aligned 10 pt left of the text column, the top at the
  marked spot; not in slides mode.
- **`*` / `#`**: search for the selected text (its first non-empty line, trimmed), else the word
  under the mouse (the painted word, trimmed of leading / trailing characters that are not
  letters, digits or `_`); nothing: toast "Select some text or point at a word to search for
  it". The find bar opens with it (the keyboard stays on the document, so `n` / `N` work),
  smartcase as usual; the current match is the first one from the word's block on (`*`) or the
  last one up to it (`#`).
- Visual suite: `marks.mdscript` (`marks.md`, `marks-other.md`).

## 29. Document export: pages

Any document (Markdown or Org) as a PDF of real pages, in the normal reading layout (not slides).
Same machinery as the slides export (§26 Export, §27 Export button): the app draws headlessly and
writes one picture per page into the PDF.

- **Command line**: `calligrus --export OUT.pdf --pages FILE [--paper a4|letter] [--theme NAME]
  [--body-size PT] [--base-dir DIR] [--scale S]`. Defaults: A4, the config's `light_theme` (paper is light; `--theme`
  overrides, any theme name), the config's `body_size` converted to points (`body_size` is a size in logical
  pixels on screen; exports convert at 0.75 pt per px, the CSS convention: 16 px = 12 pt, 13 px = 10 pt;
  rounded to 0.5 pt, kept in 8..64) (`--body-size`: a number of real points, 8 to 64,
  else error, exit 2; fractions allowed, e.g. 17.5; no conversion), `--base-dir`: where relative pictures
  and links resolve from (default: FILE's folder), scale 2 (sharper than slides: it may be printed). Without `--pages`, `--export` is
  the slides export as before. `--pages` is a flag (FILE is the one argument that is not an
  option's value: `--export OUT --pages FILE` = `--export OUT FILE --pages`). `--pages` with an
  OUT that is not `.pdf`, or an unknown `--paper`: error, exit 2; an unknown `--theme` name:
  `export: unknown theme NAME`, exit 1. Exit codes as §26. On success it prints `exported N pages to OUT` (the app's toast reads this).
- **Page geometry** (points, 1/72 inch): A4 595.3 x 841.9, Letter 612 x 792; margins 54 (3/4 inch)
  on every side. The text column is the page width less the margins (A4: 487.3), laid out exactly
  as normal mode lays out a column that wide (ignores `max_width` and the zoom; the body size is
  `--body-size`, else the config's; everything that scales with it, headings, code, gaps, maths,
  follows as on screen). Pictures wider than the column shrink as on screen. The page number `n / N` sits
  centred in the bottom margin, 9 pt, doc `weak` colour, the bottom of its text box 25 pt above
  the page edge (its baseline is then about 27 pt above it).
  Page background: the theme's doc `bg`, edge to edge.
- **Where a page may end** (in document coordinates, y down; all apps compute the same set):
  - between two top-level blocks: the top of block i (i > 0);
  - inside a paragraph, heading, footnote, list item or quote child: between two visual lines
    (the bottom of a line box, as text selection records them);
  - inside a code block: between two code lines; inside a table: between two rows (never
    between the lines of one cell);
  - never inside a picture row, a display formula, a Mermaid diagram, a horizontal rule;
  - never right after a heading (the place below a heading's last line, which would leave it
    alone at the bottom of a page), unless the heading is the last block;
  - not within the first 2 lines or the last 2 lines of a paragraph / list item / code block of
    4+ lines (no lone first or last line: "widows and orphans"; table rows are exempt).
- **Filling a page**: the page's text area is H = page height - 2 x margin. Starting at y0 (0 for
  the first page), the page ends at the largest allowed place in (y0, y0 + H]. If that place is
  above y0 + H/4 (one unbreakable thing — a tall picture, diagram, table row or code without
  allowed places — would leave the page three-quarters empty), the page ends at the largest
  allowed place in (y0, y0 + H] ignoring the heading and widow/orphan rules; if there is still
  none, at y0 + H (the item is cut). The next page starts where this one ended. What lies below
  the end on a page is not drawn there (it is covered with the background).
- **Drawing**: the export window is the page size (points) at `scale` pixels per point, so
  each page's picture is e.g. 1191 x 1684 px for A4 at scale 2; the PDF page is the paper size.
  For each page the document is scrolled so y0 is at the top of the text area; everything above
  the text area (the top margin: it would show the previous page's last lines) and everything below
  the page's end (the bottom margin included) is painted over with the page background; then the
  page number is drawn. The measuring layout starts the first block at y = 0 (no 16 pt margin
  above it) and the document's height is the bottom of its last block. Links in the PDF are not clickable (pictures only, as the slides export).
- **Export button**: the toolbar's **PDF** button opens a small menu: "Slides…" and "Pages…"
  (each then the save dialog, `<doc>.pdf`), and under them a checkbox "Use current theme"
  (default off, kept for the session only, not saved) and a paper choice for Pages, two radio
  items "A4" (default) / "Letter" (also session only), passed as `--paper a4|letter` to the child
  export; `e` in slides mode stays the slides export.
  **Text size rule** (CSS px to pt): Pages passes `--body-size` = config `body_size` (logical px) x the
  current zoom (Ctrl +/-) x 0.75, rounded to 0.5 pt and kept in 8..64 (`pages::export_body_size`): the
  PDF text is as big as the screen text at 100 % display scaling, as a browser prints. The CLI without
  `--body-size` uses `export_body_size(body_size, 1)`; `--body-size PT` is real points.
  **Editor mode**: Pages... (and Slides...) export the editor's text, see §27 Export button.
  **Theme rule**: box off, no `--theme` (the config's `light_theme`); box on, `--theme NAME` with
  the theme on screen now (for `theme = "auto"`: the resolved light or dark theme's name). Slides
  export is unchanged: it takes neither (it draws in the config's theme and its own slide zoom).
  Script `export-pages` goes through the same function (so it sees the zoom, and the box, off). The
  toast says "Exported N pages to NAME". Script command `export-pages FILE` = "Pages…" without
  the dialog (waits like `export`). (Calligrus runs the CLI export as a script with two internal
  commands, `pages-begin` and `page-shot`; they are not for users' scripts.)
- **Tests**: unit tests of the break rules on made-up measurements (pure function: blocks with
  their kind, top, height, line boxes / rows → page ends), a visual
  fixture `pages.mdscript` that exports a three-to-four-page sample (headings, a long paragraph,
  a long code block, a table, a picture, a display formula) and shoots the toast; a second export in it after two `keys ctrl+plus` (bigger text: 5 pages, not 4), and a script
  check that the PDF has the expected page count (the script's line
  `# expect-pdf-pages: FILE N`, checked by `run.py`).

## 30. System fonts for missing characters

A document in a script no bundled font has (Gujarati, Devanagari, Thai, CJK...) showed `◻`. Now
installed system fonts fill the gaps, after the bundled chain.

- **When**: after a document (or editor text) is loaded, the app collects the characters of its
  text (all blocks, prose and code; not maths) that no font of the current chains has (bundled
  fonts + configured `[fonts]` faces; each font's coverage read once from its cmap). None
  missing: nothing happens (no system font scan at all).
- **Choosing** (in the background, on the font index of §8a, scanning the system font folders if
  that hasn't happened yet): candidates are regular-style faces. Order: first these families,
  in this order, if installed — Windows: Nirmala UI, Segoe UI Symbol, Segoe UI Historic, Ebrima,
  Gadugi, Leelawadee UI, Microsoft YaHei, Yu Gothic, Malgun Gothic, Microsoft JhengHei, Cambria
  Math; Linux: Noto Sans (any family starting "Noto Sans", by name), DejaVu Sans, FreeSans —
  then every other face, by family name. Greedy: take the candidate that covers the most still-
  missing characters (ties: the earlier one), drop those characters, repeat; at most 4 fonts;
  stop when nothing is missing or no candidate covers any. Colour emoji fonts are skipped (the
  bundled emoji fonts stay in charge of emoji).
- **Using them**: the chosen faces are appended to the prose/UI and code chains, after Hack (so
  before the missing-glyph `◻`), sized like the other fallbacks (em ratio, §8). Fonts are only
  ever added during a session (opening another document may add more, up to 8 in total);
  nothing is saved. The font atlas is rebuilt once when they arrive; the text re-lays out.
  `FontFiles::system` is merged last into every face by `load_fonts`, and the atlas is rebuilt
  the way a `[fonts]` change does it. The check runs again whenever the document or the chains
  change; the same fonts are never added twice, so it ends.
- **Fonts and caches** (§27 rule): when the atlas is rebuilt for new system fonts, the shaping and glyph caches are
  dropped with it (their generations change); the system-font search itself only reads the fonts' coverage and, to find
  out what is missing, looks characters up in the atlas at 16 px, which must not disturb later glyphs (§27, fixture
  `code-sysfallback`: the page must look the same with `system_fallback` on and off when nothing is missing).
- **The saved font index** (so the scan happens once per installation, not at every start): the index of §8a also
  keeps, for every face, the characters it has (the ranges of code points its Unicode `cmap` subtables map to a
  glyph, formats 4, 6, 12 and 13) and whether it is a colour font. It is saved as a text file,
  `<state folder>/calligrus/fontindex.cache` (`$XDG_STATE_HOME`, else `~/.local/state`; on Android the app's files
  folder; on Windows `%LOCALAPPDATA%`), one record per font file keyed by its path, size and modification time. At
  start-up the folders are walked (file names and stat only), a file whose key is in the saved index is taken from
  it, only new or changed files are opened, and the file is rewritten (after the index is already usable) when
  anything changed or a font disappeared. A damaged file is ignored. Choosing fonts for missing characters then
  reads no font file at all: the candidates' coverage comes from the index; a candidate the index does not know is
  read from its file as before.
- **Never wait for a font**: the document is shown at once. While a search for fonts is pending (the check has not
  run yet for this document, the font index is still being read, or a search for characters not tried before is
  under way) characters no font has are drawn as **blank space**, never as `◻` and never as a blank screen; their
  width in the layout is the `◻`'s until the font arrives. When the search is done, the chosen font files are read
  on a worker thread (the choice and the read both happen off the UI thread; Android's `/system/fonts` Noto CJK
  files are many MB), the UI thread hands them to the atlas (a file already read for the previous atlas is not read
  again), the text is laid out again and drawn in the same frame. Characters no installed font has are tried once per
  session; once the search is over they show as `◻` as before. Script mode (§14) has no blanking and no waiting
  frames: it waits for the searches to finish before a screenshot, so a shot never shows the in-between state; with
  `system_fallback = false` nothing is blanked.
- **Timing log**: with the environment variable `CALLIGRUS_TRACE` set (Android: always, tag `calligrus`) the stages
  are logged with milliseconds: the font index (walk, saved index loaded, files read, built), the check of the
  document's characters, the choice and the file reads, the first frame, and the atlas rebuild with the frame that
  lays the text out again.
- **Config**: `system_fallback = true` (default); `false` turns it off. Settings: a checkbox
  "Use system fonts for missing characters".
- **Shaping**: code text is shaped one-to-one (§27 ligatures) and complex-script prose as in §32.
- **Tests**: unit tests of the choice on made-up faces and coverage (greedy order, ties, limit,
  colour fonts skipped); the character ranges of a cmap against asking it code by code; the saved index (written,
  read again for an unchanged file, a changed file read again, a damaged file ignored); the coverage check on the bundled fonts (e.g. "a" covered, U+0A97 not);
  the visual suite unchanged (system fonts differ per machine, so its `config.toml` sets
  `system_fallback = false`; its documents have no characters that need them, except the symbols
  fixture, which has a few no bundled font covers).

## 31. `<style>` and `<script>` blocks are hidden

A raw HTML block that starts with `<style` or `<script` (CommonMark HTML block type 1, which runs
to the line with its closing tag) is not drawn, in either mode, like HTML comments (§26). Inline
`<style>`/`<script>` inside a paragraph is left alone. Org files are unaffected. An upper case closing tag (`</STYLE>`) works too; lines after the closing
tag are ordinary Markdown again (CommonMark ends the block there). md4c does this itself. Find does not
search them; editor sync counts their lines as part of no block. Visual suite: add a `<style>`
block to an existing fixture document whose shot shows its place (or a new small fixture).

## 32. Shaping complex scripts in prose

Drawing one glyph per character would show the right letters but uncombined for Devanagari, Bengali,
Tamil, Sinhala, Thai... (with a font present, §30). So prose in those scripts is shaped with HarfBuzz
into conjuncts and placed vowel signs. Code ligatures (§27, one-to-one substitutions) stay as they are.

- **Scope**: left-to-right complex scripts only: the Unicode blocks of Devanagari, Bengali,
  Gurmukhi, Gujarati, Oriya, Tamil, Telugu, Kannada, Malayalam, Sinhala, Thai, Lao, Tibetan,
  Myanmar, Khmer (a range table in the code). Text without any such character is laid out and
  drawn exactly as today (no layout change for Latin, Greek, Cyrillic, CJK, symbols, emoji, so
  no existing screenshot changes). Right-to-left scripts (Arabic, Hebrew) need bidi: later.
- **Where**: the three functions every piece of text goes through, in inline_layout.cpp:
  `text_width` (word widths for line breaking), `char_xs` (character boundaries for selection,
  find highlights, Ctrl+click) and `draw_text`. When the string has a complex-script character,
  they use the shaped result instead of per-character advances. Words are still split at spaces
  as today (no dictionary line breaking for Thai: a long Thai run wraps like a long word).
- **Runs and fonts**: a string is cut into runs of one script. Of the Common / Inherited
  characters, the ones that belong to their letters (ZWJ/ZWNJ, combining marks, variation
  selectors) join the run before them; spaces, punctuation and digits do not: they stay on the old
  path, drawn from the chain's first font that has them (the prose face), so line
  breaks do not depend on the script font (a script font's comma or digits are other widths). Each complex run is shaped with the first source font of the ImFont's chain (the face,
  then its merged fallbacks in order: Ubuntu, emoji, Cascadia, Hack, system fonts) that has
  every character of the run (HarfBuzz nominal-glyph check); if none has them all, the run is
  drawn the old way (per character). Simple runs keep the old path. Bold / italic use the
  matching chain (a fallback without that style: its regular face, upright).
- **Shaping**: HarfBuzz (already a dependency), LTR, the run's script and default features,
  font scaled so advances come out in the same pixels the old path uses for that size (the
  `advance_scale` rule: advances from the exact em size). Glyph positions include HarfBuzz's
  x/y offsets (marks above / below).
- **Clusters**: each HarfBuzz cluster (the characters that make one visible unit, e.g. a
  conjunct) is atomic for selection: every character boundary inside a cluster equals the
  cluster's left edge, its last boundary the cluster's right edge. So clicks, drags and find
  highlights cover whole clusters; copy still copies the characters.
- **Drawing**: glyphs by id through `shaped_draw` (FreeType by index, same flags as ImGui, own GL
  pages), generalised from the code font to any source font (keyed by font + glyph + size), at
  pen + offsets, in the span colour; whole-pixel x like the old path. Glyph id 0 (`.notdef`):
  that cluster falls back to the old per-character path (◻ where missing).
- **Cache**: the plan of a string (its runs with their shaped glyphs) keyed by (ImFont, text),
  capped (4096 entries, cleared when fonts change). Glyph positions are kept in fractions of the
  em, so the key needs no size; the size comes in when the plan is laid out.
- **Not covered**: ImGui's own widgets (the find box, the settings fields...) still draw one
  glyph per character; only document text is shaped. Inline code with a complex script shows `◻`
  per character where the code font has no glyph.
- **Tests**: unit tests on run splitting (script ranges, Common / Inherited attachment), the
  cluster → boundaries mapping, and the fallback rules, with made-up data; a shaping test with a
  test font. A visual fixture `indic.mdscript` with its own config (`# config:
  config-indic.toml`) whose `[fonts] prose` is a test-only OFL font kept in spec/tests/fonts/
  (Noto Sans Devanagari Regular, static: it also has Latin; a path relative to the folder the
  suite runs in), so the shots don't depend on installed fonts; the
  shots do not depend on installed fonts. Also worth a manual look: a real Gujarati file with system fonts (§30).
## 33. PDF with real text

Exported PDFs (pages §29 and slides §26) were one picture per page: big (a 3-page document
~665 KB; Chrome's print of the same was 95 KB), blurry when zoomed, no selecting / searching /
copying. Now each page has two layers: the page drawn WITHOUT its text as a picture (flat colours:
compresses to little), and the text on top as real PDF text with embedded font subsets.

- **Picture layer**: the page drawn as now, minus every text glyph (the draw commands whose texture
  is the font atlas or a `shaped_draw` glyph page are skipped). Maths formulas' glyphs are text too (see "As built":
  TeX maths), their bars, radical rules and big-delimiter pieces, pictures, diagrams, boxes,
  rules, backgrounds stay in the picture. Written as RGB with Flate and the PNG "Up" predictor
  (`/DecodeParms << /Predictor 15 /Colors 3 /Columns W >>`), which shrinks flat areas to almost
  nothing.
- **Text layer**: every glyph drawn on the page, with its font (the actual source font that drew
  it, fallbacks included: Cascadia, Hack, Nirmala UI...), glyph id, position (baseline origin),
  size and colour, in drawing order (= reading order).
  - A capture sink in `draw_text` / the shaped paths (they know the source font and glyph
    id; plain characters: the font's cmap glyph for that character).
  - Text whose source font can't be read or subset is left in the picture layer instead
    (still visible, just not selectable).
- **Fonts in the PDF**: per source font used, a `/Type0` font with `/CIDFontType2` descendant,
  `/Identity-H` encoding, `/CIDToGIDMap /Identity`, the font program subset to the used glyphs
  (HarfBuzz `hb-subset`; see "As built" for the glyph numbering), widths (`/W`) from the
  font, and a `/ToUnicode` CMap (glyph → the characters of its cluster) so copy and search give
  the real text, conjuncts included. Font names `/ABCDEF+Name` subset tags.
- **Placement**: text as `BT /Fn size Tf r g b rg ... ET` with an absolute `Td` per glyph run
  (consecutive glyphs on one baseline with their natural advances in one `TJ` with corrections),
  coordinates converted from the page picture's pixels to points (y flipped). Text render mode 0
  (visible); the picture layer underneath has no text, so nothing doubles.
- **Modes**: vector is the default for pages and slides exports; `--raster` gives the old one-
  picture pages (also the automatic fallback if building the text layer fails, with a log line).
- **Expected**: the pages fixture's PDF far smaller (aim: < 150 KB for its 4 pages), text
  selectable in PDF viewers, same look as the raster export.
- **Tests**: PDF writer unit tests (CID font objects, ToUnicode, predictor stream round-trip with
  a tiny inflate); the `pages` fixture checks with PyMuPDF (run.py): the PDF's text
  contains its headings and "क्षत्रिय"-style text from `indic` exported too (copy/search through
  ToUnicode), the file is under the size limit, and a page rendered by PyMuPDF at the export's dpi
  differs from the raster export's page by a small percentage (different rasterisers: tolerance).
- **As built**:
  - The app does not skip draw commands: while capture is on (`text_capture.h`), `draw_text` reports each
    glyph it can describe and does not draw it, so the picture has no text and a glyph whose font can't
    be embedded is simply drawn as before. Its source font is the first font of the ImFont's chain
    (`prose_shaping::find_source`) that has the character; the code font's ligature glyphs and shaped
    clusters report their own glyph ids. `cover()` drops glyphs under the rectangles a page paints over
    its margins; the fonts' bytes are copied (the app reloads fonts while an export runs).
  - Clipping rule: a glyph is kept only if the middle of its x-height (baseline - 0.3 em) is
    inside the clip rectangle, the page and not covered, so a line cut by a page edge is wholly in or
    out     Checked by run.py's `# expect-pdf-no-repeat`.
  - Subsetting renumbers glyphs (hb-subset plan's old-to-new map), not RETAIN_GIDS: far smaller for big fonts. /W, the content stream and ToUnicode use the new ids.
    Fonts with TrueType outlines (`/FontFile2`) and CFF outlines in an OpenType file (the math font:
    `/CIDFontType0` + `/FontFile3 /Subtype /OpenType`, no `/CIDToGIDMap`) are embedded; others stay in the picture.
  - A glyph's ToUnicode entry is the same wherever it is used, but a vowel sign belongs to a different
    consonant each time, so only one-glyph clusters get ToUnicode entries; clusters of several glyphs or
    characters are wrapped in `/Span << /ActualText <FEFF...> >> BDC ... EMC`. The layout has no space
    glyphs between words, so the writer adds a real space glyph where two clusters on a baseline are
    0.15 to 2 em apart (copy then gives "word word"); a gap over 0.1 em starts a new `BT` run.
  - Fallback: the app runs itself again with `--raster`, logging "the PDF's text layer failed (...)".
    The text layer is only built for `.pdf` output.
  - Dependencies: `zlib` (Flate at level 9, as stb's fixed-Huffman deflater made a flat A4 picture 8 times
    bigger) and HarfBuzz's `hb-subset` (link `harfbuzz::harfbuzz-subset`).
  - Checks: `run.py` reads `# expect-pdf-text: FILE words|words`, `# expect-pdf-max-kb: FILE N` and
    `# expect-pdf-raster-diff: FILE DOC.md PCT` (PyMuPDF; skipped with a message when it is missing): `pages.mdscript` checks its headings, 150 KB and 6 % of pixels (measured 2 to 3 %);
    `indic.mdscript` exports itself and checks the Devanagari words. Unit tests in `calligrus-pdf-test`: CID font objects, /W, subset font, ToUnicode (clusters, chunks), ActualText,
    word spaces, predictor round trip, content stream, `--raster` in the `export_args` tests.
  - Size (4-page `pages` fixture, raster / vector): about 701 KB / 76 KB.
  - **TeX maths is text**. The engine's glyphs (Latin Modern Math, CFF) are no longer in the formula's
    bitmap while an export captures text: `texmath::render(.., capture)` leaves out every uniformly scaled
    glyph (`sx == sy`) that has a character and lists it as a `MathGlyph` (glyph id, baseline origin in bitmap
    pixels, pixels per em, character). The bitmap keeps its size and origin; fraction and radical bars (rules),
    `\cancel` lines and glyphs without a character (size variants like `parenleft.size2`, the pieces of braces
    and tall delimiters, big operators' display forms such as the integral sign) stay in the picture. Which
    character: the lowest code point the font's cmap sends to the glyph (controls and private use excluded);
    math alphanumerics are folded to the plain letter / digit / Greek letter (italic x copies as "x", ℎ as "h"),
    so search finds them. Capture mode is off for the screen; `math_get` caches capture bitmaps
    separately and `math_draw` (the three places a formula is painted) reports the glyphs through
    `text_capture::math` (same clip rule: middle of the x-height inside the clip and page). Copying a
    formula gives e.g. "e−x2 dx = √π 2".
  - **Opacity**: a glyph has an alpha (the colour's alpha); under 255 its run is wrapped in
    `q /GS<alpha> gs ... Q` with the page's
    `/ExtGState << /GS<alpha> << /Type /ExtGState /ca a >> >>` (a = alpha / 255, 3 decimals). Runs of different alpha
    never merge.
  - More checks: `pdf::tests` / `calligrus-pdf-test` (ExtGState, CFF font as OpenType), `calligrus-texmath-test capture`, and `math.mdscript` exports `math.pdf` and expects its formulas' text.

## 34. Tabs

Several documents in one window, normal mode only. Editor mode never has tabs (it shows the
buffer the editor has focused).

- **A tab** holds everything about one open document: path, parsed blocks and their caches,
  scroll position, selection, find (query, matches, current), back/forward history, slides state
  (on/off, slide, step, overview, clock), the marks margin, the file watcher / reload state. The
  window keeps what is global: config, theme, fonts, zoom, outline panel shown, settings panel,
  presenter window, export job, toasts, recent files, marks (state.toml).
- **Tab bar**: a row under the toolbar, only when 2+ tabs are open. Each tab: the file name
  (longer than 24 characters: the first 23 and `…`; tooltip = full path), an `x` on hover / for the active tab;
  active tab drawn like a selected toolbar button. If tabs don't fit, the row scrolls
  horizontally (wheel over it) and keeps the active one visible. Hidden in slides mode (as the
  toolbar is) and in editor mode.
  Look: a second top panel under the toolbar with the same margins
  (8 px at the sides, 2 px above and below), 13 pt UI text, exactly as tall as the toolbar panel
  (the document moves down by the toolbar's height), and its own 1 px rule in the last pixel row
  (colour of the toolbar's rule); the document starts under that rule. Each tab is like a selectable
  label: no background unless hovered (`widget_hover`) or active
  (`selection` fill, `selection_text` text colour); button padding 4 px sideways, 4 px between the
  name and the `×` (a weak-colour `x`, shown on the active tab and on the tab under the
  pointer; it gets its own `widget_hover` square when the pointer is on it), 2 px between tabs.
  Tooltip on the name: the full path.
  The names are measured and drawn with the document font's exact advances plus the face's kerning
  (`prose_shaping::kerned_pieces`), not ImGui's whole-pixel UI advances, and a tab is
  `ceil(4 + name + 4 + x + 4)` points wide.
- **Opening**: Open / recent / drop / command line replace the current tab's document, (history keeps working). A NEW tab: Ctrl+click or middle-click on a link to a Markdown/Org file;
  Ctrl+T opens the file dialog into a new tab; files given on the command line (several allowed)
  each open in a tab; "Open in new tab" in the recent popup (Ctrl+Enter there). Opening a file
  that is already open in a tab switches to that tab instead (same rule as recent files: same
  path, case-insensitive on Windows). A new tab is appended at the end and becomes the active one;
  an unreadable file opens nothing. Several command-line files (also a repeated one: that file
  once): the first is shown, the others wait in tabs behind it. Ctrl+click on a row of the recent
  popup is a new tab too. Exception: when the current tab shows the built-in demo (no file), the
  file takes the demo tab's place instead of opening next to it (so Ctrl+T from a fresh window
  gives one tab, not demo + file). Ctrl+click / middle-click on anything but a link to a Markdown/Org file
  behaves as without Ctrl (a middle-click on a link does not start autoscroll).
- **Switching / closing**: Ctrl+Tab / Ctrl+Shift+Tab (next / previous, wrapping), Ctrl+PgDn /
  Ctrl+PgUp (same), Ctrl+1..8 (that tab; none if there are fewer), Ctrl+9 (last); click a tab; Ctrl+W or middle-click a tab
  or its `x` closes it (the tab to its right becomes the active one, or the last one if it was
  the last; closing a tab that is not the active one keeps the active one); closing the last tab leaves an empty window showing the built-in demo, as
  at start without a file. Vim keys: `gt` / `gT` next / previous.
- **Global marks** (`'A`) and links to an already-open file switch to its tab; otherwise they open
  in the current tab.
- **State**: `state.toml` gains `tabs = ['path', ...]` and `active_tab = n`; on start without a
  file argument the tabs are reopened (missing files skipped), each at its remembered scroll.
  With file arguments, those replace the saved tabs. Exact format (the exact bytes written):
  - Written only while 2 or more tabs with a file are open (a tab showing the built-in demo is not
    listed); with one tab neither key is written (the old behaviour: start without a file = demo).
    Written with every state save: on exit, and when a tab is opened, closed or switched.
  - Placement: top-level keys, right after `recent` and before any other plain key and the first
    `[table]`: `recent = [...]`, then `tabs = [...]`, then `active_tab = N`.
  - `tabs` is written as `recent` is: absolute UTF-8 paths, each a TOML literal string `'...'`
    (a basic `"..."` string if the path holds `'` or a newline), separated by `, `, e.g.
    `tabs = ['D:\docs\a.md', 'D:\docs\b.md']`.
  - `active_tab` is 1-BASED (like `expect-tabs` and Ctrl+1..9), an integer, `1 <= N <= len(tabs)`.
  - Reading: entries that are not non-empty strings are skipped; files that no longer exist are
    skipped when reopening (`active_tab` is a position in the SAVED list; if that tab was skipped,
    the first one is shown); no key / no tab left: the normal start (demo). `tabs` counts as known
    only when it is an array and `active_tab` only when it is an integer (then they are not kept in
    `extra_values`; a `[tabs]` table from some other version is an unknown key and survives); other
    unknown keys are preserved as before. Every tab's scroll offset goes into `[[scroll]]` as for a single
    file (all tabs are remembered on every save, not only the active one).
  - Script mode reads the tabs like a normal start (so a test can seed them through `CALLIGRUS_STATE`)
    but never writes the state file; editor mode ignores them.
- **Script mode**: `keys ctrl+tab` etc. work; new command `expect-tabs N
  ACTIVE` fails unless N tabs are open and tab ACTIVE (1-based) is active. Visual fixture
  `tabs.mdscript` (with `tabs.md`, `tabs-b.md`, `tabs-with-a-very-long-file-name.md`): a ctrl+click and
  a middle-click on links open two tabs, shots of the bar, Ctrl+1, Ctrl+Tab (wraps), Ctrl+W twice.
  The scripts' `expect-tabs` are checked; shots: one, two, first, three, wrapped,
  closed, last.
- **Implementation**: `struct Tab` = DocState + FindState + SlideState + History in `tabs`; the frame loop keeps
  working on the variables `st` / `find` / `slides` / `history` that many lambdas hold by reference, and
  switching SWAPS them with the chosen tab's, `tabs[active]` being an empty stand-in.


## 35. Android

The Android app (`io.github.thekaossphere.calligrus`, label "Calligrus", minSdk 26, target 36; arm64-v8a for tablets, x86_64
for the emulator) runs the same document, layout and render code as the desktop app: `main.cpp` is compiled for
both, against a small window layer (`src/android/window_android.*`: the part of GLFW it uses, over GameActivity,
EGL and OpenGL ES 3; the ImGui OpenGL backend runs with `#version 300 es`). Anything not listed here behaves as in the
sections above. Build: BUILDING.md. Desktop behaviour is unchanged (the visual suite is the guard).

- **Scrolling (finger)**: platform-free `touch.h` / `touch.cpp` (console test `calligrus-touch-test`; times are
  seconds on any steady clock). The physics follow Android's own (the Android Open Source Project, Apache
  License 2.0: `android.view.ViewConfiguration`, `android.widget.OverScroller` / `SplineOverScroller`,
  `android.view.VelocityTracker`'s default LSQ2 strategy), with the screen density d (dpi / 160) from
  `AConfiguration_getDensity`. Fingers that start on the document view:
  - A press that stays within the slop (8 dp) and ends within 0.35 s is a **tap**: a click at the press point (links,
    pictures, the right word on a double tap). Nothing else is a tap.
  - **Press feedback**: a widget (toolbar, menu) shows its pressed look only after the finger has rested on it 90 ms,
    so a press that turns into a scroll never flashes a pressed button. Letting go on it clicks it; a drag or a
    press over 0.35 s lets go elsewhere (no click).
  - A drag moves the content **1:1** with the finger; when the slop (8 dp, vertical) is crossed the slop is
    *subtracted* (as `ScrollView` does), so the content starts from rest, with no jump. No easing. Moving back moves
    back.
  - Lifting while moving starts a **fling**. Speed = the **LSQ2** velocity of the samples of the last 100 ms (at most
    20): `VelocityTracker`'s default strategy for a finger's X and Y (frameworks/native `VelocityTracker`,
    `DEFAULT_STRATEGY = LSQ2`; the impulse strategy is what it uses for the other axes, such as the scroll wheel):
    the samples are fitted, newest first, with a parabola in the time before the newest sample by least squares, and
    the speed is its slope at the newest sample (a line with two samples, no speed with one or with a degenerate fit).
    A pause of 40 ms or more between samples forgets the earlier ones (`ASSUME_POINTER_STOPPED_TIME`), two samples with
    the same time are one (the later wins), and a finger that rested 40 ms or more before lifting has no momentum.
    **Every sample counts**: a MotionEvent that arrives with older samples batched in (its history, each with its own
    time) feeds all of them to the tracker in time order, then its current sample; times are the events' own times,
    never the time the app received them. (The impulse value of the same samples is kept in the debug log.)
    Below 50 dp/s: no fling; capped at 8000 dp/s.
    The fling is `SplineOverScroller`'s: with `ppi = 160 d`, `coeff = 9.80665 * 39.37 * ppi * 0.84`,
    `l = ln(0.35 |v| / (0.015 coeff))` and `DECELERATION_RATE = ln 0.78 / ln 0.9`, the distance is
    `0.015 coeff exp(DECELERATION_RATE / (DECELERATION_RATE - 1) l)` and the duration
    `floor(1000 exp(l / (DECELERATION_RATE - 1)))` ms, travelled along the spline position table of
    `SplineOverScroller` (100 samples, computed as it does). E.g. at d = 2.625: 1000 px/s goes 95.5 px in 272 ms,
    4000 px/s 1059.9 px in 757 ms. Time based (the same at 30 and 60 fps). A fling **stops at the ends** of the
    document (no bounce). A touch that lands on a running fling stops it at once and is not a tap.
  - **Pinch** (two fingers): the ratio of the finger distance is the zoom factor (zoom 0.5 to 3.0, spec 1; rounded to
    0.01, saved like the zoom keys do). It zooms the **document only** (the toolbar, outline and widgets keep their
    size: they use the screen density, the document the density times the zoom), **around the fingers**: the
    content under them stays under them.
    *While the fingers are down* nothing is laid out again and the font atlas is not touched: the document, as laid
    out, is drawn into the same draw list and its vertices and clip rectangles are then scaled by the pinch factor
    about the fingers' starting point and moved with their centre (blocks are drawn for the screen range that maps
    onto the view, so zooming out shows more). It is a little blurry or pixelated in between, by design.
    *When the fingers lift* the document is laid out once at the final zoom, and the block that was under the
    fingers is put back under them (`zoom_anchor` plus the offset of the fingers from the top of the view); then
    the picture is sharp. Lifting one finger of a pinch continues as a drag from where that finger is, with no fling
    and no tap. A third finger is ignored. (A developer switch, a file `pinch_legacy` in the home folder, makes a pinch
    lay out at every step, to measure the difference; the log, tag `calligrus`, prints the cost per frame.)
  - Fingers that start elsewhere (toolbar, outline, a popup, the settings window) belong to ImGui: a tap clicks the
    widget under it; a drag scrolls the **window under the finger** (a popup, the theme list, the recent files, the overflow
    menu, the outline, the settings panel, a child window: the front-most window at the touch-down, or its nearest scrollable
    parent child window) by writing its scroll offset: 1:1 after the slop, the same fling physics as the document, clamped at
    the ends (a fling stops there). The pointer is **not** moved while the finger drags: no item is hovered or highlighted on
    the way (the theme picker's live preview follows the keys only). A press shows only after 90 ms of rest and is released
    off the item (no click) as soon as the finger moves past the slop; only a tap (up within the slop and 350 ms) clicks.
    **Wide blocks**: a code block or display formula wider than the column scrolls sideways (`RenderResult::hblocks`, offset in
    `RenderCache::code`). A formula with a `	ag` keeps at least 1 em between formula and tag (a centred formula moves left to
    keep it); when formula + gap + tag is wider than the column the formula and its tag are one wide line that scrolls (Shift +
    wheel or a sideways wheel on the desktop, no bar there; a thin bar shows for 1.2 s after any scroll, as under code). A finger
    that goes down on such a block feeds both the page tracker and a second tracker with x as y (same slop, LSQ2, fling); the
    axis that crosses the slop first is kept (both in one event: the larger of |dx|, |dy|), the other is cancelled, a second finger
    cancels the block scroll; a tap on the block is the document's tap. Offsets clamp to 0..overflow and a fling stops there.
    Two fingers on a list do nothing (no document zoom). While the settings window is open a **sideways** drag (past the
    slop with no vertical drag yet) is the mouse from the touch-down point, so sliders can be dragged; a vertical drag scrolls.
  - A hardware keyboard works (letters, arrows, Page Up / Down, Space, Ctrl chords, so the vim keys and Ctrl+F work).
    The soft keyboard is not wired up yet: the find bar and text fields need a hardware keyboard.
- **Mouse and tooltips**: input from a real mouse or trackpad (`AINPUT_SOURCE_MOUSE`, or tool type mouse) is not a
  finger: it goes to ImGui as the mouse (`ImGuiMouseSource_Mouse`: hover, the buttons, the wheel and hover moves), and
  the document scrolls with its wheel as on the desktop (a notch = 40 pt x the document scale, as `kWheelNotchPoints`; fractional
  values of a trackpad or a high-resolution wheel scroll proportionally; the horizontal axis, or Shift + the wheel, scrolls a
  code block sideways, as in render.cpp). GameActivity copies only the X and Y axes of a pointer unless asked, so the
  wheel axes (`AMOTION_EVENT_AXIS_VSCROLL`, `AXIS_HSCROLL`) are enabled at start-up. Fingers feed ImGui with the touch-screen source
  (`ImGuiMouseSource_TouchScreen`) before every mouse event they make. **Tooltips** (toolbar buttons, tabs, link
  addresses) show only while the current mouse source is the mouse, so a tap leaves no tooltip behind and a mouse on
  a tablet still has them and the hover looks. (The desktop's mouse is always the mouse: no change there.)
- **Touch debug log**: a file named `touch_log` in the app's home folder (the files folder:
  `adb shell run-as io.github.thekaossphere.calligrus touch files/touch_log`; delete it to stop; looked at at every
  press) makes the app log, tag `calligrus`, every touch sample (`touch move id= x= y= t=<ms> hist=<0|1> raw=<the
  event's raw time>`; `hist=1` for a batched history sample) and, at every lift, `touch lift: samples= lsq2= impulse=
  clamped= stale= fling=` in px/s: what a real device sends and what the velocity tracker made of it.
- **Window**: edge to edge; the app draws inside the system bars and the camera cut-out (insets are read every frame;
  the status-bar strip is painted in the panel colour and its icons follow the theme: light icons on dark themes).
  The screen density (dpi / 160) is the DPI scale of spec 1: the toolbar and text are in dp. Widgets are
  finger-sized (larger padding and scroll bars). Rotation and split screen only resize the window.
  While the app is in the background the GL context and all textures are kept; the event loop sleeps.
- **Toolbar**: Open, the recent-files arrow, Reload, Outline, A- / A+, the theme picker, Settings and the file name.
  Left out: Slides (slides mode, tabs and the presenter need keys and a second window), PDF export (a child process
  and a save dialog), editor mode (`--editor`), `--register`.
  **Overflow menu (shared code)**: when the buttons do not fit the width, they move one by one into a menu behind a
  three-dot button at the right end of the bar, so the bar never runs off the screen. Priority, the last to go first
  in the list: Open, Outline, A- / A+, Settings, theme, Reload, recent files (then, on the desktop, Slides and PDF), so a phone
  keeps Open, Outline, A- / A+, the gear and the three dots.
  The menu lists the moved items by name ("Theme..." opens the theme list below the button, "Recent files..." the
  recent list, "Smaller text" / "Larger text" stay open for repeated taps). **The file name**: with the three-dot button (a phone upright) the bar has **no file name**; the menu starts with it instead,
  as a greyed line (not an item), shortened in the middle with `…` (both ends kept, so the extension stays:
  `JW-MWM_2…10-29.md`) when it is longer than 260 pt, followed by a rule. Without the button (a tablet, a phone sideways, any
  desktop window) the name gets the room left before the right edge of the bar: shortened in the middle the same way
  (Android; the desktop draws the whole name and the window edge cuts it, as the references show), and left out when
  there is room for fewer than 3 characters. The desktop has the three-dot button only below 400 px of toolbar width, so its
  usual window sizes (the visual suite's included) are unchanged. On Android the button comes at any width where the buttons do not fit (a landscape tablet shows everything). On a
  screen narrower than 600 dp (a phone upright) the outline panel starts hidden.
- **Opening files**: the Open button starts Android's document picker (`ACTION_OPEN_DOCUMENT`, `text/*`,
  `application/octet-stream`, any name: `.md`, `.markdown`, `.org`). The app is also offered by "Open with" (`VIEW` with
  Markdown / Org / plain-text types, or a path ending in `.md`, `.markdown`, `.org`) and by "Share" (`SEND`, a file
  or plain text, shown as `shared.md`). Java reads the stream (up to 64 MB) with `ContentResolver` and hands the name
  and bytes to native code, which stores them as `<files dir>/opened/<hash of the bytes>/<name>` (the 40 newest are kept)
  and opens that file like a dropped one; the name's extension picks Markdown or Org (spec 25), as on the desktop.
  Differences: **relative pictures and links next to the document do not resolve** (only the copy is local;
  absolute `http` pictures are not fetched either, as on the desktop), and a changed original is not reloaded.
  Links to `http`, `https` and `mailto` open in the system's app; opening a file link to another local file works only
  inside the copy's folder.
- **Folders**: `HOME` and the XDG folders point into the app's files dir (`config/calligrus/config.toml`, `state/`,
  themes), so the config, themes and state work as on Linux; `TMPDIR` is the cache dir. The system theme (night mode) is
  read from the configuration.
- **Fonts**: the bundled fonts are inside the library as on the desktop. The system-font fallback (spec 30) scans
  `/system/fonts` (and `/product/fonts`, `/system_ext/fonts`) for characters no bundled font has, so Devanagari,
  Gujarati and the like use the device's Noto fonts.
- **Power** (an idle app costs nothing): the loop sleeps in `ALooper_pollOnce` with no polling. GameActivity's glue writes a
  command to the looper's pipe for every touch, key and text event, so input wakes the loop at once (the old 16 ms
  polling is gone); jobs wake it with `glfwPostEmptyEvent`. The wait's timeout is the next real deadline only: the
  debounced config write (300 ms), the scrollbar hiding, the toast ending, a find search waiting for the typing to stop,
  and, after input, a 200 ms step for one second (hover delays, widget fades) and a 500 ms step while a text field has the
  focus (caret). With none of these there is no timer at all. A wake that is only the config timer writes the file and
  neither draws nor swaps buffers; frames are also skipped while nothing moves (no animation, no pending scroll, anchor,
  layout or font job). There are no file watchers on Android (opened files are local copies): the night mode is read at
  every wake. The config and the state file are written at once when the app goes to the background (`APP_CMD_PAUSE`,
  `STOP`, `SAVE_STATE`, `TERM_WINDOW`), since the process can be killed from then on. ImGui's clock follows real time
  (the frame delta is capped at 5 s, not 0.5 s). Dev switches in the app home: `power_legacy` (the old loop), `power_log`
  (a log line per wait, frame and skipped wake, tag `calligrus`).
- **Later work**: the soft keyboard, slides and the presenter, PDF export, text selection by long press and handles,
  the outline as a drawer on narrow screens, relative pictures (copy the document's folder, or read it through the storage framework), a share-out, a Play build.
