// Mermaid port: number formatting that matches Rust's format! for f32.
#pragma once

#include <charconv>
#include <string>

namespace mermaid {

// `format!("{:.N}", v)`: fixed notation with N decimals, rounded on the exact binary value
// (ties to even), like std::to_chars with a precision.
inline std::string fmt_fixed(float v, int precision) {
    char buf[128];
    auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, precision);
    return std::string(buf, r.ptr);
}

// `format!("{}", v)`: shortest decimal that parses back to the same f32, never in exponent form.
// One difference to std::to_chars: when the exact value lies exactly halfway between two
// shortest candidates (173.328125 -> ...12 or ...13) Rust picks the upper one, to_chars the even one.
inline std::string fmt_shortest(float v) {
    char buf[128];
    auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed);
    std::string s(buf, r.ptr);
    size_t dot = s.find('.');
    if (dot == std::string::npos) return s;  // whole numbers print without a decimal point
    // Exact decimal expansion of the float (it terminates, so a long precision is exact).
    char ebuf[512];
    auto er = std::to_chars(ebuf, ebuf + sizeof ebuf, v, std::chars_format::fixed, 120);
    std::string exact(ebuf, er.ptr);
    if (exact.compare(0, s.size(), s) != 0) return s;  // shortest digits are not a prefix: not the tie case
    std::string rest = exact.substr(s.size());
    bool tie = !rest.empty() && rest[0] == '5';
    for (size_t i = 1; tie && i < rest.size(); ++i) tie = rest[i] == '0';
    if (!tie) return s;
    // Round the last digit up (away from zero), propagating a carry over 9s.
    std::string up = s;
    size_t i = up.size();
    bool carry = true;
    while (carry && i-- > 0) {
        if (up[i] == '.' || up[i] == '-') continue;
        if (up[i] == '9') up[i] = '0';
        else { ++up[i]; carry = false; }
    }
    if (carry) up.insert(up[0] == '-' ? 1 : 0, "1");
    float back = 0.0f;
    auto fr = std::from_chars(up.data(), up.data() + up.size(), back);
    if (fr.ec != std::errc() || back != v) return s;  // the upper candidate must round-trip
    return up;
}

}  // namespace mermaid
