// Mermaid port: SVG subset rasteriser (see svg_raster.h).
//
// Reading guide: (1) a tiny XML reader, (2) attribute values (numbers, colours, transforms, path
// data), (3) geometry: paths, flattening, stroking, dashing, (4) compositing coverage into the
// canvas with tiny-skia's 8-bit "lowp" blend, (5) walking the tree, text, and the final ink() step.
#include "svg_raster.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>

#include "../texmath/canvas.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>
#include "ir.h"
#include "strutil.h"

namespace mermaid {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// ======================================================================================
// (1) XML
// ======================================================================================

struct XNode {
    std::string tag;  // empty for a text node
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XNode> children;
    std::string text;  // text nodes only (entities decoded)

    const std::string* attr(const std::string& name) const {
        for (const auto& [k, v] : attrs)
            if (k == name) return &v;
        return nullptr;
    }
};

void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
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

std::string decode_entities(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) { out += s[i]; continue; }
        std::string_view ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')
                              ? static_cast<unsigned>(std::strtoul(std::string(ent.substr(2)).c_str(), nullptr, 16))
                              : static_cast<unsigned>(std::strtoul(std::string(ent.substr(1)).c_str(), nullptr, 10));
            append_utf8(out, cp);
        } else {
            out += s[i];
            continue;
        }
        i = semi;
    }
    return out;
}

class XmlReader {
public:
    explicit XmlReader(std::string_view s) : s_(s) {}

    XNode parse_document() {
        XNode root;
        root.tag = "#document";
        parse_children(root);
        if (root.children.empty()) throw ParseError("empty SVG");
        return root;
    }

private:
    std::string_view s_;
    size_t i_ = 0;

    bool starts(std::string_view p) const { return s_.substr(i_, p.size()) == p; }

    void parse_children(XNode& parent) {
        while (i_ < s_.size()) {
            if (s_[i_] != '<') {
                size_t j = s_.find('<', i_);
                if (j == std::string_view::npos) j = s_.size();
                XNode t;
                t.text = decode_entities(s_.substr(i_, j - i_));
                parent.children.push_back(std::move(t));
                i_ = j;
                continue;
            }
            if (starts("</")) {  // end tag: the caller consumes it
                return;
            }
            if (starts("<!--")) { size_t j = s_.find("-->", i_); i_ = j == std::string_view::npos ? s_.size() : j + 3; continue; }
            if (starts("<?") || starts("<!")) { size_t j = s_.find('>', i_); i_ = j == std::string_view::npos ? s_.size() : j + 1; continue; }
            parent.children.push_back(parse_element());
        }
    }

    XNode parse_element() {
        ++i_;  // '<'
        XNode n;
        size_t start = i_;
        while (i_ < s_.size() && !is_space(s_[i_]) && s_[i_] != '>' && s_[i_] != '/') ++i_;
        n.tag = std::string(s_.substr(start, i_ - start));
        for (;;) {
            while (i_ < s_.size() && is_space(s_[i_])) ++i_;
            if (i_ >= s_.size()) throw ParseError("unterminated SVG tag");
            if (s_[i_] == '/') {  // self-closing
                i_ += 2;
                return n;
            }
            if (s_[i_] == '>') { ++i_; break; }
            size_t ks = i_;
            while (i_ < s_.size() && s_[i_] != '=' && !is_space(s_[i_]) && s_[i_] != '>') ++i_;
            std::string key(s_.substr(ks, i_ - ks));
            while (i_ < s_.size() && is_space(s_[i_])) ++i_;
            if (i_ < s_.size() && s_[i_] == '=') {
                ++i_;
                while (i_ < s_.size() && is_space(s_[i_])) ++i_;
                char q = i_ < s_.size() ? s_[i_] : '"';
                if (q != '"' && q != '\'') throw ParseError("unquoted SVG attribute");
                size_t vs = ++i_;
                size_t ve = s_.find(q, vs);
                if (ve == std::string_view::npos) throw ParseError("unterminated SVG attribute");
                n.attrs.emplace_back(std::move(key), decode_entities(s_.substr(vs, ve - vs)));
                i_ = ve + 1;
            } else {
                n.attrs.emplace_back(std::move(key), "");
            }
        }
        parse_children(n);
        // end tag
        if (starts("</")) {
            size_t j = s_.find('>', i_);
            i_ = j == std::string_view::npos ? s_.size() : j + 1;
        }
        return n;
    }
};

// ======================================================================================
// (2) values
// ======================================================================================

struct Mat {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;  // x' = a x + c y + e,  y' = b x + d y + f
};
// first apply `inner`, then `outer`
Mat mul(const Mat& outer, const Mat& inner) {
    return {outer.a * inner.a + outer.c * inner.b, outer.b * inner.a + outer.d * inner.b,
            outer.a * inner.c + outer.c * inner.d, outer.b * inner.c + outer.d * inner.d,
            outer.a * inner.e + outer.c * inner.f + outer.e, outer.b * inner.e + outer.d * inner.f + outer.f};
}
struct P {
    float x = 0, y = 0;
};
P apply(const Mat& m, P p) { return {m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f}; }

// Numbers in a list ("1,2 3-4 .5e2"): returns nullopt on junk.
std::optional<std::vector<float>> parse_numbers(std::string_view s) {
    std::vector<float> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (is_space(s[i]) || s[i] == ',')) ++i;
        if (i >= s.size()) break;
        std::string tmp(s.substr(i, 40));
        char* end = nullptr;
        float v = std::strtof(tmp.c_str(), &end);
        if (end == tmp.c_str()) return std::nullopt;
        out.push_back(v);
        i += static_cast<size_t>(end - tmp.c_str());
    }
    return out;
}

// A length such as "14", "14px" (other units are not used by the writer).
std::optional<float> parse_length(const std::string* s) {
    if (!s) return std::nullopt;
    std::string t = trim(*s);
    if (ends_with(t, "px")) t.resize(t.size() - 2);
    char* end = nullptr;
    float v = std::strtof(t.c_str(), &end);
    if (t.empty() || end != t.c_str() + t.size()) return std::nullopt;
    return v;
}

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
};

// The CSS colour keywords (SVG 1.1 / CSS3 list).
std::optional<Rgb> named_color(const std::string& n) {
    static const std::map<std::string, Rgb> table = {
        {"aliceblue", {240, 248, 255}}, {"antiquewhite", {250, 235, 215}}, {"aqua", {0, 255, 255}},
        {"aquamarine", {127, 255, 212}}, {"azure", {240, 255, 255}}, {"beige", {245, 245, 220}},
        {"bisque", {255, 228, 196}}, {"black", {0, 0, 0}}, {"blanchedalmond", {255, 235, 205}}, {"blue", {0, 0, 255}},
        {"blueviolet", {138, 43, 226}}, {"brown", {165, 42, 42}}, {"burlywood", {222, 184, 135}},
        {"cadetblue", {95, 158, 160}}, {"chartreuse", {127, 255, 0}}, {"chocolate", {210, 105, 30}},
        {"coral", {255, 127, 80}}, {"cornflowerblue", {100, 149, 237}}, {"cornsilk", {255, 248, 220}},
        {"crimson", {220, 20, 60}}, {"cyan", {0, 255, 255}}, {"darkblue", {0, 0, 139}}, {"darkcyan", {0, 139, 139}},
        {"darkgoldenrod", {184, 134, 11}}, {"darkgray", {169, 169, 169}}, {"darkgreen", {0, 100, 0}},
        {"darkgrey", {169, 169, 169}}, {"darkkhaki", {189, 183, 107}}, {"darkmagenta", {139, 0, 139}},
        {"darkolivegreen", {85, 107, 47}}, {"darkorange", {255, 140, 0}}, {"darkorchid", {153, 50, 204}},
        {"darkred", {139, 0, 0}}, {"darksalmon", {233, 150, 122}}, {"darkseagreen", {143, 188, 143}},
        {"darkslateblue", {72, 61, 139}}, {"darkslategray", {47, 79, 79}}, {"darkslategrey", {47, 79, 79}},
        {"darkturquoise", {0, 206, 209}}, {"darkviolet", {148, 0, 211}}, {"deeppink", {255, 20, 147}},
        {"deepskyblue", {0, 191, 255}}, {"dimgray", {105, 105, 105}}, {"dimgrey", {105, 105, 105}},
        {"dodgerblue", {30, 144, 255}}, {"firebrick", {178, 34, 34}}, {"floralwhite", {255, 250, 240}},
        {"forestgreen", {34, 139, 34}}, {"fuchsia", {255, 0, 255}}, {"gainsboro", {220, 220, 220}},
        {"ghostwhite", {248, 248, 255}}, {"gold", {255, 215, 0}}, {"goldenrod", {218, 165, 32}},
        {"gray", {128, 128, 128}}, {"green", {0, 128, 0}}, {"greenyellow", {173, 255, 47}}, {"grey", {128, 128, 128}},
        {"honeydew", {240, 255, 240}}, {"hotpink", {255, 105, 180}}, {"indianred", {205, 92, 92}},
        {"indigo", {75, 0, 130}}, {"ivory", {255, 255, 240}}, {"khaki", {240, 230, 140}},
        {"lavender", {230, 230, 250}}, {"lavenderblush", {255, 240, 245}}, {"lawngreen", {124, 252, 0}},
        {"lemonchiffon", {255, 250, 205}}, {"lightblue", {173, 216, 230}}, {"lightcoral", {240, 128, 128}},
        {"lightcyan", {224, 255, 255}}, {"lightgoldenrodyellow", {250, 250, 210}}, {"lightgray", {211, 211, 211}},
        {"lightgreen", {144, 238, 144}}, {"lightgrey", {211, 211, 211}}, {"lightpink", {255, 182, 193}},
        {"lightsalmon", {255, 160, 122}}, {"lightseagreen", {32, 178, 170}}, {"lightskyblue", {135, 206, 250}},
        {"lightslategray", {119, 136, 153}}, {"lightslategrey", {119, 136, 153}}, {"lightsteelblue", {176, 196, 222}},
        {"lightyellow", {255, 255, 224}}, {"lime", {0, 255, 0}}, {"limegreen", {50, 205, 50}},
        {"linen", {250, 240, 230}}, {"magenta", {255, 0, 255}}, {"maroon", {128, 0, 0}},
        {"mediumaquamarine", {102, 205, 170}}, {"mediumblue", {0, 0, 205}}, {"mediumorchid", {186, 85, 211}},
        {"mediumpurple", {147, 112, 219}}, {"mediumseagreen", {60, 179, 113}}, {"mediumslateblue", {123, 104, 238}},
        {"mediumspringgreen", {0, 250, 154}}, {"mediumturquoise", {72, 209, 204}},
        {"mediumvioletred", {199, 21, 133}}, {"midnightblue", {25, 25, 112}}, {"mintcream", {245, 255, 250}},
        {"mistyrose", {255, 228, 225}}, {"moccasin", {255, 228, 181}}, {"navajowhite", {255, 222, 173}},
        {"navy", {0, 0, 128}}, {"oldlace", {253, 245, 230}}, {"olive", {128, 128, 0}}, {"olivedrab", {107, 142, 35}},
        {"orange", {255, 165, 0}}, {"orangered", {255, 69, 0}}, {"orchid", {218, 112, 214}},
        {"palegoldenrod", {238, 232, 170}}, {"palegreen", {152, 251, 152}}, {"paleturquoise", {175, 238, 238}},
        {"palevioletred", {219, 112, 147}}, {"papayawhip", {255, 239, 213}}, {"peachpuff", {255, 218, 185}},
        {"peru", {205, 133, 63}}, {"pink", {255, 192, 203}}, {"plum", {221, 160, 221}},
        {"powderblue", {176, 224, 230}}, {"purple", {128, 0, 128}}, {"rebeccapurple", {102, 51, 153}},
        {"red", {255, 0, 0}}, {"rosybrown", {188, 143, 143}}, {"royalblue", {65, 105, 225}},
        {"saddlebrown", {139, 69, 19}}, {"salmon", {250, 128, 114}}, {"sandybrown", {244, 164, 96}},
        {"seagreen", {46, 139, 87}}, {"seashell", {255, 245, 238}}, {"sienna", {160, 82, 45}},
        {"silver", {192, 192, 192}}, {"skyblue", {135, 206, 235}}, {"slateblue", {106, 90, 205}},
        {"slategray", {112, 128, 144}}, {"slategrey", {112, 128, 144}}, {"snow", {255, 250, 250}},
        {"springgreen", {0, 255, 127}}, {"steelblue", {70, 130, 180}}, {"tan", {210, 180, 140}},
        {"teal", {0, 128, 128}}, {"thistle", {216, 191, 216}}, {"tomato", {255, 99, 71}},
        {"turquoise", {64, 224, 208}}, {"violet", {238, 130, 238}}, {"wheat", {245, 222, 179}},
        {"white", {255, 255, 255}}, {"whitesmoke", {245, 245, 245}}, {"yellow", {255, 255, 0}},
        {"yellowgreen", {154, 205, 50}},
    };
    auto it = table.find(to_ascii_lower(n));
    if (it == table.end()) return std::nullopt;
    return it->second;
}

