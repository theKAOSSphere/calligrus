// Mermaid port: small string helpers that behave like the Rust str methods the crate uses.
// (ASCII whitespace only: Rust's trim()/split_whitespace() also know Unicode spaces; the
// difference only matters for exotic input.)
#pragma once

#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace mermaid {

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

inline std::string_view trim_view(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) ++a;
    while (b > a && is_space(s[b - 1])) --b;
    return s.substr(a, b - a);
}
inline std::string trim(std::string_view s) { return std::string(trim_view(s)); }

inline std::string_view trim_end_view(std::string_view s) {
    size_t b = s.size();
    while (b > 0 && is_space(s[b - 1])) --b;
    return s.substr(0, b);
}
inline std::string_view trim_start_view(std::string_view s) {
    size_t a = 0;
    while (a < s.size() && is_space(s[a])) ++a;
    return s.substr(a);
}

inline bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
inline bool ends_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

// Rust str::lines(): split on '\n', drop a trailing '\r', no empty last line after a final '\n'.
inline std::vector<std::string> lines_of(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find('\n', i);
        size_t end = (j == std::string_view::npos) ? s.size() : j;
        size_t e2 = end;
        if (e2 > i && s[e2 - 1] == '\r') --e2;
        out.emplace_back(s.substr(i, e2 - i));
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    return out;
}

// Rust str::split_whitespace().
inline std::vector<std::string> split_whitespace(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_space(s[i])) ++i;
        size_t j = i;
        while (j < s.size() && !is_space(s[j])) ++j;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

inline std::string to_ascii_lower(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}
inline std::string to_ascii_upper(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return r;
}

inline std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// Number of Unicode scalar values (Rust chars().count()) in a UTF-8 string.
inline size_t char_count(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// Decode UTF-8 into code points (invalid bytes become U+FFFD).
inline std::vector<char32_t> decode_utf8(std::string_view s) {
    std::vector<char32_t> out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
        else { out.push_back(0xFFFD); ++i; continue; }
        if (i + static_cast<size_t>(len) > s.size()) { out.push_back(0xFFFD); break; }
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += static_cast<size_t>(len);
    }
    return out;
}

// Rust `str::parse::<f32>()`: the whole string must be a decimal number ("inf" / "nan" spellings
// included, hex floats and surrounding blanks refused). Returns false when Rust would return Err.
inline bool parse_rust_f32(const std::string& t, float& out) {
    if (t.empty()) return false;
    std::string lower;
    for (char c : t) lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    std::string body = (lower[0] == '+' || lower[0] == '-') ? lower.substr(1) : lower;
    if (body == "inf" || body == "infinity" || body == "nan") {
        out = body == "nan" ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
        if (lower[0] == '-') out = -out;
        return true;
    }
    bool digit = false;
    for (char c : body) {
        if (c >= '0' && c <= '9') digit = true;
        else if (c != '.' && c != 'e' && c != '+' && c != '-') return false;
    }
    if (!digit) return false;
    char* end = nullptr;
    out = std::strtof(t.c_str(), &end);
    return end == t.c_str() + t.size();
}

// parser.rs split_args: split on commas outside quotes, trim, drop empty pieces.
inline std::vector<std::string> split_args(std::string_view input) {
    std::vector<std::string> args;
    std::string current;
    char quote = 0;
    auto flush = [&]() {
        std::string t(trim_view(current));
        if (!t.empty()) args.push_back(t);
        current.clear();
    };
    for (char ch : input) {
        if (quote) {
            if (ch == quote) quote = 0;
            current.push_back(ch);
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            current.push_back(ch);
            continue;
        }
        if (ch == ',') {
            flush();
            continue;
        }
        current.push_back(ch);
    }
    flush();
    return args;
}

}  // namespace mermaid
