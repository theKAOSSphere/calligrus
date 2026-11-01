// Inline text layout: turns a paragraph's styled spans into wrapped lines.
//
// Dear ImGui has no rich text: ImGui::TextWrapped wraps one string in one font.
// A paragraph like "some **bold** and `code`" mixes fonts, so we do our own
// line breaking (the same approach as the Rust port):
//
//   spans --tokenize()--> [Tok]   (words, spaces, forced breaks; each measured)
//   [Tok] --layout()----> Layout  (which token goes on which line, at what x)
//   Layout --paint()----> ImDrawList calls (+ clickable link rectangles)
//
// All sizes here are in em-pixels, matching the Rust version.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>

#include "fonts.h"
#include "model.h"
#include "select.h"
#include "theme.h"  // colours: Theme


// Starting style for a run of spans (headings are bold, quotes dimmed...).
struct Base {
    float size;               // em-pixels
    bool bold = false;
    bool italic = false;
    ImU32 color = 0;
    float leading = 1.55f;    // line pitch as a multiple of `size`
    // Colours for bold / italic spans in plain body text (theme doc.bold / doc.italic);
    // 0: keep `color` (headings, labels, code...).
    ImU32 bold_color = 0, italic_color = 0;
};

enum class HAlign { Left, Center, Right };

struct Tok {
    enum class Kind { Word, Space, Break };
    Kind kind = Kind::Word;
    std::string text;
    ImFont* font = nullptr;
    float size = 0;           // ImGui font size for `font` (see Fonts::px)
    float adv = 1.0f;         // advance scale (Fonts::advance_scale): exact em size / rounded size
    float w = 0, asc = 0, desc = 0;  // asc/desc: egui's line-box metrics (layout)
    float ink_asc = 0;        // ImGui's own ascent at `size`: where AddText must put the top so the baseline lands on ours
    ImU32 color = 0;
    bool code = false;        // draw a rounded background behind it
    float pad = 0;            // code: horizontal padding inside that background
    bool strike = false;
    float raise = 0;          // footnote marks: baseline raised by this many pixels
    std::string link;
    bool glue = false;        // no whitespace before: must stay on the previous token's line
    std::vector<float> cuts;  // where a too-long word may be cut (after / \ _ - . :): x of each cut inside the text
    bool cont = false, more = false;  // a piece of a cut word (see fit): continues the previous piece / is continued
    const struct MathTex* math = nullptr;  // set: draw this rendered formula instead of text
                                           // (`text` then holds "$tex$", what copying gives)
};

struct Line {
    std::vector<std::pair<size_t, float>> items;  // (token index, x from line start)
    float width = 0;
    float baseline = 0;                           // y of the baseline from the layout's top
    float top = 0, bottom = 0;                    // line box (selection highlights fill it)
    bool hard = false;                            // starts after a hard line break
};

struct Layout {
    std::vector<Line> lines;
    float height = 0;
};

// A hover area: clickable if `url` is set; shows `tip` (or the url) as a tooltip. Without a url, a
// picture or diagram is zoomed by a click (spec 27): `zoom` 1 = the picture file `zoom_src` (UTF-8
// path, `zoom_alt` its caption), 2 = the diagram whose source is `zoom_src`.
struct LinkHit {
    ImVec2 min, max;
    std::string url;
    std::string tip;
    int zoom = 0;
    std::string zoom_src, zoom_alt;
};

// Ascent/descent (pixels) of a Plex face at `em` pixels per em.
void font_metrics(const Fonts& fonts, ImFont* font, float em, float& asc, float& desc);

std::vector<Tok> tokenize(const md::Spans& spans, const Base& base, const Fonts& fonts, const Theme& theme);

// Width if nothing wrapped, and width of the widest unbreakable chunk.
std::pair<float, float> measure(const std::vector<Tok>& toks);

// Cut the words of chunks wider than `width` into pieces `layout` can wrap: after / \ _ - . :, and
// per character where even such a piece is too wide. Chunks that fit are untouched. Call with the
// width `layout` will get.
std::vector<Tok> fit(std::vector<Tok> toks, float width);

// Greedy line breaking into `width` pixels. Every line is at least `line_h` tall
// and at least as tall as `strut` (ascent, descent) of the base font.
Layout layout(const std::vector<Tok>& toks, float width, float line_h, std::pair<float, float> strut,
              HAlign align = HAlign::Left);

// x of each character boundary of `text` drawn with `font` at ImGui size `size`
// starting at x0 (the advances draw_text uses).
std::vector<float> char_xs(ImFont* font, float size, float adv, const std::string& text, float x0);

// Width of `s` with the advances of `font` at ImGui size `size`, each times `adv`.
float text_width(ImFont* font, float size, float adv, std::string_view s);

// Draw `s` glyph by glyph: x advances exactly (like egui) and each glyph lands on a whole pixel
// (like egui's glyph cache), so text is as crisp as before but as wide as in the Rust port.
// `pos` is the top-left of the line box (as for AddText).
void draw_text(ImDrawList* dl, ImFont* font, float size, float adv, ImVec2 pos, ImU32 col, std::string_view s);

// While a system font for missing characters is being looked for (spec 30), characters no font has are drawn as
// blank space (with the width the layout gives them) instead of the missing-glyph box. Off: the box, as before.
void set_blank_missing(bool on);

// With a Collector, every painted token is reported for text selection and
// selected parts get a highlight (drawn first, behind the text), find matches too.
// (Colours come from the theme: find_match / find_current; highlight = selection.)
void paint(ImDrawList* dl, ImVec2 origin, const Layout& lay, const std::vector<Tok>& toks,
           const Theme& theme, std::vector<LinkHit>& links, Collector* sel = nullptr, ImU32 highlight = 0);

// Find-in-page backgrounds for the run just pushed to sel, over the line box y0..y1.
void draw_find_highlights(ImDrawList* dl, const Collector& sel, float y0, float y1, const Theme& theme);
