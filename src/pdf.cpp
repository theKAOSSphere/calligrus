#include "pdf.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <zlib.h>

#include <hb-ot.h>
#include <hb-subset.h>
#include <hb.h>


namespace pdf {

std::vector<unsigned char> rgb_from_gl_rgba(const std::vector<unsigned char>& rgba, int width, int height) {
    std::vector<unsigned char> out(size_t(width) * height * 3);
    for (int y = 0; y < height; y++) {
        const unsigned char* src = rgba.data() + size_t(height - 1 - y) * width * 4;
        unsigned char* dst = out.data() + size_t(y) * width * 3;
        for (int x = 0; x < width; x++) dst[x * 3] = src[x * 4], dst[x * 3 + 1] = src[x * 4 + 1], dst[x * 3 + 2] = src[x * 4 + 2];
    }
    return out;
}

std::string zlib(const std::vector<unsigned char>& data) {
    // zlib at its best level: the pictures are mostly flat, which a good Huffman stage squeezes far more
    // than a fast one (stb's fixed codes made a flat A4 page 8 times bigger).
    uLongf n = compressBound(uLong(data.size()));
    std::string out(n, 'x');
    if (compress2(reinterpret_cast<Bytef*>(out.data()), &n, data.data(), uLong(data.size()), 9) != Z_OK) return std::string();
    out.resize(n);
    return out;
}

std::vector<unsigned char> up_filtered(const std::vector<unsigned char>& rgb, int width) {
    const size_t row = size_t(width) * 3;
    std::vector<unsigned char> out;
    if (row == 0) return out;
    out.reserve(rgb.size() + rgb.size() / row);
    for (size_t n = 0; (n + 1) * row <= rgb.size(); n++) {
        out.push_back(2);
        for (size_t i = 0; i < row; i++) {
            const unsigned char above = n == 0 ? 0 : rgb[(n - 1) * row + i];
            out.push_back(static_cast<unsigned char>(rgb[n * row + i] - above));
        }
    }
    return out;
}

namespace {

// A number for a content stream: at most `digits` decimals, no trailing zeros.
std::string num(double v, int digits) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

std::string hex4(unsigned v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04X", v & 0xFFFF);
    return buf;
}

// UTF-8 to UTF-16 big endian hex (what a ToUnicode entry holds).
std::string utf16_hex(const std::string& utf8) {
    std::string out;
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        unsigned cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
        for (int k = 1; k < n && i + k < utf8.size(); k++) cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        i += size_t(n);
        if (cp >= 0x10000) {  // a surrogate pair
            cp -= 0x10000;
            out += hex4(0xD800 + (cp >> 10)) + hex4(0xDC00 + (cp & 0x3FF));
        } else {
            out += hex4(cp);
        }
    }
    return out;
}

template <class T, void (*Destroy)(T*)>
struct Deleter {
    void operator()(T* p) const { Destroy(p); }
};
using Face = std::unique_ptr<hb_face_t, Deleter<hb_face_t, hb_face_destroy>>;
using HbFont = std::unique_ptr<hb_font_t, Deleter<hb_font_t, hb_font_destroy>>;
using Blob = std::unique_ptr<hb_blob_t, Deleter<hb_blob_t, hb_blob_destroy>>;

Face open_face(std::string_view bytes, int index) {
    Blob blob(hb_blob_create(bytes.data(), unsigned(bytes.size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr));
    return Face(hb_face_create(blob.get(), unsigned(index)));
}

int16_t s16(const char* p) { return int16_t((unsigned char)p[0] << 8 | (unsigned char)p[1]); }

// A font cut down to its used glyphs, and what the PDF needs to know about it.
struct Embedded {
    std::string file;                   // the subset font file
    std::map<unsigned, unsigned> remap; // old glyph id -> glyph id in the subset
    std::vector<unsigned> widths;       // advance of each subset glyph in 1/1000 em, by the new id
    int ascent = 0, descent = 0, bbox[4] = {0, 0, 0, 0};
    bool italic = false;
    bool cff = false;                   // CFF outlines (an OpenType file for /FontFile3), else TrueType (/FontFile2)
    std::string name;                   // the PostScript name (without subset tag)
};

// Cut `font` down to `used` glyphs (their ids change: `remap` says how). false: failed (`error` says why).
bool embed(const Font& font, const std::vector<unsigned>& used, Embedded& out, std::string& error) {
    Face face = open_face(font.bytes, font.face);
    if (!face || hb_face_get_glyph_count(face.get()) == 0) return error = "cannot read a font", false;
    const unsigned upem = std::max(1u, hb_face_get_upem(face.get()));
    HbFont hf(hb_font_create(face.get()));
    hb_font_set_scale(hf.get(), int(upem), int(upem));

    hb_subset_input_t* in = hb_subset_input_create_or_fail();
    if (!in) return error = "cannot subset a font", false;
    hb_set_add(hb_subset_input_glyph_set(in), 0);  // .notdef
    for (unsigned g : used) hb_set_add(hb_subset_input_glyph_set(in), g);
    // PDF needs only the outlines and metrics: no layout tables, no hinting programs.
    hb_set_t* drop = hb_subset_input_set(in, HB_SUBSET_SETS_DROP_TABLE_TAG);
    for (hb_tag_t t : {HB_TAG('G', 'S', 'U', 'B'), HB_TAG('G', 'P', 'O', 'S'), HB_TAG('G', 'D', 'E', 'F')}) hb_set_add(drop, t);
    hb_subset_input_set_flags(in, HB_SUBSET_FLAGS_NO_HINTING);
    hb_subset_plan_t* plan = hb_subset_plan_create_or_fail(face.get(), in);
    hb_face_t* sub = plan ? hb_subset_plan_execute_or_fail(plan) : nullptr;
    if (!sub) {
        if (plan) hb_subset_plan_destroy(plan);
        hb_subset_input_destroy(in);
        return error = "cannot subset a font", false;
    }
    const hb_map_t* map = hb_subset_plan_old_to_new_glyph_mapping(plan);
    out.widths.assign(hb_face_get_glyph_count(sub), 0);
    for (unsigned old = 0, n = hb_face_get_glyph_count(face.get()); old < n; old++) {
        if (!hb_map_has(map, old)) continue;
        const unsigned nw = hb_map_get(map, old);
        out.remap[old] = nw;
        if (nw < out.widths.size()) out.widths[nw] = unsigned(std::max<hb_position_t>(0, std::lround(double(hb_font_get_glyph_h_advance(hf.get(), old)) * 1000.0 / upem)));
    }
    Blob blob(hb_face_reference_blob(sub));
    unsigned len = 0;
    const char* data = hb_blob_get_data(blob.get(), &len);
    out.file.assign(data, len);
    hb_face_destroy(sub);
    hb_subset_plan_destroy(plan);
    hb_subset_input_destroy(in);

    hb_font_extents_t ext{};
    hb_font_get_h_extents(hf.get(), &ext);
    out.ascent = int(std::lround(double(ext.ascender) * 1000.0 / upem));
    out.descent = int(std::lround(double(ext.descender) * 1000.0 / upem));
    // head: xMin, yMin, xMax, yMax at 36..43, macStyle (bit 1: italic) at 44.
    Blob head(hb_face_reference_table(face.get(), HB_TAG('h', 'e', 'a', 'd')));
    unsigned hl = 0;
    const char* h = hb_blob_get_data(head.get(), &hl);
    if (hl >= 46) {
        for (int i = 0; i < 4; i++) out.bbox[i] = int(std::lround(double(s16(h + 36 + 2 * i)) * 1000.0 / upem));
        out.italic = (s16(h + 44) & 2) != 0;
    }
    Blob glyf(hb_face_reference_table(face.get(), HB_TAG('g', 'l', 'y', 'f')));
    out.cff = hb_blob_get_length(glyf.get()) == 0;
    char name[128] = {};
    unsigned n = sizeof name - 1;
    hb_ot_name_get_utf8(face.get(), HB_OT_NAME_ID_POSTSCRIPT_NAME, HB_LANGUAGE_INVALID, &n, name);
    out.name = name;
    return true;
}

// `AAAAAB+Name`: the subset tag PDF wants in front of an embedded font's name (unique per font).
std::string subset_name(size_t index, const std::string& name) {
    std::string tag(6, 'A');
    for (size_t i = 6, n = index; i-- > 0; n /= 26) tag[i] = char('A' + n % 26);
    std::string clean;
    for (char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-') clean += c;
    return tag + "+" + (clean.empty() ? "Font" : clean);
}

// The clusters of a glyph list: ranges of glyphs that stand for one stretch of text. A cluster starts at a
// glyph with text (or the first glyph) and takes the glyphs after it that have none.
std::vector<std::pair<size_t, size_t>> clusters(const std::vector<Glyph>& glyphs) {
    std::vector<std::pair<size_t, size_t>> out;
    for (size_t n = 0; n < glyphs.size(); n++) {
        if (!out.empty() && glyphs[n].text.empty()) out.back().second = n + 1;
        else out.push_back({n, n + 1});
    }
    return out;
}

// A cluster needs its text spelled out (/ActualText) when its glyphs and characters do not pair up one to
// one: a glyph's ToUnicode entry is the same wherever it is used, but e.g. an "i" vowel sign belongs to a
// different consonant each time.
bool needs_actual_text(const std::vector<Glyph>& glyphs, std::pair<size_t, size_t> cluster) {
    if (cluster.second - cluster.first > 1) return true;
    int chars = 0;
    for (unsigned char c : glyphs[cluster.first].text) chars += (c & 0xC0) != 0x80;
    return chars > 1;
}

// `glyphs` with a space glyph added where two clusters on one baseline in one font, size and colour are a
// word space apart (a gap of more than 0.15 em, up to two ems). The space goes at the end of the
// first one, so reading order and copy see "word word" (the layout has no space: a word is a piece of text
// of its own).
std::vector<Glyph> with_spaces(const std::vector<Glyph>& glyphs, const std::vector<Font>& fonts) {
    struct Opened {
        Face face;
        HbFont font;
    };
    std::vector<Opened> opened(fonts.size());
    auto open = [&](size_t f) -> hb_font_t* {
        if (!opened[f].font) {
            opened[f].face = open_face(fonts[f].bytes, fonts[f].face);
            opened[f].font = HbFont(hb_font_create(opened[f].face.get()));
            const int upem = int(std::max(1u, hb_face_get_upem(opened[f].face.get())));
            hb_font_set_scale(opened[f].font.get(), upem, upem);
        }
        return opened[f].font.get();
    };
    auto advance = [&](const Glyph& g) {
        hb_font_t* f = open(g.font);
        return double(hb_font_get_glyph_h_advance(f, g.gid)) * g.size / std::max(1u, hb_face_get_upem(hb_font_get_face(f)));
    };
    std::vector<Glyph> out;
    const auto all = clusters(glyphs);
    for (size_t n = 0; n < all.size(); n++) {
        double end = -1e30;
        for (size_t k = all[n].first; k < all[n].second; k++) {
            out.push_back(glyphs[k]);
            end = std::max(end, glyphs[k].x + advance(glyphs[k]));
        }
        if (n + 1 == all.size()) break;
        const Glyph &a = glyphs[all[n].first], &b = glyphs[all[n + 1].first];
        if (a.font != b.font || a.size != b.size || std::memcmp(a.rgb, b.rgb, 3) != 0 || a.alpha != b.alpha || std::fabs(a.y - b.y) > 0.01f || a.text == " ") continue;
        const double gap = b.x - end;
        hb_codepoint_t space = 0;
        if (gap > 0.15 * a.size && gap < 2.0 * a.size && hb_font_get_nominal_glyph(open(a.font), ' ', &space) && space != 0) {
            Glyph s = a;
            s.gid = space, s.x = float(end), s.text = " ";
            out.push_back(std::move(s));
        }
    }
    return out;
}

// The text layer of one page (`glyphs`) as a content stream, for a page of w x h points whose picture is
// page.width x page.height pixels. `ids[font]` maps old to subset glyph ids, `widths[font]` are the subset
// glyphs' advances (1/1000 em).
std::string text_stream(const Page& page, const std::vector<Glyph>& glyphs, double w, double h, const std::vector<std::map<unsigned, unsigned>>& ids,
                        const std::vector<std::vector<unsigned>>& widths) {
    const double kx = w / std::max(1, page.width), ky = h / std::max(1, page.height);  // pixels -> points
    std::string out;
    // The advance the PDF itself makes after a glyph, in pixels.
    auto advance = [&](const Glyph& g) { return double(widths[g.font][ids[g.font].at(g.gid)]) * g.size / 1000.0; };
    const auto all = clusters(glyphs);
    size_t c = 0;
    while (c < all.size()) {
        // A run: clusters one after another in one font, size and colour on one baseline: one BT.
        const Glyph& first = glyphs[all[c].first];
        double pen = -1e30;
        auto take = [&](size_t cl) {
            for (size_t k = all[cl].first; k < all[cl].second; k++) pen = std::max(pen, glyphs[k].x + advance(glyphs[k]));
        };
        take(c);
        size_t end = c + 1;
        while (end < all.size()) {
            const Glyph& g = glyphs[all[end].first];
            const bool same = g.font == first.font && g.size == first.size && std::memcmp(g.rgb, first.rgb, 3) == 0 && g.alpha == first.alpha && g.skew == first.skew &&
                              std::fabs(g.y - first.y) < 0.01f;
            // A gap of more than a tenth of an em starts a new run.
            if (!same || std::fabs(g.x - pen) > 0.1 * g.size) break;
            take(end++);
        }
        if (first.alpha < 255) out += "q /GS" + std::to_string(first.alpha) + " gs\n";
        out += "BT /F" + std::to_string(first.font) + " " + num(first.size * kx, 3) + " Tf " + num(first.rgb[0] / 255.0, 3) + " " + num(first.rgb[1] / 255.0, 3) +
               " " + num(first.rgb[2] / 255.0, 3) + " rg\n";
        bool open = false;  // inside a [ ... ] TJ
        double line_y = 0;
        const Glyph* prev = nullptr;
        for (size_t cl = c; cl < end; cl++) {
            const bool actual = needs_actual_text(glyphs, all[cl]);
            if (actual) {
                if (open) out += "] TJ\n", open = false;
                out += "/Span << /ActualText <FEFF" + utf16_hex(glyphs[all[cl].first].text) + "> >> BDC\n";
            }
            for (size_t n = all[cl].first; n < all[cl].second; n++) {
                const Glyph& g = glyphs[n];
                if (!prev || std::fabs(g.y - line_y) > 0.01) {  // another baseline (a mark above or below): a new text position
                    if (open) out += "] TJ\n";
                    out += "1 0 " + num(first.skew, 3) + " 1 " + num(g.x * kx, 2) + " " + num(h - g.y * ky, 2) + " Tm\n[";
                    open = true, line_y = g.y;
                } else {
                    if (!open) out += "[", open = true;
                    // The previous glyph's own advance moved the pen; a correction (thousandths of the size,
                    // positive moves left) puts this one where the layout had it.
                    const double adj = (advance(*prev) - (g.x - prev->x)) * 1000.0 / first.size;
                    if (std::fabs(adj) >= 0.05) out += " " + num(adj, 1) + " ";
                }
                out += "<" + hex4(ids[g.font].at(g.gid)) + ">";
                prev = &g;
            }
            if (actual) out += "] TJ\nEMC\n", open = false;
        }
        if (open) out += "] TJ\n";
        out += "ET\n";
        if (first.alpha < 255) out += "Q\n";
        c = end;
    }
    return out;
}

std::string zlib_str(const std::string& s) { return zlib(std::vector<unsigned char>(s.begin(), s.end())); }

}  // namespace

std::string to_unicode_cmap(const std::map<unsigned, std::string>& map) {
    std::string out =
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    std::vector<std::pair<unsigned, const std::string*>> entries;
    for (const auto& [gid, text] : map)
        if (!text.empty()) entries.push_back({gid, &text});
    // (a bfchar block holds at most 100 entries)
    for (size_t at = 0; at < entries.size(); at += 100) {
        const size_t n = std::min<size_t>(100, entries.size() - at);
        out += std::to_string(n) + " beginbfchar\n";
        for (size_t k = at; k < at + n; k++) out += "<" + hex4(entries[k].first) + "> <" + utf16_hex(*entries[k].second) + ">\n";
        out += "endbfchar\n";
    }
    out += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    return out;
}

bool can_embed(std::string_view bytes, int face) {
    if (bytes.empty()) return false;
    Face f = open_face(bytes, face);
    if (!f || hb_face_get_glyph_count(f.get()) == 0) return false;
    Blob glyf(hb_face_reference_table(f.get(), HB_TAG('g', 'l', 'y', 'f')));
    Blob cff(hb_face_reference_table(f.get(), HB_TAG('C', 'F', 'F', ' ')));
    return hb_blob_get_length(glyf.get()) > 0 || hb_blob_get_length(cff.get()) > 0;
}

std::string write(const std::vector<Page>& pages, float points_per_pixel, const std::vector<Font>& fonts, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return std::string();
    };
    // The text layers, with a real space glyph between words.
    std::vector<std::vector<Glyph>> lists;
    for (const Page& p : pages) {
        for (const Glyph& g : p.glyphs)
            if (g.font >= fonts.size()) return fail("a glyph of an unknown font");
        lists.push_back(with_spaces(p.glyphs, fonts));
    }
    // The glyphs each font needs, over all pages, and what its subset will hold.
    std::vector<std::vector<unsigned>> used(fonts.size());
    for (const std::vector<Glyph>& list : lists)
        for (const Glyph& g : list) {
            std::vector<unsigned>& u = used[g.font];
            if (std::find(u.begin(), u.end(), g.gid) == u.end()) u.push_back(g.gid);
        }
    std::vector<Embedded> embedded(fonts.size());
    for (size_t n = 0; n < fonts.size(); n++) {
        std::string err;
        if (!embed(fonts[n], used[n], embedded[n], err)) return fail(err);
    }
    std::vector<std::map<unsigned, std::string>> unicode(fonts.size());
    std::vector<std::map<unsigned, unsigned>> ids;
    std::vector<std::vector<unsigned>> widths;
    for (const Embedded& e : embedded) ids.push_back(e.remap), widths.push_back(e.widths);
    // (only clusters of one glyph: a glyph of a bigger cluster means different text each time; those carry /ActualText)
    for (const std::vector<Glyph>& list : lists)
        for (const auto& cl : clusters(list)) {
            const Glyph& g = list[cl.first];
            auto it = ids[g.font].find(g.gid);
            if (it == ids[g.font].end()) return fail("a glyph the subset lost");
            if (cl.second - cl.first == 1 && !g.text.empty()) unicode[g.font].emplace(it->second, g.text);
        }

    // Objects: 1 catalog, 2 page tree, per font n five (Type0, CID font, descriptor, file,
    // ToUnicode), then per page k: page, content, picture.
    auto font_id = [](size_t n) { return 3 + 5 * n; };
    auto page_id = [&](size_t k) { return 3 + 5 * fonts.size() + 3 * k; };
    std::string out = "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n";
    std::vector<size_t> offsets;  // byte offset of each object, in id order
    auto object = [&](const std::string& head, const std::string* stream) {
        offsets.push_back(out.size());
        out += std::to_string(offsets.size()) + " 0 obj\n" + head + "\n";
        if (stream) out += "stream\n" + *stream + "\nendstream\n";
        out += "endobj\n";
    };
    object("<< /Type /Catalog /Pages 2 0 R >>", nullptr);
    std::string kids;
    for (size_t k = 0; k < pages.size(); k++) kids += (k ? " " : "") + std::to_string(page_id(k)) + " 0 R";
    object("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages.size()) + " >>", nullptr);
    char buf[64];
    for (size_t n = 0; n < fonts.size(); n++) {
        const Embedded& e = embedded[n];
        const size_t base = font_id(n);
        const std::string name = subset_name(n, fonts[n].name.empty() ? e.name : fonts[n].name);
        object("<< /Type /Font /Subtype /Type0 /BaseFont /" + name + " /Encoding /Identity-H /DescendantFonts [" + std::to_string(base + 1) +
                   " 0 R] /ToUnicode " + std::to_string(base + 4) + " 0 R >>",
               nullptr);
        std::string w;
        for (size_t g = 0; g < e.widths.size(); g++) w += (g ? " " : "") + std::to_string(e.widths[g]);
        object(std::string("<< /Type /Font /Subtype /") + (e.cff ? "CIDFontType0" : "CIDFontType2") + " /BaseFont /" + name +
                   " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor " + std::to_string(base + 2) +
                   " 0 R" + (e.cff ? "" : " /CIDToGIDMap /Identity") + " /DW 1000 /W [0 [" + w + "]] >>",
               nullptr);
        // Flags: 4 = symbolic (the glyphs are addressed by id, not by a standard encoding), +64 italic.
        object("<< /Type /FontDescriptor /FontName /" + name + " /Flags " + std::to_string(4 + (e.italic ? 64 : 0)) + " /FontBBox [" +
                   std::to_string(e.bbox[0]) + " " + std::to_string(e.bbox[1]) + " " + std::to_string(e.bbox[2]) + " " + std::to_string(e.bbox[3]) +
                   "] /ItalicAngle " + (e.italic ? "-12" : "0") + " /Ascent " + std::to_string(e.ascent) + " /Descent " + std::to_string(e.descent) +
                   " /CapHeight " + std::to_string(e.ascent) + " /StemV 80 /" + (e.cff ? "FontFile3 " : "FontFile2 ") + std::to_string(base + 3) + " 0 R >>",
               nullptr);
        const std::string file = zlib_str(e.file);
        if (e.cff) object("<< /Length " + std::to_string(file.size()) + " /Subtype /OpenType /Filter /FlateDecode >>", &file);
        else object("<< /Length " + std::to_string(file.size()) + " /Length1 " + std::to_string(e.file.size()) + " /Filter /FlateDecode >>", &file);
        const std::string cmap = zlib_str(to_unicode_cmap(unicode[n]));
        object("<< /Length " + std::to_string(cmap.size()) + " /Filter /FlateDecode >>", &cmap);
    }
    std::string font_resources;
    for (size_t n = 0; n < fonts.size(); n++) font_resources += (n ? " " : "") + std::string("/F") + std::to_string(n) + " " + std::to_string(font_id(n)) + " 0 R";
    for (size_t k = 0; k < pages.size(); k++) {
        const Page& p = pages[k];
        const double w = p.width_pt > 0 ? p.width_pt : p.width * points_per_pixel, h = p.height_pt > 0 ? p.height_pt : p.height * points_per_pixel;
        const std::string im = "/Im" + std::to_string(k);
        std::string fonts_entry = fonts.empty() ? "" : " /Font << " + font_resources + " >>";
        // One graphics state per opacity the page's text uses.
        std::vector<int> alphas;
        for (const Glyph& g : lists[k])
            if (g.alpha < 255 && std::find(alphas.begin(), alphas.end(), int(g.alpha)) == alphas.end()) alphas.push_back(g.alpha);
        std::sort(alphas.begin(), alphas.end());
        if (!alphas.empty()) {
            fonts_entry += " /ExtGState <<";
            for (int a : alphas) fonts_entry += " /GS" + std::to_string(a) + " << /Type /ExtGState /ca " + num(a / 255.0, 3) + " >>";
            fonts_entry += " >>";
        }
        std::snprintf(buf, sizeof buf, "%.2f %.2f", w, h);
        const std::string wh = buf;
        object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + wh + "] /Resources << /XObject << " + im + " " + std::to_string(page_id(k) + 2) + " 0 R >>" +
                   fonts_entry + " >> /Contents " + std::to_string(page_id(k) + 1) + " 0 R >>",
               nullptr);
        std::snprintf(buf, sizeof buf, "%.2f 0 0 %.2f", w, h);
        std::string draw = "q " + std::string(buf) + " 0 0 cm " + im + " Do Q\n";
        if (lists[k].empty()) {
            object("<< /Length " + std::to_string(draw.size()) + " >>", &draw);
        } else {
            draw += text_stream(p, lists[k], w, h, ids, widths);
            const std::string z = zlib_str(draw);
            object("<< /Length " + std::to_string(z.size()) + " /Filter /FlateDecode >>", &z);
        }
        const std::string data = zlib(p.predictor ? up_filtered(p.rgb, p.width) : p.rgb);
        const std::string parms = p.predictor ? " /DecodeParms << /Predictor 15 /Colors 3 /BitsPerComponent 8 /Columns " + std::to_string(p.width) + " >>" : "";
        object("<< /Type /XObject /Subtype /Image /Width " + std::to_string(p.width) + " /Height " + std::to_string(p.height) +
                   " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode" + parms + " /Length " + std::to_string(data.size()) + " >>",
               &data);
    }
    // The cross-reference table: every entry exactly 20 bytes.
    const size_t xref = out.size();
    out += "xref\n0 " + std::to_string(offsets.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t o : offsets) {
        std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", o);
        out += buf;
    }
    out += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

}  // namespace pdf
