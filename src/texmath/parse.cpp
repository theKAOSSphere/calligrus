#include "parse.h"

#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>

#include "utf.h"

namespace texmath {

namespace {

// Deepest allowed nesting of groups/commands. Real formulas stay far below
// this; it exists so hostile input can't overflow the stack.
constexpr size_t MAX_DEPTH = 48;

[[noreturn]] void fail(const std::string& msg) { throw ParseError(msg); }

bool is_one_of(const std::string& n, std::initializer_list<std::string_view> names) {
    for (std::string_view s : names) {
        if (n == s) return true;
    }
    return false;
}

// Trim Unicode whitespace from both ends (Rust's `str::trim`).
std::u32string_view trim(std::u32string_view s) {
    while (!s.empty() && is_whitespace(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_whitespace(s.back())) s.remove_suffix(1);
    return s;
}

// Rust's `str::parse::<f32>()` for the only inputs that can reach it here
// (no letters): `[+-]? (digits | digits '.' digits? | '.' digits)`.
// Anything else is an error, which parse_dimension turns into 0.
std::optional<float> parse_f32(std::u32string_view s) {
    std::string ascii;
    size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == U'+' || s[i] == U'-')) {
        negative = s[i] == U'-';
        i++;
    }
    size_t digits = 0, dots = 0;
    for (; i < s.size(); i++) {
        char32_t c = s[i];
        if (is_ascii_digit(c)) {
            digits++;
        } else if (c == U'.' && dots == 0) {
            dots++;
        } else {
            return std::nullopt;
        }
        ascii += static_cast<char>(c);
    }
    if (digits == 0) return std::nullopt;
    float value = 0.0f;
    auto res = std::from_chars(ascii.data(), ascii.data() + ascii.size(), value);
    if (res.ec == std::errc::result_out_of_range) {
        // Rust rounds out-of-range values to infinity / zero instead of failing.
        double d = 0.0;
        auto res2 = std::from_chars(ascii.data(), ascii.data() + ascii.size(), d);
        if (res2.ec == std::errc::result_out_of_range) {
            // Too long even for a double: decide by the position of the first
            // non-zero digit relative to the decimal point.
            size_t dot = ascii.find('.');
            size_t first = ascii.find_first_not_of("0.");
            bool big = first != std::string::npos && (dot == std::string::npos || first < dot);
            d = big ? std::numeric_limits<double>::infinity() : 0.0;
        }
        value = static_cast<float>(d);
    } else if (res.ec != std::errc()) {
        return std::nullopt;
    }
    return negative ? -value : value;
}

// Parse "3em", "-2mu", "10pt", "1ex" into em.
float parse_dimension(std::u32string_view raw_in) {
    std::u32string_view raw = trim(raw_in);
    size_t split = raw.size();
    for (size_t i = 0; i < raw.size(); i++) {
        if (is_ascii_alpha(raw[i])) {
            split = i;
            break;
        }
    }
    const float num = parse_f32(trim(raw.substr(0, split))).value_or(0.0f);
    const std::string unit = to_utf8(trim(raw.substr(split)));
    if (unit == "em" || unit.empty()) return num;
    if (unit == "ex") return num * 0.43f;
    if (unit == "mu") return num / 18.0f;
    if (unit == "pt") return num / 10.0f;
    if (unit == "px") return num / 16.0f;
    // 10pt font: 1em = 10pt, 1cm = 28.45pt
    if (unit == "cm") return num * 2.845f;
    if (unit == "mm") return num * 0.2845f;
    if (unit == "in") return num * 7.227f;
    return num;
}

node::Accent accent(char32_t ch, Nodes body, bool wide) { return node::Accent{ch, std::move(body), wide}; }

// A single source character as an atom, with its TeX class.
Node char_atom(char32_t c) {
    using C = Class;
    if (is_ascii_alpha(c)) return node::Sym{c, C::Ord, true};
    if (c >= U'α' && c <= U'ω') return node::Sym{c, C::Ord, true};
    if (is_ascii_digit(c)) return node::Sym{c, C::Ord, false};
    switch (c) {
        case U'+': return node::Sym{U'+', C::Bin, false};
        case U'-': return node::Sym{U'−', C::Bin, false};
        case U'*': return node::Sym{U'∗', C::Bin, false};
        case U'=': case U'<': case U'>': case U':': return node::Sym{c, C::Rel, false};
        case U'(': case U'[': return node::Sym{c, C::Open, false};
        case U')': case U']': case U'!': case U'?': return node::Sym{c, C::Close, false};
        case U',': case U';': return node::Sym{c, C::Punct, false};
        case U'`': return node::Sym{U'‘', C::Ord, false};
        case U'×': case U'÷': case U'±': case U'∓': case U'·': case U'⋅': case U'∘':
            return node::Sym{c, C::Bin, false};
        case U'≤': case U'≥': case U'≠': case U'≈': case U'≡': case U'∈': case U'∉': case U'⊂':
        case U'⊆': case U'→': case U'←': case U'⇒': case U'⇔': case U'∼':
            return node::Sym{c, C::Rel, false};
        default: return node::Sym{c, C::Ord, false};
    }
}

class Parser {
public:
    Parser(std::u32string chars, size_t depth) : chars_(std::move(chars)), depth_(depth) {}

