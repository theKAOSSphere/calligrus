// Marp's image filters (spec 26, imagefilter.rs): `![bg sepia](a.png)`, `![bg blur:4px grayscale](b.jpg)`.
// Each filter is a word in the picture's alt text, optionally with a value after a colon; they are
// applied to the decoded pixels in the order written, like CSS's `filter` (whose formulas these are,
// applied to the sRGB values).
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace imagefilter {

enum class Kind { Blur, Brightness, Contrast, Grayscale, HueRotate, Invert, Opacity, Saturate, Sepia };

// One filter and its amount (Blur: the radius in pixels of a 1280 px wide slide; HueRotate: degrees).
struct Filter {
    Kind kind;
    float amount;
    bool operator==(const Filter&) const = default;
};

// Blur radii are in pixels of a slide this wide (Marp's default size), whatever the picture's.
constexpr float kBlurSlideWidth = 1280.0f;
// The largest blur radius, in picture pixels (a bigger one would only cost time).
constexpr float kMaxBlur = 200.0f;

// `sepia`, `sepia:50%`, `brightness:1.5`, `blur:4px`, `hue-rotate:90deg`... nullopt if the word is
// not a filter (or its value doesn't parse). Without a value each gets Marp's default.
std::optional<Filter> parse_filter(std::string_view word);

// Apply `filters` in order to `rgba` (`w` x `h` pixels, 4 bytes each, not premultiplied).
void apply(std::vector<unsigned char>& rgba, int w, int h, const std::vector<Filter>& filters);

// The three box widths whose successive box blurs approximate a Gaussian of `sigma`.
std::array<int, 3> blur_boxes(float sigma);

// The filters as text, for cache keys ("" for none).
std::string key(const std::vector<Filter>& filters);

}  // namespace imagefilter
