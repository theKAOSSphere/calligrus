// Mermaid port: font advance reader + text measuring (see fontmetrics.h).
#include "fontmetrics.h"

#include "strutil.h"
#include "unicode_width.h"

namespace mermaid {

namespace {
const FontMetrics* g_font = nullptr;

uint16_t be16(const std::vector<uint8_t>& d, size_t o) {
    if (o + 2 > d.size()) return 0;
    return static_cast<uint16_t>((d[o] << 8) | d[o + 1]);
}
uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
    if (o + 4 > d.size()) return 0;
    return (static_cast<uint32_t>(d[o]) << 24) | (static_cast<uint32_t>(d[o + 1]) << 16) |
           (static_cast<uint32_t>(d[o + 2]) << 8) | d[o + 3];
}
}  // namespace

void set_measure_font(const FontMetrics* font) { g_font = font; }
const FontMetrics* measure_font() { return g_font; }

bool FontMetrics::load(std::vector<uint8_t> data) {
    data_ = std::move(data);
    units_per_em_ = 0;
    cmaps_.clear();
    if (data_.size() < 12) return false;
    size_t base = 0;
    if (data_[0] == 't' && data_[1] == 't' && data_[2] == 'c' && data_[3] == 'f') base = be32(data_, 12);
    uint16_t num_tables = be16(data_, base + 4);
    uint32_t head = 0, hhea = 0, hmtx = 0, hmtx_len = 0, cmap = 0;
    for (uint16_t i = 0; i < num_tables; ++i) {
        size_t rec = base + 12 + static_cast<size_t>(i) * 16;
        if (rec + 16 > data_.size()) return false;
        std::string_view tag(reinterpret_cast<const char*>(&data_[rec]), 4);
        uint32_t off = be32(data_, rec + 8);
        if (tag == "head") head = off;
        else if (tag == "hhea") hhea = off;
        else if (tag == "hmtx") { hmtx = off; hmtx_len = be32(data_, rec + 12); }
        else if (tag == "cmap") cmap = off;
    }
    if (!head || !hhea || !hmtx || !cmap) return false;
    units_per_em_ = be16(data_, head + 18);
    if (units_per_em_ == 0) return false;
    num_hmetrics_ = be16(data_, hhea + 34);
    hmtx_off_ = hmtx;
    hmtx_len_ = hmtx_len;

    // ttf-parser only uses Unicode subtables: platform 0, or platform 3 with encoding 1 or 10.
    uint16_t n = be16(data_, cmap + 2);
    for (uint16_t i = 0; i < n; ++i) {
        size_t rec = cmap + 4 + static_cast<size_t>(i) * 8;
        uint16_t platform = be16(data_, rec), encoding = be16(data_, rec + 2);
        uint32_t off = cmap + be32(data_, rec + 4);
        bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        if (!unicode) continue;
        uint16_t format = be16(data_, off);
        if (format == 4 || format == 12) cmaps_.push_back({format, off});
    }
    for (int c = 0; c < 128; ++c) ascii_adv_[c] = advance(glyph_index(static_cast<char32_t>(c)));
    return !cmaps_.empty();
}

uint16_t FontMetrics::glyph_index(char32_t cp) const {
    for (const CmapSub& s : cmaps_) {
        uint32_t o = s.offset;
        if (s.format == 4) {
            if (cp > 0xFFFF) continue;
            uint16_t segx2 = be16(data_, o + 6);
            uint16_t seg = segx2 / 2;
            uint32_t ends = o + 14, starts = ends + segx2 + 2, deltas = starts + segx2, ranges = deltas + segx2;
            for (uint16_t i = 0; i < seg; ++i) {
                uint16_t end = be16(data_, ends + i * 2);
                if (cp > end) continue;
                uint16_t start = be16(data_, starts + i * 2);
                if (cp < start) break;
                uint16_t delta = be16(data_, deltas + i * 2);
                uint16_t range = be16(data_, ranges + i * 2);
                uint16_t g;
                if (range == 0) {
                    g = static_cast<uint16_t>((cp + delta) & 0xFFFF);
                } else {
                    uint32_t gaddr = ranges + i * 2 + range + (cp - start) * 2;
                    g = be16(data_, gaddr);
                    if (g != 0) g = static_cast<uint16_t>((g + delta) & 0xFFFF);
                }
                if (g != 0) return g;
                break;
            }
        } else {  // format 12: sorted groups of (start, end, first glyph)
            uint32_t groups = be32(data_, o + 12);
            uint32_t lo = 0, hi = groups;
            while (lo < hi) {
                uint32_t mid = (lo + hi) / 2;
                uint32_t gp = o + 16 + mid * 12;
                uint32_t start = be32(data_, gp), end = be32(data_, gp + 4);
                if (cp < start) hi = mid;
                else if (cp > end) lo = mid + 1;
                else {
                    uint32_t g = be32(data_, gp + 8) + (cp - start);
                    if (g != 0 && g <= 0xFFFF) return static_cast<uint16_t>(g);
                    break;
                }
            }
        }
    }
    return 0;
}

uint16_t FontMetrics::advance(uint16_t glyph) const {
    if (num_hmetrics_ == 0) return 0;
    uint32_t idx = glyph < num_hmetrics_ ? glyph : static_cast<uint32_t>(num_hmetrics_ - 1);
    return be16(data_, hmtx_off_ + idx * 4);
}

// Port of FontFace::measure_width (text_metrics.rs). `fallback` = 0.56 em for glyphs the font
// lacks; wide (CJK / emoji) characters count 1 em.
float FontMetrics::measure_width(std::string_view utf8, float font_size) const {
    float scale = font_size / static_cast<float>(units_per_em_);
    float fallback = font_size * 0.56f;

    bool ascii = true;
    for (unsigned char c : utf8)
        if (c >= 0x80) { ascii = false; break; }
    if (ascii) {
        float width = 0.0f;
        for (unsigned char b : utf8) {
            if (b == '\n') continue;
            uint16_t adv = ascii_adv_[b];
            if (adv == 0) width += fallback;
            else width += static_cast<float>(adv) * scale;
        }
        return width > 0.0f ? width : 0.0f;
    }

    std::vector<char32_t> chars = decode_utf8(utf8);
    float width = 0.0f;
    size_t idx = 0;
    while (idx < chars.size()) {
        char32_t ch = chars[idx];
        if (ch == U'\n') { ++idx; continue; }
        if (auto cl = consume_cluster(chars, idx)) {
            width += cl->kind == Cluster::Wide ? font_size : 0.0f;
            idx = cl->next;
            continue;
        }
        uint16_t g = glyph_index(ch);
        if (g != 0) width += static_cast<float>(advance(g)) * scale;
        else if (is_cjk_wide_char(ch)) width += font_size;
        else width += fallback;
        ++idx;
    }
    return width > 0.0f ? width : 0.0f;
}

}  // namespace mermaid
