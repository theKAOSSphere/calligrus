// Mermaid port: helpers shared by the label placement source files. Mirrors the small functions
// and constants at the top and bottom of layout/label_placement.rs (pure geometry, no SVG).
//
// label_placement.cpp          centre-label driver, penalties, tightening passes
// label_placement_flowchart.cpp  flowchart-only global assignment / de-overlap passes
// label_placement_endpoint.cpp   start/end labels
//
// The Rust code is a pile of scored candidate searches, so the port keeps every constant, loop
// order and tie-break of the original: one different float operation order can flip a choice.
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "geometry.h"
#include "layout_internal.h"
#include "strutil.h"

namespace mermaid::lp {

constexpr float LABEL_OVERLAP_WIDE_THRESHOLD = 1e-4f;
constexpr float LABEL_ANCHOR_FRACTIONS[5] = {0.5f, 0.35f, 0.65f, 0.2f, 0.8f};
constexpr float LABEL_ANCHOR_POS_EPS = 1.0f;
constexpr float LABEL_ANCHOR_DIR_EPS = 0.02f;
constexpr size_t LABEL_EXTRA_SEGMENT_ANCHORS = 6;
constexpr float FLOWCHART_LABEL_CLEARANCE_PAD = 1.5f;
constexpr float FLOWCHART_LABEL_SOFT_GAP = 6.0f;

constexpr float WEIGHT_NODE_OVERLAP = 1.6f;
constexpr float WEIGHT_NODE_OVERLAP_FLOWCHART = 2.6f;
constexpr float WEIGHT_LABEL_OVERLAP = 1.0f;
constexpr float WEIGHT_FLOWCHART_LABEL_OVERLAP = 1.5f;
constexpr float WEIGHT_EDGE_OVERLAP = 0.45f;
constexpr float WEIGHT_FLOWCHART_EDGE_OVERLAP = 1.15f;
constexpr float WEIGHT_OUTSIDE = 1.2f;
constexpr float OWN_EDGE_GAP_TARGET = 1.2f;
constexpr float OWN_EDGE_GAP_TARGET_FLOWCHART = 1.8f;
constexpr float OWN_EDGE_GAP_TARGET_CLASS = 0.35f;
constexpr float OWN_EDGE_GAP_UNDER_WEIGHT = 0.7f;
constexpr float OWN_EDGE_GAP_UNDER_WEIGHT_FLOWCHART = 1.6f;
constexpr float OWN_EDGE_GAP_UNDER_WEIGHT_CLASS = 0.08f;
constexpr float OWN_EDGE_GAP_OVER_WEIGHT = 0.06f;
constexpr float OWN_EDGE_GAP_OVER_WEIGHT_FLOWCHART = 0.65f;
constexpr float OWN_EDGE_GAP_OVER_WEIGHT_CLASS = 0.18f;
constexpr float OWN_EDGE_TOUCH_HARD_PENALTY = 0.25f;
constexpr float OWN_EDGE_TOUCH_HARD_PENALTY_FLOWCHART = 1.25f;
constexpr float OWN_EDGE_TOUCH_HARD_PENALTY_CLASS = 0.02f;
constexpr float FLOWCHART_OWN_EDGE_SOFT_MAX_GAP = 6.0f;
constexpr float FLOWCHART_OWN_EDGE_HARD_MAX_GAP = 10.0f;
constexpr float FLOWCHART_OWN_EDGE_SOFT_MAX_GAP_WEIGHT = 0.85f;
constexpr float FLOWCHART_OWN_EDGE_HARD_MAX_GAP_WEIGHT = 4.5f;
constexpr float FLOWCHART_FOREIGN_EDGE_OVERLAP_WEIGHT = 0.9f;
constexpr float FLOWCHART_FOREIGN_EDGE_TOUCH_HARD_PENALTY = 2.0f;
constexpr float STATE_OWN_EDGE_HARD_MAX_GAP = 7.0f;
constexpr float CLASS_OWN_EDGE_HARD_MAX_GAP = 7.0f;
constexpr float DEFAULT_OWN_EDGE_HARD_MAX_GAP = 8.0f;

constexpr float INF = std::numeric_limits<float>::infinity();

// ---- tiny float helpers with Rust semantics -------------------------------------------------

// f32::max / f32::min (a NaN operand is ignored).
inline float fmax_(float a, float b) { return std::fmax(a, b); }
inline float fmin_(float a, float b) { return std::fmin(a, b); }
// f32::clamp
inline float clamp_(float v, float lo, float hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}
// Rust `partial_cmp(..).unwrap_or(Equal)` as a -1/0/1 result.
inline int cmp_f(float a, float b) { return a < b ? -1 : (a > b ? 1 : 0); }

// ---- value types ----------------------------------------------------------------------------

// Rust `type Rect = (f32, f32, f32, f32)`: x, y, width, height.
struct Rect {
    float x, y, w, h;
};
// (x, y, dir_x, dir_y): a point on an edge and the unit direction there.
struct Anchor {
    float x, y, dx, dy;
};
using Cost = std::pair<float, float>;  // (primary, secondary), compared with candidate_better
using Bounds = std::optional<std::pair<float, float>>;

struct EdgeObstacle {
    size_t edge;
    Rect rect;
};

struct CenterLabelOverlapScore {
    size_t count = 0;
    float total = 0.0f;
    float max = 0.0f;
    // Rust `self.improved_by(other)`: true when `other` is the better (lower) score.
    bool improved_by(const CenterLabelOverlapScore& other) const {
        return count < other.count || (count == other.count && max + 0.1f < other.max) ||
               (count == other.count && std::fabs(max - other.max) <= 0.1f && total + 0.1f < other.total);
    }
};

struct FlowchartCenterLabelEntry {
    size_t edge_idx = 0;
    float label_w = 0, label_h = 0;
    Point initial_center{0.0f, 0.0f};
    float initial_s_norm = 0, initial_d_signed = 0;
    Point current_center{0.0f, 0.0f};
    std::vector<Point> edge_points;
    std::vector<Point> candidates;
};

// ---- per-kind tuning ------------------------------------------------------------------------

inline float edge_distance_weight(DiagramKind kind, float overlap_pressure) {
    float base;
    switch (kind) {
        case DiagramKind::Flowchart: base = 0.72f; break;
        case DiagramKind::State: base = 0.38f; break;
        case DiagramKind::Class: base = 0.24f; break;
        default: base = 0.16f; break;
    }
    if (overlap_pressure <= 0.025f) return base;
    if (overlap_pressure <= 0.10f) {
        switch (kind) {
            case DiagramKind::Flowchart: return base * 0.92f;
            case DiagramKind::State: return base * 0.80f;
            default: return base * 0.55f;
        }
    }
    switch (kind) {
        case DiagramKind::Flowchart: return base * 0.68f;
        case DiagramKind::State: return base * 0.55f;
        default: return base * 0.2f;
    }
}

inline float center_label_node_obstacle_pad(DiagramKind kind, const Theme& theme, float label_pad_x,
                                            float label_pad_y) {
    switch (kind) {
        case DiagramKind::Flowchart:
            return fmax_(theme.font_size * 0.55f, fmax_(label_pad_x, label_pad_y + FLOWCHART_LABEL_CLEARANCE_PAD));
        case DiagramKind::State: return fmax_(fmax_(theme.font_size * 0.22f, label_pad_y), 2.0f);
        case DiagramKind::Class: return fmax_(fmax_(theme.font_size * 0.28f, label_pad_y), 2.2f);
        default: return fmax_(theme.font_size * 0.45f, fmax_(label_pad_x, label_pad_y));
    }
}

inline float edge_target_distance(DiagramKind kind, float label_h, float label_pad_y) {
    if (kind == DiagramKind::Flowchart) return fmax_(label_h * 0.52f + label_pad_y * 0.65f + 0.4f, 4.8f);
    return fmax_(label_h * 0.65f + label_pad_y, 6.0f);
}

inline bool flowchart_own_gap_allowed(float gap, float max_gap) {
    return std::isfinite(gap) && gap >= OWN_EDGE_GAP_TARGET_FLOWCHART * 0.5f && gap <= max_gap;
}

inline float sweep_bias(DiagramKind kind, float tangent_step, float normal_step) {
    float normal_w, tangent_w;
    if (kind == DiagramKind::Flowchart) { normal_w = 0.018f; tangent_w = 0.004f; }
    else { normal_w = 0.010f; tangent_w = 0.003f; }
    return std::fabs(normal_step) * normal_w + std::fabs(tangent_step) * tangent_w;
}

// edge_label_padding(kind, config) is declared in layout_internal.h (defined in layout.cpp).

inline std::pair<float, float> endpoint_label_padding(DiagramKind kind) {
    switch (kind) {
        case DiagramKind::State: return {2.6f, 1.4f};
        case DiagramKind::Flowchart: return {3.4f, 1.8f};
        case DiagramKind::Class: return {3.2f, 1.6f};
        default: return {3.0f, 1.6f};
    }
}

inline std::optional<float> center_label_hard_max_gap(DiagramKind kind) {
    switch (kind) {
        case DiagramKind::Flowchart: return FLOWCHART_OWN_EDGE_HARD_MAX_GAP;
        case DiagramKind::State: return STATE_OWN_EDGE_HARD_MAX_GAP;
        case DiagramKind::Class: return CLASS_OWN_EDGE_HARD_MAX_GAP;
        case DiagramKind::Sequence:
        case DiagramKind::ZenUML: return std::nullopt;
        default: return DEFAULT_OWN_EDGE_HARD_MAX_GAP;
    }
}

// ---- plain geometry -------------------------------------------------------------------------

inline float edge_path_length(const EdgeLayout& edge) {
    float total = 0.0f;
    for (size_t i = 0; i + 1 < edge.points.size(); ++i) {
        float dx = edge.points[i + 1].first - edge.points[i].first;
        float dy = edge.points[i + 1].second - edge.points[i].second;
        total += std::sqrt(dx * dx + dy * dy);
    }
    return total;
}

inline float polyline_path_length(const std::vector<Point>& points) {
    float total = 0.0f;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        float dx = points[i + 1].first - points[i].first;
        float dy = points[i + 1].second - points[i].second;
        total += std::sqrt(dx * dx + dy * dy);
    }
    return total;
}

