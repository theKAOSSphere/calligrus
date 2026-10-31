#include "layout.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace texmath {

namespace {

// Rust's f32::max / f32::min: if one side is NaN, the other one wins.
float max_f(float a, float b) { return std::fmax(a, b); }
float min_f(float a, float b) { return std::fmin(a, b); }

// Shift every item of `items` by (dx, dy).
Item shifted(const Item& it, float dx, float dy) {
    if (auto* g = std::get_if<GlyphItem>(&it)) return GlyphItem{g->gid, g->x + dx, g->y + dy, g->sx, g->sy};
    if (auto* r = std::get_if<RuleItem>(&it)) return RuleItem{r->x + dx, r->y + dy, r->w, r->h};
    const auto& l = std::get<LineItem>(it);
    return LineItem{l.x1 + dx, l.y1 + dy, l.x2 + dx, l.y2 + dy, l.t};
}

MBox items_box(std::vector<Item> items) {
    MBox b;
    b.items = std::move(items);
    return b;
}

}  // namespace

void MBox::place(const MBox& other, float dx, float dy) {
    for (const Item& it : other.items) items.push_back(shifted(it, dx, dy));
    h = max_f(h, other.h + dy);
    d = max_f(d, other.d - dy);
}

// A math alphanumeric (italic x, bold A, double-struck R...) as the plain letter, digit or Greek letter it is a
// style of, so that copying and searching a PDF's formulas finds "x", not "U+1D465" (as layout.rs's plain_letter).
static char32_t plain_letter(char32_t c) {
    if (c == 0x210E) return U'h';
    if (c >= 0x1D400 && c <= 0x1D6A3) {  // Latin: 13 styles of 52 letters (A-Z, a-z)
        const char32_t k = (c - 0x1D400) % 52;
        return k < 26 ? 0x41 + k : 0x61 + k - 26;
    }
    if (c >= 0x1D6A8 && c <= 0x1D7C9) {  // Greek: 5 styles of 58 (capitals, nabla, small letters, partial, variants): letters only
        const char32_t k = (c - 0x1D6A8) % 58;
        if (k == 17) return 0x3F4;
        if (k <= 24) return 0x391 + k;
        if (k >= 26 && k <= 50) return 0x3B1 + k - 26;
        return c;
    }
    if (c >= 0x1D7CE && c <= 0x1D7FF) return 0x30 + (c - 0x1D7CE) % 10;  // digits: 5 styles of 10
    return c;
}

std::optional<char32_t> Layouter::text_of(uint16_t gid) const {
    auto it = text_of_.find(gid);
    if (it == text_of_.end()) return std::nullopt;
    return plain_letter(it->second);
}

Layouter::Layouter(Font font) : font_(std::move(font)) {
    // (controls and private use are not text; the font has nothing above U+1FFFF)
    for (char32_t cp = 0x20; cp <= 0x1FFFF; cp++) {
        if ((cp >= 0x7F && cp <= 0x9F) || (cp >= 0xD800 && cp <= 0xDFFF) || (cp >= 0xE000 && cp <= 0xF8FF)) continue;
        if (auto g = font_.glyph_index(cp)) text_of_.emplace(*g, cp);  // (ascending: the lowest code point stays)
    }
    upem_ = static_cast<float>(font_.units_per_em());
    const MathConstants& c = font_.math_constants();
    script_scale_ = static_cast<float>(c.script_percent_scale_down) / 100.0f;
    script_script_scale_ = static_cast<float>(c.script_script_percent_scale_down) / 100.0f;
}

MBox Layouter::layout(const Nodes& nodes, bool display) const {
    Ctx ctx{display ? Style::Display : Style::Text, false, FontStyle::Default};
    return row(nodes, ctx);
}

// ----- font access helpers ----------------------------------------------

// A MATH-table constant in em, multiplied by the style's scale.
float Layouter::k(int16_t value, Ctx ctx) const { return static_cast<float>(value) / upem_ * scale(ctx.style); }

float Layouter::scale(Style style) const {
    switch (style) {
        case Style::Display:
        case Style::Text: return 1.0f;
        case Style::Script: return script_scale_;
        case Style::ScriptScript: return script_script_scale_;
    }
    return 1.0f;
}

float Layouter::axis(Ctx ctx) const { return k(font_.math_constants().axis_height, ctx); }

bool Layouter::is_script(Style style) { return style == Style::Script || style == Style::ScriptScript; }

Style Layouter::sup_style(Style style) {
    return (style == Style::Display || style == Style::Text) ? Style::Script : Style::ScriptScript;
}

Style Layouter::num_style(Style style) {
    switch (style) {
        case Style::Display: return Style::Text;
        case Style::Text: return Style::Script;
        default: return Style::ScriptScript;
    }
}

// (xmin, xmax, ymin, ymax) in em at scale 1
Layouter::Extents Layouter::glyph_extents(uint16_t gid) const {
    if (auto b = font_.glyph_bounding_box(gid)) {
        return {static_cast<float>(b->x_min) / upem_, static_cast<float>(b->x_max) / upem_,
                static_cast<float>(b->y_min) / upem_, static_cast<float>(b->y_max) / upem_};
    }
    return {0.0f, 0.0f, 0.0f, 0.0f};
}

// One glyph as a box. Returns an invisible placeholder if the font lacks it.
MBox Layouter::glyph(char32_t ch, Ctx ctx) const {
    const float s = scale(ctx.style);
    auto gid = font_.glyph_index(ch);
    if (!gid) {
        MBox b;
        b.w = 0.5f * s;
        b.h = 0.7f * s;
        return b;
    }
    return glyph_id(*gid, s);
}

MBox Layouter::glyph_id(uint16_t gid, float s) const {
    MBox b;
    b.w = static_cast<float>(font_.glyph_hor_advance(gid).value_or(0)) / upem_ * s;
    const Extents e = glyph_extents(gid);
    b.h = max_f(e.ymax, 0.0f) * s;
    b.d = max_f(-e.ymin, 0.0f) * s;
    b.items.push_back(GlyphItem{gid, 0.0f, 0.0f, s, s});
    if (auto v = font_.italic_correction(gid)) b.ic = static_cast<float>(*v) / upem_ * s;
    if (auto v = font_.top_accent_attachment(gid)) b.accent_x = static_cast<float>(*v) / upem_ * s;
    b.is_char = true;
    b.kern_gid = gid;
    b.kern_dy = 0.0f;
    return b;
}