struct Paint {
    bool none = false;
    Rgb rgb;
    float alpha = 1.0f;  // the colour's own alpha (rgba(), hsla(), #rrggbbaa), multiplied into the opacity
    std::string url;     // paint server reference ("url(#id)"): a <linearGradient>; rgb is unused then
};

// The arguments of a CSS colour function: numbers with an optional '%' (svgtypes parse_list_number_or_percent).
struct ColorArg {
    double value = 0;
    bool percent = false;
};
std::optional<std::vector<ColorArg>> parse_color_args(std::string_view s) {
    std::vector<ColorArg> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (is_space(s[i]) || s[i] == ',' || s[i] == '/')) ++i;
        if (i >= s.size()) break;
        std::string tmp(s.substr(i, 40));
        char* end = nullptr;
        double v = std::strtod(tmp.c_str(), &end);
        if (end == tmp.c_str()) return std::nullopt;
        i += static_cast<size_t>(end - tmp.c_str());
        ColorArg a{v, false};
        if (i < s.size() && s[i] == '%') { a.percent = true; ++i; }
        out.push_back(a);
    }
    return out;
}

uint8_t round_u8(float v) { return static_cast<uint8_t>(std::clamp(std::round(v), 0.0f, 255.0f)); }  // Rust `x.round() as u8`

// svgtypes color.rs hsl_to_rgb (hue 0..6)
float hue_to_rgb(float t1, float t2, float hue) {
    if (hue < 0.0f) hue += 6.0f;
    if (hue >= 6.0f) hue -= 6.0f;
    if (hue < 1.0f) return (t2 - t1) * hue + t1;
    if (hue < 3.0f) return t2;
    if (hue < 4.0f) return (t2 - t1) * (4.0f - hue) + t1;
    return t1;
}
Rgb hsl_to_rgb(float hue, float saturation, float lightness) {
    float t2 = lightness <= 0.5f ? lightness * (saturation + 1.0f) : lightness + saturation - (lightness * saturation);
    float t1 = lightness * 2.0f - t2;
    return {round_u8(hue_to_rgb(t1, t2, hue + 2.0f) * 255.0f), round_u8(hue_to_rgb(t1, t2, hue) * 255.0f),
            round_u8(hue_to_rgb(t1, t2, hue - 2.0f) * 255.0f)};
}

// A paint value svgtypes would reject.
struct BadPaint {};

Paint parse_paint(const std::string& raw) {
    std::string s = trim(raw);
    Paint p;
    if (s == "none" || s == "transparent") { p.none = true; return p; }
    if (starts_with(s, "url(#") && ends_with(s, ")")) {
        p.url = s.substr(5, s.size() - 6);
        return p;
    }
    if (!s.empty() && s[0] == '#') {
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        std::string h = s.substr(1);
        for (char c : h)
            if (hex(c) < 0) throw BadPaint();
        if (h.size() == 3) {
            p.rgb = {static_cast<uint8_t>(hex(h[0]) * 17), static_cast<uint8_t>(hex(h[1]) * 17), static_cast<uint8_t>(hex(h[2]) * 17)};
            return p;
        }
        if (h.size() == 6) {
            p.rgb = {static_cast<uint8_t>(hex(h[0]) * 16 + hex(h[1])), static_cast<uint8_t>(hex(h[2]) * 16 + hex(h[3])),
                     static_cast<uint8_t>(hex(h[4]) * 16 + hex(h[5]))};
            return p;
        }
        throw BadPaint();
    }
    // rgb(), rgba(), hsl(), hsla() (svgtypes Stream::parse_color)
    size_t paren = s.find('(');
    if (paren != std::string::npos && ends_with(s, ")")) {
        std::string name = to_ascii_lower(trim(s.substr(0, paren)));
        auto args = parse_color_args(std::string_view(s).substr(paren + 1, s.size() - paren - 2));
        if (!args || args->size() < 3 || args->size() > 4) throw BadPaint();
        const auto& a = *args;
        if (name == "rgb" || name == "rgba") {
            if (a[0].percent) {
                // percentages: every component is a percentage (the first is scaled like the others)
                p.rgb = {round_u8(static_cast<float>((a[0].value / 100.0) * 255.0)), round_u8(static_cast<float>((a[1].percent ? a[1].value / 100.0 : a[1].value) * 255.0)),
                         round_u8(static_cast<float>((a[2].percent ? a[2].value / 100.0 : a[2].value) * 255.0))};
            } else {
                p.rgb = {round_u8(static_cast<float>(a[0].value)), round_u8(static_cast<float>(a[1].value)), round_u8(static_cast<float>(a[2].value))};
            }
        } else if (name == "hsl" || name == "hsla") {
            double hue = std::fmod(std::fmod(a[0].value, 360.0) + 360.0, 360.0);
            double sat = std::clamp(a[1].percent ? a[1].value / 100.0 : a[1].value, 0.0, 1.0);
            double lig = std::clamp(a[2].percent ? a[2].value / 100.0 : a[2].value, 0.0, 1.0);
            p.rgb = hsl_to_rgb(static_cast<float>(hue) / 60.0f, static_cast<float>(sat), static_cast<float>(lig));
        } else {
            throw BadPaint();
        }
        if (a.size() == 4) p.alpha = static_cast<float>(round_u8(static_cast<float>(a[3].value * 255.0))) / 255.0f;
        return p;
    }
    if (auto c = named_color(s)) { p.rgb = *c; return p; }
    throw BadPaint();
}

Mat parse_transform(const std::string& s) {
    Mat total;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (is_space(s[i]) || s[i] == ',')) ++i;
        if (i >= s.size()) break;
        size_t open = s.find('(', i);
        size_t close = s.find(')', i);
        if (open == std::string::npos || close == std::string::npos) throw Unsupported("transform " + s);
        std::string name = trim(s.substr(i, open - i));
        auto nums = parse_numbers(std::string_view(s).substr(open + 1, close - open - 1));
        if (!nums) throw Unsupported("transform " + s);
        const auto& v = *nums;
        Mat m;
        if (name == "translate" && !v.empty()) { m.e = v[0]; m.f = v.size() > 1 ? v[1] : 0.0f; }
        else if (name == "scale" && !v.empty()) { m.a = v[0]; m.d = v.size() > 1 ? v[1] : v[0]; }
        else if (name == "rotate" && !v.empty()) {
            float rad = v[0] * kPi / 180.0f;
            float cs = std::cos(rad), sn = std::sin(rad);
            Mat r{cs, sn, -sn, cs, 0, 0};
            if (v.size() == 3) {
                Mat t1;
                t1.e = v[1];
                t1.f = v[2];
                Mat t2;
                t2.e = -v[1];
                t2.f = -v[2];
                m = mul(t1, mul(r, t2));
            } else {
                m = r;
            }
        } else if (name == "matrix" && v.size() == 6) m = {v[0], v[1], v[2], v[3], v[4], v[5]};
        else if (name == "skewX" && !v.empty()) m.c = std::tan(v[0] * kPi / 180.0f);
        else if (name == "skewY" && !v.empty()) m.b = std::tan(v[0] * kPi / 180.0f);
        else throw Unsupported("transform " + name);
        total = mul(total, m);
        i = close + 1;
    }
    return total;
}

// ======================================================================================
// (3) geometry
// ======================================================================================

enum class Cmd : uint8_t { M, L, C, Z };

// A path in user space: M x y | L x y | C x1 y1 x2 y2 x y | Z.
struct VPath {
    std::vector<Cmd> cmds;
    std::vector<P> pts;
    void move_to(P p) { cmds.push_back(Cmd::M); pts.push_back(p); }
    void line_to(P p) { cmds.push_back(Cmd::L); pts.push_back(p); }
    void cubic_to(P a, P b, P c) { cmds.push_back(Cmd::C); pts.push_back(a); pts.push_back(b); pts.push_back(c); }
    void close() { cmds.push_back(Cmd::Z); }
    bool empty() const { return cmds.empty(); }
};

