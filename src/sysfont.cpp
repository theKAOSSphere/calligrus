// System fonts for missing characters (spec 30; mirrors the Rust port function by function).
#include "sysfont.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>

namespace sysfont {

const std::vector<std::string>& preferred_families() {
#ifdef _WIN32
    static const std::vector<std::string> list = {
        "Nirmala UI", "Segoe UI Symbol", "Segoe UI Historic", "Ebrima",          "Gadugi",
        "Leelawadee UI", "Microsoft YaHei", "Yu Gothic",       "Malgun Gothic", "Microsoft JhengHei",
        "Cambria Math"};
#else
    static const std::vector<std::string> list = {"Noto Sans*", "DejaVu Sans", "FreeSans"};
#endif
    return list;
}

bool ignorable(char32_t c) {
    const bool space = c == ' ' || (c >= 0x09 && c <= 0x0D) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
                       (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000;
    const bool control = c < 0x20 || (c >= 0x7F && c <= 0x9F);
    return space || control || (c >= 0x200B && c <= 0x200F) || (c >= 0x2028 && c <= 0x202E) ||
           (c >= 0x2060 && c <= 0x2064) || (c >= 0xFE00 && c <= 0xFE0F) || c == 0xFEFF || (c >= 0xE0100 && c <= 0xE01EF);
}

std::vector<char32_t> missing_chars(const std::set<char32_t>& chars, const std::function<bool(char32_t)>& has) {
    std::vector<char32_t> out;
    for (char32_t c : chars)  // (a std::set: each once, sorted)
        if (!ignorable(c) && !has(c)) out.push_back(c);
    return out;
}

// ---------------------------------------------------------------------------
// The cmap table
// ---------------------------------------------------------------------------

namespace {

uint32_t be16(std::string_view b, size_t at) {
    return at + 2 <= b.size() ? (uint32_t(uint8_t(b[at])) << 8) | uint8_t(b[at + 1]) : 0;
}
uint32_t be32(std::string_view b, size_t at) { return (be16(b, at) << 16) | be16(b, at + 2); }

// Does one cmap subtable (starting at `t`) map `c` to a glyph other than 0?
bool subtable_has(std::string_view t, char32_t c) {
    switch (be16(t, 0)) {
        case 4: {  // segments of consecutive codes (the BMP)
            if (c > 0xFFFF) return false;
            const size_t seg_count = be16(t, 6) / 2;
            const size_t ends = 14, starts = 16 + seg_count * 2, deltas = starts + seg_count * 2, offsets = deltas + seg_count * 2;
            for (size_t i = 0; i < seg_count; i++) {
                if (c > be16(t, ends + i * 2)) continue;  // segments are sorted by their end
                if (c < be16(t, starts + i * 2)) return false;
                const uint32_t range_offset = be16(t, offsets + i * 2);
                if (range_offset == 0) return ((c + be16(t, deltas + i * 2)) & 0xFFFF) != 0;
                const size_t at = offsets + i * 2 + range_offset + (c - be16(t, starts + i * 2)) * 2;
                return be16(t, at) != 0;
            }
            return false;
        }
        case 6: {  // one run of consecutive codes
            const uint32_t first = be16(t, 6), count = be16(t, 8);
            return c >= first && c < first + count && be16(t, 10 + (c - first) * 2) != 0;
        }
        case 12:
        case 13: {  // groups of consecutive codes (all of Unicode)
            const uint32_t groups = be32(t, 12);
            for (uint32_t i = 0; i < groups; i++) {
                const size_t g = 16 + size_t(i) * 12;
                if (c >= be32(t, g) && c <= be32(t, g + 4)) {
                    const uint32_t glyph = be16(t, 0) == 12 ? be32(t, g + 8) + (c - be32(t, g)) : be32(t, g + 8);
                    return glyph != 0;
                }
            }
            return false;
        }
        default:
            return false;
    }
}

}  // namespace

bool cmap_has(std::string_view cmap, char32_t c) {
    const uint32_t count = be16(cmap, 2);
    for (uint32_t i = 0; i < count; i++) {
        const size_t rec = 4 + size_t(i) * 8;
        const uint32_t platform = be16(cmap, rec), encoding = be16(cmap, rec + 2);
        const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        const size_t offset = be32(cmap, rec + 4);
        if (unicode && offset < cmap.size() && subtable_has(cmap.substr(offset), c)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The document's characters
// ---------------------------------------------------------------------------

namespace {

// UTF-8 -> code points (invalid bytes are skipped).
void add_utf8(std::string_view s, std::set<char32_t>& out) {
    for (size_t i = 0; i < s.size();) {
        const uint8_t b = uint8_t(s[i]);
        const size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) {
            i++;
            continue;
        }
        char32_t c = n == 1 ? b : b & (0xFF >> (n + 1));
        for (size_t k = 1; k < n; k++) c = (c << 6) | (uint8_t(s[i + k]) & 0x3F);
        out.insert(c);
        i += n;
    }
}

void add_spans(const md::Spans& spans, std::set<char32_t>& out) {
    for (const md::Span& s : spans)
        if (!s.math) add_utf8(s.text, out);
}

void add_block(const md::Block& b, std::set<char32_t>& out) {
    using Kind = md::Block::Kind;
    if (b.kind == Kind::Math) return;  // maths has its own font
    add_spans(b.spans, out);
    if (b.kind == Kind::Code) add_utf8(b.text, out);
    for (const md::Block& child : b.children) add_block(child, out);
    for (const auto& item : b.items)
        for (const md::Block& child : item) add_block(child, out);
    for (const md::Spans& cell : b.header) add_spans(cell, out);
    for (const auto& row : b.rows)
        for (const md::Spans& cell : row) add_spans(cell, out);
}

}  // namespace

std::set<char32_t> document_chars(const md::Document& doc) {
    std::set<char32_t> out;
    for (const md::Block& b : doc) add_block(b, out);
    return out;
}

// ---------------------------------------------------------------------------
// Choosing
// ---------------------------------------------------------------------------

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

// Where a family is in the preferred list (the list's size if it is not).
size_t rank(const std::string& family, const std::vector<std::string>& preferred) {
    for (size_t i = 0; i < preferred.size(); i++) {
        const std::string& p = preferred[i];
        if (!p.empty() && p.back() == '*') {
            const std::string_view prefix = std::string_view(p).substr(0, p.size() - 1);
            if (family.size() >= prefix.size() && same_family(std::string_view(family).substr(0, prefix.size()), prefix)) return i;
        } else if (same_family(p, family)) {
            return i;
        }
    }
    return preferred.size();
}

}  // namespace

std::vector<FontFace> ordered_candidates(const std::vector<FontFace>& faces, const std::vector<std::string>& preferred) {
    std::vector<FontFace> out;
    for (const FontFamily& family : list_families(faces))
        if (auto regular = pick_faces(faces, family.name).regular) out.push_back(*regular);
    std::stable_sort(out.begin(), out.end(), [&](const FontFace& a, const FontFace& b) {
        const size_t ra = rank(a.family, preferred), rb = rank(b.family, preferred);
        return ra != rb ? ra < rb : lower(a.family) < lower(b.family);
    });
    return out;
}

std::vector<size_t> choose(const std::vector<std::vector<char32_t>>& covers, const std::vector<char32_t>& missing,
                           size_t max) {
    std::vector<char32_t> left = missing;
    std::vector<size_t> chosen;
    auto in = [](const std::vector<char32_t>& v, char32_t c) { return std::find(v.begin(), v.end(), c) != v.end(); };
    while (chosen.size() < max && !left.empty()) {
        size_t best = 0, best_n = 0;  // the candidate, and how many of `left` it covers
        for (size_t i = 0; i < covers.size(); i++) {
            const size_t n = size_t(std::count_if(covers[i].begin(), covers[i].end(), [&](char32_t c) { return in(left, c); }));
            if (n > best_n) best = i, best_n = n;
        }
        if (best_n == 0) break;
        left.erase(std::remove_if(left.begin(), left.end(), [&](char32_t c) { return in(covers[best], c); }), left.end());
        chosen.push_back(best);
    }
    return chosen;
}

std::vector<FontSource> pick_with(const std::vector<FontFace>& faces, const std::vector<std::string>& preferred,
                                  const std::vector<char32_t>& missing, size_t max, const CoverageFn& coverage) {
    std::vector<FontFace> usable;
    std::vector<std::vector<char32_t>> covers;
    for (const FontFace& face : ordered_candidates(faces, preferred)) {
        std::vector<char32_t> covered;
        // Candidates covering nothing are dropped here (they could never be chosen).
        if (coverage(face, missing, covered) && !covered.empty()) {
            usable.push_back(face);
            covers.push_back(std::move(covered));
        }
    }
    std::vector<FontSource> out;
    for (size_t i : choose(covers, missing, max)) out.push_back({usable[i].file, usable[i].face});
    return out;
}

std::vector<FontSource> pick_system_fonts(const std::vector<FontFace>& faces, const std::vector<char32_t>& missing,
                                          size_t max) {
    auto coverage = [](const FontFace& face, const std::vector<char32_t>& missing, std::vector<char32_t>& covered) {
        std::string cmap;
        bool colour = false;
        const std::u8string path(face.file.begin(), face.file.end());
        if (!read_font_cmap(std::filesystem::path(path), face.face, cmap, colour) || colour) return false;
        for (char32_t c : missing)
            if (cmap_has(cmap, c)) covered.push_back(c);
        return true;
    };
    return pick_with(faces, preferred_families(), missing, max, coverage);
}

}  // namespace sysfont
