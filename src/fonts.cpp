#include "fonts.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>

#include <imgui.h>
#include <imgui_internal.h>  // ImFontLoader / atlas rects, to post-process glyphs
#ifdef IMGUI_ENABLE_FREETYPE
#include <imgui_freetype.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SIZES_H
#endif

#include "platform.h"
#include "prose_shaping.h"
#include "resources.h"
#include "shaping.h"
#include "trace.h"

namespace fs = std::filesystem;

float Fonts::em_ratio(const ImFont* font) const {
    if (font == mono) return mono_em;
    if (font == bold) return bold_em;
    if (font == italic) return italic_em;
    if (font == bold_italic) return bold_italic_em;
    return regular_em;
}

float Fonts::desc_fraction(const ImFont* font) const {
    if (font == mono) return mono_df;
    if (font == bold) return bold_df;
    if (font == italic) return italic_df;
    if (font == bold_italic) return bold_italic_df;
    return regular_df;
}

float Fonts::px(const ImFont* font, float em) const {
    const float exact = em * em_ratio(font);
    // CALLIGRUS_FRACTIONAL_FONT_SIZES=1 restores the old (blurry) exact sizes, for comparison.
#pragma warning(suppress : 4996)  // MSVC: getenv is fine for reading a flag
    static const bool fractional = std::getenv("CALLIGRUS_FRACTIONAL_FONT_SIZES") != nullptr;
    return fractional ? exact : std::round(exact);
}

float Fonts::advance_scale(const ImFont* font, float em) const {
    const float rounded = px(font, em);
    return rounded > 0 ? em * em_ratio(font) / rounded : 1.0f;
}

ImFont* Fonts::face(bool is_bold, bool is_italic) const {
    if (is_bold && is_italic) return bold_italic;
    if (is_bold) return bold;
    if (is_italic) return italic;
    return regular;
}

// ---------------------------------------------------------------------------
// Font metrics straight from the file
// ---------------------------------------------------------------------------
//
// ImGui (with FreeType) sizes a font so that ascender - descender equals the
// requested size. FreeType takes those from the `hhea` table, or from `OS/2`
// (typo, then win metrics) when hhea has none. Reading the same numbers here
// tells us how many pixels per em a given ImGui size gives.

// Shared by font_em_ratio and font_desc_fraction: (ascender - descender) / upem and descender / (ascender - descender).
static bool vertical_metrics(std::string_view data, int face, float& em, float& desc_frac) {
    auto u16 = [&](size_t at) -> uint32_t {
        return at + 2 <= data.size() ? (uint32_t(uint8_t(data[at])) << 8) | uint8_t(data[at + 1]) : 0;
    };
    auto s16 = [&](size_t at) { return int(int16_t(uint16_t(u16(at)))); };
    auto u32 = [&](size_t at) { return (u16(at) << 16) | u16(at + 2); };

    size_t font = 0;  // offset of the table directory
    if (data.compare(0, 4, "ttcf") == 0) {
        // A collection: the number of faces at 8, their directory offsets from 12.
        if (face < 0 || uint32_t(face) >= u32(8)) return false;
        font = u32(12 + size_t(face) * 4);
    } else if (face != 0) {
        return false;
    }
    // Find a table by its tag.
    auto table = [&](const char* tag) -> size_t {
        const uint32_t n = u16(font + 4);
        for (uint32_t i = 0; i < n; i++) {
            const size_t rec = font + 12 + size_t(i) * 16;
            if (rec + 16 > data.size()) return 0;
            if (data.compare(rec, 4, tag) == 0) return u32(rec + 8);
        }
        return 0;
    };
    const size_t head = table("head"), hhea = table("hhea"), os2 = table("OS/2");
    if (!head || head + 20 > data.size()) return false;
    const int upem = int(u16(head + 18));
    if (upem <= 0) return false;

    int asc = hhea ? s16(hhea + 4) : 0, desc = hhea ? s16(hhea + 6) : 0;
    if (asc == 0 && desc == 0 && os2) {
        asc = s16(os2 + 68), desc = s16(os2 + 70);                         // sTypoAscender / Descender
        if (asc == 0 && desc == 0) asc = int(u16(os2 + 74)), desc = -int(u16(os2 + 76));  // usWinAscent / Descent
    }
    const float ratio = float(asc - desc) / float(upem);
    if (!(ratio > 0.2f && ratio < 5.0f)) return false;
    em = ratio;
    desc_frac = std::clamp(float(-desc) / float(asc - desc), 0.0f, 1.0f);
    return true;
}

