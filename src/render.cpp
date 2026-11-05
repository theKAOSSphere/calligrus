#include <cstdint>
#include "render.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>
#include <string_view>

#include "highlight.h"
#include "images.h"
#include "math.h"
#include "text_capture.h"

namespace fs = std::filesystem;

// Resolve an image reference relative to the open document.
fs::path resolve(const fs::path& base_dir, std::string src) {
    if (src.starts_with("file://")) src.erase(0, 7);
    for (size_t at; (at = src.find("%20")) != std::string::npos;) src.replace(at, 3, " ");
    fs::path p = fs::path(std::u8string(src.begin(), src.end()));
    return p.is_absolute() ? p : base_dir / p;
}

static bool g_export = false;
void render_set_export(bool on) { g_export = on; }

bool render_scroll_h(RenderCache& cache, const char* key, float dx, bool* hit_end) {
    const auto it = cache.code.find(key);
    if (it == cache.code.end() || it->second.overflow <= 0) return false;
    CodeScroll& s = it->second;
    const float want = s.offset - dx, got = std::clamp(want, 0.0f, s.overflow);
    if (hit_end) *hit_end = dx != 0 && got != want;
    s.offset = got;
    s.scrolled_at = ImGui::GetTime();
    return true;
}

namespace {

// Everything a block needs to draw itself. Passed by value into nested blocks so
// e.g. a quote can dim the colour for its children without affecting siblings.
struct Ctx {
    ImDrawList* dl;
    const Fonts& fonts;
    const Theme& theme;
    RenderResult& out;
    float origin_y;  // top of the document, to turn screen y into document offsets
    ImU32 color;     // body text colour (dimmed inside quotes)
    ImU32 bold, italic;  // bold / italic body text colour (theme doc.bold / doc.italic; quotes: see draw_quote)
    int depth;       // list nesting depth (picks the bullet shape)
    const fs::path& base_dir;  // the document's folder, for relative image paths
    Collector* sel;            // text selection: painted runs are reported here
    ImU32 hl;                  // selection highlight colour
    RenderCache& cache;        // code blocks' scroll state lives here
    const RenderOptions& opts;
    std::vector<SubRect>* sub = nullptr;  // top-level list / quote: records its items' / children's rects here
    size_t items_shown = SIZE_MAX;        // top-level list: draw only this many items (an incremental list)
    HAlign text_align = HAlign::Left;     // the top-level paragraph / heading being drawn (slides `alignment:`)

    // A length in points (the reading layout's units) as whole pixels at the
    // current DPI and zoom. Whole pixels keep textured quads on the pixel grid.
    float pt(float points) const { return std::round(points * fonts.scale); }
};

float draw_block(const Ctx& c, const md::Block& b, float x, float y, float width, bool tight);

// Base style for body text.
Base body_base(const Ctx& c) {
    return Base{.size = c.fonts.body, .color = c.color, .bold_color = c.bold, .italic_color = c.italic};
}

// Lay out a run of spans the same way everywhere (strut from the base font).
Layout layout_spans(const Ctx& c, const std::vector<Tok>& toks, const Base& base, float width,
                    HAlign align = HAlign::Left) {
    float asc, desc;
    font_metrics(c.fonts, c.fonts.face(base.bold, base.italic), base.size, asc, desc);
    return layout(toks, width, base.size * base.leading, {asc, desc}, align);
}

// Lay out and paint a run of spans; returns the height used. `selectable` text
// is a selection group of its own (list numbers aren't selectable).
float draw_spans(const Ctx& c, const md::Spans& spans, const Base& base, float x, float y, float width,
                 HAlign align = HAlign::Left, bool selectable = true, Sep sep = Sep::Para) {
    auto toks = fit(tokenize(spans, base, c.fonts, c.theme), width);
    Layout lay = layout_spans(c, toks, base, width, align);
    if (selectable && c.sel) {
        c.sel->begin_group(sep);
        paint(c.dl, ImVec2(x, y), lay, toks, c.theme, c.out.links, c.sel, c.hl);
    } else {
        paint(c.dl, ImVec2(x, y), lay, toks, c.theme, c.out.links);
    }
    return lay.height;
}

std::string plain_text(const md::Spans& spans) {
    std::string s;
    for (const auto& sp : spans) s += sp.text;
    return s;
}

void hrule(const Ctx& c, float x, float y, float width) {
    c.dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + 1), c.theme.rule);
}

// Heading sizes relative to body text, h1..h6 (same as render.rs).
constexpr float kHeadingScale[6] = {2.0f, 1.6f, 1.32f, 1.14f, 1.0f, 0.92f};

float draw_heading(const Ctx& c, const md::Block& b, float x, float y, float width) {
    const float body = c.fonts.body;
    const int level = std::clamp(b.level, 1, 6);
    y += body * (level <= 2 ? 0.9f : 0.6f);
    c.out.outline.push_back({level, plain_text(b.spans), y - c.origin_y});
    Base base{.size = body * kHeadingScale[level - 1],
              .bold = true,
              .color = c.theme.headings[level - 1],
              .leading = level <= 2 ? 1.25f : 1.4f};
    y += draw_spans(c, b.spans, base, x, y, width, c.text_align);
    if (level <= 2) {  // h1/h2 get an underline rule, like GitHub
        y += body * 0.25f;
        hrule(c, x, y, width);
        y += 1;
    }
    return y + body * 0.55f;
}

float draw_code(const Ctx& c, const md::Block& b, float x, float y, float width);

// A ```mermaid block: the diagram centred and shrunk to fit (like render.rs);
// on failure, the source as a code block plus an error line.
float draw_diagram(const Ctx& c, const md::Block& b, float x, float y, float width) {
    // Same scale as the Rust version (diagramcache.rs: 1.05 units -> points).
    if (const MathTex* d = diagram_get(b.text, 1.05f * c.fonts.scale)) {
        float fit = std::min(1.0f, width / d->px_w);
        float w = std::round(d->px_w * fit), h = std::round(d->px_h * fit);
        ImVec2 p0(std::round(x + (width - w) / 2), std::round(y));
        c.dl->AddImage(d->tex, p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(1, 1), c.color);
        c.out.links.push_back({p0, ImVec2(p0.x + w, p0.y + h), "", "", 2, b.text, ""});  // a click zooms it (spec 27)
        return y + h;
    }
    md::Block code = b;
    code.kind = md::Block::Kind::Code;
    y = draw_code(c, code, x, y, width);
    Base err{.size = c.fonts.body * 0.8f, .italic = true, .color = c.theme.error};
    return y + draw_spans(c, {md::Span{.text = "diagram error: could not draw this diagram"}}, err, x, y, width);
}

