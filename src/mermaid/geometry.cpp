// Mermaid port of layout/geometry.rs (see geometry.h).
#include "geometry.h"

#include <algorithm>
#include <limits>

namespace mermaid {

namespace {
constexpr float POINT_EPS = 0.5f;
constexpr float GEOM_EPS = 1e-6f;

float fmaxf_(float a, float b) { return a > b ? a : (a < b ? b : a); }
float fminf_(float a, float b) { return a < b ? a : (a > b ? b : a); }

float orient(Point a, Point b, Point c) {
    return (b.first - a.first) * (c.second - a.second) - (b.second - a.second) * (c.first - a.first);
}

// Whether p lies within the bounding box of segment a-b (callers check collinearity first).
bool on_segment(Point a, Point b, Point p) {
    return p.first >= fminf_(a.first, b.first) - GEOM_EPS && p.first <= fmaxf_(a.first, b.first) + GEOM_EPS &&
           p.second >= fminf_(a.second, b.second) - GEOM_EPS && p.second <= fmaxf_(a.second, b.second) + GEOM_EPS;
}

bool point_in_rect(Point point, Rect4 rect) {
    auto [rx, ry, rw, rh] = rect;
    return point.first >= rx && point.first <= rx + rw && point.second >= ry && point.second <= ry + rh;
}

bool points_near(Point a, Point b) {
    return std::fabs(a.first - b.first) <= POINT_EPS && std::fabs(a.second - b.second) <= POINT_EPS;
}

bool point_near_segment(Point point, Point a, Point b, float eps) {
    float dx = b.first - a.first;
    float dy = b.second - a.second;
    float len2 = dx * dx + dy * dy;
    if (len2 <= GEOM_EPS) return points_near(point, a);
    float t = ((point.first - a.first) * dx + (point.second - a.second) * dy) / len2;
    if (!(t >= -GEOM_EPS && t <= 1.0f + GEOM_EPS)) return false;
    float clamped_t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    Point proj{a.first + dx * clamped_t, a.second + dy * clamped_t};
    return std::hypot(point.first - proj.first, point.second - proj.second) <= eps;
}

bool point_in_polygon_strict(Point point, const std::vector<Point>& polygon) {
    if (polygon.size() < 3) return false;
    for (size_t i = 0; i < polygon.size(); ++i)
        if (point_near_segment(point, polygon[i], polygon[(i + 1) % polygon.size()], POINT_EPS)) return false;
    bool inside = false;
    float px = point.first, py = point.second;
    Point prev = polygon.back();
    for (const Point& curr : polygon) {
        if ((curr.second > py) != (prev.second > py)) {
            float denom = prev.second - curr.second;
            if (std::fabs(denom) > GEOM_EPS) {
                float x_at_y = (prev.first - curr.first) * (py - curr.second) / denom + curr.first;
                if (px < x_at_y) inside = !inside;
            }
        }
        prev = curr;
    }
    return inside;
}

bool point_inside_node_bounds_strict(const NodeLayout& n, Point p) {
    return p.first > n.x + POINT_EPS && p.first < n.x + n.width - POINT_EPS && p.second > n.y + POINT_EPS &&
           p.second < n.y + n.height - POINT_EPS;
}

bool point_inside_node_shape_strict(const NodeLayout& node, Point point) {
    switch (node.shape) {
    case NodeShape::Circle:
    case NodeShape::DoubleCircle: {
        Point c = node_center(node);
        float rx = fmaxf_(node.width * 0.5f - POINT_EPS, 1.0f);
        float ry = fmaxf_(node.height * 0.5f - POINT_EPS, 1.0f);
        float nx = (point.first - c.first) / rx;
        float ny = (point.second - c.second) / ry;
        return nx * nx + ny * ny < 1.0f;
    }
    default: {
        auto poly = shape_polygon_points(node);
        if (poly) return point_in_polygon_strict(point, *poly);
        return point_inside_node_bounds_strict(node, point);
    }
    }
}

}  // namespace

int f2i(float v) {
    if (v != v) return 0;
    if (v >= 2147483648.0f) return std::numeric_limits<int>::max();
    if (v <= -2147483648.0f) return std::numeric_limits<int>::min();
    return static_cast<int>(v);
}

Point node_center(const NodeLayout& node) { return {node.x + node.width * 0.5f, node.y + node.height * 0.5f}; }

EdgeSide endpoint_side_for_point(const NodeLayout& node, Point point) {
    float left = std::fabs(point.first - node.x);
    float right = std::fabs(point.first - (node.x + node.width));
    float top = std::fabs(point.second - node.y);
    float bottom = std::fabs(point.second - (node.y + node.height));
    float best = left;
    EdgeSide best_side = EdgeSide::Left;
    if (right < best) { best = right; best_side = EdgeSide::Right; }
    if (top < best) { best = top; best_side = EdgeSide::Top; }
    if (bottom < best) { best = bottom; best_side = EdgeSide::Bottom; }
    return best_side;
}

bool side_points_outward(EdgeSide side, Point endpoint, Point outside) {
    switch (side) {
    case EdgeSide::Left: return outside.first <= endpoint.first + POINT_EPS;
    case EdgeSide::Right: return outside.first >= endpoint.first - POINT_EPS;
    case EdgeSide::Top: return outside.second <= endpoint.second + POINT_EPS;
    case EdgeSide::Bottom: return outside.second >= endpoint.second - POINT_EPS;
    }
    return false;
}

bool source_exits_outward(EdgeSide side, Point start, Point next) { return side_points_outward(side, start, next); }
bool target_enters_from_outside(EdgeSide side, Point prev, Point end) { return side_points_outward(side, end, prev); }

bool segment_intrudes_endpoint_rect(EdgeSide side, Point outside, Point endpoint, const NodeLayout& node) {
    bool within_y = endpoint.second >= node.y - POINT_EPS && endpoint.second <= node.y + node.height + POINT_EPS;
    bool within_x = endpoint.first >= node.x - POINT_EPS && endpoint.first <= node.x + node.width + POINT_EPS;
    switch (side) {
    case EdgeSide::Left: return within_y && outside.first > endpoint.first + POINT_EPS;
    case EdgeSide::Right: return within_y && outside.first < endpoint.first - POINT_EPS;
    case EdgeSide::Top: return within_x && outside.second > endpoint.second + POINT_EPS;
    case EdgeSide::Bottom: return within_x && outside.second < endpoint.second - POINT_EPS;
    }
    return false;
}

std::optional<std::vector<Point>> shape_polygon_points(const NodeLayout& node) {
    float x = node.x, y = node.y, w = node.width, h = node.height;
    switch (node.shape) {
    case NodeShape::Rectangle:
    case NodeShape::ForkJoin:
    case NodeShape::RoundRect:
    case NodeShape::ActorBox:
    case NodeShape::Stadium:
    case NodeShape::Subroutine:
    case NodeShape::Text:
    case NodeShape::MindmapDefault:
        return std::vector<Point>{{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    case NodeShape::Diamond: {
        float cx = x + w / 2.0f;
        float cy = y + h / 2.0f;
        return std::vector<Point>{{cx, y}, {x + w, cy}, {cx, y + h}, {x, cy}};
    }
    case NodeShape::Hexagon: {
        float x1 = x + w * 0.25f;
        float x2 = x + w * 0.75f;
        float y_mid = y + h / 2.0f;
        return std::vector<Point>{{x1, y}, {x2, y}, {x + w, y_mid}, {x2, y + h}, {x1, y + h}, {x, y_mid}};
    }
    case NodeShape::Parallelogram:
    case NodeShape::ParallelogramAlt: {
        float offset = w * 0.18f;
        if (node.shape == NodeShape::Parallelogram)
            return std::vector<Point>{{x + offset, y}, {x + w, y}, {x + w - offset, y + h}, {x, y + h}};
        return std::vector<Point>{{x, y}, {x + w - offset, y}, {x + w, y + h}, {x + offset, y + h}};
    }
    case NodeShape::Trapezoid:
    case NodeShape::TrapezoidAlt: {
        float offset = w * 0.18f;
        if (node.shape == NodeShape::Trapezoid)
            return std::vector<Point>{{x + offset, y}, {x + w - offset, y}, {x + w, y + h}, {x, y + h}};
        return std::vector<Point>{{x, y}, {x + w, y}, {x + w - offset, y + h}, {x + offset, y + h}};
    }
    case NodeShape::Asymmetric: {
        float slant = w * 0.22f;
        return std::vector<Point>{{x, y}, {x + w - slant, y}, {x + w, y + h / 2.0f}, {x + w - slant, y + h}, {x, y + h}};
    }
    case NodeShape::Circle:
    case NodeShape::DoubleCircle:
    case NodeShape::Cylinder:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<Point> ray_polygon_intersection(Point origin, Point dir, const std::vector<Point>& poly) {
    std::optional<float> best_t;
    float ox = origin.first, oy = origin.second, rx = dir.first, ry = dir.second;
    if (poly.size() < 2) return std::nullopt;
    for (size_t i = 0; i < poly.size(); ++i) {
        auto [x1, y1] = poly[i];
        auto [x2, y2] = poly[(i + 1) % poly.size()];
        float sx = x2 - x1, sy = y2 - y1;
        float qx = x1 - ox, qy = y1 - oy;
        float denom = rx * sy - ry * sx;
        if (std::fabs(denom) < GEOM_EPS) continue;
        float t = (qx * sy - qy * sx) / denom;
        float u = (qx * ry - qy * rx) / denom;
        if (t >= 0.0f && (u >= 0.0f && u <= 1.0f)) {
            if (best_t && t >= *best_t) continue;
            best_t = t;
        }
    }
    if (!best_t) return std::nullopt;
    return Point{ox + rx * *best_t, oy + ry * *best_t};
}

std::optional<Point> ray_ellipse_intersection(Point origin, Point dir, Point center, float rx, float ry) {
    float ox = origin.first - center.first;
    float oy = origin.second - center.second;
    float dx = dir.first, dy = dir.second;
    float a = (dx * dx) / (rx * rx) + (dy * dy) / (ry * ry);
    float b = 2.0f * ((ox * dx) / (rx * rx) + (oy * dy) / (ry * ry));
    float c = (ox * ox) / (rx * rx) + (oy * oy) / (ry * ry) - 1.0f;
    float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f || std::fabs(a) < GEOM_EPS) return std::nullopt;
    float sqrt_disc = std::sqrt(disc);
    float t1 = (-b - sqrt_disc) / (2.0f * a);
    float t2 = (-b + sqrt_disc) / (2.0f * a);
    float t;
    if (t1 >= 0.0f) t = t1;
    else if (t2 >= 0.0f) t = t2;
    else return std::nullopt;
    return Point{origin.first + dx * t, origin.second + dy * t};
}

bool segment_hits_node_shape_interior(Point a, Point b, const NodeLayout& node) {
    float len = std::hypot(b.first - a.first, b.second - a.second);
    size_t steps = std::max<size_t>(f2usize(std::ceil(len / 4.0f)), 2);
    for (size_t i = 1; i < steps; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(steps);
        if (point_inside_node_shape_strict(node, {a.first + (b.first - a.first) * t, a.second + (b.second - a.second) * t}))
            return true;
    }
    return false;
}

float path_length(const std::vector<Point>& points) {
    float sum = 0.0f;
    for (size_t i = 0; i + 1 < points.size(); ++i)
        sum += std::hypot(points[i + 1].first - points[i].first, points[i + 1].second - points[i].second);
    return sum;
}

std::optional<Point> path_point_at_progress(const std::vector<Point>& points, float progress) {
    if (points.size() < 2) return std::nullopt;
    float total = path_length(points);
    if (!std::isfinite(total) || total <= GEOM_EPS) return points[0];
    float p = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
    float remain = total * p;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        Point a = points[i], b = points[i + 1];
        float dx = b.first - a.first, dy = b.second - a.second;
        float seg_len = std::sqrt(dx * dx + dy * dy);
        if (seg_len <= GEOM_EPS) continue;
        if (remain <= seg_len) {
            float t = remain / seg_len;
            return Point{a.first + dx * t, a.second + dy * t};
        }
        remain -= seg_len;
    }
    return points.back();
}

size_t path_bend_count(const std::vector<Point>& points) {
    if (points.size() < 3) return 0;
    size_t bends = 0;
    for (size_t i = 1; i + 1 < points.size(); ++i) {
        Point p0 = points[i - 1], p1 = points[i], p2 = points[i + 1];
        float dx1 = p1.first - p0.first, dy1 = p1.second - p0.second;
        float dx2 = p2.first - p1.first, dy2 = p2.second - p1.second;
        if ((std::fabs(dx1) <= 1e-4f && std::fabs(dy1) <= 1e-4f) || (std::fabs(dx2) <= 1e-4f && std::fabs(dy2) <= 1e-4f))
            continue;
        float cross = dx1 * dy2 - dy1 * dx2;
        if (std::fabs(cross) > 1e-4f) ++bends;
    }
    return bends;
}

bool segments_intersect(Point a, Point b, Point c, Point d) {
    float o1 = orient(a, b, c);
    float o2 = orient(a, b, d);
    float o3 = orient(c, d, a);
    float o4 = orient(c, d, b);
    if (((o1 > 0.0f && o2 < 0.0f) || (o1 < 0.0f && o2 > 0.0f)) && ((o3 > 0.0f && o4 < 0.0f) || (o3 < 0.0f && o4 > 0.0f)))
        return true;
    if (std::fabs(o1) <= GEOM_EPS && on_segment(a, b, c)) return true;
    if (std::fabs(o2) <= GEOM_EPS && on_segment(a, b, d)) return true;
    if (std::fabs(o3) <= GEOM_EPS && on_segment(c, d, a)) return true;
    if (std::fabs(o4) <= GEOM_EPS && on_segment(c, d, b)) return true;
    return false;
}

bool segment_intersects_rect_bounds(Point a, Point b, Rect4 rect) {
    auto [rx, ry, rw, rh] = rect;
    if (rw <= 0.0f || rh <= 0.0f) return false;
    float min_x = fminf_(a.first, b.first), max_x = fmaxf_(a.first, b.first);
    float min_y = fminf_(a.second, b.second), max_y = fmaxf_(a.second, b.second);
    if (max_x < rx || min_x > rx + rw || max_y < ry || min_y > ry + rh) return false;
    if (point_in_rect(a, rect) || point_in_rect(b, rect)) return true;
    Point corners[4] = {{rx, ry}, {rx + rw, ry}, {rx + rw, ry + rh}, {rx, ry + rh}};
    for (int i = 0; i < 4; ++i)
        if (segments_intersect(a, b, corners[i], corners[(i + 1) % 4])) return true;
    return false;
}

}  // namespace mermaid
