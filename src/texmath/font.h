// A minimal OpenType reader for the bundled Latin Modern Math font: just the
// parts the TeX engine needs (character map, advances, CFF glyph outlines and
// the OpenType MATH table).
//
// The Rust engine uses the `ttf-parser` crate; this is a port of the pieces of
// it we use, kept faithful on purpose (same float outline coordinates, same
// bounding-box rule, same MATH lookups) so both engines lay out formulas to
// the same numbers. FreeType doesn't read the MATH table, and HarfBuzz's MATH
// API rounds kerning heights to integers, so neither gives exact parity.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace texmath {

// A glyph's bounding box in font units (like ttf-parser's `Rect`).
struct GlyphRect {
    int16_t x_min, y_min, x_max, y_max;
};

// Receives a glyph outline (font units, y up), like ttf-parser's OutlineBuilder.
class OutlineSink {
public:
    virtual ~OutlineSink() = default;
    virtual void move_to(float x, float y) = 0;
    virtual void line_to(float x, float y) = 0;
    virtual void curve_to(float x1, float y1, float x2, float y2, float x, float y) = 0;
    virtual void close() = 0;
};

// ---------------------------------------------------------------------------
// OpenType MATH table pieces (font units). Device tables are ignored, as in
// the Rust engine (which only ever reads `MathValue::value`).
// ---------------------------------------------------------------------------

// MathConstants: the global numbers that drive TeX's Appendix G rules.
struct MathConstants {
    int16_t script_percent_scale_down = 0;
    int16_t script_script_percent_scale_down = 0;
    uint16_t delimited_sub_formula_min_height = 0;
    uint16_t display_operator_min_height = 0;
    int16_t math_leading = 0;
    int16_t axis_height = 0;
    int16_t accent_base_height = 0;
    int16_t flattened_accent_base_height = 0;
    int16_t subscript_shift_down = 0;
    int16_t subscript_top_max = 0;
    int16_t subscript_baseline_drop_min = 0;
    int16_t superscript_shift_up = 0;
    int16_t superscript_shift_up_cramped = 0;
    int16_t superscript_bottom_min = 0;
    int16_t superscript_baseline_drop_max = 0;
    int16_t sub_superscript_gap_min = 0;
    int16_t superscript_bottom_max_with_subscript = 0;
    int16_t space_after_script = 0;
    int16_t upper_limit_gap_min = 0;
    int16_t upper_limit_baseline_rise_min = 0;
    int16_t lower_limit_gap_min = 0;
    int16_t lower_limit_baseline_drop_min = 0;
    int16_t stack_top_shift_up = 0;
    int16_t stack_top_display_style_shift_up = 0;
    int16_t stack_bottom_shift_down = 0;
    int16_t stack_bottom_display_style_shift_down = 0;
    int16_t stack_gap_min = 0;
    int16_t stack_display_style_gap_min = 0;
    int16_t stretch_stack_top_shift_up = 0;
    int16_t stretch_stack_bottom_shift_down = 0;
    int16_t stretch_stack_gap_above_min = 0;
    int16_t stretch_stack_gap_below_min = 0;
    int16_t fraction_numerator_shift_up = 0;
    int16_t fraction_numerator_display_style_shift_up = 0;
    int16_t fraction_denominator_shift_down = 0;
    int16_t fraction_denominator_display_style_shift_down = 0;
    int16_t fraction_numerator_gap_min = 0;
    int16_t fraction_num_display_style_gap_min = 0;
    int16_t fraction_rule_thickness = 0;
    int16_t fraction_denominator_gap_min = 0;
    int16_t fraction_denom_display_style_gap_min = 0;
    int16_t skewed_fraction_horizontal_gap = 0;
    int16_t skewed_fraction_vertical_gap = 0;
    int16_t overbar_vertical_gap = 0;
    int16_t overbar_rule_thickness = 0;
    int16_t overbar_extra_ascender = 0;
    int16_t underbar_vertical_gap = 0;
    int16_t underbar_rule_thickness = 0;
    int16_t underbar_extra_descender = 0;
    int16_t radical_vertical_gap = 0;
    int16_t radical_display_style_vertical_gap = 0;
    int16_t radical_rule_thickness = 0;
    int16_t radical_extra_ascender = 0;
    int16_t radical_kern_before_degree = 0;
    int16_t radical_kern_after_degree = 0;
    int16_t radical_degree_bottom_raise_percent = 0;
};

// A MathKern table: `heights.size()` correction heights and one more kern
// value than heights (the kern applies between consecutive heights).
struct MathKern {
    std::vector<int16_t> heights;
    std::vector<int16_t> kerns;
};