// One line of a code block, coloured by the syntax highlighter. `ls` is the
// line's byte offset in the whole block (spans are offsets into the block).
// Colours come from the theme's [syntax] table; plain text is doc.text (spec 6b;
// doc.code_fg is for inline code), dimmed inside quotes like other text.
void draw_code_line(const Ctx& c, ImVec2 at, float px, std::string_view line, size_t ls,
                    const std::vector<CodeSpan>& spans) {
    const float adv = c.fonts.advance_scale(c.fonts.mono, c.fonts.body * 0.88f);
    auto x_of = [&](size_t byte) { return at.x + text_width(c.fonts.mono, px, adv, line.substr(0, byte)); };
    auto draw = [&](size_t a, size_t b, ImU32 color, bool italic = false) {
        if (b <= a) return;
        const int first = c.dl->VtxBuffer.Size;
        text_capture::set_shear(italic ? 0.25f : 0.0f);  // (the PDF export shears the captured glyphs too)
        draw_text(c.dl, c.fonts.mono, px, adv, ImVec2(x_of(a), at.y), color, line.substr(a, b - a));
        text_capture::set_shear(0.0f);
        if (italic) {
            // Cascadia Code is used here without its italic face. Fake it the way egui does (text_layout.rs):
            // shear each glyph quad, moving its top edge right by a quarter of
            // the quad's height. AddText writes 4 vertices per glyph.
            ImDrawVert* v = c.dl->VtxBuffer.Data;
            for (int q = first; q + 3 < c.dl->VtxBuffer.Size; q += 4) {
                float bottom = std::max({v[q].pos.y, v[q + 1].pos.y, v[q + 2].pos.y, v[q + 3].pos.y});
                for (int k = q; k < q + 4; k++) v[k].pos.x += (bottom - v[k].pos.y) * 0.25f;
            }
        }
    };
    const ImU32 plain = c.color;
    const size_t le = ls + line.size();
    // Spans are sorted and don't overlap: jump to the first one ending inside this line.
    auto it = std::lower_bound(spans.begin(), spans.end(), ls, [](const CodeSpan& s, size_t v) { return s.end <= v; });
    size_t pos = 0;
    for (; it != spans.end() && it->start < le; ++it) {
        size_t a = std::max<size_t>(it->start, ls) - ls, e = std::min<size_t>(it->end, le) - ls;
        draw(pos, a, plain);
        if (const SyntaxStyle* s = c.theme.entry_style(it->entry)) draw(a, e, s->color, s->italic);
        else draw(a, e, plain);  // an entry this theme doesn't style
        pos = e;
    }
    draw(pos, line.size(), plain);
}

