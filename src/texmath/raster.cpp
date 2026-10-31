#include "raster.h"

#include <cmath>

#include "canvas.h"

namespace texmath {

namespace {

// Collects a glyph outline as a path, mapping font units -> pixels on the fly.
class Outline final : public OutlineSink {
public:
    Outline(float ox, float oy, float x, float y, float sx, float sy, float upem, float ppem)
        : ox_(ox), oy_(oy), x_(x), y_(y), sx_(sx), sy_(sy), upem_(upem), ppem_(ppem) {}

    PathBuilder pb;

    void move_to(float x, float y) override {
        const Point p = tf(x, y);
        pb.move_to(p.x, p.y);
    }
    void line_to(float x, float y) override {
        const Point p = tf(x, y);
        pb.line_to(p.x, p.y);
    }
    void curve_to(float x1, float y1, float x2, float y2, float x, float y) override {
        const Point a = tf(x1, y1), b = tf(x2, y2), c = tf(x, y);
        pb.cubic_to(a.x, a.y, b.x, b.y, c.x, c.y);
    }
    void close() override { pb.close(); }

private:
    float ox_, oy_, x_, y_, sx_, sy_, upem_, ppem_;
    // font units (y up) -> pixels (y down)
    Point tf(float fx, float fy) const {
        return Point{ox_ + (x_ + fx / upem_ * sx_) * ppem_, oy_ - (y_ + fy / upem_ * sy_) * ppem_};
    }
};

struct Bounds {
    float x0, y0, x1, y1;
};

// Ink bounds of a box in em: (xmin, ymin, xmax, ymax), y up.
Bounds bounds(const Layouter& lay, const MBox& b) {
    const float upem = lay.upem();
    Bounds r{0.0f, -b.d, b.w, b.h};
    for (const Item& it : b.items) {
        if (auto* g = std::get_if<GlyphItem>(&it)) {
            if (auto bb = lay.face().glyph_bounding_box(g->gid)) {
                r.x0 = std::fmin(r.x0, g->x + static_cast<float>(bb->x_min) / upem * g->sx);
                r.x1 = std::fmax(r.x1, g->x + static_cast<float>(bb->x_max) / upem * g->sx);
                r.y0 = std::fmin(r.y0, g->y + static_cast<float>(bb->y_min) / upem * g->sy);
                r.y1 = std::fmax(r.y1, g->y + static_cast<float>(bb->y_max) / upem * g->sy);
            }
        } else if (auto* rule = std::get_if<RuleItem>(&it)) {
            r.x0 = std::fmin(r.x0, rule->x);
            r.x1 = std::fmax(r.x1, rule->x + rule->w);
            r.y0 = std::fmin(r.y0, rule->y);
            r.y1 = std::fmax(r.y1, rule->y + rule->h);
        } else {
            const auto& l = std::get<LineItem>(it);
            r.x0 = std::fmin(r.x0, std::fmin(l.x1, l.x2) - l.t);
            r.x1 = std::fmax(r.x1, std::fmax(l.x1, l.x2) + l.t);
            r.y0 = std::fmin(r.y0, std::fmin(l.y1, l.y2) - l.t);
            r.y1 = std::fmax(r.y1, std::fmax(l.y1, l.y2) + l.t);
        }
    }
    return r;
}

// Rust's `f32 as i32` for the rounded rule edges (saturating).
int to_int(float v) {
    if (std::isnan(v)) return 0;
    if (v <= -2147483648.0f) return INT32_MIN;
    if (v >= 2147483647.0f) return INT32_MAX;
    return static_cast<int>(v);
}

}  // namespace

std::optional<Raster> rasterize(const Layouter& lay, const MBox& b, float ppem, const std::vector<bool>& skip) {
    const Bounds bb = bounds(lay, b);
    const float pad = 2.0f;
    const float fw = std::ceil((bb.x1 - bb.x0) * ppem) + 2.0f * pad;
    const float fh = std::ceil((bb.y1 - bb.y0) * ppem) + 2.0f * pad;
    // Refuse absurd sizes (NaN, infinity, or something like `\hspace{1000000em}`).
    if (!(std::isfinite(fw) && std::isfinite(fh)) || fw > 16384.0f || fh > 16384.0f || fw * fh > 40000000.0f) {
        return std::nullopt;
    }
    const auto width = static_cast<uint32_t>(std::fmax(fw, 1.0f));
    const auto height = static_cast<uint32_t>(std::fmax(fh, 1.0f));
    Pixmap pm(width, height);

    // em coordinates (y up) -> pixel coordinates (y down)
    const float ox = pad - bb.x0 * ppem;
    const float oy = pad + bb.y1 * ppem;
    const float upem = lay.upem();

    for (size_t n = 0; n < b.items.size(); n++) {
        if (n < skip.size() && skip[n]) continue;
        const Item& it = b.items[n];
        if (auto* g = std::get_if<GlyphItem>(&it)) {
            Outline ol(ox, oy, g->x, g->y, g->sx, g->sy, upem, ppem);
            if (lay.face().outline_glyph(g->gid, ol)) {
                if (auto path = ol.pb.finish()) pm.fill_path(*path);
            }
        } else if (auto* l = std::get_if<LineItem>(&it)) {
            pm.stroke_line(Point{ox + l->x1 * ppem, oy - l->y1 * ppem}, Point{ox + l->x2 * ppem, oy - l->y2 * ppem},
                           std::fmax(l->t * ppem, 1.0f));
        } else {
            const auto& r = std::get<RuleItem>(it);
            // Snap rules to whole pixels (at least one pixel thick) so bars stay crisp.
            const float left = std::round(ox + r.x * ppem);
            const float right = std::fmax(std::round(ox + (r.x + r.w) * ppem), left + 1.0f);
            const float bottom = std::round(oy - r.y * ppem);
            float top = std::round(oy - (r.y + r.h) * ppem);
            if (bottom - top < 1.0f) top = bottom - 1.0f;
            if (std::isfinite(left) && std::isfinite(right) && std::isfinite(top) && std::isfinite(bottom)) {
                pm.fill_int_rect(to_int(left), to_int(top), to_int(right), to_int(bottom));
            }
        }
    }

    Raster out;
    out.width = width;
    out.height = height;
    out.rgba = std::move(pm.data());
    out.origin_x = ox;
    out.origin_y = oy;
    return out;
}

}  // namespace texmath