char32_t Layouter::sym_char(char32_t ch, bool italic, Ctx ctx) const {
    const FontStyle style = (ctx.font == FontStyle::Default && !italic) ? FontStyle::Roman : ctx.font;
    const char32_t mapped = style_char(ch, style);
    return font_.glyph_index(mapped) ? mapped : ch;
}

// ----- rows ---------------------------------------------------------------

Class Layouter::class_of(const Node& n) const {
    if (auto* s = std::get_if<node::Sym>(&n.v)) return s->cls;
    if (auto* sc = std::get_if<node::Script>(&n.v)) return class_of(*sc->base);
    if (std::holds_alternative<node::Op>(n.v)) return Class::Op;
    if (std::holds_alternative<node::Fenced>(n.v)) return Class::Inner;
    if (auto* b = std::get_if<node::Big>(&n.v)) return b->cls;
    if (auto* c = std::get_if<node::Classed>(&n.v)) return c->cls;
    if (std::holds_alternative<node::XArrow>(n.v)) return Class::Rel;
    // like \left...\right, a delimited matrix is an Inner atom
    if (auto* m = std::get_if<node::Matrix>(&n.v); m && (m->left || m->right)) return Class::Inner;
    return Class::Ord;
}

MBox Layouter::row(const Nodes& nodes, Ctx ctx) const {
    const size_t n = nodes.size();
    std::vector<Class> cls(n);
    for (size_t i = 0; i < n; i++) cls[i] = class_of(nodes[i]);

    // Binary operators that don't sit between two operands become ordinary
    // (TeXbook Appendix G, rules 5 and 6).
    std::vector<size_t> sig;
    for (size_t i = 0; i < n; i++) {
        const auto& v = nodes[i].v;
        if (!std::holds_alternative<node::Space>(v) && !std::holds_alternative<node::Empty>(v) &&
            !std::holds_alternative<node::StyleSwitch>(v)) {
            sig.push_back(i);
        }
    }
    for (size_t k = 0; k < sig.size(); k++) {
        const size_t i = sig[k];
        if (cls[i] != Class::Bin) continue;
        bool prev_bad = k == 0;
        if (!prev_bad) {
            const Class p = cls[sig[k - 1]];
            prev_bad = p == Class::Bin || p == Class::Op || p == Class::Rel || p == Class::Open || p == Class::Punct;
        }
        bool next_bad = k + 1 >= sig.size();
        if (!next_bad) {
            const Class q = cls[sig[k + 1]];
            next_bad = q == Class::Rel || q == Class::Close || q == Class::Punct;
        }
        if (prev_bad || next_bad) cls[i] = Class::Ord;
    }

    MBox out;
    float x = 0.0f;
    std::optional<Class> prev;
    float last_ic = 0.0f;
    std::optional<float> last_accent;
    int count = 0;
    bool single_char = false;
    std::optional<uint16_t> last_kern_gid;
    float last_kern_dy = 0.0f;

    for (size_t i = 0; i < n; i++) {
        const Node& nd = nodes[i];
        if (auto* sw = std::get_if<node::StyleSwitch>(&nd.v)) {
            ctx.style = sw->style;
            continue;
        }
        if (auto* sp = std::get_if<node::Space>(&nd.v)) {
            x += sp->em * scale(ctx.style);
            continue;
        }
        if (std::holds_alternative<node::Empty>(nd.v)) continue;

        const MBox b = node(nd, cls[i], ctx);
        // Inter-atom glue from the TeXbook's spacing table (p.170).
        if (prev) x += spacing_mu(*prev, cls[i], is_script(ctx.style)) / 18.0f * scale(ctx.style);
        out.place(b, x, 0.0f);
        x += b.w;
        last_ic = b.ic;
        last_accent = b.accent_x ? std::optional<float>(*b.accent_x + (x - b.w)) : std::nullopt;
        single_char = b.is_char;
        last_kern_gid = b.kern_gid;
        last_kern_dy = b.kern_dy;
        prev = cls[i];
        count += 1;
    }
    out.w = x;
    if (count == 1) {
        out.ic = last_ic;
        out.is_char = single_char;
        // accent attach point only meaningful for a single glyph
        out.accent_x = last_accent;
        out.kern_gid = last_kern_gid;
        out.kern_dy = last_kern_dy;
    }
    return out;
}

MBox Layouter::node(const Node& n, Class cls, Ctx ctx) const {
    const float s = scale(ctx.style);
    const auto& v = n.v;
    if (std::holds_alternative<node::Empty>(v) || std::holds_alternative<node::StyleSwitch>(v)) return MBox{};
    if (auto* sp = std::get_if<node::Space>(&v)) return MBox::space(sp->em * s);
    if (auto* sym = std::get_if<node::Sym>(&v)) {
        const char32_t c = sym_char(sym->ch, sym->italic, ctx);
        MBox b = glyph(c, ctx);
        // Delimiters don't take an italic correction.
        if (cls == Class::Open || cls == Class::Close) b.ic = 0.0f;
        return b;
    }
    if (auto* g = std::get_if<node::Group>(&v)) return row(g->body, ctx);
    if (auto* c = std::get_if<node::Classed>(&v)) return row(c->body, ctx);
    if (auto* f = std::get_if<node::Font>(&v)) return row(f->body, Ctx{ctx.style, ctx.cramped, f->style});
    if (auto* t = std::get_if<node::Text>(&v)) return text(t->s, t->style, ctx);
    if (auto* f = std::get_if<node::Frac>(&v)) return frac(*f, ctx);
    if (auto* sq = std::get_if<node::Sqrt>(&v)) return sqrt(*sq, ctx);
    if (auto* sc = std::get_if<node::Script>(&v)) return script(*sc, cls, ctx);
    if (auto* o = std::get_if<node::Op>(&v)) return op(*o, ctx);
    if (auto* fe = std::get_if<node::Fenced>(&v)) return fenced(*fe, ctx);
    if (auto* big = std::get_if<node::Big>(&v)) {
        static constexpr float kSizes[4] = {1.2f, 1.8f, 2.4f, 3.0f};
        const int level = std::clamp<int>(big->level, 1, 4);
        return delim(big->ch, kSizes[level - 1] * s, ctx);
    }
    if (auto* a = std::get_if<node::Accent>(&v)) return accent(*a, ctx);
    if (auto* o = std::get_if<node::Over>(&v)) return over(*o, ctx);
    if (auto* m = std::get_if<node::Matrix>(&v)) return matrix(*m, ctx);
    if (auto* xa = std::get_if<node::XArrow>(&v)) return xarrow(*xa, ctx);
    if (auto* bx = std::get_if<node::Boxed>(&v)) return boxed(bx->body, ctx);
    if (auto* ca = std::get_if<node::Cancel>(&v)) return cancel(*ca, ctx);
    if (auto* ph = std::get_if<node::Phantom>(&v)) {
        // Takes up the space of its body but draws nothing.
        const MBox b = row(ph->body, ctx);
        MBox out;
        out.w = ph->horizontal ? b.w : 0.0f;
        out.h = ph->vertical ? b.h : 0.0f;
        out.d = ph->vertical ? b.d : 0.0f;
        return out;
    }
    return MBox{};
}