// A small text button drawn into the document (code blocks' "Copy").
// Returns true when clicked. `right` / `top`: its top-right corner.
bool doc_button(const Ctx& c, const char* label, float right, float top, bool& hovered) {
    const float size = c.fonts.px(c.fonts.regular, c.fonts.body * 0.7f);
    const float adv = c.fonts.advance_scale(c.fonts.regular, c.fonts.body * 0.7f);
    const ImVec2 text(text_width(c.fonts.regular, size, adv, label), size);
    const float padx = c.pt(5), pady = c.pt(2);
    const ImVec2 min(std::round(right - text.x - 2 * padx), std::round(top));
    const ImVec2 max(std::round(right), std::round(top + text.y + 2 * pady));
    const ImVec2 m = ImGui::GetMousePos();
    hovered = c.opts.interactive && c.opts.mouse_in_view && m.x >= min.x && m.x < max.x && m.y >= min.y && m.y < max.y;
    // Like an egui button: an opaque code_bg underlay, then the theme's widget fill (brighter on hover),
    // a 1 px outline only while hovered, text in the widget text colour.
    const bool dark = c.theme.dark;
    c.dl->AddRectFilled(min, max, c.theme.code_bg, c.pt(4));
    c.dl->AddRectFilled(min, max, hovered ? c.theme.ui.widget_hover : c.theme.ui.widget, c.pt(3));
    if (hovered) c.dl->AddRect(ImVec2(min.x + 0.5f, min.y + 0.5f), ImVec2(max.x - 0.5f, max.y - 0.5f), dark ? IM_COL32(150, 150, 150, 255) : IM_COL32(105, 105, 105, 255), c.pt(3));
    const ImU32 text_col = hovered ? (dark ? IM_COL32(240, 240, 240, 255) : IM_COL32(0, 0, 0, 255)) : c.theme.ui.text;
    draw_text(c.dl, c.fonts.regular, size, adv, ImVec2(min.x + padx, min.y + pady), text_col, label);
    return hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

// A fenced code block (spec 4): background with a 1 px border and rounded
// corners, the language label top-right, a Copy button while hovered, an
// optional line-number gutter, and long lines that scroll sideways (Shift +
// wheel, or a thin scrollbar at the bottom) instead of wrapping.
float draw_code(const Ctx& c, const md::Block& b, float x, float y, float width) {
    const float px = c.fonts.px(c.fonts.mono, c.fonts.body * 0.88f);
    const float line_h = px;  // The code font's ascent+descent == egui's row height
    // egui's Frame: inner margin 10 + the 1 px stroke, which counts as margin too.
    const float pad = c.pt(11);
    const float radius = c.pt(6);

    // Split into lines (ignoring a trailing newline).
    std::vector<std::string_view> lines;
    std::string_view text = b.text;
    while (!text.empty() && text.back() == '\n') text.remove_suffix(1);
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) nl = text.size();
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }

    // Line-number gutter: right-aligned numbers, then a gap of one character.
    const float adv = c.fonts.advance_scale(c.fonts.mono, c.fonts.body * 0.88f);
    const float ch_w = text_width(c.fonts.mono, px, adv, "0");
    const int digits = int(std::to_string(lines.size()).size());
    const float gutter = c.opts.line_numbers ? std::round(ch_w * (digits + 1)) : 0.0f;
    const float text_left = std::round(x + pad + gutter);
    const float view_w = std::max(1.0f, x + width - pad - text_left);  // width the text may use

    // Rows: one per line. In an export there is no scrolling: a line wider than the column is cut into
    // rows of the columns that fit; continuation rows are indented by 2 columns (where the arrow goes).
    struct Row {
        std::string_view text;  // (a part of the block's text: highlight spans still apply)
        bool cont = false;      // a continuation of the previous row's line
    };
    std::vector<Row> rows;
    const size_t cols = std::max<size_t>(size_t(view_w / ch_w), 4);
    for (std::string_view line : lines) {
        if (!g_export) {
            rows.push_back({line, false});
            continue;
        }
        std::string_view rest = line;
        bool cont = false;
        do {
            const size_t room = cont ? cols - 2 : cols;
            size_t n = 0, chars = 0;
            while (n < rest.size() && chars < room) {  // whole characters (UTF-8)
                n++;
                while (n < rest.size() && (static_cast<unsigned char>(rest[n]) & 0xC0) == 0x80) n++;
                chars++;
            }
            rows.push_back({rest.substr(0, n), cont});
            rest.remove_prefix(n);
            cont = true;
        } while (!rest.empty());
    }

    // Widest line -> how far the block can scroll (never, in an export).
    float content_w = 0;
    for (std::string_view line : lines)
        content_w = std::max(content_w, text_width(c.fonts.mono, px, adv, line));
    const float overflow = g_export ? 0.0f : std::max(0.0f, content_w - view_w);

    // A block that scrolls sideways gets 6 px more room under its text for the scrollbar (egui: floating_allocated_width).
    const float h = rows.size() * line_h + 2 * pad + (overflow > 0 ? c.pt(6) : 0.0f);
    const ImVec2 min(x, y), max(x + width, y + h);
    c.dl->AddRectFilled(min, max, c.theme.code_bg, radius);
    // 1 px border, on the pixel centres so it stays crisp.
    c.dl->AddRect(ImVec2(min.x + 0.5f, min.y + 0.5f), ImVec2(max.x - 0.5f, max.y - 0.5f), c.theme.code_border, radius);

    CodeScroll& state = c.cache.code[b.text.data()];
    const ImVec2 m = ImGui::GetMousePos();
    const bool hovered = c.opts.interactive && c.opts.mouse_in_view && m.x >= min.x && m.x < max.x && m.y >= min.y && m.y < max.y;
    ImGuiIO& io = ImGui::GetIO();
    state.overflow = overflow;
    if (overflow > 0 && c.opts.interactive) c.out.hblocks.push_back({min, max, b.text.data()});

    // Shift + wheel (or a sideways wheel / touchpad) over the block scrolls it.
    if (hovered && overflow > 0) {
        float wheel = io.MouseWheelH != 0 ? -io.MouseWheelH : (io.KeyShift ? io.MouseWheel : 0.0f);
        state.offset -= wheel * 40.0f * c.fonts.scale;  // egui's line_scroll_speed, like the main view (main.cpp kWheelNotchPoints)
    }

    // The scrollbar: a 4 px bar (6 px when hovered) along the bottom padding.
    const float track_l = x + pad, track_r = max.x - pad, track_w = track_r - track_l;
    const float thumb_w = overflow > 0 ? std::max(c.pt(20), track_w * view_w / content_w) : 0.0f;
    const float bar_bottom = max.y - pad;  // egui: the bar sits in the frame's inner margin edge
    const bool bar_hovered = overflow > 0 && hovered && m.y >= bar_bottom - c.pt(8);
    const bool dragging = c.cache.dragging == b.text.data();
    if (overflow > 0 && c.opts.interactive) {
        if (bar_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            // Grab the thumb where it was clicked (a click beside it centres it there).
            const float thumb_x = track_l + (track_w - thumb_w) * (state.offset / overflow);
            c.cache.dragging = b.text.data();
            c.cache.drag_grab = (m.x >= thumb_x && m.x < thumb_x + thumb_w) ? m.x - thumb_x : thumb_w / 2;
            c.out.mouse_captured = true;
        }
        if (dragging) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const float thumb_x = m.x - c.cache.drag_grab;
                state.offset = (thumb_x - track_l) / std::max(1.0f, track_w - thumb_w) * overflow;
                c.out.mouse_captured = true;
            } else {
                c.cache.dragging = nullptr;
            }
        }
    }
    state.offset = std::round(std::clamp(state.offset, 0.0f, overflow));  // whole pixels: crisp glyphs

    // The text, clipped to the block (and right of the gutter).
    // (egui's inner scroll area clips at the inner margin: code never shows in the padding.)
    const ImVec2 clip_min(text_left, min.y + 1), clip_max(max.x - pad, max.y - 1);
    c.dl->PushClipRect(clip_min, clip_max, true);
    const std::vector<CodeSpan>& spans = highlight_code(b.text, b.lang);
    // Rows far outside the window are not drawn (same pixels; a data file of a million lines overflowed the draw list and crashed).
    const float vis_top = c.dl->GetClipRectMin().y - 2 * line_h, vis_bottom = c.dl->GetClipRectMax().y + 2 * line_h;
    float ly = y + pad;
    for (size_t i = 0; i < rows.size(); i++) {
        std::string_view line = rows[i].text;
        ImVec2 at(text_left - state.offset + (rows[i].cont ? 2 * ch_w : 0.0f), std::round(ly));
        if (c.sel) {  // each code line is a selection group, copied with newlines between
            if (!rows[i].cont) c.sel->begin_group(i == 0 ? Sep::Para : Sep::Line);  // (a wrapped line's rows share one)
            c.sel->new_line();
            c.sel->next_glued = rows[i].cont;
            std::vector<float> xs = char_xs(c.fonts.mono, px, adv, std::string(line), at.x);
            ImVec2 box_min(at.x, ly), box_max(std::max(xs.back(), at.x + 1), ly + line_h);
            auto hl = c.sel->push(box_min, box_max, std::move(xs), std::string(line), false);
            draw_find_highlights(c.dl, *c.sel, box_min.y, box_max.y, c.theme);
            if (hl) c.dl->AddRectFilled(ImVec2(hl->first, box_min.y), ImVec2(hl->second, box_max.y), c.hl);
        }
        if (ly < vis_top || ly > vis_bottom) {
            ly += line_h;
            continue;
        }
        draw_code_line(c, at, px, line, size_t(line.data() - b.text.data()), spans);
        if (rows[i].cont) {  // the mark of a continuation row: a small arrow in the indent
            const float small = c.fonts.px(c.fonts.mono, c.fonts.body * 0.7f);
            const float small_adv = c.fonts.advance_scale(c.fonts.mono, c.fonts.body * 0.7f);
            draw_text(c.dl, c.fonts.mono, small, small_adv, ImVec2(text_left, std::round(ly + (px - small) / 2)), c.theme.weak, "\xE2\x86\xAA");
        }
        ly += line_h;
    }
    c.dl->PopClipRect();
    // Where each code line is (the editor's Ctrl+click -> source line).
    c.cache.rows_now.clear();
    for (size_t i = 0; i < rows.size(); i++) c.cache.rows_now.push_back({y + pad + float(i) * line_h - c.origin_y, line_h});

    // Line numbers (not part of the selection, so never copied).
    if (gutter > 0) {
        size_t number = 0;
        for (size_t i = 0; i < rows.size(); i++) {
            if (rows[i].cont) continue;  // (a wrapped line's other rows have no number)
            const float ny = y + pad + i * line_h;
            if (ny < vis_top || ny > vis_bottom) { ++number; continue; }
            const std::string n = std::to_string(++number);
            const float w = text_width(c.fonts.mono, px, adv, n);
            draw_text(c.dl, c.fonts.mono, px, adv, ImVec2(std::round(text_left - ch_w - w), std::round(y + pad + i * line_h)),
                      c.theme.weak, n);
        }
    }

    if (overflow > 0) {
        const float t = (bar_hovered || dragging) ? c.pt(6) : c.pt(4);
        const float thumb_x = std::round(track_l + (track_w - thumb_w) * (state.offset / overflow));
        // egui's handle: widget text colour at 60 % (idle), 80 % (pointer over the block), solid black/white while the bar is used.
        ImU32 col = c.theme.ui.text;
        int alpha = hovered ? 204 : 153;
        if (bar_hovered || dragging) {
            col = c.theme.dark ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
            alpha = 255;
        }
        col = (col & ~IM_COL32_A_MASK) | (ImU32(alpha) << IM_COL32_A_SHIFT);
        c.dl->AddRectFilled(ImVec2(thumb_x, bar_bottom - t), ImVec2(thumb_x + thumb_w, bar_bottom), col, t / 2);
    }
    if (bar_hovered || dragging) c.out.over_code_control = true;

    // Language tag in the top-right corner (italic, small, weak); the Copy
    // button appears left of it while the mouse is over the block.
    float label_left = max.x - c.pt(8);
    if (!b.lang.empty()) {
        float small = c.fonts.px(c.fonts.italic, c.fonts.body * 0.7f);
        const float small_adv = c.fonts.advance_scale(c.fonts.italic, c.fonts.body * 0.7f);
        float w = text_width(c.fonts.italic, small, small_adv, b.lang);
        label_left = std::round(max.x - w - c.pt(8));
        // On an opaque pill so it stays readable over code scrolling under it.
        const float lp = c.pt(4);  // pill padding
        const ImVec2 tp(label_left, std::round(y + c.pt(3)));
        c.dl->AddRectFilled(ImVec2(tp.x - lp, tp.y), ImVec2(tp.x + w + lp, tp.y + small * 1.25f), c.theme.code_bg, c.pt(4));
        draw_text(c.dl, c.fonts.italic, small, small_adv, tp, c.theme.weak, b.lang);
        label_left -= lp + c.pt(6);
    }
    const double now = ImGui::GetTime();
    const bool copied = now - state.copied_at < 1.5;
    if (hovered || copied) {
        bool over = false;
        if (doc_button(c, copied ? "Copied" : "Copy", label_left, y + c.pt(3), over)) {
            ImGui::SetClipboardText(std::string(text).c_str());  // the block's source (no trailing newline)
            state.copied_at = now;
        }
        if (over) {
            c.out.over_code_control = true;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) c.out.mouse_captured = true;
        }
    }
    if (copied) c.out.animating = true;  // redraw to switch back to "Copy"
    return y + h;
}

