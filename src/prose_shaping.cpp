#include "prose_shaping.h"

#include <algorithm>
#include <string>
#include <unordered_map>

#include <hb.h>

namespace prose_shaping {

namespace {

// The complex scripts handled, as ranges of code points (whole Unicode blocks, plus the extension
// blocks of the same script). Sorted by `first` is not needed: the table is short, searched in turn.
struct Range {
    uint32_t first, last;
    Script script;
    hb_script_t hb;
};
const Range kRanges[] = {
    {0x0900, 0x097F, Script::Devanagari, HB_SCRIPT_DEVANAGARI},
    {0xA8E0, 0xA8FF, Script::Devanagari, HB_SCRIPT_DEVANAGARI},  // extended
    {0x0980, 0x09FF, Script::Bengali, HB_SCRIPT_BENGALI},
    {0x0A00, 0x0A7F, Script::Gurmukhi, HB_SCRIPT_GURMUKHI},
    {0x0A80, 0x0AFF, Script::Gujarati, HB_SCRIPT_GUJARATI},
    {0x0B00, 0x0B7F, Script::Oriya, HB_SCRIPT_ORIYA},
    {0x0B80, 0x0BFF, Script::Tamil, HB_SCRIPT_TAMIL},
    {0x0C00, 0x0C7F, Script::Telugu, HB_SCRIPT_TELUGU},
    {0x0C80, 0x0CFF, Script::Kannada, HB_SCRIPT_KANNADA},
    {0x0D00, 0x0D7F, Script::Malayalam, HB_SCRIPT_MALAYALAM},
    {0x0D80, 0x0DFF, Script::Sinhala, HB_SCRIPT_SINHALA},
    {0x0E00, 0x0E7F, Script::Thai, HB_SCRIPT_THAI},
    {0x0E80, 0x0EFF, Script::Lao, HB_SCRIPT_LAO},
    {0x0F00, 0x0FFF, Script::Tibetan, HB_SCRIPT_TIBETAN},
    {0x1000, 0x109F, Script::Myanmar, HB_SCRIPT_MYANMAR},
    {0xA9E0, 0xA9FF, Script::Myanmar, HB_SCRIPT_MYANMAR},  // extended B
    {0xAA60, 0xAA7F, Script::Myanmar, HB_SCRIPT_MYANMAR},  // extended A
    {0x1780, 0x17FF, Script::Khmer, HB_SCRIPT_KHMER},
    {0x19E0, 0x19FF, Script::Khmer, HB_SCRIPT_KHMER},  // symbols
};

hb_script_t hb_script(Script s) {
    for (const Range& r : kRanges)
        if (r.script == s) return r.hb;
    return HB_SCRIPT_COMMON;
}

// Characters that are not "of a script" (Unicode: Common / Inherited): ASCII punctuation, digits
// and space, the Latin-1 punctuation, general punctuation (which has ZWJ / ZWNJ), currency
// signs, combining marks, variation selectors; the danda (a Common sign inside the Devanagari
// block) and the baht sign. Deliberately a short list: anything else (Latin letters, CJK...) is "Simple".
bool is_neutral(uint32_t c) {
    if (c < 0x80) return !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
    return (c >= 0xA0 && c <= 0xBF && c != 0xAA && c != 0xB5 && c != 0xBA) || (c >= 0x0300 && c <= 0x036F) ||
           c == 0x0964 || c == 0x0965 || c == 0x0E3F || (c >= 0x2000 && c <= 0x206F) ||
           (c >= 0x20A0 && c <= 0x20CF) || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F);
}

// The neutral characters that belong to the letters around them, not to the text between words:
// ZWJ / ZWNJ and the other format characters, combining marks, variation selectors. A shaped run
// keeps them; spaces, punctuation and digits do not join it (see split_runs).
bool is_joiner(uint32_t c) {
    return (c >= 0x200B && c <= 0x200F) || (c >= 0x2060 && c <= 0x2064) || (c >= 0x0300 && c <= 0x036F) ||
           (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F);
}

// Characters a font need not have a glyph for: they only steer their neighbours (ZWJ, ZWNJ, variation
// selectors, soft hyphen...). ImGui draws nothing for them either (fonts.cpp is_invisible).
bool is_ignorable(uint32_t c) {
    return (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0x200B && c <= 0x200F) || (c >= 0x2060 && c <= 0x2064) ||
           c == 0x00AD || c == 0xFEFF;
}

// ---- Fonts ----------------------------------------------------------------------------------

// A source plus its HarfBuzz font, made on first use.
struct Loaded {
    Source source;
    hb_font_t* font = nullptr;
    bool tried = false;
};
struct LoadedChain {
    std::vector<Loaded> sources;
};

std::unordered_map<const void*, LoadedChain> g_chains;
unsigned g_generation = 0;

// Shaped text -> plan. Plans are drawn every frame; emptied when it gets big.
constexpr size_t kMaxCached = 4096;
std::unordered_map<std::string, std::shared_ptr<const Plan>> g_cache;

void release() {
    for (auto& [id, chain] : g_chains)
        for (Loaded& l : chain.sources)
            if (l.font) {
                hb_face_t* face = hb_font_get_face(l.font);
                hb_font_destroy(l.font);
                hb_face_destroy(face);
            }
    g_chains.clear();
    g_cache.clear();
}

hb_font_t* open(Loaded& l) {
    if (!l.tried) {
        l.tried = true;
        if (!l.source.bytes.empty()) {
            // The bytes belong to the caller: HB_MEMORY_MODE_READONLY reads them in place.
            hb_blob_t* blob = hb_blob_create(l.source.bytes.data(), unsigned(l.source.bytes.size()),
                                             HB_MEMORY_MODE_READONLY, nullptr, nullptr);
            hb_face_t* face = hb_face_create(blob, unsigned(l.source.face));
            hb_blob_destroy(blob);  // the face keeps it alive
            l.font = hb_font_create(face);
            const int upem = int(hb_face_get_upem(face));
            hb_font_set_scale(l.font, upem, upem);  // positions in font units
        }
    }
    return l.font;
}

// Shape characters [begin, end) of `cps` with `font` as one run of `script`.
std::vector<Glyph> shape(hb_font_t* font, const std::vector<uint32_t>& cps, size_t begin, size_t end, Script script) {
    hb_buffer_t* buf = hb_buffer_create();
    // UTF-32, so a "cluster" is a character index (not a byte offset).
    hb_buffer_add_utf32(buf, cps.data() + begin, int(end - begin), 0, int(end - begin));
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, hb_script(script));
    // (No language: the default must not depend on the machine's locale.)
    hb_shape(font, buf, nullptr, 0);  // default features: the script's own (akhn, half, pres, mark...)