MBox Layouter::text(const std::u32string& str, FontStyle style, Ctx ctx) const {
    const float s = scale(ctx.style);
    MBox out;
    float x = 0.0f;
    for (char32_t c : str) {
        if (c == U' ') {
            x += 0.33f * s;
            continue;
        }
        const char32_t m = style_char(c, style);
        const char32_t ch = font_.glyph_index(m) ? m : c;
        const MBox b = glyph(ch, ctx);
        out.place(b, x, 0.0f);
        x += b.w;
        out.ic = b.ic;
    }
    out.w = x;
    return out;
}

// ----- fractions ------------------------------------------------------------

// TeXbook Appendix G, rule 15 (with OpenType MATH's gap minimums).
MBox Layouter::frac(const node::Frac& fr, Ctx ctx) const {
    const MathConstants& K = font_.math_constants();
    Ctx c = ctx;
    if (fr.force) c.style = *fr.force;
    const bool display = c.style == Style::Display;
    const float s = scale(c.style);
    const MBox n = row(fr.num, Ctx{num_style(c.style), c.cramped, c.font});
    const MBox d = row(fr.den, Ctx{num_style(c.style), true, c.font});
    const float ax = axis(c);

    float up, down, t;
    if (fr.rule) {
        t = k(K.fraction_rule_thickness, c);
        float num_up, den_down, num_gap, den_gap;
        if (display) {
            num_up = k(K.fraction_numerator_display_style_shift_up, c);
            den_down = k(K.fraction_denominator_display_style_shift_down, c);
            num_gap = k(K.fraction_num_display_style_gap_min, c);
            den_gap = k(K.fraction_denom_display_style_gap_min, c);
        } else {
            num_up = k(K.fraction_numerator_shift_up, c);
            den_down = k(K.fraction_denominator_shift_down, c);
            num_gap = k(K.fraction_numerator_gap_min, c);
            den_gap = k(K.fraction_denominator_gap_min, c);
        }
        up = max_f(num_up, ax + t / 2.0f + num_gap + n.d);
        down = max_f(den_down, -ax + t / 2.0f + den_gap + d.h);
    } else {
        // \binom, \atop: a "stack" with no bar (rule 15c).
        t = 0.0f;
        float top_up, bot_down, gap_min;
        if (display) {
            top_up = k(K.stack_top_display_style_shift_up, c);
            bot_down = k(K.stack_bottom_display_style_shift_down, c);
            gap_min = k(K.stack_display_style_gap_min, c);
        } else {
            top_up = k(K.stack_top_shift_up, c);
            bot_down = k(K.stack_bottom_shift_down, c);
            gap_min = k(K.stack_gap_min, c);
        }
        float u = top_up, dn = bot_down;
        const float gap = (u - n.d) - (d.h - dn);
        if (gap < gap_min) {
            const float extra = (gap_min - gap) / 2.0f;
            u += extra;
            dn += extra;
        }
        up = u;
        down = dn;
    }

    const float pad = 0.06f * s;
    const float inner_w = max_f(n.w, d.w);
    MBox out;
    out.place(n, pad + (inner_w - n.w) / 2.0f, up);
    out.place(d, pad + (inner_w - d.w) / 2.0f, -down);
    if (fr.rule) {
        out.items.push_back(RuleItem{pad, ax - t / 2.0f, inner_w, t});
        out.h = max_f(out.h, ax + t / 2.0f);
        out.d = max_f(out.d, -(ax - t / 2.0f));
    }
    out.w = inner_w + 2.0f * pad;

    if (!fr.delims) return out;
    // TeX uses fixed delimiter sizes for \binom: 2.39em in display, 1.01em in text.
    const float size = (display ? 2.39f : 1.01f) * s;
    const MBox lb = delim(fr.delims->first, size, c);
    const MBox rb = delim(fr.delims->second, size, c);
    MBox wrapped;
    wrapped.place(lb, 0.0f, 0.0f);
    wrapped.place(out, lb.w, 0.0f);
    wrapped.place(rb, lb.w + out.w, 0.0f);
    wrapped.w = lb.w + out.w + rb.w;
    return wrapped;
}

// ----- scripts & operators ----------------------------------------------------

bool Layouter::wants_limits(const Node& base, Ctx ctx) const {
    if (auto* o = std::get_if<node::Op>(&base.v)) {
        return o->limits.value_or(o->default_limits && ctx.style == Style::Display);
    }
    if (auto* ov = std::get_if<node::Over>(&base.v)) return ov->mark == Mark::Brace && !ov->label;
    return false;
}

