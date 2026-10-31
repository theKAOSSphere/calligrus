// A JSON5 syntax checker: says whether the text parses (the Rust crate only needs to know that for
// "%%{init: {...}}%%" directives; it never uses the parsed config when rendering).
#include "json5.h"

#include <cctype>
#include <string>

namespace mermaid {

namespace {

struct P {
    const std::string& s;
    size_t i = 0;
    int depth = 0;

    explicit P(const std::string& text) : s(text) {}

    bool eof() const { return i >= s.size(); }
    char cur() const { return s[i]; }

    // whitespace and comments
    bool skip_ws() {
        while (!eof()) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B || c == 0x0C) { ++i; continue; }
            if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) { i += 2; continue; }
            if (c == 0xEF && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xBB && static_cast<unsigned char>(s[i + 2]) == 0xBF) { i += 3; continue; }
            if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 && (static_cast<unsigned char>(s[i + 2]) == 0xA8 || static_cast<unsigned char>(s[i + 2]) == 0xA9)) { i += 3; continue; }
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                while (!eof() && s[i] != '\n') ++i;
                continue;
            }
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
                size_t end = s.find("*/", i + 2);
                if (end == std::string::npos) return false;
                i = end + 2;
                continue;
            }
            break;
        }
        return true;
    }

    bool is_id_start(unsigned char c) { return std::isalpha(c) || c == '_' || c == '$' || c >= 0x80; }
    bool is_id_part(unsigned char c) { return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80; }

    bool string_lit() {
        char q = s[i++];
        while (!eof()) {
            char c = s[i++];
            if (c == q) return true;
            if (c == '\\') {
                if (eof()) return false;
                char e = s[i++];
                if (e == 'x') { if (i + 2 > s.size() || !std::isxdigit(static_cast<unsigned char>(s[i])) || !std::isxdigit(static_cast<unsigned char>(s[i + 1]))) return false; i += 2; }
                else if (e == 'u') { if (i + 4 > s.size()) return false; for (int k = 0; k < 4; ++k) if (!std::isxdigit(static_cast<unsigned char>(s[i + k]))) return false; i += 4; }
                else if (e == '\r' && !eof() && s[i] == '\n') ++i;
            } else if (c == '\n' || c == '\r') {
                return false;  // raw line breaks are not allowed inside strings
            }
        }
        return false;
    }

    bool number() {
        size_t start = i;
        if (!eof() && (s[i] == '+' || s[i] == '-')) ++i;
        if (s.compare(i, 8, "Infinity") == 0) { i += 8; return true; }
        if (s.compare(i, 3, "NaN") == 0) { i += 3; return true; }
        if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
            i += 2;
            size_t d = i;
            while (!eof() && std::isxdigit(static_cast<unsigned char>(s[i]))) ++i;
            return i > d;
        }
        size_t int_digits = 0;
        while (!eof() && std::isdigit(static_cast<unsigned char>(s[i]))) { ++i; ++int_digits; }
        size_t frac_digits = 0;
        if (!eof() && s[i] == '.') {
            ++i;
            while (!eof() && std::isdigit(static_cast<unsigned char>(s[i]))) { ++i; ++frac_digits; }
        }
        if (int_digits == 0 && frac_digits == 0) { i = start; return false; }
        if (!eof() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (!eof() && (s[i] == '+' || s[i] == '-')) ++i;
            size_t e = i;
            while (!eof() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            if (i == e) return false;
        }
        return true;
    }

    bool value() {
        if (++depth > 200) return false;
        bool ok = value_inner();
        --depth;
        return ok;
    }

    bool value_inner() {
        if (!skip_ws() || eof()) return false;
        char c = cur();
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"' || c == '\'') return string_lit();
        if (c == '+' || c == '-' || c == '.' || std::isdigit(static_cast<unsigned char>(c))) return number();
        for (const char* kw : {"true", "false", "null", "Infinity", "NaN"}) {
            size_t n = std::string(kw).size();
            if (s.compare(i, n, kw) == 0 && (i + n >= s.size() || !is_id_part(static_cast<unsigned char>(s[i + n])))) { i += n; return true; }
        }
        return false;
    }

    bool array() {
        ++i;
        while (true) {
            if (!skip_ws() || eof()) return false;
            if (cur() == ']') { ++i; return true; }
            if (!value()) return false;
            if (!skip_ws() || eof()) return false;
            if (cur() == ',') { ++i; continue; }
            if (cur() == ']') { ++i; return true; }
            return false;
        }
    }

    bool object() {
        ++i;
        while (true) {
            if (!skip_ws() || eof()) return false;
            if (cur() == '}') { ++i; return true; }
            // key: string or identifier
            if (cur() == '"' || cur() == '\'') { if (!string_lit()) return false; }
            else if (is_id_start(static_cast<unsigned char>(cur())) || cur() == '\\') {
                while (!eof() && (is_id_part(static_cast<unsigned char>(cur())) || cur() == '\\')) ++i;
            } else return false;
            if (!skip_ws() || eof() || cur() != ':') return false;
            ++i;
            if (!value()) return false;
            if (!skip_ws() || eof()) return false;
            if (cur() == ',') { ++i; continue; }
            if (cur() == '}') { ++i; return true; }
            return false;
        }
    }
};

}  // namespace

bool json5_is_valid(const std::string& text) {
    P p(text);
    if (!p.value()) return false;
    if (!p.skip_ws()) return false;
    return p.eof();
}

}  // namespace mermaid
