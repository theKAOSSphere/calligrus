# Shared tests for Calligrus

Inputs and expected results for spec §5 (config file) and §6 (themes) in
`../SPEC.md`. `calligrus-config-test` and `check.py` must pass every file here. Where this
README is more precise than SPEC.md, this README is the rule.

`check.py` (Python 3.11+, no packages needed) is a reference implementation of
every rule below. It checks all files in this folder and the built-in themes in
`../../themes/`, and prints a contrast table:

    python check.py           # exit code 1 on any failure
    python check.py --regen   # rewrite the expected files from the reference code (review the diff!)

All files are UTF-8. Expected files use `\n` line endings and end with a
newline, except `edit-*.expected-toml`, which are the exact expected bytes.

## 1. Config parsing: `config-<name>.toml` → `config-<name>.expected`

Load the `.toml` as a config file, resolve every setting, and print one line per
setting:

- `key = value`, with exactly one space on each side of `=`.
- Keys of tables are dotted: `fonts.mono`, `code.line_numbers`.
- All 13 settings are always printed (fully resolved, defaults included), sorted
  by key in byte order.
- Values in TOML syntax: `true`/`false`; integers in decimal; floats as the
  shortest decimal text that reads back as the same double, always with a `.`
  (`1.0`, `1.5`, `0.5`, `3.0`); strings as TOML *basic* strings (see §4 below).
- Strings are printed exactly as configured: no `~` / `%VAR%` expansion, no
  check that a font file or a theme exists (that happens later, at use).

| Key | Default | Valid values |
|---|---|---|
| `theme` | `"auto"` | any non-empty string |
| `dark_theme` | `"calligrus-dark"` | any non-empty string |
| `light_theme` | `"calligrus-light"` | any non-empty string |
| `zoom` | `1.0` | a float or an integer, finite, 0.5 ≤ zoom ≤ 3.0 (an integer is converted: `3` → `3.0`) |
| `body_size` | `16` | an integer, 12 ≤ body_size ≤ 28 (a float, even `16.0`, is the wrong type) |
| `max_width` | `860` | an integer, 400 ≤ max_width ≤ 2400 (points at zoom 1.0) |
| `show_outline` | `true` | boolean |
| `vim_keys` | `true` | boolean |
| `remember_window` | `true` | boolean |
| `editor_marker` | `true` | boolean |
| `system_fallback` | `true` | boolean (system fonts for characters no bundled font has, spec §30) |
| `fonts.prose`, `fonts.prose_bold`, `fonts.prose_italic`, `fonts.prose_bold_italic`, `fonts.mono` | `""` | any string (`""` = built-in face) |
| `code.line_numbers` | `false` | boolean |

Rules:

- **Fallback, never clamp.** A value of the wrong type, or a number outside its
  range (including `nan` / `inf`), is replaced by the key's **default**. It is
  NOT clamped: `zoom = 3.5` → `1.0`, `body_size = 11` → `16`. (The zoom
  *keys* clamp at run time, §1; a config file with an out-of-range value is
  treated as invalid.) Each key falls back on its own; valid keys in the same
  file still apply.
- Unknown keys and unknown tables are ignored. Keys are case-sensitive
  (`Zoom` is an unknown key).
- If `fonts` or `code` exists but is not a table (`fonts = "Hack"`), all keys of
  that table take their defaults.
- A file that is not valid TOML at all (`config-broken.toml`) resolves to all
  defaults. An empty file (`config-defaults.toml`) resolves to all defaults.

| File | What it covers |
|---|---|
| `config-defaults.toml` | empty file |
| `config-full.toml` | every key set to a valid non-default value; Windows paths as literal and as escaped strings, a `"` inside a string |
| `config-invalid.toml` | wrong types, out-of-range zoom/body_size, empty theme name, unknown keys and tables, bad `[fonts]` / `[code]` value types |
| `config-numbers.toml` | `zoom = nan`, `body_size = 16.0` |
| `config-bounds.toml`, `config-bounds-low.toml` | inclusive range bounds, integer zoom, an unknown theme name is still a valid *string* |
| `config-badtables.toml` | `fonts` / `code` given as non-tables |
| `config-broken.toml` | a TOML syntax error |
| `config-comments.toml` | comments, blank lines, tabs, no/odd spacing, a quoted key, `[ fonts ]`, `#` inside a string, commented-out keys |

## 2. In-place writer: `edit-<name>.toml` + `edit-<name>.ops` → `edit-<name>.expected-toml`

Start from the bytes of `edit-<name>.toml`, apply each op of the `.ops` file in
order (each op works on the result of the previous one), and compare the final
bytes with `edit-<name>.expected-toml` exactly.

