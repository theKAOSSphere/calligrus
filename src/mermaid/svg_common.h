// Mermaid port: SVG building blocks shared by the per-diagram writers (defined in svg_writer.cpp).
#pragma once

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "text.h"

namespace mermaid::svg {

std::string F(float v);   // Rust "{}" of an f32
std::string F2(float v);  // Rust "{:.2}"
std::string F3(float v);  // Rust "{:.3}"
std::string escape_xml(const std::string& in);
std::string normalize_font_family(const std::string& font_family);

// The root <svg> tag, the background rect and the marker <defs> that render_svg_with_dimensions writes
// before any diagram-specific content. `percent_width`: width="100%" plus a max-width style and no height
// attribute (what the crate does for pie / mindmap / gitgraph / c4 when use_max_width is set).
std::string document_start(float width, float height, float vx, float vy, float vw, float vh, bool percent_width,
                           bool has_links, const Theme& theme, bool with_defs = true,
                           const std::vector<std::string>* extra_stroke_colors = nullptr);

// render.rs points_to_path / LabelRect / edge_label_background_visible (centre labels) for writers outside svg_writer.cpp.
std::string points_to_path(const std::vector<std::pair<float, float>>& points);
std::array<float, 4> label_rect_from_center(float cx, float cy, float lw, float lh, float pad_x, float pad_y);  // x, y, w, h
bool label_background_visible(DiagramKind kind, const std::vector<std::pair<float, float>>& points, const std::array<float, 4>& rect);

// The <defs> block with the arrow markers: one pair per colour (render_svg_with_dimensions; document_start uses the
// theme line colour only). Pass with_defs=false to document_start when you add this yourself.
std::string marker_defs(const std::vector<std::string>& colors);

// render.rs text_block_svg_with_font_size / _weight.
std::string text_block(float x, float y, const TextBlock& label, const Theme& theme, const LayoutConfig& config, float font_size,
                       const std::string& anchor, const std::optional<std::string>& override_color, bool baseline,
                       const std::optional<std::string>& font_weight = std::nullopt);
// render.rs text_line_svg_with_font_size
std::string text_line(float x, float y, const std::string& text, const Theme& theme, float font_size, const std::string& fill,
                      const std::string& anchor);

}  // namespace mermaid::svg
