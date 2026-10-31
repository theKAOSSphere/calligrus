// A tiny anti-aliased 2D rasterizer: just what the TeX engine draws (glyph
// outlines, rules, strike-through lines) in white ink.
//
// The Rust engine draws with the `tiny-skia` crate (a port of Skia's CPU
// rasterizer). This is a port of the exact code paths it takes there:
// Skia's 4x-supersampling scan converter for filled paths, its anti-aliased
// hairlines (1px lines), and its butt-capped stroker for single segments,
// with the same fixed-point math and blending, so the pixels match.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace texmath {

struct Point {
    float x, y;
};

// A float rectangle (left/top/right/bottom), y down.
struct RectF {
    float left, top, right, bottom;
};

enum class Verb : uint8_t { Move, Line, Cubic, Close };

struct Path {
    std::vector<Verb> verbs;
    std::vector<Point> points;
    RectF bounds;
};

// Builds a Path the way tiny-skia's PathBuilder does (a `line_to` after a
// `close` re-opens at the last move point, a repeated `move_to` replaces the
// previous one, ...).
class PathBuilder {
public:
    void move_to(float x, float y);
    void line_to(float x, float y);
    void cubic_to(float x1, float y1, float x2, float y2, float x, float y);
    void close();
    // nullopt for empty paths, a lone move, or non-finite points.
    std::optional<Path> finish();

    std::vector<Verb> verbs;
    std::vector<Point> points;

private:
    void inject_move_to_if_needed();
    size_t last_move_to_index_ = 0;
    bool move_to_required_ = true;
};

// A premultiplied RGBA8 image, initially transparent, painted in opaque white.
class Pixmap {
public:
    Pixmap(uint32_t width, uint32_t height);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    std::vector<uint8_t>& data() { return rgba_; }

    // Fill a closed path with the non-zero winding rule, anti-aliased.
    void fill_path(const Path& path);
    // Fill a rectangle whose edges are on whole pixels.
    void fill_int_rect(int left, int top, int right, int bottom);
    // Stroke a straight line `width` pixels wide with butt caps. Lines of
    // width <= 1 are drawn as anti-aliased hairlines, like tiny-skia does.
    void stroke_line(Point a, Point b, float width);

private:
    uint32_t width_, height_;
    std::vector<uint8_t> rgba_;
};

}  // namespace texmath