float draw_list(const Ctx& c, const md::Block& b, float x, float y, float width) {
    const float body = c.fonts.body;
    Base base = body_base(c);

    // Marker column: wide enough for the longest number; fixed for bullets.
    float marker_w = body * 1.45f;
    if (b.ordered) {
        float widest = 0;
        for (size_t i = 0; i < b.items.size(); i++) {
            auto toks = tokenize({md::Span{.text = std::to_string(b.start + i) + "."}}, base, c.fonts, c.theme);
            widest = std::max(widest, measure(toks).first);
        }
        marker_w = std::max(widest + body * 0.6f, body * 1.3f);
    }
    // Baseline of an item's first line, so markers sit on it.
    float asc, desc;
    font_metrics(c.fonts, c.fonts.regular, body, asc, desc);
    const float first_baseline = (body * base.leading - (asc + desc)) / 2.0f + asc;

    // egui's item frame takes its left margin as a whole number of points (`marker_w as i8`).
    const float indent = std::floor(marker_w / c.fonts.scale) * c.fonts.scale;
    Ctx inner = c;
    inner.depth = c.depth + 1;
    inner.sub = nullptr;
    inner.items_shown = SIZE_MAX;
    unsigned n = b.start;
    for (size_t k = 0; k < b.items.size() && k < c.items_shown; k++) {
        const auto& item = b.items[k];
        const float item_top = y;
        if (b.ordered) {
            md::Span num{.text = std::to_string(n++) + "."};
            draw_spans(c, {num}, base, x, y, marker_w - body * 0.25f, HAlign::Right, false);
        } else {
            // Bullets are shapes (fonts disagree on bullet glyphs): ● ○ ■ by depth.
            ImVec2 ctr(x + marker_w - body * 0.72f, y + first_baseline - body * 0.3f);
            float r = body * 0.15f;
            switch (c.depth % 3) {
                case 0: c.dl->AddCircleFilled(ctr, r, c.color); break;
                // (+0.5: ImGui draws a circle's outline half a pixel inside `radius`, egui centres it on it)
                case 1: c.dl->AddCircle(ctr, r * 0.9f + 0.5f, c.color, 0, 1.3f); break;
                default:
                    c.dl->AddRectFilled(ImVec2(ctr.x - r * 0.85f, ctr.y - r * 0.85f),
                                        ImVec2(ctr.x + r * 0.85f, ctr.y + r * 0.85f), c.color);
            }
        }
        for (const auto& child : item) y = draw_block(inner, child, x + indent, y, width - indent, true);
        // The item's rect ends before the gap to the next one (as in render.rs: the editor marker bar spans only the item).
        if (c.sub) c.sub->push_back({item_top - c.origin_y, y - item_top, std::move(c.cache.rows_now)});
        c.cache.rows_now.clear();
        y += body * 0.15f;
    }
    return y;
}