    std::optional<Nodes> tag_;  // set by \tag{..}

    std::optional<char32_t> peek() const {
        if (pos_ < chars_.size()) return chars_[pos_];
        return std::nullopt;
    }
    std::optional<char32_t> next() {
        auto c = peek();
        if (c) pos_++;
        return c;
    }
    void skip_ws() {
        while (pos_ < chars_.size() && is_whitespace(chars_[pos_])) pos_++;
    }
    void expect(char32_t c) {
        skip_ws();
        if (next() != c) fail("expected '" + to_utf8(c) + "'");
    }

    // Name of the control sequence starting at the cursor (which must be on a
    // backslash), without consuming anything.
    std::u32string peek_cmd() const {
        size_t i = pos_ + 1;
        if (i >= chars_.size()) return {};
        if (is_ascii_alpha(chars_[i])) {
            std::u32string s;
            while (i < chars_.size() && is_ascii_alpha(chars_[i])) s += chars_[i++];
            // `\operatorname*`, `\tag*`: treat the star as part of the name
            if (i < chars_.size() && chars_[i] == U'*') s += U'*';
            return s;
        }
        return std::u32string(1, chars_[i]);
    }

    std::u32string eat_cmd() {
        std::u32string name = peek_cmd();
        pos_ += 1 + name.size();
        return name;
    }

    // Is the cursor at `&`, `\\`, `\end`, `\right` or `\middle`?
    bool at_stop() const {
        auto c = peek();
        if (c == U'&') return true;
        if (c == U'\\') {
            std::u32string n = peek_cmd();
            return n == U"\\" || n == U"end" || n == U"right" || n == U"middle";
        }
        return false;
    }
    bool at_amp_or_newline() const {
        auto c = peek();
        if (c == U'&') return true;
        if (c == U'\\') return peek_cmd() == U"\\";
        return false;
    }

    // Parse a sequence of atoms until a closing brace, `&`, `\\`, `\end`,
    // `\right`, `\middle` or end of input. The terminator is NOT consumed.
    Nodes row() {
        DepthGuard guard(*this);
        return row_inner();
    }

    // Parse rows/cells separated by `&` and `\\`. `first` is the already
    // parsed first cell. If `end` is set, a matching `\end{end}` terminates.
    std::vector<std::vector<Nodes>> table_body(Nodes first, const std::u32string* end) {
        std::vector<std::vector<Nodes>> rows;
        std::vector<Nodes> current;
        current.push_back(std::move(first));
        for (;;) {
            skip_ws();
            auto c = peek();
            if (c == U'&') {
                next();
                current.push_back(row());
            } else if (c == U'\\' && peek_cmd() == U"\\") {
                eat_cmd();
                // optional `[2pt]` row spacing
                skip_ws();
                if (peek() == U'[') {
                    while (auto d = next()) {
                        if (*d == U']') break;
                    }
                }
                rows.push_back(std::move(current));
                current.clear();
                current.push_back(row());
            } else if (c == U'\\' && peek_cmd() == U"end") {
                eat_cmd();
                std::u32string got = raw_group();
                if (end != nullptr && got != *end) {
                    fail("\\begin{" + to_utf8(*end) + "} ended by \\end{" + to_utf8(got) + "}");
                }
                break;
            } else if (!c && end == nullptr) {
                break;
            } else if (!c) {
                fail("missing \\end");
            } else {
                fail("unexpected '" + to_utf8(*c) + "' in table");
            }
        }
        rows.push_back(std::move(current));
        // drop a trailing empty row (from a final `\\`)
        if (rows.size() > 1) {
            bool all_empty = true;
            for (const Nodes& cell : rows.back()) all_empty = all_empty && cell.empty();
            if (all_empty) rows.pop_back();
        }
        return rows;
    }

private:
    std::u32string chars_;
    size_t pos_ = 0;
    size_t depth_;

