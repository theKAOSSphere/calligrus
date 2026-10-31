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

## Licences

The renderer is a port of mermaid-rs-renderer (MIT); the rasteriser's fill and blend code is a port of tiny-skia
(BSD-3-Clause) and its text placement follows usvg / resvg behaviour (resvg is MPL-2.0; no resvg source is copied, it
is only the behaviour the output was compared against). See `THIRD-PARTY.md`.
