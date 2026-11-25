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
// `keep_colour` (optional): set to whether this kind of diagram is drawn in its own colours (a sankey: its nodes
// and flows are told apart by colour) instead of as a one-colour ink mask. mermaid-rs-renderer folds every colour into
// the mask; Calligrus keeps the colours of the kinds listed in render.cpp (see PORTING.md, "Deviations").
std::string render_mermaid_svg(const std::string& source, bool* keep_colour = nullptr);

}  // namespace mermaid