    // Counts nesting for MAX_DEPTH (Rust's enter() / `depth -= 1`).
    struct DepthGuard {
        explicit DepthGuard(Parser& p) : p_(p) {
            if (p_.depth_ >= MAX_DEPTH) fail("formula is nested too deeply");
            p_.depth_++;
        }
        ~DepthGuard() { p_.depth_--; }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
        Parser& p_;
    };

    Nodes row_inner() {
        Nodes out;
        for (;;) {
            skip_ws();
            auto c = peek();
            if (!c || *c == U'}') break;
            if (at_stop()) break;
            if (*c == U'\\') {
                std::u32string n = peek_cmd();
                if (n == U"over" || n == U"atop" || n == U"choose") {
                    // Infix fraction: everything so far is the numerator and
                    // everything after is the denominator.
                    std::u32string which = eat_cmd();
                    Nodes den = row();
                    Nodes num = std::move(out);
                    out.clear();
                    std::optional<std::pair<char32_t, char32_t>> delims;
                    if (which == U"choose") delims = std::pair{U'(', U')'};
                    out.push_back(node::Frac{std::move(num), std::move(den), which == U"over", std::nullopt, delims});
                    break;
                }
            }
            out.push_back(atom_with_scripts());
        }
        return out;
    }

    Node atom_with_scripts() {
        skip_ws();
        Node base;
        auto c = peek();
        if (c == U'^' || c == U'_' || c == U'\'') {
            base = node::Empty{};
        } else {
            base = atom();
        }

        // `\limits` / `\nolimits` modify the operator just parsed.
        for (;;) {
            skip_ws();
            if (peek() == U'\\') {
                std::u32string name = peek_cmd();
                if (name == U"limits" || name == U"nolimits") {
                    eat_cmd();
                    if (auto* op = std::get_if<node::Op>(&base.v)) op->limits = name == U"limits";
                    continue;
                }
            }
            break;
        }

        std::optional<Nodes> sub, sup;
        for (;;) {
            skip_ws();
            auto d = peek();
            if (d == U'^') {
                next();
                Nodes a = arg();
                if (!sup) sup.emplace();
                for (Node& n : a) sup->push_back(std::move(n));
            } else if (d == U'_') {
                next();
                Nodes a = arg();
                if (!sub) sub.emplace();
                for (Node& n : a) sub->push_back(std::move(n));
            } else if (d == U'\'') {
                next();
                if (!sup) sup.emplace();
                sup->push_back(node::Sym{U'′', Class::Ord, false});
            } else {
                break;
            }
        }
        if (sub || sup) return node::Script{Box<Node>(std::move(base)), std::move(sub), std::move(sup)};
        return base;
    }

    // One macro argument: either a `{group}` or a single token.
    Nodes arg() {
        skip_ws();
        auto c = peek();
        if (c == U'{') {
            next();
            Nodes r = row();
            expect(U'}');
            return r;
        }
        if (!c || *c == U'}') fail("missing argument");
        Nodes one;
        one.push_back(atom());
        return one;
    }

    // Collect the text of a `[...]` argument (braces may nest; the closing
    // `]` is consumed but not included).
    std::u32string bracket_text() {
        std::u32string s;
        int depth = 0;
        while (auto c = next()) {
            if (*c == U'{') {
                depth += 1;
            } else if (*c == U'}') {
                depth -= 1;
            } else if (*c == U']' && depth == 0) {
                break;
            }
            s += *c;
        }
        return s;
    }