inline float point_segment_distance(Point point, Point a, Point b) {
    float vx = b.first - a.first;
    float vy = b.second - a.second;
    float len2 = vx * vx + vy * vy;
    if (len2 <= 1e-6f) {
        float dx = point.first - a.first;
        float dy = point.second - a.second;
        return std::sqrt(dx * dx + dy * dy);
    }
    float t = ((point.first - a.first) * vx + (point.second - a.second) * vy) / len2;
    t = clamp_(t, 0.0f, 1.0f);
    float proj_x = a.first + vx * t;
    float proj_y = a.second + vy * t;
    float dx = point.first - proj_x;
    float dy = point.second - proj_y;
    return std::sqrt(dx * dx + dy * dy);
}

inline float point_polyline_distance(Point point, const std::vector<Point>& points) {
    if (points.size() < 2) return 0.0f;
    float best = INF;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        float dist = point_segment_distance(point, points[i], points[i + 1]);
        if (dist < best) best = dist;
    }
    return std::isfinite(best) ? best : 0.0f;
}

inline float point_rect_distance(Point point, const Rect& rect) {
    float min_x = rect.x, min_y = rect.y;
    float max_x = rect.x + rect.w, max_y = rect.y + rect.h;
    float dx = point.first < min_x ? min_x - point.first : (point.first > max_x ? point.first - max_x : 0.0f);
    float dy = point.second < min_y ? min_y - point.second : (point.second > max_y ? point.second - max_y : 0.0f);
    return std::sqrt(dx * dx + dy * dy);
}

