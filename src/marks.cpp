#include "marks.h"

#include <algorithm>

namespace marks {

namespace {

// UTF-8 -> UTF-32 (invalid bytes become U+FFFD).
std::u32string utf32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + size_t(n) > s.size()) {
            out += U'�';
            i++;
            continue;
        }
        char32_t cp = n == 1 ? c : c & (0x7F >> n);
        for (int k = 1; k < n; k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3F);
        out += cp;
        i += size_t(n);
    }
    return out;
}

std::string utf8(const std::u32string& s) {
    std::string out;
    for (char32_t c : s) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 0x3F));
        else if (c < 0x10000) out += char(0xE0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
        else
            out += char(0xF0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3F)), out += char(0x80 | ((c >> 6) & 0x3F)),
                out += char(0x80 | (c & 0x3F));
    }
    return out;
}

// Unicode White_Space (Rust's char::is_whitespace).
bool is_space(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

void spans_text(const md::Spans& spans, std::string& out) {
    for (const md::Span& s : spans) out += s.math ? "\xEF\xBF\xBC" : s.text;  // an equation: U+FFFC (find.rs's ATOM)
}

// Everything find.rs's block_groups yields, separated by spaces (the key collapses them anyway).
void block_text(const md::Block& b, std::string& out) {
    using K = md::Block::Kind;
    out += ' ';
    switch (b.kind) {
        case K::Paragraph:
        case K::Heading: spans_text(b.spans, out); break;
        case K::Code: {
            std::string lang = b.lang;
            std::transform(lang.begin(), lang.end(), lang.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            lang.erase(0, lang.find_first_not_of(" \t"));
            lang.erase(lang.find_last_not_of(" \t") + 1);
            if (lang != "mermaid") out += b.text;  // (a diagram is a picture, not text)
            break;
        }
        case K::Quote:
            if (!b.callout.empty()) {  // the title line: "Note", "Tip"...
                std::string title = b.callout;
                title[0] = char(std::toupper(static_cast<unsigned char>(title[0])));
                out += title;
            }
            for (const md::Block& c : b.children) block_text(c, out);
            break;
        case K::List:
            for (const auto& item : b.items)
                for (const md::Block& c : item) block_text(c, out);
            break;
        case K::Table:
            for (const md::Spans& cell : b.header) spans_text(cell, out += ' ');
            for (const auto& row : b.rows)
                for (const md::Spans& cell : row) spans_text(cell, out += ' ');
            break;
        case K::Math:
        case K::Rule:
        case K::Images: break;
    }
}

}  // namespace

std::string block_key(const md::Block& b) {
    std::string text;
    block_text(b, text);
    std::u32string key;
    bool space = true;  // (no space at the start)
    for (char32_t c : utf32(text)) {
        if (is_space(c)) {
            space = space || !key.empty();
            continue;
        }
        if (space && !key.empty()) key += U' ';
        space = false;
        key += c;
    }
    if (key.size() > kKeyChars) key.resize(kKeyChars);  // (as marks.rs: a cut may leave a space at the end)
    return utf8(key);
}

std::optional<size_t> resolve(size_t block, const std::string& text, const std::vector<std::string>& keys) {
    if (block < keys.size() && keys[block] == text) return block;
    if (text.empty()) return std::nullopt;
    std::optional<size_t> best;
    for (size_t i = 0; i < keys.size(); i++) {
        if (keys[i] != text) continue;
        const size_t d = i > block ? i - block : block - i, bd = best ? (*best > block ? *best - block : block - *best) : 0;
        if (!best || d < bd) best = i;
    }
    return best;
}

std::string margin_label(std::vector<char> names) {
    std::sort(names.begin(), names.end(), [](char a, char b) {
        const bool ua = a >= 'A' && a <= 'Z', ub = b >= 'A' && b <= 'Z';
        return ua != ub ? !ua : a < b;
    });
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return std::string(names.begin(), names.end());
}

}  // namespace marks