// GitHub callout colours (dark theme / light theme) and the title word.
ImU32 callout_color(const std::string& kind, bool dark) {
    struct K { const char* name; ImU32 dark, light; };
    static const K kinds[] = {
        {"note", IM_COL32(0x44, 0x93, 0xf8, 255), IM_COL32(0x09, 0x69, 0xda, 255)},
        {"tip", IM_COL32(0x3f, 0xb9, 0x50, 255), IM_COL32(0x1a, 0x7f, 0x37, 255)},
        {"important", IM_COL32(0xab, 0x7d, 0xf8, 255), IM_COL32(0x82, 0x50, 0xdf, 255)},
        {"warning", IM_COL32(0xd2, 0x99, 0x22, 255), IM_COL32(0x9a, 0x67, 0x00, 255)},
        {"caution", IM_COL32(0xf8, 0x51, 0x49, 255), IM_COL32(0xcf, 0x22, 0x2e, 255)},
    };
    for (const K& k : kinds)
        if (kind == k.name) return dark ? k.dark : k.light;
    return 0;
}

float draw_quote(const Ctx& c, const md::Block& b, float x, float y, float width) {
    Ctx inner = c;
    inner.sub = nullptr;
    // Quote text in doc.quote; bold/italic that the theme left at `text` follow it.
    inner.color = c.theme.quote;
    if (c.theme.bold == c.theme.text) inner.bold = c.theme.quote;
    if (c.theme.italic == c.theme.text) inner.italic = c.theme.quote;
    const float indent = c.pt(18);
    const float gap = c.fonts.body * 0.9f;
    float end = y;
    ImU32 bar = c.theme.quote_bar;
    if (!b.callout.empty()) {  // title line: the kind word in bold, in the kind's colour
        bar = callout_color(b.callout, c.theme.dark);
        std::string title = b.callout;
        title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
        Base tb{.size = c.fonts.body, .bold = true, .color = bar};
        end += draw_spans(c, {md::Span{.text = title}}, tb, x + indent, end, width - indent) + gap * 0.3f;
        if (c.sub) c.sub->push_back({y - c.origin_y, end - y, {}});
    }
    for (const auto& child : b.children) {
        const float child_top = end;
        end = draw_block(inner, child, x + indent, end, width - indent, false);
        if (c.sub) c.sub->push_back({child_top - c.origin_y, end - child_top, std::move(c.cache.rows_now)});
        c.cache.rows_now.clear();
    }
    const float bar_w = b.callout.empty() ? c.pt(3) : c.pt(4);
    c.dl->AddRectFilled(ImVec2(x + c.pt(2), y), ImVec2(x + c.pt(2) + bar_w, end - gap * 0.5f), bar, bar_w / 2);
    return end + gap * 0.2f;
}

float draw_table(const Ctx& c, const md::Block& b, float x, float y, float width) {
    size_t ncols = b.header.size();
    for (const auto& r : b.rows) ncols = std::max(ncols, r.size());
    if (ncols == 0) return y;
    const float hpad = c.pt(10), vpad = c.pt(6);
    const Base regular = body_base(c);
    Base bold = regular;
    bold.bold = true;

    // Tokenise every cell once (header first).
    std::vector<std::vector<std::vector<Tok>>> cells;
    auto add_row = [&](const std::vector<md::Spans>& row, const Base& base) {
        std::vector<std::vector<Tok>> r(ncols);
        for (size_t i = 0; i < ncols; i++) r[i] = tokenize(i < row.size() ? row[i] : md::Spans{}, base, c.fonts, c.theme);
        cells.push_back(std::move(r));
    };
    const bool has_head = !b.header.empty();  // (Org tables may have none: no band, no bold row)
    if (has_head) add_row(b.header, bold);
    for (const auto& r : b.rows) add_row(r, regular);

    // Column widths: natural if they fit, otherwise share the squeeze by flexibility.
    std::vector<float> nat(ncols, 0), minw(ncols, 0), w(ncols);
    for (const auto& row : cells)
        for (size_t i = 0; i < ncols; i++) {
            auto [n, m] = measure(row[i]);
            nat[i] = std::max(nat[i], n + 2 * hpad);
            minw[i] = std::max(minw[i], m + 2 * hpad);
        }
    float sum_nat = 0, sum_min = 0;
    for (size_t i = 0; i < ncols; i++) sum_nat += nat[i], sum_min += minw[i];
    for (size_t i = 0; i < ncols; i++) {
        if (sum_nat <= width) w[i] = nat[i];
        else if (sum_min >= width) w[i] = minw[i];
        else w[i] = minw[i] + (width - sum_min) * (nat[i] - minw[i]) / (sum_nat - sum_min);
    }
    float total_w = 0;
    for (float v : w) total_w += v;

    auto align_of = [&](size_t i) {
        if (i >= b.aligns.size()) return HAlign::Left;
        if (b.aligns[i] == md::Align::Center) return HAlign::Center;
        if (b.aligns[i] == md::Align::Right) return HAlign::Right;
        return HAlign::Left;
    };

    // Lay out each cell at its column width; rows are as tall as their tallest cell.
    Base cell_base = regular;
    cell_base.leading = 1.45f;
    std::vector<std::vector<Layout>> lays;
    std::vector<float> row_h;
    for (auto& row : cells) {
        std::vector<Layout> lr;
        float h = 0;
        for (size_t i = 0; i < ncols; i++) {
            row[i] = fit(std::move(row[i]), w[i] - 2 * hpad);
            lr.push_back(layout_spans(c, row[i], cell_base, w[i] - 2 * hpad, align_of(i)));
            h = std::max(h, lr.back().height);
        }
        lays.push_back(std::move(lr));
        row_h.push_back(h + 2 * vpad);
    }

    // Where each row is (the editor's Ctrl+click -> source line).
    c.cache.rows_now.clear();
    {
        float row_y = y;
        for (const float h : row_h) {
            c.cache.rows_now.push_back({row_y - c.origin_y, h});
            row_y += h;
        }
    }

    // Backgrounds, cells, grid. Row/column edges are floored to whole pixels, as egui snaps its
    // 1 px lines: a line at a fractional position would be smeared over two pixel rows.
    float ry = y;
    for (size_t r = 0; r < cells.size(); r++) {
        const float top = std::floor(ry), bottom = std::floor(ry + row_h[r]);
        if (has_head && r == 0) c.dl->AddRectFilled(ImVec2(x, top), ImVec2(x + total_w, bottom), c.theme.table_head_bg);
        else if ((r + (has_head ? 0 : 1)) % 2 == 0) c.dl->AddRectFilled(ImVec2(x, top), ImVec2(x + total_w, bottom), c.theme.zebra);
        float cx = x;
        for (size_t i = 0; i < ncols; i++) {
            // Cells copy as tab-separated values, rows as lines.
            if (c.sel) c.sel->begin_group(r == 0 && i == 0 ? Sep::Para : i == 0 ? Sep::Row : Sep::Cell);
            paint(c.dl, ImVec2(cx + hpad, ry + vpad), lays[r][i], cells[r][i], c.theme, c.out.links, c.sel, c.hl);
            cx += w[i];
        }
        ry += row_h[r];
    }
    auto hline = [&](float ly) { c.dl->AddRectFilled(ImVec2(x, std::floor(ly)), ImVec2(x + total_w, std::floor(ly) + 1), c.theme.rule); };
    auto vline = [&](float lx) { c.dl->AddRectFilled(ImVec2(std::floor(lx), std::floor(y)), ImVec2(std::floor(lx) + 1, std::floor(ry) + 1), c.theme.rule); };
    hline(y);
    hline(ry);   // (egui's Inside stroke: the last line is the one containing the bottom edge)
    vline(x);
    vline(x + total_w);
    float ly = y;
    for (size_t r = 0; r + 1 < row_h.size(); r++) {
        ly += row_h[r];
        hline(ly);
    }
    float lx = x;
    for (size_t i = 0; i + 1 < ncols; i++) {
        lx += w[i];
        vline(lx);
    }
    return ry;
}