// SVG arc (endpoint form) -> cubic Béziers appended to `path`; `cur` is the start point.
void arc_to(VPath& path, P cur, float rx, float ry, float rot_deg, bool large, bool sweep, P end) {
    if (cur.x == end.x && cur.y == end.y) return;
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    if (rx == 0.0f || ry == 0.0f) { path.line_to(end); return; }
    float phi = rot_deg * kPi / 180.0f;
    float cp = std::cos(phi), sp = std::sin(phi);
    float dx2 = (cur.x - end.x) / 2.0f, dy2 = (cur.y - end.y) / 2.0f;
    float x1p = cp * dx2 + sp * dy2, y1p = -sp * dx2 + cp * dy2;
    float lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lam > 1.0f) { float s = std::sqrt(lam); rx *= s; ry *= s; }
    float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    float coef = den == 0.0f ? 0.0f : std::sqrt(std::max(0.0f, num / den));
    if (large == sweep) coef = -coef;
    float cxp = coef * (rx * y1p / ry), cyp = coef * -(ry * x1p / rx);
    float cx = cp * cxp - sp * cyp + (cur.x + end.x) / 2.0f;
    float cy = sp * cxp + cp * cyp + (cur.y + end.y) / 2.0f;
    auto angle = [](float ux, float uy, float vx, float vy) {
        float a = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        return a;
    };
    float th1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
    float dth = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
    if (!sweep && dth > 0) dth -= 2.0f * kPi;
    if (sweep && dth < 0) dth += 2.0f * kPi;
    int n = std::max(1, static_cast<int>(std::ceil(std::fabs(dth) / (kPi / 2.0f) - 1e-4f)));
    float step = dth / static_cast<float>(n);
    float k = 4.0f / 3.0f * std::tan(step / 4.0f);
    auto point = [&](float th, float& x, float& y, float& dxdt, float& dydt) {
        float ct = std::cos(th), st = std::sin(th);
        x = cx + rx * ct * cp - ry * st * sp;
        y = cy + rx * ct * sp + ry * st * cp;
        dxdt = -rx * st * cp - ry * ct * sp;
        dydt = -rx * st * sp + ry * ct * cp;
    };
    float th = th1;
    for (int i = 0; i < n; ++i) {
        float x0, y0, dx0, dy0, x3, y3, dx3, dy3;
        point(th, x0, y0, dx0, dy0);
        point(th + step, x3, y3, dx3, dy3);
        P c1{x0 + k * dx0, y0 + k * dy0}, c2{x3 - k * dx3, y3 - k * dy3};
        P e{x3, y3};
        if (i == n - 1) e = end;  // land exactly on the end point
        path.cubic_to(c1, c2, e);
        th += step;
    }
}

// SVG path data (d attribute).
VPath parse_path_data(const std::string& d) {
    VPath path;
    size_t i = 0;
    P cur, start, last_ctrl;
    char cmd = 0, prev_cmd = 0;
    auto skip = [&]() { while (i < d.size() && (is_space(d[i]) || d[i] == ',')) ++i; };
    auto number = [&](float& out) {
        skip();
        std::string tmp = d.substr(i, 40);
        char* end = nullptr;
        out = std::strtof(tmp.c_str(), &end);
        if (end == tmp.c_str()) throw Unsupported("bad path data");
        i += static_cast<size_t>(end - tmp.c_str());
    };
    auto flag = [&](bool& out) {
        skip();
        if (i >= d.size() || (d[i] != '0' && d[i] != '1')) throw Unsupported("bad arc flag");
        out = d[i++] == '1';
    };
    for (;;) {
        skip();
        if (i >= d.size()) break;
        char c = d[i];
        if (std::isalpha(static_cast<unsigned char>(c))) { cmd = c; ++i; }
        else if (cmd == 0) throw Unsupported("bad path data");
        else if (cmd == 'M') cmd = 'L';
        else if (cmd == 'm') cmd = 'l';
        bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
        char up = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
        float v[7];
        auto abs_pt = [&](float x, float y) { return rel ? P{cur.x + x, cur.y + y} : P{x, y}; };
        switch (up) {
            case 'M': { number(v[0]); number(v[1]); cur = abs_pt(v[0], v[1]); start = cur; path.move_to(cur); break; }
            case 'L': { number(v[0]); number(v[1]); cur = abs_pt(v[0], v[1]); path.line_to(cur); break; }
            case 'H': { number(v[0]); cur.x = rel ? cur.x + v[0] : v[0]; path.line_to(cur); break; }
            case 'V': { number(v[0]); cur.y = rel ? cur.y + v[0] : v[0]; path.line_to(cur); break; }
            case 'C': {
                for (int k = 0; k < 6; ++k) number(v[k]);
                P a = abs_pt(v[0], v[1]), b = abs_pt(v[2], v[3]), e = abs_pt(v[4], v[5]);
                path.cubic_to(a, b, e);
                last_ctrl = b;
                cur = e;
                break;
            }
            case 'S': {
                for (int k = 0; k < 4; ++k) number(v[k]);
                P a = (prev_cmd == 'C' || prev_cmd == 'S') ? P{2 * cur.x - last_ctrl.x, 2 * cur.y - last_ctrl.y} : cur;
                P b = abs_pt(v[0], v[1]), e = abs_pt(v[2], v[3]);
                path.cubic_to(a, b, e);
                last_ctrl = b;
                cur = e;
                break;
            }
            case 'Q': {
                for (int k = 0; k < 4; ++k) number(v[k]);
                P q = abs_pt(v[0], v[1]), e = abs_pt(v[2], v[3]);
                path.cubic_to({cur.x + 2.0f / 3.0f * (q.x - cur.x), cur.y + 2.0f / 3.0f * (q.y - cur.y)},
                              {e.x + 2.0f / 3.0f * (q.x - e.x), e.y + 2.0f / 3.0f * (q.y - e.y)}, e);
                last_ctrl = q;
                cur = e;
                break;
            }
            case 'T': {
                for (int k = 0; k < 2; ++k) number(v[k]);
                P q = (prev_cmd == 'Q' || prev_cmd == 'T') ? P{2 * cur.x - last_ctrl.x, 2 * cur.y - last_ctrl.y} : cur;
                P e = abs_pt(v[0], v[1]);
                path.cubic_to({cur.x + 2.0f / 3.0f * (q.x - cur.x), cur.y + 2.0f / 3.0f * (q.y - cur.y)},
                              {e.x + 2.0f / 3.0f * (q.x - e.x), e.y + 2.0f / 3.0f * (q.y - e.y)}, e);
                last_ctrl = q;
                cur = e;
                break;
            }
            case 'A': {
                bool large, sweep;
                number(v[0]); number(v[1]); number(v[2]); flag(large); flag(sweep); number(v[3]); number(v[4]);
                P e = abs_pt(v[3], v[4]);
                arc_to(path, cur, v[0], v[1], v[2], large, sweep, e);
                cur = e;
                break;
            }
            case 'Z': { path.close(); cur = start; break; }
            default: throw Unsupported(std::string("path command ") + cmd);
        }
        prev_cmd = up;
    }
    return path;
}

VPath rect_path(float x, float y, float w, float h, float rx, float ry) {
    VPath p;
    if (rx <= 0.0f && ry <= 0.0f) {
        p.move_to({x, y});
        p.line_to({x + w, y});
        p.line_to({x + w, y + h});
        p.line_to({x, y + h});
        p.close();
        return p;
    }
    if (rx <= 0.0f) rx = ry;
    if (ry <= 0.0f) ry = rx;
    rx = std::min(rx, w / 2.0f);
    ry = std::min(ry, h / 2.0f);
    p.move_to({x + rx, y});
    p.line_to({x + w - rx, y});
    arc_to(p, {x + w - rx, y}, rx, ry, 0, false, true, {x + w, y + ry});
    p.line_to({x + w, y + h - ry});
    arc_to(p, {x + w, y + h - ry}, rx, ry, 0, false, true, {x + w - rx, y + h});
    p.line_to({x + rx, y + h});
    arc_to(p, {x + rx, y + h}, rx, ry, 0, false, true, {x, y + h - ry});
    p.line_to({x, y + ry});
    arc_to(p, {x, y + ry}, rx, ry, 0, false, true, {x + rx, y});
    p.close();
    return p;
}

VPath ellipse_path(float cx, float cy, float rx, float ry) {
    VPath p;
    p.move_to({cx + rx, cy});
    arc_to(p, {cx + rx, cy}, rx, ry, 0, false, true, {cx, cy + ry});
    arc_to(p, {cx, cy + ry}, rx, ry, 0, false, true, {cx - rx, cy});
    arc_to(p, {cx - rx, cy}, rx, ry, 0, false, true, {cx, cy - ry});
    arc_to(p, {cx, cy - ry}, rx, ry, 0, false, true, {cx + rx, cy});
    p.close();
    return p;
}

// ---- flattening (device space) ----

struct Polyline {
    std::vector<P> pts;
    bool closed = false;
};

void flatten_cubic(std::vector<P>& out, P p0, P p1, P p2, P p3) {
    float ddx = std::max(std::fabs(p0.x - 2 * p1.x + p2.x), std::fabs(p1.x - 2 * p2.x + p3.x));
    float ddy = std::max(std::fabs(p0.y - 2 * p1.y + p2.y), std::fabs(p1.y - 2 * p2.y + p3.y));
    float dd = std::sqrt(ddx * ddx + ddy * ddy);
    int n = static_cast<int>(std::ceil(std::sqrt(3.0f * dd / (4.0f * 0.02f))));
    n = std::clamp(n, 1, 200);
    for (int i = 1; i <= n; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(n), u = 1.0f - t;
        float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
        out.push_back({b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x, b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y});
    }
}

std::vector<Polyline> flatten(const VPath& path, const Mat& m) {
    std::vector<Polyline> out;
    size_t pi = 0;
    Polyline cur;
    auto flush = [&]() {
        if (!cur.pts.empty()) out.push_back(std::move(cur));
        cur = Polyline{};
    };
    P start;
    for (Cmd c : path.cmds) {
        switch (c) {
            case Cmd::M: flush(); cur.pts.push_back(apply(m, path.pts[pi++])); start = cur.pts.back(); break;
            case Cmd::L: if (cur.pts.empty()) cur.pts.push_back(start); cur.pts.push_back(apply(m, path.pts[pi++])); break;
            case Cmd::C: {
                if (cur.pts.empty()) cur.pts.push_back(start);
                P p1 = apply(m, path.pts[pi]), p2 = apply(m, path.pts[pi + 1]), p3 = apply(m, path.pts[pi + 2]);
                pi += 3;
                flatten_cubic(cur.pts, cur.pts.back(), p1, p2, p3);
                break;
            }
            case Cmd::Z: cur.closed = true; flush(); cur.pts.clear(); break;
        }
    }
    flush();
    return out;
}

// ---- dashing ----

std::vector<Polyline> dash(const std::vector<Polyline>& in, const std::vector<float>& pattern, float offset) {
    std::vector<Polyline> out;
    float total = 0.0f;
    for (float v : pattern) total += v;
    for (const Polyline& pl : in) {
        std::vector<P> pts = pl.pts;
        if (pl.closed && pts.size() > 1) pts.push_back(pts[0]);
        // start inside the pattern at `offset`
        size_t idx = 0;
        float into = std::fmod(offset, total);
        if (into < 0) into += total;
        while (into >= pattern[idx % pattern.size()]) { into -= pattern[idx % pattern.size()]; ++idx; }
        float remain = pattern[idx % pattern.size()] - into;
        bool on = idx % 2 == 0;
        Polyline piece;
        if (on && !pts.empty()) piece.pts.push_back(pts[0]);
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            P a = pts[i], b = pts[i + 1];
            float len = std::hypot(b.x - a.x, b.y - a.y);
            float pos = 0.0f;
            while (len - pos > remain) {
                pos += remain;
                float t = len > 0 ? pos / len : 0.0f;
                P q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                if (on) {
                    piece.pts.push_back(q);
                    out.push_back(std::move(piece));
                    piece = Polyline{};
                } else {
                    piece.pts.push_back(q);
                }
                on = !on;
                ++idx;
                remain = pattern[idx % pattern.size()];
                if (remain <= 0.0f) remain = 1e-4f;
            }
            remain -= (len - pos);
            if (on) piece.pts.push_back(b);
        }
        if (on && piece.pts.size() > 1) out.push_back(std::move(piece));
    }
    return out;
}

