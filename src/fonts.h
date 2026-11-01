// Font loading.
//
// Dear ImGui 1.92+ has *dynamic* font sizes: we load each font once and can
// then draw it at any pixel size (glyphs are rasterised on demand into the
// atlas). So headings just use the same bold face at a bigger size.
//
// The default faces (IBM Plex Sans x4, Cascadia Code, and egui's emoji/icon fallbacks)
// are embedded in the exe (resources.h). The config file can replace the
// prose and mono faces with any TTF/OTF/TTC face, given by path or by family
// name ([fonts] in config.toml, spec 8 / 8a; names: font_index.h).
// Variable fonts: FreeType loads their default instance only.
//
// Sizes: everything in the layout code is in *em* (like egui / the Rust
// version), but ImGui scales a font so that ascent+descent == the size you ask
// for. `px()` converts, using each face's own (ascender - descender) /
// units-per-em, read from the font file: 1.3 for IBM Plex Sans, 1.367 for Cascadia Code.
#pragma once

#include <string>
#include <string_view>
#include <vector>

struct ImFont;

// One face to load: a font file (UTF-8, already ~/%VAR%-expanded) and the
// face inside it (a .ttc holds several; ImGui: ImFontConfig::FontNo).
// file "" = the built-in face for that slot.
struct FontSource {
    std::string file;
    int face = 0;
    bool builtin() const { return file.empty(); }
    bool operator==(const FontSource&) const = default;
};

// The five faces to load (resolve_fonts in font_index.h turns the [fonts]
// config values, paths or family names, into these).
struct FontFiles {
    FontSource prose, prose_bold, prose_italic, prose_bold_italic, mono;
    // Installed fonts for characters no other font has (spec 30; sysfont.h): merged last into
    // every face, so they only draw what nothing else does.
    std::vector<FontSource> system;
    bool operator==(const FontFiles&) const = default;
};

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    ImFont* italic = nullptr;
    ImFont* bold_italic = nullptr;
    ImFont* mono = nullptr;
    // Plex Regular with whole-pixel advances, for ImGui's own widgets (toolbar, popups...).
    // The five document faces above have exact (fractional) advances like egui's, and the
    // document code places each glyph on a whole pixel itself (inline_layout.h draw_text).
    ImFont* ui = nullptr;

    float scale = 1.0f;  // pixels per point: monitor DPI scale x zoom
    float body = 16.0f;  // body text size in em-pixels (body points x `scale`)

    // (ascender - descender) / units-per-em of each face, from its file.
    float regular_em = 1.3f, bold_em = 1.3f, italic_em = 1.3f, bold_italic_em = 1.3f, mono_em = 1.164f;

    // descender / (ascender - descender) of each face: the line-box split egui uses (inline_layout.cpp font_metrics).
    float regular_df = 0.2115f, bold_df = 0.2115f, italic_df = 0.2115f, bold_italic_df = 0.2115f, mono_df = 0.2f;

    // Problems loading custom fonts (shown in the error bar).
    std::vector<std::string> errors;

    // Pick the face for a bold/italic combination.
    ImFont* face(bool is_bold, bool is_italic) const;

    // The ImGui size that makes `font` render (about) `em` pixels per em.
    //
    // Rounded to whole pixels: ImGui bakes glyphs only at whole-pixel sizes
    // (ImGui::GetRoundedFontSize), and drawing at any other size scales every
    // glyph quad by size/baked (e.g. 20.8/21), which puts glyphs between
    // pixels and makes the GPU blur them. At a whole-pixel size the scale is
    // exactly 1 and glyph bitmaps land 1:1 on the screen's pixels.
    float px(const ImFont* font, float em) const;

    // Ratio (ascender - descender) / units-per-em for `font` (one of the five faces).
    float em_ratio(const ImFont* font) const;

    // `px()` rounds the size to whole pixels, which would make text up to 1 % wider or narrower
    // than egui's. Multiplying every advance by this factor (exact em size / rounded size)
    // gives egui's layout back, while the glyph bitmaps stay crisp at whole-pixel sizes.
    float advance_scale(const ImFont* font, float em) const;
    float desc_fraction(const ImFont* font) const;
};

// (ascender - descender) / units-per-em of face `face` of a TTF/OTF/TTC file
// (`face` only matters for a .ttc), the way FreeType (and so ImGui) sizes it:
// hhea metrics, else OS/2 typo, else OS/2 win. 0 if the data isn't a usable
// font or has no such face.
float font_em_ratio(std::string_view data, int face = 0);

// descender / (ascender - descender) of the same face and metrics (0 if unusable).
float font_desc_fraction(std::string_view data, int face = 0);

// Whether glyphs get the dark-theme coverage curve (the `dark` of the last load_fonts).
bool fonts_coverage_boost();

// Load the faces. `dark` picks the coverage boost egui uses for light-on-dark
// text. Call between frames, after clearing the atlas (io.Fonts->Clear()) when
// reloading (theme or font change). `scale` and `body` are set by the caller.
// A file that can't be used falls back (prose styles: the custom regular face
// if that loaded, else the built-in one) and adds a message to `errors`.
Fonts load_fonts(const FontFiles& files, bool dark);

// Read font files into memory ahead of load_fonts, which then finds them there instead of reading them on the
// thread that draws (spec 30). Safe to call from any thread, at any time (also while load_fonts runs elsewhere).
// Files that cannot be read are left out: load_fonts reports them as usual.
void prefetch_font_files(const std::vector<FontSource>& sources);
