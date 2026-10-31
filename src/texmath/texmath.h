// A small, self-contained TeX math renderer (C++ port of the Rust port's
// `texmath` module, which lives in the Rust port).
//
// Pipeline:  LaTeX source --parse--> Node tree --layout--> MBox
//            --rasterize--> white-on-transparent RGBA bitmap.
//
// No web tech involved: the layout rules are TeX's, the numbers come from the
// Latin Modern Math font's OpenType MATH table, and the pixels come from
// filling the font's glyph outlines.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace texmath {

// A glyph of a formula that a PDF export draws as real text (spec 33): left out of `Rendered::rgba`.
struct MathGlyph {
    uint16_t gid = 0;   // its id in the math font
    std::string text;   // the character it is (UTF-8, math styles folded to plain letters): what copying gives
    float x = 0, y = 0; // its baseline origin in pixels from the bitmap's top-left corner
    float size = 0;     // pixels per em
};

// A rendered formula, ready to be uploaded as a texture.
struct Rendered {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;  // premultiplied RGBA8 (white ink)
    // Baseline-left corner of the formula inside the bitmap, in pixels.
    float origin_x = 0.0f, origin_y = 0.0f;
    // Logical box in em: width, height above the baseline, depth below it.
    float em_w = 0.0f, em_h = 0.0f, em_d = 0.0f;
    // The glyphs drawn as text instead (see render's `capture`); empty normally.
    std::vector<MathGlyph> glyphs;
};

struct Equation {
    Rendered main;
    std::optional<Rendered> tag;  // `\tag{..}` label, if any
};

// Either the equation or an error message (Rust's `Result<Equation, String>`).
using Result = std::variant<Equation, std::string>;

// Load the math font (latinmodern-math.otf) once, before rendering.
// Returns false and sets `error` if the file is missing or unusable.
bool init_font(const std::filesystem::path& path, std::string* error = nullptr);
// The same from the font file's bytes (the app embeds the font in the exe).
bool init_font_data(std::vector<uint8_t> bytes, std::string* error = nullptr);
bool font_ready();

// Render TeX math given as UTF-8. `display` = display style (big operators,
// limits above/below). `ppem` = pixels per em of the surrounding text.
// Never throws: malformed input, absurd sizes or a missing font give an error.
//
// `capture` (a PDF export with real text): every glyph that has a character is left out of the bitmap and listed
// in `Rendered::glyphs` instead; rules, radical signs, big delimiters and other glyphs without a character stay in it.
Result render(std::string_view tex, bool display, float ppem, bool capture = false);

}  // namespace texmath