inline bool point_inside_rect(Point point, const Rect& rect) {
    return point.first >= rect.x && point.first <= rect.x + rect.w && point.second >= rect.y &&
           point.second <= rect.y + rect.h;
}

inline float orientation(Point a, Point b, Point c) {
    return (b.first - a.first) * (c.second - a.second) - (b.second - a.second) * (c.first - a.first);
}

inline bool point_on_segment(Point point, Point a, Point b, float eps) {
    return point.first >= fmin_(a.first, b.first) - eps && point.first <= fmax_(a.first, b.first) + eps &&
           point.second >= fmin_(a.second, b.second) - eps && point.second <= fmax_(a.second, b.second) + eps;
}

// label_placement.rs has its own segments_intersect (eps 1e-4), unlike geometry.h's.
inline bool lp_segments_intersect(Point a, Point b, Point c, Point d) {
    const float eps = 1e-4f;
    float o1 = orientation(a, b, c);
    float o2 = orientation(a, b, d);
    float o3 = orientation(c, d, a);
    float o4 = orientation(c, d, b);
    bool crosses = ((o1 > eps && o2 < -eps) || (o1 < -eps && o2 > eps)) &&
                   ((o3 > eps && o4 < -eps) || (o3 < -eps && o4 > eps));
    if (crosses) return true;
    if (std::fabs(o1) <= eps && point_on_segment(c, a, b, eps)) return true;
    if (std::fabs(o2) <= eps && point_on_segment(d, a, b, eps)) return true;
    if (std::fabs(o3) <= eps && point_on_segment(a, c, d, eps)) return true;
    if (std::fabs(o4) <= eps && point_on_segment(b, c, d, eps)) return true;
    return false;
}

