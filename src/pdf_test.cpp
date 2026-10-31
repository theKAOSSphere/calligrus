// PDF writer tests (spec 33), console only: CID font objects, the subset font, ToUnicode, the content
// stream, the PNG "Up" predictor, as pdf.rs's tests. Uses the bundled IBM Plex Sans.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <hb.h>

#define STB_IMAGE_IMPLEMENTATION  // (only for its zlib decoder: a tiny inflate for the checks)
#define STBI_ONLY_PNG
#include <stb_image.h>

#include "pdf.h"
#include "resources.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

// The inflated stream of object `id`.
static std::string object_stream(const std::string& pdf, int id) {
    const size_t at = pdf.find("\n" + std::to_string(id) + " 0 obj");
    if (at == std::string::npos) return "";
    const size_t s = pdf.find("stream\n", at) + 7, e = pdf.find("\nendstream", s);
    int n = 0;
    char* out = stbi_zlib_decode_malloc(pdf.data() + s, int(e - s), &n);
    if (!out) return "";
    std::string r(out, size_t(n));
    free(out);
    return r;
}

static bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

int main() {
    const std::string_view plex = *resource("fonts/IBMPlexSans-Regular.ttf");
    hb_blob_t* blob = hb_blob_create(plex.data(), unsigned(plex.size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    hb_face_t* face = hb_face_create(blob, 0);
    hb_font_t* hf = hb_font_create(face);
    const unsigned upem = hb_face_get_upem(face);
    hb_font_set_scale(hf, int(upem), int(upem));  // positions in font units
    auto gid = [&](unsigned cp) {
        hb_codepoint_t g = 0;
        hb_font_get_nominal_glyph(hf, cp, &g);
        return unsigned(g);
    };
    auto advance = [&](unsigned g, float size) { return float(hb_font_get_glyph_h_advance(hf, g)) * size / float(upem); };

    auto glyph = [](size_t font, unsigned g, float x, const char* text) {
        pdf::Glyph out;
        out.font = font, out.gid = g, out.x = x, out.y = 30, out.size = 20, out.rgb[0] = 255, out.text = text;
        return out;
    };
    auto text_page = [](std::vector<pdf::Glyph> glyphs) {
        pdf::Page p;
        p.width = 100, p.height = 50, p.rgb.assign(100 * 50 * 3, 255), p.predictor = true, p.glyphs = std::move(glyphs);
        return p;
    };
    // hb-subset numbers the kept glyphs in their old order (.notdef is 0): the subset id of `g`, as 4 hex digits.
    auto new_id = [](unsigned a, unsigned b, unsigned c, unsigned g) {
        std::vector<unsigned> v = {a, b, c};
        std::sort(v.begin(), v.end());
        char buf[8];
        std::snprintf(buf, sizeof buf, "%04X", unsigned(std::find(v.begin(), v.end(), g) - v.begin()) + 1);
        return std::string(buf);
    };
    const std::vector<pdf::Font> fonts = {{"IBM Plex Sans", plex, 0}};

    // The PNG "Up" predictor: filter bytes, differences, and back (the way a reader undoes it).
    {
        std::vector<unsigned char> rgb(27);
        for (size_t i = 0; i < rgb.size(); i++) rgb[i] = static_cast<unsigned char>(i * 37 % 251);
        const std::vector<unsigned char> f = pdf::up_filtered(rgb, 3);
        CHECK(f.size() == 30 && f[0] == 2 && f[10] == 2);
        std::vector<unsigned char> back;
        for (size_t n = 0; n < 3; n++)
            for (size_t i = 0; i < 9; i++) back.push_back(static_cast<unsigned char>(f[n * 10 + 1 + i] + (n == 0 ? 0 : back[(n - 1) * 9 + i])));
        CHECK(back == rgb);
        // through the writer: the stream inflates to the filtered bytes, and a flat page is tiny
        const std::string doc = pdf::write({text_page({})}, 0.75f);
        CHECK(has(doc, "/DecodeParms << /Predictor 15 /Colors 3 /BitsPerComponent 8 /Columns 100 >>"));
        const std::string pic = object_stream(doc, 5);
        CHECK(pic.size() == 50 * 301 && pic[0] == 2);
        CHECK(doc.size() < 1500);
    }
    // ToUnicode: clusters, a pair outside the BMP, no entry for a glyph without text, blocks of 100.
    {
        std::map<unsigned, std::string> m = {{1, "a"}, {2, "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7"}, {3, "\xF0\x9F\x98\x80"}, {4, ""}};
        const std::string c = pdf::to_unicode_cmap(m);
        CHECK(has(c, "3 beginbfchar\n<0001> <0061>\n<0002> <0915094D0937>\n<0003> <D83DDE00>\nendbfchar"));
        CHECK(has(c, "<0000> <FFFF>"));
        std::map<unsigned, std::string> many;
        for (unsigned g = 1; g <= 250; g++) many[g] = "x";
        const std::string d = pdf::to_unicode_cmap(many);
        CHECK(has(d, "100 beginbfchar") && has(d, "50 beginbfchar"));
    }
    // CID font objects, the subset font, /W, ToUnicode.
    {
        const unsigned h = gid('H'), i = gid('i'), t = gid('t');
        const std::string doc = pdf::write({text_page({glyph(0, h, 10, "H"), glyph(0, i, 24, "i"), glyph(0, t, 90, "t")})}, 0.75f, fonts);
        CHECK(!doc.empty());
        CHECK(has(doc, "/Subtype /Type0 /BaseFont /AAAAAA+IBMPlexSans") && has(doc, "/Encoding /Identity-H"));
        CHECK(has(doc, "/Subtype /CIDFontType2") && has(doc, "/CIDToGIDMap /Identity") && has(doc, "/FontFile2") && has(doc, "/ToUnicode"));
        CHECK(has(doc, "/Font << /F0 3 0 R >>"));
        // /W: a width for each glyph of the subset (.notdef + H i t)
        const size_t w = doc.find("/W [0 [") + 7;
        const std::string list = doc.substr(w, doc.find(']', w) - w);
        int count = 1;
        for (char c : list) count += c == ' ';
        CHECK(count == 4);
        // the subset is a real, much smaller font with 4 glyphs
        const std::string font = object_stream(doc, 6);
        CHECK(!font.empty() && font.size() < plex.size() / 4);
        hb_blob_t* fb = hb_blob_create(font.data(), unsigned(font.size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
        hb_face_t* ff = hb_face_create(fb, 0);
        CHECK(hb_face_get_glyph_count(ff) == 4);
        hb_face_destroy(ff);
        hb_blob_destroy(fb);
        // ToUnicode (object 7): the subset's glyphs map to their characters (under their new ids)
        const std::string cmap = object_stream(doc, 7);
        CHECK(has(cmap, "3 beginbfchar\n"));
        CHECK(has(cmap, "<" + new_id(h, i, t, h) + "> <0048>") && has(cmap, "<" + new_id(h, i, t, i) + "> <0069>") && has(cmap, "<" + new_id(h, i, t, t) + "> <0074>"));
    }
    // The content stream: "Hi" on one baseline with the natural advance + 1 px, then a far word.
    {
        const unsigned h = gid('H'), i = gid('i'), t = gid('t');
        const std::string doc = pdf::write({text_page({glyph(0, h, 10, "H"), glyph(0, i, 10 + advance(h, 20) + 1, "i"), glyph(0, t, 95, "t")})}, 0.75f, fonts);
        const std::string content = object_stream(doc, 9);
        CHECK(content.starts_with("q 75.00 0 0 37.50 0 0 cm /Im0 Do Q\n"));
        // 20 px = 15 pt; "H" at 10 px = 7.5 pt; baseline 30 px from the top = 15 pt from the bottom
        CHECK(has(content, "BT /F0 15 Tf 1 0 0 rg\n1 0 0 1 7.5 15 Tm\n[<" + new_id(h, i, t, h) + "> -50 <" + new_id(h, i, t, i) + ">] TJ\nET"));
        CHECK(has(content, "1 0 0 1 71.25 15 Tm\n[<" + new_id(h, i, t, t) + ">] TJ\nET"));
    }
    // A cluster of two glyphs for "Hi" (the second raised 5 px) is spelled out with /ActualText, and adds nothing to
    // ToUnicode; a word gap gets a space glyph; touching or far apart glyphs do not.
    {
        const unsigned h = gid('H'), i = gid('i'), t = gid('t');
        pdf::Glyph raised = glyph(0, i, 10, "");
        raised.y -= 5;
        const std::string doc = pdf::write({text_page({glyph(0, h, 10, "Hi"), raised, glyph(0, t, 95, "t")})}, 0.75f, fonts);
        const std::string content = object_stream(doc, 9);
        CHECK(has(content, "/Span << /ActualText <FEFF00480069> >> BDC\n1 0 0 1 7.5 15 Tm\n[<" + new_id(h, i, t, h) + ">] TJ\n1 0 0 1 7.5 18.75 Tm\n[<" +
                               new_id(h, i, t, i) + ">] TJ\nEMC\n"));
        CHECK(has(object_stream(doc, 7), "1 beginbfchar\n<" + new_id(h, i, t, t) + "> <0074>\nendbfchar"));
        const float hw = advance(h, 20);
        const std::string spaced = pdf::write({text_page({glyph(0, h, 10, "H"), glyph(0, i, 10 + hw + 6, "i")})}, 0.75f, fonts);
        CHECK(has(object_stream(spaced, 7), "<0020>") && has(object_stream(spaced, 7), "3 beginbfchar"));
        const std::string touching = pdf::write({text_page({glyph(0, h, 10, "H"), glyph(0, i, 10 + hw, "i")})}, 0.75f, fonts);
        CHECK(!has(object_stream(touching, 7), "<0020>"));
    }
    // Translucent text (alpha under 255) is drawn through an /ExtGState with /ca, only for the runs that need it.
    {
        const unsigned h = gid('H'), i = gid('i');
        pdf::Glyph half = glyph(0, h, 10, "H");
        half.alpha = 128;
        const std::string doc = pdf::write({text_page({half, glyph(0, i, 90, "i")})}, 0.75f, fonts);
        CHECK(has(doc, "/ExtGState << /GS128 << /Type /ExtGState /ca 0.502 >> >>"));
        const std::string content = object_stream(doc, 9);
        const std::string hid = h < i ? "0001" : "0002";  // (the subset numbers the kept glyphs in their old order)
        CHECK(has(content, "q /GS128 gs\nBT /F0 15 Tf 1 0 0 rg\n1 0 0 1 7.5 15 Tm\n[<" + hid + ">] TJ\nET\nQ\nBT /F0 15 Tf 1 0 0 rg"));
        const std::string opaque = pdf::write({text_page({glyph(0, h, 10, "H")})}, 0.75f, fonts);
        CHECK(!has(opaque, "ExtGState"));
    }
    // The math font has CFF outlines: a CIDFontType0 with an OpenType /FontFile3, still cut down.
    {
        const std::string_view math = *resource("fonts/latinmodern-math.otf");
        hb_blob_t* mb = hb_blob_create(math.data(), unsigned(math.size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
        hb_face_t* mf = hb_face_create(mb, 0);
        hb_font_t* mh = hb_font_create(mf);
        hb_codepoint_t x = 0, plus = 0;
        hb_font_get_nominal_glyph(mh, 'x', &x);
        hb_font_get_nominal_glyph(mh, '+', &plus);
        const std::vector<pdf::Font> mfonts = {{"LatinModernMath-Regular", math, 0}};
        const std::string doc = pdf::write({text_page({glyph(0, x, 10, "x"), glyph(0, plus, 40, "+")})}, 0.75f, mfonts);
        CHECK(!doc.empty());
        CHECK(has(doc, "/Subtype /CIDFontType0 /BaseFont /AAAAAA+LatinModernMath-Regular") && has(doc, "/FontFile3") && has(doc, "/Subtype /OpenType"));
        CHECK(!has(doc, "/CIDToGIDMap"));
        const std::string font = object_stream(doc, 6);
        CHECK(!font.empty() && font.size() < math.size() / 10);
        hb_blob_t* fb = hb_blob_create(font.data(), unsigned(font.size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
        hb_face_t* ff = hb_face_create(fb, 0);
        CHECK(hb_face_get_glyph_count(ff) == 4);  // .notdef, x, +, and the word space between them
        hb_face_destroy(ff);
        hb_blob_destroy(fb);
        hb_font_destroy(mh);
        hb_face_destroy(mf);
        hb_blob_destroy(mb);
    }
    // A font that cannot be read is an error (the caller then falls back to the picture-only PDF).
    {
        std::string err;
        const std::vector<pdf::Font> bad = {{"x", std::string_view("abc", 3), 0}};
        CHECK(pdf::write({text_page({glyph(0, 1, 0, "a")})}, 0.75f, bad, &err).empty() && !err.empty());
        CHECK(!pdf::can_embed("abc", 0) && pdf::can_embed(plex, 0));
        CHECK(pdf::can_embed(*resource("fonts/latinmodern-math.otf"), 0));  // CFF outlines: embedded as /FontFile3
    }
    hb_font_destroy(hf);
    hb_face_destroy(face);
    hb_blob_destroy(blob);
    if (failures) std::printf("%d checks failed\n", failures);
    else std::printf("pdf: all checks passed\n");
    return failures ? 1 : 0;
}
