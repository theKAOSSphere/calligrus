#include "imagefilter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace imagefilter {
namespace {

// The whole of `s` as a finite number (nothing before or after it).
std::optional<float> number(std::string_view s) {
    if (s.empty() || std::isspace(static_cast<unsigned char>(s.front()))) return std::nullopt;
    const std::string t(s);
    char* end = nullptr;
    const float v = std::strtof(t.c_str(), &end);
    if (end != t.c_str() + t.size() || !std::isfinite(v)) return std::nullopt;
    return v;
}

// The 3x3 colour matrix of a matrix filter (nullopt for the others), from the Filter Effects spec.
using Matrix = std::array<std::array<float, 3>, 3>;
std::optional<Matrix> matrix(Filter f) {
    const float a = f.amount;
    switch (f.kind) {
    case Kind::Grayscale: {
        const float s = 1.0f - a;
        return Matrix{{{0.2126f + 0.7874f * s, 0.7152f - 0.7152f * s, 0.0722f - 0.0722f * s},
                       {0.2126f - 0.2126f * s, 0.7152f + 0.2848f * s, 0.0722f - 0.0722f * s},
                       {0.2126f - 0.2126f * s, 0.7152f - 0.7152f * s, 0.0722f + 0.9278f * s}}};
    }
    case Kind::Sepia: {
        const float s = 1.0f - a;
        return Matrix{{{0.393f + 0.607f * s, 0.769f - 0.769f * s, 0.189f - 0.189f * s},
                       {0.349f - 0.349f * s, 0.686f + 0.314f * s, 0.168f - 0.168f * s},
                       {0.272f - 0.272f * s, 0.534f - 0.534f * s, 0.131f + 0.869f * s}}};
    }
    case Kind::Saturate:
        return Matrix{{{0.213f + 0.787f * a, 0.715f - 0.715f * a, 0.072f - 0.072f * a},
                       {0.213f - 0.213f * a, 0.715f + 0.285f * a, 0.072f - 0.072f * a},
                       {0.213f - 0.213f * a, 0.715f - 0.715f * a, 0.072f + 0.928f * a}}};
    case Kind::HueRotate: {
        const float r = a * (3.14159265358979323846f / 180.0f);
        const float s = std::sin(r), c = std::cos(r);
        return Matrix{{{0.213f + c * 0.787f - s * 0.213f, 0.715f - c * 0.715f - s * 0.715f, 0.072f - c * 0.072f + s * 0.928f},
                       {0.213f - c * 0.213f + s * 0.143f, 0.715f + c * 0.285f + s * 0.140f, 0.072f - c * 0.072f - s * 0.283f},
                       {0.213f - c * 0.213f - s * 0.787f, 0.715f - c * 0.715f + s * 0.715f, 0.072f + c * 0.928f + s * 0.072f}}};
    }
    case Kind::Brightness:
        return Matrix{{{a, 0, 0}, {0, a, 0}, {0, 0, a}}};
    default:
        return std::nullopt;
    }
}

unsigned char to_byte(float v) { return static_cast<unsigned char>(std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

// Each colour channel (0..1) through `f`, clamped.
template <class F> void per_channel(std::vector<unsigned char>& rgba, F f) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
        for (size_t k = 0; k < 3; k++) rgba[i + k] = to_byte(f(rgba[i + k] / 255.0f));
}

// One box blur of radius `r` from `src` to `dst`, along the rows (`across`) or the columns.
void box_pass(const std::vector<unsigned char>& src, std::vector<unsigned char>& dst, int w, int h, int r, bool across) {
    const int lines = across ? h : w, len = across ? w : h;
    auto at = [&](int line, int i) { return across ? (size_t(line) * w + i) * 4 : (size_t(i) * w + line) * 4; };
    const unsigned n = unsigned(2 * r + 1);
    for (int line = 0; line < lines; line++) {
        for (int ch = 0; ch < 4; ch++) {
            auto px = [&](int i) { return unsigned(src[at(line, std::clamp(i, 0, len - 1)) + ch]); };
            unsigned sum = 0;
            for (int i = -r; i <= r; i++) sum += px(i);
            for (int i = 0; i < len; i++) {
                dst[at(line, i) + ch] = static_cast<unsigned char>((sum + n / 2) / n);
                sum += px(i + r + 1);
                sum -= px(i - r);
            }
        }
    }
}

// A Gaussian-like blur of radius `sigma` pixels: three box blurs across, then three down (all
// channels, edges repeated; integer arithmetic, so both apps get the same pixels).
void blur(std::vector<unsigned char>& rgba, int w, int h, float sigma) {
    if (sigma < 0.5f || w == 0 || h == 0) return;
    std::vector<unsigned char> tmp(rgba.size());
    for (int b : blur_boxes(sigma)) {
        const int r = (b - 1) / 2;
        if (r == 0) continue;
        box_pass(rgba, tmp, w, h, r, true);
        box_pass(tmp, rgba, w, h, r, false);
    }
}

}  // namespace

std::optional<Filter> parse_filter(std::string_view word) {
    const size_t colon = word.find(':');
    const std::string_view name = word.substr(0, colon);
    const std::optional<std::string_view> value = colon == std::string_view::npos ? std::nullopt : std::optional(word.substr(colon + 1));
    // A number, or a percentage (50% = 0.5); never negative.
    auto amount = [](std::string_view v) -> std::optional<float> {
        std::optional<float> n;
        if (!v.empty() && v.back() == '%') {
            n = number(v.substr(0, v.size() - 1));
            if (n) *n /= 100.0f;
        } else {
            n = number(v);
        }
        if (n && *n < 0) n.reset();
        return n;
    };
    auto value_or = [&](float def, std::string_view unit) -> std::optional<float> {
        if (!value) return def;
        if (unit.empty()) return amount(*value);
        std::string_view v = *value;
        if (v.size() >= unit.size() && v.substr(v.size() - unit.size()) == unit) v.remove_suffix(unit.size());
        auto n = number(v);
        if (n && *n < 0 && unit != "deg") n.reset();
        return n;
    };
    auto make = [](Kind k, std::optional<float> v, bool at_most_1 = false) -> std::optional<Filter> {
        if (!v) return std::nullopt;
        return Filter{k, at_most_1 ? std::min(*v, 1.0f) : *v};
    };
    if (name == "blur") return make(Kind::Blur, value_or(10.0f, "px"));
    if (name == "brightness") return make(Kind::Brightness, value_or(1.5f, ""));
    if (name == "contrast") return make(Kind::Contrast, value_or(2.0f, ""));
    if (name == "grayscale") return make(Kind::Grayscale, value_or(1.0f, ""), true);
    if (name == "hue-rotate") return make(Kind::HueRotate, value_or(180.0f, "deg"));
    if (name == "invert") return make(Kind::Invert, value_or(1.0f, ""), true);
    if (name == "opacity") return make(Kind::Opacity, value_or(0.5f, ""), true);
    if (name == "saturate") return make(Kind::Saturate, value_or(2.0f, ""));
    if (name == "sepia") return make(Kind::Sepia, value_or(1.0f, ""), true);
    return std::nullopt;
}

std::array<int, 3> blur_boxes(float sigma) {
    const double s = sigma;
    const double ideal = std::sqrt(12.0 * s * s / 3.0 + 1.0);
    long long lo = (long long)std::floor(ideal);
    if (lo % 2 == 0) lo -= 1;
    lo = std::max(lo, 1LL);
    const long long hi = lo + 2;
    const long long m = std::llround((12.0 * s * s - 3.0 * double(lo * lo) - 12.0 * double(lo) - 9.0) / (-4.0 * double(lo) - 4.0));
    std::array<int, 3> out{};
    for (int i = 0; i < 3; i++) out[i] = int(i < m ? lo : hi);
    return out;
}

void apply(std::vector<unsigned char>& rgba, int w, int h, const std::vector<Filter>& filters) {
    for (const Filter& f : filters) {
        const float a = f.amount;
        switch (f.kind) {
        case Kind::Blur:
            blur(rgba, w, h, std::min(a * float(w) / kBlurSlideWidth, kMaxBlur));
            break;
        case Kind::Opacity:
            for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = static_cast<unsigned char>(std::round(rgba[i] * a));
            break;
        case Kind::Contrast:
            per_channel(rgba, [a](float c) { return (c - 0.5f) * a + 0.5f; });
            break;
        case Kind::Invert:
            per_channel(rgba, [a](float c) { return c + a * (1.0f - 2.0f * c); });
            break;
        default: {
            const auto m = matrix(f);
            if (!m) break;
            for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
                const float c[3] = {rgba[i] / 255.0f, rgba[i + 1] / 255.0f, rgba[i + 2] / 255.0f};
                for (size_t k = 0; k < 3; k++) rgba[i + k] = to_byte((*m)[k][0] * c[0] + (*m)[k][1] * c[1] + (*m)[k][2] * c[2]);
            }
        }
        }
    }
}

std::string key(const std::vector<Filter>& filters) {
    std::string out;
    char buf[48];
    for (const Filter& f : filters) {
        std::snprintf(buf, sizeof buf, "|%d:%g", int(f.kind), f.amount);
        out += buf;
    }
    return out;
}

}  // namespace imagefilter