// TeXbook Appendix G, rules 18a-18f: superscript and subscript placement.
MBox Layouter::script(const node::Script& sc, Class cls, Ctx ctx) const {
    const MathConstants& K = font_.math_constants();
    const MBox b = node(*sc.base, cls, ctx);
    if (wants_limits(*sc.base, ctx)) return limits(b, sc.sub, sc.sup, ctx);
    const float s = scale(ctx.style);
    const Ctx sup_ctx{sup_style(ctx.style), ctx.cramped, ctx.font};
    const Ctx sub_ctx{sup_style(ctx.style), true, ctx.font};
    std::optional<MBox> sp, sb;
    if (sc.sup) sp = row(*sc.sup, sup_ctx);
    if (sc.sub) sb = row(*sc.sub, sub_ctx);
    const float sscale = scale(sup_ctx.style);

    const float sup_drop = static_cast<float>(K.superscript_baseline_drop_max) / upem_ * sscale;
    const float sub_drop = static_cast<float>(K.subscript_baseline_drop_min) / upem_ * sscale;
    // Rule 18a: a single character keeps the font's shifts; anything bigger
    // hangs its scripts off its own top and bottom.
    float u = 0.0f, v = 0.0f;
    if (!b.is_char) {
        u = b.h - sup_drop;
        v = b.d + sub_drop;
    }

    MBox out;
    out.place(b, 0.0f, 0.0f);
    float right = b.w;
    float sup_x = b.w + b.ic;
    float sub_x = b.w;

    auto min_up = [&] {
        return ctx.cramped ? k(K.superscript_shift_up_cramped, ctx) : k(K.superscript_shift_up, ctx);
    };
    if (sp && !sb) {
        const MBox& p = *sp;
        u = max_f(max_f(u, min_up()), p.d + k(K.superscript_bottom_min, ctx));
        sup_x += math_kern(b, true, u - p.d, u + p.h, s);
        out.place(p, sup_x, u);
        right = max_f(right, sup_x + p.w);
    } else if (!sp && sb) {
        const MBox& q = *sb;
        v = max_f(max_f(v, k(K.subscript_shift_down, ctx)), q.h - k(K.subscript_top_max, ctx));
        sub_x += math_kern(b, false, -v - q.d, -v + q.h, s);
        out.place(q, sub_x, -v);
        right = max_f(right, sub_x + q.w);
    } else if (sp && sb) {
        const MBox& p = *sp;
        const MBox& q = *sb;
        u = max_f(max_f(u, min_up()), p.d + k(K.superscript_bottom_min, ctx));
        v = max_f(v, k(K.subscript_shift_down, ctx));
        // Rule 18e: keep a minimum gap between the two scripts.
        const float gap_min = k(K.sub_superscript_gap_min, ctx);
        const float gap = (u - p.d) - (q.h - v);
        if (gap < gap_min) {
            v += gap_min - gap;
            const float psi = k(K.superscript_bottom_max_with_subscript, ctx) - (u - p.d);
            if (psi > 0.0f) {
                u += psi;
                v -= psi;
            }
        }
        sup_x += math_kern(b, true, u - p.d, u + p.h, s);
        sub_x += math_kern(b, false, -v - q.d, -v + q.h, s);
        out.place(p, sup_x, u);
        out.place(q, sub_x, -v);
        right = max_f(max_f(right, sup_x + p.w), sub_x + q.w);
    }
    out.w = right + k(K.space_after_script, ctx);
    return out;
}

// Horizontal adjustment (em) for a script whose vertical extent is
// [`lo`, `hi`] (em above the baseline), taken from the base glyph's MATH
// kern table. This is what tucks scripts into the curves of the base,
// e.g. the limits of an integral sign.
float Layouter::math_kern(const MBox& base, bool top_right, float lo, float hi, float s) const {
    if (!base.kern_gid) return 0.0f;
    auto info = font_.kern_info(*base.kern_gid);
    if (!info) return 0.0f;
    const std::optional<MathKern>& table = top_right ? info->top_right : info->bottom_right;
    if (!table) return 0.0f;
    // Correction height -> kern: the table lists heights at which the value changes.
    auto at = [&](float h_em) -> float {
        const float h = (h_em - base.kern_dy) / s * upem_;
        const size_t n = table->heights.size();
        for (size_t i = 0; i < n; i++) {
            if (h < static_cast<float>(table->heights[i])) {
                return i < table->kerns.size() ? static_cast<float>(table->kerns[i]) : 0.0f;
            }
        }
        return n < table->kerns.size() ? static_cast<float>(table->kerns[n]) : 0.0f;
    };
    return min_f(at(lo), at(hi)) / upem_ * s;
}

// Scripts stacked above/below (big operators in display style, `\underbrace`).
// TeXbook Appendix G, rule 13a.
MBox Layouter::limits(MBox b, const std::optional<Nodes>& sub, const std::optional<Nodes>& sup, Ctx ctx) const {
    const MathConstants& K = font_.math_constants();
    const Ctx sup_ctx{sup_style(ctx.style), ctx.cramped, ctx.font};
    const Ctx sub_ctx{sup_style(ctx.style), true, ctx.font};
    std::optional<MBox> up, down;
    if (sup) up = row(*sup, sup_ctx);
    if (sub) down = row(*sub, sub_ctx);
    const float width = max_f(max_f(b.w, up ? up->w : 0.0f), down ? down->w : 0.0f);

    MBox out;
    out.place(b, (width - b.w) / 2.0f, 0.0f);
    if (up) {
        const float shift =
            max_f(k(K.upper_limit_baseline_rise_min, ctx), b.h + k(K.upper_limit_gap_min, ctx) + up->d);
        out.place(*up, (width - up->w) / 2.0f + b.ic / 2.0f, shift);
    }
    if (down) {
        const float shift =
            max_f(k(K.lower_limit_baseline_drop_min, ctx), b.d + k(K.lower_limit_gap_min, ctx) + down->h);
        out.place(*down, (width - down->w) / 2.0f - b.ic / 2.0f, -shift);
    }
    out.w = width;
    return out;
}

MBox Layouter::op(const node::Op& o, Ctx ctx) const {
    if (!o.is_glyph) {
        const Ctx c{ctx.style, ctx.cramped, FontStyle::Roman};
        MBox b = text(o.name, FontStyle::Roman, c);
        b.is_char = false;
        return b;
    }
    const float s = scale(ctx.style);
    MBox b;
    auto gid = font_.glyph_index(o.glyph);
    if (gid && ctx.style == Style::Display) {
        // Use the larger "display" variant.
        const float min = static_cast<float>(font_.math_constants().display_operator_min_height) / upem_ * s;
        const uint16_t g = vertical_variant(*gid, min).value_or(*gid);
        b = glyph_id(g, s);
    } else {
        b = glyph(o.glyph, ctx);
    }
    // TeX centres large operators on the math axis (rule 13).
    const float shift = (b.h - b.d) / 2.0f - axis(ctx);
    if (std::fabs(shift) > 1e-4f) {
        for (Item& it : b.items) {
            if (auto* g = std::get_if<GlyphItem>(&it)) g->y -= shift;
        }
        b.h -= shift;
        b.d += shift;
        b.kern_dy = -shift;
    }
    b.is_char = false;
    return b;
}

// ----- stretchy things ----------------------------------------------------

// Smallest vertical variant of `gid` that is at least `min_em` tall.
std::optional<uint16_t> Layouter::vertical_variant(uint16_t gid, float min_em) const {
    auto cons = font_.vertical_construction(gid);
    if (!cons) return std::nullopt;
    const float min_units = min_em * upem_;
    std::optional<uint16_t> best;
    for (const GlyphVariant& v : cons->variants) {
        best = v.variant_glyph;
        if (static_cast<float>(v.advance_measurement) >= min_units) break;
    }
    return best;
}

