#include "math.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <deque>
#include <map>
#include <string_view>
#include <tuple>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <GL/gl.h>  // GL 1.1 texture calls are exported by opengl32 directly

#include "mermaid/native.h"
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

// Upload a white-ink bitmap as a GL texture.
static MathTex upload_pixels(unsigned width, unsigned height, unsigned char* rgba, float ox, float oy) {
    // The engines give premultiplied white ink; ImGui blends straight alpha,
    // so make every pixel pure white and keep only the alpha.
    for (size_t i = 0; i < size_t(width) * height; i++) {
        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
    }
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    MathTex m;
    m.tex = static_cast<ImTextureID>(id);
    m.px_w = float(width);
    m.px_h = float(height);
    m.ox = ox;
    m.oy = oy;
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
        return mermaid::native_init(std::move(fonts));
    }();
    return enabled;
}

const MathTex* diagram_get(const std::string& src, float scale) {
    static std::map<std::tuple<std::string, int>, MathTex, std::less<>> cache;
    int key_scale = static_cast<int>(std::lround(scale * 100));
    if (auto it = cache.find(std::make_tuple(std::string_view(src), key_scale)); it != cache.end())
        return it->second.tex ? &it->second : nullptr;
    MathTex m;
    bool done = false;
    if (native_mermaid_enabled()) {
        if (auto ink = mermaid::native_render(src, scale)) {
            m = upload_pixels(ink->width, ink->height, ink->rgba.data(), 0.0f, 0.0f);
            done = true;
        } else if (env_is_set("CALLIGRUS_MERMAID_LOG")) {
            std::fprintf(stderr, "mermaid native: %s (falling back to Rust)\n", mermaid::native_last_reason().c_str());
        }
    }
    auto& slot = cache[std::make_tuple(src, key_scale)] = m;
    return slot.tex ? &slot : nullptr;
}
