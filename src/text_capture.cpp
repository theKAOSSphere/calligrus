#include "text_capture.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <utility>

#include <imgui_internal.h>  // ImFontBaked

#include "resources.h"

namespace text_capture {

namespace {

// One captured glyph, in ImGui's screen pixels.
struct Item {
    size_t font = 0;       // index into g_fonts
    uint32_t gid = 0;
    float x = 0, y = 0;    // baseline origin
    float size = 0;        // pixels per em
    ImU32 col = 0;  // (its alpha is the text's opacity)
    float skew = 0;
    std::string text;
};

bool g_enabled = false;
float g_shear = 0;
std::vector<Item> g_items;                                 // this frame's text, in drawing order
std::vector<pdf::Font> g_fonts;                            // every font a captured glyph came from
std::deque<std::string> g_font_files;                      // their bytes: our own copies (the fonts the app loaded may be replaced while the export runs)
std::map<std::pair<const char*, int>, int> g_font_index;   // (file bytes, face) -> index in g_fonts, or -1: can't be embedded

// The index of `source`'s font in g_fonts, adding it; -1 if the PDF writer can't embed it.
int font_index(const prose_shaping::Source& source) {
    // The app may load its fonts again while an export runs (the installed fonts come in after the first
    // frames): the old files are freed and new ones may even take their addresses. So the lookup by
    // address is only good within one generation of fonts; after a change, fonts are told apart by content.
    static unsigned generation = ~0u;
    if (generation != prose_shaping::generation()) {
        generation = prose_shaping::generation();
        g_font_index.clear();
    }
    const auto key = std::make_pair(source.bytes.data(), source.face);
    auto it = g_font_index.find(key);
    if (it != g_font_index.end()) return it->second;
    int index = -1;
    for (size_t i = 0; i < g_fonts.size() && index < 0; i++)
        if (g_fonts[i].face == source.face && g_fonts[i].bytes == source.bytes) index = int(i);
    if (index < 0 && pdf::can_embed(source.bytes, source.face)) {
        index = int(g_fonts.size());
        g_font_files.emplace_back(source.bytes);  // (a deque: the views of the earlier copies stay valid)
        g_fonts.push_back(pdf::Font{"", g_font_files.back(), source.face});
    }
    return g_font_index.emplace(key, index).first->second;
}

// The height of the middle of a glyph's x-height, the point that decides if a glyph is on the page, clipped
// or covered (the Rust app, pdftext.rs, uses the same point): a line cut by an edge is wholly in or out.
float x_height_middle(float baseline, float size) { return baseline - 0.3f * size; }

std::string utf8(uint32_t c) {
    std::string s;
    if (c < 0x80) s += char(c);
    else if (c < 0x800) s += char(0xC0 | (c >> 6)), s += char(0x80 | (c & 0x3F));
    else if (c < 0x10000) s += char(0xE0 | (c >> 12)), s += char(0x80 | ((c >> 6) & 0x3F)), s += char(0x80 | (c & 0x3F));
    else s += char(0xF0 | (c >> 18)), s += char(0x80 | ((c >> 12) & 0x3F)), s += char(0x80 | ((c >> 6) & 0x3F)), s += char(0x80 | (c & 0x3F));
    return s;
}

}  // namespace

void enable(bool on) {
    g_enabled = on;
    g_items.clear();
}

bool enabled() { return g_enabled; }

void begin_frame() {
    if (g_enabled) g_items.clear();
}

bool glyph(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const prose_shaping::Source& source, uint32_t gid,
           std::string_view text) {
    if (!g_enabled) return false;
    const int f = font_index(source);
    ImFontBaked* baked = font->GetFontBaked(size);
    if (f < 0 || !baked || source.em <= 0) return false;
    // Where the glyph goes: the pen position, truncated to whole pixels like the drawing code does,
    // and the baseline `Ascent` below the top of the line.
    const float x = IM_TRUNC(pos.x), y = IM_TRUNC(pos.y) + baked->Ascent;
    const float size_px = baked->Size * source.size_ratio / source.em;  // ImGui scales a font so ascent + descent = its size
    // Not drawn at all if it lies outside what is visible (the draw list's clip rectangle): it would be clipped.
    // Like the Rust app, a glyph counts as inside when the middle of its x-height is (x_height_middle).
    const ImVec4 clip = dl->_CmdHeader.ClipRect;
    const float mid = x_height_middle(y, size_px);
    if (x < clip.x || x > clip.z || mid < clip.y || mid > clip.w) return true;
    Item it;
    it.font = size_t(f);
    it.gid = gid;
    it.x = x, it.y = y;
    it.size = size_px;
    it.col = col;
    it.skew = g_shear;
    it.text = std::string(text);
    g_items.push_back(std::move(it));
    return true;
}

bool character(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, uint32_t c) {
    if (!g_enabled) return false;
    uint32_t gid = 0;
    const prose_shaping::Source* source = prose_shaping::find_source(font, c, gid);
    return source && glyph(dl, font, size, pos, col, *source, gid, utf8(c));
}

// The math font's index in g_fonts, adding it on first use; -1: it can't be embedded.
int math_font_index() {
    static int index = -2;
    if (index == -2) {
        index = -1;
        if (auto bytes = resource("fonts/latinmodern-math.otf"); bytes && pdf::can_embed(*bytes, 0)) {
            index = int(g_fonts.size());
            g_fonts.push_back(pdf::Font{"", *bytes, 0});  // (the resource lives as long as the exe)
        }
    }
    return index;
}

void math(ImDrawList* dl, ImVec2 origin, const std::vector<texmath::MathGlyph>& glyphs, ImU32 col) {
    if (!g_enabled) return;
    const int f = math_font_index();
    if (f < 0) return;
    const ImVec4 clip = dl->_CmdHeader.ClipRect;
    for (const texmath::MathGlyph& m : glyphs) {
        const float x = origin.x + m.x, y = origin.y + m.y;
        // Dropped when outside the clip rectangle (the middle of its x-height decides, as for text).
        const float mid = x_height_middle(y, m.size);
        if (x < clip.x || x > clip.z || mid < clip.y || mid > clip.w) continue;
        Item it;
        it.font = size_t(f);
        it.gid = m.gid;
        it.x = x, it.y = y;
        it.size = m.size;
        it.col = col;
        it.text = m.text;
        g_items.push_back(std::move(it));
    }
}

void set_shear(float skew) { g_shear = skew; }

void cover(ImVec2 min, ImVec2 max) {
    if (!g_enabled) return;
    // A glyph is under the rectangle when the middle of its x-height is.
    std::erase_if(g_items, [&](const Item& it) {
        const float mid = x_height_middle(it.y, it.size);
        return it.x >= min.x && it.x < max.x && mid >= min.y && mid < max.y;
    });
}

void fill(pdf::Page& page) {
    for (const Item& it : g_items) {
        const float mid = x_height_middle(it.y, it.size);
        if (it.x < 0 || it.x > float(page.width) || mid < 0 || mid > float(page.height)) continue;
        pdf::Glyph g;
        g.font = it.font;
        g.gid = it.gid;
        g.x = it.x, g.y = it.y;
        g.size = it.size;
        g.rgb[0] = static_cast<unsigned char>((it.col >> IM_COL32_R_SHIFT) & 0xFF);
        g.rgb[1] = static_cast<unsigned char>((it.col >> IM_COL32_G_SHIFT) & 0xFF);
        g.rgb[2] = static_cast<unsigned char>((it.col >> IM_COL32_B_SHIFT) & 0xFF);
        g.alpha = static_cast<unsigned char>((it.col >> IM_COL32_A_SHIFT) & 0xFF);
        g.skew = it.skew;
        g.text = it.text;
        page.glyphs.push_back(std::move(g));
    }
}

const std::vector<pdf::Font>& fonts() { return g_fonts; }

}  // namespace text_capture
