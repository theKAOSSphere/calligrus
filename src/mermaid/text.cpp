// Mermaid port of layout/text.rs (see text.h).
#include "text.h"

#include <algorithm>

#include "fontmetrics.h"
#include "ir.h"
#include "strutil.h"

namespace mermaid {

float measure_text_width(const std::string& text, float font_size) {
    if (text.empty() || font_size <= 0.0f) return 0.0f;
    const FontMetrics* font = measure_font();
    if (!font) throw Unsupported("no measuring font set");
    // TextMeasurer::measure turns tabs into four spaces first.
    if (text.find('\t') != std::string::npos) return font->measure_width(replace_all(text, "\t", "    "), font_size);
    return font->measure_width(text, font_size);
}

namespace {

// text_width(): the glyph-advance path (fast_text_metrics is off in the app).
float text_width(const std::string& text, float font_size) { return measure_text_width(text, font_size); }

// average_char_width(): width of a pangram-ish sample divided by its length.
float average_char_width(float font_size) {
    if (font_size <= 0.0f) return font_size * 0.56f;
    const std::string sample = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    float width = measure_text_width(sample, font_size);
    float count = static_cast<float>(std::max<size_t>(sample.size(), 1));
    return width / count;
}

float max_label_width_px(size_t max_chars, float font_size) {
    float avg_char = average_char_width(font_size);
    return static_cast<float>(std::max<size_t>(max_chars, 1)) * avg_char;
}

std::vector<std::string> wrap_line(const std::string& line, float max_width, float font_size) {
    if (text_width(line, font_size) <= max_width) return {line};
    std::vector<std::string> lines;
    std::string current;
    for (const std::string& word : split_whitespace(line)) {
        std::string candidate = current.empty() ? word : current + " " + word;
        if (text_width(candidate, font_size) > max_width) {
            if (!current.empty()) {
                lines.push_back(current);
                current.clear();
            }
            current = word;
        } else {
            current = candidate;
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

TextBlock measure_label_with_max_width(const std::string& text, float font_size, float max_width,
                                       const LayoutConfig& config, bool wrap) {
    std::vector<std::string> raw_lines = split_lines(text);
    std::vector<std::string> lines;
    max_width = std::max(max_width, 1.0f);
    for (const std::string& line : raw_lines) {
        if (wrap) {
            auto wrapped = wrap_line(line, max_width, font_size);
            lines.insert(lines.end(), wrapped.begin(), wrapped.end());
        } else {
            lines.push_back(line);
        }
    }
    if (lines.empty()) lines.push_back(std::string());

    size_t max_len = 1;
    {
        size_t m = 0;
        for (const auto& l : lines) m = std::max(m, char_count(l));
        max_len = m;  // unwrap_or(1) only applies to an empty iterator, impossible here
    }
    float widest = 0.0f;
    for (const auto& l : lines) widest = std::max(widest, text_width(l, font_size));
    float avg_char = average_char_width(font_size);
    float guard_width = static_cast<float>(max_len) * avg_char;
    TextBlock block;
    block.width = std::max(widest, guard_width);
    block.height = static_cast<float>(lines.size()) * font_size * config.label_line_height;
    block.lines = std::move(lines);
    return block;
}

}  // namespace

// Mermaid sources may contain <br/>, <br> or a literal backslash-n as line breaks.
std::vector<std::string> split_lines(const std::string& text) {
    std::string current = replace_all(replace_all(normalize_display_math(text), "<br/>", "\n"), "<br>", "\n");
    current = replace_all(current, "\\n", "\n");
    std::vector<std::string> lines;
    size_t pos = 0;
    while (true) {
        size_t j = current.find('\n', pos);
        if (j == std::string::npos) {
            lines.push_back(trim(std::string_view(current).substr(pos)));
            break;
        }
        lines.push_back(trim(std::string_view(current).substr(pos, j - pos)));
        pos = j + 1;
    }
    return lines;
}

TextBlock measure_label_with_max_width_px(const std::string& text, float font_size, float max_width, const LayoutConfig& config,
                                          bool wrap) {
    return measure_label_with_max_width(text, font_size, max_width, config, wrap);
}

TextBlock measure_label_with_font_size(const std::string& text, float font_size, const LayoutConfig& config,
                                       bool wrap) {
    float max_width_px = max_label_width_px(config.max_label_width_chars, font_size);
    return measure_label_with_max_width(text, font_size, max_width_px, config, wrap);
}

TextBlock measure_label(const std::string& text, const Theme& theme, const LayoutConfig& config) {
    // Mermaid sizes labels from a baseline of about 16px even when the theme font is smaller.
    float measure_font_size = std::max(theme.font_size, 16.0f);
    return measure_label_with_font_size(text, measure_font_size, config, true);
}

}  // namespace mermaid
