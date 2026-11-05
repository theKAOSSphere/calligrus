#include "shaped_draw.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#ifdef __ANDROID__
#include <GLES3/gl3.h>
#else
#include <GL/gl.h>  // GL 1.1 texture calls are exported by opengl32 directly
#endif

#include <ft2build.h>
#include FT_FREETYPE_H

#include <imgui_internal.h>  // ImFontBaked

#include "fonts.h"
#include "prose_shaping.h"
#include "shaping.h"

namespace shaped_draw {

namespace {

// The textures grow with use: most documents need only a few glyphs here (code ligatures, complex scripts), so the
// first page is small (256 KB instead of 4 MB of video and driver memory), the second medium, the rest full size.
// Up to 8 full pages (32 MB of glyph pictures) plus the two small ones.
constexpr size_t kMaxPages = 10;
constexpr int page_size(size_t i) { return i == 0 ? 256 : i == 1 ? 512 : 1024; }  // pixels, square

// One texture with glyphs packed in rows ("shelves"): glyphs go left to right; a glyph that
// does not fit starts a new row below, as tall as the first glyph of that row.
struct Page {
    GLuint tex = 0;
    int size = 0;  // the texture is size x size pixels
    int x = 0, y = 0, row_h = 0;
};

// Where a glyph's picture is, and where it goes relative to the pen position.
struct Cached {
    bool visible = false;
    size_t page = 0;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // quad corners from the pen's x and from the line's top
};

// A source font opened with FreeType. Identified by where its bytes are and its face number
// (the fonts' bytes live until the next load_fonts, which also bumps the generations below).
struct Face {
    const char* data = nullptr;
    int index = 0;
    FT_Face ft = nullptr;  // nullptr: it could not be opened
    float requested = -1;  // size the FT face is set to now
};

FT_Library g_lib = nullptr;
std::vector<Face> g_faces;
unsigned g_generation = ~0u;  // the generations of shaping + prose_shaping the state below belongs to
std::vector<Page> g_pages;
std::unordered_map<uint64_t, Cached> g_glyphs;  // key: face, glyph id and size

void forget_fonts() {
    for (const Page& p : g_pages) glDeleteTextures(1, &p.tex);
    g_pages.clear();
    g_glyphs.clear();
    for (const Face& f : g_faces)
        if (f.ft) FT_Done_Face(f.ft);
    g_faces.clear();
}

// The FreeType face of a source font, opened on first use (and again after a font change).
// Returns its number in g_faces, or -1.
int open_face(std::string_view bytes, int index) {
    const unsigned generation = shaping::generation() * 100003u + prose_shaping::generation();
    if (generation != g_generation) {
        forget_fonts();
        g_generation = generation;
    }
    if (bytes.empty()) return -1;
    for (size_t i = 0; i < g_faces.size(); i++)
        if (g_faces[i].data == bytes.data() && g_faces[i].index == index) return g_faces[i].ft ? int(i) : -1;
    Face f;
    f.data = bytes.data();
    f.index = index;
    if (!g_lib && FT_Init_FreeType(&g_lib)) return -1;
    if (FT_New_Memory_Face(g_lib, reinterpret_cast<const FT_Byte*>(bytes.data()), FT_Long(bytes.size()), index, &f.ft))
        f.ft = nullptr;
    g_faces.push_back(f);
    return f.ft ? int(g_faces.size() - 1) : -1;
}

// Find room for w x h pixels: {page, x, y}, or page == kMaxPages if all pages are full.
struct Slot {
    size_t page;
    int x, y;
};
Slot reserve(int w, int h) {
    for (size_t i = 0; i < kMaxPages; i++) {
        if (i == g_pages.size()) {
            Page p;
            p.size = page_size(i);
            glGenTextures(1, &p.tex);
            glBindTexture(GL_TEXTURE_2D, p.tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            const std::vector<unsigned char> blank(size_t(p.size) * p.size * 4, 0);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, p.size, p.size, 0, GL_RGBA, GL_UNSIGNED_BYTE, blank.data());
            g_pages.push_back(p);
        }
        Page& p = g_pages[i];
        if (w > p.size) continue;  // (too wide for this small page)
        if (p.x + w > p.size) p.y += p.row_h, p.x = 0, p.row_h = 0;  // next row
        if (p.y + h > p.size) continue;
        const Slot s{i, p.x, p.y};
        p.x += w + 1;  // one empty pixel between glyphs
        p.row_h = std::max(p.row_h, h + 1);
        return s;
    }
    return {kMaxPages, 0, 0};
}

// Rasterise `glyph` at `size` pixels (times `density`) the way ImGui's FreeType loader does
// (imgui_freetype.cpp: size request, load flags, FT_Render_Glyph, white + coverage as alpha)
// and put it in a page. `ascent` is ImGui's baked ascent for this size.
bool rasterise(Face& face, uint32_t glyph, float size, float density, float ascent, Cached& out) {
    // `size` is the size ImGui gives THIS font (a merged font's: times its ExtraSizeScale).
    if (face.requested != size * density) {
        FT_Size_RequestRec req{};
        req.type = FT_SIZE_REQUEST_TYPE_REAL_DIM;
        req.height = FT_Long(size * 64 * density);
        if (FT_Request_Size(face.ft, &req)) return false;
        face.requested = size * density;
    }
    // What the loader asks for with LightHinting | NoAutoHint (fonts.cpp load_fonts).
    if (FT_Load_Glyph(face.ft, glyph, FT_LOAD_NO_BITMAP | FT_LOAD_NO_AUTOHINT | FT_LOAD_TARGET_LIGHT)) return false;
    FT_GlyphSlot slot = face.ft->glyph;
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE && slot->format != FT_GLYPH_FORMAT_BITMAP) return false;
    if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL)) return false;
    const FT_Bitmap& bm = slot->bitmap;
    const int w = int(bm.width), h = int(bm.rows);
    out = Cached{};
    if (w == 0 || h == 0) return true;                  // an empty glyph (a spacer)
    if (bm.pixel_mode != FT_PIXEL_MODE_GRAY) return false;  // (a colour glyph: not in code fonts)

    const Slot at = reserve(w, h);
    if (at.page == kMaxPages) return false;
    std::vector<unsigned char> rgba(size_t(w) * h * 4);
    const bool boost = fonts_coverage_boost();
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int c = bm.buffer[size_t(y) * bm.pitch + x];
            // The same dark-theme coverage curve as load_glyph_with_curve (fonts.cpp).
            if (boost) c = (2 * c * 255 - c * c + 127) / 255;
            unsigned char* px = &rgba[(size_t(y) * w + x) * 4];
            px[0] = px[1] = px[2] = 255;
            px[3] = static_cast<unsigned char>(c);
        }
    glBindTexture(GL_TEXTURE_2D, g_pages[at.page].tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, at.x, at.y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    out.visible = true;
    out.page = at.page;
    const float page_px = float(g_pages[at.page].size);
    out.u0 = float(at.x) / page_px, out.v0 = float(at.y) / page_px;
    out.u1 = float(at.x + w) / page_px, out.v1 = float(at.y + h) / page_px;
    out.x0 = float(slot->bitmap_left) / density;
    out.y0 = float(-slot->bitmap_top) / density + ascent;
    out.x1 = out.x0 + float(w) / density;
    out.y1 = out.y0 + float(h) / density;
    return true;
}

}  // namespace