    unsigned n = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
    const float upem = float(hb_face_get_upem(hb_font_get_face(font)));
    std::vector<Glyph> out(n);
    for (unsigned i = 0; i < n; i++)
        out[i] = Glyph{info[i].codepoint, info[i].cluster, float(pos[i].x_advance) / upem, float(pos[i].x_offset) / upem,
                       float(pos[i].y_offset) / upem};
    hb_buffer_destroy(buf);
    return out;
}

}  // namespace

// ---- Public: scripts and runs ---------------------------------------------------------------

Script script_of(uint32_t cp) {
    // A danda etc. is neutral even inside a block: ask for that first.
    if (is_neutral(cp)) return Script::Neutral;
    for (const Range& r : kRanges)
        if (cp >= r.first && cp <= r.last) return r.script;
    return Script::Simple;
}

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

std::vector<Run> split_runs(const std::vector<uint32_t>& cps) {
    // Each character's script. A neutral joiner (ZWJ, a combining mark...) takes the one before it;
    // other neutral characters (space, punctuation, digits) stay out of complex runs: the old path
    // draws them from the font chain's first font that has them (the prose face), as egui does,
    // instead of the script's font (whose comma or digits are other widths).
    std::vector<Script> of(cps.size());
    Script prev = Script::Neutral;  // "none yet"
    for (size_t i = 0; i < cps.size(); i++) {
        Script s = script_of(cps[i]);
        if (s == Script::Neutral) s = is_joiner(cps[i]) ? prev : Script::Simple;
        of[i] = s;
        prev = s;
    }
    // A joiner at the start has nothing before it: it joins what follows.
    Script next = Script::Simple;
    for (size_t i = cps.size(); i-- > 0;) {
        if (of[i] == Script::Neutral) of[i] = next;
        else next = of[i];
    }
    std::vector<Run> runs;
    for (size_t i = 0; i < cps.size(); i++) {
        if (!runs.empty() && runs.back().script == of[i]) runs.back().end = i + 1;
        else runs.push_back(Run{i, i + 1, of[i]});
    }
    return runs;
}

std::vector<Cluster> clusters(const std::vector<Glyph>& glyphs, size_t nchars) {
    std::vector<Cluster> out;
    for (size_t g = 0; g < glyphs.size(); g++) {
        if (!out.empty() && out.back().first == glyphs[g].cluster) {
            out.back().glyph_last = g + 1;  // another glyph of the same cluster
        } else {
            out.push_back(Cluster{glyphs[g].cluster, nchars, g, g + 1});
            if (out.size() > 1) out[out.size() - 2].last = glyphs[g].cluster;  // the previous one ends here
        }
    }
    return out;
}

// ---- Public: fonts and plans ----------------------------------------------------------------

void set_chains(std::vector<Chain> chains) {
    release();
    g_generation++;
    for (Chain& c : chains) {
        LoadedChain& lc = g_chains[c.font];
        for (Source& s : c.sources) lc.sources.push_back(Loaded{s});
    }
}

unsigned generation() { return g_generation; }

const Source* find_source(const void* font, uint32_t cp, uint32_t& glyph) {
    auto chain_it = g_chains.find(font);
    if (chain_it == g_chains.end()) return nullptr;
    for (Loaded& l : chain_it->second.sources) {
        hb_font_t* hb = open(l);
        hb_codepoint_t g = 0;
        if (hb && hb_font_get_nominal_glyph(hb, cp, &g) && g != 0) {
            glyph = g;
            return &l.source;
        }
    }
    return nullptr;
}