float font_em_ratio(std::string_view data, int face) {
    float em = 0, df = 0;
    return vertical_metrics(data, face, em, df) ? em : 0.0f;
}

float font_desc_fraction(std::string_view data, int face) {
    float em = 0, df = 0;
    return vertical_metrics(data, face, em, df) ? df : 0.0f;
}

// ---------------------------------------------------------------------------
// egui's coverage curve
// ---------------------------------------------------------------------------
//
// egui draws light-on-dark text with alpha = 2c - c^2 (c = how much of the
// pixel the glyph covers): edge pixels get noticeably brighter, so strokes read
// as crisper. ImGui only has a linear RasterizerMultiply, so we wrap the font
// loader: let FreeType rasterize each glyph into the atlas as usual, then
// remap that glyph's pixels once, before they are uploaded to the GPU.

static const ImFontLoader* g_base_loader = nullptr;  // FreeType (or stb_truetype)
static ImFontLoader g_loader;                       // copy of it with our glyph hook
// Dev switch: CALLIGRUS_FT_LEGACY=1 leaves the loader's own (stale) activation check alone, to see the old behaviour.
static const bool g_ft_legacy = [] {
    const char* v = std::getenv("CALLIGRUS_FT_LEGACY");
    if (v && *v) return true;
    const char* home = std::getenv("HOME");  // (Android: a file `ft_legacy` in the app home)
    return home && fs::exists(fs::path(home) / "ft_legacy");
}();
static bool g_boost = false;                         // apply the curve (dark theme only, like egui)

// Characters that only steer how their neighbours look (emoji variation
// selectors like the U+FE0F in "⚠️", zero-width joiners, the BOM, ...). Our
// fonts have no glyphs for them, so ImGui would draw its "missing" box; egui
// draws nothing, so we hand back an empty, zero-width glyph.
static bool is_invisible(unsigned c) {
    return (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0x200B && c <= 0x200F) || (c >= 0x2060 && c <= 0x2064) ||
           c == 0x00AD || c == 0xFEFF || (c >= 0xE0100 && c <= 0xE01EF);
}

static bool load_glyph_with_curve(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void* data,
                                  ImWchar codepoint, ImFontGlyph* glyph, float* advance_x) {
    if (is_invisible(codepoint)) {
        if (glyph) *glyph = ImFontGlyph();  // not visible, advance 0
        if (advance_x) *advance_x = 0.0f;
        return true;
    }
#ifdef IMGUI_ENABLE_FREETYPE
    // ImGui's FreeType loader skips activating a baked's FT_Size when that baked is "the one activated last", which
    // it remembers by ADDRESS (BakedLastActivated). That memo goes stale (a baked is discarded, its FT_Size freed, and
    // the pool of bakeds moves or reuses the address), and the glyph is then loaded, advance and picture, at the size
    // of whatever FT_Size is active: code came out with wrong advances and glyphs, overlapping (spec 27). `data` begins
    // with this baked's FT_Size for this source: make it the active one ourselves, every time (a cheap pointer swap).
    if (data && !g_ft_legacy) {
        const FT_Size size = *static_cast<FT_Size*>(data);
        if (size && size->face && size->face->size != size) FT_Activate_Size(size);
    }
#endif
    if (!g_base_loader->FontBakedLoadGlyph(atlas, src, baked, data, codepoint, glyph, advance_x)) return false;
    if (!g_boost || glyph == nullptr || glyph->PackId < 0 || glyph->Colored) return true;

    ImTextureRect* r = ImFontAtlasPackGetRect(atlas, glyph->PackId);
    ImTextureData* tex = atlas->TexData;
    if (r == nullptr || tex == nullptr) return true;
    // Pixels are white with coverage in alpha (RGBA32), or alpha only (Alpha8).
    const int alpha = tex->Format == ImTextureFormat_RGBA32 ? 3 : 0;
    for (int y = 0; y < r->h; y++) {
        auto* row = static_cast<unsigned char*>(tex->GetPixelsAt(r->x, r->y + y));
        for (int x = 0; x < r->w; x++) {
            unsigned char& a = row[x * tex->BytesPerPixel + alpha];
            int c = a;                                // coverage 0..255
            a = static_cast<unsigned char>((2 * c * 255 - c * c + 127) / 255);  // 255 * (2c - c^2)
        }
    }
    return true;
}