`.ops` format: one op per line, `set <key> <value>`, where `<key>` is a
top-level key (`zoom`) or `table.key` (`fonts.mono`), and `<value>` is the rest
of the line, a TOML value (parse it as the value of `v = <value>`). Blank
lines and lines starting with `#` are ignored. The writer receives the *typed*
value and serialises it itself (canonical form, §4), so `'C:\x'` in an op is
written as `"C:\\x"`.

The writer's rules (the file is treated as lines; a line ends at `\n` or
`\r\n`; the last line may have no terminator):

1. **Line ending**: the file's line ending is the terminator of its first
   line (`\r\n` or `\n`); a file with no terminator at all (or an empty file)
   uses `\n`. Every line the writer adds uses it. Existing lines are never
   changed except as described here.
2. **Sections**: a *header line* is a line whose first non-blank character is
   `[`; its table name is the text between `[` and the first `]`, trimmed of
   spaces and tabs (`[ fonts ]  # x` is `fonts`). The top-level section runs
   from the start of the file to the first header; a table's section runs from
   its header to the next header (or the end of the file). If a name occurs
   twice, the first one is used.
3. **Finding the key**: inside the key's section, the first line that, after
   leading spaces/tabs, starts with the key name, bare (`mono`) or in double
   quotes (`"mono"`), followed by optional spaces/tabs and `=`. So `prose`
   does not match `prose_bold = ...`, and comment lines (`# zoom = 2.0`) never
   match. Dotted keys (`fonts.mono = ...` at top level) and inline tables are
   not recognised (not tested; avoid them).
4. **Replacing a value**: the old value starts at the first non-blank after
   `=` and ends at the closing quote of a `"..."` string (honouring `\"` and
   `\\`) or a `'...'` string, otherwise at the first space, tab or `#`. Only
   that span is replaced; everything before it (indentation, key, spacing
   around `=`) is kept byte-for-byte. After it:
   - if a comment follows (only spaces/tabs between the value and `#`) and that
     gap consists of spaces only, the gap is resized so the `#` stays in the same
     column: new gap = max(1, old gap + old value length − new value length),
     lengths in characters (Unicode scalar values);
   - a gap containing a tab, trailing whitespace without a comment, the
     comment itself and the line terminator are kept unchanged.
   Setting a key to the value it already has (in canonical form) changes
   nothing.
5. **Adding a key to an existing section**: insert the line `key = value`
   directly after the last non-blank line of the section (comment lines count
   as non-blank), i.e. before any blank lines that precede the next header. If
   the section has no non-blank line, insert directly after its header (for the
   top-level section: at the start of the file). If the line it is inserted
   after has no terminator (end of file), give it one first; the new line
   always gets one.
6. **Adding a key whose table does not exist**: at the end of the file: give the
   last line a terminator if it lacks one; if the file is not empty and its last
   line is not blank, add an empty line; then add `[table]` and `key = value`,
   each with a terminator. In an empty file the result is just those two lines.
7. A UTF-8 byte order mark, if present, is kept (not tested).

| File | What it covers |
|---|---|
| `edit-value-comment` | replacing values that have trailing comments, keeping the comment column |
| `edit-fonts-value` | values inside `[fonts]`; `prose` vs `prose_bold`; a `#` inside the old string value |
| `edit-add-key` | adding missing keys to the top-level section and to `[fonts]` (before the blank line and the next header) |
| `edit-add-table` | creating `[code]` and `[fonts]` at the end of the file; the second fonts key goes into the new table |
| `edit-no-newline`, `edit-no-newline-replace`, `edit-no-newline-table` | file without a trailing newline: replacing on the last line keeps it unterminated; adding a line terminates it |
| `edit-crlf` | CRLF endings kept and used for new lines and new tables |
| `edit-commented-key` | commented-out keys are not the key; a section holding only a comment |
| `edit-windows-path` | Windows paths (from a literal-string op and from an escaped-string op) written as escaped basic strings |
| `edit-empty` | empty file: a top-level key, then a new table (with the blank separator line) |
| `edit-realign` | longer value than the gap allows (gap shrinks to 1 space), a tab gap kept, a line without a comment, `zoom=1.0` without spaces |
| `edit-same-value` | setting existing values again leaves the file byte-identical |

## 3. Themes

### `theme-<name>.toml` → `theme-<name>.expected`

Load the file as a (user) theme and print every resolved `[ui]` and `[doc]`
colour, one `table.key = #rrggbbaa` line each (no quotes), all 35 lines sorted
by key in byte order (so all `doc.*` lines come before `ui.*`).

- Colours are written in lower case, 8-digit form: `#RRGGBB` → `#rrggbbff`,
  `#RRGGBBAA` → `#rrggbbaa`.
- A missing key, or a value that is not a `#rrggbb` / `#rrggbbaa` string
  (`"grey"`, `"#abc"`), takes the value of the built-in `calligrus-dark` when the
  theme has `dark = true`, otherwise of `calligrus-light`.