// A delimiter stretched to at least `target` em, centred on the math axis:
// first the font's pre-drawn sizes, then a glyph assembly (rule 19).
MBox Layouter::delim(std::optional<char32_t> ch, float target, Ctx ctx) const {
    if (!ch) return MBox::space(0.0f);
    auto gid_opt = font_.glyph_index(*ch);
    if (!gid_opt) return MBox::space(0.0f);
    const uint16_t gid = *gid_opt;
    const float s = scale(ctx.style);
    const float ax = axis(ctx);
    const float target_units = target / s * upem_;

    auto cons = font_.vertical_construction(gid);
    const Extents be = glyph_extents(gid);
    const float base_size = (be.ymax - be.ymin) * upem_;

    uint16_t chosen = gid;
    float stretch_y = 1.0f;
    std::optional<Assembly> assembly;

    if (base_size < target_units) {
        if (cons) {
            bool found = false;
            uint16_t last = gid;
            for (const GlyphVariant& v : cons->variants) {
                last = v.variant_glyph;
                if (static_cast<float>(v.advance_measurement) >= target_units) {
                    chosen = v.variant_glyph;
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (cons->assembly) {
                    const float overlap = static_cast<float>(font_.min_connector_overlap());
                    assembly = assemble(*cons->assembly, overlap, target_units, true, s);
                } else {
                    chosen = last;
                }
            }
        } else if (base_size > 0.0f) {
            // No size variants in the font: stretch the glyph vertically (capped).
            stretch_y = std::clamp(target_units / base_size, 1.0f, 2.2f);
        }
    }

    if (assembly) {
        const float total = assembly->ymax - assembly->ymin;
        const float dy = ax - total / 2.0f - assembly->ymin;
        MBox b;
        b.w = assembly->w;
        b.place(items_box(std::move(assembly->items)), 0.0f, dy);
        b.h = max_f(assembly->ymax + dy, 0.0f);
        b.d = max_f(-(assembly->ymin + dy), 0.0f);
        return b;
    }

    MBox b = glyph_id(chosen, s);
    const Extents e = glyph_extents(chosen);
    const float ymin = e.ymin * s * stretch_y, ymax = e.ymax * s * stretch_y;
    const float dy = ax - (ymin + ymax) / 2.0f;
    if (!b.items.empty()) {
        if (auto* g = std::get_if<GlyphItem>(&b.items.front())) {
            g->y = dy;
            g->sy *= stretch_y;
        }
    }
    b.h = max_f(ymax + dy, 0.0f);
    b.d = max_f(-(ymin + dy), 0.0f);
    b.ic = 0.0f;
    b.is_char = false;
    return b;
}

// Build a glyph assembly of at least `target_units` (font units): repeat the
// extender parts as often as needed, then spread any surplus by overlapping
// the parts a bit more (within the connector lengths).
// Returns items (origin at 0), width, ymin, ymax in em (scaled by `s`)
// for vertical assemblies; for horizontal ones `ymin/ymax` carry the
// glyphs' vertical extents.
Layouter::Assembly Layouter::assemble(const std::vector<GlyphPart>& parts, float min_overlap, float target_units,
                                      bool vertical, float s) const {
    size_t n_ext = 0;
    for (const GlyphPart& p : parts) n_ext += p.extender() ? 1 : 0;
    const size_t n_fixed = parts.size() - n_ext;
    auto adv_sum = [&](size_t k) {
        float sum = -0.0f;
        for (const GlyphPart& p : parts) {
            sum += static_cast<float>(p.full_advance) * (p.extender() ? static_cast<float>(k) : 1.0f);
        }
        return sum;
    };
    auto count = [&](size_t k) { return n_fixed + n_ext * k; };
    auto saturating_sub1 = [](size_t v) { return v == 0 ? size_t(0) : v - 1; };
    auto size = [&](size_t k) { return adv_sum(k) - min_overlap * static_cast<float>(saturating_sub1(count(k))); };

    size_t reps_k = 1;
    if (n_ext > 0) {
        while (size(reps_k) < target_units && reps_k < 60) reps_k += 1;
    }
    const float joints = static_cast<float>(std::max<size_t>(saturating_sub1(count(reps_k)), 1));
    float max_overlap = std::numeric_limits<float>::max();
    for (const GlyphPart& p : parts) {
        max_overlap = min_f(max_overlap, static_cast<float>(std::min(p.start_connector_length, p.end_connector_length)));
    }
    const float surplus = max_f(size(reps_k) - target_units, 0.0f);
    const float overlap = min_f(min_overlap + surplus / joints, max_f(max_overlap, min_overlap));

    Assembly a{{}, 0.0f, std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()};
    float cursor = 0.0f;  // font units along the stretch direction
    for (const GlyphPart& p : parts) {
        const size_t reps = p.extender() ? reps_k : 1;
        for (size_t r = 0; r < reps; r++) {
            const Extents e = glyph_extents(p.glyph_id);
            const float full = static_cast<float>(p.full_advance);
            if (vertical) {
                const float origin_y = cursor / upem_ * s - e.ymin * s;
                a.items.push_back(GlyphItem{p.glyph_id, 0.0f, origin_y, s, s});
                const float adv = static_cast<float>(font_.glyph_hor_advance(p.glyph_id).value_or(0)) / upem_ * s;
                a.w = max_f(max_f(a.w, adv), e.xmax * s);
                a.ymin = min_f(a.ymin, cursor / upem_ * s);
                a.ymax = max_f(a.ymax, (cursor + full) / upem_ * s);
            } else {
                const float origin_x = cursor / upem_ * s;
                a.items.push_back(GlyphItem{p.glyph_id, origin_x, 0.0f, s, s});
                a.w = max_f(a.w, (cursor + full) / upem_ * s);
                a.ymin = min_f(a.ymin, e.ymin * s);
                a.ymax = max_f(a.ymax, e.ymax * s);
            }
            cursor += full - overlap;
        }
    }
    return a;
}

// Horizontally stretch `ch` to at least `target` em (braces, wide accents).
std::optional<Layouter::Stretched> Layouter::hstretch(char32_t ch, float target, Ctx ctx) const {
    auto gid_opt = font_.glyph_index(ch);
    if (!gid_opt) return std::nullopt;
    const uint16_t gid = *gid_opt;
    const float s = scale(ctx.style);
    const float target_units = target / s * upem_;
    auto cons = font_.horizontal_construction(gid);
    const float base_w = static_cast<float>(font_.glyph_hor_advance(gid).value_or(0));

    auto single = [&](uint16_t g) {
        const float w = static_cast<float>(font_.glyph_hor_advance(g).value_or(0)) / upem_ * s;
        const Extents e = glyph_extents(g);
        std::vector<Item> items;
        items.push_back(GlyphItem{g, 0.0f, 0.0f, s, s});
        return Stretched{std::move(items), w, e.ymin * s, e.ymax * s};
    };
    if (!cons) return single(gid);
    if (base_w >= target_units) return single(gid);
    uint16_t last = gid;
    for (const GlyphVariant& v : cons->variants) {
        last = v.variant_glyph;
        if (static_cast<float>(v.advance_measurement) >= target_units) return single(v.variant_glyph);
    }
    if (cons->assembly) {
        const float overlap = static_cast<float>(font_.min_connector_overlap());
        Assembly a = assemble(*cons->assembly, overlap, target_units, false, s);
        return Stretched{std::move(a.items), a.w, a.ymin, a.ymax};
    }
    return single(last);
}

// `\left( ... \middle| ... \right)`: delimiters sized to the tallest piece
// (rule 19: at least 90.1% of the content, or within 0.5em of it).
MBox Layouter::fenced(const node::Fenced& f, Ctx ctx) const {
    std::vector<MBox> boxes;
    boxes.reserve(f.segs.size());
    for (const Nodes& sg : f.segs) boxes.push_back(row(sg, ctx));
    const float s = scale(ctx.style);
    const float ax = axis(ctx);
    float h = 0.0f, d = 0.0f;
    for (const MBox& b : boxes) {
        h = max_f(h, b.h);
        d = max_f(d, b.d);
    }
    const float delta = max_f(h - ax, d + ax);
    const float target = max_f(delta * 2.0f * 0.901f, delta * 2.0f - 0.5f * s);

    MBox out;
    float x = 0.0f;
    const MBox lb = delim(f.left, target, ctx);
    out.place(lb, x, 0.0f);
    x += lb.w;
    for (size_t i = 0; i < boxes.size(); i++) {
        if (i > 0) {
            const char32_t mid = i - 1 < f.mids.size() ? f.mids[i - 1] : U'|';
            const MBox mb = delim(mid, target, ctx);
            out.place(mb, x, 0.0f);
            x += mb.w;
        }
        out.place(boxes[i], x, 0.0f);
        x += boxes[i].w;
    }
    const MBox rb = delim(f.right, target, ctx);
    out.place(rb, x, 0.0f);
    x += rb.w;
    out.w = x;
    return out;
}

// ----- roots ------------------------------------------------------------------

// TeXbook Appendix G, rule 11 (with OpenType's radical gap / thickness).
MBox Layouter::sqrt(const node::Sqrt& sq, Ctx ctx) const {
    const MathConstants& K = font_.math_constants();
    const float s = scale(ctx.style);
    const MBox b = row(sq.body, Ctx{ctx.style, true, ctx.font});
    const bool display = ctx.style == Style::Display;
    const float gap = display ? k(K.radical_display_style_vertical_gap, ctx) : k(K.radical_vertical_gap, ctx);
    const float t = k(K.radical_rule_thickness, ctx);
    const float extra = k(K.radical_extra_ascender, ctx);
    const float needed = b.h + b.d + gap + t;

    // Pick a radical glyph (or assembly) tall enough.
    std::vector<Item> items;
    float rad_w, ymin, ymax;
    if (auto rad_gid = font_.glyph_index(U'√')) {
        const uint16_t gid = *rad_gid;
        auto cons = font_.vertical_construction(gid);
        const float target_units = needed / s * upem_;
        uint16_t chosen = gid;
        std::optional<Assembly> as;
        if (cons) {
            const Extents e = glyph_extents(gid);
            if ((e.ymax - e.ymin) * upem_ < target_units) {
                bool found = false;
                uint16_t last = gid;
                for (const GlyphVariant& v : cons->variants) {
                    last = v.variant_glyph;
                    if (static_cast<float>(v.advance_measurement) >= target_units) {
                        chosen = v.variant_glyph;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    if (cons->assembly) {
                        const float ov = static_cast<float>(font_.min_connector_overlap());
                        as = assemble(*cons->assembly, ov, target_units, true, s);
                    } else {
                        chosen = last;
                    }
                }
            }
        }
        if (as) {
            items = std::move(as->items);
            rad_w = as->w;
            ymin = as->ymin;
            ymax = as->ymax;
        } else {
            MBox bx = glyph_id(chosen, s);
            const Extents e = glyph_extents(chosen);
            items = std::move(bx.items);
            rad_w = bx.w;
            ymin = e.ymin * s;
            ymax = e.ymax * s;
        }
    } else {
        rad_w = 0.6f * s;
        ymin = 0.0f;
        ymax = 1.0f * s;
    }
    const float rad_h = ymax - ymin;
    const float pad = max_f((rad_h - needed) / 2.0f, 0.0f);
    const float bottom = -b.d - pad;
    const float dy = bottom - ymin;
    const float top = bottom + max_f(rad_h, needed);
    for (Item& it : items) {
        if (auto* g = std::get_if<GlyphItem>(&it)) g->y += dy;
    }

    // Optional index (the 3 in cube roots).
    float left = 0.0f;
    std::optional<MBox> idx_box;
    float idx_x = 0.0f, idx_y = 0.0f;
    if (sq.index) {
        MBox ib = row(*sq.index, Ctx{Style::ScriptScript, false, ctx.font});
        const float before = k(K.radical_kern_before_degree, ctx);
        const float after = k(K.radical_kern_after_degree, ctx);
        const float raise = static_cast<float>(K.radical_degree_bottom_raise_percent) / 100.0f * (top - bottom);
        left = max_f(before + ib.w + after, 0.0f);
        idx_x = max_f(before, 0.0f);
        idx_y = bottom + raise;
        idx_box = std::move(ib);
    }

    MBox out;
    if (idx_box) out.place(*idx_box, idx_x, idx_y);
    out.place(items_box(std::move(items)), left, 0.0f);
    out.place(b, left + rad_w, 0.0f);
    // The bar over the radicand: overlaps the radical's top slightly so they join.
    const float overlap = 0.01f * s;
    out.items.push_back(RuleItem{left + rad_w - overlap, top - t, b.w + overlap, t});
    out.w = left + rad_w + b.w;
    out.h = max_f(out.h, top + extra);
    out.d = max_f(out.d, max_f(-bottom, 0.0f));
    return out;
}

// ----- accents and over/under ------------------------------------------------

// TeXbook Appendix G, rule 12 (attachment point from the MATH table).
MBox Layouter::accent(const node::Accent& a, Ctx ctx) const {
    const float s = scale(ctx.style);
    MBox b = row(a.body, Ctx{ctx.style, true, ctx.font});
    auto gid = font_.glyph_index(a.ch);
    if (!gid) return b;

    std::vector<Item> items;
    float ink_min_x, ink_max_x, ymax;
    if (a.wide) {
        auto st = hstretch(a.ch, b.w, ctx);
        if (!st) return b;
        // ink extents in x: assume centred on the advance
        ink_min_x = 0.0f;
        ink_max_x = st->w;
        ymax = st->ymax;
        items = std::move(st->items);
    } else {
        const Extents e = glyph_extents(*gid);
        ink_min_x = e.xmin * s;
        ink_max_x = e.xmax * s;
        ymax = e.ymax * s;
        items.push_back(GlyphItem{*gid, 0.0f, 0.0f, s, s});
    }

    const float base_attach = b.accent_x.value_or(b.w / 2.0f);
    const float accent_center = (ink_min_x + ink_max_x) / 2.0f;
    const float dx = base_attach - accent_center;
    const float accent_base = k(font_.math_constants().accent_base_height, ctx);
    const float raise = max_f(b.h - accent_base, 0.0f);

    MBox out;
    out.place(b, 0.0f, 0.0f);
    out.place(items_box(std::move(items)), dx, raise);
    out.h = max_f(out.h, ymax + raise);
    out.w = b.w;
    out.ic = b.ic;
    return out;
}

MBox Layouter::over(const node::Over& o, Ctx ctx) const {
    const MathConstants& K = font_.math_constants();
    const MBox b = row(o.body, ctx);
    const float s = scale(ctx.style);
    MBox out;
    out.place(b, 0.0f, 0.0f);
    out.w = b.w;
    if (o.mark == Mark::Bar) {
        // \overline / \underline (rules 9 and 10)
        if (o.under) {
            const float gap = k(K.underbar_vertical_gap, ctx);
            const float t = k(K.underbar_rule_thickness, ctx);
            const float extra = k(K.underbar_extra_descender, ctx);
            out.items.push_back(RuleItem{0.0f, -(b.d + gap + t), b.w, t});
            out.d = b.d + gap + t + extra;
        } else {
            const float gap = k(K.overbar_vertical_gap, ctx);
            const float t = k(K.overbar_rule_thickness, ctx);
            const float extra = k(K.overbar_extra_ascender, ctx);
            out.items.push_back(RuleItem{0.0f, b.h + gap, b.w, t});
            out.h = b.h + gap + t + extra;
        }
    } else if (o.mark == Mark::Brace) {
        const char32_t brace_ch = o.under ? U'⏟' : U'⏞';
        if (auto st = hstretch(brace_ch, b.w, ctx)) {
            const float gap = 0.12f * s;
            const float w = max_f(st->w, b.w);
            out = MBox{};
            out.place(b, (w - b.w) / 2.0f, 0.0f);
            const float dy = o.under ? -(b.d + gap) - st->ymax : b.h + gap - st->ymin;
            const float st_w = st->w, st_ymin = st->ymin, st_ymax = st->ymax;
            out.place(items_box(std::move(st->items)), (w - st_w) / 2.0f, dy);
            if (o.under) {
                out.d = max_f(out.d, -(st_ymin + dy));
            } else {
                out.h = max_f(out.h, st_ymax + dy);
            }
            out.w = w;
        }
    }
    if (o.label) {
        // \overset / \underset: small label stacked above or below.
        const MBox lb = row(*o.label, Ctx{sup_style(ctx.style), o.under, ctx.font});
        const float gap = k(K.upper_limit_gap_min, ctx);
        const float w = max_f(out.w, lb.w);
        MBox res;
        res.place(out, (w - out.w) / 2.0f, 0.0f);
        if (o.under) {
            res.place(lb, (w - lb.w) / 2.0f, -(out.d + gap + lb.h));
        } else {
            res.place(lb, (w - lb.w) / 2.0f, out.h + gap + lb.d);
        }
        res.w = w;
        return res;
    }
    return out;
}

// ----- extensible arrows, boxes, strike-throughs -------------------------------

// `\xrightarrow[under]{over}`: an arrow stretched to fit its labels.
MBox Layouter::xarrow(const node::XArrow& xa, Ctx ctx) const {
    const float s = scale(ctx.style);
    const Ctx sctx{sup_style(ctx.style), ctx.cramped, ctx.font};
    const MBox ob = row(xa.over, sctx);
    std::optional<MBox> ub;
    if (xa.under) ub = row(*xa.under, Ctx{sctx.style, true, sctx.font});
    const float label_w = max_f(ob.w, ub ? ub->w : 0.0f);
    auto st = hstretch(xa.ch, label_w + 0.9f * s, ctx);
    if (!st) return ob;
    const float ax = axis(ctx);
    const float dy = ax - (st->ymin + st->ymax) / 2.0f;
    const float w = max_f(st->w, label_w + 0.2f * s);
    const float st_w = st->w, st_ymin = st->ymin, st_ymax = st->ymax;
    MBox out;
    out.place(items_box(std::move(st->items)), (w - st_w) / 2.0f, dy);
    out.h = max_f(out.h, st_ymax + dy);
    out.d = max_f(out.d, -(st_ymin + dy));
    const float gap = 0.12f * s;
    out.place(ob, (w - ob.w) / 2.0f, st_ymax + dy + gap + ob.d);
    if (ub) out.place(*ub, (w - ub->w) / 2.0f, st_ymin + dy - gap - ub->h);
    out.w = w;
    return out;
}

MBox Layouter::boxed(const Nodes& body, Ctx ctx) const {
    const float s = scale(ctx.style);
    const MBox b = row(body, ctx);
    const float t = k(font_.math_constants().fraction_rule_thickness, ctx);
    const float sep = 0.2f * s;
    const float w = b.w + 2.0f * (sep + t);
    const float top = b.h + sep + t, bot = b.d + sep + t;
    MBox out;
    out.place(b, sep + t, 0.0f);
    out.items.push_back(RuleItem{0.0f, -bot, w, t});
    out.items.push_back(RuleItem{0.0f, b.h + sep, w, t});
    out.items.push_back(RuleItem{0.0f, -bot, t, top + bot});
    out.items.push_back(RuleItem{w - t, -bot, t, top + bot});
    out.h = top;
    out.d = bot;
    out.w = w;
    return out;
}

MBox Layouter::cancel(const node::Cancel& c, Ctx ctx) const {
    const float s = scale(ctx.style);
    const MBox b = row(c.body, ctx);
    const float t = k(font_.math_constants().fraction_rule_thickness, ctx);
    const float o = 0.06f * s;
    MBox out;
    out.place(b, 0.0f, 0.0f);
    out.w = b.w;
    if (!c.back || c.both) out.items.push_back(LineItem{-o, -b.d - o, b.w + o, b.h + o, t});
    if (c.back || c.both) out.items.push_back(LineItem{-o, b.h + o, b.w + o, -b.d - o, t});
    out.ic = b.ic;
    return out;
}

// ----- matrices -------------------------------------------------------------

MBox Layouter::matrix(const node::Matrix& m, Ctx ctx) const {
    const MatKind::Tag kind = m.kind.tag;
    Style cell_style;
    if (kind == MatKind::Small || kind == MatKind::Substack) {
        cell_style = Style::Script;
    } else if (kind == MatKind::Aligned || kind == MatKind::Gathered) {
        cell_style = ctx.style;
    } else {
        cell_style = ctx.style == Style::Display ? Style::Text : ctx.style;
    }
    const Ctx cctx{cell_style, ctx.cramped, ctx.font};
    const float cs = scale(cell_style);
    size_t ncols = 0;
    for (const auto& r : m.rows) ncols = std::max(ncols, r.size());
    if (ncols == 0) return MBox{};

    std::vector<std::vector<MBox>> boxes;
    boxes.reserve(m.rows.size());
    for (const auto& r : m.rows) {
        std::vector<MBox> row_boxes;
        row_boxes.reserve(r.size());
        for (size_t c = 0; c < r.size(); c++) {
            // In `aligned`, odd columns start after an `&`: a leading empty
            // atom makes `=` get its usual relation spacing.
            if (kind == MatKind::Aligned && c % 2 == 1) {
                Nodes v;
                v.push_back(node::Group{});
                v.insert(v.end(), r[c].begin(), r[c].end());
                row_boxes.push_back(row(v, cctx));
            } else {
                row_boxes.push_back(row(r[c], cctx));
            }
        }
        boxes.push_back(std::move(row_boxes));
    }

    std::vector<float> col_w(ncols, 0.0f);
    for (const auto& r : boxes) {
        for (size_t c = 0; c < r.size(); c++) col_w[c] = max_f(col_w[c], r[c].w);
    }
    auto align = [&](size_t c) -> char32_t {
        switch (kind) {
            case MatKind::Cases: return U'l';
            case MatKind::Aligned: return c % 2 == 0 ? U'r' : U'l';
            case MatKind::Array: return c < m.kind.cols.size() ? m.kind.cols[c] : U'c';
            default: return U'c';
        }
    };
    auto sep = [&](size_t c) -> float {  // separation after column c
        switch (kind) {
            case MatKind::Small: return 0.5f * cs;
            case MatKind::Aligned: return c % 2 == 0 ? 0.0f : 2.0f * cs;
            default: return 1.0f * cs;
        }
    };
    std::vector<float> col_x(ncols, 0.0f);
    for (size_t c = 1; c < ncols; c++) col_x[c] = col_x[c - 1] + col_w[c - 1] + sep(c - 1);
    const float total_w = col_x[ncols - 1] + col_w[ncols - 1];

    // Row metrics with a strut so rows never collapse.
    const bool small = kind == MatKind::Substack || kind == MatKind::Small;
    const float strut_h = small ? 0.6f * cs : 0.7f * cs;
    const float strut_d = small ? 0.25f * cs : 0.3f * cs;
    const size_t nrows = m.rows.size();
    std::vector<float> row_h(nrows), row_d(nrows);
    for (size_t i = 0; i < nrows; i++) {
        float hh = strut_h, dd = strut_d;
        for (const MBox& b : boxes[i]) {
            hh = max_f(hh, b.h);
            dd = max_f(dd, b.d);
        }
        row_h[i] = hh;
        row_d[i] = dd;
    }
    // Baseline offsets relative to the first row, going DOWN.
    std::vector<float> base_y(nrows, 0.0f);
    // amsmath adds \jot (0.3em) between rows of aligned/gathered displays.
    const bool display_rows = kind == MatKind::Aligned || kind == MatKind::Gathered;
    const float pitch = display_rows ? 1.5f : 1.2f;
    const float min_gap = display_rows ? 0.3f : 0.2f;
    for (size_t i = 1; i < nrows; i++) {
        const float dist = max_f(pitch * cs, row_d[i - 1] + row_h[i] + min_gap * cs);
        base_y[i] = base_y[i - 1] + dist;
    }
    const float top = row_h[0];
    const float bottom = base_y[nrows - 1] + row_d[nrows - 1];
    const float total_h = top + bottom;
    // vertically centre on the axis
    const float ax = axis(ctx);
    const float first_baseline = ax + total_h / 2.0f - top;

    MBox body;
    for (size_t i = 0; i < boxes.size(); i++) {
        const float y = first_baseline - base_y[i];
        for (size_t c = 0; c < boxes[i].size(); c++) {
            const MBox& b = boxes[i][c];
            float x;
            switch (align(c)) {
                case U'l': x = col_x[c]; break;
                case U'r': x = col_x[c] + col_w[c] - b.w; break;
                default: x = col_x[c] + (col_w[c] - b.w) / 2.0f; break;
            }
            body.place(b, x, y);
        }
    }
    body.w = total_w;
    body.h = max_f(body.h, first_baseline + top);
    body.d = max_f(body.d, -(first_baseline - bottom));

    if (!m.left && !m.right) return body;
    const float s = scale(ctx.style);
    const float target = max_f(total_h * 0.901f, total_h - 0.5f * s);
    const float pad = 3.0f / 18.0f * s;
    const MBox lb = delim(m.left, target, ctx);
    const MBox rb = delim(m.right, target, ctx);
    MBox out;
    float x = 0.0f;
    out.place(lb, x, 0.0f);
    x += lb.w + (m.left ? pad : 0.0f);
    out.place(body, x, 0.0f);
    x += body.w + (m.right ? pad : 0.0f);
    out.place(rb, x, 0.0f);
    out.w = x + rb.w;
    return out;
}

}  // namespace texmath
