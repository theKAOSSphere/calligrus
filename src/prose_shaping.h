// Shaping complex scripts in prose (spec 32).
//
// Devanagari, Bengali, Tamil, Thai... are not "one glyph per character": consonants fuse into
// conjuncts (क + ् + ष = क्ष), vowel signs move around their consonant, marks sit above and
// below. ImGui draws one glyph per character, so such text showed the right letters, but
// uncombined. This module asks HarfBuzz what glyphs a piece of text really makes (and where),
// so inline_layout.cpp can lay it out and draw it (by glyph id, via shaped_draw.h).
//
// Only left-to-right complex scripts are handled (the table in script_of). Text without any
// such character is not touched at all: plan() says "no plan" and the caller keeps its old,
// per-character path. This module knows nothing about ImGui: fonts are `const void*` ids.
//
// Overview of plan(font, text):
//   1. cut the text into runs of one script (ZWJ and combining marks join their letters; spaces
//      and punctuation stay out, on the old path);
//   2. for a run of a complex script, pick the first font of the chain (face, Ubuntu, emoji,
//      Cascadia, Hack, system fonts) that has every character of the run;
//   3. shape the run with HarfBuzz: glyph ids, advances and offsets, in fractions of the em;
//   4. results are cached.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace prose_shaping {

// ---- Scripts and runs (pure functions, no fonts) -------------------------------------------

enum class Script : uint8_t {
    Simple,  // anything that needs no shaping (Latin, Greek, CJK...): the old per-character path
    Neutral, // "Common" / "Inherited": space, punctuation, digits, combining marks, ZWJ/ZWNJ
    Devanagari, Bengali, Gurmukhi, Gujarati, Oriya, Tamil, Telugu, Kannada, Malayalam, Sinhala,
    Thai, Lao, Tibetan, Myanmar, Khmer,
};

Script script_of(uint32_t cp);
inline bool is_complex(Script s) { return s >= Script::Devanagari; }

// UTF-8 -> code points (a bad byte is U+FFFD, one byte, like ImGui's decoder).
std::vector<uint32_t> decode(std::string_view utf8);

// A stretch of characters [begin, end) of one script. Script::Simple covers everything that is
// not complex (including neutral characters next to it).
struct Run {
    size_t begin = 0, end = 0;
    Script script = Script::Simple;
    bool operator==(const Run&) const = default;
};

// Cut `cps` into runs. Neutral characters that belong to letters (ZWJ, ZWNJ, combining marks,
// variation selectors) join the run before them (a first one: the run after it). Other neutral
// characters (space, punctuation, digits) are Simple: "हिन्दी, ok" is [हिन्दी] [, ok], so they are
// drawn like the rest of the prose, as in egui. Neighbouring runs of one script are merged.
// Empty input: no runs.
std::vector<Run> split_runs(const std::vector<uint32_t>& cps);

// The index of the first of `count` fonts that covers every character of `cps` (as given by
// has(font, cp)), or -1. (Pure: the caller decides what "covers" means.)
template <class Has>
int choose_source(size_t count, const uint32_t* cps, size_t n, const Has& has) {
    for (size_t f = 0; f < count; f++) {
        bool all = true;
        for (size_t i = 0; all && i < n; i++) all = has(f, cps[i]);
        if (all) return int(f);
    }
    return -1;
}

// ---- Shaped glyphs and clusters --------------------------------------------------------------

// One glyph HarfBuzz made. Distances are fractions of the em (multiply by pixels per em).
struct Glyph {
    uint32_t id = 0;       // glyph id in the source font (0 = .notdef: the font can't draw it)
    uint32_t cluster = 0;  // index of the first character of its cluster, within the run
    float x_adv = 0;
    float x_off = 0, y_off = 0;  // y_off: up is positive
};

// The characters that make one visible unit (a conjunct: क्ष is three characters) and the glyphs
// that draw them. Characters [first, last) of the run, glyphs [glyph_first, glyph_last).
struct Cluster {
    size_t first = 0, last = 0;
    size_t glyph_first = 0, glyph_last = 0;
    bool operator==(const Cluster&) const = default;
};

// Group shaped glyphs (left to right, cluster values rising) into clusters of a run with
// `nchars` characters. A cluster reaches up to the next cluster's first character, or the run's
// end. Selection treats a cluster as atomic: every character boundary inside it is its left edge.
std::vector<Cluster> clusters(const std::vector<Glyph>& glyphs, size_t nchars);

// ---- Fonts and plans -----------------------------------------------------------------------

// One source font of an ImFont's chain (what ImGui merged into it), in the order ImGui tries them.
struct Source {
    std::string_view bytes;  // the font file (must stay valid until the next set_chains)
    int face = 0;            // face inside a .ttc
    float size_ratio = 1;    // ImFontConfig::ExtraSizeScale it was merged with (1 for the main face)
    float em = 1;            // its (ascender - descender) / units per em
};
struct Chain {
    const void* font = nullptr;  // the ImFont
    std::vector<Source> sources;
};

// Make the fonts known (after every load_fonts). Clears the cache and bumps generation().
void set_chains(std::vector<Chain> chains);
unsigned generation();

// Pixels per em of source `s` when the layout's "exact size" (ImGui size x advance_scale) is
// `exact_size`: ImGui scales every merged font so that ascent+descent = size x size_ratio.
inline float ppem(const Source& s, float exact_size) { return s.em > 0 ? exact_size * s.size_ratio / s.em : 0; }

// A stretch of the text, laid out one way: a complex run in one source font, or a plain run
// (shaped == false) that keeps the old per-character path.
struct Part {
    size_t begin = 0, end = 0;  // characters [begin, end) of the text
    bool shaped = false;
    Source source;              // shaped: the font the run was shaped with
    std::vector<Glyph> glyphs;  // shaped: cluster values count from `begin`
    std::vector<Cluster> clusters;
};
struct Plan {
    std::vector<uint32_t> chars;  // the text's characters
    std::vector<Part> parts;      // cover all of them, in order
};

// The first source of `font`'s chain that has a glyph for character `cp` (what ImGui draws it from),
// and that glyph's id; nullptr if none has it or the font is unknown. (Text capture for the PDF, spec 33.)
const Source* find_source(const void* font, uint32_t cp, uint32_t& glyph);

// The main face of `font`'s chain (the first source), nullptr if the font is unknown.
const Source* main_source(const void* font);

// A short label laid out the way egui does it: shaped with the main face's kerning (egui measures and
// places text with HarfBuzz's positions, so "tabs-b.md" is 0.4 px narrower than the sum of its advances).
// `pieces` are consecutive stretches of the text with their x from the left edge (a ligature's characters
// are one piece), `width` the total, both in pixels at the exact size `exact_size` (ImGui size x advance
// scale). false: no kerned layout (unknown font, or a character the main face lacks): use the plain advances.
struct Piece {
    std::string text;  // UTF-8
    float x = 0;
};
bool kerned_pieces(const void* font, std::string_view utf8, float exact_size, std::vector<Piece>& pieces, float& width);

// How to lay out `utf8` in `font`: nullptr if it has no complex-script character, or the font
// is unknown (use the old path for all of it). A run no font of the chain has all characters
// for comes back as a plain part. Cached; the pointer stays valid while the caller holds it.
std::shared_ptr<const Plan> plan(const void* font, std::string_view utf8);

}  // namespace prose_shaping