float draw_math(const Ctx& c, const md::Block& b, float x, float y, float width) {
    // Display math: centred, display style, 1.2x body (like render.rs).
    const float size = c.fonts.body * 1.2f;
    const float pad = size * 0.35f;
    if (const MathTex* m = math_get(b.text, true, size * 1.1f)) {
        // The formula is centred; a tag (	ag{1}) sits flush right on its baseline. If the two would touch (less than `gap`
        // between them), a centred formula moves left to keep the gap; if even that does not fit the column, the formula and
        // its tag are one wide line that scrolls sideways (a finger drag on Android, Shift + wheel), with the gap between them.
        const float gap = m->tag ? size : 0.0f;
        const float tag_w = m->tag ? m->tag->w : 0.0f;
        const float content_w = m->w + (m->tag ? gap + tag_w : 0.0f);
        const bool scrolls = !g_export && content_w > width;
        CodeScroll* state = nullptr;
        float shift = 0;  // how far the scrolled line has moved left
        float left = std::round(x + std::max(0.0f, (width - m->w) / 2.0f));
        if (m->tag && !scrolls && left + m->w + gap > x + width - tag_w) left = std::round(x + width - tag_w - gap - m->w);
        if (scrolls) {
            state = &c.cache.code[b.text.data()];
            state->overflow = content_w - width;
            if (c.opts.interactive) c.out.hblocks.push_back({ImVec2(x, y), ImVec2(x + width, y + m->asc + m->desc + 2 * pad), b.text.data()});
            const ImVec2 mp = ImGui::GetMousePos();
            const ImGuiIO& io = ImGui::GetIO();
            const bool over = c.opts.interactive && c.opts.mouse_in_view && mp.x >= x && mp.x < x + width && mp.y >= y && mp.y < y + m->asc + m->desc + 2 * pad;
            if (over) {  // Shift + wheel, or a sideways wheel, as over a code block
                const float wheel = io.MouseWheelH != 0 ? -io.MouseWheelH : (io.KeyShift ? io.MouseWheel : 0.0f);
                if (wheel != 0) state->offset -= wheel * 40.0f * c.fonts.scale, state->scrolled_at = ImGui::GetTime();
            }
            state->offset = std::round(std::clamp(state->offset, 0.0f, state->overflow));
            shift = state->offset;
            left = std::round(x - shift);
        }
        float base_y = std::round(y + pad + m->asc);
        const float block_h = m->asc + m->desc + 2 * pad;
        if (scrolls) c.dl->PushClipRect(ImVec2(x, y), ImVec2(x + width, y + block_h), true);
        if (c.sel) {  // selectable as a whole: copies the TeX source
            c.sel->begin_group(Sep::Para);
            c.sel->new_line();
            ImVec2 box_min(left, y + pad), box_max(left + m->w, y + pad + m->asc + m->desc);
            c.sel->math(box_min, box_max, "$$" + b.text + "$$");
            if (c.sel->push(box_min, box_max, {left, left + m->w}, "$$" + b.text + "$$", false))
                c.dl->AddRectFilled(box_min, box_max, c.hl, 4.0f);
        }
        // Whole-pixel corner (like mathcache.rs): the bitmap's origin offsets are
        // fractional, and an off-grid quad gets blurred by texture filtering.
        ImVec2 p0(std::round(left - m->ox), std::round(base_y - m->oy));
        math_draw(c.dl, *m, p0, c.color);
        if (const MathTex* t = m->tag) {  // 	ag{..}: flush right on the formula's baseline (like render.rs)
            const float tx = scrolls ? left + m->w + gap : x + width - t->w;
            ImVec2 q(std::round(tx - t->ox), std::round(base_y - t->oy));
            math_draw(c.dl, *t, q, c.color);
        }
        if (scrolls) {
            c.dl->PopClipRect();
            // A thin bar (as under a code block) while the formula has just been scrolled.
            const double age = ImGui::GetTime() - state->scrolled_at;
            if (age < 1.2) {
                const float track_l = x + pad, track_w = width - 2 * pad;
                const float thumb_w = std::max(c.pt(20), track_w * width / content_w);
                const float thumb_x = std::round(track_l + (track_w - thumb_w) * (state->offset / state->overflow));
                const float bar_bottom = y + block_h - c.pt(2);
                int alpha = age < 0.8 ? 153 : int(153 * (1.2 - age) / 0.4);
                ImU32 col = (c.theme.ui.text & ~IM_COL32_A_MASK) | (ImU32(alpha) << IM_COL32_A_SHIFT);
                c.dl->AddRectFilled(ImVec2(thumb_x, bar_bottom - c.pt(4)), ImVec2(thumb_x + thumb_w, bar_bottom), col, c.pt(2));
                c.out.animating = true;
            }
        }
        return y + block_h;
    }
    Base base{.size = c.fonts.body, .color = c.theme.code_fg};
    return y + draw_spans(c, {md::Span{.text = "$$" + b.text + "$$", .code = true}}, base, x, y, width);
}


