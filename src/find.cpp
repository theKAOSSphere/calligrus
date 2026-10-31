#include "find.h"

namespace {

// Number of UTF-8 code points in `s`.
size_t char_count(const std::string& s) {
    size_t n = 0;
    for (char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return n;
}

constexpr char32_t kAtomic = 0xFFFC;  // stands in for an equation: never matches

}  // namespace

std::u32string to_utf32(std::string_view s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        const int len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + len > s.size()) {
            out += U'�';
            i++;
            continue;
        }
        char32_t c = len == 1 ? b : b & (0x7F >> len);
        bool ok = true;
        for (int k = 1; k < len; k++) {
            const unsigned char cont = static_cast<unsigned char>(s[i + k]);
            ok = ok && (cont & 0xC0) == 0x80;
            c = (c << 6) | (cont & 0x3F);
        }
        out += ok ? c : U'�';
        i += ok ? size_t(len) : 1;
    }
    return out;
}

char32_t fold_case(char32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c < 0xC0) return c;
    if (c <= 0xDE) return c == 0xD7 ? c : c + 32;  // Latin-1 (except ×)
    // Latin Extended-A: upper/lower pairs, upper on even or odd code points.
    if ((c >= 0x100 && c <= 0x12F) || (c >= 0x132 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c % 2 == 0 ? c + 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return c % 2 == 1 ? c + 1 : c;
    if (c == 0x178) return 0xFF;  // Ÿ
    // Greek.
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 63;
    // Cyrillic.
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

bool find_case_sensitive(std::string_view query) {
    for (char32_t c : to_utf32(query))
        if (fold_case(c) != c) return true;
    return false;
}

std::vector<std::pair<size_t, size_t>> find_in_text(std::u32string_view hay, std::u32string_view needle,
                                                    bool case_sensitive) {
    std::vector<std::pair<size_t, size_t>> out;
    if (needle.empty() || needle.size() > hay.size()) return out;
    auto same = [&](char32_t a, char32_t b) { return case_sensitive ? a == b : fold_case(a) == fold_case(b); };
    for (size_t i = 0; i + needle.size() <= hay.size();) {
        size_t k = 0;
        while (k < needle.size() && same(hay[i + k], needle[k])) k++;
        if (k == needle.size()) {
            out.emplace_back(i, i + needle.size());
            i += needle.size();  // non-overlapping
        } else {
            i++;
        }
    }
    return out;
}

std::vector<TextRange> find_matches(const std::vector<Run>& runs, std::string_view query, int case_mode) {
    std::vector<TextRange> out;
    const std::u32string needle = to_utf32(query);
    if (needle.empty()) return out;
    const bool case_sensitive = case_mode < 0 ? find_case_sensitive(query) : case_mode != 0;

    // One group at a time: its text, and for every character where it starts
    // and ends as a selection position.
    std::u32string text;
    std::vector<TextPos> starts, ends;
    auto search_group = [&] {
        for (auto [a, b] : find_in_text(text, needle, case_sensitive))
            if (starts[a] < ends[b - 1]) out.push_back({starts[a], ends[b - 1]});
        text.clear();
        starts.clear();
        ends.clear();
    };

    const Run* prev = nullptr;
    for (const Run& r : runs) {
        if (prev && r.group != prev->group) search_group();
        else if (prev && r.line != prev->line) {
            // A wrapped line: the space the wrap swallowed (or the hard
            // break) is searchable text too, as in copied text (select.cpp).
            char32_t gap = r.hard ? U'\n' : (!prev->text.empty() && prev->text.back() != ' ' && r.text != " ") ? U' ' : 0;
            if (gap) {
                text += gap;
                starts.push_back(TextPos{r.pos.block, r.pos.tok, 0});
                ends.push_back(TextPos{prev->pos.block, prev->pos.tok, prev->len()});
            }
        }
        const uint32_t n = r.len();
        const bool atomic = n != char_count(r.text);
        const std::u32string chars = atomic ? std::u32string(n, kAtomic) : to_utf32(r.text);
        for (uint32_t k = 0; k < n && k < chars.size(); k++) {
            text += chars[k];
            starts.push_back(TextPos{r.pos.block, r.pos.tok, k});
            ends.push_back(TextPos{r.pos.block, r.pos.tok, k + 1});
        }
        prev = &r;
    }
    if (prev) search_group();
    return out;
}
