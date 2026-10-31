#include "symbols.h"

#include <string>
#include <unordered_map>

namespace texmath {

namespace {

struct SymEntry {
    std::string_view name;
    char32_t ch;
    Class cls;
};

// One row per command name (Rust's `"vee" | "lor" => ...` arms are split into
// one row each). Large operators are handled separately (see big_operator).
constexpr SymEntry kSymbols[] = {
    // --- Greek lowercase (drawn italic by default) ---
    {"alpha", U'α', Class::Ord},
    {"beta", U'β', Class::Ord},
    {"gamma", U'γ', Class::Ord},
    {"delta", U'δ', Class::Ord},
    {"epsilon", U'ϵ', Class::Ord},
    {"varepsilon", U'ε', Class::Ord},
    {"zeta", U'ζ', Class::Ord},
    {"eta", U'η', Class::Ord},
    {"theta", U'θ', Class::Ord},
    {"vartheta", U'ϑ', Class::Ord},
    {"iota", U'ι', Class::Ord},
    {"kappa", U'κ', Class::Ord},
    {"varkappa", U'ϰ', Class::Ord},
    {"lambda", U'λ', Class::Ord},
    {"mu", U'μ', Class::Ord},
    {"nu", U'ν', Class::Ord},
    {"xi", U'ξ', Class::Ord},
    {"omicron", U'ο', Class::Ord},
    {"pi", U'π', Class::Ord},
    {"varpi", U'ϖ', Class::Ord},
    {"rho", U'ρ', Class::Ord},
    {"varrho", U'ϱ', Class::Ord},
    {"sigma", U'σ', Class::Ord},
    {"varsigma", U'ς', Class::Ord},
    {"tau", U'τ', Class::Ord},
    {"upsilon", U'υ', Class::Ord},
    {"phi", U'ϕ', Class::Ord},
    {"varphi", U'φ', Class::Ord},
    {"chi", U'χ', Class::Ord},
    {"psi", U'ψ', Class::Ord},
    {"omega", U'ω', Class::Ord},
    // --- Greek uppercase (upright) ---
    {"Gamma", U'Γ', Class::Ord},
    {"Delta", U'Δ', Class::Ord},
    {"Theta", U'Θ', Class::Ord},
    {"Lambda", U'Λ', Class::Ord},
    {"Xi", U'Ξ', Class::Ord},
    {"Pi", U'Π', Class::Ord},
    {"Sigma", U'Σ', Class::Ord},
    {"Upsilon", U'Υ', Class::Ord},
    {"Phi", U'Φ', Class::Ord},
    {"Psi", U'Ψ', Class::Ord},
    {"Omega", U'Ω', Class::Ord},

    // --- Binary operators ---
    {"pm", U'±', Class::Bin},
    {"mp", U'∓', Class::Bin},
    {"times", U'×', Class::Bin},
    {"div", U'÷', Class::Bin},
    {"cdot", U'⋅', Class::Bin},
    {"ast", U'∗', Class::Bin},
    {"star", U'⋆', Class::Bin},
    {"circ", U'∘', Class::Bin},
    {"bullet", U'∙', Class::Bin},
    {"oplus", U'⊕', Class::Bin},
    {"ominus", U'⊖', Class::Bin},
    {"otimes", U'⊗', Class::Bin},
    {"oslash", U'⊘', Class::Bin},
    {"odot", U'⊙', Class::Bin},
    {"cup", U'∪', Class::Bin},
    {"cap", U'∩', Class::Bin},
    {"uplus", U'⊎', Class::Bin},
    {"sqcup", U'⊔', Class::Bin},
    {"sqcap", U'⊓', Class::Bin},
    {"vee", U'∨', Class::Bin},
    {"lor", U'∨', Class::Bin},
    {"wedge", U'∧', Class::Bin},
    {"land", U'∧', Class::Bin},
    {"setminus", U'∖', Class::Bin},
    {"smallsetminus", U'∖', Class::Bin},
    {"wr", U'≀', Class::Bin},
    {"dagger", U'†', Class::Bin},
    {"ddagger", U'‡', Class::Bin},
    {"diamond", U'⋄', Class::Bin},
    {"triangleleft", U'◁', Class::Bin},
    {"triangleright", U'▷', Class::Bin},
    {"bigtriangleup", U'△', Class::Bin},
    {"bigtriangledown", U'▽', Class::Bin},
    {"boxplus", U'⊞', Class::Bin},
    {"boxminus", U'⊟', Class::Bin},
    {"boxtimes", U'⊠', Class::Bin},
    {"ltimes", U'⋉', Class::Bin},
    {"rtimes", U'⋊', Class::Bin},
    {"amalg", U'⨿', Class::Bin},

    // --- Relations ---
    {"leq", U'≤', Class::Rel},
    {"le", U'≤', Class::Rel},
    {"geq", U'≥', Class::Rel},
    {"ge", U'≥', Class::Rel},
    {"neq", U'≠', Class::Rel},
    {"ne", U'≠', Class::Rel},
    {"equiv", U'≡', Class::Rel},
    {"approx", U'≈', Class::Rel},
    {"sim", U'∼', Class::Rel},
    {"simeq", U'≃', Class::Rel},
    {"cong", U'≅', Class::Rel},
    {"propto", U'∝', Class::Rel},
    {"ll", U'≪', Class::Rel},
    {"gg", U'≫', Class::Rel},
    {"subset", U'⊂', Class::Rel},
    {"supset", U'⊃', Class::Rel},
    {"subseteq", U'⊆', Class::Rel},
    {"supseteq", U'⊇', Class::Rel},
    {"subsetneq", U'⊊', Class::Rel},
    {"supsetneq", U'⊋', Class::Rel},
    {"nsubseteq", U'⊈', Class::Rel},
    {"in", U'∈', Class::Rel},
    {"ni", U'∋', Class::Rel},
    {"owns", U'∋', Class::Rel},
    {"notin", U'∉', Class::Rel},
    {"sqsubset", U'⊏', Class::Rel},
    {"sqsupset", U'⊐', Class::Rel},
    {"sqsubseteq", U'⊑', Class::Rel},
    {"sqsupseteq", U'⊒', Class::Rel},
    {"perp", U'⊥', Class::Rel},
    {"parallel", U'∥', Class::Rel},
    {"nparallel", U'∦', Class::Rel},
    {"mid", U'∣', Class::Rel},
    {"nmid", U'∤', Class::Rel},
    {"vdash", U'⊢', Class::Rel},
    {"dashv", U'⊣', Class::Rel},
    {"models", U'⊨', Class::Rel},
    {"asymp", U'≍', Class::Rel},
    {"doteq", U'≐', Class::Rel},
    {"prec", U'≺', Class::Rel},
    {"succ", U'≻', Class::Rel},
    {"preceq", U'⪯', Class::Rel},
    {"succeq", U'⪰', Class::Rel},
    {"lesssim", U'≲', Class::Rel},
    {"gtrsim", U'≳', Class::Rel},
    {"triangleq", U'≜', Class::Rel},
    {"coloneqq", U'≔', Class::Rel},
    {"bowtie", U'⋈', Class::Rel},
    {"smile", U'⌣', Class::Rel},
    {"frown", U'⌢', Class::Rel},
    // arrows
    {"to", U'→', Class::Rel},
    {"rightarrow", U'→', Class::Rel},
    {"leftarrow", U'←', Class::Rel},
    {"gets", U'←', Class::Rel},
    {"leftrightarrow", U'↔', Class::Rel},
    {"Rightarrow", U'⇒', Class::Rel},
    {"Leftarrow", U'⇐', Class::Rel},
    {"Leftrightarrow", U'⇔', Class::Rel},
    {"implies", U'⟹', Class::Rel},
    {"Longrightarrow", U'⟹', Class::Rel},
    {"impliedby", U'⟸', Class::Rel},
    {"Longleftarrow", U'⟸', Class::Rel},
    {"iff", U'⟺', Class::Rel},
    {"Longleftrightarrow", U'⟺', Class::Rel},
    {"mapsto", U'↦', Class::Rel},
    {"longmapsto", U'⟼', Class::Rel},
    {"longrightarrow", U'⟶', Class::Rel},
    {"longleftarrow", U'⟵', Class::Rel},
    {"longleftrightarrow", U'⟷', Class::Rel},
    {"uparrow", U'↑', Class::Rel},
    {"downarrow", U'↓', Class::Rel},
    {"updownarrow", U'↕', Class::Rel},
    {"Uparrow", U'⇑', Class::Rel},
    {"Downarrow", U'⇓', Class::Rel},
    {"Updownarrow", U'⇕', Class::Rel},
    {"nearrow", U'↗', Class::Rel},
    {"searrow", U'↘', Class::Rel},
    {"swarrow", U'↙', Class::Rel},
    {"nwarrow", U'↖', Class::Rel},
    {"hookrightarrow", U'↪', Class::Rel},
    {"hookleftarrow", U'↩', Class::Rel},
    {"rightharpoonup", U'⇀', Class::Rel},
    {"rightharpoondown", U'⇁', Class::Rel},
    {"leftharpoonup", U'↼', Class::Rel},
    {"leftharpoondown", U'↽', Class::Rel},
    {"rightleftharpoons", U'⇌', Class::Rel},
    {"leadsto", U'⇝', Class::Rel},
    {"rightsquigarrow", U'⇝', Class::Rel},
    {"twoheadrightarrow", U'↠', Class::Rel},
    {"twoheadleftarrow", U'↞', Class::Rel},
    {"nrightarrow", U'↛', Class::Rel},
    {"nleftarrow", U'↚', Class::Rel},

    // --- Ordinary symbols ---
    {"infty", U'∞', Class::Ord},
    {"partial", U'∂', Class::Ord},
    {"nabla", U'∇', Class::Ord},
    {"forall", U'∀', Class::Ord},
    {"exists", U'∃', Class::Ord},
    {"nexists", U'∄', Class::Ord},
    {"emptyset", U'∅', Class::Ord},
    {"varnothing", U'∅', Class::Ord},
    {"neg", U'¬', Class::Ord},
    {"lnot", U'¬', Class::Ord},
    {"top", U'⊤', Class::Ord},
    {"bot", U'⊥', Class::Ord},
    {"angle", U'∠', Class::Ord},
    {"measuredangle", U'∡', Class::Ord},
    {"triangle", U'△', Class::Ord},
    {"ell", U'ℓ', Class::Ord},
    {"hbar", U'ℏ', Class::Ord},
    {"hslash", U'ℏ', Class::Ord},
    {"Re", U'ℜ', Class::Ord},
    {"Im", U'ℑ', Class::Ord},
    {"aleph", U'ℵ', Class::Ord},
    {"beth", U'ℶ', Class::Ord},
    {"gimel", U'ℷ', Class::Ord},
    {"wp", U'℘', Class::Ord},
    {"imath", U'ı', Class::Ord},
    {"jmath", U'ȷ', Class::Ord},
    {"prime", U'′', Class::Ord},
    {"backprime", U'‵', Class::Ord},
    {"surd", U'√', Class::Ord},
    {"Box", U'□', Class::Ord},
    {"square", U'□', Class::Ord},
    {"blacksquare", U'■', Class::Ord},
    {"lozenge", U'◊', Class::Ord},
    {"spadesuit", U'♠', Class::Ord},
    {"heartsuit", U'♡', Class::Ord},
    {"diamondsuit", U'♢', Class::Ord},
    {"clubsuit", U'♣', Class::Ord},
    {"flat", U'♭', Class::Ord},
    {"natural", U'♮', Class::Ord},
    {"sharp", U'♯', Class::Ord},
    {"checkmark", U'✓', Class::Ord},
    {"degree", U'°', Class::Ord},
    {"complement", U'∁', Class::Ord},
    {"eth", U'ð', Class::Ord},
    {"mho", U'℧', Class::Ord},
    {"S", U'§', Class::Ord},
    {"P", U'¶', Class::Ord},
    {"copyright", U'©', Class::Ord},
    {"pounds", U'£', Class::Ord},
    {"yen", U'¥', Class::Ord},
    {"euro", U'€', Class::Ord},
    {"ldots", U'…', Class::Inner},
    {"dots", U'…', Class::Inner},
    {"dotsc", U'…', Class::Inner},
    {"dotso", U'…', Class::Inner},
    {"mathellipsis", U'…', Class::Inner},
    {"cdots", U'⋯', Class::Inner},
    {"dotsb", U'⋯', Class::Inner},
    {"dotsm", U'⋯', Class::Inner},
    {"dotsi", U'⋯', Class::Inner},
    {"cdotp", U'⋯', Class::Inner},
    {"vdots", U'⋮', Class::Ord},
    {"ddots", U'⋱', Class::Ord},
    {"iddots", U'⋰', Class::Ord},
    {"colon", U':', Class::Punct},
    {"ldotp", U'.', Class::Punct},
    {"textbackslash", U'\\', Class::Ord},
    {"backslash", U'\\', Class::Ord},
    {"lbrace", U'{', Class::Open},
    {"rbrace", U'}', Class::Close},
    {"langle", U'⟨', Class::Open},
    {"rangle", U'⟩', Class::Close},
    {"lfloor", U'⌊', Class::Open},
    {"rfloor", U'⌋', Class::Close},
    {"lceil", U'⌈', Class::Open},
    {"rceil", U'⌉', Class::Close},
    {"lvert", U'|', Class::Ord},
    {"vert", U'|', Class::Ord},
    {"rvert", U'|', Class::Ord},
    {"lVert", U'‖', Class::Ord},
    {"rVert", U'‖', Class::Ord},
    {"Vert", U'‖', Class::Ord},
    {"lbrack", U'[', Class::Open},
    {"rbrack", U']', Class::Close},
    {"llbracket", U'⟦', Class::Open},
    {"rrbracket", U'⟧', Class::Close},
    {"lgroup", U'⟮', Class::Open},
    {"rgroup", U'⟯', Class::Close},
    {"lmoustache", U'⎰', Class::Open},
    {"rmoustache", U'⎱', Class::Close},
    {"{", U'{', Class::Open},
    {"}", U'}', Class::Close},
    {"|", U'‖', Class::Ord},
    {"$", U'$', Class::Ord},
    {"%", U'%', Class::Ord},
    {"&", U'&', Class::Ord},
    {"#", U'#', Class::Ord},
    {"_", U'_', Class::Ord},
};

struct BigOpEntry {
    std::string_view name;
    char32_t ch;
    bool limits;
};

constexpr BigOpEntry kBigOps[] = {
    {"sum", U'∑', true},       {"prod", U'∏', true},      {"coprod", U'∐', true},
    {"int", U'∫', false},      {"iint", U'∬', false},     {"iiint", U'∭', false},
    {"iiiint", U'⨌', false},   {"oint", U'∮', false},     {"oiint", U'∯', false},
    {"oiiint", U'∰', false},   {"intop", U'∫', true},     {"bigcup", U'⋃', true},
    {"bigcap", U'⋂', true},    {"bigvee", U'⋁', true},    {"bigwedge", U'⋀', true},
    {"bigoplus", U'⨁', true},  {"bigotimes", U'⨂', true}, {"bigodot", U'⨀', true},
    {"biguplus", U'⨄', true},  {"bigsqcup", U'⨆', true},
};

struct NamedOpEntry {
    std::string_view name;
    std::u32string_view text;
    bool limits;
};

constexpr NamedOpEntry kNamedOps[] = {
    {"sin", U"sin", false},       {"cos", U"cos", false},       {"tan", U"tan", false},
    {"cot", U"cot", false},       {"sec", U"sec", false},       {"csc", U"csc", false},
    {"sinh", U"sinh", false},     {"cosh", U"cosh", false},     {"tanh", U"tanh", false},
    {"coth", U"coth", false},     {"arcsin", U"arcsin", false}, {"arccos", U"arccos", false},
    {"arctan", U"arctan", false}, {"arg", U"arg", false},       {"deg", U"deg", false},
    {"dim", U"dim", false},       {"exp", U"exp", false},       {"hom", U"hom", false},
    {"ker", U"ker", false},       {"lg", U"lg", false},         {"ln", U"ln", false},
    {"log", U"log", false},       {"det", U"det", true},        {"gcd", U"gcd", true},
    {"inf", U"inf", true},        {"lim", U"lim", true},        {"liminf", U"lim inf", true},
    {"limsup", U"lim sup", true}, {"max", U"max", true},        {"min", U"min", true},
    {"Pr", U"Pr", true},          {"sup", U"sup", true},        {"injlim", U"inj lim", true},
    {"projlim", U"proj lim", true}, {"mod", U"mod", false},     {"bmod", U"mod", false},
    {"Tr", U"Tr", false},         {"tr", U"tr", false},         {"rank", U"rank", false},
    {"Re", U"Re", false},         {"Im", U"Im", false},
};

// Build a name -> entry hash map from one of the tables above, once.
template <class Entry, size_t N>
const Entry* find_entry(const Entry (&table)[N], std::string_view name) {
    static const auto index = [&table] {
        std::unordered_map<std::string_view, const Entry*> m;
        for (const Entry& e : table) m.emplace(e.name, &e);
        return m;
    }();
    auto it = index.find(name);
    return it == index.end() ? nullptr : it->second;
}

bool is_ascii_upper(char32_t c) { return c >= U'A' && c <= U'Z'; }
bool is_ascii_lower(char32_t c) { return c >= U'a' && c <= U'z'; }
bool is_ascii_digit(char32_t c) { return c >= U'0' && c <= U'9'; }

// Lowercase Greek alphabets in the math block all share one layout:
// 25 letters starting at alpha, then the partial-derivative symbol, then the
// six "variant" forms. Returns 0 when `c` has no slot.
char32_t greek_block(char32_t c, char32_t alpha) {
    auto variant = [alpha](char32_t i) { return alpha + 26 + i; };
    if (c >= U'α' && c <= U'ω') return alpha + (c - U'α');
    switch (c) {
        case U'ϵ': return variant(0);  // epsilon symbol
        case U'ϑ': return variant(1);
        case U'ϰ': return variant(2);
        case U'ϕ': return variant(3);
        case U'ϱ': return variant(4);
        case U'ϖ': return variant(5);
        default: return 0;
    }
}

}  // namespace

std::optional<Sym> lookup(std::string_view name) {
    if (const SymEntry* e = find_entry(kSymbols, name)) return Sym{e->ch, e->cls};
    return std::nullopt;
}

std::optional<BigOp> big_operator(std::string_view name) {
    if (const BigOpEntry* e = find_entry(kBigOps, name)) return BigOp{e->ch, e->limits};
    return std::nullopt;
}

std::optional<NamedOp> named_operator(std::string_view name) {
    if (const NamedOpEntry* e = find_entry(kNamedOps, name)) return NamedOp{e->text, e->limits};
    return std::nullopt;
}

float spacing_mu(Class left, Class right, bool script) {
    using C = Class;
    constexpr float THIN = 3.0f;
    constexpr float MED = 4.0f;
    constexpr float THICK = 5.0f;
    // In script styles only the "non-parenthesised" entries of the table survive.
    auto thin = [script](bool always) { return always || !script ? THIN : 0.0f; };
    const float med = script ? 0.0f : MED;
    const float thick = script ? 0.0f : THICK;
    switch (left) {
        case C::Ord:
            switch (right) {
                case C::Op: return thin(true);
                case C::Bin: return med;
                case C::Rel: return thick;
                case C::Inner: return thin(false);
                default: return 0.0f;
            }
        case C::Op:
            switch (right) {
                case C::Ord:
                case C::Op: return thin(true);
                case C::Rel: return thick;
                case C::Inner: return thin(false);
                default: return 0.0f;
            }
        case C::Bin:
            switch (right) {
                case C::Ord:
                case C::Op:
                case C::Open:
                case C::Inner: return med;
                default: return 0.0f;
            }
        case C::Rel:
            switch (right) {
                case C::Ord:
                case C::Op:
                case C::Open:
                case C::Inner: return thick;
                default: return 0.0f;
            }
        case C::Open:
            return 0.0f;
        case C::Close:
            switch (right) {
                case C::Op: return thin(true);
                case C::Bin: return med;
                case C::Rel: return thick;
                case C::Inner: return thin(false);
                default: return 0.0f;
            }
        case C::Punct:
            switch (right) {
                case C::Ord:
                case C::Op:
                case C::Rel:
                case C::Open:
                case C::Close:
                case C::Punct:
                case C::Inner: return thin(false);
                default: return 0.0f;
            }
        case C::Inner:
            switch (right) {
                case C::Ord:
                case C::Op:
                case C::Open:
                case C::Punct:
                case C::Inner: return thin(false);
                case C::Bin: return med;
                case C::Rel: return thick;
                default: return 0.0f;  // Inner, Close
            }
    }
    return 0.0f;
}

// ---------------------------------------------------------------------------
// Math alphabets
// ---------------------------------------------------------------------------

char32_t style_char(char32_t c, FontStyle style) {
    using F = FontStyle;
    const char32_t u = c;
    const bool is_upper = is_ascii_upper(c);
    const bool is_lower = is_ascii_lower(c);
    const bool is_digit = is_ascii_digit(c);
    const bool is_greek_lower = (c >= U'α' && c <= U'ω') || c == U'ϑ' || c == U'ϰ' || c == U'ϕ' ||
                                c == U'ϱ' || c == U'ϖ' || c == U'ϵ';
    const bool is_greek_upper = c >= U'Α' && c <= U'Ω' && c != U'΢';
    // Latin letters: capital and small alphabets start at these code points.
    auto latin_i = [&](char32_t base_u, char32_t base_l) -> char32_t {
        if (is_upper) return base_u + (u - U'A');
        if (is_lower) return base_l + (u - U'a');
        return 0;
    };
    char32_t mapped = 0;  // 0 = "no mapping, keep c"
    switch (style) {
        case F::Roman:
            break;
        case F::Default:
        case F::Italic:
            if (c == U'h') {
                mapped = U'ℎ';  // the italic h lives outside the math block
            } else if (is_upper || is_lower) {
                mapped = latin_i(0x1D434, 0x1D44E);
            } else if (is_greek_lower) {
                mapped = greek_block(c, 0x1D6FC);
            } else if (style == F::Italic && is_greek_upper) {
                mapped = 0x1D6E2 + (u - U'Α');
            }
            break;
        case F::Bold:
            if (is_upper || is_lower) {
                mapped = latin_i(0x1D400, 0x1D41A);
            } else if (is_digit) {
                mapped = 0x1D7CE + (u - U'0');
            } else if (is_greek_upper) {
                mapped = 0x1D6A8 + (u - U'Α');
            } else if (is_greek_lower) {
                mapped = greek_block(c, 0x1D6C2);
            }
            break;
        case F::BoldItalic:
            if (is_upper || is_lower) {
                mapped = latin_i(0x1D468, 0x1D482);
            } else if (is_greek_upper) {
                mapped = 0x1D71C + (u - U'Α');
            } else if (is_greek_lower) {
                mapped = greek_block(c, 0x1D736);
            } else if (is_digit) {
                mapped = 0x1D7CE + (u - U'0');
            }
            break;
        case F::Blackboard:
            if (is_upper) {
                switch (c) {
                    case U'C': mapped = U'ℂ'; break;
                    case U'H': mapped = U'ℍ'; break;
                    case U'N': mapped = U'ℕ'; break;
                    case U'P': mapped = U'ℙ'; break;
                    case U'Q': mapped = U'ℚ'; break;
                    case U'R': mapped = U'ℝ'; break;
                    case U'Z': mapped = U'ℤ'; break;
                    default: mapped = 0x1D538 + (u - U'A'); break;
                }
            } else if (is_lower) {
                mapped = 0x1D552 + (u - U'a');
            } else if (is_digit) {
                mapped = 0x1D7D8 + (u - U'0');
            }
            break;
        case F::Script:
            if (is_upper) {
                switch (c) {
                    case U'B': mapped = U'ℬ'; break;
                    case U'E': mapped = U'ℰ'; break;
                    case U'F': mapped = U'ℱ'; break;
                    case U'H': mapped = U'ℋ'; break;
                    case U'I': mapped = U'ℐ'; break;
                    case U'L': mapped = U'ℒ'; break;
                    case U'M': mapped = U'ℳ'; break;
                    case U'R': mapped = U'ℛ'; break;
                    default: mapped = 0x1D49C + (u - U'A'); break;
                }
            } else if (is_lower) {
                switch (c) {
                    case U'e': mapped = U'ℯ'; break;
                    case U'g': mapped = U'ℊ'; break;
                    case U'o': mapped = U'ℴ'; break;
                    default: mapped = 0x1D4B6 + (u - U'a'); break;
                }
            }
            break;
        case F::Fraktur:
            if (is_upper) {
                switch (c) {
                    case U'C': mapped = U'ℭ'; break;
                    case U'H': mapped = U'ℌ'; break;
                    case U'I': mapped = U'ℑ'; break;
                    case U'R': mapped = U'ℜ'; break;
                    case U'Z': mapped = U'ℨ'; break;
                    default: mapped = 0x1D504 + (u - U'A'); break;
                }
            } else if (is_lower) {
                mapped = 0x1D51E + (u - U'a');
            }
            break;
        case F::Sans:
            if (is_upper || is_lower) {
                mapped = latin_i(0x1D5A0, 0x1D5BA);
            } else if (is_digit) {
                mapped = 0x1D7E2 + (u - U'0');
            }
            break;
        case F::Mono:
            if (is_upper || is_lower) {
                mapped = latin_i(0x1D670, 0x1D68A);
            } else if (is_digit) {
                mapped = 0x1D7F6 + (u - U'0');
            }
            break;
    }
    return mapped != 0 ? mapped : c;
}

}  // namespace texmath