- Exception: `doc.heading`, when missing or invalid, defaults to this theme's
  own resolved `doc.text` (SPEC.md: "optional, default = text").
- `doc.heading_1` .. `doc.heading_6` (per-level heading colours) are optional
  and never come from the base theme: missing or invalid, levels 1-5 take this
  theme's resolved `doc.heading` and level 6 its `doc.weak` (h6 is drawn weak).
- `doc.bold`, `doc.italic` and `doc.quote` are optional too and never come
  from the base theme: missing or invalid, bold and italic take this theme's
  resolved `doc.text`, quote its `doc.weak`.

`theme-syntax.toml` is not resolved this way; it is a helper theme for the
syntax lookups below.

### `syntax-lookup.txt`

Lines `<theme name> <capture name> => <result>`; blank lines and lines starting
with `#` are comments. The themes are all built-in themes (`../../themes/*.toml`)
plus `theme-syntax.toml` (name `syntax-test`). `<result>` is `plain` or the
colour as `#rrggbbaa` (lower case) followed by ` bold` and/or ` italic`, in
that order.

Resolution, in two stages:

1. Capture → *entry name* via `grammars/theme.txt` (as the highlighter does
   today): a capture starting with `_` is a helper → `plain`. Otherwise use the
   capture itself if listed, else its longest listed dotted prefix
   (`function.builtin.call` → `function.builtin`; `functional` does **not**
   match `function`). No listed prefix, or an entry whose colour is `-`
   (`variable`, `operator`, `punctuation`, ...) → `plain`.
2. Entry name → style via the theme's `[syntax]` table: the entry itself, else
   its dotted prefixes within the table (`type.builtin` → `type`), else
   `plain`. `[syntax]` keys do **not** fall back to calligrus-dark/-light.

A `[syntax]` value is `"#rrggbb"` or `"#rrggbbaa"` (any case) followed by
optional whitespace-separated words `bold` and/or `italic` in any order, with
any amount of whitespace. A value whose colour is invalid counts as a missing
entry; unknown words are ignored (both not tested).

## 4. String and float formatting (expected files and the writer)

- Strings are always written as TOML **basic** (double-quoted) strings, never
  literal strings: `\` → `\\`, `"` → `\"`, `\b \t \n \f \r` as those escapes,
  other control characters (U+0000–U+001F, U+007F) as `\uXXXX` (upper-case
  hex); everything else, including non-ASCII, as plain UTF-8. So the Windows
  path `C:\Share\me\x.ttf` is written `"C:\\Share\\me\\x.ttf"`.
- Floats: the shortest decimal text that reads back as the same double, with a
  `.` and at least one digit after it (`2.0`, `1.25`); no exponent form (not
  needed in Calligrus's ranges). Booleans `true`/`false`, integers in decimal.

## 5. Font picking by family name (SPEC.md §8a): `fontpick-<name>.txt`

Each file is a self-contained test: a fake font index plus queries with their
expected answers. Test the apps' **resolver** (name-vs-path rule, face picking,
family listing) against it, separately from the real font scanner.

### File format

UTF-8, `\n` line endings. Blank lines and lines starting with `#` are comments.
A line containing ` => ` is a **query**; every other line is a **face line**.
All face lines of the file form the index (their position relative to the
queries does not matter); the order of face lines never affects any answer.

Face line, six or seven fields separated by `|`, each trimmed of spaces and tabs:

    family | subfamily | weight | italic | mono | file#face [| width]

- `family`, `subfamily`: as the scanner reads them from the `name` table
  (typographic IDs 16/17, else 1/2). Neither contains `|`.
- `weight`: the `OS/2` weight class, an integer 1–1000.
- `italic`: `1` if the italic bit (fsSelection bit 0) is set, else `0`.
- `mono`: `1` if the scanner found the face monospaced (`post.isFixedPitch` ≠ 0
  or panose proportion = 9), else `0`.
- `file#face`: the font file path, then `#`, then the face index in decimal
  (`0` for `.ttf`/`.otf`; one line per face of a `.ttc`). The face index is the
  digits after the **last** `#`.
- `width` (optional 7th field): the `OS/2` `usWidthClass`, a non-negative
  integer; 5 = normal, lower = narrower. Missing field → 5. A value outside
  1–9 (e.g. `0`) is treated as 5, as the scanner does for a missing or
  out-of-range usWidthClass.

Query lines (`<query> => <expected>`; the expected answer is the text after the
last ` => `):

| Query | Answer |
|---|---|
| `kind <value>` | `name` or `path` for a `[fonts]` config value |
| `pick <family>` | `regular=<R> bold=<B> italic=<I> bold_italic=<BI>`, each the `file#face` of the picked face exactly as written in its face line, or `none`; or `not found` if no face has that family |
| `families mono` | the families the mono dropdown lists, joined with ` \| ` (`-` if none) |
| `families all` | every family ("Show all"), same format |

