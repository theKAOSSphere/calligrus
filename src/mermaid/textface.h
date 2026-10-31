// Mermaid port: a font face for DRAWING diagram text (shaping + glyph outlines through
// HarfBuzz). The Rust path does this inside resvg (rustybuzz = a port of HarfBuzz for shaping,
// ttf-parser for outlines), so HarfBuzz gives the same glyph choices, kerning and ligatures.
// (Measuring text for the layout is a different job: see fontmetrics.h.)
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

struct hb_blob_t;
struct hb_face_t;
struct hb_font_t;

namespace mermaid {

// Receives a glyph outline in font units, y up (like ttf-parser's OutlineBuilder).
class OutlineSink {
public:
    virtual ~OutlineSink() = default;
    virtual void move_to(float x, float y) = 0;
    virtual void line_to(float x, float y) = 0;
    virtual void quad_to(float cx, float cy, float x, float y) = 0;
    virtual void cubic_to(float c1x, float c1y, float c2x, float c2y, float x, float y) = 0;
    virtual void close() = 0;
};

struct ShapedGlyph {
    uint32_t gid = 0;
    float x_advance = 0, x_offset = 0, y_offset = 0;  // font units
};

class TextFace {
public:
    TextFace() = default;
    ~TextFace();
    TextFace(const TextFace&) = delete;
    TextFace& operator=(const TextFace&) = delete;

    bool load(std::vector<uint8_t> data);  // false if HarfBuzz cannot read the font
    bool loaded() const { return font_ != nullptr; }
    float units_per_em() const { return upem_; }
    // Vertical metrics in font units (hhea / OS/2, as ttf-parser reads them): ascent > 0, descent < 0.
    float ascent() const;
    float descent() const;
    float x_height() const;  // OS/2 sxHeight; 45% of the line height when the font has none (usvg)

    // Left-to-right shaping with HarfBuzz' default features (kern, liga, ...).
    std::vector<ShapedGlyph> shape(std::string_view utf8) const;
    void outline(uint32_t gid, OutlineSink& sink) const;

private:
    std::vector<uint8_t> data_;
    hb_blob_t* blob_ = nullptr;
    hb_face_t* face_ = nullptr;
    hb_font_t* font_ = nullptr;
    float upem_ = 1000.0f;
};

}  // namespace mermaid
