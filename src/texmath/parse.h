// LaTeX math parser: source string -> tree of `Node`s.
// C++ port of the Rust port
//
// The parser is deliberately forgiving. Unknown `\commands` become literal
// text rather than errors, so a typo never blanks out a whole equation.
// Only structural problems (unbalanced braces, missing `\right`, ...) fail.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "symbols.h"

namespace texmath {

// TeX's four math styles. Display is "big equation on its own line",
// Text is "inline in a paragraph", Script/ScriptScript are sub/superscripts.
enum class Style { Display, Text, Script, ScriptScript };

// What decorates a body in `node::Over`.
enum class Mark { Bar, Brace, None };

// What kind of grid a `node::Matrix` is (changes cell style, alignment, gaps).
struct MatKind {
    enum Tag { Matrix, Small, Cases, Aligned, Gathered, Array, Substack } tag = Matrix;
    std::vector<char32_t> cols;  // column alignments ('l'/'c'/'r'), only for Array
};

// The structural errors the parser reports. Thrown inside the engine and
// turned into an error string at the public API (texmath.cpp).
struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Node;
using Nodes = std::vector<Node>;

// A heap-allocated value with value semantics (copying a Box copies its
// contents), like Rust's `Box<T>`. Used where a Node must contain one Node.
template <class T>
class Box {
public:
    explicit Box(T value) : p_(std::make_unique<T>(std::move(value))) {}
    Box(const Box& other) : p_(std::make_unique<T>(*other.p_)) {}
    Box(Box&&) noexcept = default;
    Box& operator=(const Box& other) {
        p_ = std::make_unique<T>(*other.p_);
        return *this;
    }
    Box& operator=(Box&&) noexcept = default;
    ~Box() = default;
    const T& operator*() const { return *p_; }
    const T* operator->() const { return p_.get(); }
    T& operator*() { return *p_; }
    T* operator->() { return p_.get(); }

private:
    std::unique_ptr<T> p_;
};

// The node kinds, one struct each (Rust's `enum Node` variants).
namespace node {
struct Empty {};
// A single character atom. `italic` = letters that TeX italicises by default.
struct Sym {
    char32_t ch;
    Class cls;
    bool italic;
};
struct Group {
    Nodes body;
};
struct Text {
    std::u32string s;
    FontStyle style;
};
struct Space {
    float em;
};
struct Frac {
    Nodes num, den;
    bool rule;                                            // fraction bar (false for \binom, \atop)
    std::optional<Style> force;                           // \dfrac, \tfrac
    std::optional<std::pair<char32_t, char32_t>> delims;  // \binom's parentheses
};
struct Sqrt {
    std::optional<Nodes> index;
    Nodes body;
};
struct Script {
    Box<Node> base;
    std::optional<Nodes> sub, sup;
};
// A large operator: either a glyph such as `∑`/`∫` or an upright word such as `lim`.
struct Op {
    bool is_glyph;
    char32_t glyph;      // when is_glyph
    std::u32string name; // otherwise
    bool default_limits;
    std::optional<bool> limits;  // \limits / \nolimits
};
struct Fenced {
    std::optional<char32_t> left, right;
    std::vector<Nodes> segs;      // the pieces between \left, \middle..., \right
    std::vector<char32_t> mids;   // the \middle delimiters
};
struct Big {
    std::optional<char32_t> ch;
    uint8_t level;  // 1..4 = \big, \Big, \bigg, \Bigg
    Class cls;
};
struct Accent {
    char32_t ch;
    Nodes body;
    bool wide;
};
struct Over {
    Nodes body;
    Mark mark;
    std::optional<Nodes> label;
    bool under;
};
struct Matrix {
    std::vector<std::vector<Nodes>> rows;
    std::optional<char32_t> left, right;
    MatKind kind;
};
struct Font {
    FontStyle style;
    Nodes body;
};
struct StyleSwitch {
    Style style;
};
struct Classed {
    Class cls;
    Nodes body;
};
struct Phantom {
    Nodes body;
    bool horizontal, vertical;
};
// `\xrightarrow[under]{over}`
struct XArrow {
    char32_t ch;
    Nodes over;
    std::optional<Nodes> under;
};
struct Boxed {
    Nodes body;
};
// `\cancel` (back=false), `\bcancel` (back=true), `\xcancel` (both=true)
struct Cancel {
    Nodes body;
    bool back, both;
};
}  // namespace node

struct Node {
    std::variant<node::Empty, node::Sym, node::Group, node::Text, node::Space, node::Frac, node::Sqrt,
                 node::Script, node::Op, node::Fenced, node::Big, node::Accent, node::Over, node::Matrix,
                 node::Font, node::StyleSwitch, node::Classed, node::Phantom, node::XArrow, node::Boxed,
                 node::Cancel>
        v;

    // Any node kind converts to a Node, so `Node n = node::Space{1.0f};` works.
    template <class T>
        requires(!std::is_same_v<std::decay_t<T>, Node>)
    Node(T value) : v(std::move(value)) {}
    Node() = default;
};

struct Parsed {
    Nodes nodes;
    std::optional<Nodes> tag;  // equation tag from `\tag{..}`
};

// Parse a formula given as Unicode code points. Throws ParseError.
Parsed parse(const std::u32string& src);

}  // namespace texmath
