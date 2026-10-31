// sfnt container, cmap, hmtx and MATH table (CFF outlines are in cff.cpp).
#include "font.h"

namespace texmath {

// ---------------------------------------------------------------------------
// Bounds-checked big-endian reads
// ---------------------------------------------------------------------------

std::optional<uint8_t> Font::u8(size_t off) const {
    if (off >= data_.size()) return std::nullopt;
    return data_[off];
}

std::optional<uint16_t> Font::u16(size_t off) const {
    if (off > data_.size() || data_.size() - off < 2) return std::nullopt;
    return static_cast<uint16_t>((data_[off] << 8) | data_[off + 1]);
}

std::optional<int16_t> Font::i16(size_t off) const {
    auto v = u16(off);
    if (!v) return std::nullopt;
    return static_cast<int16_t>(*v);
}

std::optional<uint32_t> Font::u32(size_t off) const {
    if (off > data_.size() || data_.size() - off < 4) return std::nullopt;
    return (uint32_t(data_[off]) << 24) | (uint32_t(data_[off + 1]) << 16) | (uint32_t(data_[off + 2]) << 8) |
           uint32_t(data_[off + 3]);
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

bool Font::load(std::vector<uint8_t> bytes, std::string* error) {
    data_ = std::move(bytes);
    auto bad = [error](const char* why) {
        if (error) *error = why;
        return false;
    };

    // The table directory: tag + checksum + offset + length per table.
    auto num_tables = u16(4);
    if (!num_tables) return bad("not an OpenType font");
    struct TableRef {
        size_t offset = 0, length = 0;
        bool found = false;
    };
    auto find = [&](const char tag[5]) {
        TableRef t;
        for (uint16_t i = 0; i < *num_tables; i++) {
            const size_t rec = 12 + size_t(i) * 16;
            if (rec + 16 > data_.size()) break;
            if (data_[rec] == uint8_t(tag[0]) && data_[rec + 1] == uint8_t(tag[1]) &&
                data_[rec + 2] == uint8_t(tag[2]) && data_[rec + 3] == uint8_t(tag[3])) {
                t.offset = *u32(rec + 8);
                t.length = *u32(rec + 12);
                // A table that doesn't fit in the file counts as missing.
                t.found = t.offset <= data_.size() && t.length <= data_.size() - t.offset;
                break;
            }
        }
        return t;
    };

    TableRef head = find("head"), maxp = find("maxp"), hhea = find("hhea"), hmtx = find("hmtx");
    TableRef cmap = find("cmap"), cff = find("CFF "), math = find("MATH");
    if (!head.found || !maxp.found || !hhea.found) return bad("font lacks head/maxp/hhea");

    upem_ = u16(head.offset + 18).value_or(0);
    if (upem_ < 16 || upem_ > 16384) return bad("bad unitsPerEm");
    num_glyphs_ = u16(maxp.offset + 4).value_or(0);
    if (num_glyphs_ == 0) return bad("font has no glyphs");

    // hmtx: advances for the first numberOfHMetrics glyphs; the rest reuse the
    // last one (ttf-parser's `hmtx::Table`).
    num_hmetrics_ = u16(hhea.offset + 34).value_or(0);
    if (hmtx.found && num_hmetrics_ > 0 && size_t(num_hmetrics_) * 4 <= hmtx.length) {
        hmtx_ = hmtx.offset;
        hmtx_glyphs_ = num_hmetrics_;
        if (num_glyphs_ >= num_hmetrics_) hmtx_glyphs_ = num_glyphs_;
    }

    // cmap: remember the Unicode subtables we can read, in file order. Like
    // ttf-parser, a subtable that fails to parse ends the list.
    if (cmap.found) {
        const uint16_t count = u16(cmap.offset + 2).value_or(0);
        for (uint16_t i = 0; i < count; i++) {
            const size_t rec = cmap.offset + 4 + size_t(i) * 8;
            auto platform = u16(rec), encoding = u16(rec + 2);
            auto sub_off = u32(rec + 4);
            if (!platform || !encoding || !sub_off || *platform > 4) break;
            const size_t sub = cmap.offset + *sub_off;
            auto format = u16(sub);
            if (!format) break;
            const bool f12 = *format == 12 || *format == 13;
            const bool unicode = *platform == 0 || (*platform == 3 && *encoding == 1) ||
                                 (*platform == 3 && *encoding == 10 && f12);
            if (*format == 4) {
                auto seg_x2 = u16(sub + 6);
                if (!seg_x2 || *seg_x2 < 2) break;
                // end codes, pad, start codes, deltas, range offsets must all fit
                if (!u16(sub + 14 + size_t(*seg_x2) * 4)) break;
            } else if (f12) {
                auto groups = u32(sub + 12);
                if (!groups || size_t(*groups) * 12 > data_.size() - (sub + 16)) break;
            } else if (*format != 0 && *format != 2 && *format != 6 && *format != 8 && *format != 10 &&
                       *format != 14) {
                break;
            }
            if (unicode) cmap_.push_back({*format, sub});
        }
    }

    if (!cff.found) return bad("font has no CFF outlines");
    cff_ = cff.offset;
    cff_len_ = cff.length;
    if (!parse_cff(error)) return false;

    if (!math.found) return bad("font has no MATH table");
    return parse_math(math.offset, math.length, error);
}

// ---------------------------------------------------------------------------
// cmap / hmtx
// ---------------------------------------------------------------------------

std::optional<uint16_t> Font::glyph_index(char32_t c) const {
    const uint32_t code = c;
    for (const CmapSubtable& t : cmap_) {
        if (t.format == 4) {
            // Segment mapping to delta values (BMP only).
            if (code > 0xFFFF) continue;
            const uint16_t cp = static_cast<uint16_t>(code);
            const uint16_t seg_count = *u16(t.offset + 6) / 2;
            const size_t end_codes = t.offset + 14;
            const size_t start_codes = end_codes + size_t(seg_count) * 2 + 2;
            const size_t id_deltas = start_codes + size_t(seg_count) * 2;
            const size_t id_range_offsets = id_deltas + size_t(seg_count) * 2;
            const size_t id_range_offset_pos = id_range_offsets - t.offset;  // relative to the subtable
            // A binary search, exactly as ttf-parser does it.
            uint32_t start = 0, end = seg_count;
            std::optional<uint16_t> found;
            bool done = false;
            while (end > start && !done) {
                const uint32_t index = (start + end) / 2;
                const uint16_t end_value = *u16(end_codes + index * 2);
                if (end_value >= cp) {
                    const uint16_t start_value = *u16(start_codes + index * 2);
                    if (start_value > cp) {
                        end = index;
                    } else {
                        done = true;
                        const uint16_t range_offset = *u16(id_range_offsets + index * 2);
                        const int16_t delta = *i16(id_deltas + index * 2);
                        if (range_offset == 0) {
                            found = static_cast<uint16_t>(cp + static_cast<uint16_t>(delta));
                        } else if (range_offset != 0xFFFF) {
                            const uint32_t d = (uint32_t(cp) - start_value) * 2;
                            if (d <= 0xFFFF) {
                                uint16_t pos = static_cast<uint16_t>(id_range_offset_pos + index * 2);
                                pos = static_cast<uint16_t>(pos + d);
                                pos = static_cast<uint16_t>(pos + range_offset);
                                auto glyph = u16(t.offset + pos);
                                if (glyph && *glyph != 0) {
                                    const int16_t id = static_cast<int16_t>(static_cast<int16_t>(*glyph) + delta);
                                    if (id >= 0) found = static_cast<uint16_t>(id);
                                }
                            }
                        }
                    }
                } else {
                    start = index + 1;
                }
            }
            if (found) return found;
        } else if (t.format == 12 || t.format == 13) {
            // Segmented coverage: sorted groups of consecutive code points.
            const uint32_t count = *u32(t.offset + 12);
            const size_t groups = t.offset + 16;
            uint32_t lo = 0, hi = count;
            while (lo < hi) {
                const uint32_t mid = lo + (hi - lo) / 2;
                const uint32_t first = *u32(groups + mid * 12);
                const uint32_t last = *u32(groups + mid * 12 + 4);
                if (first > code) {
                    hi = mid;
                } else if (last < code) {
                    lo = mid + 1;
                } else {
                    const uint32_t start_glyph = *u32(groups + mid * 12 + 8);
                    const uint64_t id = uint64_t(start_glyph) + code - first;
                    if (t.format == 12 && id <= 0xFFFF) return static_cast<uint16_t>(id);
                    if (t.format == 13 && start_glyph <= 0xFFFF) return static_cast<uint16_t>(start_glyph);
                    break;
                }
            }
        }
        // Other formats never map anything we draw (the bundled font only has
        // formats 4 and 12 in its Unicode subtables).
    }
    return std::nullopt;
}

std::optional<uint16_t> Font::glyph_hor_advance(uint16_t gid) const {
    if (hmtx_ == 0 || gid >= hmtx_glyphs_) return std::nullopt;
    const uint16_t index = gid < num_hmetrics_ ? gid : static_cast<uint16_t>(num_hmetrics_ - 1);
    return u16(hmtx_ + size_t(index) * 4);
}

// ---------------------------------------------------------------------------
// MATH table
// ---------------------------------------------------------------------------

// Coverage table lookup: which slot (if any) of a per-glyph array belongs to `gid`.
std::optional<uint16_t> Font::coverage_index(size_t coverage, uint16_t gid) const {
    auto format = u16(coverage);
    auto count = u16(coverage + 2);
    if (!format || !count) return std::nullopt;
    if (*format == 1) {
        // Sorted glyph list: the index is the position in it.
        uint32_t lo = 0, hi = *count;
        while (lo < hi) {
            const uint32_t mid = (lo + hi) / 2;
            auto g = u16(coverage + 4 + mid * 2);
            if (!g) return std::nullopt;
            if (*g < gid) {
                lo = mid + 1;
            } else if (*g > gid) {
                hi = mid;
            } else {
                return static_cast<uint16_t>(mid);
            }
        }
    } else if (*format == 2) {
        // Ranges of glyphs, each with the index of its first glyph.
        uint32_t lo = 0, hi = *count;
        while (lo < hi) {
            const uint32_t mid = (lo + hi) / 2;
            const size_t rec = coverage + 4 + mid * 6;
            auto first = u16(rec), last = u16(rec + 2), start_index = u16(rec + 4);
            if (!first || !last || !start_index) return std::nullopt;
            if (gid < *first) {
                hi = mid;
            } else if (gid > *last) {
                lo = mid + 1;
            } else {
                const uint32_t v = uint32_t(*start_index) + (gid - *first);
                if (v > 0xFFFF) return std::nullopt;
                return static_cast<uint16_t>(v);
            }
        }
    }
    return std::nullopt;
}

bool Font::parse_math(size_t offset, size_t len, std::string* error) {
    (void)len;
    auto major = u16(offset);
    if (!major || (*major & 0xFF) != 1) {
        if (error) *error = "unsupported MATH table version";
        return false;
    }
    auto sub = [&](size_t field) -> size_t {  // absolute offset of a subtable, 0 if absent
        auto o = u16(field);
        if (!o || *o == 0) return 0;
        return offset + *o;
    };
    const size_t constants = sub(offset + 4);
    glyph_info_ = sub(offset + 6);
    variants_ = sub(offset + 8);
    if (constants == 0) {
        if (error) *error = "MATH table has no constants";
        return false;
    }

    // MathConstants: a fixed layout. The first four are plain numbers, the
    // rest are MathValueRecords (value + device-table offset, 4 bytes each).
    MathConstants& c = consts_;
    auto rd16 = [&](size_t at) { return i16(constants + at).value_or(0); };
    auto rdu16 = [&](size_t at) { return u16(constants + at).value_or(0); };
    auto rec = [&](size_t at) -> int16_t {
        // A record needs all 4 bytes, otherwise ttf-parser falls back to 0.
        if (!u32(constants + at)) return 0;
        return *i16(constants + at);
    };
    c.script_percent_scale_down = rd16(0);
    c.script_script_percent_scale_down = rd16(2);
    c.delimited_sub_formula_min_height = rdu16(4);
    c.display_operator_min_height = rdu16(6);
    c.math_leading = rec(8);
    c.axis_height = rec(12);
    c.accent_base_height = rec(16);
    c.flattened_accent_base_height = rec(20);
    c.subscript_shift_down = rec(24);
    c.subscript_top_max = rec(28);
    c.subscript_baseline_drop_min = rec(32);
    c.superscript_shift_up = rec(36);
    c.superscript_shift_up_cramped = rec(40);
    c.superscript_bottom_min = rec(44);
    c.superscript_baseline_drop_max = rec(48);
    c.sub_superscript_gap_min = rec(52);
    c.superscript_bottom_max_with_subscript = rec(56);
    c.space_after_script = rec(60);
    c.upper_limit_gap_min = rec(64);
    c.upper_limit_baseline_rise_min = rec(68);
    c.lower_limit_gap_min = rec(72);
    c.lower_limit_baseline_drop_min = rec(76);
    c.stack_top_shift_up = rec(80);
    c.stack_top_display_style_shift_up = rec(84);
    c.stack_bottom_shift_down = rec(88);
    c.stack_bottom_display_style_shift_down = rec(92);
    c.stack_gap_min = rec(96);
    c.stack_display_style_gap_min = rec(100);
    c.stretch_stack_top_shift_up = rec(104);
    c.stretch_stack_bottom_shift_down = rec(108);
    c.stretch_stack_gap_above_min = rec(112);
    c.stretch_stack_gap_below_min = rec(116);
    c.fraction_numerator_shift_up = rec(120);
    c.fraction_numerator_display_style_shift_up = rec(124);
    c.fraction_denominator_shift_down = rec(128);
    c.fraction_denominator_display_style_shift_down = rec(132);
    c.fraction_numerator_gap_min = rec(136);
    c.fraction_num_display_style_gap_min = rec(140);
    c.fraction_rule_thickness = rec(144);
    c.fraction_denominator_gap_min = rec(148);
    c.fraction_denom_display_style_gap_min = rec(152);
    c.skewed_fraction_horizontal_gap = rec(156);
    c.skewed_fraction_vertical_gap = rec(160);
    c.overbar_vertical_gap = rec(164);
    c.overbar_rule_thickness = rec(168);
    c.overbar_extra_ascender = rec(172);
    c.underbar_vertical_gap = rec(176);
    c.underbar_rule_thickness = rec(180);
    c.underbar_extra_descender = rec(184);
    c.radical_vertical_gap = rec(188);
    c.radical_display_style_vertical_gap = rec(192);
    c.radical_rule_thickness = rec(196);
    c.radical_extra_ascender = rec(200);
    c.radical_kern_before_degree = rec(204);
    c.radical_kern_after_degree = rec(208);
    c.radical_degree_bottom_raise_percent = rd16(212);

    if (variants_ != 0) min_connector_overlap_ = u16(variants_).value_or(0);
    return true;
}

// MathValues (italic corrections / top accent attachments): coverage offset,
// count, then one MathValueRecord per covered glyph. `field` is the
// MathGlyphInfo slot holding the table's offset.
std::optional<int16_t> Font::math_values_get(size_t field, uint16_t gid) const {
    if (glyph_info_ == 0) return std::nullopt;
    auto off = u16(field);
    if (!off || *off == 0) return std::nullopt;
    const size_t table = glyph_info_ + *off;
    auto cov = u16(table);
    auto count = u16(table + 2);
    if (!cov || *cov == 0 || !count) return std::nullopt;
    auto index = coverage_index(table + *cov, gid);
    if (!index || *index >= *count) return std::nullopt;
    return i16(table + 4 + size_t(*index) * 4);
}

std::optional<int16_t> Font::italic_correction(uint16_t gid) const { return math_values_get(glyph_info_, gid); }

std::optional<int16_t> Font::top_accent_attachment(uint16_t gid) const {
    return math_values_get(glyph_info_ + 2, gid);
}

std::optional<KernInfo> Font::kern_info(uint16_t gid) const {
    if (glyph_info_ == 0) return std::nullopt;
    auto off = u16(glyph_info_ + 6);
    if (!off || *off == 0) return std::nullopt;
    const size_t table = glyph_info_ + *off;  // MathKernInfo
    auto cov = u16(table);
    auto count = u16(table + 2);
    if (!cov || *cov == 0 || !count) return std::nullopt;
    auto index = coverage_index(table + *cov, gid);
    if (!index || *index >= *count) return std::nullopt;
    const size_t rec = table + 4 + size_t(*index) * 8;
    if (!u32(rec) || !u32(rec + 4)) return std::nullopt;
    // Each corner is an offset (from the MathKernInfo table) to a MathKern.
    auto corner = [&](size_t field) -> std::optional<MathKern> {
        const uint16_t o = *u16(field);
        if (o == 0) return std::nullopt;
        const size_t k = table + o;
        auto n = u16(k);
        if (!n) return std::nullopt;
        // heights: n records, kerns: n + 1 records (4 bytes each)
        const size_t needed = 2 + (size_t(*n) * 2 + 1) * 4;
        if (k > data_.size() || data_.size() - k < needed) return std::nullopt;
        MathKern mk;
        for (size_t i = 0; i < *n; i++) mk.heights.push_back(*i16(k + 2 + i * 4));
        for (size_t i = 0; i <= *n; i++) mk.kerns.push_back(*i16(k + 2 + (size_t(*n) + i) * 4));
        return mk;
    };
    KernInfo info;
    info.top_right = corner(rec);
    info.top_left = corner(rec + 2);
    info.bottom_right = corner(rec + 4);
    info.bottom_left = corner(rec + 6);
    return info;
}

// MathVariants: overlap, two coverages, two counts, then the construction
// offsets (vertical first, then horizontal).
std::optional<GlyphConstruction> Font::construction(bool vertical, uint16_t gid) const {
    if (variants_ == 0) return std::nullopt;
    auto vcov = u16(variants_ + 2), hcov = u16(variants_ + 4);
    auto vcount = u16(variants_ + 6), hcount = u16(variants_ + 8);
    if (!vcov || !hcov || !vcount || !hcount) return std::nullopt;
    const uint16_t cov = vertical ? *vcov : *hcov;
    if (cov == 0) return std::nullopt;
    auto index = coverage_index(variants_ + cov, gid);
    const uint16_t count = vertical ? *vcount : *hcount;
    if (!index || *index >= count) return std::nullopt;
    const size_t offsets = variants_ + 10 + (vertical ? 0 : size_t(*vcount) * 2);
    auto c_off = u16(offsets + size_t(*index) * 2);
    if (!c_off || *c_off == 0) return std::nullopt;
    const size_t con = variants_ + *c_off;  // MathGlyphConstruction

    GlyphConstruction gc;
    auto asm_off = u16(con);
    auto nvariants = u16(con + 2);
    if (!asm_off || !nvariants) return std::nullopt;
    if (con + 4 + size_t(*nvariants) * 4 > data_.size()) return std::nullopt;
    for (size_t i = 0; i < *nvariants; i++) {
        gc.variants.push_back({*u16(con + 4 + i * 4), *u16(con + 6 + i * 4)});
    }
    if (*asm_off != 0) {
        // GlyphAssembly: italic correction record, part count, parts (10 bytes each).
        const size_t a = con + *asm_off;
        auto nparts = u16(a + 4);
        if (u32(a) && nparts && a + 6 + size_t(*nparts) * 10 <= data_.size()) {
            std::vector<GlyphPart> parts;
            for (size_t i = 0; i < *nparts; i++) {
                const size_t p = a + 6 + i * 10;
                parts.push_back({*u16(p), *u16(p + 2), *u16(p + 4), *u16(p + 6), *u16(p + 8)});
            }
            gc.assembly = std::move(parts);
        }
    }
    return gc;
}

std::optional<GlyphConstruction> Font::vertical_construction(uint16_t gid) const { return construction(true, gid); }
std::optional<GlyphConstruction> Font::horizontal_construction(uint16_t gid) const {
    return construction(false, gid);
}

}  // namespace texmath