    // An optional `[...]` argument, parsed as math.
    std::optional<Nodes> optional_arg() {
        skip_ws();
        if (peek() != U'[') return std::nullopt;
        next();
        Parser sub(bracket_text(), depth_);
        return sub.row();
    }

    // Raw text of a `{...}` argument (nested braces allowed, no parsing).
    std::u32string raw_group() {
        skip_ws();
        if (next() != U'{') fail("expected '{'");
        int depth = 1;
        std::u32string s;
        while (auto c = next()) {
            if (*c == U'{') {
                depth += 1;
                s += *c;
            } else if (*c == U'}') {
                depth -= 1;
                if (depth == 0) return s;
                s += *c;
            } else {
                s += *c;
            }
        }
        fail("unclosed '{'");
    }

    Node atom() {
        DepthGuard guard(*this);
        return atom_inner();
    }

    Node atom_inner() {
        skip_ws();
        auto c = peek();
        if (!c) fail("unexpected end");
        switch (*c) {
            case U'{': {
                next();
                Nodes r = row();
                expect(U'}');
                return node::Group{std::move(r)};
            }
            case U'\\': return command();
            case U'~': next(); return node::Space{0.33f};
            case U'$': next(); return node::Empty{};
            case U'}': fail("unexpected '}'");
            case U'&': fail("unexpected '&'");
            default: next(); return char_atom(*c);
        }
    }

    // Read a delimiter after `\left`, `\right`, `\big`...: a char or `\cmd`.
    std::optional<char32_t> delimiter() {
        skip_ws();
        auto c = next();
        if (!c) fail("missing delimiter");
        if (*c == U'.') return std::nullopt;
        if (*c == U'\\') {
            pos_ -= 1;
            const std::string name = to_utf8(eat_cmd());
            if (is_one_of(name, {"{", "lbrace", "rbrace", "}"})) {
                return (name == "}" || name == "rbrace") ? U'}' : U'{';
            }
            if (is_one_of(name, {"|", "Vert", "lVert", "rVert"})) return U'‖';
            if (is_one_of(name, {"vert", "lvert", "rvert"})) return U'|';
            if (name == "backslash") return U'\\';
            if (name == ".") return std::nullopt;
            if (auto s = lookup(name)) return s->ch;
            return std::nullopt;
        }
        return *c;
    }

    Node font(FontStyle style) { return node::Font{style, arg()}; }
    Node fenced1(char32_t l, char32_t r) {
        std::vector<Nodes> segs;
        segs.push_back(arg());
        return node::Fenced{l, r, std::move(segs), {}};
    }

