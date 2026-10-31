// Mermaid port: SVG writer (render.rs, flowchart path) and the app-level helpers.
#pragma once

#include <string>

#include "config.h"
#include "layout_types.h"

namespace mermaid {

// render_svg(): the diagram as SVG text, byte-identical to the Rust crate's output.
std::string render_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config);

// fix_svg() of the reference implementation: post-processing the app applies to the crate's SVG.
std::string fix_svg(const std::string& svg);

// The whole text -> SVG pipeline the app uses (parse, layout, render, fix_svg) with the ink
// theme and default layout config. Throws ParseError / Unsupported.
std::string render_mermaid_svg(const std::string& source);

}  // namespace mermaid