inline bool segment_intersects_rect_lp(Point a, Point b, const Rect& rect) {
    if (point_inside_rect(a, rect) || point_inside_rect(b, rect)) return true;
    float x0 = rect.x, y0 = rect.y, x1 = rect.x + rect.w, y1 = rect.y + rect.h;
    Point corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 4; ++i)
        if (lp_segments_intersect(a, b, corners[i], corners[(i + 1) % 4])) return true;
    return false;
}

inline float segment_rect_distance(Point a, Point b, const Rect& rect) {
    if (segment_intersects_rect_lp(a, b, rect)) return 0.0f;
    float best = fmin_(point_rect_distance(a, rect), point_rect_distance(b, rect));
    float x0 = rect.x, y0 = rect.y, x1 = rect.x + rect.w, y1 = rect.y + rect.h;
    Point corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (const Point& c : corners) best = fmin_(best, point_segment_distance(c, a, b));
    return best;
}

inline float polyline_rect_distance(const std::vector<Point>& points, const Rect& rect) {
    if (points.size() < 2) return INF;
    float best = INF;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        float dist = segment_rect_distance(points[i], points[i + 1], rect);
        if (dist < best) best = dist;
        if (best <= 0.0f) break;
    }
    return best;
}

inline float overlap_area(const Rect& a, const Rect& b) {
    float x0 = fmax_(a.x, b.x);
    float y0 = fmax_(a.y, b.y);
    float x1 = fmin_(a.x + a.w, b.x + b.w);
    float y1 = fmin_(a.y + a.h, b.y + b.h);
    float w = fmax_(x1 - x0, 0.0f);
    float h = fmax_(y1 - y0, 0.0f);
    return w * h;
}

// (count of obstacles overlapping more than `threshold`, their total area)
inline std::pair<unsigned, float> overlap_stats(const Rect& rect, const std::vector<Rect>& obstacles,
                                                float threshold) {
    unsigned count = 0;
    float area = 0.0f;
    for (const Rect& o : obstacles) {
        float ov = overlap_area(rect, o);
        if (ov > threshold) {
            ++count;
            area += ov;
        }
    }
    return {count, area};
}

inline float rect_gap(const Rect& a, const Rect& b) {
    float ax0 = a.x, ay0 = a.y, ax1 = a.x + a.w, ay1 = a.y + a.h;
    float bx0 = b.x, by0 = b.y, bx1 = b.x + b.w, by1 = b.y + b.h;
    float dx = ax1 < bx0 ? bx0 - ax1 : (bx1 < ax0 ? ax0 - bx1 : 0.0f);
    float dy = ay1 < by0 ? by0 - ay1 : (by1 < ay0 ? ay0 - by1 : 0.0f);
    return std::sqrt(dx * dx + dy * dy);
}