bool fonts_coverage_boost() { return g_boost; }

static void install_loader(ImFontAtlas* atlas) {
    if (g_base_loader == nullptr) {
#ifdef IMGUI_ENABLE_FREETYPE
        g_base_loader = ImGuiFreeType::GetFontLoader();
#else
        g_base_loader = ImFontAtlasGetFontLoaderForStbTruetype();
#endif
        g_loader = *g_base_loader;
        g_loader.Name = "freetype+egui-coverage";
        g_loader.FontBakedLoadGlyph = load_glyph_with_curve;
    }
    if (atlas->FontLoader != &g_loader) atlas->SetFontLoader(&g_loader);
}


// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

namespace {

// The bytes of one font file, which face of it to use, and that face's em ratio.
struct FontData {
    std::string_view bytes;
    int face = 0;  // ImFontConfig::FontNo (a face of a .ttc)
    float em = 0;
    float df = 0;  // descender / (ascender - descender)
};

// Bytes of custom font files: the atlas reads them while rasterizing (it
// doesn't copy them), so they live here until the next load_fonts. Keyed by
// path, so a file used for several faces (a .ttc, or a regular file standing
// in for bold) is read once.
std::map<std::string, std::unique_ptr<std::string>> g_custom_files;
// What the previous load_fonts had: a file wanted again (the next atlas has the same system fonts, and one more)
// is taken from here instead of being read again. Emptied when a load is done.
std::map<std::string, std::unique_ptr<std::string>> g_previous_files;
// Files read ahead of time by prefetch_font_files (another thread), until a load takes them.
std::mutex g_prefetch_mutex;
std::map<std::string, std::unique_ptr<std::string>> g_prefetched;

std::unique_ptr<std::string> read_font_file(const std::string& file) {
    auto bytes = platform::read_file(fs::path(std::u8string(file.begin(), file.end())));
    return bytes ? std::make_unique<std::string>(std::move(*bytes)) : nullptr;
}

FontData builtin(const char* name) {
    auto bytes = resource(name);
    if (!bytes) return {};
    return {*bytes, 0, font_em_ratio(*bytes), font_desc_fraction(*bytes)};
}

// A face from the config; nullopt (plus one message per file) if it can't be used.
std::optional<FontData> custom(const FontSource& src, std::vector<std::string>& errors) {
    auto it = g_custom_files.find(src.file);
    if (it == g_custom_files.end()) {
        std::unique_ptr<std::string> bytes;
        const char* from = "read";
        if (auto prev = g_previous_files.find(src.file); prev != g_previous_files.end() && prev->second) {
            bytes = std::move(prev->second), from = "kept from the last load";
        } else if (std::lock_guard<std::mutex> lock(g_prefetch_mutex); g_prefetched.count(src.file)) {
            bytes = std::move(g_prefetched[src.file]), from = "prefetched";
            g_prefetched.erase(src.file);
        }
        if (!bytes) {
            trace::Stopwatch sw;
            bytes = read_font_file(src.file);
            trace::log("font file %s: read on the UI thread, %zu bytes, %.1f ms", src.file.c_str(), bytes ? bytes->size() : size_t(0), sw.ms());
        } else {
            trace::log("font file %s: %s (%zu bytes)", src.file.c_str(), from, bytes->size());
        }
        if (!bytes) errors.push_back("Could not read font " + src.file + " (using the built-in font)");
        // (A failed read is remembered as nullptr, so it's reported once.)
        it = g_custom_files.emplace(src.file, std::move(bytes)).first;
    }
    if (!it->second) return std::nullopt;
    const float em = font_em_ratio(*it->second, src.face);
    if (em <= 0) {
        const std::string what = src.face ? src.file + " #" + std::to_string(src.face) : src.file;
        const std::string msg = "Not a usable TTF/OTF font: " + what + " (using the built-in font)";
        if (std::find(errors.begin(), errors.end(), msg) == errors.end()) errors.push_back(msg);
        return std::nullopt;
    }
    return FontData{*it->second, src.face, em, font_desc_fraction(*it->second, src.face)};
}

// Add one font to the atlas. `merge` adds its glyphs to the previously added
// font instead (used for icon/emoji fallbacks).
// ref_em = the em ratio of the font this one is merged into (its own if not merged):
// ImGui scales a merged font by its size relative to the main font's, so both get the
// same reference size and ExtraSizeScale does the rest.
ImFont* add(const FontData& f, bool merge = false, float extra_scale = 1.0f, float ref_em = 0, bool snap = true) {
    if (f.bytes.empty()) return nullptr;
    ImFontConfig cfg;
    cfg.FontNo = f.face;
    cfg.MergeMode = merge;
    cfg.ExtraSizeScale = extra_scale;
    cfg.FontDataOwnedByAtlas = false;  // the bytes are in the exe (or g_custom_files)
    // snap: whole-pixel glyph advances, so glyphs land exactly on the pixel grid instead of
    // being smeared across two pixels by the GPU's linear filtering. The document faces
    // are loaded without it (exact advances, like egui) and draw glyph by glyph instead.
    cfg.PixelSnapH = snap;
    // (The size only matters as a default: ImGui 1.92 draws fonts at any size.)
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<char*>(f.bytes.data()), int(f.bytes.size()),
                                                      16.0f * (ref_em > 0 ? ref_em : f.em), &cfg);
}

