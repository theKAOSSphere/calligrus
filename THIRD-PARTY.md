# Third-party software and data

Calligrus itself is licensed under the GNU General Public License, version 3 or (at your option) any later
version (GPL-3.0-or-later, see `LICENSE`). It bundles or links the following. Everything below is either
linked into the executable, embedded in it, or shipped in this repository.

## C++ libraries (built with vcpkg, see `vcpkg.json`)

| Library | Used for | Licence | Upstream |
|---|---|---|---|
| Dear ImGui 1.92 | the user interface | MIT | https://github.com/ocornut/imgui |
| GLFW 3 | window, input, OpenGL context | Zlib | https://github.com/glfw/glfw |
| md4c | Markdown parser | MIT | https://github.com/mity/md4c |
| stb (`stb_image`, `stb_image_write`) | image decoding (PNG, JPEG, GIF, BMP), PNG output | MIT or public domain (your choice) | https://github.com/nothings/stb |
| libwebp | WebP images | BSD-3-Clause | https://chromium.googlesource.com/webm/libwebp |
| toml++ | reading and parsing config and theme files | MIT | https://github.com/marzer/tomlplusplus |
| zlib | Flate streams in PDF export | Zlib | https://github.com/madler/zlib |
| HarfBuzz | text shaping, font subsetting for PDF export | "Old MIT" (MIT-style) | https://github.com/harfbuzz/harfbuzz |
| FreeType | glyph rasterisation (through ImGui), code ligatures | FreeType Licence (FTL) or GPL-2.0-or-later; we use the FTL | https://freetype.org |
| libpng | PNG support for FreeType | libpng licence (PNG Reference Library License version 2) | http://www.libpng.org/pub/png/libpng.html |
| Brotli | WOFF2 / compressed font support for FreeType | MIT | https://github.com/google/brotli |
| bzip2 | compressed font support for FreeType | bzip2 licence (BSD-style) | https://sourceware.org/bzip2/ |

The licence texts are in each package's `share/<port>/copyright` file inside the vcpkg installation that built the
executable. A build that links these statically (the Windows release does) contains their code.

## Ported code