inline Rect inflate_rect(Rect rect, float pad) {
    if (pad <= 0.0f) return rect;
    return {rect.x - pad, rect.y - pad, rect.w + pad * 2.0f, rect.h + pad * 2.0f};
}

inline float outside_area(const Rect& rect, std::pair<float, float> bounds) {
    auto [w, h] = bounds;
    float rect_area = fmax_(rect.w, 0.0f) * fmax_(rect.h, 0.0f);
    if (rect_area <= 0.0f) return 0.0f;
    float x0 = fmax_(rect.x, 0.0f);
    float y0 = fmax_(rect.y, 0.0f);
    float x1 = fmin_(rect.x + rect.w, w);
    float y1 = fmin_(rect.y + rect.h, h);
    float inside_w = fmax_(x1 - x0, 0.0f);
    float inside_h = fmax_(y1 - y0, 0.0f);
    return rect_area - inside_w * inside_h;
}

inline Point clamp_label_center_to_bounds(Point center, float label_w, float label_h, float pad_x, float pad_y,
                                          std::pair<float, float> bounds) {
    auto [w, h] = bounds;
    if (w <= 0.0f || h <= 0.0f) return center;
    float min_x = label_w * 0.5f + pad_x;
    float min_y = label_h * 0.5f + pad_y;
    float max_x = w - label_w * 0.5f - pad_x;
    float max_y = h - label_h * 0.5f - pad_y;
    float x = max_x < min_x ? center.first : clamp_(center.first, min_x, max_x);
    float y = max_y < min_y ? center.second : clamp_(center.second, min_y, max_y);
    return {x, y};
}

// Rust `points.windows(2).any(|s| segment_intersects_rect(s[0], s[1], rect))`
inline bool path_intersects_rect(const std::vector<Point>& points, const Rect& rect) {
    for (size_t i = 0; i + 1 < points.size(); ++i)
        if (segment_intersects_rect_lp(points[i], points[i + 1], rect)) return true;
    return false;
}

inline Rect label_core_rect(Point center, const TextBlock& label) {
    return {center.first - label.width / 2.0f, center.second - label.height / 2.0f, label.width, label.height};
}

// The label rectangle including its padding. (The Rust code writes the half size as `/ 2.0` in
// some places and `* 0.5` in others; both are exact, so one helper serves both.)
inline Rect padded_label_rect(Point c, float w, float h, float pad_x, float pad_y) {
    return {c.first - w * 0.5f - pad_x, c.second - h * 0.5f - pad_y, w + 2.0f * pad_x, h + 2.0f * pad_y};
}

inline bool candidate_better(Cost candidate, Cost best) {
    if (candidate.first + 1e-6f < best.first) return true;
    return std::fabs(candidate.first - best.first) <= 1e-6f && candidate.second + 1e-6f < best.second;
}

inline void push_center_unique(std::vector<Point>& centers, Point candidate) {
    for (const Point& c : centers)
        if (std::fabs(c.first - candidate.first) <= 0.35f && std::fabs(c.second - candidate.second) <= 0.35f) return;
    centers.push_back(candidate);
}

// ---- obstacle lists -------------------------------------------------------------------------

std::optional<Rect> subgraph_label_rect(const SubgraphLayout& sub, DiagramKind kind, const Theme& theme);

std::vector<Rect> build_label_obstacles(const std::map<std::string, NodeLayout>& nodes,
                                        const std::vector<SubgraphLayout>& subgraphs, DiagramKind kind,
                                        const Theme& theme, float node_obstacle_pad, float subgraph_label_pad);
std::vector<Rect> build_node_text_obstacles(const std::map<std::string, NodeLayout>& nodes, float pad);
std::vector<EdgeObstacle> build_edge_obstacles(const std::vector<EdgeLayout>& edges, float pad);

inline std::vector<Rect> obstacle_rects(const std::vector<EdgeObstacle>& obs) {
    std::vector<Rect> r;
    r.reserve(obs.size());
    for (const auto& o : obs) r.push_back(o.rect);
    return r;
}

