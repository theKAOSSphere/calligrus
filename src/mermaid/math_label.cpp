// Mermaid port of layout/text.rs normalize_display_math / render_plain_math: "$$...$$" in a label becomes plain
// text (fractions as (a)/(b), sub / superscripts as Unicode, a few Greek letters). See text.h.
#include <string>
#include <vector>

#include "strutil.h"
#include "text.h"

namespace mermaid {

namespace {

using U32 = std::u32string;

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

U32 to_u32(const std::string& s) {
    auto v = decode_utf8(s);
    return U32(v.begin(), v.end());
}

std::string from_u32(const U32& s) {
    std::string out;
    for (char32_t c : s) append_utf8(out, c);
    return out;
}

bool is_ws(char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B || c == 0x0C || c == 0x85 || c == 0xA0 || c == 0x2028 || c == 0x2029 || c == 0x3000; }
bool is_ascii_alpha(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

std::string render_plain_math(const std::string& input);

std::optional<size_t> matching_brace(const U32& chars, size_t open) {
    if (open >= chars.size() || chars[open] != U'{') return std::nullopt;
    size_t depth = 0;
    for (size_t idx = open; idx < chars.size(); ++idx) {
        if (chars[idx] == U'{') ++depth;
        else if (chars[idx] == U'}') {
            depth = depth > 0 ? depth - 1 : 0;
            if (depth == 0) return idx;
        }
    }
    return std::nullopt;
}

std::optional<std::string> read_group(const U32& chars, size_t& idx) {
    while (idx < chars.size() && is_ws(chars[idx])) ++idx;
    auto end = matching_brace(chars, idx);
    if (!end) return std::nullopt;
    std::string inner = from_u32(chars.substr(idx + 1, *end - idx - 1));
    idx = *end + 1;
    return render_plain_math(inner);
}

std::optional<char32_t> script_char(char32_t ch, bool superscript) {
    static const char32_t sup_from[] = U"0123456789+-=()";
    static const char32_t sup_to[] = {0x2070, 0xB9, 0xB2, 0xB3, 0x2074, 0x2075, 0x2076, 0x2077, 0x2078, 0x2079, 0x207A, 0x207B, 0x207C, 0x207D, 0x207E};
    static const char32_t sub_to[] = {0x2080, 0x2081, 0x2082, 0x2083, 0x2084, 0x2085, 0x2086, 0x2087, 0x2088, 0x2089, 0x208A, 0x208B, 0x208C, 0x208D, 0x208E};
    for (size_t i = 0; i < 15; ++i)
        if (sup_from[i] == ch) return superscript ? sup_to[i] : sub_to[i];
    return std::nullopt;
}

std::string render_script(const std::string& value, bool superscript) {
    std::string out;
    bool all_mapped = true;
    for (char32_t ch : to_u32(value)) {
        if (auto m = script_char(ch, superscript)) append_utf8(out, *m);
        else {
            all_mapped = false;
            break;
        }
    }
    if (all_mapped && !out.empty()) return out;
    return std::string(superscript ? "^(" : "_(") + render_plain_math(value) + ")";
}

std::string render_plain_math(const std::string& input) {
    U32 chars = to_u32(input);
    std::string out;
    size_t idx = 0;
    while (idx < chars.size()) {
        char32_t c = chars[idx];
        if (c == U'\\') {
            if (idx + 1 < chars.size() && chars[idx + 1] == U'\\') {
                out += "; ";
                idx += 2;
                continue;
            }
            ++idx;
            size_t start = idx;
            while (idx < chars.size() && is_ascii_alpha(chars[idx])) ++idx;
            std::string command = from_u32(chars.substr(start, idx - start));
            if (command == "sqrt") {
                if (auto g = read_group(chars, idx)) out += "\xE2\x88\x9A(" + *g + ")";
            } else if (command == "frac") {
                std::string numerator = read_group(chars, idx).value_or("");
                std::string denominator = read_group(chars, idx).value_or("");
                out += "(" + numerator + ")/(" + denominator + ")";
            } else if (command == "text" || command == "overbrace") {
                if (auto g = read_group(chars, idx)) out += *g;
            } else if (command == "begin") {
                std::string env = read_group(chars, idx).value_or("");
                if (env.find("matrix") != std::string::npos) out += "[";
            } else if (command == "end") {
                std::string env = read_group(chars, idx).value_or("");
                if (env.find("matrix") != std::string::npos) out += "]";
            } else if (command == "pi") out += "\xCF\x80";
            else if (command == "alpha") out += "\xCE\xB1";
            else if (command == "beta") out += "\xCE\xB2";
            else if (command == "gamma") out += "\xCE\xB3";
            else if (command == "delta") out += "\xCE\xB4";
            else if (command == "lambda") out += "\xCE\xBB";
            else if (command == "mu") out += "\xCE\xBC";
            else if (command == "sigma") out += "\xCF\x83";
            else if (command == "theta") out += "\xCE\xB8";
            else if (command == "cos" || command == "sin" || command == "tan" || command == "log" || command == "ln" || command == "exp") out += command;
            else if (command == "left" || command == "right" || command == "cdot") {
            } else if (!command.empty()) out += command;
            else out += '\\';
        } else if (c == U'^' || c == U'_') {
            bool superscript = c == U'^';
            ++idx;
            std::string script;
            if (idx < chars.size() && chars[idx] == U'{') script = read_group(chars, idx).value_or("");
            else if (idx < chars.size()) {
                append_utf8(script, chars[idx]);
                ++idx;
            }
            out += render_script(script, superscript);
        } else if (c == U'{' || c == U'}') {
            ++idx;
        } else if (c == U'&') {
            out += ' ';
            ++idx;
        } else {
            append_utf8(out, c);
            ++idx;
        }
    }
    // split_whitespace().join(" ")
    U32 u = to_u32(out);
    std::string result;
    bool pending_space = false;
    for (char32_t ch : u) {
        if (is_ws(ch)) {
            pending_space = !result.empty();
            continue;
        }
        if (pending_space) result += ' ';
        pending_space = false;
        append_utf8(result, ch);
    }
    return result;
}

}  // namespace

std::string normalize_display_math(const std::string& text) {
    if (text.find("$$") == std::string::npos) return text;
    std::string out;
    std::string rest = text;
    while (true) {
        size_t start = rest.find("$$");
        if (start == std::string::npos) break;
        out += rest.substr(0, start);
        std::string after_start = rest.substr(start + 2);
        size_t end = after_start.find("$$");
        if (end != std::string::npos) {
            out += render_plain_math(after_start.substr(0, end));
            rest = after_start.substr(end + 2);
        } else {
            out += "$$";
            out += after_start;
            return out;
        }
    }
    out += rest;
    return out;
}

}  // namespace mermaid
