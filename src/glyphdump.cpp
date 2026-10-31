// calligrus-glyphdump: draw a line of text exactly like the app does (same fonts,
// same FreeType settings, same ImGui glyph placement), without a window, and
// save it as a grayscale PNG (white text on black = the glyph alpha).
//
// Counterpart of the Rust port, for comparing text sharpness:
//     calligrus-glyphdump out.png [size_px] [dark|light] [text] [mono]
//
// ImGui 1.92 rasterizes glyphs on demand into the font atlas, which lives in
// CPU memory (ImTextureData) until a renderer uploads it. We ask for each glyph,
// then copy its atlas texels to the output at the position ImGui's text
// renderer would give it.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "fonts.h"
#include "platform.h"

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "imgui-text.png";
    const float em = argc > 2 ? static_cast<float>(std::atof(argv[2])) : 16.0f;
    const bool dark = argc > 3 ? std::string(argv[3]) != "light" : true;
    std::string text = argc > 4 ? argv[4]
                                : "No browser, no webview, no HTML. Markdown is parsed to a document tree.\n"
                                  "Hamburgefonstiv 0123456789 (illumination) {}[]";

    if (text.size() > 1 && text[0] == '@') {  // @FILE: the text is in a UTF-8 file (argv isn't UTF-8 on Windows)
        std::ifstream in(text.substr(1), std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini next to the exe
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920, 1080);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // atlas grows on demand, no GPU needed
    Fonts fonts = load_fonts(FontFiles{}, dark);  // the embedded default fonts
    ImGui::NewFrame();

    ImFont* font = argc > 5 && std::string(argv[5]) == "mono" ? fonts.mono : fonts.regular;
    const float size = fonts.px(font, em);  // what render.cpp passes to AddText
    ImFontBaked* baked = font->GetFontBaked(size);
    std::printf("requested size %.3f -> baked size %.3f (draw scale %.4f), RasterizerDensity %.2f\n",
                size, baked->Size, size / baked->Size, baked->RasterizerDensity);

    std::vector<unsigned int> chars;
    for (const char* s = text.c_str(); *s;) {
        unsigned int c = 0;
        s += ImTextCharFromUtf8(&c, s, nullptr);
        chars.push_back(c);
    }
    // Loading a glyph can grow/repack the atlas (moving earlier glyphs and
    // their UVs), so load every glyph first, and only then read them.
    for (unsigned int c : chars) baked->FindGlyph(static_cast<ImWchar>(c));

    // Lay the text out like ImGui's RenderText: pen advances by AdvanceX, each
    // glyph quad at pen + (X0, Y0). Lines are a whole number of pixels apart.
    struct Placed { const ImFontGlyph* g; float x, y; };
    std::vector<Placed> placed;
    float x = 0, y = 0, max_x = 0;
    for (unsigned int c : chars) {
        if (c == '\n') { x = 0; y += std::round(size); continue; }
        const ImFontGlyph* g = baked->FindGlyph(static_cast<ImWchar>(c));
        if (!g) continue;
        if (argc > 5) std::printf("U+%04X pen %.3f X0 %.3f X1 %.3f adv %.3f\n", g->Codepoint, x, g->X0, g->X1, g->AdvanceX);
        placed.push_back({g, x, y});
        x += g->AdvanceX;
        max_x = std::max(max_x, x);
    }

    ImTextureData* tex = io.Fonts->TexData;
    const int pad = 8;
    const int w = static_cast<int>(std::ceil(max_x)) + 2 * pad;
    const int h = static_cast<int>(std::ceil(y + size)) + 2 * pad;
    std::vector<unsigned char> img(size_t(w) * h, 0);
    int unaligned = 0;
    for (const Placed& p : placed) {
        const ImFontGlyph* g = p.g;
        if (!g->Visible) continue;
        const float qx = p.x + g->X0, qy = p.y + g->Y0;
        if (std::fabs(qx - std::round(qx)) > 0.01f || std::fabs(qy - std::round(qy)) > 0.01f) {
            if (unaligned < 4) std::printf("unaligned glyph U+%04X: X0 %.3f Y0 %.3f pen %.3f\n", g->Codepoint, g->X0, g->Y0, p.x);
            unaligned++;
        }
        const int tx0 = static_cast<int>(std::lround(g->U0 * tex->Width)), ty0 = static_cast<int>(std::lround(g->V0 * tex->Height));
        const int gw = static_cast<int>(std::lround((g->U1 - g->U0) * tex->Width));
        const int gh = static_cast<int>(std::lround((g->V1 - g->V0) * tex->Height));
        // The GPU stretches the gw x gh texels over the quad (X1-X0) x (Y1-Y0):
        // any mismatch means resampling (blur).
        if (static int shown = 0; shown < 6 && (std::fabs((g->X1 - g->X0) - gw) > 0.01f || std::fabs((g->Y1 - g->Y0) - gh) > 0.01f)) {
            std::printf("quad/texel size mismatch U+%04X: quad %.3f x %.3f, texels %d x %d\n", g->Codepoint, g->X1 - g->X0, g->Y1 - g->Y0, gw, gh);
            shown++;
        }
        const int dx0 = static_cast<int>(std::lround(qx)) + pad, dy0 = static_cast<int>(std::lround(qy)) + pad;
        for (int yy = 0; yy < gh; yy++) {
            const auto* row = static_cast<const unsigned char*>(tex->GetPixelsAt(tx0, ty0 + yy));
            for (int xx = 0; xx < gw; xx++) {
                const int ox = dx0 + xx, oy = dy0 + yy;
                if (ox < 0 || oy < 0 || ox >= w || oy >= h) continue;
                const unsigned a = tex->Format == ImTextureFormat_RGBA32 ? row[xx * 4 + 3] : row[xx];
                unsigned char& d = img[size_t(oy) * w + ox];
                d = static_cast<unsigned char>(std::min(255u, a + d * (255u - a) / 255u));  // white "over"
            }
        }
    }
    if (unaligned) std::printf("note: %d glyph quads not pixel-aligned (drawn rounded)\n", unaligned);

    stbi_write_png(out, w, h, 1, img.data(), w);
    std::printf("wrote %s (%dx%d, %.1f em-px -> ImGui size %.2f, %s)\n", out, w, h, em, size, dark ? "dark" : "light");
    ImGui::EndFrame();
    ImGui::DestroyContext();
    return 0;
}