// MathKernInfo for one glyph: cut-ins at its four corners.
struct KernInfo {
    std::optional<MathKern> top_right, top_left, bottom_right, bottom_left;
};

// One piece of a glyph assembly (e.g. the top hook, the straight extender
// and the bottom hook of a tall parenthesis).
struct GlyphPart {
    uint16_t glyph_id;
    uint16_t start_connector_length;
    uint16_t end_connector_length;
    uint16_t full_advance;
    uint16_t part_flags;
    bool extender() const { return (part_flags & 1) != 0; }  // may be repeated
};

// A ready-made bigger size of a glyph.
struct GlyphVariant {
    uint16_t variant_glyph;
    uint16_t advance_measurement;
};

// How to make a glyph bigger: pre-drawn variants, then an assembly.
struct GlyphConstruction {
    std::optional<std::vector<GlyphPart>> assembly;
    std::vector<GlyphVariant> variants;
};

class Font {
public:
    // Parse the font. Returns false (with a reason) if it's unusable for math.
    bool load(std::vector<uint8_t> bytes, std::string* error);

    uint16_t units_per_em() const { return upem_; }

    // ---- glyph basics (ttf-parser's Face methods) ----
    std::optional<uint16_t> glyph_index(char32_t c) const;
    std::optional<uint16_t> glyph_hor_advance(uint16_t gid) const;
    // Tight box of the outline's points (control points included, truncated
    // to integers), like ttf-parser. nullopt for empty glyphs.
    std::optional<GlyphRect> glyph_bounding_box(uint16_t gid) const;
    // Stream the CFF outline into `sink`; returns the box, or nullopt if the
    // glyph has no outline (the sink may still have received partial calls).
    std::optional<GlyphRect> outline_glyph(uint16_t gid, OutlineSink& sink) const;

    // ---- MATH table ----
    const MathConstants& math_constants() const { return consts_; }
    bool has_math_glyph_info() const { return glyph_info_ != 0; }
    bool has_math_variants() const { return variants_ != 0; }
    std::optional<int16_t> italic_correction(uint16_t gid) const;
    std::optional<int16_t> top_accent_attachment(uint16_t gid) const;
    std::optional<KernInfo> kern_info(uint16_t gid) const;
    uint16_t min_connector_overlap() const { return min_connector_overlap_; }
    std::optional<GlyphConstruction> vertical_construction(uint16_t gid) const;
    std::optional<GlyphConstruction> horizontal_construction(uint16_t gid) const;

private:
    std::vector<uint8_t> data_;
    uint16_t upem_ = 1000;
    uint16_t num_glyphs_ = 0;

    // cmap: the Unicode subtables, in file order (offsets into data_).
    struct CmapSubtable {
        uint16_t format;
        size_t offset;
    };
    std::vector<CmapSubtable> cmap_;

    // hmtx
    size_t hmtx_ = 0;
    uint16_t num_hmetrics_ = 0;
    uint16_t hmtx_glyphs_ = 0;  // number_of_metrics after ttf-parser's adjustment

    // CFF (all offsets absolute into data_)
    struct Index {
        size_t data = 0;      // start of the object data
        size_t data_len = 0;
        size_t offsets = 0;   // start of the offset array
        uint32_t count = 0;   // number of objects
        uint8_t off_size = 1;
        std::optional<std::pair<size_t, size_t>> get(const Font& f, uint32_t i) const;  // [start, end)
    };
    size_t cff_ = 0, cff_len_ = 0;
    Index global_subrs_, char_strings_, local_subrs_;
    bool parse_index(size_t& pos, size_t limit, Index& out) const;
    bool parse_cff(std::string* error);
    std::optional<GlyphRect> outline_cff(uint16_t gid, OutlineSink& sink) const;
    friend struct CharStringParser;

    // MATH (absolute offsets; 0 = absent)
    MathConstants consts_;
    size_t glyph_info_ = 0;
    size_t variants_ = 0;
    uint16_t min_connector_overlap_ = 0;
    bool parse_math(size_t offset, size_t len, std::string* error);
    std::optional<int16_t> math_values_get(size_t field, uint16_t gid) const;
    std::optional<GlyphConstruction> construction(bool vertical, uint16_t gid) const;

    // Bounds-checked big-endian reads (nullopt when out of range).
    std::optional<uint8_t> u8(size_t off) const;
    std::optional<uint16_t> u16(size_t off) const;
    std::optional<int16_t> i16(size_t off) const;
    std::optional<uint32_t> u32(size_t off) const;
    std::optional<uint16_t> coverage_index(size_t coverage, uint16_t gid) const;
};

}  // namespace texmath