// ---- stroking: every piece is a convex polygon with the same orientation, filled with the
// non-zero rule, so their union is the stroke. ----

struct StrokeStyle {
    float width = 1.0f;
    int cap = 0;   // 0 butt, 1 round, 2 square
    int join = 0;  // 0 miter, 1 round, 2 bevel
    float miter_limit = 4.0f;
};

using Poly = std::vector<P>;

float signed_area(const Poly& p) {
    float a = 0.0f;
    for (size_t i = 0; i < p.size(); ++i) {
        const P &u = p[i], &v = p[(i + 1) % p.size()];
        a += u.x * v.y - v.x * u.y;
    }
    return a * 0.5f;
}

void push_poly(std::vector<Poly>& polys, Poly p) {
    if (p.size() < 3) return;
    if (signed_area(p) < 0.0f) std::reverse(p.begin(), p.end());
    polys.push_back(std::move(p));
}

Poly circle_poly(P c, float r) {
    int n = static_cast<int>(std::ceil(kPi / std::acos(std::max(0.0f, 1.0f - 0.02f / std::max(r, 0.05f)))));
    n = std::clamp(n, 8, 96);
    Poly p;
    for (int i = 0; i < n; ++i) {
        float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(n);
        p.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
    }
    return p;
}

void stroke_polyline(std::vector<Poly>& polys, const Polyline& in, const StrokeStyle& st) {
    float hw = st.width / 2.0f;
    // drop repeated points
    std::vector<P> pts;
    for (const P& p : in.pts)
        if (pts.empty() || std::hypot(p.x - pts.back().x, p.y - pts.back().y) > 1e-5f) pts.push_back(p);
    bool closed = in.closed;
    if (closed && pts.size() > 1 && std::hypot(pts.front().x - pts.back().x, pts.front().y - pts.back().y) <= 1e-5f) pts.pop_back();
    if (pts.size() == 1) {  // a dot: only round / square caps draw anything
        P c = pts[0];
        if (st.cap == 1) push_poly(polys, circle_poly(c, hw));
        else if (st.cap == 2) push_poly(polys, {{c.x - hw, c.y - hw}, {c.x + hw, c.y - hw}, {c.x + hw, c.y + hw}, {c.x - hw, c.y + hw}});
        return;
    }
    if (pts.size() < 2) return;
    size_t n = pts.size();
    size_t segs = closed ? n : n - 1;
    std::vector<P> dir(segs);
    for (size_t i = 0; i < segs; ++i) {
        P a = pts[i], b = pts[(i + 1) % n];
        float len = std::hypot(b.x - a.x, b.y - a.y);
        dir[i] = {(b.x - a.x) / len, (b.y - a.y) / len};
    }
    for (size_t i = 0; i < segs; ++i) {
        P a = pts[i], b = pts[(i + 1) % n];
        P nrm{-dir[i].y * hw, dir[i].x * hw};
        P a0 = a, b0 = b;
        if (!closed && st.cap == 2) {  // square caps extend the end segments
            if (i == 0) a0 = {a.x - dir[i].x * hw, a.y - dir[i].y * hw};
            if (i == segs - 1) b0 = {b.x + dir[i].x * hw, b.y + dir[i].y * hw};
        }
        push_poly(polys, {{a0.x + nrm.x, a0.y + nrm.y}, {b0.x + nrm.x, b0.y + nrm.y}, {b0.x - nrm.x, b0.y - nrm.y}, {a0.x - nrm.x, a0.y - nrm.y}});
    }
    if (!closed && st.cap == 1) {
        push_poly(polys, circle_poly(pts.front(), hw));
        push_poly(polys, circle_poly(pts.back(), hw));
    }
    // joins at interior vertices (all vertices when closed)
    size_t first_join = closed ? 0 : 1, last_join = closed ? n : n - 1;
    for (size_t v = first_join; v < last_join; ++v) {
        const P& d0 = dir[(v + segs - 1) % segs];
        const P& d1 = dir[v % segs];
        float cross = d0.x * d1.y - d0.y * d1.x;
        float dot = d0.x * d1.x + d0.y * d1.y;
        if (std::fabs(cross) < 1e-6f && dot > 0.0f) continue;  // straight on
        P c = pts[v];
        if (st.join == 1) {
            push_poly(polys, circle_poly(c, hw));
            continue;
        }
        float sgn = cross > 0.0f ? -1.0f : 1.0f;  // the outer side of the turn
        P n0{-d0.y * sgn, d0.x * sgn}, n1{-d1.y * sgn, d1.x * sgn};
        P A{c.x + n0.x * hw, c.y + n0.y * hw}, B{c.x + n1.x * hw, c.y + n1.y * hw};
        bool miter = false;
        if (st.join == 0) {
            float cos_half_sq = (1.0f + (n0.x * n1.x + n0.y * n1.y)) / 2.0f;  // cos^2 of half the turn angle
            if (cos_half_sq > 1e-6f && 1.0f / std::sqrt(cos_half_sq) <= st.miter_limit) miter = true;
        }
        if (miter) {
            float k = hw / (1.0f + (n0.x * n1.x + n0.y * n1.y));
            P M{c.x + (n0.x + n1.x) * k, c.y + (n0.y + n1.y) * k};
            push_poly(polys, {c, A, M, B});
        } else {
            push_poly(polys, {c, A, B});
        }
    }
}

// ======================================================================================
// (4) canvas
// ======================================================================================

uint32_t div255(uint32_t v) { return (v + 255) >> 8; }  // tiny-skia lowp

struct Canvas {
    uint32_t w, h;
    std::vector<uint8_t> px;  // premultiplied RGBA8
    Canvas(uint32_t w_, uint32_t h_) : w(w_), h(h_), px(size_t(w_) * h_ * 4, 0) {}

    // While set, everything drawn is also multiplied by this path's coverage (marker clipping).
    const texmath::Path* clip = nullptr;

    // A per-pixel colour source: premultiplied RGBA8 of the pixel at integer position (x, y).
    using Shader = std::function<void(int x, int y, uint32_t& r, uint32_t& g, uint32_t& b, uint32_t& a)>;

    // Fill device-space polygons/paths with a solid colour. `rgb` is straight colour, `alpha` 0..1.
    void fill(const texmath::Path& path, Rgb rgb, float alpha) { fill(path, rgb, alpha, nullptr); }

    // Same, but the colour comes from `shader` (alpha is then ignored: the shader includes it).
    void fill(const texmath::Path& path, Rgb rgb, float alpha, const Shader* shader) {
        const texmath::RectF& b = path.bounds;
        int x0 = std::max(0, static_cast<int>(std::floor(b.left)) - 1);
        int y0 = std::max(0, static_cast<int>(std::floor(b.top)) - 1);
        int x1 = std::min(static_cast<int>(w), static_cast<int>(std::ceil(b.right)) + 1);
        int y1 = std::min(static_cast<int>(h), static_cast<int>(std::ceil(b.bottom)) + 1);
        if (x1 <= x0 || y1 <= y0) return;
        // coverage mask: paint the path white into a small transparent pixmap and read its alpha
        texmath::PathBuilder pb;
        size_t pi = 0;
        for (texmath::Verb v : path.verbs) {
            switch (v) {
                case texmath::Verb::Move: pb.move_to(path.points[pi].x - x0, path.points[pi].y - y0); ++pi; break;
                case texmath::Verb::Line: pb.line_to(path.points[pi].x - x0, path.points[pi].y - y0); ++pi; break;
                case texmath::Verb::Cubic:
                    pb.cubic_to(path.points[pi].x - x0, path.points[pi].y - y0, path.points[pi + 1].x - x0,
                                path.points[pi + 1].y - y0, path.points[pi + 2].x - x0, path.points[pi + 2].y - y0);
                    pi += 3;
                    break;
                case texmath::Verb::Close: pb.close(); break;
            }
        }
        auto shifted = pb.finish();
        if (!shifted) return;
        texmath::Pixmap cov(static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0));
        cov.fill_path(*shifted);
        if (clip) {
            texmath::PathBuilder cb;
            size_t ci = 0;
            for (texmath::Verb v : clip->verbs) {
                switch (v) {
                    case texmath::Verb::Move: cb.move_to(clip->points[ci].x - x0, clip->points[ci].y - y0); ++ci; break;
                    case texmath::Verb::Line: cb.line_to(clip->points[ci].x - x0, clip->points[ci].y - y0); ++ci; break;
                    case texmath::Verb::Cubic:
                        cb.cubic_to(clip->points[ci].x - x0, clip->points[ci].y - y0, clip->points[ci + 1].x - x0,
                                    clip->points[ci + 1].y - y0, clip->points[ci + 2].x - x0, clip->points[ci + 2].y - y0);
                        ci += 3;
                        break;
                    case texmath::Verb::Close: cb.close(); break;
                }
            }
            texmath::Pixmap clip_cov(cov.width(), cov.height());
            if (auto cp = cb.finish()) clip_cov.fill_path(*cp);
            std::vector<uint8_t>& cd2 = cov.data();
            const std::vector<uint8_t>& kd = clip_cov.data();
            for (size_t i = 0; i < cd2.size(); i += 4) cd2[i + 3] = static_cast<uint8_t>(div255(cd2[i + 3] * kd[i + 3]));
        }

