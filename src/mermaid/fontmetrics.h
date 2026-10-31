// Mermaid port: reads glyph advances from a TrueType/OpenType font file (cmap + hmtx), the way
// the Rust crate's text_metrics.rs does through ttf-parser, and measures text widths exactly
// like `FontFace::measure_width` there (same f32 arithmetic, same fallbacks).
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace mermaid {

class FontMetrics {
public:
    // Parse a font (single font file; a .ttc collection uses face 0). False if the tables we
    // need are missing.
    bool load(std::vector<uint8_t> data);
    bool loaded() const { return units_per_em_ != 0; }

    // Width in the same units as font_size, like text_metrics::measure_text_width.
    float measure_width(std::string_view utf8, float font_size) const;

private:
    uint16_t glyph_index(char32_t cp) const;  // via the cmap; 0 = no glyph
    uint16_t advance(uint16_t glyph) const;

    std::vector<uint8_t> data_;
    uint16_t units_per_em_ = 0;
    uint16_t num_hmetrics_ = 0;
    uint32_t hmtx_off_ = 0, hmtx_len_ = 0;
    struct CmapSub {
        uint16_t format;
        uint32_t offset;  // absolute offset of the subtable
    };
    std::vector<CmapSub> cmaps_;
    uint16_t ascii_adv_[128] = {};
};

// The font used for all measuring (set once by the app or test; the Rust crate has a global too).
void set_measure_font(const FontMetrics* font);
const FontMetrics* measure_font();

}  // namespace mermaid
