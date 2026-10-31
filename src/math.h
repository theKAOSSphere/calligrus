// TeX math (the C++ engine in src/texmath/) and Mermaid diagrams (the C++ port in src/mermaid/),
// uploaded as GL textures and cached.
#pragma once

#include <string>
#include <vector>

#include <imgui.h>

#include "texmath/texmath.h"

struct MathTex {
    ImTextureID tex = 0;
    float px_w = 0, px_h = 0;   // bitmap size in pixels
    float ox = 0, oy = 0;       // baseline-left corner inside the bitmap
    float w = 0, asc = 0, desc = 0;  // logical box in pixels
    // PDF export (spec 33): the glyphs left out of the bitmap, to be real text (offsets from the bitmap's top-left).
    std::vector<texmath::MathGlyph> glyphs;
    const MathTex* tag = nullptr;    // display math with 	ag{..}: the label (drawn flush right on the baseline)
};

// Rendered (and cached) formula at `size` pixels per em; nullptr if it failed to parse.
const MathTex* math_get(const std::string& tex, bool display, float size);

// Draw a formula's bitmap with its top-left corner at `p0`, tinted `col`; while a PDF export captures text
// (text_capture.h) its glyphs are reported as text instead (they are not in the bitmap then).
void math_draw(ImDrawList* dl, const MathTex& m, ImVec2 p0, ImU32 col);

// Rendered (and cached) Mermaid diagram at `scale` pixels per diagram unit
// (only px_w/px_h are meaningful); nullptr if it couldn't be drawn.
const MathTex* diagram_get(const std::string& src, float scale);