// A row of pictures that wraps like text (same rules as render.rs): natural
// size shrunk to fit the column, an explicit width="..." wins. Remote images
// aren't fetched: their alt text is shown as a link instead.
float draw_images(const Ctx& c, const md::Block& b, float x, float y, float width) {
    const float gap = 8.0f * c.fonts.scale;
    float cx = x, row_h = 0;
    // Next free spot for an item of size w x h, wrapping to a new row if needed.
    auto place = [&](float w, float h) {
        if (cx > x && cx + w > x + width) {
            y += row_h + gap;
            cx = x;
            row_h = 0;
        }
        ImVec2 p(std::round(cx), std::round(y));
        cx += w + gap;
        row_h = std::max(row_h, h);
        return p;
    };
    // A one-line label (remote image / load failure) placed like a picture.
    // Drawn like the Rust version's egui widget: 13pt, coloured, not underlined.
    auto label = [&](const std::string& text, ImU32 color, const std::string& url, const std::string& tip) {
        Base base = body_base(c);
        base.size = 13.0f * c.fonts.scale;
        base.color = color;
        auto toks = tokenize({md::Span{.text = text}}, base, c.fonts, c.theme);
        float w = measure(toks).first + 1;
        Layout lay = layout_spans(c, toks, base, w);
        ImVec2 p = place(w, lay.height);
        std::vector<LinkHit> hits;
        paint(c.dl, p, lay, toks, c.theme, hits);
        c.out.links.push_back({p, ImVec2(p.x + w, p.y + lay.height), url, tip});
    };

    for (const md::ImageRef& im : b.images) {
        const std::string name = im.alt.empty() ? im.src : im.alt;
        bool remote = im.src.starts_with("http://") || im.src.starts_with("https://") || im.src.starts_with("data:");
        if (remote) {
            std::string target = im.link.empty() ? im.src : im.link;
            label("\xF0\x9F\x96\xBC " + name, c.theme.link, target, target);  // "🖼 alt"
            continue;
        }
        fs::path path = resolve(c.base_dir, im.src);
        const LoadedImage* img = image_get(path);
        if (!img) {
            std::u8string u8 = path.u8string();
            label("\xE2\x9A\xA0 " + name, c.theme.error, "",
                  "could not load " + std::string(u8.begin(), u8.end()));  // "⚠ alt"
            continue;
        }
        // 1 image pixel = 1 point, like egui; then shrink to the column.
        // Whole pixels, so the picture's corners land on the pixel grid.
        float w = std::round(std::min((im.width > 0 ? im.width : img->w) * c.fonts.scale, width));
        float h = std::round(img->h * (w / img->w));
        ImVec2 p = place(w, h);
        ImVec2 q(p.x + w, p.y + h);
        c.dl->AddImage(img->tex, p, q, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE);  // square corners, like the Rust port
        LinkHit hit{p, q, im.link, im.alt.empty() ? im.src : im.alt};
        if (im.link.empty()) {  // a click zooms it (spec 27)
            const std::u8string u8 = path.u8string();
            hit.zoom = 1, hit.zoom_src.assign(u8.begin(), u8.end()), hit.zoom_alt = im.alt;
        }
        c.out.links.push_back(std::move(hit));
    }
    return y + row_h;
}

// One block plus the gap after it (`tight` = inside a list item: smaller gaps).
float draw_block(const Ctx& c, const md::Block& b, float x, float y, float width, bool tight) {
    using K = md::Block::Kind;
    c.cache.rows_now.clear();  // (only the block drawn last may leave rows behind)
    const float gap = c.fonts.body * (tight ? 0.3f : 0.9f);
    switch (b.kind) {
        case K::Heading: return draw_heading(c, b, x, y, width);
        case K::Paragraph:  // tight paragraphs (list items) copy one per line, others with a blank line
            return y + draw_spans(c, b.spans, body_base(c), x, y, width, c.text_align, true, tight ? Sep::Line : Sep::Para) + gap;
        case K::Code:
            return (b.lang == "mermaid" ? draw_diagram(c, b, x, y, width) : draw_code(c, b, x, y, width)) + gap;
        case K::Math: return draw_math(c, b, x, y, width) + gap * 0.5f;
        case K::Quote: return draw_quote(c, b, x, y, width);
        case K::List: return draw_list(c, b, x, y, width) + (c.depth == 0 ? gap : 0.0f);
        case K::Table: return draw_table(c, b, x, y, width) + gap;
        case K::Images: return draw_images(c, b, x, y, width) + gap;
        case K::Rule:
            y += gap * 0.4f;
            hrule(c, x, y, width);
            return y + 1 + gap * 0.9f;
    }
    return y;
}

}  // namespace

