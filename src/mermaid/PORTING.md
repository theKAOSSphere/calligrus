# The Mermaid renderer

`src/mermaid/` is a C++ port of the Rust crate [mermaid-rs-renderer](https://github.com/1jehuang/mermaid-rs-renderer)
0.3.1 (MIT). Every diagram in a document is parsed, laid out and written as SVG by this code, then rasterised
to an "ink" mask by `svg_raster.cpp` (the app draws the mask in the text colour). Nothing is sent anywhere and no
browser is involved.

The goal of the port is **byte-identical SVG** to what the crate writes, because its router and its
score-driven layout passes are heuristic: any change in the order of a floating-point operation flips a tie and
changes a path. That is why the target is built with `/fp:strict` (MSVC) or `-ffp-contract=off` (GCC / Clang).

## Pipeline

```
parse (parser.cpp, parser_graphs.cpp, kind_*.cpp)      -> Graph (ir.h)
layout (layout.cpp, ranking, routing, router, path_cleanup, edge_pipeline, label_placement*, subgraphs, sequence.cpp,
        kind_*.cpp)                                    -> Layout
write SVG (svg_writer.cpp, render.cpp, kind_*.cpp)     -> string
rasterise (svg_raster.cpp, textface.cpp)               -> ink mask (native.cpp is the entry point the app calls)
```

- One file per diagram kind with its own layout and writer: `kind_<name>.cpp`, registered in `cmake/mermaid.cmake`,
  `kinds.h`, `kinds.cpp` and `parser.cpp` (`addkind.py Name lower` does the registration for a new kind).
  Flowchart, class, state, ER and requirement diagrams share the flowchart layout.
- Text is measured with the bundled IBM Plex Sans (`fontmetrics.*`, HarfBuzz shaping in `textface.cpp`), so layout
  is the same on every machine. (The crate itself measures with whatever system font it finds; the oracle below
  seeds its font cache with Plex to get reproducible goldens.)
- Number formatting mimics Rust: `{:.2}` is `%.2f`, and f32 `Display` (shortest round-trip digits) is
  `std::to_chars(float)` (`numfmt.h`, `fmt_shortest`).
- A construct the port does not handle throws `mermaid::Unsupported` and the diagram gets no picture.

## Test oracle and corpus

`tests/mermaid/` holds Mermaid inputs (`*.mmd`) with goldens made by the Rust crate: `X.svg` (the SVG the crate writes,
after the app's `fix_svg` post-processing) and `X.layout.txt` (a text dump of node boxes, edge polylines and subgraph
boxes). The goldens come from `scripts/mermaid-oracle` (its own Cargo project; needs a Rust toolchain only to
regenerate them): `cargo build --release` there, then `target/release/mermaid-oracle tests/mermaid/*.mmd` rewrites each
`X.svg` next to its `X.mmd`.

`calligrus-mermaid-test` compares our output with the goldens at three levels per file: NODES (sizes and relative
positions), LAYOUT (text-identical layout dump) and SVG (byte-identical, numbers within 0.01). Inputs that the crate
rejects are expected to give a parse error here too. `tests/mermaid-plex/` is reserved for inputs whose crate output
depends on a system font (ER key badges): not reproducible, so they have no golden.

Environment switches used while developing: `CALLIGRUS_MERMAID_LOG=1` prints why a diagram was not drawn;
`CALLIGRUS_MERMAID_MASKS=<dir>` (with `CALLIGRUS_MERMAID_SCALE`, `CALLIGRUS_MERMAID_DUMP=<dir>`) makes the test
compare the rasteriser's masks with masks the oracle wrote with `mermaid-oracle --masks <dir> <scale> files...`
(about 0.0 to 0.4 % of pixels differ by more than 32/255: anti-aliasing on rounded corners and dashes).

## Rasteriser

`svg_raster.cpp` draws the SVG subset the writers produce: rect, circle, ellipse, line, polyline, polygon, path (with
arcs), text, g, a, markers, fill and stroke colours and opacity, dashes, caps, joins, transforms, viewBox, linear
gradients on strokes, mix-blend-mode as layers, `<image>` PNG decoding (switched off by `kDrawImages`). It has its own
XML reader and its own stroker and dasher; fills go through the tiny-skia port in `src/texmath/canvas.cpp` (including
tiny-skia's 8-bit "lowp" blend). Text uses `textface.cpp` (HarfBuzz for shaping and glyph outlines). Constructs
outside this subset (radial gradients, filters, `<use>`, evenodd fills outside the C4 database symbol) throw
`Unsupported`.

## Deviations from mermaid-rs-renderer

Everything above is byte parity with the crate. These are the places where Calligrus deliberately differs (bugs of the crate or
things it cannot do); `calligrus-mermaid-test` checks each one in `run_deviation_tests()`, and the corpus files that the crate
rejects but we draw are listed in `kRendersWhereRustRejects`.

- **`<<annotation>>` lines in a class body** (`class R { <<interface>> ... }`, and `<<enumeration>>`, `<<service>>`...). The crate's
  validator takes any line that starts with `<<` for an arrow without a source node and rejects the whole diagram ("unexpected
  token '<<interface>>'"). Mermaid takes it, so `starts_with_arrow` (parser.cpp) lets `<<` followed by a letter through; the class
  parser already handled stereotypes. The result equals the crate's output for `R : <<interface>>`.
- **Sankey keeps its colours.** The crate folds every picture into a white-ink mask (`ink()`: alpha minus luminance), so a
  sankey's palette fills and gradient flows turn into grey. For the kinds `render_mermaid_svg` reports through `keep_colour`
  (the sankey) `rasterize_svg` draws chromatic paints (any colour that is not a pure grey, and every gradient) into a second,
  transparent canvas and returns it as `InkImage::colour`; black text and lines stay in the ink mask. The app (math.cpp
  `diagram_draw`) draws the colour layer as it is and the mask, tinted with the text colour, over it (the flows are "multiply" over the labels in the crate, which keeps black text black).
- **Sankey layout like mermaid.js (d3-sankey).** The crate's layout never separates the nodes of a column (every node in the
  first column sits at y = 0, later ones at the top of their first incoming link, so they overlap or touch), centres labels on
  the node's rank rather than beside the node, and places an incoming band at "source's link top minus target top", so a band
  can miss its target. `compute_sankey_layout` (kind_sankey.cpp) instead does what d3-sankey does: depth from the longest path
  with sinks pushed to the last column ("justify"), node height = max(in, out) * one scale for all columns, 24 px padding
  between the nodes of a column (the diagram grows taller when a column would not fit), six relaxation passes (each node moves
  toward the value-weighted centre of its neighbours, then collisions are resolved), and link bands stacked from the node's top
  at both ends in the order of the node at the other end. Labels sit beside their node, centred on it: left of the last
  column, right of all others. Because the geometry differs, the layout text and SVG goldens of the sankey corpus
  (`sankey-torture`, `sankey-t-basic`, `exp-sankey-energy`, `bench-sankey-medium`) are no longer compared (`kSankeyDeviates` in
  mermaid_test.cpp); `run_deviation_tests()` checks the layout's invariants on them instead (padding, bands inside nodes,
  heights = flow sums). Parsing, palette and the SVG writer's structure are unchanged.
- **Maths in labels.** The crate turns `$$...$$` into a plain-text approximation (`x^2` becomes "x²", `rac{a}{b}` "(a)/(b)").
  When the app registers its TeX engine (`set_math_hooks`, math.cpp) a label line that is exactly one `$$...$$` the engine can
  typeset is measured from the real formula (display style) and drawn as a bitmap in the label colour: `normalize_display_math`
  keeps it as a private-use marker line, the writer emits the plain text plus `data-tex="..."` on the `<tspan>`, and the
  rasteriser draws the bitmap instead of the glyphs. Formulas the engine rejects, and formulas inside a sentence, keep the plain
  approximation, and without hooks (the console test) the output is the crate's byte for byte. Taller formulas make their node
  taller (`TextBlock::height`); lines of a multi-line label are still spaced evenly.

## Licences

The renderer is a port of mermaid-rs-renderer (MIT); the rasteriser's fill and blend code is a port of tiny-skia
(BSD-3-Clause) and its text placement follows usvg / resvg behaviour (resvg is MPL-2.0; no resvg source is copied, it
is only the behaviour the output was compared against). See `THIRD-PARTY.md`.
