#include "math.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <deque>
#include <algorithm>
#include <map>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#ifdef __ANDROID__
#include <GLES3/gl3.h>
#else
#include "gl_funcs.h"  // (GL 1.1 calls: opengl32 on Windows, function pointers on Linux)
#endif

#include "gl_caps.h"
#include "mermaid/native.h"
#include "mermaid/text.h"
#include "resources.h"
#include "text_capture.h"
#include "texmath/texmath.h"

// The C++ TeX engine reads the math font embedded in the exe (resources.h); without it no formula is drawn.
static bool tex_engine_ready() {
    static const bool ready = [] {
        auto font = resource("fonts/latinmodern-math.otf");
        return font && texmath::init_font_data(std::vector<uint8_t>(font->begin(), font->end()));
    }();
    return ready;
}

// Upload a white-ink bitmap as a GL texture. `layout_mul` (at most 1): the bitmap was drawn at that fraction of the wanted scale
// to fit the texture limit, so it is drawn 1 / layout_mul times bigger on screen (px_w, px_h). A bitmap still over the limit is
// cut down by resampling (its px_w, px_h stay what they were). A refused upload gives a MathTex without a texture.
// `whiten` false: the pixels are straight-alpha colour already (a diagram's colour layer) and stay as they are.
static MathTex upload_pixels(unsigned width, unsigned height, unsigned char* rgba, float ox, float oy, float layout_mul = 1.0f, bool whiten = true) {
    // The engines give premultiplied white ink; ImGui blends straight alpha,
    // so make every pixel pure white and keep only the alpha.
    if (whiten)
        for (size_t i = 0; i < size_t(width) * height; i++) {
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
        }
    int tw = int(width), th = int(height);
    std::vector<unsigned char> small;
    if (const double fit = glcaps::fit_factor(tw, th); fit < 1.0) {
        tw = std::max(1, int(std::lround(tw * fit))), th = std::max(1, int(std::lround(th * fit)));
        trace::log("bitmap %ux%u is over the texture limit %d: uploaded as %dx%d", width, height, glcaps::max_texture(), tw, th);
        small = glcaps::resample_rgba(rgba, int(width), int(height), tw, th);
        rgba = small.data();
    }
    while (glGetError() != GL_NO_ERROR) {}
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);  // CLAMP_TO_EDGE (GLES 2 wants it for non-power-of-two textures)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    MathTex m;
    if (const GLenum err = glGetError(); err != GL_NO_ERROR) {
        glcaps::upload_failed("formula/diagram", unsigned(err), tw, th);
        glDeleteTextures(1, &id);
        return m;
    }
    m.tex = static_cast<ImTextureID>(id);
    m.px_w = float(width) / layout_mul;
    m.px_h = float(height) / layout_mul;
    m.ox = ox / layout_mul;
    m.oy = oy / layout_mul;
    return m;
}

const MathTex* math_get(const std::string& tex, bool display, float size) {
    // Cache by (source, style, pixel size). Failures are cached too (as empty entries).
    // `std::less<>` lets us look up with a string_view, so a cache hit (every
    // frame, for every visible formula) doesn't copy the formula text.
    // (a PDF export that captures text has its own entries: those bitmaps lack the glyphs that become text)
    static std::map<std::tuple<std::string, bool, int, bool>, MathTex, std::less<>> cache;
    const bool capture = text_capture::enabled();
    int ppem = static_cast<int>(std::lround(size));
    if (auto it = cache.find(std::make_tuple(std::string_view(tex), display, ppem, capture)); it != cache.end())
        return it->second.tex ? &it->second : nullptr;

    MathTex m;
    if (tex_engine_ready()) {
        texmath::Result r = texmath::render(tex, display, static_cast<float>(ppem), capture);
        if (auto* eq = std::get_if<texmath::Equation>(&r)) {
            texmath::Rendered& main = eq->main;
            m = upload_pixels(main.width, main.height, main.rgba.data(), main.origin_x, main.origin_y);
            m.glyphs = std::move(main.glyphs);
            m.w = main.em_w * ppem;
            m.asc = main.em_h * ppem;
            m.desc = main.em_d * ppem;
            if (eq->tag) {  // 	ag{..}: its own little bitmap, placed by the renderer
                static std::deque<MathTex> tags;  // stable addresses
                texmath::Rendered& t = *eq->tag;
                MathTex& tm = tags.emplace_back(upload_pixels(t.width, t.height, t.rgba.data(), t.origin_x, t.origin_y));
                tm.glyphs = std::move(t.glyphs);
                tm.w = t.em_w * ppem;
                tm.asc = t.em_h * ppem;
                tm.desc = t.em_d * ppem;
                m.tag = &tm;
            }
        }
    }
    auto& slot = cache[std::make_tuple(tex, display, ppem, capture)] = m;
    return slot.tex ? &slot : nullptr;
}

void math_draw(ImDrawList* dl, const MathTex& m, ImVec2 p0, ImU32 col) {
    dl->AddImage(m.tex, p0, ImVec2(p0.x + m.px_w, p0.y + m.px_h), ImVec2(0, 0), ImVec2(1, 1), col);
    if (!m.glyphs.empty()) text_capture::math(dl, p0, m.glyphs, col);
}

static bool env_is_set(const char* name) {
#ifdef _WIN32
    char value[8] = {};
    return GetEnvironmentVariableA(name, value, sizeof(value)) > 0;
#else
    return std::getenv(name) != nullptr;
#endif
}