        // premultiplied source colour as tiny-skia's uniform_color stage makes it
        auto q = [](float v) { return static_cast<uint32_t>(v * 255.0f + 0.5f); };
        alpha = std::clamp(alpha, 0.0f, 1.0f);
        uint32_t sr = q(static_cast<float>(rgb.r) / 255.0f * alpha), sg = q(static_cast<float>(rgb.g) / 255.0f * alpha),
                 sb = q(static_cast<float>(rgb.b) / 255.0f * alpha), sa = q(alpha);
        const std::vector<uint8_t>& cd = cov.data();
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                uint32_t c = cd[(size_t(y - y0) * cov.width() + (x - x0)) * 4 + 3];
                if (c == 0) continue;
                uint32_t r = sr, g = sg, bl = sb, a = sa;
                if (shader) (*shader)(x, y, r, g, bl, a);
                if (c != 255) { r = div255(r * c); g = div255(g * c); bl = div255(bl * c); a = div255(a * c); }
                uint8_t* d = &px[(size_t(y) * w + x) * 4];
                uint32_t inv = 255 - a;
                d[0] = static_cast<uint8_t>(r + div255(d[0] * inv));
                d[1] = static_cast<uint8_t>(g + div255(d[1] * inv));
                d[2] = static_cast<uint8_t>(bl + div255(d[2] * inv));
                d[3] = static_cast<uint8_t>(a + div255(d[3] * inv));
            }
    }

    void fill_polys(const std::vector<Poly>& polys, Rgb rgb, float alpha, const Shader* shader = nullptr) {
        texmath::PathBuilder pb;
        for (const Poly& p : polys) {
            pb.move_to(p[0].x, p[0].y);
            for (size_t i = 1; i < p.size(); ++i) pb.line_to(p[i].x, p[i].y);
            pb.close();
        }
        if (auto path = pb.finish()) fill(*path, rgb, alpha, shader);
    }

    // Draw `layer` (same size, premultiplied) over this canvas with an opacity and a blend mode. resvg does this with
    // draw_pixmap, whose pattern shader takes tiny-skia's float ("highp") pipeline: scale by the opacity, then
    // source-over or multiply, rounded to 8 bits on store.
    void composite(const Canvas& layer, float opacity, bool multiply) {
        const float factor = 1.0f / 255.0f;
        auto store = [](float v) { return static_cast<uint8_t>(std::nearbyint(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        for (size_t i = 0; i < px.size(); i += 4) {
            if (!(layer.px[i] | layer.px[i + 1] | layer.px[i + 2] | layer.px[i + 3])) continue;
            float s[4], d[4];
            for (int k = 0; k < 4; ++k) {
                s[k] = (static_cast<float>(layer.px[i + k]) * factor) * opacity;
                d[k] = static_cast<float>(px[i + k]) * factor;
            }
            float sa = s[3], da = d[3];
            for (int k = 0; k < 4; ++k) {
                float v;
                if (!multiply || k == 3) v = d[k] * (1.0f - sa) + s[k];
                else v = s[k] * (1.0f - da) + d[k] * (1.0f - sa) + s[k] * d[k];
                px[i + k] = store(v);
            }
        }
    }
};

// ======================================================================================
// (5) the tree
// ======================================================================================

struct Style {
    Paint fill{false, {0, 0, 0}};
    float fill_opacity = 1.0f;
    Paint stroke{true, {0, 0, 0}};
    float stroke_opacity = 1.0f;
    float stroke_width = 1.0f;
    StrokeStyle stroke_style;
    std::vector<float> dash;
    float dash_offset = 0.0f;
    float font_size = 12.0f;
    int font_weight = 400;
    bool italic = false;
    std::string anchor = "start";
    float opacity = 1.0f;  // element opacity (not inherited): drawn as a layer
    bool multiply = false;  // mix-blend-mode: multiply (not inherited): drawn as a layer
    std::string dominant_baseline = "auto";   // inherited
    std::string alignment_baseline = "auto";  // not inherited
    std::string marker_start, marker_end;  // ids of <marker> elements
    Mat ctm;
};

float parse_opacity(const std::string& v) {
    char* end = nullptr;
    std::string t = trim(v);
    float f = std::strtof(t.c_str(), &end);
    if (t.empty() || (*end != '\0' && *end != '%')) throw Unsupported("opacity " + v);
    if (*end == '%') f /= 100.0f;
    return std::clamp(f, 0.0f, 1.0f);
}

// Apply one presentation property (also used for the `style` attribute).
void apply_property(Style& st, const std::string& key, const std::string& val) {
    if (key == "fill") {
        try {
            st.fill = parse_paint(val);
        } catch (const BadPaint&) {
            st.fill = parse_paint("#000000");  // usvg: "Failed to parse fill value. Fallback to black."
        }
    }
    else if (key == "fill-opacity") st.fill_opacity = parse_opacity(val);
    else if (key == "stroke") {
        try {
            st.stroke = parse_paint(val);
        } catch (const BadPaint&) {
            st.stroke = parse_paint("none");  // usvg: "Failed to parse stroke value. Fallback to no stroke."
        }
    }
    else if (key == "stroke-opacity") st.stroke_opacity = parse_opacity(val);
    else if (key == "stroke-width") {
        std::string v = val;
        auto l = parse_length(&v);
        if (!l) throw Unsupported("stroke-width " + val);
        st.stroke_width = *l;
    } else if (key == "stroke-linecap") {
        st.stroke_style.cap = val == "round" ? 1 : (val == "square" ? 2 : 0);
    } else if (key == "stroke-linejoin") {
        st.stroke_style.join = val == "round" ? 1 : (val == "bevel" ? 2 : 0);
    } else if (key == "stroke-miterlimit") {
        auto n = parse_numbers(val);
        if (n && !n->empty()) st.stroke_style.miter_limit = (*n)[0];
    } else if (key == "stroke-dasharray") {
        st.dash.clear();
        if (trim(val) != "none") {
            auto n = parse_numbers(val);
            if (!n) throw Unsupported("dasharray " + val);
            st.dash = *n;
        }
    } else if (key == "stroke-dashoffset") {
        auto n = parse_numbers(val);
        if (n && !n->empty()) st.dash_offset = (*n)[0];
    } else if (key == "font-size") {
        std::string v = val;
        auto l = parse_length(&v);
        if (!l) throw Unsupported("font-size " + val);
        st.font_size = *l;
    } else if (key == "font-weight") {
        if (val == "bold" || val == "bolder") st.font_weight = 700;
        else if (val == "normal") st.font_weight = 400;
        else st.font_weight = static_cast<int>(std::strtol(val.c_str(), nullptr, 10));
    } else if (key == "font-style") {
        st.italic = val == "italic" || val == "oblique";
    } else if (key == "text-anchor") {
        st.anchor = val;
    } else if (key == "opacity") {
        st.opacity = parse_opacity(val);
    } else if (key == "mix-blend-mode") {
        if (val == "multiply") st.multiply = true;
        else if (val == "normal") st.multiply = false;
        else throw Unsupported("mix-blend-mode " + val);
    } else if (key == "dominant-baseline") {
        st.dominant_baseline = val;
    } else if (key == "alignment-baseline") {
        st.alignment_baseline = val;
    } else if (key == "fill-rule" || key == "clip-rule") {
        if (val != "nonzero") throw Unsupported(key + " " + val);
    } else if (key == "marker-start" || key == "marker-end") {
        std::string id;
        if (val != "none") {
            if (!starts_with(val, "url(#") || !ends_with(val, ")")) throw Unsupported(key + " " + val);
            id = val.substr(5, val.size() - 6);
        }
        (key == "marker-start" ? st.marker_start : st.marker_end) = id;
    } else if (key == "marker-mid") {
        if (val != "none") throw Unsupported("marker-mid");
    } else if (key == "filter" || key == "clip-path" || key == "mask" || key == "visibility" || key == "display") {
        if (val != "none" && val != "visible" && val != "inline") throw Unsupported(key);
    }
    // everything else (id, class, font-family, ...) does not change the picture
}

Style styled(const Style& parent, const XNode& n) {
    Style st = parent;
    st.opacity = 1.0f;  // not inherited
    st.multiply = false;
    st.alignment_baseline = "auto";
    static const char* props[] = {"fill", "fill-opacity", "stroke", "stroke-opacity", "stroke-width", "stroke-linecap",
                                  "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray", "stroke-dashoffset",
                                  "font-size", "font-weight", "font-style", "text-anchor", "opacity", "fill-rule",
                                  "clip-rule", "filter", "clip-path", "mask", "marker-start", "marker-mid", "marker-end",
                                  "visibility", "display", "mix-blend-mode", "dominant-baseline", "alignment-baseline"};
    for (const char* p : props)
        if (const std::string* v = n.attr(p)) apply_property(st, p, trim(*v));
    if (const std::string* css = n.attr("style")) {
        size_t i = 0;
        while (i < css->size()) {
            size_t semi = css->find(';', i);
            if (semi == std::string::npos) semi = css->size();
            std::string decl = css->substr(i, semi - i);
            size_t colon = decl.find(':');
            if (colon != std::string::npos) apply_property(st, trim(decl.substr(0, colon)), trim(decl.substr(colon + 1)));
            i = semi + 1;
        }
    }
    if (const std::string* t = n.attr("transform")) st.ctm = mul(st.ctm, parse_transform(*t));
    return st;
}

texmath::Path device_path(const VPath& vp, const Mat& m, bool close_subpaths) {
    texmath::PathBuilder pb;
    size_t pi = 0;
    bool open = false;
    auto pt = [&](P p) { return apply(m, p); };
    for (Cmd c : vp.cmds) {
        switch (c) {
            case Cmd::M: { if (open && close_subpaths) pb.close(); P p = pt(vp.pts[pi++]); pb.move_to(p.x, p.y); open = true; break; }
            case Cmd::L: { P p = pt(vp.pts[pi++]); pb.line_to(p.x, p.y); break; }
            case Cmd::C: {
                P a = pt(vp.pts[pi]), b = pt(vp.pts[pi + 1]), e = pt(vp.pts[pi + 2]);
                pi += 3;
                pb.cubic_to(a.x, a.y, b.x, b.y, e.x, e.y);
                break;
            }
            case Cmd::Z: pb.close(); open = false; break;
        }
    }
    if (open && close_subpaths) pb.close();
    auto p = pb.finish();
    if (!p) {
        texmath::Path empty;
        empty.bounds = {0, 0, 0, 0};
        return empty;
    }
    return *p;
}

class Renderer {
public:
    Renderer(Canvas& canvas, const FontSet& fonts) : cv_(&canvas), fonts_(fonts) {}

    // Remember every element that has an id (markers are looked up by it).
    void index_ids(const XNode& n) {
        if (const std::string* id = n.attr("id")) ids_[*id] = &n;
        for (const XNode& c : n.children) index_ids(c);
    }

    void draw(const XNode& n, const Style& parent) {
        if (n.tag.empty()) return;
        // paint servers are looked up by id when a shape uses them; nothing is drawn for them
        if (n.tag == "defs" || n.tag == "title" || n.tag == "desc" || n.tag == "metadata" || n.tag == "marker" || n.tag == "linearGradient")
            return;
        if (n.tag == "style" || n.tag == "use" || n.tag == "foreignObject" || n.tag == "pattern" ||
            n.tag == "radialGradient" || n.tag == "clipPath" || n.tag == "mask" || n.tag == "filter" || n.tag == "symbol")
            throw Unsupported("svg element <" + n.tag + ">");
        Style st = styled(parent, n);
        if (st.opacity == 1.0f && !st.multiply) {
            draw_styled(n, st);
            return;
        }
        // An element with opacity or a blend mode is drawn into its own transparent layer first (usvg makes
        // it a group), and the layer is then composited as a whole.
        float opacity = st.opacity;
        bool multiply = st.multiply;
        st.opacity = 1.0f;
        st.multiply = false;
        Canvas layer(cv_->w, cv_->h);
        layer.clip = cv_->clip;
        Canvas* parent_canvas = cv_;
        cv_ = &layer;
        draw_styled(n, st);
        cv_ = parent_canvas;
        cv_->composite(layer, opacity, multiply);
    }

private:
    void draw_styled(const XNode& n, const Style& st) {
        if (n.tag == "g" || n.tag == "a" || n.tag == "svg") {
            for (const XNode& c : n.children) draw(c, st);
            return;
        }
        if (n.tag == "image") {
            // The Rust path's resvg build has no raster-image support, so its C4 person icons never appear;
            // keep that picture (flip kDrawImages to draw them).
            constexpr bool kDrawImages = false;
            if (kDrawImages) draw_image(n, st);
            return;
        }
        auto len = [&](const char* k, float dflt = 0.0f) { return parse_length(n.attr(k)).value_or(dflt); };
        VPath vp;
        bool fillable = true;
        if (n.tag == "rect") {
            vp = rect_path(len("x"), len("y"), len("width"), len("height"), len("rx"), len("ry"));
            if (len("width") <= 0.0f || len("height") <= 0.0f) return;
        } else if (n.tag == "circle") {
            float r = len("r");
            if (r <= 0.0f) return;
            vp = ellipse_path(len("cx"), len("cy"), r, r);
        } else if (n.tag == "ellipse") {
            if (len("rx") <= 0.0f || len("ry") <= 0.0f) return;
            vp = ellipse_path(len("cx"), len("cy"), len("rx"), len("ry"));
        } else if (n.tag == "line") {
            vp.move_to({len("x1"), len("y1")});
            vp.line_to({len("x2"), len("y2")});
            fillable = false;
        } else if (n.tag == "polygon" || n.tag == "polyline") {
            auto nums = n.attr("points") ? parse_numbers(*n.attr("points")) : std::nullopt;
            if (!nums || nums->size() < 4) return;
            for (size_t i = 0; i + 1 < nums->size(); i += 2) {
                P p{(*nums)[i], (*nums)[i + 1]};
                if (i == 0) vp.move_to(p);
                else vp.line_to(p);
            }
            if (n.tag == "polygon") vp.close();
        } else if (n.tag == "path") {
            if (!n.attr("d")) return;
            vp = parse_path_data(*n.attr("d"));
        } else if (n.tag == "text") {
            draw_text(n, st);
            return;
        } else {
            throw Unsupported("svg element <" + n.tag + ">");
        }
        if (vp.empty()) return;
        paint_shape(vp, st, fillable);
        if (!st.marker_start.empty() || !st.marker_end.empty()) {
            if (n.tag != "path" && n.tag != "line" && n.tag != "polyline") throw Unsupported("markers on <" + n.tag + ">");
            draw_markers(vp, st);
        }
    }

    Canvas* cv_;
    const FontSet& fonts_;
    std::map<std::string, const XNode*> ids_;

    // ---- markers (resvg usvg/src/parser/marker.rs) ----

    // Tangent direction (user space) at the start / end of the path; nullopt for a degenerate path.
    static std::optional<P> path_end_direction(const VPath& vp, bool start, P& vertex) {
        std::vector<P> pts;  // control polygon in drawing order is enough for the tangent
        size_t pi = 0;
        std::vector<std::vector<P>> segs;  // per segment: its points including the start
        P cur;
        bool have = false;
        for (Cmd c : vp.cmds) {
            switch (c) {
                case Cmd::M: cur = vp.pts[pi++]; have = true; break;
                case Cmd::L: segs.push_back({cur, vp.pts[pi]}); cur = vp.pts[pi++]; break;
                case Cmd::C: segs.push_back({cur, vp.pts[pi], vp.pts[pi + 1], vp.pts[pi + 2]}); cur = vp.pts[pi + 2]; pi += 3; break;
                case Cmd::Z: break;
            }
        }
        if (!have || segs.empty()) return std::nullopt;
        if (start) {
            const auto& sg = segs.front();
            vertex = sg.front();
            for (size_t i = 1; i < sg.size(); ++i)
                if (sg[i].x != vertex.x || sg[i].y != vertex.y) return P{sg[i].x - vertex.x, sg[i].y - vertex.y};
        } else {
            const auto& sg = segs.back();
            vertex = sg.back();
            for (size_t i = sg.size() - 1; i-- > 0;)
                if (sg[i].x != vertex.x || sg[i].y != vertex.y) return P{vertex.x - sg[i].x, vertex.y - sg[i].y};
        }
        return std::nullopt;
    }

    void draw_marker(const std::string& id, bool is_start, const VPath& vp, const Style& st) {
        auto it = ids_.find(id);
        if (it == ids_.end() || it->second->tag != "marker") return;  // unknown reference: nothing drawn
        const XNode& marker = *it->second;
        P vertex;
        auto dir = path_end_direction(vp, is_start, vertex);
        if (!dir && marker.attr("orient") && *marker.attr("orient") != "0") return;

        auto num = [&](const char* k, float dflt) { return parse_length(marker.attr(k)).value_or(dflt); };
        float marker_w = num("markerWidth", 3.0f), marker_h = num("markerHeight", 3.0f);
        float ref_x = num("refX", 0.0f), ref_y = num("refY", 0.0f);
        const std::string* units = marker.attr("markerUnits");
        float stroke_scale = (units && *units == "userSpaceOnUse") ? 1.0f : st.stroke_width;
        if (marker_w <= 0.0f || marker_h <= 0.0f) return;

        float angle_deg = 0.0f;
        if (const std::string* orient = marker.attr("orient")) {
            if (*orient == "auto" || *orient == "auto-start-reverse") {
                angle_deg = dir ? std::atan2(dir->y, dir->x) * 180.0f / kPi : 0.0f;
                if (is_start && *orient == "auto-start-reverse") angle_deg += 180.0f;
            } else if (auto n = parse_numbers(*orient); n && !n->empty()) {
                angle_deg = (*n)[0];
            }
        }
        if (marker.attr("preserveAspectRatio") && *marker.attr("preserveAspectRatio") != "xMidYMid meet" &&
            *marker.attr("preserveAspectRatio") != "xMidYMid")
            throw Unsupported("marker preserveAspectRatio");

        // content -> user space: translate(vertex) rotate(angle) scale(view box scale) translate(-ref)
        Mat t;
        t.e = vertex.x;
        t.f = vertex.y;
        float rad = angle_deg * kPi / 180.0f;
        Mat rot{std::cos(rad), std::sin(rad), -std::sin(rad), std::cos(rad), 0, 0};
        std::array<float, 4> clip_rect{0, 0, marker_w / stroke_scale, marker_h / stroke_scale};  // x, y, w, h in content units
        float sx = stroke_scale, sy = stroke_scale;
        if (const std::string* vb = marker.attr("viewBox")) {
            auto nums = parse_numbers(*vb);
            if (!nums || nums->size() != 4 || (*nums)[2] <= 0 || (*nums)[3] <= 0) throw Unsupported("marker viewBox");
            float fit = std::min(marker_w * stroke_scale / (*nums)[2], marker_h * stroke_scale / (*nums)[3]);  // "meet"
            sx = sy = fit;
            clip_rect = {(*nums)[0], (*nums)[1], (*nums)[2], (*nums)[3]};
        }
        Mat scale{sx, 0, 0, sy, 0, 0};
        Mat tr;
        tr.e = -ref_x;
        tr.f = -ref_y;
        Mat content = mul(st.ctm, mul(t, mul(rot, mul(scale, tr))));

        // clip to the marker viewport (overflow: hidden)
        VPath clip_vp = rect_path(clip_rect[0], clip_rect[1], clip_rect[2], clip_rect[3], 0, 0);
        texmath::Path clip_path = device_path(clip_vp, content, true);
        const texmath::Path* saved_clip = cv_->clip;
        cv_->clip = &clip_path;
        Style base;
        base.ctm = content;
        Style marker_style = styled(base, marker);
        marker_style.ctm = content;  // the marker's own transform attribute does not exist; keep the placement
        for (const XNode& c : marker.children) draw(c, marker_style);
        cv_->clip = saved_clip;
    }

    void draw_markers(const VPath& vp, const Style& st) {
        if (!st.marker_start.empty()) draw_marker(st.marker_start, true, vp, st);
        if (!st.marker_end.empty()) draw_marker(st.marker_end, false, vp, st);
    }

    // ---- paint servers (linear gradients) ----

    // A tiny-skia style linear gradient shader for `url(#id)`; nullopt = nothing to draw (unknown id, no stops).
    // `opacity` is the fill-/stroke-opacity, folded into the stop alphas like resvg does.
    std::optional<Canvas::Shader> gradient_shader(const std::string& id, const Mat& ctm, float opacity) {
        auto it = ids_.find(id);
        if (it == ids_.end() || it->second->tag != "linearGradient") return std::nullopt;
        const XNode& g = *it->second;
        if (g.attr("xlink:href") || g.attr("href") || g.attr("gradientTransform") || g.attr("spreadMethod"))
            throw Unsupported("gradient attributes");
        const std::string* units = g.attr("gradientUnits");
        if (!units || *units != "userSpaceOnUse" || !g.attr("x2")) throw Unsupported("gradient units");
        auto coord = [&](const char* k) { return parse_length(g.attr(k)).value_or(0.0f); };
        P p1{coord("x1"), coord("y1")}, p2{coord("x2"), coord("y2")};

        struct Stop {
            float offset;
            float rgba[4];  // straight colour 0..1
        };
        std::vector<Stop> stops;
        for (const XNode& c : g.children) {
            if (c.tag != "stop") continue;
            float off = 0.0f;
            if (const std::string* o = c.attr("offset")) {
                std::string t = trim(*o);
                bool pct = ends_with(t, "%");
                auto nums = parse_numbers(pct ? t.substr(0, t.size() - 1) : t);
                if (!nums || nums->size() != 1) throw Unsupported("stop offset");
                off = pct ? (*nums)[0] / 100.0f : (*nums)[0];
            }
            off = std::clamp(off, 0.0f, 1.0f);
            if (!stops.empty() && off < stops.back().offset) off = stops.back().offset;  // offsets never go back
            Paint col;
            if (const std::string* sc = c.attr("stop-color")) {
                try {
                    col = parse_paint(trim(*sc));
                } catch (const BadPaint&) {
                    col = parse_paint("#000000");
                }
            }
            float so = 1.0f;
            if (const std::string* sop = c.attr("stop-opacity")) so = parse_opacity(*sop);
            stops.push_back({off,
                             {static_cast<float>(col.rgb.r) / 255.0f, static_cast<float>(col.rgb.g) / 255.0f,
                              static_cast<float>(col.rgb.b) / 255.0f, so * col.alpha * opacity}});
        }
        if (stops.empty()) return std::nullopt;

        auto premul8 = [](const float c[4], uint32_t& r, uint32_t& g2, uint32_t& b, uint32_t& a) {
            auto q = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
            r = q(c[0] * c[3]);
            g2 = q(c[1] * c[3]);
            b = q(c[2] * c[3]);
            a = q(c[3]);
        };
        float dx = p2.x - p1.x, dy = p2.y - p1.y;
        float len2 = dx * dx + dy * dy;
        if (stops.size() == 1 || len2 <= 1e-12f) {  // a single colour (a degenerate gradient shows its last stop)
            Stop only = stops.back();
            return Canvas::Shader([only, premul8](int, int, uint32_t& r, uint32_t& g2, uint32_t& b, uint32_t& a) { premul8(only.rgba, r, g2, b, a); });
        }
        // device -> user space
        float det = ctm.a * ctm.d - ctm.b * ctm.c;
        if (std::fabs(det) < 1e-12f) return std::nullopt;
        Mat inv{ctm.d / det, -ctm.b / det, -ctm.c / det, ctm.a / det, 0, 0};
        inv.e = -(inv.a * ctm.e + inv.c * ctm.f);
        inv.f = -(inv.b * ctm.e + inv.d * ctm.f);
        return Canvas::Shader([=](int x, int y, uint32_t& r, uint32_t& g2, uint32_t& b, uint32_t& a) {
            P u = apply(inv, {static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f});
            float t = std::clamp(((u.x - p1.x) * dx + (u.y - p1.y) * dy) / len2, 0.0f, 1.0f);
            // find the pair of stops around t (before the first / after the last stop the colour is flat)
            float c[4];
            if (t <= stops.front().offset) {
                for (int k = 0; k < 4; ++k) c[k] = stops.front().rgba[k];
            } else if (t >= stops.back().offset) {
                for (int k = 0; k < 4; ++k) c[k] = stops.back().rgba[k];
            } else {
                size_t i = 1;
                while (i < stops.size() && stops[i].offset < t) ++i;
                const Stop& s0 = stops[i - 1];
                const Stop& s1 = stops[i];
                float span = s1.offset - s0.offset;
                float f = span > 0.0f ? (t - s0.offset) / span : 1.0f;
                for (int k = 0; k < 4; ++k) c[k] = s0.rgba[k] + (s1.rgba[k] - s0.rgba[k]) * f;
            }
            premul8(c, r, g2, b, a);
        });
    }

    void paint_fill(const texmath::Path& path, const Style& st) {
        if (!st.fill.url.empty()) {
            if (auto sh = gradient_shader(st.fill.url, st.ctm, st.fill_opacity)) cv_->fill(path, {}, 1.0f, &*sh);
            return;
        }
        cv_->fill(path, st.fill.rgb, st.fill_opacity * st.fill.alpha);
    }

    void paint_stroke(const std::vector<Poly>& polys, const Style& st) {
        if (!st.stroke.url.empty()) {
            if (auto sh = gradient_shader(st.stroke.url, st.ctm, st.stroke_opacity)) cv_->fill_polys(polys, {}, 1.0f, &*sh);
            return;
        }
        cv_->fill_polys(polys, st.stroke.rgb, st.stroke_opacity * st.stroke.alpha);
    }

    // ---- <image> (PNG data URIs, as the C4 person icons) ----

    static std::vector<uint8_t> base64_decode(std::string_view in) {
        std::vector<uint8_t> out;
        uint32_t acc = 0;
        int bits = 0;
        for (char ch : in) {
            int v;
            if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
            else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
            else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
            else if (ch == '+') v = 62;
            else if (ch == '/') v = 63;
            else continue;  // padding, whitespace
            acc = (acc << 6) | static_cast<uint32_t>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
            }
        }
        return out;
    }

    void draw_image(const XNode& n, const Style& st) {
        const std::string* href = n.attr("xlink:href");
        if (!href) href = n.attr("href");
        static const std::string kPrefix = "data:image/png;base64,";
        if (!href || !starts_with(*href, kPrefix)) throw Unsupported("image source");
        if (const std::string* par = n.attr("preserveAspectRatio"); par && *par != "xMidYMid meet" && *par != "xMidYMid")
            throw Unsupported("image preserveAspectRatio");
        std::vector<uint8_t> bytes = base64_decode(std::string_view(*href).substr(kPrefix.size()));
        int iw = 0, ih = 0, comp = 0;
        uint8_t* data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &iw, &ih, &comp, 4);
        if (!data) throw Unsupported("image decode");
        // premultiplied float pixels (tiny-skia premultiply_u8 rounding)
        std::vector<std::array<float, 4>> img(static_cast<size_t>(iw) * ih);
        for (size_t i = 0; i < img.size(); ++i) {
            uint32_t a = data[i * 4 + 3];
            for (int k = 0; k < 3; ++k) {
                uint32_t prod = data[i * 4 + k] * a + 128;
                img[i][k] = static_cast<float>((prod + (prod >> 8)) >> 8) / 255.0f;
            }
            img[i][3] = static_cast<float>(a) / 255.0f;
        }
        stbi_image_free(data);

        auto num = [&](const char* k) { return parse_length(n.attr(k)).value_or(0.0f); };
        float x = num("x"), y = num("y"), w = num("width"), h = num("height");
        if (w <= 0.0f || h <= 0.0f) return;
        float scale = std::min(w / static_cast<float>(iw), h / static_cast<float>(ih));  // "meet"
        float tx = x + (w - static_cast<float>(iw) * scale) / 2.0f, ty = y + (h - static_cast<float>(ih) * scale) / 2.0f;
        Mat m = mul(st.ctm, mul(Mat{1, 0, 0, 1, tx, ty}, Mat{scale, 0, 0, scale, 0, 0}));  // image pixels -> device
        float det = m.a * m.d - m.b * m.c;
        if (std::fabs(det) < 1e-12f) return;
        Mat inv{m.d / det, -m.b / det, -m.c / det, m.a / det, 0, 0};
        inv.e = -(inv.a * m.e + inv.c * m.f);
        inv.f = -(inv.b * m.e + inv.d * m.f);
        bool translate_only = m.a == 1.0f && m.d == 1.0f && m.b == 0.0f && m.c == 0.0f;

        auto texel = [&](int px, int py) -> const std::array<float, 4>& {
            px = std::clamp(px, 0, iw - 1);
            py = std::clamp(py, 0, ih - 1);
            return img[static_cast<size_t>(py) * iw + px];
        };
        // tiny-skia bicubic weights (Mitchell, B = C = 1/3)
        auto near_w = [](float t) { return ((-21.0f / 18.0f * t + 27.0f / 18.0f) * t + 9.0f / 18.0f) * t + 1.0f / 18.0f; };
        auto far_w = [](float t) { return (t * t) * (7.0f / 18.0f * t - 6.0f / 18.0f); };
        Canvas::Shader shader = [&](int px, int py, uint32_t& r, uint32_t& g, uint32_t& b, uint32_t& a) {
            P u = apply(inv, {static_cast<float>(px) + 0.5f, static_cast<float>(py) + 0.5f});
            float c[4] = {0, 0, 0, 0};
            if (translate_only) {
                const auto& t = texel(static_cast<int>(std::floor(u.x)), static_cast<int>(std::floor(u.y)));
                for (int k = 0; k < 4; ++k) c[k] = t[k];
            } else {
                float fx = (u.x + 0.5f) - std::floor(u.x + 0.5f), fy = (u.y + 0.5f) - std::floor(u.y + 0.5f);
                float wx[4] = {far_w(1.0f - fx), near_w(1.0f - fx), near_w(fx), far_w(fx)};
                float wy[4] = {far_w(1.0f - fy), near_w(1.0f - fy), near_w(fy), far_w(fy)};
                for (int j = 0; j < 4; ++j)
                    for (int i = 0; i < 4; ++i) {
                        const auto& t = texel(static_cast<int>(std::floor(u.x - 1.5f + static_cast<float>(i))),
                                              static_cast<int>(std::floor(u.y - 1.5f + static_cast<float>(j))));
                        for (int k = 0; k < 4; ++k) c[k] += t[k] * wx[i] * wy[j];
                    }
                c[3] = std::clamp(c[3], 0.0f, 1.0f);
                for (int k = 0; k < 3; ++k) c[k] = std::clamp(c[k], 0.0f, c[3]);
            }
            auto q = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
            r = q(c[0]);
            g = q(c[1]);
            b = q(c[2]);
            a = q(c[3]);
        };
        VPath rect = rect_path(0, 0, static_cast<float>(iw), static_cast<float>(ih), 0, 0);
        texmath::Path device = device_path(rect, m, true);
        if (!device.verbs.empty()) cv_->fill(device, {}, 1.0f, &shader);
    }

    void paint_shape(const VPath& vp, const Style& st, bool fillable) {
        if (fillable && !st.fill.none) {
            texmath::Path path = device_path(vp, st.ctm, true);
            if (!path.verbs.empty()) paint_fill(path, st);
        }
        if (!st.stroke.none && st.stroke_width > 0.0f) stroke(vp, st);
    }

    void stroke(const VPath& vp, const Style& st) {
        float det = std::fabs(st.ctm.a * st.ctm.d - st.ctm.b * st.ctm.c);
        float sx = std::sqrt(det);
        StrokeStyle ss = st.stroke_style;
        ss.width = st.stroke_width * sx;
        std::vector<Polyline> lines = flatten(vp, st.ctm);
        // dashes: only when the pattern has a visible gap (a "1,0" pattern is a solid line)
        std::vector<float> pattern;
        for (float v : st.dash) pattern.push_back(v * sx);
        float total = 0.0f, gaps = 0.0f;
        bool bad = false;
        for (size_t i = 0; i < pattern.size(); ++i) {
            if (pattern[i] < 0.0f) bad = true;
            total += pattern[i];
            if (i % 2 == 1) gaps += pattern[i];
        }
        if (pattern.size() % 2 == 1) {  // an odd list repeats once to get on/off pairs
            std::vector<float> twice = pattern;
            twice.insert(twice.end(), pattern.begin(), pattern.end());
            pattern = twice;
            total *= 2.0f;
            gaps = total / 2.0f;
        }
        if (!bad && total > 0.0f && gaps > 0.0f) lines = dash(lines, pattern, st.dash_offset * sx);
        std::vector<Poly> polys;
        for (const Polyline& pl : lines) stroke_polyline(polys, pl, ss);
        paint_stroke(polys, st);
    }

    // ---- text ----

    struct Run {
        std::string text;
        Style style;
    };
    struct Chunk {
        float x = 0, y = 0;
        std::vector<Run> runs;
    };

    struct TextState {
        float x = 0, y = 0;  // current text position
        std::vector<Chunk> chunks;
        bool at_chunk_start = true;
    };

    static std::string collapse_space(const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            if (c == ' ' && !out.empty() && out.back() == ' ') continue;
            out += c;
        }
        return out;
    }

    // The first number of a coordinate list; a unit of "em" multiplies by the font size.
    static std::optional<float> first_length(const std::string* v, float font_size) {
        if (!v) return std::nullopt;
        std::string t = trim(*v);
        char* end = nullptr;
        float num = std::strtof(t.c_str(), &end);
        if (end == t.c_str()) return std::nullopt;
        std::string rest = trim(std::string(end));
        if (starts_with(rest, "em")) return num * font_size;
        return num;
    }

    void collect(const XNode& n, const Style& parent, TextState& ts, bool root) {
        Style st = root ? parent : styled(parent, n);
        if (!root) {
            auto first = [&](const char* k) -> std::optional<float> { return first_length(n.attr(k), st.font_size); };
            auto x = first("x");
            auto y = first("y");
            if (x || y) {
                if (x) ts.x = *x;
                if (y) ts.y = *y;
                ts.at_chunk_start = true;
            }
            if (auto dy = first("dy")) ts.y += *dy;
            if (auto dx = first("dx")) { (void)dx; throw Unsupported("tspan dx"); }
        }
        for (const XNode& c : n.children) {
            if (c.tag.empty()) {
                std::string text = collapse_space(c.text);
                if (text.empty()) continue;
                if (ts.at_chunk_start || ts.chunks.empty()) {
                    Chunk ch;
                    ch.x = ts.x;
                    ch.y = ts.y;
                    ts.chunks.push_back(std::move(ch));
                    ts.at_chunk_start = false;
                }
                ts.chunks.back().runs.push_back({text, st});
            } else if (c.tag == "tspan" || c.tag == "a") {
                collect(c, st, ts, false);
            } else if (c.tag == "title" || c.tag == "desc") {
                continue;
            } else {
                throw Unsupported("element <" + c.tag + "> inside text");
            }
        }
    }

    const TextFace* face_for(const Style& st) const {
        if (st.italic && fonts_.italic) return fonts_.italic;
        if (st.font_weight >= 600 && fonts_.bold) return fonts_.bold;
        return fonts_.regular;
    }

    // Collects glyph outlines (font units, y up) into a device-space path.
    class GlyphPath : public OutlineSink {
    public:
        GlyphPath(const Mat& ctm, float ox, float oy, float scale) : ctm_(ctm), ox_(ox), oy_(oy), s_(scale) {}
        texmath::PathBuilder* pb = nullptr;
        void move_to(float x, float y) override { P p = tf(x, y); pb->move_to(p.x, p.y); }
        void line_to(float x, float y) override { P p = tf(x, y); pb->line_to(p.x, p.y); }
        void quad_to(float cx, float cy, float x, float y) override {
            // a quadratic as the equivalent cubic (tf() also remembers the last point, so read it first)
            P s = last_;
            P q = tf(cx, cy), e = tf(x, y);
            pb->cubic_to(s.x + 2.0f / 3.0f * (q.x - s.x), s.y + 2.0f / 3.0f * (q.y - s.y),
                         e.x + 2.0f / 3.0f * (q.x - e.x), e.y + 2.0f / 3.0f * (q.y - e.y), e.x, e.y);
            last_ = e;
        }
        void cubic_to(float c1x, float c1y, float c2x, float c2y, float x, float y) override {
            P a = tf(c1x, c1y), b = tf(c2x, c2y), e = tf(x, y);
            pb->cubic_to(a.x, a.y, b.x, b.y, e.x, e.y);
            last_ = e;
        }
        void close() override { pb->close(); }

    private:
        Mat ctm_;
        float ox_, oy_, s_;
        P last_;
        P tf(float fx, float fy) {
            last_ = apply(ctm_, {ox_ + fx * s_, oy_ - fy * s_});
            return last_;
        }
    };

    // Vertical shift of a run for dominant-baseline / alignment-baseline (usvg resolve_baseline: Chrome-like
    // hard-coded baselines from the font's ascent, descent and x-height). Positive moves the text down.
    static float baseline_shift(const TextFace& face, const Style& st) {
        std::string kind = st.alignment_baseline;
        if (kind == "auto" || kind == "baseline") {
            kind = st.dominant_baseline;
            if (kind == "use-script" || kind == "no-change" || kind == "reset-size") kind = "auto";
        }
        float scale = st.font_size / face.units_per_em();
        float ascent = face.ascent() * scale, descent = face.descent() * scale;
        float x_height = face.x_height() * scale;
        if (kind == "before-edge" || kind == "text-before-edge") return ascent;
        if (kind == "middle") return x_height * 0.5f;
        if (kind == "central") return ascent - (ascent - descent) * 0.5f;
        if (kind == "after-edge" || kind == "text-after-edge" || kind == "ideographic") return descent;
        if (kind == "hanging") return ascent * 0.8f;
        if (kind == "mathematical") return ascent * 0.5f;
        return 0.0f;  // auto, baseline, alphabetic
    }

    void draw_text(const XNode& n, const Style& st) {
        if (!st.stroke.none) throw Unsupported("stroked text");
        TextState ts;
        if (auto* v = n.attr("x")) { auto nums = parse_numbers(*v); if (nums && !nums->empty()) ts.x = (*nums)[0]; }
        if (auto* v = n.attr("y")) { auto nums = parse_numbers(*v); if (nums && !nums->empty()) ts.y = (*nums)[0]; }
        if (auto dy = first_length(n.attr("dy"), st.font_size)) ts.y += *dy;
        collect(n, st, ts, true);
        // textLength (lengthAdjust="spacing"): the glyph advances are widened so the text is that long
        std::optional<float> text_length = first_length(n.attr("textLength"), st.font_size);
        if (const std::string* la = n.attr("lengthAdjust"); la && *la != "spacing") throw Unsupported("lengthAdjust " + *la);
        for (Chunk& chunk : ts.chunks) {
            // trim the chunk's outer spaces (xml:space="default")
            if (!chunk.runs.empty()) {
                std::string& t0 = chunk.runs.front().text;
                if (!t0.empty() && t0.front() == ' ') t0.erase(t0.begin());
                std::string& t1 = chunk.runs.back().text;
                if (!t1.empty() && t1.back() == ' ') t1.pop_back();
            }
            // shape every run, then place the chunk according to text-anchor
            struct Shaped {
                const TextFace* face;
                std::vector<ShapedGlyph> glyphs;
                float scale;
                const Style* style;
                float shift = 0.0f;
            };
            std::vector<Shaped> shaped;
            float width = 0.0f;
            for (const Run& r : chunk.runs) {
                const TextFace* face = face_for(r.style);
                if (!face || !face->loaded()) throw Unsupported("no font for text");
                Shaped s{face, face->shape(r.text), r.style.font_size / face->units_per_em(), &r.style, 0.0f};
                s.shift = baseline_shift(*face, r.style);
                for (const auto& g : s.glyphs) width += g.x_advance * s.scale;
                shaped.push_back(std::move(s));
            }
            float extra = 0.0f;  // textLength: added to every glyph advance
            if (text_length) {
                size_t count = 0;
                for (const Shaped& s : shaped) count += s.glyphs.size();
                if (count > 1) extra = (*text_length - width) / static_cast<float>(count - 1);
                width += extra * static_cast<float>(count);
            }
            float pen = chunk.x;
            if (st.anchor == "middle") pen -= width / 2.0f;
            else if (st.anchor == "end") pen -= width;
            for (const Shaped& s : shaped) {
                texmath::PathBuilder pb;
                for (const ShapedGlyph& g : s.glyphs) {
                    GlyphPath gp(s.style->ctm, pen + g.x_offset * s.scale, chunk.y + s.shift - g.y_offset * s.scale, s.scale);
                    gp.pb = &pb;
                    s.face->outline(g.gid, gp);
                    pen += g.x_advance * s.scale + extra;
                }
                if (!s.style->fill.none)
                    if (auto path = pb.finish()) paint_fill(*path, *s.style);
            }
        }
    }
};