// ---- anchors on an edge ---------------------------------------------------------------------

Anchor edge_label_anchor(const EdgeLayout& edge);
std::optional<Anchor> edge_label_anchor_at_fraction(const EdgeLayout& edge, float t);
std::optional<Anchor> edge_label_anchor_from_point(const EdgeLayout& edge, Point point);
std::vector<Anchor> edge_segment_anchors(const EdgeLayout& edge, size_t max_count);
std::vector<Anchor> edge_terminal_segment_anchors(const EdgeLayout& edge, size_t max_count);
void push_anchor_unique(std::vector<Anchor>& anchors, Anchor c);
std::vector<std::optional<float>> edge_label_bundle_fractions(const std::vector<EdgeLayout>& edges);

// ---- obstacle grid --------------------------------------------------------------------------

// Spatial index for fast overlap queries: cell (ix, iy) -> indices of the rects touching it.
class ObstacleGrid {
public:
    ObstacleGrid(float cell, const std::vector<Rect>& rects) : cell_(fmax_(cell, 16.0f)) {
        for (size_t i = 0; i < rects.size(); ++i) insert(i, rects[i]);
    }
    void insert(size_t idx, const Rect& rect) {
        int x0 = f2i(std::floor(rect.x / cell_));
        int y0 = f2i(std::floor(rect.y / cell_));
        int x1 = f2i(std::floor((rect.x + rect.w) / cell_));
        int y1 = f2i(std::floor((rect.y + rect.h) / cell_));
        for (int ix = x0; ix <= x1; ++ix)
            for (int iy = y0; iy <= y1; ++iy) cells_[key(ix, iy)].push_back(idx);
    }
    // Indices of obstacles that could overlap `rect`, each once, in cell-scan order.
    std::vector<size_t> query(const Rect& rect) const {
        int x0 = f2i(std::floor(rect.x / cell_));
        int y0 = f2i(std::floor(rect.y / cell_));
        int x1 = f2i(std::floor((rect.x + rect.w) / cell_));
        int y1 = f2i(std::floor((rect.y + rect.h) / cell_));
        std::vector<size_t> out;
        std::set<size_t> seen;
        for (int ix = x0; ix <= x1; ++ix)
            for (int iy = y0; iy <= y1; ++iy) {
                auto it = cells_.find(key(ix, iy));
                if (it == cells_.end()) continue;
                for (size_t idx : it->second)
                    if (seen.insert(idx).second) out.push_back(idx);
            }
        return out;
    }

private:
    static long long key(int ix, int iy) { return (static_cast<long long>(ix) << 32) ^ static_cast<unsigned>(iy); }
    float cell_;
    std::unordered_map<long long, std::vector<size_t>> cells_;
};

// ---- the penalty function every search uses -------------------------------------------------

struct LabelPenaltyContext {
    DiagramKind kind;
    const std::vector<Rect>& occupied;
    const ObstacleGrid& occupied_grid;
    size_t node_obstacle_count;
    const std::vector<EdgeObstacle>& edge_obstacles;
    const ObstacleGrid& edge_grid;
    size_t edge_idx;
    const std::vector<Point>& own_edge_points;
    Bounds bounds;
};

// (overlap pressure per label area, distance of the label centre from `anchor` per label size)
Cost label_penalties(Rect rect, Point anchor, float label_w, float label_h, const LabelPenaltyContext& ctx);

// ---- flowchart-only passes (label_placement_flowchart.cpp) ----------------------------------

// deoverlap_flowchart_center_labels: global assignment + refinement of centre labels.
void deoverlap_flowchart_center_labels(std::vector<EdgeLayout>& edges, const std::map<std::string, NodeLayout>& nodes,
                                       const std::vector<SubgraphLayout>& subgraphs, Bounds bounds,
                                       const Theme& theme, float label_pad_x, float label_pad_y,
                                       const std::set<size_t>& locked_indices);

inline Rect flowchart_center_label_rect(Point center, float label_w, float label_h, float pad_x, float pad_y) {
    return padded_label_rect(center, label_w, label_h, pad_x, pad_y);
}

}  // namespace mermaid::lp
