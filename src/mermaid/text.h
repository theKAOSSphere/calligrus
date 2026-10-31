// Mermaid port of layout/text.rs: measuring labels (line splitting, word wrap, block size).
#pragma once

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
std::string normalize_display_math(const std::string& text);
std::vector<std::string> split_lines(const std::string& text);

}  // namespace mermaid