// A face plus egui's fallback fonts merged in, in the Rust port's order (its font families):
//   prose: the face, Ubuntu-Light, Noto Emoji, emoji-icon-font, Cascadia Code (box drawing ┌─┐), then
//          Hack (the symbols Cascadia lacks: ⇒ ⇔ ∀ ∃...), then Noto Sans Symbols 2 (✗ ✘ ☒ and more);
//   code (`code`): the face, Cascadia Code (if the face is a custom one), Hack, Ubuntu-Light, the two emoji fonts,
//          Noto Sans Symbols 2.
//   both: then the installed fonts (spec 30), and Latin Modern Math (CFF outlines) as the very last resort.
// So "📂", "⟳", "☰", "🌙"... render.
// Same fallback order and shrink factors as egui's defaults (epaint/src/text/fonts.rs).
// (Emoji above U+FFFF need ImGui built with 32-bit ImWchar: vcpkg feature "wchar32".)
//
// Merged fonts are sized like any ImGui font: so that THEIR ascent+descent
// equals the size. That size was chosen for the main face (em ratio `f.em`),
// so each fallback's scale includes (its own ratio / the main face's) to get
// the em size egui gives it: emoji-icon-font is 1.0 em tall, Noto Emoji 1.3618.
ImFont* add_with_symbols(const FontData& f, std::vector<prose_shaping::Chain>& chains, bool snap = false,
                         bool code = false, bool custom_code = false, const std::vector<FontData>& system = {}) {
    ImFont* font = add(f, false, 1.0f, 0, snap);
    if (font) {
        // The fonts merged into `font`, in the order ImGui looks glyphs up (prose_shaping.h needs them
        // to pick the font that shapes a run of text).
        prose_shaping::Chain chain{font, {}};
        auto note = [&](const FontData& d, float scale) {
            if (!d.bytes.empty() && d.em > 0) chain.sources.push_back({d.bytes, d.face, scale, d.em});
        };
        auto merge = [&](const FontData& d, float scale) {
            add(d, true, scale, f.em, snap);
            note(d, scale);
        };
        note(f, 1.0f);
        const FontData cascadia = builtin("fonts/CascadiaCode-Regular.ttf");
        const FontData hack = builtin("fonts/Hack-Regular.ttf");
        if (code && custom_code) merge(cascadia, cascadia.em / f.em);
        if (code) merge(hack, hack.em / f.em);
        const FontData ubuntu = builtin("fonts/Ubuntu-Light.ttf");
        merge(ubuntu, ubuntu.em / f.em);
        const FontData noto = builtin("fonts/NotoEmoji-Regular.ttf");
        const FontData icons = builtin("fonts/emoji-icon-font.ttf");
        merge(noto, 0.81f * (noto.em / f.em));
        merge(icons, 0.90f * (icons.em / f.em));
        if (!code) {
            merge(cascadia, cascadia.em / f.em);
            merge(hack, hack.em / f.em);
        }
        // Noto Sans Symbols 2: the dingbats and symbols none of the fonts above have (✗ ✘ ☒...).
        const FontData symbols = builtin("fonts/NotoSansSymbols2-Regular.ttf");
        merge(symbols, symbols.em / f.em);
        // Installed fonts for missing characters (spec 30), after everything else.
        for (const FontData& s : system) merge(s, s.em / f.em);
        // Latin Modern Math, the very last resort: maths symbols nothing else has (⊨ ⋈ ⟹ ♭...).
        const FontData math = builtin("fonts/latinmodern-math.otf");
        merge(math, math.em / f.em);
        // A glyph none of them has: egui's "◻" (ImGui would pick U+FFFD or "?").
        if (font->IsGlyphInFont(0x25FB)) font->FallbackChar = 0x25FB;
        chains.push_back(std::move(chain));
    }
    return font;
}

}  // namespace