- **Mermaid renderer** (`src/mermaid/`): a C++ port of [mermaid-rs-renderer](https://github.com/1jehuang/mermaid-rs-renderer)
  0.3.1 by Jehuang, MIT licence. The test goldens in `tests/mermaid/` are output of that crate (made with
  `scripts/mermaid-oracle`, which depends on it and on resvg 0.47). See `src/mermaid/PORTING.md`.
- **Rasteriser** (`src/texmath/canvas.cpp`, also used by `src/mermaid/svg_raster.cpp`): a port of the code paths of
  [tiny-skia](https://github.com/RazrFalcon/tiny-skia) (anti-aliased path filling, hairlines, butt-cap strokes,
  8-bit "lowp" blending) by Yevhenii Reizner, BSD-3-Clause, itself a port of parts of Skia (Google, BSD-3-Clause).
- **OpenType reading** (`src/texmath/font.cpp`, `cff.cpp`): written after the behaviour of
  [ttf-parser](https://github.com/RazrFalcon/ttf-parser) (MIT or Apache-2.0) so outlines and metrics agree.
- **Text-width classification** (`src/mermaid/unicode_width.h`) follows `unicode_width.rs` of mermaid-rs-renderer (MIT).
- The SVG text placement in `svg_raster.cpp` reproduces the behaviour of [resvg](https://github.com/linebender/resvg)
  / usvg (MPL-2.0) for the output we compare against; no resvg source code is included.

## Syntax highlighting (tree-sitter)

The tree-sitter C runtime (`grammars/runtime/`, version 0.27.0, MIT, Copyright (c) 2018 Max Brunsfeld,
https://github.com/tree-sitter/tree-sitter) and these grammars, each with its licence file in `grammars/<lang>/LICENSE`
(commit hashes in `grammars/README.md`):

| Language | Upstream | Licence |
|---|---|---|
| Rust | https://github.com/tree-sitter/tree-sitter-rust | MIT |
| C | https://github.com/tree-sitter/tree-sitter-c | MIT |
| C++ | https://github.com/tree-sitter/tree-sitter-cpp | MIT |
| Python | https://github.com/tree-sitter/tree-sitter-python | MIT |
| JavaScript | https://github.com/tree-sitter/tree-sitter-javascript | MIT |
| TypeScript, TSX | https://github.com/tree-sitter/tree-sitter-typescript | MIT |
| JSON | https://github.com/tree-sitter/tree-sitter-json | MIT |
| TOML | https://github.com/tree-sitter-grammars/tree-sitter-toml | MIT |
| YAML | https://github.com/tree-sitter-grammars/tree-sitter-yaml | MIT |
| Bash | https://github.com/tree-sitter/tree-sitter-bash | MIT |
| PowerShell | https://github.com/airbus-cert/tree-sitter-powershell | MIT |
| Lua | https://github.com/tree-sitter-grammars/tree-sitter-lua | MIT |
| Emacs Lisp | https://github.com/Wilfred/tree-sitter-elisp | MIT |
| Markdown (block grammar) | https://github.com/tree-sitter-grammars/tree-sitter-markdown | MIT |
| LaTeX | https://github.com/latex-lsp/tree-sitter-latex | MIT |
| CMake | https://github.com/uyha/tree-sitter-cmake | MIT |
| Go | https://github.com/tree-sitter/tree-sitter-go | MIT |
| HTML | https://github.com/tree-sitter/tree-sitter-html | MIT |
| CSS | https://github.com/tree-sitter/tree-sitter-css | MIT |
| AutoHotkey v2 | https://github.com/holy-tao/tree-sitter-autohotkey | Unlicense |

The highlight queries (`grammars/<lang>/highlights.scm`) are the grammars' upstream queries, merged and adjusted by
`grammars/vendor.py` (every change is documented there). The LaTeX query is the one from Helix
(`runtime/queries/latex/highlights.scm`, https://github.com/helix-editor/helix) and is licensed **MPL-2.0**; its licence
is `grammars/latex/LICENSE-highlights-helix`. That file's terms apply to `grammars/latex/highlights.scm` only.

## Fonts (embedded in the executable; licences in `assets/fonts/`)

| Font | Licence | Licence file | Upstream |
|---|---|---|---|
| IBM Plex Sans (Regular, Bold, Italic, Bold Italic) | SIL OFL 1.1 | `IBMPlex-LICENSE.txt` | https://github.com/IBM/plex |
| Cascadia Code (Regular) | SIL OFL 1.1 | `Cascadia-LICENSE.txt` | https://github.com/microsoft/cascadia-code |
| Hack (Regular) | MIT, with the Bitstream Vera licence for the DejaVu / Vera-derived glyphs | `Hack-LICENSE.txt` | https://github.com/source-foundry/Hack |
| Noto Sans Symbols 2 (Regular) | SIL OFL 1.1 | `NotoSansSymbols2-LICENSE.txt`, `OFL.txt` | https://github.com/notofonts/symbols |
| Noto Emoji (Regular) | SIL OFL 1.1 | `OFL.txt` | https://github.com/googlefonts/noto-emoji |
| Latin Modern Math | GUST Font License (LPPL-based) | `LatinModernMath-LICENSE.txt` | https://www.gust.org.pl/projects/e-foundry/lm-math |
| Ubuntu (Light) | Ubuntu Font Licence 1.0 | `UFL.txt` | https://design.ubuntu.com/font |
| emoji-icon-font | MIT | `emoji-icon-font-mit-license.txt` | https://github.com/jslegers/emoji-icon-font |

The test font `spec/tests/fonts/NotoSansDevanagari-Regular.ttf` (SIL OFL 1.1, https://github.com/notofonts/devanagari)
is used by tests only and is not part of the executable; see `spec/tests/fonts/README.md`.

## Themes (`themes/*.toml`, embedded in the executable)

The theme files are colour palettes written for this program, each credited to its source in the comment at the top of
the file. No theme source code is copied, only colour values.

| Theme | Source | Source licence |
|---|---|---|
| bearded-vivid-black | Bearded Theme, Vivid Black, by BeardedBear: https://github.com/BeardedBear/bearded-theme | GPL-3.0 (palette only, see above) |
| catppuccin-mocha | Catppuccin: https://catppuccin.com, https://github.com/catppuccin/palette, https://github.com/catppuccin/nvim | MIT |
| dracula | Dracula by Zeno Rocha and contributors: https://draculatheme.com, https://github.com/dracula/visual-studio-code | MIT |
| github-dark, github-light | GitHub Primer VS Code theme: https://github.com/primer/github-vscode-theme | MIT |
| gruvbox-dark | gruvbox by Pavel Pertsev: https://github.com/morhetz/gruvbox | MIT |
| imgui-dark | Dear ImGui `StyleColorsDark()`: https://github.com/ocornut/imgui | MIT |
| leuven | Emacs leuven-theme (palette adapted): https://github.com/fniessen/emacs-leuven-theme | GPL-3.0 (palette only, see above) |
| nord | Nord by Arctic Ice Studio and Sven Greb: https://www.nordtheme.com, https://github.com/nordtheme/visual-studio-code | MIT |
| solarized-dark, solarized-light | Solarized by Ethan Schoonover: https://ethanschoonover.com/solarized | MIT |
| tokyo-night | Tokyo Night by enkia (https://github.com/enkia/tokyo-night-vscode-theme, MIT) in folke's port https://github.com/folke/tokyonight.nvim | Apache-2.0 / MIT |
| calligrus-dark, calligrus-light, print-dark | this project | GPL-3.0-or-later |

Bearded Theme and leuven-theme are GPL-3 projects, compatible with Calligrus's own licence; the attribution is kept in
each theme file.

## Editor plugins and tools

`editors/emacs` and `editors/nvim` and `scripts/` are part of this project (GPL-3.0-or-later). `scripts/mermaid-oracle` is a
developer tool that is not built with the app; it depends on the crates mermaid-rs-renderer (MIT) and resvg (MPL-2.0).
