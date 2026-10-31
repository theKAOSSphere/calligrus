// TeX-style math layout: `Node` tree -> boxes of positioned glyphs and rules.
// C++ port of the Rust port
//
// This follows the TeXbook's Appendix G, but takes every font-dependent
// number from the font's own OpenType `MATH` table (axis height, script
// shifts, fraction gaps, radical gaps, ...), the way modern engines do.
//
// Coordinates: units are "em of the base font size". `x` grows right, `y`
// grows UP from the baseline. A box has a width `w`, a height `h` above the
// baseline and a depth `d` below it.
#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

#include "font.h"
#include "parse.h"

namespace texmath {

// A glyph with its origin at (x, y), drawn `sx`/`sy` em wide/tall.
struct GlyphItem {
    uint16_t gid;
    float x, y, sx, sy;
};
// A filled rectangle (fraction bars, radical bars, ...). `y` is the bottom edge.
struct RuleItem {
    float x, y, w, h;
};
// A stroked line segment of thickness `t` (strike-throughs for `\cancel`).
struct LineItem {
    float x1, y1, x2, y2, t;
};
using Item = std::variant<GlyphItem, RuleItem, LineItem>;

struct MBox {
    float w = 0.0f, h = 0.0f, d = 0.0f;
    std::vector<Item> items;
    // Italic correction of the last glyph (extra room before a superscript).
    float ic = 0.0f;
    // Where an accent should attach, if not the middle.
    std::optional<float> accent_x;
    // True if this box is a single character (changes script placement rules).
    bool is_char = false;
    // The glyph whose math-kern table should shape script placement (single glyph boxes).
    std::optional<uint16_t> kern_gid;
    // Vertical offset of that glyph's origin from the box baseline.
    float kern_dy = 0.0f;

    static MBox space(float w) {
        MBox b;
        b.w = w;
        return b;
    }

    // Add `other` at offset (dx, dy), growing h/d but not touching `w`.
    void place(const MBox& other, float dx, float dy);
};

class Layouter {
public:
    explicit Layouter(Font font);

    const Font& face() const { return font_; }
    float upem() const { return upem_; }

    // The text a glyph stands for in a PDF (its character, math styles folded to plain letters);
    // nullopt: not a plain character glyph (a size variant, a piece of a big delimiter).
    std::optional<char32_t> text_of(uint16_t gid) const;

    // Lay out a whole formula. `display` selects display vs inline style.
    MBox layout(const Nodes& nodes, bool display) const;

private:
    struct Ctx {
        Style style;
        bool cramped;
        FontStyle font;
    };
    // Result of stretching a glyph horizontally (for braces / wide accents).
    struct Stretched {
        std::vector<Item> items;
        float w, ymin, ymax;
    };
    // A glyph assembly: items (origin at 0), width, ymin, ymax in em.
    struct Assembly {
        std::vector<Item> items;
        float w, ymin, ymax;
    };
    struct Extents {
        float xmin, xmax, ymin, ymax;
    };

    Font font_;
    // Glyph id -> the character it is the plain glyph of (the lowest code point the cmap sends to it).
    std::unordered_map<uint16_t, char32_t> text_of_;
    float upem_;
    float script_scale_;
    float script_script_scale_;

    // ----- font access helpers -----
    float k(int16_t value, Ctx ctx) const;
    float scale(Style style) const;
    float axis(Ctx ctx) const;
    static bool is_script(Style style);
    static Style sup_style(Style style);
    static Style num_style(Style style);
    Extents glyph_extents(uint16_t gid) const;
    MBox glyph(char32_t ch, Ctx ctx) const;
    MBox glyph_id(uint16_t gid, float s) const;
    char32_t sym_char(char32_t ch, bool italic, Ctx ctx) const;

    // ----- rows -----
    Class class_of(const Node& n) const;
    MBox row(const Nodes& nodes, Ctx ctx) const;
    MBox node(const Node& n, Class cls, Ctx ctx) const;
    MBox text(const std::u32string& text, FontStyle style, Ctx ctx) const;

    // ----- fractions, scripts, operators -----
    MBox frac(const node::Frac& f, Ctx ctx) const;
    bool wants_limits(const Node& base, Ctx ctx) const;
    MBox script(const node::Script& sc, Class cls, Ctx ctx) const;
    float math_kern(const MBox& base, bool top_right, float lo, float hi, float s) const;
    MBox limits(MBox b, const std::optional<Nodes>& sub, const std::optional<Nodes>& sup, Ctx ctx) const;
    MBox op(const node::Op& o, Ctx ctx) const;

    // ----- stretchy things -----
    std::optional<uint16_t> vertical_variant(uint16_t gid, float min_em) const;
    MBox delim(std::optional<char32_t> ch, float target, Ctx ctx) const;
    Assembly assemble(const std::vector<GlyphPart>& parts, float min_overlap, float target_units, bool vertical,
                      float s) const;
    std::optional<Stretched> hstretch(char32_t ch, float target, Ctx ctx) const;
    MBox fenced(const node::Fenced& f, Ctx ctx) const;

    // ----- roots, accents, decorations -----
    MBox sqrt(const node::Sqrt& sq, Ctx ctx) const;
    MBox accent(const node::Accent& a, Ctx ctx) const;
    MBox over(const node::Over& o, Ctx ctx) const;
    MBox xarrow(const node::XArrow& xa, Ctx ctx) const;
    MBox boxed(const Nodes& body, Ctx ctx) const;
    MBox cancel(const node::Cancel& c, Ctx ctx) const;

    // ----- matrices -----
    MBox matrix(const node::Matrix& m, Ctx ctx) const;
};

}  // namespace texmath