const Source* main_source(const void* font) {
    auto chain_it = g_chains.find(font);
    return chain_it == g_chains.end() || chain_it->second.sources.empty() ? nullptr : &chain_it->second.sources[0].source;
}

bool kerned_pieces(const void* font, std::string_view utf8, float exact_size, std::vector<Piece>& pieces, float& width) {
    auto chain_it = g_chains.find(font);
    if (chain_it == g_chains.end() || chain_it->second.sources.empty()) return false;
    Loaded& main = chain_it->second.sources[0];
    hb_font_t* hb = open(main);
    const std::vector<uint32_t> cps = decode(utf8);
    if (!hb || cps.empty()) return false;
    for (uint32_t c : cps) {
        hb_codepoint_t g = 0;
        if (!hb_font_get_nominal_glyph(hb, c, &g) || g == 0) return false;
    }
    const std::vector<Glyph> glyphs = shape(hb, cps, 0, cps.size(), Script::Simple);
    const float em_px = ppem(main.source, exact_size);
    auto encode = [](std::string& s, uint32_t c) {
        if (c < 0x80) s += char(c);
        else if (c < 0x800) s += char(0xC0 | (c >> 6)), s += char(0x80 | (c & 0x3F));
        else if (c < 0x10000) s += char(0xE0 | (c >> 12)), s += char(0x80 | ((c >> 6) & 0x3F)), s += char(0x80 | (c & 0x3F));
        else s += char(0xF0 | (c >> 18)), s += char(0x80 | ((c >> 12) & 0x3F)), s += char(0x80 | ((c >> 6) & 0x3F)), s += char(0x80 | (c & 0x3F));
    };
    pieces.clear();
    float x = 0;
    for (size_t i = 0; i < glyphs.size(); i++) {
        // The glyph's cluster: its characters up to the next glyph's cluster (a ligature has several).
        if (i > 0 && glyphs[i].cluster == glyphs[i - 1].cluster) {
            x += glyphs[i].x_adv * em_px;
            continue;
        }
        size_t next = cps.size();
        for (size_t k = i + 1; k < glyphs.size(); k++)
            if (glyphs[k].cluster != glyphs[i].cluster) {
                next = glyphs[k].cluster;
                break;
            }
        Piece p;
        p.x = x + glyphs[i].x_off * em_px;
        for (size_t c = glyphs[i].cluster; c < next && c < cps.size(); c++) encode(p.text, cps[c]);
        pieces.push_back(std::move(p));
        x += glyphs[i].x_adv * em_px;
    }
    width = x;
    return true;
}

std::shared_ptr<const Plan> plan(const void* font, std::string_view utf8) {
    // Fast exit: every complex script starts at U+0900, whose UTF-8 lead byte is 0xE0 or more.
    if (std::none_of(utf8.begin(), utf8.end(), [](char c) { return static_cast<unsigned char>(c) >= 0xE0; }))
        return nullptr;
    auto chain_it = g_chains.find(font);
    if (chain_it == g_chains.end()) return nullptr;

    std::string key(reinterpret_cast<const char*>(&font), sizeof font);
    key.append(utf8);
    if (auto hit = g_cache.find(key); hit != g_cache.end()) return hit->second;

    auto result = std::make_shared<Plan>();
    result->chars = decode(utf8);
    const std::vector<uint32_t>& cps = result->chars;
    bool any = false;
    for (const Run& run : split_runs(cps)) {
        Part part;
        part.begin = run.begin;
        part.end = run.end;
        if (is_complex(run.script)) {
            // The characters that need a glyph (a ZWJ need not have one).
            std::vector<uint32_t> need;
            for (size_t i = run.begin; i < run.end; i++)
                if (!is_ignorable(cps[i])) need.push_back(cps[i]);
            std::vector<Loaded>& sources = chain_it->second.sources;
            const int pick = choose_source(sources.size(), need.data(), need.size(), [&](size_t f, uint32_t cp) {
                hb_font_t* hb = open(sources[f]);
                hb_codepoint_t glyph = 0;
                return hb && hb_font_get_nominal_glyph(hb, cp, &glyph);
            });
            if (pick >= 0) {
                part.shaped = true;
                part.source = sources[size_t(pick)].source;
                part.glyphs = shape(sources[size_t(pick)].font, cps, run.begin, run.end, run.script);
                part.clusters = clusters(part.glyphs, run.end - run.begin);
                any = true;
            }
        }
        result->parts.push_back(std::move(part));
    }
    // Nothing shaped (no font has them): the old path is the whole answer.
    std::shared_ptr<const Plan> out = any ? std::move(result) : nullptr;
    if (g_cache.size() >= kMaxCached) g_cache.clear();
    g_cache.emplace(std::move(key), out);
    return out;
}

}  // namespace prose_shaping
