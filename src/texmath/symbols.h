// Symbol tables: LaTeX command names -> Unicode characters + TeX atom class,
// and the "math alphabet" mappings (bold, blackboard, script, ...).
// C++ port of the Rust port
#pragma once

#include <optional>
#include <string_view>

namespace texmath {

// TeX atom classes. They decide the spacing between neighbouring atoms
// (e.g. `a + b` gets space around the Bin `+`, but `a, b` only after the Punct).
enum class Class { Ord, Op, Bin, Rel, Open, Close, Punct, Inner };

// Which "math alphabet" letters are drawn in (`\mathbf`, `\mathbb`, ...).
enum class FontStyle {
    Default,  // TeX default: Latin/Greek lowercase letters italic, everything else upright.
    Roman,
    Bold,
    BoldItalic,
    Italic,
    Blackboard,
    Script,
    Fraktur,
    Sans,
    Mono,
};

struct Sym {
    char32_t ch;
    Class cls;
};

// Look up a `\command` that stands for a single symbol.
std::optional<Sym> lookup(std::string_view name);

// Large operators: the glyph and "limits by default in display style".
struct BigOp {
    char32_t ch;
    bool limits;
};
std::optional<BigOp> big_operator(std::string_view name);

// Named operators set in upright type: printed text and "limits in display style".
struct NamedOp {
    std::u32string_view text;
    bool limits;
};
std::optional<NamedOp> named_operator(std::string_view name);

// TeX inter-atom spacing in mu (1 mu = 1/18 em), from The TeXbook p.170.
float spacing_mu(Class left, Class right, bool script);

// Map a character into the requested math alphabet using the Unicode
// "Mathematical Alphanumeric Symbols" block (U+1D400..).
char32_t style_char(char32_t c, FontStyle style);

}  // namespace texmath
