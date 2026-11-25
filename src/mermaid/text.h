// Mermaid port of layout/text.rs: measuring labels (line splitting, word wrap, block size).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "config.h"

namespace mermaid {

struct TextBlock {
    std::vector<std::string> lines;
    float width = 0.0f;
    float height = 0.0f;
};

// text_metrics::measure_text_width (empty text or font_size <= 0 gives 0). Needs measure_font().
float measure_text_width(const std::string& text, float font_size);

// layout::measure_label: font size is max(theme.font_size, 16) as in the Rust code.
TextBlock measure_label(const std::string& text, const Theme& theme, const LayoutConfig& config);
TextBlock measure_label_with_font_size(const std::string& text, float font_size, const LayoutConfig& config,
                                       bool wrap);
// layout::text::measure_label_with_max_width (explicit wrap width in pixels).
TextBlock measure_label_with_max_width_px(const std::string& text, float font_size, float max_width, const LayoutConfig& config,
                                          bool wrap);
// layout/text.rs normalize_display_math: "$$...$$" spans become plain text (math_label.cpp).
// Deviation from mermaid-rs-renderer (which only ever draws the plain-text approximation): when the app has registered a
// TeX engine (set_math_hooks) and a label line is nothing but one `$$...$$` span the engine can parse, the line is kept as
// a "math marker" (kMathOpen + tex + kMathClose) instead, so the layout sizes it from the real formula and the SVG writer
// can ask the rasteriser to draw it. escape_xml() and the width measurer turn a marker back into the plain text, so every
// other consumer of a label line still sees the reference's approximation.
std::string normalize_display_math(const std::string& text);

// The TeX engine behind math labels (src/math.cpp sets it; the console tests leave it unset = reference behaviour).
struct MathBox {
    float width = 0, ascent = 0, descent = 0;  // the formula's box in the same units as the font size
};
struct MathBitmap {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;     // premultiplied RGBA8, white ink
    float origin_x = 0, origin_y = 0;  // the formula's baseline-left corner inside the bitmap, in pixels
};
struct MathHooks {
    // Box of `tex` at `font_size` units per em; nullopt when it cannot be typeset (the plain text stays).
    std::function<std::optional<MathBox>(const std::string& tex, float font_size)> measure;
    // The formula drawn at `ppem` pixels per em; false when it cannot be drawn (the plain text is drawn instead).
    std::function<bool(const std::string& tex, float ppem, MathBitmap& out)> render;
};
void set_math_hooks(MathHooks hooks);  // an empty MathHooks switches TeX labels off
const MathHooks& math_hooks();
bool math_hooks_active();

inline constexpr const char* kMathOpen = "\xEE\x80\x80";   // U+E000 (private use)
inline constexpr const char* kMathClose = "\xEE\x80\x81";  // U+E001
// A line made of one math marker: its TeX source in `tex` (if given).
bool is_math_marker_line(const std::string& line, std::string* tex = nullptr);
// The plain-text approximation of a line (a marker becomes render_plain_math of its source; other text is unchanged).
std::string math_marker_plain(const std::string& line);
// Height of a math line beyond a normal text line (0 for text), for the block height.
float math_line_extra_height(const std::string& line, float font_size, float line_height);
std::vector<std::string> split_lines(const std::string& text);

}  // namespace mermaid
