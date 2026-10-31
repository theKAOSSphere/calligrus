// Mermaid port: SVG text -> white-ink bitmap, the job resvg + ink() do in the reference implementation.
//
// Only the SVG subset the diagram writer produces is drawn (rect, circle, ellipse, line, polyline,
// polygon, path, text/tspan, g, a; fill/stroke colours, opacity, dashes, caps and joins,
// transforms). Anything else throws mermaid::Unsupported, so the app can fall back to the Rust
// renderer instead of drawing something wrong.
//
// Geometry is filled with the tiny-skia port in src/texmath/canvas.cpp (the same scan converter
// resvg uses); strokes are expanded to polygons here; text is shaped and outlined with HarfBuzz.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "textface.h"

namespace mermaid {

// White ink on transparent: premultiplied RGBA8, all four channels equal (what ink() returns).
struct InkImage {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

// The faces text may use (pointers stay owned by the caller). bold / italic may be null: the
// regular face is used then.
struct FontSet {
    const TextFace* regular = nullptr;
    const TextFace* bold = nullptr;
    const TextFace* italic = nullptr;
};

// `scale` = output pixels per SVG unit. Throws Unsupported (and ParseError for broken SVG).
InkImage rasterize_svg(const std::string& svg, float scale, const FontSet& fonts);

}  // namespace mermaid