RenderResult render_document(ImDrawList* dl, const md::Document& doc, const Fonts& fonts, const Theme& theme,
                             ImVec2 origin, float width, RenderCache& cache, size_t version,
                             float view_top, float view_bottom, const fs::path& base_dir,
                             Collector& sel, std::optional<TextRange> sel_range, ImU32 highlight,
                             const RenderOptions& opts) {
    RenderResult out;
    Ctx c{dl, fonts, theme, out, origin.y, theme.text, theme.bold, theme.italic, 0, base_dir, &sel, highlight, cache, opts};
    sel.begin_frame(sel_range, opts.find, opts.find_current);
    if (cache.code_version != version) {  // a new document: forget code blocks' scroll state
        cache.code.clear();
        cache.dragging = nullptr;
        cache.code_version = version;
    }

    // Cached heights are only valid for one document / column width / text size.
    // (Column layouts change every block's width: they get their own key, like main.cpp's layout_key.)
    const std::vector<slides::Placement>* columns = opts.columns && opts.columns->size() == doc.size() ? opts.columns : nullptr;
    size_t key = std::hash<size_t>{}(version) ^ (std::hash<float>{}(width) << 1) ^ (std::hash<float>{}(fonts.body) << 2) ^
                 (columns ? kColumnsKey : 0) ^ (opts.styles ? kStylesKey : 0) ^ (opts.backgrounds ? kBackgroundsKey : 0);
    if (key != cache.key || cache.height.size() != doc.size()) {
        cache.key = key;
        cache.height.assign(doc.size(), NAN);
        cache.head_dy.assign(doc.size(), 0.0f);
        cache.sub.assign(doc.size(), {});
        cache.rows.assign(doc.size(), {});
    }
    cache.top.resize(doc.size());
    cache.space.assign(doc.size(), 0.0f);
    cache.hit_x.assign(doc.size(), {-FLT_MAX, FLT_MAX});

    const float margin = opts.margin;  // draw a little beyond the visible area
    const float gap = c.pt(32);        // between columns
    float y = origin.y;                // top of the next full-width block
    // The column layout (row) being filled: its id, each column's next y, and its bottom so far.
    int row = -1;
    std::vector<float> col_y;
    float row_bottom = 0;
    for (size_t i = 0; i < doc.size(); i++) {
        const md::Block& b = doc[i];
        const slides::Placement* p = columns && (*columns)[i].row >= 0 ? &(*columns)[i] : nullptr;
        if (row >= 0 && (!p || p->row != row)) y = row_bottom, row = -1;  // the row ends: continue below it
        if (p && row < 0) row = p->row, col_y.assign(p->weights.size(), y), row_bottom = y;
        // Slides `new_lines:` (full-width blocks): empty body lines above the block.
        const slides::TextStyle style = opts.styles && i < opts.styles->size() ? (*opts.styles)[i] : slides::TextStyle{};
        if (!p && style.lines_before > 0) {
            cache.space[i] = std::round(float(style.lines_before) * fonts.body * 1.55f);
            y += cache.space[i];
        }
        float bx = origin.x, bw = width, by = y;
        if (p) {
            // Column i is (width - gaps) * w_i / sum(w) wide; whole-pixel edges.
            const float avail = width - gap * float(p->weights.size() - 1);
            float sum = 0, before = 0;
            for (size_t k = 0; k < p->weights.size(); k++) {
                sum += float(p->weights[k]);
                if (int(k) < p->column) before += float(p->weights[k]);
            }
            const float left = std::round(avail * before / sum + gap * float(p->column));
            const float right = std::round(avail * (before + float(p->weights[size_t(p->column)])) / sum + gap * float(p->column));
            bx = origin.x + left, bw = right - left, by = col_y[size_t(p->column)];
            cache.hit_x[i] = {p->column == 0 ? -FLT_MAX : left - gap / 2,
                              p->column + 1 == int(p->weights.size()) ? FLT_MAX : right + gap / 2};
        }
        cache.top[i] = by - origin.y;
        if (opts.backgrounds && i < opts.backgrounds->size() && (*opts.backgrounds)[i]) {
            cache.height[i] = 0;  // a background picture: drawn behind the slide by main.cpp
            continue;
        }
        float h = cache.height[i];
        // Blocks inside the selection are always laid out, so copying gets all their text.
        bool in_sel = sel_range && i >= sel_range->first.block && i <= sel_range->second.block;
        // An incremental list's first items: drawn, the rest keeps its (measured) space.
        const bool partial = i == opts.hide_from && opts.hide_items > 0 && !std::isnan(h);
        const bool hidden = i >= opts.hide_from && !std::isnan(h) && !partial;
        float end;
        if (hidden || (!in_sel && !std::isnan(h) && (by + h < view_top - margin || by > view_bottom + margin))) {
            // Off-screen (or hidden by a pause) and measured: skip it, but keep headings in the outline.
            if (b.kind == md::Block::Kind::Heading)
                out.outline.push_back({b.level, plain_text(b.spans), by + cache.head_dy[i] - origin.y});
            end = by + h;
        } else {
            sel.begin_block(i);
            cache.sub[i].clear();
            c.sub = &cache.sub[i];
            c.items_shown = partial ? opts.hide_items : SIZE_MAX;
            // Slides `alignment:` (paragraphs and headings) and `font_size:` (the text size, times n).
            const bool text = b.kind == md::Block::Kind::Paragraph || b.kind == md::Block::Kind::Heading;
            c.text_align = !text ? HAlign::Left : style.align == slides::TextAlign::Center ? HAlign::Center
                         : style.align == slides::TextAlign::Right                     ? HAlign::Right
                                                                                         : HAlign::Left;
            if (style.font_size > 1) {
                Fonts big = fonts;
                big.body = fonts.body * float(style.font_size);
                Ctx cb{dl, big, theme, out, origin.y, theme.text, theme.bold, theme.italic, 0, base_dir, &sel, highlight, cache, opts};
                cb.sub = c.sub, cb.items_shown = c.items_shown, cb.text_align = c.text_align;
                end = draw_block(cb, b, bx, by, bw, false);
            } else {
                end = draw_block(c, b, bx, by, bw, false);
            }
            cache.rows[i] = std::move(cache.rows_now);
            cache.rows_now.clear();
            if (partial) end = by + h;
            else cache.height[i] = end - by;
            if (b.kind == md::Block::Kind::Heading) cache.head_dy[i] = out.outline.back().y + origin.y - by;
        }
        if (p) col_y[size_t(p->column)] = end, row_bottom = std::max(row_bottom, end);
        else y = end;
    }
    if (row >= 0) y = row_bottom;
    out.height = y - origin.y + std::round(64.0f * fonts.scale);
    return out;
}
