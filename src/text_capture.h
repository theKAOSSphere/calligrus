// Text capture for the PDF export (spec 33).
//
// A PDF export has two layers: the page drawn WITHOUT its text (flat colours: a small picture) and
// the text itself as real PDF text. All document text goes through draw_text (inline_layout.cpp);
// while capture is on, it does not draw the glyphs it can describe but reports each one here: its
// source font (the real font file ImGui took the glyph from: the face, then its merged fallbacks),
// glyph id, where its baseline starts, its size and colour. pdf.h then embeds those fonts.
//
// A glyph whose font can't be embedded (no usable file, CFF outlines) is not captured: draw_text
// draws it into the picture as always, so it stays visible, just not selectable.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "pdf.h"
#include "prose_shaping.h"
#include "texmath/texmath.h"

namespace text_capture {

// Capture on: draw_text reports its glyphs here instead of drawing them. (A PDF export, vector mode.)
void enable(bool on);
bool enabled();

// A new frame starts: forget the text of the last one.
void begin_frame();

// A glyph of `source` (a font merged into `font`) at ImGui size `size`, whose top-left pen position
// (where shaped_draw::draw / ImFont::RenderChar would draw it) is `pos`; `text` the characters it
// stands for (UTF-8; empty for a glyph that is only part of a cluster). true: captured, the caller
// must not draw it.
bool glyph(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const prose_shaping::Source& source, uint32_t gid,
           std::string_view text);

// A plain character: finds the font that has it (the first of `font`'s chain) and captures it.
// false: capture is off, or no embeddable font has it: draw it as usual.
bool character(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, uint32_t c);

// The glyphs of a formula that were left out of its bitmap (texmath's capture mode), whose top-left corner is
// `origin`: captured as text in the math font, with their characters. Like `glyph`, those outside the clip
// rectangle are dropped.
void math(ImDrawList* dl, ImVec2 origin, const std::vector<texmath::MathGlyph>& glyphs, ImU32 col);

// Fake italics (code in a face without an italic): glyphs captured from now on are sheared by `skew`
// (the top moves right by that much of the height); 0 = upright.
void set_shear(float skew);

// An opaque rectangle was painted over what was captured so far (the export of a page covers what
// is above and below its text area): drop the glyphs under it.
void cover(ImVec2 min, ImVec2 max);

// Move what the last frame captured into `page` (glyph positions are in the page picture's pixels:
// ImGui's own, the picture being the frame). Glyphs outside the picture are left out.
void fill(pdf::Page& page);

// The fonts the glyphs refer to (the pages' pdf::Glyph::font is an index into this), for pdf::write.
const std::vector<pdf::Font>& fonts();

}  // namespace text_capture