// ink(): colour picture -> white-ink mask (copy of the function in the reference implementation).
void ink(const std::vector<uint8_t>& rgba, std::vector<uint8_t>& out) {
    out.resize(rgba.size());
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        float a = static_cast<float>(rgba[i + 3]) / 255.0f;
        float lum = 0.299f * static_cast<float>(rgba[i]) + 0.587f * static_cast<float>(rgba[i + 1]) +
                    0.114f * static_cast<float>(rgba[i + 2]);
        float v = std::clamp(a * 255.0f - lum, 0.0f, 255.0f);
        auto ink8 = static_cast<uint8_t>(std::round(v));  // Rust f32::round: half away from zero (v >= 0)
        out[i] = out[i + 1] = out[i + 2] = out[i + 3] = ink8;
    }
}

}  // namespace

InkImage rasterize_svg(const std::string& svg, float scale, const FontSet& fonts) {
    XmlReader reader(svg);
    XNode doc = reader.parse_document();
    const XNode* root = nullptr;
    for (const XNode& c : doc.children)
        if (c.tag == "svg") root = &c;
    if (!root) throw ParseError("no <svg> element");

    // size and viewBox
    // usvg resolve_svg_size: a missing or percentage width / height is that share of the viewBox.
    std::optional<float> w, h;
    Mat view;
    if (const std::string* vb = root->attr("viewBox")) {
        auto nums = parse_numbers(*vb);
        if (!nums || nums->size() != 4 || (*nums)[2] <= 0 || (*nums)[3] <= 0) throw Unsupported("viewBox");
        auto dim = [&](const char* name, float vb_len) -> std::optional<float> {
            const std::string* a = root->attr(name);
            if (!a) return vb_len;
            std::string t = trim(*a);
            if (ends_with(t, "%")) {
                auto pct = parse_numbers(t.substr(0, t.size() - 1));
                if (!pct || pct->size() != 1) return std::nullopt;
                return vb_len * ((*pct)[0] / 100.0f);
            }
            return parse_length(a);
        };
        w = dim("width", (*nums)[2]);
        h = dim("height", (*nums)[3]);
        if (!w || !h) throw Unsupported("svg size");
        float sx = *w / (*nums)[2], sy = *h / (*nums)[3];
        if (std::fabs(sx - sy) > 1e-4f) throw Unsupported("non-uniform viewBox");
        view = {sx, 0, 0, sy, -(*nums)[0] * sx, -(*nums)[1] * sy};
    } else {
        w = parse_length(root->attr("width"));
        h = parse_length(root->attr("height"));
        if (!w || !h) throw Unsupported("svg without width/height");
    }
    // `width * scale` rounds up like `(size.width() * scale).ceil()`.
    float fw = std::ceil(*w * scale), fh = std::ceil(*h * scale);
    if (!(fw >= 1.0f && fh >= 1.0f)) throw Unsupported("empty diagram");
    if (fw > 8192.0f || fh > 8192.0f) throw Unsupported("diagram too large");
    Canvas canvas(static_cast<uint32_t>(fw), static_cast<uint32_t>(fh));

    Style base;
    base.ctm = mul(Mat{scale, 0, 0, scale, 0, 0}, view);
    Renderer renderer(canvas, fonts);
    renderer.index_ids(*root);
    for (const XNode& c : root->children) renderer.draw(c, base);

    InkImage out;
    out.width = canvas.w;
    out.height = canvas.h;
    ink(canvas.px, out.rgba);
    return out;
}

}  // namespace mermaid
