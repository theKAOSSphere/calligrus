// PDF export (specs 26, 29, 33): pages made of a picture and, optionally, real text on top, as pdf.rs
// writes them.
//
// The smallest PDF that does the job: a catalog, a page tree, and per page a page object, its
// content stream and the picture, RGB compressed with zlib (/FlateDecode).
//
// Since spec 33 a page can also carry text: the picture then has no text in it (flat colours,
// which the PNG "Up" predictor shrinks to almost nothing) and the glyphs are drawn over it as
// real PDF text, so they can be selected, searched and copied. Each source font that drew a
// glyph is embedded once, cut down to the glyphs used (HarfBuzz hb-subset), as a /Type0 font
// with a /CIDFontType2 descendant (/Identity-H: the two bytes of a character code are the glyph
// id), its widths (/W) and a /ToUnicode CMap that says which characters each glyph stands for
// (so copy and search give the real text, a conjunct's several characters included).
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pdf {

// A source font to embed: its file (must stay valid until write() returns), which face of it
// (a .ttc has several) and a name for the PDF (/BaseFont; "" = the font's own PostScript name).
struct Font {
    std::string name;
    std::string_view bytes;
    int face = 0;
};

// One glyph of a page's text layer. Distances are in the page picture's pixels.
struct Glyph {
    size_t font = 0;      // which of the writer's fonts
    unsigned gid = 0;     // the glyph's id in that font's file
    float x = 0, y = 0;   // its baseline origin, from the picture's top left corner (y grows down)
    float size = 0;       // font size: pixels per em
    unsigned char rgb[3] = {0, 0, 0};
    unsigned char alpha = 255;  // opacity, 255 = opaque (drawn through an /ExtGState with /ca when less)
    float skew = 0;       // fake italics: the glyph is sheared by this much (x moves by skew times the height)
    std::string text;     // the characters it stands for (UTF-8): the whole cluster's on its first glyph, empty on the others
};

// One page: a width x height picture, its pixels RGB (3 bytes each, rows top down).
struct Page {
    int width = 0, height = 0;
    std::vector<unsigned char> rgb;
    // The page's size in points when it is not the picture's size times the writer's points per
    // pixel (a document export: the paper size, which the picture only approximates); 0 = not set.
    float width_pt = 0, height_pt = 0;
    bool predictor = false;     // write the picture with the PNG "Up" predictor (a picture without text)
    std::vector<Glyph> glyphs;  // the text layer, in drawing order
};

// RGBA pixels as OpenGL reads them (rows bottom up) to RGB rows top down, dropping alpha.
std::vector<unsigned char> rgb_from_gl_rgba(const std::vector<unsigned char>& rgba, int width, int height);

// The PDF file holding `pages`, with `fonts` for their text layers; each page is `points_per_pixel`
// points per picture pixel in size (0.75 = 96 pixels per inch). "" (and `*error`): a font could not be embedded.
std::string write(const std::vector<Page>& pages, float points_per_pixel, const std::vector<Font>& fonts = {},
                  std::string* error = nullptr);

// zlib-compress `data` (exposed for the tests).
std::string zlib(const std::vector<unsigned char>& data);

// The picture's bytes before compression, with the PNG "Up" filter: every row starts with the filter
// type 2 and holds each byte minus the one above it (a PDF reader undoes it with /Predictor 15).
std::vector<unsigned char> up_filtered(const std::vector<unsigned char>& rgb, int width);

// The /ToUnicode CMap: for each glyph id of the (subset) font, the characters (UTF-8) it stands for.
std::string to_unicode_cmap(const std::map<unsigned, std::string>& map);

// Whether a font can be embedded by this writer: a readable font with TrueType outlines (/FontFile2) or CFF
// outlines in an OpenType file (/FontFile3, e.g. the math font). Others stay in the picture.
bool can_embed(std::string_view bytes, int face);

}  // namespace pdf