`<value>` and `<family>` are everything between the query word plus one space
and the last ` => `, taken literally: no trimming, no `~`/`%VAR%` expansion.

### Rules (these refine §8a; both apps must follow them exactly)

1. **Name or path**: a value is a *path* if it contains `/` or `\`, or ends in
   `.ttf`, `.otf` or `.ttc` in any letter case; otherwise it is a *family
   name*. (So `Fira Code 6.2` and `Inter.woff2` are names, `.ttf` and
   `fonts\Inter` are paths.) The empty value means "built-in face" (§5) and is
   not a query here.
2. **Family match**: case-insensitive in **ASCII only** (`A`–`Z` fold to
   `a`–`z`; other characters must match exactly, so every platform agrees without
   Unicode tables). Exact name only: no trimming, no prefix or fuzzy matching
   (`Inter Var` does not find `Inter Variable`). Faces whose family names differ
   only in ASCII case belong to one family.
3. **Italic**: a face is italic if its `italic` field is `1` **or** its
   subfamily contains `italic` or `oblique` (ASCII case-insensitive), e.g.
   DejaVu's `Oblique` faces with the bit clear. The resolver applies this rule
   itself, so the result is the same whether or not the scanner already did.
4. **Picking** (upright = not italic). Each style has a candidate set:
   - regular: the upright faces; target weight 400;
   - bold: the upright faces with weight ≥ 600 (none → `none`); target 700;
   - italic: the italic faces (none → `none`); target 400;
   - bold_italic: the italic faces with weight ≥ 600 (none → `none`); target 700.

   Among a style's candidates, the winner is the first by this order:
   1. **width** closest to 5 (`|width − 5|`; widths 4 and 6 tie);
   2. weight closest to the target;
   3. the lower weight;
   4. the file path in byte order (UTF-8 bytes, case-sensitive, compared as
      written);
   5. the smaller face index as a number (`#2` before `#10`).

   Width closeness is decided **per style, among that style's candidates**
   only: a family whose only faces ≥ 600 are condensed gets a condensed bold
   (the closest-width one of those, even if a wider candidate has a weight
   further from 700), while its regular stays normal width. A family with only
   condensed faces still gets them. So "Arial" picks `arial.ttf`, not Arial
   Narrow's `ARIALN.TTF` (same family name, weight 400, earlier in byte order).
   Width comes before weight even across weights: if a family's only
   normal-width upright face is a 700, that face is also its regular pick
   (over a semi-condensed 400).
   - A family with **no upright face** uses its italic face closest to 400 as
     regular (so `regular` is never `none` for a known family); its bold is
     `none`.
   - `none` is the automatic pick only. At use, a `none` style falls back to
     the family's regular face as in §5, and an explicit `prose_bold`/… value
     (name or path) overrides the pick for that style. Neither is tested here.
5. **Variable fonts**: the scanner indexes one face per file/face index, with
   the weight class of the default instance; the app loads that default
   instance only. A variable family therefore usually has `bold = none`
   (fallback to regular, no synthetic bold from the variation axes).
6. **Family lists** (`families`): one entry per family (rule 2), spelled as the
   family field of that family's *regular* pick (rule 4), sorted by the
   ASCII-lower-cased name, then by the spelling in byte order. `families mono`
   lists a family if **any** of its faces has `mono = 1` (so a family whose
   scanner flags differ between files is not hidden).
7. **Unknown family** → `not found`; the app then uses the built-in face and
   shows `font "X" not found` in the error bar.

| File | What it covers |
|---|---|
| `fontpick-basic.txt` | a 4-style family; regular+bold only; six static weights (closest to 400/700, lower-weight ties, italics at 300/500/600/800); nothing ≥ 600; no 400/700 at all; equal weights in two files (path order); a two-family `.ttc` and a `.ttc` with `#2` vs `#10`; variable fonts; italic from an `Oblique` subfamily; an italic-only family; a family spelled in two cases with one mono face; width: Arial + Arial Narrow in one family (narrow files sort first), a condensed-only family, bold existing only in condensed widths (width beats weight within the bold candidates), width 4 vs 6 tie, out-of-range width 0 → 5, a normal-width bold picked as regular over a semi-condensed 400; case-insensitive and unknown lookups; both family lists |
| `fontpick-kinds.txt` | names with spaces and dots (`JetBrains Mono`, `Fira Code 6.2`, `St. Mono`, `Inter.woff2`); `.TTF`/`.Otf`/`.ttc` suffixes in any case; paths with `/`, `\`, `~`, `%VAR%`, UNC, and a path without a font suffix |
