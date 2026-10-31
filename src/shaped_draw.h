// Draws one glyph of a font BY GLYPH ID: the ligature glyphs of the code font (shaping.h) and the
// glyphs of complex-script text shaped from any source font of a chain (prose_shaping.h).
//
// ImGui's font atlas is keyed by character, so it cannot hold "the second half of an arrow".
// We rasterise such glyphs ourselves with the same FreeType settings ImGui uses for its fonts,
// keep them in our own small OpenGL textures, and draw them as textured quads at the spot ImGui
// would have put the character's own glyph.
#pragma once

#include <cstdint>

#include <imgui.h>

#include "prose_shaping.h"

namespace shaped_draw {

// Draw glyph `glyph` of `source` (one of the fonts merged into `font`) at ImGui size `size`, where
// ImFont::RenderChar would draw a character. false: could not (the caller draws the character normally).
bool draw(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const prose_shaping::Source& source,
          uint32_t glyph);

// Draw glyph `glyph` of `font` (the code font) at ImGui size `size`, where ImFont::RenderChar(dl,
// size, pos, col, ...) would draw a character's glyph. false: could not (the caller draws the
// character normally instead).
bool draw(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, uint32_t glyph);

}  // namespace shaped_draw