bool draw(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const prose_shaping::Source& source,
          uint32_t glyph) {
    const int face_no = open_face(source.bytes, source.face);
    if (face_no < 0) return false;
    ImFontBaked* baked = font->GetFontBaked(size);
    if (!baked) return false;
    const float px = baked->Size, density = baked->RasterizerDensity;
    const float own = px * source.size_ratio;  // the pixel size of this source font
    const uint64_t key = (uint64_t(face_no) << 48) | (uint64_t(glyph & 0xFFFF) << 32) | uint32_t(own * density * 64);
    auto it = g_glyphs.find(key);
    if (it == g_glyphs.end()) {
        Cached c;
        if (!rasterise(g_faces[size_t(face_no)], glyph, own, density, baked->Ascent, c)) return false;  // (tried again next time)
        it = g_glyphs.emplace(key, c).first;
    }
    const Cached& c = it->second;
    if (!c.visible) return true;  // a spacer: nothing to draw

    // RenderChar truncates the pen position to whole pixels; so do we.
    const float x = IM_TRUNC(pos.x), y = IM_TRUNC(pos.y);
    dl->AddImage(static_cast<ImTextureID>(g_pages[c.page].tex), ImVec2(x + c.x0, y + c.y0),
                 ImVec2(x + c.x1, y + c.y1), ImVec2(c.u0, c.v0), ImVec2(c.u1, c.v1), col);
    return true;
}

bool draw(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, uint32_t glyph) {
    prose_shaping::Source code_font;  // the code font: the main face of its ImFont
    code_font.bytes = shaping::font_bytes();
    code_font.face = shaping::font_face();
    return draw(dl, font, size, pos, col, code_font, glyph);
}

}  // namespace shaped_draw
