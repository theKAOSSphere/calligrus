// Small Unicode helpers for the TeX engine: strict UTF-8 decoding (like
// Rust's `str`), encoding, and Rust's `char::is_whitespace`.
#pragma once

#include <string>
#include <string_view>

namespace texmath {

// Append the UTF-8 encoding of `c` to `out`.
inline void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

inline std::string to_utf8(std::u32string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s) append_utf8(out, c);
    return out;
}

inline std::string to_utf8(char32_t c) {
    std::string out;
    append_utf8(out, c);
    return out;
}

// Decode UTF-8 into code points. Rejects everything Rust's `str::from_utf8`
// rejects (overlong forms, surrogates, values above U+10FFFF, truncation),
// so both engines accept exactly the same inputs.
inline bool decode_utf8(std::string_view in, std::u32string& out) {
    out.clear();
    out.reserve(in.size());
    size_t i = 0;
    const size_t n = in.size();
    auto byte = [&](size_t k) { return static_cast<unsigned char>(in[k]); };
    auto cont = [&](size_t k) { return k < n && (byte(k) & 0xC0) == 0x80; };
    while (i < n) {
        const unsigned char b0 = byte(i);
        char32_t c;
        if (b0 < 0x80) {
            c = b0;
            i += 1;
        } else if (b0 >= 0xC2 && b0 <= 0xDF) {
            if (!cont(i + 1)) return false;
            c = (char32_t(b0 & 0x1F) << 6) | (byte(i + 1) & 0x3F);
            i += 2;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            if (!cont(i + 1) || !cont(i + 2)) return false;
            c = (char32_t(b0 & 0x0F) << 12) | (char32_t(byte(i + 1) & 0x3F) << 6) | (byte(i + 2) & 0x3F);
            if (c < 0x800 || (c >= 0xD800 && c <= 0xDFFF)) return false;  // overlong / surrogate
            i += 3;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3)) return false;
            c = (char32_t(b0 & 0x07) << 18) | (char32_t(byte(i + 1) & 0x3F) << 12) |
                (char32_t(byte(i + 2) & 0x3F) << 6) | (byte(i + 3) & 0x3F);
            if (c < 0x10000 || c > 0x10FFFF) return false;
            i += 4;
        } else {
            return false;
        }
        out += c;
    }
    return true;
}

// Rust's `char::is_whitespace` (the Unicode White_Space property).
inline bool is_whitespace(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000;
}

inline bool is_ascii_alpha(char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z'); }
inline bool is_ascii_digit(char32_t c) { return c >= U'0' && c <= U'9'; }
inline bool is_ascii_alnum(char32_t c) { return is_ascii_alpha(c) || is_ascii_digit(c); }

}  // namespace texmath
