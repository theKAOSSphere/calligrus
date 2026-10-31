#include "shaping.h"

#include <cstdio>
#include <string>
#include <unordered_map>

#include <hb.h>

namespace shaping {

namespace {

const void* g_id = nullptr;
hb_font_t* g_font = nullptr;
std::string_view g_bytes;
int g_face = 0;
unsigned g_generation = 0;
bool g_warned = false;  // said once that this font's shaping is not one-to-one

// Shaped text -> glyphs; code is redrawn every frame. Emptied when it gets big.
constexpr size_t kMaxCached = 4096;
std::unordered_map<std::string, std::vector<uint32_t>> g_cache;  // empty vector = no substitution

// UTF-8 -> code points (a bad byte is U+FFFD, one byte, like ImGui's decoder).
std::vector<uint32_t> decode(std::string_view s) {
    std::vector<uint32_t> out;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        uint32_t cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
        bool ok = n > 0 && i + n <= s.size();
        for (int k = 1; ok && k < n; k++) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) ok = false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) cp = 0xFFFD, n = 1;
        out.push_back(cp);
        i += n;
    }
    return out;
}

void release() {
    if (g_font) {
        hb_face_t* face = hb_font_get_face(g_font);
        hb_font_destroy(g_font);
        hb_face_destroy(face);
    }
    g_font = nullptr;
    g_cache.clear();
}

// Shape `text`. true: one glyph per character (shaped / nominal filled in).
bool shape(std::string_view text, std::vector<uint32_t>& shaped, std::vector<uint32_t>& nominal) {
    const std::vector<uint32_t> chars = decode(text);
    if (!g_font || chars.empty()) return false;

    hb_buffer_t* buf = hb_buffer_create();
    // Add as UTF-32, so a "cluster" is a character index (not a byte offset).
    hb_buffer_add_utf32(buf, chars.data(), int(chars.size()), 0, int(chars.size()));
    hb_buffer_set_cluster_level(buf, HB_BUFFER_CLUSTER_LEVEL_CHARACTERS);
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);  // code is left to right
    hb_buffer_guess_segment_properties(buf);         // script and language
    hb_shape(g_font, buf, nullptr, 0);               // default features: calt, liga, kern...

    unsigned n = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
    bool ok = n == chars.size();
    shaped.assign(chars.size(), 0);
    nominal.assign(chars.size(), 0);
    for (size_t i = 0; ok && i < chars.size(); i++) {
        ok = info[i].cluster == i && pos[i].x_offset == 0 && pos[i].y_offset == 0;
        hb_codepoint_t nom = 0;
        hb_font_get_nominal_glyph(g_font, chars[i], &nom);
        shaped[i] = info[i].codepoint;
        nominal[i] = nom;
        // Same width as the character's own glyph, or the layout (which uses those) would drift.
        if (ok && nom != 0) ok = pos[i].x_advance == hb_font_get_glyph_h_advance(g_font, nom);
    }
    hb_buffer_destroy(buf);
    return ok;
}

}  // namespace

void set_font(const void* id, std::string_view bytes, int face) {
    release();
    g_generation++;
    g_bytes = bytes;
    g_face = face;
    g_id = id;
    g_warned = false;
    if (!id || bytes.empty()) return;
    // The bytes belong to the caller: HB_MEMORY_MODE_READONLY reads them in place.
    hb_blob_t* blob = hb_blob_create(bytes.data(), unsigned(bytes.size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    hb_face_t* hbface = hb_face_create(blob, unsigned(face));
    hb_blob_destroy(blob);  // the face keeps it alive
    g_font = hb_font_create(hbface);
    const int upem = int(hb_face_get_upem(hbface));
    hb_font_set_scale(g_font, upem, upem);  // advances in font units
}

std::string_view font_bytes() { return g_bytes; }
int font_face() { return g_face; }
unsigned generation() { return g_generation; }

const std::vector<uint32_t>* glyphs(const void* font_id, std::string_view text) {
    if (!g_font || font_id != g_id || text.empty()) return nullptr;
    // A lone character never ligates (all of these fonts need a neighbour).
    if (text.size() == 1) return nullptr;
    const std::string key(text);
    auto it = g_cache.find(key);
    if (it == g_cache.end()) {
        if (g_cache.size() >= kMaxCached) g_cache.clear();
        std::vector<uint32_t> shaped, nominal, result;
        if (shape(text, shaped, nominal)) {
            // Keep only the characters whose glyph really changed.
            bool any = false;
            result.assign(shaped.size(), 0);
            for (size_t i = 0; i < shaped.size(); i++)
                if (nominal[i] != 0 && shaped[i] != nominal[i]) result[i] = shaped[i], any = true;
            if (!any) result.clear();
        } else if (!g_warned && text.size() > 1) {
            // (Plain text is simply drawn as usual; say so once, not on every redraw.)
            g_warned = true;
            std::fprintf(stderr, "calligrus: code font shaping is not one-to-one for \"%.20s\"; ligatures are off for such text\n", key.c_str());
        }
        it = g_cache.emplace(key, std::move(result)).first;
    }
    return it->second.empty() ? nullptr : &it->second;
}

bool shape_for_test(std::string_view text, std::vector<uint32_t>& shaped, std::vector<uint32_t>& nominal) {
    return shape(text, shaped, nominal);
}

}  // namespace shaping
