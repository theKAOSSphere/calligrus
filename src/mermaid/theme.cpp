// Mermaid port of theme.rs: colour helpers and the parts of Theme::modern() the ink theme keeps.
#include <cmath>
#include <cstdlib>

#include "config.h"
#include "numfmt.h"
#include "strutil.h"

namespace mermaid {

namespace {

float f32_clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The whole string must be a number (Rust str::parse::<f32>()).
bool parse_float(const std::string& text, float& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    out = std::strtof(text.c_str(), &end);
    return end == text.c_str() + text.size();
}

bool parse_hsl(const std::string& value_in, float& h, float& s, float& l) {
    std::string value = trim(value_in);
    size_t open = value.find('(');
    size_t close = value.rfind(')');
    if (open == std::string::npos || close == std::string::npos) return false;
    std::string prefix = to_ascii_lower(trim(value.substr(0, open)));
    if (prefix != "hsl" && prefix != "hsla") return false;
    if (close < open + 1) return false;
    std::string inner = value.substr(open + 1, close - open - 1);
    std::vector<std::string> parts;
    size_t pos = 0;
    while (true) {
        size_t j = inner.find(',', pos);
        if (j == std::string::npos) {
            parts.push_back(inner.substr(pos));
            break;
        }
        parts.push_back(inner.substr(pos, j - pos));
        pos = j + 1;
    }
    if (parts.size() < 3) return false;
    auto strip_pct = [](std::string t) {
        t = trim(t);
        while (!t.empty() && t.back() == '%') t.pop_back();
        return t;
    };
    return parse_float(trim(parts[0]), h) && parse_float(strip_pct(parts[1]), s) && parse_float(strip_pct(parts[2]), l);
}

bool hex_pair(const std::string& d, size_t at, int& out) {
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int a = hv(d[at]), b = hv(d[at + 1]);
    if (a < 0 || b < 0) return false;
    out = a * 16 + b;
    return true;
}

bool parse_hex(const std::string& value, float& r, float& g, float& b) {
    if (value.empty() || value[0] != '#') return false;
    std::string hex = value.substr(1);
    for (unsigned char c : hex)
        if (c >= 0x80) return false;
    std::string digits;
    switch (hex.size()) {
    case 3:
        for (char ch : hex) {
            digits += ch;
            digits += ch;
        }
        break;
    case 6: digits = hex; break;
    case 8: digits = hex.substr(0, 6); break;
    default: return false;
    }
    int ri, gi, bi;
    if (!hex_pair(digits, 0, ri) || !hex_pair(digits, 2, gi) || !hex_pair(digits, 4, bi)) return false;
    r = static_cast<float>(ri) / 255.0f;
    g = static_cast<float>(gi) / 255.0f;
    b = static_cast<float>(bi) / 255.0f;
    return true;
}

float fmax_(float a, float c) { return a > c ? a : (a < c ? c : a); }
float fmin_(float a, float c) { return a < c ? a : (a > c ? c : a); }

void rgb_to_hsl(float r, float g, float b, float& h_out, float& s_out, float& l_out) {
    float max = fmax_(r, fmax_(g, b));
    float min = fmin_(r, fmin_(g, b));
    float h = 0.0f;
    float l = (max + min) / 2.0f;
    float d = max - min;
    float s = d == 0.0f ? 0.0f : d / (1.0f - std::fabs(2.0f * l - 1.0f));
    if (d != 0.0f) {
        if (max == r) h = std::fmod((g - b) / d, 6.0f);
        else if (max == g) h = (b - r) / d + 2.0f;
        else h = (r - g) / d + 4.0f;
        h *= 60.0f;
        if (h < 0.0f) h += 360.0f;
    }
    h_out = h;
    s_out = s * 100.0f;
    l_out = l * 100.0f;
}

}  // namespace

bool parse_color_to_hsl(const std::string& color_in, float& h, float& s, float& l) {
    std::string color = trim(color_in);
    if (parse_hsl(color, h, s, l)) return true;
    float r, g, b;
    if (!parse_hex(color, r, g, b)) return false;
    rgb_to_hsl(r, g, b, h, s, l);
    return true;
}

std::string adjust_color(const std::string& color, float delta_h, float delta_s, float delta_l) {
    float h, s, l;
    if (!parse_color_to_hsl(color, h, s, l)) return color;
    h = h + delta_h;
    if (h < 0.0f) h = std::fmod(h, 360.0f) + 360.0f;
    else if (h >= 360.0f) h = std::fmod(h, 360.0f);
    s = f32_clamp(s + delta_s, 0.0f, 100.0f);
    l = f32_clamp(l + delta_l, 0.0f, 100.0f);
    return "hsl(" + fmt_fixed(h, 10) + ", " + fmt_fixed(s, 10) + "%, " + fmt_fixed(l, 10) + "%)";
}

Theme::Theme() {
    // Theme::modern(): the base colours the pie palette is derived from (the ink theme overrides the
    // visible colours afterwards but leaves pie_colors alone).
    const std::string primary = "#F8FAFC", secondary = "#E2E8F0", tertiary = "#FFFFFF";
    pie_colors = {primary,
                  secondary,
                  adjust_color(tertiary, 0.0f, 0.0f, -40.0f),
                  adjust_color(primary, 0.0f, 0.0f, -10.0f),
                  adjust_color(secondary, 0.0f, 0.0f, -30.0f),
                  adjust_color(tertiary, 0.0f, 0.0f, -20.0f),
                  adjust_color(primary, 60.0f, 0.0f, -20.0f),
                  adjust_color(primary, -60.0f, 0.0f, -40.0f),
                  adjust_color(primary, 120.0f, 0.0f, -40.0f),
                  adjust_color(primary, 60.0f, 0.0f, -40.0f),
                  adjust_color(primary, -90.0f, 0.0f, -40.0f),
                  adjust_color(primary, 120.0f, 0.0f, -30.0f)};
    git_colors = {"hsl(240, 100%, 46.2745098039%)", "hsl(60, 100%, 43.5294117647%)",  "hsl(80, 100%, 46.2745098039%)",
                  "hsl(210, 100%, 46.2745098039%)", "hsl(180, 100%, 46.2745098039%)", "hsl(150, 100%, 46.2745098039%)",
                  "hsl(300, 100%, 46.2745098039%)", "hsl(0, 100%, 46.2745098039%)"};
    git_inv_colors = {"hsl(60, 100%, 3.7254901961%)",
                      "rgb(0, 0, 160.5)",
                      "rgb(48.8333333334, 0, 146.5000000001)",
                      "rgb(146.5000000001, 73.2500000001, 0)",
                      "rgb(146.5000000001, 0, 0)",
                      "rgb(146.5000000001, 0, 73.2500000001)",
                      "rgb(0, 146.5000000001, 0)",
                      "rgb(0, 146.5000000001, 146.5000000001)"};
    git_branch_label_colors = {"#ffffff", "black", "black", "#ffffff", "black", "black", "black", "black"};
    git_commit_label_color = "#000021";
    git_commit_label_background = "#ffffde";
    git_tag_label_color = "#131300";
    git_tag_label_background = "#ECECFF";
    git_tag_label_border = "hsl(240, 60%, 86.2745098039%)";
}

}  // namespace mermaid
