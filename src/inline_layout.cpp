#include "inline_layout.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include <imgui_internal.h>  // ImTextCharFromUtf8

#include "math.h"
#include "prose_shaping.h"
#include "shaped_draw.h"
#include "shaping.h"
#include "text_capture.h"

void draw_find_highlights(ImDrawList* dl, const Collector& sel, float y0, float y1, const Theme& theme) {
    for (const FindHighlight& f : sel.find_highlights()) {
        dl->AddRectFilled(ImVec2(f.x0, y0), ImVec2(f.x1, y1), f.current ? theme.find_current : theme.find_match);
        // The current match also gets a 1 px ui.accent outline (on pixel centres: crisp).
        if (f.current) dl->AddRect(ImVec2(f.x0 + 0.5f, y0 + 0.5f), ImVec2(f.x1 - 0.5f, y1 - 0.5f), theme.ui.accent);
    }
}

namespace {

// Black or white, whichever reads better on `bg` (relative luminance > 0.4: black).
ImU32 contrast_text(ImU32 bg) {
    auto lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    const float r = lin(float((bg >> IM_COL32_R_SHIFT) & 0xFF) / 255), g = lin(float((bg >> IM_COL32_G_SHIFT) & 0xFF) / 255),
                b = lin(float((bg >> IM_COL32_B_SHIFT) & 0xFF) / 255);
    return 0.2126f * r + 0.7152f * g + 0.0722f * b > 0.4f ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
}

// Ascent/descent of `font` at ImGui size `size`. In ImGui 1.92 per-size metrics
// live in an ImFontBaked (one per size actually used). Descent is stored negative.
void metrics(ImFont* font, float size, float& asc, float& desc) {
    ImFontBaked* baked = font->GetFontBaked(size);
    asc = baked->Ascent;
    desc = -baked->Descent;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Characters a too-long word may be cut after, like browsers do for long paths.
bool breaks_after(unsigned int c) { return c == '/' || c == '\\' || c == '_' || c == '-' || c == '.' || c == ':'; }

std::vector<unsigned int> codepoints(const std::string& s) {
    std::vector<unsigned int> out;
    const char* p = s.data();
    const char* end = p + s.size();
    while (p < end) {
        unsigned int c = 0;
        const int n = ImTextCharFromUtf8(&c, p, end);
        if (n <= 0) break;
        p += n;
        out.push_back(c);
    }
    return out;
}

// Unhinted glyph advances. FreeType's light hinting rounds each advance to a whole pixel (even
// with PixelSnapH off), which made code 6 % and prose 1 % narrower or wider than egui's. So
// ask the same font at a bigger size (where rounding hardly matters) and scale back down.
// (That costs no atlas space: from 128 px up ImGui loads only a glyph's advance, not its
// picture. All characters need it: box drawing in code once drifted off the columns.)
struct Advances {
    ImFontBaked* big;
    float factor = 1.0f;
    Advances(ImFont* font, float size) {
        factor = std::clamp(std::floor(1200.0f / std::max(size, 1.0f)), 1.0f, 32.0f);
        big = font->GetFontBaked(size * factor);
    }
    float of(unsigned int c) const { return big->GetCharAdvance(static_cast<ImWchar>(c)) / factor; }
};

// Text with complex-script characters (spec 32), laid out from HarfBuzz's glyphs instead of one
// advance per character. `xs` are the character boundaries (n + 1 of them, from the string's
// left edge); `items` what to draw. Where a run can't be shaped, its characters stay per character.
struct Placed {
    struct Item {
        float x = 0, y = 0;                  // pen position (y: up is negative, from HarfBuzz's offset)
        unsigned int c = 0;                  // a plain character to draw like ImGui does, or...
        uint32_t glyph = 0;                  // ...a glyph of `source` (0 = none: it's a character)
        const prose_shaping::Source* source = nullptr;
        unsigned int cluster_char = 0;       // what to draw if the glyph can't be (the cluster's first character)
        std::string text;                    // PDF export: the cluster's characters (UTF-8) on its first glyph, else empty
    };
    std::vector<float> xs;
    std::vector<Item> items;
    std::shared_ptr<const prose_shaping::Plan> plan;  // keeps `source` pointers valid
};

// `c` as UTF-8, appended to `s`.
void append_utf8(std::string& s, uint32_t c) {
    char buf[5] = {};
    const int n = ImTextCharToUtf8(buf, c);
    s.append(buf, size_t(n));
}

std::string utf8_of(unsigned int c) {
    std::string s;
    append_utf8(s, c);
    return s;
}

// false: the string has no complex-script text (or no font to shape it): use the old path.
bool place(ImFont* font, float size, float adv, std::string_view s, Placed& out) {
    out.plan = prose_shaping::plan(font, s);
    if (!out.plan) return false;
    const Advances advances(font, size);
    const float exact = size * adv;  // the size the layout really uses (see Fonts::advance_scale)
    float x = 0;
    for (const prose_shaping::Part& part : out.plan->parts) {
        if (!part.shaped) {  // the old path
            for (size_t i = part.begin; i < part.end; i++) {
                const unsigned int c = out.plan->chars[i];
                out.xs.push_back(x);
                out.items.push_back({x, 0, c});
                x += advances.of(c) * adv;
            }
            continue;
        }
        const float em_px = prose_shaping::ppem(part.source, exact);
        for (const prose_shaping::Cluster& cl : part.clusters) {
            const unsigned int first_char = out.plan->chars[part.begin + cl.first];
            bool broken = false;  // a glyph the font does not have (.notdef)
            for (size_t g = cl.glyph_first; g < cl.glyph_last; g++) broken |= part.glyphs[g].id == 0;
            if (broken) {  // this cluster only: per character, as the old path (◻ where nothing has it)
                for (size_t i = cl.first; i < cl.last; i++) {
                    const unsigned int c = out.plan->chars[part.begin + i];
                    out.xs.push_back(x);
                    out.items.push_back({x, 0, c});
                    x += advances.of(c) * adv;
                }
                continue;
            }
            // Every character boundary inside a cluster is its left edge: it is selected whole.
            for (size_t i = cl.first; i < cl.last; i++) out.xs.push_back(x);
            float pen = x;
            for (size_t g = cl.glyph_first; g < cl.glyph_last; g++) {
                const prose_shaping::Glyph& gl = part.glyphs[g];
                Placed::Item it;
                it.x = pen + gl.x_off * em_px;
                it.y = -gl.y_off * em_px;
                it.glyph = gl.id;
                it.source = &part.source;
                it.cluster_char = g == cl.glyph_first ? first_char : 0;
                if (g == cl.glyph_first && text_capture::enabled()) {
                    for (size_t i = cl.first; i < cl.last; i++) append_utf8(it.text, out.plan->chars[part.begin + i]);
                }
                out.items.push_back(it);
                pen += gl.x_adv * em_px;
            }
            x = pen;
        }
    }
    out.xs.push_back(x);
    return true;
}

}  // namespace

// The line-box metrics egui gives a font (what the Rust port lays out with): the whole height
// is the whole-pixel size, the descent is that height's descender share, rounded, and the
// ascent is the rest. (ImGui's own baked ascent/descent are each rounded up, so they can be
// a pixel taller in total, which made every heading and line box 1 px too tall.)
static void egui_metrics(const Fonts& fonts, const ImFont* font, float px, float& asc, float& desc) {
    desc = std::round(px * fonts.desc_fraction(font));
    asc = px - desc;
}

float text_width(ImFont* font, float size, float adv, std::string_view s) {
    if (Placed placed; place(font, size, adv, s, placed)) return placed.xs.back();
    const Advances advances(font, size);
    const char* p = s.data();
    const char* end = p + s.size();
    float w = 0;
    while (p < end) {
        unsigned int c = 0;
        const int n = ImTextCharFromUtf8(&c, p, end);
        if (n <= 0) break;
        p += n;
        w += advances.of(c) * adv;
    }
    return w;
}

static bool g_blank_missing = false;
void set_blank_missing(bool on) { g_blank_missing = on; }

void draw_text(ImDrawList* dl, ImFont* font, float size, float adv, ImVec2 pos, ImU32 col, std::string_view s) {
    const float y = std::round(pos.y);
    // A plain character (whitespace draws nothing). PDF export (spec 33): captured as text instead of drawn.
    auto plain = [&](ImVec2 at, unsigned int c) {
        if (c == ' ' || c == '\t') return;
        if (g_blank_missing && !font->IsGlyphInFont(static_cast<ImWchar>(c))) return;  // (spec 30: no box while a font is being found)
        if (!text_capture::character(dl, font, size, at, col, c)) font->RenderChar(dl, size, at, col, static_cast<ImWchar>(c));
    };
    // Complex scripts (spec 32): glyphs from the shaper, each on a whole pixel like the old path.
    if (Placed placed; place(font, size, adv, s, placed)) {
        for (const Placed::Item& it : placed.items) {
            const ImVec2 at(std::round(pos.x + it.x), y + std::round(it.y));
            if (it.glyph == 0) {
                plain(at, it.c);
            } else if (text_capture::glyph(dl, font, size, at, col, *it.source, it.glyph, it.text)) {
                // (captured for the PDF)
            } else if (!shaped_draw::draw(dl, font, size, at, col, *it.source, it.glyph) && it.cluster_char != 0) {
                font->RenderChar(dl, size, at, col, static_cast<ImWchar>(it.cluster_char));
            }
        }
        return;
    }
    const Advances advances(font, size);
    float x = pos.x;
    // Code ligatures (shaping.h): the glyph to draw for each character, null for ordinary text.
    const std::vector<uint32_t>* glyphs = shaping::glyphs(font, s);
    size_t index = 0;  // which character
    const char* p = s.data();
    const char* end = p + s.size();
    while (p < end) {
        unsigned int c = 0;
        const int n = ImTextCharFromUtf8(&c, p, end);
        if (n <= 0) break;
        p += n;
        if (c != ' ' && c != '\t') {
            const ImVec2 at(std::round(x), y);
            const uint32_t glyph = glyphs && index < glyphs->size() ? (*glyphs)[index] : 0;
            // A ligature glyph is drawn by id (for the PDF: the code font's glyph, one per character);
            // everything else (and a failure) by character, as ImGui does.
            const prose_shaping::Source* code_font = glyph != 0 && text_capture::enabled() ? prose_shaping::main_source(font) : nullptr;
            if (code_font && text_capture::glyph(dl, font, size, at, col, *code_font, glyph, utf8_of(c))) {
                // (captured for the PDF)
            } else if (glyph == 0 || !shaped_draw::draw(dl, font, size, at, col, glyph)) {
                plain(at, c);
            }
        }
        index++;
        x += advances.of(c) * adv;
    }
}

void font_metrics(const Fonts& fonts, ImFont* font, float em, float& asc, float& desc) {
    egui_metrics(fonts, font, fonts.px(font, em), asc, desc);
}

// Cut points of a word token (see Tok::cuts).
static std::vector<float> cut_points(const Tok& t) {
    const std::vector<unsigned int> cps = codepoints(t.text);
    if (!std::any_of(cps.begin(), cps.end(), breaks_after)) return {};
    const std::vector<float> xs = char_xs(t.font, t.size, t.adv, t.text, 0);
    std::vector<float> out;
    for (size_t i = 0; i + 1 < cps.size() && i + 1 < xs.size(); i++)
        if (breaks_after(cps[i])) out.push_back(xs[i + 1]);
    return out;
}

std::vector<Tok> tokenize(const md::Spans& spans, const Base& base, const Fonts& fonts, const Theme& theme) {
    std::vector<Tok> out;
    bool last_was_space = true;  // "is the previous token whitespace?" -> decides `glue`

    for (const md::Span& span : spans) {
        ImU32 color = span.link.empty() ? base.color : theme.link;
        // Emphasis in plain body text takes the theme's bold/italic colour (bold wins).
        if (span.link.empty() && !base.bold && span.bold && base.bold_color) color = base.bold_color;
        else if (span.link.empty() && !base.bold && span.italic && base.italic_color) color = base.italic_color;

        bool blank = std::all_of(span.text.begin(), span.text.end(), is_space);
        // Math: typeset by the Rust TeX engine (falls through to monospace source on error).
        if (span.math && !blank) {
            if (const MathTex* m = math_get(span.text, false, base.size * 1.1f)) {
                Tok t;
                t.math = m;
                t.text = "$" + span.text + "$";  // what copying it gives
                t.w = m->w;
                t.asc = m->asc;
                t.desc = m->desc;
                t.color = color;
                t.glue = !last_was_space;
                out.push_back(std::move(t));
                last_was_space = false;
                continue;
            }
        }
        // Code (and math that failed to parse) is one unbreakable monospace token.
        if (span.code || (span.math && !blank)) {
            Tok t;
            t.text = span.math ? "$" + span.text + "$" : span.text;
            t.font = fonts.mono;
            t.size = fonts.px(fonts.mono, base.size * 0.88f);
            egui_metrics(fonts, t.font, t.size, t.asc, t.desc);
            { float d; metrics(t.font, t.size, t.ink_asc, d); }
            t.pad = std::round(2.0f * fonts.scale);  // padding each side (2 points)
            t.adv = fonts.advance_scale(t.font, base.size * 0.88f);
            t.w = text_width(t.font, t.size, t.adv, t.text) + 2 * t.pad;
            t.color = span.link.empty() ? theme.code_fg : theme.link;
            t.code = true;
            t.link = span.link;
            t.glue = !last_was_space;
            t.cuts = cut_points(t);
            out.push_back(std::move(t));
            last_was_space = false;
            continue;
        }

        ImFont* font = fonts.face(base.bold || span.bold, base.italic || span.italic);
        const float size = fonts.px(font, span.small ? base.size * 0.8f : base.size);
        float asc, desc;
        egui_metrics(fonts, font, size, asc, desc);
        float ink_asc, ink_desc;
        metrics(font, size, ink_asc, ink_desc);
        const float em = span.small ? base.size * 0.8f : base.size;
        const float adv = fonts.advance_scale(font, em);
        const float space_w = text_width(font, size, adv, " ");

        // Split the span's text into alternating runs of words and whitespace.
        const std::string& s = span.text;
        size_t i = 0;
        while (i < s.size()) {
            size_t j = i;
            bool space = is_space(s[i]);
            while (j < s.size() && is_space(s[j]) == space) j++;
            std::string_view run(s.data() + i, j - i);

            if (space) {
                auto breaks = std::count(run.begin(), run.end(), '\n');
                if (breaks > 0) {  // hard line break(s)
                    for (int k = 0; k < breaks; k++) out.push_back(Tok{.kind = Tok::Kind::Break});
                    last_was_space = true;
                } else if (!last_was_space) {  // collapse repeated whitespace
                    Tok t{.kind = Tok::Kind::Space, .w = space_w, .color = color};
                    // Inside a link, keep the underline continuous across the space.
                    if (!out.empty() && !span.link.empty() && out.back().link == span.link) t.link = span.link;
                    out.push_back(std::move(t));
                    last_was_space = true;
                }
            } else {
                Tok t;
                t.text = std::string(run);
                t.font = font;
                t.size = size;
                t.adv = adv;
                t.w = text_width(font, size, adv, run);
                t.asc = asc;
                t.desc = desc;
                t.ink_asc = ink_asc;
                t.color = color;
                t.strike = span.strike;
                if (span.small) t.raise = std::round(base.size * 0.35f);  // footnote marks sit 0.35 em up
                t.link = span.link;
                t.glue = !last_was_space;
                t.cuts = cut_points(t);
                out.push_back(std::move(t));
                last_was_space = false;
            }
            i = j;
        }
    }
    return out;
}

std::pair<float, float> measure(const std::vector<Tok>& toks) {
    float natural = 0, cur = 0, min_w = 0, chunk = 0;
    for (const Tok& t : toks) {
        if (t.kind == Tok::Kind::Break) {
            natural = std::max(natural, cur);
            min_w = std::max(min_w, chunk);
            cur = chunk = 0;
        } else if (t.kind == Tok::Kind::Space) {
            cur += t.w;
            min_w = std::max(min_w, chunk);
            chunk = 0;
        } else {
            cur += t.w;
            if (t.cuts.empty()) {
                chunk += t.w;
            } else {  // a word that may be cut: only its pieces are unbreakable
                float from = -t.pad;
                for (const float x : t.cuts) {
                    min_w = std::max(min_w, chunk + x - from);
                    chunk = 0;
                    from = x;
                }
                chunk += t.w - t.pad - from;
            }
        }
    }
    return {std::max(natural, cur), std::max(min_w, chunk)};
}

std::vector<Tok> fit(std::vector<Tok> toks, float width) {
    // Which tokens are in an oversize chunk (grouped as layout does)?
    std::vector<bool> oversize(toks.size(), false);
    bool any = false;
    for (size_t i = 0; i < toks.size();) {
        if (toks[i].kind != Tok::Kind::Word) {
            i++;
            continue;
        }
        size_t end = i + 1;
        float w = toks[i].w;
        while (end < toks.size() && toks[end].glue && toks[end].kind == Tok::Kind::Word) w += toks[end++].w;
        if (w > width + 0.5f) {
            for (size_t k = i; k < end; k++) oversize[k] = true;
            any = true;
        }
        i = end;
    }
    if (!any) return toks;

    std::vector<Tok> out;
    for (size_t k = 0; k < toks.size(); k++) {
        Tok& t = toks[k];
        if (!oversize[k] || t.math) {
            out.push_back(std::move(t));
            continue;
        }
        // Boundaries (in characters): after each break character, then per character inside
        // pieces that are still wider than the line.
        const std::vector<unsigned int> cps = codepoints(t.text);
        const std::vector<float> xs = char_xs(t.font, t.size, t.adv, t.text, 0);
        const size_t n = std::min(cps.size(), xs.size() - 1);
        std::vector<size_t> bounds{0};
        size_t last = 0;
        for (size_t j = 1; j <= n; j++) {
            if (j != n && !breaks_after(cps[j - 1])) continue;
            size_t from = last;
            for (size_t m = last + 1; m <= j; m++)
                if (xs[m] - xs[from] + 2 * t.pad > width + 0.5f && m - 1 > from) bounds.push_back(from = m - 1);
            bounds.push_back(j);
            last = j;
        }
        if (bounds.size() < 3) {
            out.push_back(std::move(t));
            continue;
        }
        for (size_t p = 0; p + 1 < bounds.size(); p++) {
            Tok piece = t;  // keeps font, colours, link, metrics
            piece.text.clear();
            for (size_t j = bounds[p]; j < bounds[p + 1]; j++) append_utf8(piece.text, cps[j]);
            piece.cont = p > 0;
            piece.more = p + 2 < bounds.size();
            piece.pad = piece.cont ? 0.0f : t.pad;
            piece.w = text_width(t.font, t.size, t.adv, piece.text) + piece.pad + (piece.more ? 0.0f : t.pad);
            piece.glue = t.glue && !piece.cont;
            piece.cuts.clear();
            out.push_back(std::move(piece));
        }
    }
    return out;
}

Layout layout(const std::vector<Tok>& toks, float width, float line_h, std::pair<float, float> strut, HAlign align) {
    Layout lay;

    // The line under construction (starts as tall as the base font's strut).
    Line cur;
    float x = 0, asc = strut.first, desc = strut.second;
    bool has_content = false;
    float y = 0;

    auto finish = [&](bool next_hard = false) {
        float content = asc + desc;
        float h = std::max(content, line_h);
        cur.baseline = y + (h - content) / 2.0f + asc;  // centre the text in the line box
        cur.width = x;
        cur.top = y;
        cur.bottom = y + h;
        lay.lines.push_back(std::move(cur));
        cur = Line{};
        cur.hard = next_hard;
        y += h;
        x = 0;
        asc = strut.first;
        desc = strut.second;
        has_content = false;
    };

    std::ptrdiff_t pending_space = -1;  // a space we only place if a word follows on the same line
    size_t i = 0;
    while (i < toks.size()) {
        const Tok& t = toks[i];
        if (t.kind == Tok::Kind::Break) {
            finish(true);  // the next line starts after a hard break
            pending_space = -1;
            i++;
            continue;
        }
        if (t.kind == Tok::Kind::Space) {
            if (has_content) pending_space = static_cast<std::ptrdiff_t>(i);
            i++;
            continue;
        }

        // A "chunk" = this word plus any glued tokens after it ("**bold**," is
        // two tokens: "bold" and "," but must not be split across lines).
        size_t end = i + 1;
        float chunk_w = t.w;
        while (end < toks.size() && toks[end].glue && toks[end].kind == Tok::Kind::Word) {
            chunk_w += toks[end].w;
            end++;
        }

        float sp_w = pending_space >= 0 ? toks[pending_space].w : 0.0f;
        if (has_content && x + sp_w + chunk_w > width + 0.5f) {
            finish();  // doesn't fit: wrap (the pending space is dropped)
        } else if (pending_space >= 0) {
            cur.items.emplace_back(static_cast<size_t>(pending_space), x);
            x += sp_w;
        }
        pending_space = -1;

        for (size_t k = i; k < end; k++) {
            cur.items.emplace_back(k, x);
            x += toks[k].w;
            asc = std::max(asc, toks[k].asc);
            desc = std::max(desc, toks[k].desc);
        }
        has_content = true;
        i = end;
    }
    if (has_content || lay.lines.empty()) finish();

    if (align != HAlign::Left) {
        for (Line& l : lay.lines) {
            float spare = std::max(0.0f, width - l.width);
            float off = align == HAlign::Center ? spare / 2.0f : spare;
            for (auto& it : l.items) it.second += off;
        }
    }
    lay.height = y;
    return lay;
}

std::vector<float> char_xs(ImFont* font, float size, float adv, const std::string& text, float x0) {
    if (Placed placed; place(font, size, adv, text, placed)) {
        for (float& x : placed.xs) x += x0;
        return std::move(placed.xs);
    }
    std::vector<float> xs{x0};
    const Advances advances(font, size);
    const char* s = text.data();
    const char* end = s + text.size();
    float x = x0;
    while (s < end) {
        unsigned int c = 0;
        int n = ImTextCharFromUtf8(&c, s, end);
        if (n <= 0) break;
        s += n;
        x += advances.of(c) * adv;
        xs.push_back(x);
    }
    return xs;
}

void paint(ImDrawList* dl, ImVec2 origin, const Layout& lay, const std::vector<Tok>& toks,
           const Theme& theme, std::vector<LinkHit>& links, Collector* sel, ImU32 highlight) {
    for (const Line& line : lay.lines) {
        float base_y = origin.y + line.baseline;
        if (sel) sel->new_line();
        for (auto [ti, x] : line.items) {
            const Tok& t = toks[ti];
            float left = origin.x + x;
            float top = base_y - t.asc - t.raise;
            const float ink_top = t.ink_asc > 0 ? base_y - t.ink_asc - t.raise : top;  // where AddText starts
            ImVec2 min(left, top), max(left + t.w, base_y + t.desc);

            // An inline code span's background goes first, so find and selection highlights sit on top of it.
            if (t.kind == Tok::Kind::Word && !t.math && t.code) {
                // (flat where it joins the next / previous piece of a cut word)
                ImDrawFlags corners = (t.cont ? 0 : ImDrawFlags_RoundCornersLeft) | (t.more ? 0 : ImDrawFlags_RoundCornersRight);
                if (!corners) corners = ImDrawFlags_RoundCornersNone;
                dl->AddRectFilled(ImVec2(min.x, min.y - 1), ImVec2(max.x, max.y + 1), theme.code_bg, 3.0f, corners);
            }

            std::vector<FindHighlight> current;  // find_current fills under this token (its text gets a readable colour)
            if (sel && t.kind != Tok::Kind::Break) {
                std::vector<float> xs;
                std::string text;
                if (t.math) {
                    xs = {left, left + t.w};
                    text = t.text;
                    sel->math(min, max, t.text);
                } else if (t.kind == Tok::Kind::Space) {
                    xs = {left, left + t.w};
                    text = " ";
                } else {
                    xs = char_xs(t.font, t.size, t.adv, t.text, left + t.pad);
                    text = t.text;
                }
                ImVec2 box_min(left, origin.y + line.top), box_max(left + t.w, origin.y + line.bottom);
                sel->next_glued = t.cont;
                auto hl = sel->push(box_min, box_max, std::move(xs), std::move(text), line.hard);
                draw_find_highlights(dl, *sel, box_min.y, box_max.y, theme);
                for (const FindHighlight& f : sel->find_highlights())
                    if (f.current) current.push_back(f);
                if (hl) dl->AddRectFilled(ImVec2(hl->first, box_min.y), ImVec2(hl->second, box_max.y), highlight);
            }

            if (t.math) {
                // Bitmap's baseline-left corner goes on our baseline at `left`.
                ImVec2 p0(std::round(left - t.math->ox), std::round(base_y - t.math->oy));
                math_draw(dl, *t.math, p0, t.color);
            } else if (t.kind == Tok::Kind::Word) {
                const float pad = t.pad;
                // draw_text's `pos` is the top of the line box; the font places
                // glyphs `asc` pixels below it, so they sit on our baseline.
                // Each glyph is rounded to whole pixels so it isn't blurred by sub-pixel sampling.
                auto text_in = [&](ImU32 color, float x0, float x1) {
                    dl->PushClipRect(ImVec2(x0, top - 100), ImVec2(x1, top + 200 + t.asc), true);
                    draw_text(dl, t.font, t.size, t.adv, ImVec2(left + pad, ink_top), color, t.text);
                    dl->PopClipRect();
                };
                if (current.empty()) {
                    draw_text(dl, t.font, t.size, t.adv, ImVec2(left + pad, ink_top), t.color, t.text);
                } else {  // black/white inside the find_current fill, the token's colour outside
                    const ImU32 on_fill = contrast_text(theme.find_current);
                    float seg = left - 1000;
                    for (const FindHighlight& f : current) {
                        text_in(t.color, seg, f.x0);
                        text_in(on_fill, f.x0, f.x1);
                        seg = f.x1;
                    }
                    text_in(t.color, seg, left + t.w + 1000);
                }
                if (t.strike) {
                    const float sy = std::floor(base_y - t.asc * 0.32f);
                    dl->AddRectFilled(ImVec2(left, sy), ImVec2(left + t.w, sy + 1.0f), t.color);
                }
            }
            if (!t.link.empty()) {
                // A whole-pixel row (egui snaps its 1 px lines); an AA line at y.5-ish smears over two rows.
                const float uy = std::floor(base_y + 1.5f);
                dl->AddRectFilled(ImVec2(left, uy), ImVec2(left + t.w, uy + 1.0f), t.color);
                links.push_back({min, max, t.link, ""});
            }
        }
    }
}