// Maths in Mermaid labels ($$...$$ alone on a label line): typeset by the TeX engine above instead of the reference's plain-text
// approximation (src/mermaid/text.h). Display style, as `$$` means (a fraction keeps full-size numerator and denominator); a formula the engine
// cannot parse reports "no" and the label keeps the approximation. Boxes are cached by source (they scale with the font size).
static void register_mermaid_math() {
    if (!tex_engine_ready()) return;
    mermaid::MathHooks hooks;
    hooks.measure = [](const std::string& tex, float font_size) -> std::optional<mermaid::MathBox> {
        static std::map<std::string, std::optional<mermaid::MathBox>> em_boxes;  // in em
        auto it = em_boxes.find(tex);
        if (it == em_boxes.end()) {
            std::optional<mermaid::MathBox> box;
            texmath::Result r = texmath::render(tex, true, 16.0f);
            if (auto* eq = std::get_if<texmath::Equation>(&r)) box = mermaid::MathBox{eq->main.em_w, eq->main.em_h, eq->main.em_d};
            it = em_boxes.emplace(tex, box).first;
        }
        if (!it->second) return std::nullopt;
        return mermaid::MathBox{it->second->width * font_size, it->second->ascent * font_size, it->second->descent * font_size};
    };
    hooks.render = [](const std::string& tex, float ppem, mermaid::MathBitmap& out) {
        texmath::Result r = texmath::render(tex, true, ppem);
        auto* eq = std::get_if<texmath::Equation>(&r);
        if (!eq) return false;
        out.width = eq->main.width;
        out.height = eq->main.height;
        out.rgba = std::move(eq->main.rgba);
        out.origin_x = eq->main.origin_x;
        out.origin_y = eq->main.origin_y;
        return true;
    };
    mermaid::set_math_hooks(std::move(hooks));
}

// Diagrams are drawn by the C++ Mermaid port in src/mermaid/ (fonts are loaded once, on first use).
static bool native_mermaid_enabled() {
    static const bool enabled = [] {
        auto regular = resource("fonts/IBMPlexSans-Regular.ttf");
        if (!regular) return false;
        auto bold = resource("fonts/IBMPlexSans-Bold.ttf");
        auto italic = resource("fonts/IBMPlexSans-Italic.ttf");
        mermaid::NativeFonts fonts;
        fonts.regular.assign(regular->begin(), regular->end());
        if (bold) fonts.bold.assign(bold->begin(), bold->end());
        if (italic) fonts.italic.assign(italic->begin(), italic->end());
        const bool ok = mermaid::native_init(std::move(fonts));
        if (ok) register_mermaid_math();
        return ok;
    }();
    return enabled;
}

using DiagramCache = std::map<std::tuple<std::string, int>, MathTex, std::less<>>;
static DiagramCache& diagram_cache() {
    static DiagramCache cache;
    return cache;
}

void diagram_trim(const std::string& src, float above_scale, float keep_scale) {
    DiagramCache& cache = diagram_cache();
    const int above = static_cast<int>(std::lround(above_scale * 100)), keep = static_cast<int>(std::lround(keep_scale * 100));
    for (auto it = cache.upper_bound(std::make_tuple(src, above)); it != cache.end() && std::get<0>(it->first) == src;) {
        if (std::get<1>(it->first) == keep) {
            ++it;
            continue;
        }
        for (ImTextureID t : {it->second.tex, it->second.tex2})
            if (t) {
                const GLuint id = static_cast<GLuint>(t);
                glDeleteTextures(1, &id);
            }
        it = cache.erase(it);
    }
}

const MathTex* diagram_get(const std::string& src, float scale) {
    DiagramCache& cache = diagram_cache();
    int key_scale = static_cast<int>(std::lround(scale * 100));
    if (auto it = cache.find(std::make_tuple(std::string_view(src), key_scale)); it != cache.end())
        return it->second.tex ? &it->second : nullptr;
    MathTex m;
    bool done = false;
    if (native_mermaid_enabled()) {
        std::optional<mermaid::InkImage> ink = mermaid::native_render(src, scale);
        // Too big for one texture: draw it again at a scale that fits (it is shown at the wanted size all the same, a little soft).
        float used = scale;
        for (int tries = 0; ink && tries < 4 && std::max(ink->width, ink->height) > unsigned(glcaps::max_texture()); tries++) {
            used *= float(glcaps::max_texture()) / float(std::max(ink->width, ink->height)) * 0.995f;
            ink = mermaid::native_render(src, used);
        }
        if (ink) {
            m = upload_pixels(ink->width, ink->height, ink->rgba.data(), 0.0f, 0.0f, used / scale);
            if (m.tex && !ink->colour.empty())
                m.tex2 = upload_pixels(ink->width, ink->height, ink->colour.data(), 0.0f, 0.0f, used / scale, false).tex;
            done = true;
        } else if (env_is_set("CALLIGRUS_MERMAID_LOG")) {
            std::fprintf(stderr, "mermaid native: %s (falling back to Rust)\n", mermaid::native_last_reason().c_str());
        }
    }
    auto& slot = cache[std::make_tuple(src, key_scale)] = m;
    return slot.tex ? &slot : nullptr;
}

void diagram_draw(ImDrawList* dl, const MathTex& d, ImVec2 p0, ImVec2 p1, ImU32 col) {
    // (the colour layer goes underneath: the sankey's flows are drawn "multiply" over its labels, which keeps black text black)
    if (d.tex2) dl->AddImage(d.tex2, p0, p1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, (col >> IM_COL32_A_SHIFT) & 0xFF));
    dl->AddImage(d.tex, p0, p1, ImVec2(0, 0), ImVec2(1, 1), col);
}