    Node command() {
        const std::u32string name32 = eat_cmd();
        const std::string n = to_utf8(name32);
        using F = FontStyle;
        auto sym = [](char32_t ch, Class cls) { return node::Sym{ch, cls, false}; };

        // ---- spacing ----
        if (n == "," || n == "thinspace") return node::Space{3.0f / 18.0f};
        if (n == ":" || n == ">" || n == "medspace") return node::Space{4.0f / 18.0f};
        if (n == ";" || n == "thickspace") return node::Space{5.0f / 18.0f};
        if (n == "!" || n == "negthinspace") return node::Space{-3.0f / 18.0f};
        if (n == "negmedspace") return node::Space{-4.0f / 18.0f};
        if (n == "negthickspace") return node::Space{-5.0f / 18.0f};
        if (n == " ") return node::Space{0.33f};
        if (n == "quad") return node::Space{1.0f};
        if (n == "qquad") return node::Space{2.0f};
        if (n == "enspace") return node::Space{0.5f};
        if (is_one_of(n, {"hspace", "hspace*", "mkern", "kern", "mskip", "hskip"})) {
            std::u32string raw = peek() == U'{' ? raw_group() : raw_dimension();
            return node::Space{parse_dimension(raw)};
        }

        // ---- fractions & roots ----
        if (is_one_of(n, {"frac", "dfrac", "tfrac", "cfrac"})) {
            Nodes num = arg();
            Nodes den = arg();
            std::optional<Style> force;
            if (n == "dfrac" || n == "cfrac") force = Style::Display;
            if (n == "tfrac") force = Style::Text;
            return node::Frac{std::move(num), std::move(den), true, force, std::nullopt};
        }
        if (is_one_of(n, {"binom", "dbinom", "tbinom"})) {
            Nodes num = arg();
            Nodes den = arg();
            std::optional<Style> force;
            if (n == "dbinom") force = Style::Display;
            if (n == "tbinom") force = Style::Text;
            return node::Frac{std::move(num), std::move(den), false, force, std::pair{U'(', U')'}};
        }
        if (n == "sqrt") {
            skip_ws();
            std::optional<Nodes> index;
            if (peek() == U'[') {
                next();
                Parser sub(bracket_text(), depth_);
                index = sub.row();
            }
            Nodes body = arg();
            return node::Sqrt{std::move(index), std::move(body)};
        }

        // ---- text ----
        if (is_one_of(n, {"text", "textrm", "mbox", "hbox", "textnormal"})) return node::Text{raw_group(), F::Roman};
        if (n == "textbf") return node::Text{raw_group(), F::Bold};
        if (n == "textit" || n == "emph") return node::Text{raw_group(), F::Italic};
        if (n == "textsf") return node::Text{raw_group(), F::Sans};
        if (n == "texttt") return node::Text{raw_group(), F::Mono};
        if (is_one_of(n, {"operatorname", "operatorname*", "DeclareMathOperator"})) {
            std::u32string s = raw_group();
            return node::Op{false, 0, std::move(s), n == "operatorname*", std::nullopt};
        }

        // ---- math alphabets ----
        if (is_one_of(n, {"mathrm", "rm", "up", "mathup"})) return font(F::Roman);
        if (is_one_of(n, {"mathbf", "bf", "mathbfup"})) return font(F::Bold);
        if (is_one_of(n, {"boldsymbol", "bm", "pmb", "mathbfit"})) return font(F::BoldItalic);
        if (n == "mathit" || n == "it") return font(F::Italic);
        if (n == "mathnormal") return font(F::Default);
        if (is_one_of(n, {"mathbb", "Bbb", "mathds"})) return font(F::Blackboard);
        if (is_one_of(n, {"mathcal", "mathscr", "cal"})) return font(F::Script);
        if (n == "mathfrak" || n == "frak") return font(F::Fraktur);
        if (n == "mathsf" || n == "sf") return font(F::Sans);
        if (n == "mathtt" || n == "tt") return font(F::Mono);

        // ---- delimiters ----
        if (n == "left") {
            std::optional<char32_t> left = delimiter();
            std::vector<Nodes> segs;
            std::vector<char32_t> mids;
            for (;;) {
                segs.push_back(row());
                skip_ws();
                if (peek() != U'\\') fail("missing \\right");
                std::u32string cmd = eat_cmd();
                if (cmd == U"middle") {
                    mids.push_back(delimiter().value_or(U'|'));
                } else if (cmd == U"right") {
                    std::optional<char32_t> right = delimiter();
                    return node::Fenced{left, right, std::move(segs), std::move(mids)};
                } else {
                    fail("missing \\right");
                }
            }
        }
        if (is_one_of(n, {"big", "Big", "bigg", "Bigg", "bigl", "Bigl", "biggl", "Biggl", "bigr", "Bigr", "biggr",
                          "Biggr", "bigm", "Bigm", "biggm", "Biggm"})) {
            // Rust: n.trim_end_matches(['l', 'r', 'm'])
            std::string base = n;
            while (!base.empty() && (base.back() == 'l' || base.back() == 'r' || base.back() == 'm')) base.pop_back();
            uint8_t level = base == "big" ? 1 : base == "Big" ? 2 : base == "bigg" ? 3 : 4;
            std::optional<char32_t> ch = delimiter();
            // The trailing l/r/m picks the class; a bare `\big` follows the delimiter.
            Class cls = Class::Ord;
            const char last = n.back();
            if (last == 'r') {
                cls = Class::Close;
            } else if (last == 'l') {
                cls = Class::Open;
            } else if (last == 'm') {
                cls = Class::Rel;
            } else if (ch && (*ch == U'(' || *ch == U'[' || *ch == U'⟨' || *ch == U'{' || *ch == U'⌊' || *ch == U'⌈')) {
                cls = Class::Open;
            } else if (ch && (*ch == U')' || *ch == U']' || *ch == U'⟩' || *ch == U'}' || *ch == U'⌋' || *ch == U'⌉')) {
                cls = Class::Close;
            }
            return node::Big{ch, level, cls};
        }

        // ---- accents & over/under ----
        if (n == "hat") return accent(0x0302, arg(), false);
        if (n == "widehat") return accent(0x0302, arg(), true);
        if (n == "check") return accent(0x030C, arg(), false);
        if (n == "tilde") return accent(0x0303, arg(), false);
        if (n == "widetilde") return accent(0x0303, arg(), true);
        if (n == "acute") return accent(0x0301, arg(), false);
        if (n == "grave") return accent(0x0300, arg(), false);
        if (n == "dot") return accent(0x0307, arg(), false);
        if (n == "ddot") return accent(0x0308, arg(), false);
        if (n == "dddot") return accent(0x20DB, arg(), false);
        if (n == "breve") return accent(0x0306, arg(), false);
        if (n == "bar") return accent(0x0304, arg(), false);
        if (n == "mathring") return accent(0x030A, arg(), false);
        if (n == "vec") return accent(0x20D7, arg(), false);
        if (n == "overrightarrow") return accent(0x20D7, arg(), true);
        if (n == "overleftarrow") return accent(0x20D6, arg(), true);
        if (n == "overline") return node::Over{arg(), Mark::Bar, std::nullopt, false};
        if (n == "underline") return node::Over{arg(), Mark::Bar, std::nullopt, true};
        if (n == "overbrace") return node::Over{arg(), Mark::Brace, std::nullopt, false};
        if (n == "underbrace") return node::Over{arg(), Mark::Brace, std::nullopt, true};
        if (n == "overset" || n == "stackrel" || n == "underset") {
            Nodes label = arg();
            Nodes body = arg();
            return node::Over{std::move(body), Mark::None, std::move(label), n == "underset"};
        }
        if (n == "substack") {
            return node::Matrix{substack_rows(), std::nullopt, std::nullopt, MatKind{MatKind::Substack, {}}};
        }

        // ---- structure / styles ----
        if (n == "begin") return environment();
        if (n == "end") fail("unexpected \\end");
        if (n == "displaystyle") return node::StyleSwitch{Style::Display};
        if (n == "textstyle") return node::StyleSwitch{Style::Text};
        if (n == "scriptstyle") return node::StyleSwitch{Style::Script};
        if (n == "scriptscriptstyle") return node::StyleSwitch{Style::ScriptScript};
        if (is_one_of(n, {"limits", "nolimits", "nonumber", "notag", "displaylimits"})) return node::Empty{};
        if (is_one_of(n, {"label", "color", "pagecolor"})) {
            raw_group();
            return node::Empty{};
        }
        if (is_one_of(n, {"textcolor", "mathcolor", "colorbox"})) {
            raw_group();  // the colour name: we draw in the text colour
            return node::Group{arg()};
        }
        if (n == "ensuremath") return node::Group{arg()};
        if (n == "boxed" || n == "fbox") return node::Boxed{arg()};
        if (n == "cancel") return node::Cancel{arg(), false, false};
        if (n == "bcancel") return node::Cancel{arg(), true, false};
        if (n == "xcancel") return node::Cancel{arg(), false, true};
        if (n == "xrightarrow" || n == "xleftarrow") {
            std::optional<Nodes> under = optional_arg();
            Nodes over = arg();
            return node::XArrow{n == "xrightarrow" ? U'→' : U'←', std::move(over), std::move(under)};
        }
        if (is_one_of(n, {"hline", "toprule", "midrule", "bottomrule"})) return node::Empty{};
        if (n == "cline") {
            raw_group();
            return node::Empty{};
        }

        // physics-package conveniences
        if (n == "abs") return fenced1(U'|', U'|');
        if (n == "norm") return fenced1(U'‖', U'‖');
        if (n == "ket") return fenced1(U'|', U'⟩');
        if (n == "bra") return fenced1(U'⟨', U'|');
        if (n == "braket") {
            Nodes a = arg();
            Nodes b = arg();
            std::vector<Nodes> segs;
            segs.push_back(std::move(a));
            segs.push_back(std::move(b));
            return node::Fenced{U'⟨', U'⟩', std::move(segs), {U'|'}};
        }
        if (n == "vb") return font(F::Bold);
        if (n == "dd") return node::Sym{U'd', Class::Ord, false};
        if (n == "mathstrut") {
            Nodes body;
            body.push_back(sym(U'(', Class::Ord));
            return node::Phantom{std::move(body), false, true};
        }
        if (n == "tag" || n == "tag*") {
            std::u32string t = raw_group();
            Parser sub(std::move(t), depth_);
            Nodes body = sub.row();
            if (n == "tag") {
                body.insert(body.begin(), sym(U'(', Class::Open));
                body.push_back(sym(U')', Class::Close));
            }
            tag_ = std::move(body);
            return node::Empty{};
        }
        if (n == "phantom") return node::Phantom{arg(), true, true};
        if (n == "hphantom") return node::Phantom{arg(), true, false};
        if (n == "vphantom") return node::Phantom{arg(), false, true};
        if (n == "mathord") return node::Classed{Class::Ord, arg()};
        if (n == "mathop") return node::Classed{Class::Op, arg()};
        if (n == "mathbin") return node::Classed{Class::Bin, arg()};
        if (n == "mathrel") return node::Classed{Class::Rel, arg()};
        if (n == "mathopen") return node::Classed{Class::Open, arg()};
        if (n == "mathclose") return node::Classed{Class::Close, arg()};
        if (n == "mathpunct") return node::Classed{Class::Punct, arg()};
        if (n == "mathinner") return node::Classed{Class::Inner, arg()};
        if (n == "not") {
            Node next_atom = atom();
            if (auto* s = std::get_if<node::Sym>(&next_atom.v)) {
                char32_t neg = s->ch;
                switch (s->ch) {
                    case U'=': neg = U'≠'; break;
                    case U'<': neg = U'≮'; break;
                    case U'>': neg = U'≯'; break;
                    case U'∈': neg = U'∉'; break;
                    case U'∋': neg = U'∌'; break;
                    case U'⊂': neg = U'⊄'; break;
                    case U'⊃': neg = U'⊅'; break;
                    case U'⊆': neg = U'⊈'; break;
                    case U'⊇': neg = U'⊉'; break;
                    case U'≡': neg = U'≢'; break;
                    case U'∼': neg = U'≁'; break;
                    case U'≈': neg = U'≉'; break;
                    case U'≤': neg = U'≰'; break;
                    case U'≥': neg = U'≱'; break;
                    case U'≅': neg = U'≇'; break;
                    case U'∣': neg = U'∤'; break;
                    case U'∥': neg = U'∦'; break;
                    case U'→': neg = U'↛'; break;
                    case U'←': neg = U'↚'; break;
                    case U'⇒': neg = U'⇏'; break;
                    case U'⇔': neg = U'⇎'; break;
                    case U'∃': neg = U'∄'; break;
                    default: break;
                }
                s->ch = neg;
            }
            return next_atom;
        }

        // ---- symbols ----
        if (auto op = big_operator(n)) return node::Op{true, op->ch, {}, op->limits, std::nullopt};
        if (auto op = named_operator(n)) return node::Op{false, 0, std::u32string(op->text), op->limits, std::nullopt};
        if (auto s = lookup(n)) {
            const char32_t c = s->ch;
            const bool italic = (c >= U'α' && c <= U'ω') || c == U'ϑ' || c == U'ϰ' || c == U'ϕ' || c == U'ϱ' ||
                                c == U'ϖ' || c == U'ϵ' || c == U'ℓ';
            return node::Sym{c, s->cls, italic && c != U'ℓ'};
        }
        // Unknown command: show it literally so the user sees what's wrong.
        return node::Text{U"\\" + name32, F::Mono};
    }