Fonts load_fonts(const FontFiles& files, bool dark) {
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    install_loader(atlas);
    g_boost = dark;
#ifdef IMGUI_ENABLE_FREETYPE
    // Rasterize like egui: FreeType with *light* hinting snaps stems and the
    // x-height to the pixel grid vertically only (egui: skrifa "smooth" target),
    // instead of stb_truetype's unhinted, blurrier glyphs.
    // NoAutoHint: FreeType's auto-hinter rounds Plex's x-height up a whole pixel
    // at body sizes (text looked ~6% taller than egui); the font's own hints don't.
    atlas->FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting | ImGuiFreeTypeLoaderFlags_NoAutoHint;
#endif
    g_previous_files = std::move(g_custom_files);  // the atlas was cleared: nothing reads these any more (but the next may want them)
    g_custom_files.clear();

    Fonts f;
    // Which face each slot uses (resolve_fonts already applied the spec 5 / 8a
    // rules, e.g. a custom prose font without a bold face has its regular face
    // in the bold slot). A face that fails to load falls back to the custom
    // regular face if that loaded, else to the built-in face for the slot.
    std::optional<FontData> prose = files.prose.builtin() ? std::nullopt : custom(files.prose, f.errors);
    auto face = [&](const FontSource& src, const char* builtin_name) -> FontData {
        if (!src.builtin()) {
            if (auto d = custom(src, f.errors)) return *d;
            if (prose) return *prose;
        }
        return builtin(builtin_name);
    };
    const FontData regular = prose ? *prose : builtin("fonts/IBMPlexSans-Regular.ttf");
    const FontData bold = face(files.prose_bold, "fonts/IBMPlexSans-Bold.ttf");
    const FontData italic = face(files.prose_italic, "fonts/IBMPlexSans-Italic.ttf");
    const FontData bold_italic = face(files.prose_bold_italic, "fonts/IBMPlexSans-BoldItalic.ttf");
    std::optional<FontData> mono_file = files.mono.builtin() ? std::nullopt : custom(files.mono, f.errors);
    const FontData mono = mono_file ? *mono_file : builtin("fonts/CascadiaCode-Regular.ttf");

    // The system fonts for missing characters (a file that can't be used is just left out).
    std::vector<FontData> system;
    for (const FontSource& src : files.system)
        if (auto d = custom(src, f.errors)) system.push_back(*d);

    // The first font added becomes ImGui's default (used by toolbar widgets): whole-pixel advances.
    std::vector<prose_shaping::Chain> chains;  // each face's source fonts, for shaping complex scripts
    f.ui = add_with_symbols(regular, chains, true, false, false, system);
    if (!f.ui) f.ui = atlas->AddFontDefault();
    f.regular = add_with_symbols(regular, chains, false, false, false, system);
    if (!f.regular) f.regular = f.ui;
    f.bold = add_with_symbols(bold, chains, false, false, false, system);
    f.italic = add_with_symbols(italic, chains, false, false, false, system);
    f.bold_italic = add_with_symbols(bold_italic, chains, false, false, false, system);
    // Cascadia Code: the monospace font for code (the Rust version bundles the same file).
    f.mono = add_with_symbols(mono, chains, false, true, mono_file.has_value(), system);
    if (!f.bold) f.bold = f.regular;
    if (!f.italic) f.italic = f.regular;
    if (!f.bold_italic) f.bold_italic = f.bold;
    if (!f.mono) f.mono = f.regular;
    g_previous_files.clear();
    // Code ligatures: shape code text with the code face (not when it fell back to the prose face).
    shaping::set_font(f.mono == f.regular ? nullptr : f.mono, mono.bytes, mono.face);
    // Complex scripts in prose (spec 32): which fonts each face is made of.
    prose_shaping::set_chains(std::move(chains));

    f.regular_em = regular.em > 0 ? regular.em : 1.3f;
    f.bold_em = bold.em > 0 ? bold.em : f.regular_em;
    f.italic_em = italic.em > 0 ? italic.em : f.regular_em;
    f.bold_italic_em = bold_italic.em > 0 ? bold_italic.em : f.bold_em;
    f.mono_em = mono.em > 0 ? mono.em : 1.164f;
    f.regular_df = regular.df > 0 ? regular.df : f.regular_df;
    f.bold_df = bold.df > 0 ? bold.df : f.regular_df;
    f.italic_df = italic.df > 0 ? italic.df : f.regular_df;
    f.bold_italic_df = bold_italic.df > 0 ? bold_italic.df : f.bold_df;
    f.mono_df = mono.df > 0 ? mono.df : f.mono_df;
    return f;
}

void prefetch_font_files(const std::vector<FontSource>& sources) {
    for (const FontSource& src : sources) {
        if (src.builtin()) continue;
        {
            std::lock_guard<std::mutex> lock(g_prefetch_mutex);
            if (g_prefetched.count(src.file)) continue;
        }
        trace::Stopwatch sw;
        std::unique_ptr<std::string> bytes = read_font_file(src.file);
        if (!bytes) continue;
        trace::log("font file %s: prefetched on a worker thread, %zu bytes, %.1f ms", src.file.c_str(), bytes->size(), sw.ms());
        std::lock_guard<std::mutex> lock(g_prefetch_mutex);
        g_prefetched.emplace(src.file, std::move(bytes));
    }
}
