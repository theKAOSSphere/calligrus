// Programming ligatures in code text.
//
// Code fonts like Cascadia Code, Fira Code and JetBrains Mono turn "->" into an
// arrow and "!=" into a single sign. They do it with OpenType substitutions that
// keep ONE GLYPH PER CHARACTER and the normal cell width: "->" becomes two glyphs
// (the arrow's start and end, drawn across both cells), "!=" becomes the sign plus
// an empty spacer. So the layout never changes (positions, selection, find all
// keep working per character); only WHICH GLYPH each character draws does.
//
// This module asks HarfBuzz which glyph each character of a string gets. It knows
// nothing about ImGui; shaped_draw.cpp draws the glyphs.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace shaping {

// Make the code font known: `id` is any pointer that identifies it (the ImFont), `bytes`
// the font file (must stay valid until the next call), `face` the face inside a .ttc.
// Clears everything cached for the previous font. id == nullptr: no shaping at all.
void set_font(const void* id, std::string_view bytes, int face = 0);

// The current code font (for the code that rasterises its glyphs), and a number that changes
// with every set_font, so cached glyph pictures of an older font can be dropped.
std::string_view font_bytes();
int font_face();
unsigned generation();

// Which glyph does each character of `text` (UTF-8) draw? One entry per Unicode
// scalar: the glyph id the font substitutes, or 0 where the character keeps its normal
// glyph (draw it like any other text). Returns nullptr when `font_id` is not the code font,
// when nothing is substituted, or when the shaped text is not one-to-one with the characters
// (then the text must be drawn as usual). The pointer stays valid until the next call.
const std::vector<uint32_t>* glyphs(const void* font_id, std::string_view text);

// For tests: the same shaping without the cache and without the "nothing substituted"
// shortcut. `nominal` gets each character's normal glyph id (0 = the font has none).
// Returns false if the result is not one-to-one.
bool shape_for_test(std::string_view text, std::vector<uint32_t>& shaped, std::vector<uint32_t>& nominal);

}  // namespace shaping