    // Unbraced dimension like `3em` after `\kern`.
    std::u32string raw_dimension() {
        skip_ws();
        std::u32string s;
        while (auto c = peek()) {
            if (is_ascii_alnum(*c) || *c == U'.' || *c == U'-' || *c == U'+') {
                s += *c;
                pos_++;
            } else {
                break;
            }
        }
        return s;
    }

    // ---- environments ----

    Node environment() {
        const std::u32string name = raw_group();
        std::u32string env32 = name;
        while (!env32.empty() && env32.back() == U'*') env32.pop_back();  // trim_end_matches('*')
        const std::string env = to_utf8(env32);
        std::optional<char32_t> left, right;
        MatKind kind;
        if (env == "matrix") {
            kind.tag = MatKind::Matrix;
        } else if (env == "pmatrix") {
            left = U'(', right = U')', kind.tag = MatKind::Matrix;
        } else if (env == "bmatrix") {
            left = U'[', right = U']', kind.tag = MatKind::Matrix;
        } else if (env == "Bmatrix") {
            left = U'{', right = U'}', kind.tag = MatKind::Matrix;
        } else if (env == "vmatrix") {
            left = U'|', right = U'|', kind.tag = MatKind::Matrix;
        } else if (env == "Vmatrix") {
            left = U'‖', right = U'‖', kind.tag = MatKind::Matrix;
        } else if (env == "smallmatrix") {
            kind.tag = MatKind::Small;
        } else if (env == "cases" || env == "dcases") {
            left = U'{', kind.tag = MatKind::Cases;
        } else if (env == "rcases") {
            right = U'}', kind.tag = MatKind::Cases;
        } else if (is_one_of(env, {"aligned", "align", "alignedat", "split", "flalign", "eqnarray"})) {
            if (env == "alignedat") {
                try {
                    raw_group();  // the column count; ignored (errors too, like Rust's `let _ =`)
                } catch (const ParseError&) {
                }
            }
            kind.tag = MatKind::Aligned;
        } else if (is_one_of(env, {"gather", "gathered", "equation", "displaymath", "multline"})) {
            kind.tag = MatKind::Gathered;
        } else if (env == "array" || env == "tabular") {
            const std::u32string spec = raw_group();
            kind.tag = MatKind::Array;
            for (char32_t c : spec) {
                if (c == U'l' || c == U'c' || c == U'r') kind.cols.push_back(c);
            }
        } else if (env == "subarray") {
            try {
                raw_group();
            } catch (const ParseError&) {
            }
            kind.tag = MatKind::Substack;
        } else {
            fail("unknown environment '" + env + "'");
        }
        Nodes first = row();
        auto rows = table_body(std::move(first), &name);
        return node::Matrix{std::move(rows), left, right, std::move(kind)};
    }

    // `\substack{a \\ b}`: rows separated by `\\`, one cell each.
    std::vector<std::vector<Nodes>> substack_rows() {
        skip_ws();
        if (next() != U'{') fail("expected '{' after \\substack");
        std::vector<std::vector<Nodes>> rows;
        for (;;) {
            Nodes cell = row();
            std::vector<Nodes> r;
            r.push_back(std::move(cell));
            rows.push_back(std::move(r));
            skip_ws();
            auto c = peek();
            if (c == U'\\' && peek_cmd() == U"\\") {
                eat_cmd();
            } else if (c == U'}') {
                next();
                break;
            } else {
                fail("unclosed \\substack");
            }
        }
        return rows;
    }
};

}  // namespace

Parsed parse(const std::u32string& src) {
    Parser p(src, 0);
    Nodes first = p.row();
    p.skip_ws();
    Nodes nodes;
    auto c = p.peek();
    if (!c) {
        nodes = std::move(first);
    } else if ((*c == U'&' || *c == U'\\') && p.at_amp_or_newline()) {
        // Top-level `&` / `\\` (multi-line display math without an environment).
        auto rows = p.table_body(std::move(first), nullptr);
        bool multi = false;
        for (const auto& r : rows) multi = multi || r.size() > 1;
        MatKind kind{multi ? MatKind::Aligned : MatKind::Gathered, {}};
        nodes.push_back(node::Matrix{std::move(rows), std::nullopt, std::nullopt, std::move(kind)});
    } else {
        fail("unexpected '" + to_utf8(*c) + "'");
    }
    return Parsed{std::move(nodes), std::move(p.tag_)};
}

}  // namespace texmath
