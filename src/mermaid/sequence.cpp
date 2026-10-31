// Mermaid port: sequence diagram layout. Mirrors layout/sequence.rs of mermaid-rs-renderer 0.3.1:
// participants become lanes (boxes at the top, footboxes at the bottom, a lifeline between),
// messages are horizontal rows, notes / frames (alt, loop, ...) / activations / numbers are
// placed on those rows, and message labels are put at the least cluttered spot next to their arrow.
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

#include "layout_internal.h"
#include "strutil.h"
#include "text.h"

namespace mermaid {

namespace {

using Rect = std::array<float, 4>;  // x, y, width, height (Rust `type Rect = (f32, f32, f32, f32)`)

constexpr float SEQUENCE_LABEL_PAD_X = 3.0f;
constexpr float SEQUENCE_LABEL_PAD_Y = 2.0f;
constexpr float SEQUENCE_ENDPOINT_LABEL_PAD_X = 2.5f;
constexpr float SEQUENCE_ENDPOINT_LABEL_PAD_Y = 1.5f;
constexpr float SEQUENCE_LABEL_TOUCH_EPS = 0.5f;
constexpr float SEQUENCE_CENTER_LABEL_GAP_MIN = 1.8f;
constexpr float SEQUENCE_CENTER_LABEL_GAP_MAX = 7.0f;
constexpr float SEQUENCE_CENTER_LABEL_FAR_GAP = 10.5f;
constexpr float SEQUENCE_ENDPOINT_LABEL_GAP_TARGET = 2.5f;
constexpr float SEQUENCE_ENDPOINT_LABEL_GAP_MIN = 1.0f;
constexpr float SEQUENCE_ENDPOINT_LABEL_GAP_MAX = 6.0f;
constexpr float SEQUENCE_ENDPOINT_LABEL_FAR_GAP = 10.0f;
constexpr float SEQUENCE_CENTER_LABEL_TANGENT_LINEAR_WEIGHT = 0.22f;
constexpr float SEQUENCE_CENTER_LABEL_TANGENT_QUAD_WEIGHT = 0.95f;
constexpr float SEQUENCE_CENTER_LABEL_TANGENT_SOFT_LIMIT = 1.2f;
constexpr float SEQUENCE_CENTER_LABEL_TANGENT_FAR_WEIGHT = 3.2f;
constexpr float kF32Epsilon = 1.1920929e-7f;
constexpr float kInf = std::numeric_limits<float>::infinity();

float fmax_(float a, float b) { return std::fmax(a, b); }
float fmin_(float a, float b) { return std::fmin(a, b); }
float clamp_(float v, float lo, float hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

struct SequenceGeometry {
    float actor_min_width, actor_min_height, actor_pad_y, lane_pitch, min_lane_gap, message_step, note_gap_y, note_gap_x,
        note_padding_x, note_padding_y, lane_side_pad_x, footbox_gap;

    static SequenceGeometry from_theme(const Theme& theme) {
        float font = fmax_(theme.font_size, 1.0f);
        SequenceGeometry g;
        g.actor_min_width = fmax_(font * 9.375f, 150.0f);
        g.actor_min_height = fmax_(font * 4.0625f, 65.0f);
        g.actor_pad_y = fmax_(font * 0.75f, 12.0f);
        g.lane_pitch = fmax_(font * 12.5f, 200.0f);
        g.min_lane_gap = fmax_(font * 3.125f, 50.0f);
        g.message_step = fmax_(font * 2.875f, 46.0f);
        g.note_gap_y = fmax_(font * 0.625f, 10.0f);
        g.note_gap_x = fmax_(font * 1.5625f, 25.0f);
        g.note_padding_x = fmax_(font * 1.0f, 15.0f);
        g.note_padding_y = fmax_(font * 0.55f, 6.0f);
        g.lane_side_pad_x = fmax_(font * 1.5625f, 25.0f);
        g.footbox_gap = fmax_(font * 1.375f, 22.0f);
        return g;
    }
};

TextBlock measure_sequence_text(const std::string& text, const Theme& theme, const LayoutConfig& config) {
    LayoutConfig sequence_config = config;
    sequence_config.max_label_width_chars = std::min<size_t>(sequence_config.max_label_width_chars, 14);
    return measure_label_with_font_size(text, fmax_(theme.font_size, 16.0f), sequence_config, true);
}

float sequence_lane_center(const NodeLayout& node) { return node.x + node.width / 2.0f; }

std::vector<float> compute_sequence_lane_centers(const std::vector<std::string>& participants,
                                                 const std::map<std::string, float>& participant_widths, const Graph& graph,
                                                 const Theme& theme, const LayoutConfig& config,
                                                 const SequenceGeometry& geometry) {
    if (participants.empty()) return {};
    std::unordered_map<std::string, size_t> participant_indices;
    for (size_t i = 0; i < participants.size(); ++i) participant_indices.emplace(participants[i], i);  // first wins? (collect: last wins)
    // HashMap::collect keeps the LAST value for a duplicate key; participants are unique here.
    auto width_of = [&](const std::string& id) {
        auto it = participant_widths.find(id);
        return it == participant_widths.end() ? geometry.actor_min_width : it->second;
    };

    std::vector<float> pitches;
    for (size_t i = 0; i + 1 < participants.size(); ++i) {
        float left_w = width_of(participants[i]);
        float right_w = width_of(participants[i + 1]);
        pitches.push_back(fmax_(geometry.lane_pitch, (left_w + right_w) * 0.5f + geometry.min_lane_gap));
    }

    for (const Edge& edge : graph.edges) {
        auto fi = participant_indices.find(edge.from), ti = participant_indices.find(edge.to);
        if (fi == participant_indices.end() || ti == participant_indices.end()) continue;
        size_t from_idx = fi->second, to_idx = ti->second;
        size_t left_idx = std::min(from_idx, to_idx), right_idx = std::max(from_idx, to_idx);
        if (right_idx != left_idx + 1) continue;
        if (edge.label) {
            TextBlock block = measure_sequence_text(*edge.label, theme, config);
            float base_center;
            if (left_idx == 0) {
                base_center = geometry.actor_min_width * 0.5f;
            } else {
                float sum = 0.0f;
                for (size_t k = 0; k < left_idx; ++k) sum += pitches[k];
                base_center = geometry.actor_min_width * 0.5f + sum;
            }
            float default_mid_x = base_center + geometry.lane_pitch * 0.5f;
            float max_label_x = default_mid_x + block.width * 0.5f;
            float right_min_center = max_label_x + geometry.note_gap_x;
            float current_right_center = base_center + pitches[left_idx];
            pitches[left_idx] = fmax_(pitches[left_idx], right_min_center - base_center);
            pitches[left_idx] = fmax_(pitches[left_idx], current_right_center - base_center);
        }
    }

    float first_width = width_of(participants[0]);
    std::vector<float> centers;
    centers.push_back(first_width * 0.5f);
    for (float pitch : pitches) centers.push_back(centers.back() + pitch);
    return centers;
}

// ---- geometry helpers of sequence.rs (own copies, eps values differ from label_placement.rs) ----

float distance(Point a, Point b) {
    float dx = a.first - b.first, dy = a.second - b.second;
    return std::sqrt(dx * dx + dy * dy);
}

float orient(Point a, Point b, Point c) {
    return (b.first - a.first) * (c.second - a.second) - (b.second - a.second) * (c.first - a.first);
}
bool on_segment(Point a, Point b, Point c) {
    const float EPS = 1e-6f;
    return c.first >= fmin_(a.first, b.first) - EPS && c.first <= fmax_(a.first, b.first) + EPS &&
           c.second >= fmin_(a.second, b.second) - EPS && c.second <= fmax_(a.second, b.second) + EPS;
}
bool segments_intersect(Point a, Point b, Point c, Point d) {
    const float EPS = 1e-6f;
    float o1 = orient(a, b, c), o2 = orient(a, b, d), o3 = orient(c, d, a), o4 = orient(c, d, b);
    if (std::fabs(o1) < EPS && on_segment(a, b, c)) return true;
    if (std::fabs(o2) < EPS && on_segment(a, b, d)) return true;
    if (std::fabs(o3) < EPS && on_segment(c, d, a)) return true;
    if (std::fabs(o4) < EPS && on_segment(c, d, b)) return true;
    return ((o1 > 0.0f) != (o2 > 0.0f)) && ((o3 > 0.0f) != (o4 > 0.0f));
}
bool point_in_rect(Point p, const Rect& r) {
    return p.first >= r[0] && p.first <= r[0] + r[2] && p.second >= r[1] && p.second <= r[1] + r[3];
}
bool segment_intersects_rect(Point a, Point b, const Rect& rect) {
    float x = rect[0], y = rect[1], w = rect[2], h = rect[3];
    float min_x = fmin_(a.first, b.first), max_x = fmax_(a.first, b.first);
    float min_y = fmin_(a.second, b.second), max_y = fmax_(a.second, b.second);
    if (max_x < x || min_x > x + w || max_y < y || min_y > y + h) return false;
    if (point_in_rect(a, rect) || point_in_rect(b, rect)) return true;
    Point corners[4] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    for (int i = 0; i < 4; ++i)
        if (segments_intersect(a, b, corners[i], corners[(i + 1) % 4])) return true;
    return false;
}
float point_to_segment_distance(Point point, Point a, Point b) {
    Point ab{b.first - a.first, b.second - a.second};
    float len_sq = ab.first * ab.first + ab.second * ab.second;
    if (len_sq <= kF32Epsilon) return distance(point, a);
    Point ap{point.first - a.first, point.second - a.second};
    float t = clamp_((ap.first * ab.first + ap.second * ab.second) / len_sq, 0.0f, 1.0f);
    Point proj{a.first + ab.first * t, a.second + ab.second * t};
    return distance(point, proj);
}
float point_to_polyline_distance(Point point, const std::vector<Point>& points) {
    if (points.empty()) return 0.0f;
    if (points.size() == 1) return distance(point, points[0]);
    float best = kInf;
    for (size_t i = 0; i + 1 < points.size(); ++i) best = fmin_(best, point_to_segment_distance(point, points[i], points[i + 1]));
    return best;
}
float point_rect_distance(Point point, const Rect& rect) {
    float min_x = rect[0], min_y = rect[1], max_x = rect[0] + rect[2], max_y = rect[1] + rect[3];
    float dx = point.first < min_x ? min_x - point.first : (point.first > max_x ? point.first - max_x : 0.0f);
    float dy = point.second < min_y ? min_y - point.second : (point.second > max_y ? point.second - max_y : 0.0f);
    return std::sqrt(dx * dx + dy * dy);
}
float segment_rect_distance(Point a, Point b, const Rect& rect) {
    if (segment_intersects_rect(a, b, rect)) return 0.0f;
    float best = fmin_(point_rect_distance(a, rect), point_rect_distance(b, rect));
    Point corners[4] = {{rect[0], rect[1]}, {rect[0] + rect[2], rect[1]}, {rect[0] + rect[2], rect[1] + rect[3]}, {rect[0], rect[1] + rect[3]}};
    for (const Point& c : corners) best = fmin_(best, point_to_segment_distance(c, a, b));
    return best;
}
float polyline_rect_gap(const std::vector<Point>& points, const Rect& rect) {
    if (points.size() < 2) return kInf;
    float best = kInf;
    for (size_t i = 0; i + 1 < points.size(); ++i) best = fmin_(best, segment_rect_distance(points[i], points[i + 1], rect));
    return best;
}
float rect_overlap_area(const Rect& a, const Rect& b) {
    float x1 = fmax_(a[0], b[0]), y1 = fmax_(a[1], b[1]);
    float x2 = fmin_(a[0] + a[2], b[0] + b[2]), y2 = fmin_(a[1] + a[3], b[1] + b[3]);
    if (x2 <= x1 || y2 <= y1) return 0.0f;
    return (x2 - x1) * (y2 - y1);
}
Rect label_rect(Point center, const TextBlock& label, float pad_x, float pad_y) {
    return {center.first - label.width / 2.0f - pad_x, center.second - label.height / 2.0f - pad_y, label.width + pad_x * 2.0f,
            label.height + pad_y * 2.0f};
}

// ---- label placement ----

enum class Mode { Center, Endpoint };

std::pair<float, float> sequence_center_gap_profile(float label_height) {
    float h = fmax_(label_height, 1.0f);
    float target = clamp_(h * 0.16f, 2.8f, 4.8f);
    float too_close = clamp_(h * 0.10f, 1.2f, 2.6f);
    return {target, too_close};
}

float sequence_label_penalty(const Rect& rect, Point center, Point anchor, const std::vector<Point>& own_points,
                             float label_height, const std::vector<Rect>& occupied, Mode mode) {
    float overlap_area_sum = 0.0f;
    size_t overlap_count = 0;
    for (const Rect& obstacle : occupied) {
        float area = rect_overlap_area(rect, obstacle);
        if (area > 0.0f) {
            ++overlap_count;
            overlap_area_sum += area;
        }
    }
    float own_gap = polyline_rect_gap(own_points, rect);
    float gap_penalty;
    if (mode == Mode::Center) {
        auto [target_gap, too_close_limit] = sequence_center_gap_profile(label_height);
        if (!std::isfinite(own_gap)) {
            gap_penalty = 150.0f;
        } else if (own_gap <= too_close_limit) {
            gap_penalty = 140.0f + fmax_(too_close_limit - own_gap, 0.0f) * 28.0f;
        } else if (own_gap < SEQUENCE_CENTER_LABEL_GAP_MIN) {
            float delta = (SEQUENCE_CENTER_LABEL_GAP_MIN - own_gap) / SEQUENCE_CENTER_LABEL_GAP_MIN;
            gap_penalty = delta * delta * 22.0f;
        } else if (own_gap <= SEQUENCE_CENTER_LABEL_GAP_MAX) {
            float delta = (own_gap - target_gap) / fmax_(target_gap, 1e-3f);
            gap_penalty = delta * delta * 0.85f;
        } else {
            float far = own_gap - SEQUENCE_CENTER_LABEL_GAP_MAX;
            float penalty = far * far * 1.7f + far * 0.35f;
            if (own_gap > SEQUENCE_CENTER_LABEL_FAR_GAP) penalty += (own_gap - SEQUENCE_CENTER_LABEL_FAR_GAP) * 0.7f;
            gap_penalty = penalty;
        }
    } else {
        float penalty = 0.0f;
        if (own_gap <= SEQUENCE_LABEL_TOUCH_EPS) {
            penalty += 120.0f + fmax_(SEQUENCE_LABEL_TOUCH_EPS - own_gap, 0.0f) * 30.0f;
        } else if (own_gap < SEQUENCE_ENDPOINT_LABEL_GAP_MIN) {
            float delta = (SEQUENCE_ENDPOINT_LABEL_GAP_MIN - own_gap) / fmax_(SEQUENCE_ENDPOINT_LABEL_GAP_MIN, 1e-3f);
            penalty += delta * delta * 14.0f;
        } else if (own_gap <= SEQUENCE_ENDPOINT_LABEL_GAP_MAX) {
            float delta = (own_gap - SEQUENCE_ENDPOINT_LABEL_GAP_TARGET) / fmax_(SEQUENCE_ENDPOINT_LABEL_GAP_TARGET, 1e-3f);
            penalty += delta * delta * 0.9f;
        } else {
            float far = own_gap - SEQUENCE_ENDPOINT_LABEL_GAP_MAX;
            penalty += far * far * 2.4f + far * 0.4f;
            if (own_gap > SEQUENCE_ENDPOINT_LABEL_FAR_GAP) penalty += (own_gap - SEQUENCE_ENDPOINT_LABEL_FAR_GAP) * 0.9f;
        }
        gap_penalty = penalty;
    }
    float anchor_weight = mode == Mode::Center ? 0.018f : 0.025f;
    float overlap_penalty =
        overlap_count == 0 ? 0.0f : 10000.0f + static_cast<float>(overlap_count) * 1000.0f + overlap_area_sum * 4.0f;
    return overlap_penalty + gap_penalty + distance(center, anchor) * anchor_weight;
}

float sequence_edge_overlap_penalty(const Rect& rect, const std::vector<std::vector<Point>>& edge_paths, size_t edge_idx) {
    size_t hits = 0;
    for (size_t idx = 0; idx < edge_paths.size(); ++idx) {
        const auto& points = edge_paths[idx];
        if (idx == edge_idx || points.size() < 2) continue;
        bool any = false;
        for (size_t i = 0; i + 1 < points.size(); ++i)
            if (segment_intersects_rect(points[i], points[i + 1], rect)) { any = true; break; }
        if (any) ++hits;
    }
    if (hits == 0) return 0.0f;
    return 1.0f + static_cast<float>(hits) * 4.0f;
}

// (point, direction) at half the path length
std::pair<Point, Point> edge_midpoint_with_direction(const std::vector<Point>& points) {
    if (points.size() < 2) {
        Point point = points.empty() ? Point{0.0f, 0.0f} : points[0];
        return {point, {1.0f, 0.0f}};
    }
    std::vector<float> lengths;
    float total = 0.0f;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        float len = distance(points[i], points[i + 1]);
        lengths.push_back(len);
        total += len;
    }
    if (total <= kF32Epsilon) {
        float dx = points[1].first - points[0].first, dy = points[1].second - points[0].second;
        float len = fmax_(std::sqrt(dx * dx + dy * dy), 1e-6f);
        return {points[0], {dx / len, dy / len}};
    }
    float target = total * 0.5f;
    float acc = 0.0f;
    for (size_t idx = 0; idx < lengths.size(); ++idx) {
        float len = lengths[idx];
        if (acc + len >= target) {
            Point a = points[idx], b = points[idx + 1];
            float local_t = clamp_((target - acc) / fmax_(len, 1e-6f), 0.0f, 1.0f);
            Point point{a.first + (b.first - a.first) * local_t, a.second + (b.second - a.second) * local_t};
            float dx = b.first - a.first, dy = b.second - a.second;
            float dlen = fmax_(std::sqrt(dx * dx + dy * dy), 1e-6f);
            return {point, {dx / dlen, dy / dlen}};
        }
        acc += len;
    }
    Point last = points.back(), prev = points[points.size() - 2];
    float dx = last.first - prev.first, dy = last.second - prev.second;
    float len = fmax_(std::sqrt(dx * dx + dy * dy), 1e-6f);
    return {last, {dx / len, dy / len}};
}

Point choose_sequence_center_label_anchor(const std::vector<Point>& points, const TextBlock& label,
                                          const std::vector<Rect>& occupied, const std::vector<std::vector<Point>>& edge_paths,
                                          size_t edge_idx, const Theme& theme) {
    auto [anchor, dir] = edge_midpoint_with_direction(points);
    Point normal{-dir.second, dir.first};
    float normal_step = fmax_(label.height * 0.5f + SEQUENCE_LABEL_PAD_Y, 6.0f);
    float tangent_step = fmax_(label.width + theme.font_size * 0.35f, 10.0f) * 0.24f;
    static const std::vector<float> tangent_offsets_primary = {0.0f, -0.25f, 0.25f, -0.55f, 0.55f, -0.95f, 0.95f, -1.45f, 1.45f, -2.1f,
                                                                2.1f, -2.9f,  2.9f,  -3.8f,  3.8f,  -4.9f,  4.9f,  -6.2f,  6.2f};
    static const std::vector<float> tangent_offsets_wide = {0.0f, -0.35f, 0.35f, -0.75f, 0.75f, -1.3f, 1.3f, -2.0f, 2.0f, -2.9f,
                                                             2.9f, -4.0f,  4.0f,  -5.3f,  5.3f,  -6.8f, 6.8f, -8.4f, 8.4f};
    static const std::vector<float> normal_offsets_optimal = {-1.2f, 1.2f, -1.35f, 1.35f, -1.5f, 1.5f};
    static const std::vector<float> normal_offsets_near_optimal = {-1.05f, 1.05f, -1.8f, 1.8f, -2.25f, 2.25f};
    static const std::vector<float> normal_offsets_fallback = {-0.9f, 0.9f, -2.7f, 2.7f, -3.4f, 3.4f, -4.4f, 4.4f, -5.8f, 5.8f, -7.4f, 7.4f};
    Point best = anchor;
    float best_score = kInf;

    auto evaluate_band = [&](const std::vector<float>& tangent_offsets, const std::vector<float>& normal_offsets) {
        for (float t : tangent_offsets) {
            for (float n : normal_offsets) {
                Point center{anchor.first + dir.first * tangent_step * t + normal.first * normal_step * n,
                             anchor.second + dir.second * tangent_step * t + normal.second * normal_step * n};
                Rect rect = label_rect(center, label, SEQUENCE_LABEL_PAD_X, SEQUENCE_LABEL_PAD_Y);
                float score = sequence_label_penalty(rect, center, anchor, points, label.height, occupied, Mode::Center);
                score += sequence_edge_overlap_penalty(rect, edge_paths, edge_idx);
                float own_dist = point_to_polyline_distance(center, points);
                score += own_dist * 0.045f;
                float tangent_abs = std::fabs(t);
                score += tangent_abs * SEQUENCE_CENTER_LABEL_TANGENT_LINEAR_WEIGHT;
                score += tangent_abs * tangent_abs * SEQUENCE_CENTER_LABEL_TANGENT_QUAD_WEIGHT;
                if (tangent_abs > SEQUENCE_CENTER_LABEL_TANGENT_SOFT_LIMIT)
                    score += (tangent_abs - SEQUENCE_CENTER_LABEL_TANGENT_SOFT_LIMIT) * SEQUENCE_CENTER_LABEL_TANGENT_FAR_WEIGHT;
                if (std::fabs(dir.first) > std::fabs(dir.second) && center.second > anchor.second) score += 0.3f;
                if (score < best_score) {
                    best_score = score;
                    best = center;
                }
            }
        }
    };
    evaluate_band(tangent_offsets_primary, normal_offsets_optimal);
    evaluate_band(tangent_offsets_primary, normal_offsets_near_optimal);
    evaluate_band(tangent_offsets_wide, normal_offsets_fallback);
    return best;
}

std::optional<std::pair<Point, Point>> sequence_endpoint_base(const std::vector<Point>& points, bool start, const Theme& theme) {
    if (points.size() < 2) return std::nullopt;
    Point p0, p1;
    if (start) { p0 = points[0]; p1 = points[1]; }
    else { p0 = points.back(); p1 = points[points.size() - 2]; }
    float dx = p1.first - p0.first, dy = p1.second - p0.second;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len <= kF32Epsilon) return std::nullopt;
    Point dir{dx / len, dy / len};
    float offset = fmax_(theme.font_size * 0.45f, 6.0f);
    Point anchor{p0.first + dir.first * offset * 1.4f, p0.second + dir.second * offset * 1.4f};
    return std::make_pair(anchor, dir);
}

std::optional<Point> choose_sequence_endpoint_label_anchor(const std::vector<Point>& points, const TextBlock& label, bool start,
                                                           const std::vector<Rect>& occupied,
                                                           const std::vector<std::vector<Point>>& edge_paths, size_t edge_idx,
                                                           const Theme& theme) {
    auto base = sequence_endpoint_base(points, start, theme);
    if (!base) return std::nullopt;
    auto [anchor, dir] = *base;
    Point normal{-dir.second, dir.first};
    float base_step = fmax_(theme.font_size * 0.45f, 6.0f);
    static const float tangent_offsets[] = {0.0f, 0.6f, -0.6f, 1.2f, -1.2f, 2.0f, -2.0f, 2.9f, -2.9f};
    static const float normal_offsets[] = {0.35f, -0.35f, 0.75f, -0.75f, 1.1f, -1.1f, 1.45f, -1.45f, 1.8f, -1.8f};
    Point best = anchor;
    float best_score = kInf;
    for (float t : tangent_offsets)
        for (float n : normal_offsets) {
            Point center{anchor.first + dir.first * base_step * t + normal.first * base_step * n,
                         anchor.second + dir.second * base_step * t + normal.second * base_step * n};
            Rect rect = label_rect(center, label, SEQUENCE_ENDPOINT_LABEL_PAD_X, SEQUENCE_ENDPOINT_LABEL_PAD_Y);
            float score = sequence_label_penalty(rect, center, anchor, points, label.height, occupied, Mode::Endpoint);
            score += sequence_edge_overlap_penalty(rect, edge_paths, edge_idx);
            score += distance(center, anchor) * 0.05f;
            if (score < best_score) {
                best_score = score;
                best = center;
            }
        }
    return best;
}

void place_sequence_label_anchors(std::vector<EdgeLayout>& edges, const std::map<std::string, NodeLayout>& nodes,
                                  const SequenceData& seq, const Theme& theme) {
    if (edges.empty()) return;
    std::vector<Rect> occupied;
    for (const auto& [id, node] : nodes) occupied.push_back({node.x, node.y, node.width, node.height});
    for (const Lifeline& l : seq.lifelines) occupied.push_back({l.x - 1.5f, l.y1, 3.0f, fmax_(l.y2 - l.y1, 0.0f)});
    for (const NodeLayout& f : seq.footboxes) occupied.push_back({f.x, f.y, f.width, f.height});
    for (const SequenceFrameLayout& frame : seq.frames) {
        occupied.push_back(frame.label_box);
        float line_pad = 1.5f;
        occupied.push_back({frame.x - line_pad, frame.y - line_pad, frame.width + line_pad * 2.0f, line_pad * 2.0f});
        occupied.push_back({frame.x - line_pad, frame.y + frame.height - line_pad, frame.width + line_pad * 2.0f, line_pad * 2.0f});
        occupied.push_back({frame.x - line_pad, frame.y - line_pad, line_pad * 2.0f, frame.height + line_pad * 2.0f});
        occupied.push_back({frame.x + frame.width - line_pad, frame.y - line_pad, line_pad * 2.0f, frame.height + line_pad * 2.0f});
        float section_pad_x = fmax_(theme.font_size * 0.18f, 1.5f);
        float section_pad_y = fmax_(theme.font_size * 0.15f, 1.2f);
        for (const SequenceLabel& label : frame.section_labels)
            occupied.push_back({label.x - label.text.width / 2.0f - section_pad_x, label.y - label.text.height / 2.0f - section_pad_y,
                                label.text.width + section_pad_x * 2.0f, label.text.height + section_pad_y * 2.0f});
        for (float divider : frame.dividers) occupied.push_back({frame.x, divider - line_pad, frame.width, line_pad * 2.0f});
    }
    for (const SequenceNoteLayout& note : seq.notes) occupied.push_back({note.x, note.y, note.width, note.height});
    for (const SequenceActivationLayout& a : seq.activations) occupied.push_back({a.x, a.y, a.width, a.height});
    float number_r = fmax_(theme.font_size * 0.45f, 6.0f);
    for (const SequenceNumberLayout& n : seq.numbers)
        occupied.push_back({n.x - number_r, n.y - number_r, number_r * 2.0f, number_r * 2.0f});

    std::vector<std::vector<Point>> edge_paths;
    for (const EdgeLayout& e : edges) edge_paths.push_back(e.points);
    for (size_t idx = 0; idx < edges.size(); ++idx) {
        if (edges[idx].label) {
            TextBlock label = *edges[idx].label;
            Point anchor = choose_sequence_center_label_anchor(edge_paths[idx], label, occupied, edge_paths, idx, theme);
            edges[idx].label_anchor = anchor;
            occupied.push_back(label_rect(anchor, label, SEQUENCE_LABEL_PAD_X, SEQUENCE_LABEL_PAD_Y));
        }
        if (edges[idx].start_label) {
            TextBlock label = *edges[idx].start_label;
            auto anchor = choose_sequence_endpoint_label_anchor(edge_paths[idx], label, true, occupied, edge_paths, idx, theme);
            edges[idx].start_label_anchor = anchor;
            if (anchor) occupied.push_back(label_rect(*anchor, label, SEQUENCE_ENDPOINT_LABEL_PAD_X, SEQUENCE_ENDPOINT_LABEL_PAD_Y));
        }
        if (edges[idx].end_label) {
            TextBlock label = *edges[idx].end_label;
            auto anchor = choose_sequence_endpoint_label_anchor(edge_paths[idx], label, false, occupied, edge_paths, idx, theme);
            edges[idx].end_label_anchor = anchor;
            if (anchor) occupied.push_back(label_rect(*anchor, label, SEQUENCE_ENDPOINT_LABEL_PAD_X, SEQUENCE_ENDPOINT_LABEL_PAD_Y));
        }
    }
}

void extend_bounds(float& min_x, float& min_y, float& max_x, float& max_y, float x, float y, float w, float h) {
    min_x = fmin_(min_x, x);
    min_y = fmin_(min_y, y);
    max_x = fmax_(max_x, x + w);
    max_y = fmax_(max_y, y + h);
}

}  // namespace

void resolve_sequence_label_positions(Layout& layout, const Theme& theme) {
    if (!layout.sequence) return;
    place_sequence_label_anchors(layout.edges, layout.nodes, *layout.sequence, theme);
}

void finalize_sequence_layout_bounds(Layout& layout) {
    if (!layout.sequence) return;
    SequenceData& seq = *layout.sequence;
    float min_x = kInf, min_y = kInf, max_x = -kInf, max_y = -kInf;

    for (const auto& [id, node] : layout.nodes) extend_bounds(min_x, min_y, max_x, max_y, node.x, node.y, node.width, node.height);
    for (const NodeLayout& f : seq.footboxes) extend_bounds(min_x, min_y, max_x, max_y, f.x, f.y, f.width, f.height);
    for (const auto& b : seq.boxes) extend_bounds(min_x, min_y, max_x, max_y, b.x, b.y, b.width, b.height);
    for (const auto& f : seq.frames) extend_bounds(min_x, min_y, max_x, max_y, f.x, f.y, f.width, f.height);
    for (const auto& n : seq.notes) extend_bounds(min_x, min_y, max_x, max_y, n.x, n.y, n.width, n.height);
    for (const auto& a : seq.activations) extend_bounds(min_x, min_y, max_x, max_y, a.x, a.y, a.width, a.height);
    for (const auto& n : seq.numbers) extend_bounds(min_x, min_y, max_x, max_y, n.x, n.y, 0.0f, 0.0f);
    for (const EdgeLayout& edge : layout.edges) {
        for (const Point& p : edge.points) extend_bounds(min_x, min_y, max_x, max_y, p.first, p.second, 0.0f, 0.0f);
        if (edge.label && edge.label_anchor) {
            auto [x, y] = *edge.label_anchor;
            extend_bounds(min_x, min_y, max_x, max_y, x - edge.label->width / 2.0f - SEQUENCE_LABEL_PAD_X,
                          y - edge.label->height / 2.0f - SEQUENCE_LABEL_PAD_Y, edge.label->width + 2.0f * SEQUENCE_LABEL_PAD_X,
                          edge.label->height + 2.0f * SEQUENCE_LABEL_PAD_Y);
        }
        for (int which = 0; which < 2; ++which) {
            const auto& label = which == 0 ? edge.start_label : edge.end_label;
            const auto& anchor = which == 0 ? edge.start_label_anchor : edge.end_label_anchor;
            if (!label || !anchor) continue;
            extend_bounds(min_x, min_y, max_x, max_y, anchor->first - label->width / 2.0f - SEQUENCE_ENDPOINT_LABEL_PAD_X,
                          anchor->second - label->height / 2.0f - SEQUENCE_ENDPOINT_LABEL_PAD_Y,
                          label->width + 2.0f * SEQUENCE_ENDPOINT_LABEL_PAD_X, label->height + 2.0f * SEQUENCE_ENDPOINT_LABEL_PAD_Y);
        }
    }

    if (!std::isfinite(min_x) || !std::isfinite(min_y) || !std::isfinite(max_x) || !std::isfinite(max_y)) {
        layout.width = 1.0f;
        layout.height = 1.0f;
        return;
    }

    float content_margin = 0.0f;
    float shift_x = content_margin - min_x;
    float shift_y = content_margin - min_y;
    if (std::fabs(shift_x) > 1e-3f || std::fabs(shift_y) > 1e-3f) {
        for (auto& [id, node] : layout.nodes) { node.x += shift_x; node.y += shift_y; }
        for (EdgeLayout& edge : layout.edges) {
            for (Point& p : edge.points) { p.first += shift_x; p.second += shift_y; }
            for (auto* a : {&edge.label_anchor, &edge.start_label_anchor, &edge.end_label_anchor})
                if (*a) *a = Point{(*a)->first + shift_x, (*a)->second + shift_y};
        }
        for (Lifeline& l : seq.lifelines) { l.x += shift_x; l.y1 += shift_y; l.y2 += shift_y; }
        for (NodeLayout& f : seq.footboxes) { f.x += shift_x; f.y += shift_y; }
        for (auto& b : seq.boxes) { b.x += shift_x; b.y += shift_y; }
        for (auto& f : seq.frames) {
            f.x += shift_x;
            f.y += shift_y;
            f.label_box[0] += shift_x;
            f.label_box[1] += shift_y;
            f.label.x += shift_x;
            f.label.y += shift_y;
            for (auto& l : f.section_labels) { l.x += shift_x; l.y += shift_y; }
            for (float& d : f.dividers) d += shift_y;
        }
        for (auto& n : seq.notes) { n.x += shift_x; n.y += shift_y; }
        for (auto& a : seq.activations) { a.x += shift_x; a.y += shift_y; }
        for (auto& n : seq.numbers) { n.x += shift_x; n.y += shift_y; }
        min_x += shift_x;
        min_y += shift_y;
        max_x += shift_x;
        max_y += shift_y;
    }
    layout.width = fmax_(max_x - min_x + content_margin * 2.0f, 1.0f);
    layout.height = fmax_(max_y - min_y + content_margin * 2.0f, 1.0f);
}

Layout compute_sequence_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    std::map<std::string, NodeLayout> nodes;
    std::vector<EdgeLayout> edges;

    std::vector<std::string> participants = graph.sequence_participants;
    for (const auto& [id, node] : graph.nodes)
        if (std::find(participants.begin(), participants.end(), id) == participants.end()) participants.push_back(id);

    SequenceGeometry geometry = SequenceGeometry::from_theme(theme);
    std::map<std::string, TextBlock> label_blocks;
    std::map<std::string, float> participant_widths;
    float max_label_height = 0.0f;
    for (const std::string& id : participants) {
        const Node& node = graph.nodes.at(id);
        TextBlock label = measure_sequence_text(node.label, theme, config);
        max_label_height = fmax_(max_label_height, label.height);
        participant_widths[id] = geometry.actor_min_width;
        label_blocks[id] = std::move(label);
    }

    float actor_height = fmax_(max_label_height + geometry.actor_pad_y * 2.0f, geometry.actor_min_height);
    std::vector<float> lane_centers = compute_sequence_lane_centers(participants, participant_widths, graph, theme, config, geometry);

    for (size_t idx = 0; idx < participants.size(); ++idx) {
        const std::string& id = participants[idx];
        const Node& node = graph.nodes.at(id);
        float actor_width = participant_widths.count(id) ? participant_widths.at(id) : geometry.actor_min_width;
        TextBlock label;
        if (auto it = label_blocks.find(id); it != label_blocks.end()) label = it->second;
        else label.lines = {id};
        float center_x = idx < lane_centers.size() ? lane_centers[idx] : actor_width * 0.5f;
        NodeLayout nl;
        nl.id = id;
        nl.x = center_x - actor_width / 2.0f;
        nl.y = 0.0f;
        nl.width = actor_width;
        nl.height = actor_height;
        nl.label = std::move(label);
        nl.shape = node.shape;
        nl.style = resolve_node_style_of(id, graph);
        if (auto link = graph.node_links.find(id); link != graph.node_links.end()) nl.link = link->second;
        nodes[id] = std::move(nl);
    }

    float base_spacing = fmax_(geometry.message_step, 18.0f);
    std::vector<float> message_row_spacing;
    for (const Edge& edge : graph.edges) {
        float row_h = 0.0f;
        if (edge.label) row_h = fmax_(row_h, measure_sequence_text(*edge.label, theme, config).height);
        if (edge.start_label) row_h = fmax_(row_h, measure_sequence_text(*edge.start_label, theme, config).height);
        if (edge.end_label) row_h = fmax_(row_h, measure_sequence_text(*edge.end_label, theme, config).height);
        message_row_spacing.push_back(fmax_(base_spacing, row_h + theme.font_size * 1.25f));
    }

    std::vector<float> extra_before(graph.edges.size(), 0.0f);
    float frame_end_pad = base_spacing * 0.25f;
    for (const SequenceFrame& frame : graph.sequence_frames) {
        if (frame.start_idx < extra_before.size()) extra_before[frame.start_idx] += base_spacing;
        for (size_t s = 1; s < frame.sections.size(); ++s)
            if (frame.sections[s].start_idx < extra_before.size()) extra_before[frame.sections[s].start_idx] += base_spacing;
        if (frame.end_idx < extra_before.size()) extra_before[frame.end_idx] += frame_end_pad;
    }

    std::vector<std::vector<const SequenceNote*>> notes_by_index(graph.edges.size() + 1);
    for (const SequenceNote& note : graph.sequence_notes) notes_by_index[std::min(note.index, graph.edges.size())].push_back(&note);

    float message_cursor = actor_height;
    std::vector<float> message_ys;
    std::vector<SequenceNoteLayout> sequence_notes;
    for (size_t idx = 0; idx <= graph.edges.size(); ++idx) {
        for (const SequenceNote* note : notes_by_index[idx]) {
            message_cursor += geometry.note_gap_y;
            TextBlock label = measure_sequence_text(note->label, theme, config);
            float width = fmax_(label.width + geometry.note_padding_x * 2.0f, geometry.actor_min_width);
            float height = label.height + geometry.note_padding_y * 2.0f;
            std::vector<float> lifeline_xs;
            for (const auto& id : note->participants)
                if (auto it = nodes.find(id); it != nodes.end()) lifeline_xs.push_back(sequence_lane_center(it->second));
            if (lifeline_xs.empty()) lifeline_xs.push_back(0.0f);
            float base_x = lifeline_xs[0];
            float min_x = kInf, max_x = -kInf;
            for (float v : lifeline_xs) { min_x = fmin_(min_x, v); max_x = fmax_(max_x, v); }
            if (note->position == SequenceNotePosition::Over && note->participants.size() > 1) {
                float span = std::fabs(max_x - min_x);
                width = fmax_(width, span + geometry.note_gap_x * 2.0f);
            }
            float x;
            switch (note->position) {
                case SequenceNotePosition::LeftOf: x = base_x - geometry.note_gap_x - width; break;
                case SequenceNotePosition::RightOf: x = base_x + geometry.note_gap_x; break;
                default: x = (min_x + max_x) / 2.0f - width / 2.0f; break;
            }
            SequenceNoteLayout nl;
            nl.x = x;
            nl.y = message_cursor;
            nl.width = width;
            nl.height = height;
            nl.label = std::move(label);
            nl.position = note->position;
            nl.participants = note->participants;
            nl.index = note->index;
            sequence_notes.push_back(std::move(nl));
            message_cursor += height;
        }
        if (idx < graph.edges.size()) {
            message_cursor += extra_before[idx] + message_row_spacing[idx];
            message_ys.push_back(message_cursor);
        }
    }

    for (size_t idx = 0; idx < graph.edges.size(); ++idx) {
        const Edge& edge = graph.edges[idx];
        const NodeLayout& from = nodes.at(edge.from);
        const NodeLayout& to = nodes.at(edge.to);
        float y = idx < message_ys.size() ? message_ys[idx] : message_cursor;
        EdgeLayout el;
        el.from = edge.from;
        el.to = edge.to;
        if (edge.label) el.label = measure_sequence_text(*edge.label, theme, config);
        if (edge.start_label) el.start_label = measure_sequence_text(*edge.start_label, theme, config);
        if (edge.end_label) el.end_label = measure_sequence_text(*edge.end_label, theme, config);
        if (edge.from == edge.to) {
            float pad = geometry.note_gap_x * 1.4f;
            float x = sequence_lane_center(from);
            el.points = {{x, y}, {x + pad, y}, {x + pad, y + pad}, {x, y + pad}};
        } else {
            el.points = {{sequence_lane_center(from), y}, {sequence_lane_center(to), y}};
        }
        el.override_style = resolve_edge_style_of(idx, graph);
        if (edge.style == EdgeStyle::Dotted && !el.override_style.dasharray) el.override_style.dasharray = "3 3";
        el.directed = edge.directed;
        el.arrow_start = edge.arrow_start;
        el.arrow_end = edge.arrow_end;
        el.arrow_start_kind = edge.arrow_start_kind;
        el.arrow_end_kind = edge.arrow_end_kind;
        el.start_decoration = edge.start_decoration;
        el.end_decoration = edge.end_decoration;
        el.style = edge.style;
        edges.push_back(std::move(el));
    }

    std::vector<SequenceFrameLayout> sequence_frames;
    if (!graph.sequence_frames.empty() && !message_ys.empty()) {
        std::vector<SequenceFrame> frames = graph.sequence_frames;
        std::stable_sort(frames.begin(), frames.end(), [](const SequenceFrame& a, const SequenceFrame& b) {
            if (a.start_idx != b.start_idx) return a.start_idx < b.start_idx;
            return b.end_idx < a.end_idx;
        });
        for (const SequenceFrame& frame : frames) {
            if (frame.start_idx >= frame.end_idx || frame.start_idx >= message_ys.size()) continue;
            float min_center_x = kInf, max_center_x = -kInf;
            size_t count = frame.end_idx > frame.start_idx ? frame.end_idx - frame.start_idx : 0;
            for (size_t i = frame.start_idx; i < graph.edges.size() && i < frame.start_idx + count; ++i) {
                const Edge& edge = graph.edges[i];
                if (auto it = nodes.find(edge.from); it != nodes.end()) {
                    float c = sequence_lane_center(it->second);
                    min_center_x = fmin_(min_center_x, c);
                    max_center_x = fmax_(max_center_x, c);
                }
                if (auto it = nodes.find(edge.to); it != nodes.end()) {
                    float c = sequence_lane_center(it->second);
                    min_center_x = fmin_(min_center_x, c);
                    max_center_x = fmax_(max_center_x, c);
                }
            }
            if (!std::isfinite(min_center_x) || !std::isfinite(max_center_x))
                for (const auto& [id, node] : nodes) {
                    float c = sequence_lane_center(node);
                    min_center_x = fmin_(min_center_x, c);
                    max_center_x = fmax_(max_center_x, c);
                }
            if (!std::isfinite(min_center_x) || !std::isfinite(max_center_x)) continue;

            float frame_pad_x = fmax_(theme.font_size * 0.7f, 11.0f);
            float frame_x = min_center_x - frame_pad_x;
            float frame_width = (max_center_x - min_center_x) + frame_pad_x + theme.font_size * 1.05f;

            float first_y = frame.start_idx < message_ys.size() ? message_ys[frame.start_idx] : message_cursor;
            size_t last_i = frame.end_idx > 0 ? frame.end_idx - 1 : 0;
            float last_y = last_i < message_ys.size() ? message_ys[last_i] : first_y;
            float min_y = first_y, max_y = last_y;
            for (const auto& note : sequence_notes)
                if (note.index >= frame.start_idx && note.index <= frame.end_idx) {
                    min_y = fmin_(min_y, note.y);
                    max_y = fmax_(max_y, note.y + note.height);
                }
            float top_offset = fmax_(base_spacing * 1.8f, theme.font_size * 3.9f);
            float bottom_offset = fmax_(theme.font_size * 0.85f, 12.0f);
            float frame_y = min_y - top_offset;
            float frame_height = fmax_(max_y - min_y, 0.0f) + top_offset + bottom_offset;

            const char* frame_label_text = "alt";
            switch (frame.kind) {
                case SequenceFrameKind::Alt: frame_label_text = "alt"; break;
                case SequenceFrameKind::Opt: frame_label_text = "opt"; break;
                case SequenceFrameKind::Loop: frame_label_text = "loop"; break;
                case SequenceFrameKind::Par: frame_label_text = "par"; break;
                case SequenceFrameKind::Rect: frame_label_text = "rect"; break;
                case SequenceFrameKind::Critical: frame_label_text = "critical"; break;
                case SequenceFrameKind::Break: frame_label_text = "break"; break;
            }
            TextBlock label_block = measure_sequence_text(frame_label_text, theme, config);
            float label_box_w = fmax_(label_block.width + theme.font_size * 1.2f, theme.font_size * 3.1f);
            float label_box_h = fmax_(theme.font_size * 1.25f, 20.0f);
            float label_box_x = frame_x, label_box_y = frame_y;
            SequenceFrameLayout fl;
            fl.kind = frame.kind;
            fl.x = frame_x;
            fl.y = frame_y;
            fl.width = frame_width;
            fl.height = frame_height;
            fl.label_box = {label_box_x, label_box_y, label_box_w, label_box_h};
            fl.label = {label_box_x + label_box_w / 2.0f, label_box_y + label_box_h / 2.0f, label_block};

            float divider_offset = theme.font_size * 0.9f;
            for (size_t s = 0; s + 1 < frame.sections.size(); ++s) {
                size_t prev_end = frame.sections[s].end_idx;
                size_t pi = prev_end > 0 ? prev_end - 1 : 0;
                float base_y = pi < message_ys.size() ? message_ys[pi] : first_y;
                fl.dividers.push_back(base_y + divider_offset);
            }

            float label_offset = theme.font_size * 0.7f;
            for (size_t section_idx = 0; section_idx < frame.sections.size(); ++section_idx) {
                const auto& section = frame.sections[section_idx];
                if (!section.label) continue;
                std::string display = "[" + *section.label + "]";
                TextBlock block = measure_sequence_text(display, theme, config);
                float label_y;
                if (section_idx == 0) {
                    label_y = frame_y + label_box_h - theme.font_size * 0.15f;
                } else {
                    float d = section_idx - 1 < fl.dividers.size() ? fl.dividers[section_idx - 1] : frame_y + label_offset;
                    label_y = d + theme.font_size * 0.9f;
                }
                float side_pad = theme.font_size * 0.45f;
                float frame_center_x = frame_x + frame_width / 2.0f;
                auto clamp_or_midpoint = [](float preferred, float lo, float hi) {
                    if (lo <= hi) return clamp_(preferred, lo, hi);
                    return (lo + hi) / 2.0f;
                };
                float label_x;
                if (section_idx == 0) {
                    float preferred = frame_x + label_box_w + theme.font_size * 3.0f + block.width / 2.0f;
                    float lo = frame_x + block.width / 2.0f + theme.font_size * 0.4f;
                    float hi = frame_x + frame_width - block.width / 2.0f - theme.font_size * 0.4f;
                    label_x = clamp_or_midpoint(preferred, lo, hi);
                } else {
                    float preferred = frame_center_x;
                    float lo = frame_x + block.width / 2.0f + side_pad;
                    float hi = frame_x + frame_width - block.width / 2.0f - side_pad;
                    label_x = clamp_or_midpoint(preferred, lo, hi);
                }
                fl.section_labels.push_back({label_x, label_y, block});
            }
            sequence_frames.push_back(std::move(fl));
        }
    }

    float lifeline_start = actor_height;
    float last_message_y = message_ys.empty() ? lifeline_start + base_spacing : message_ys.back();
    for (const auto& note : sequence_notes) last_message_y = fmax_(last_message_y, note.y + note.height);
    float lifeline_end = last_message_y + geometry.footbox_gap;
    std::vector<Lifeline> lifelines;
    std::vector<NodeLayout> sequence_footboxes;
    for (const auto& id : participants) {
        auto it = nodes.find(id);
        if (it == nodes.end()) continue;
        lifelines.push_back({it->second.id, sequence_lane_center(it->second), lifeline_start, lifeline_end});
    }
    for (const auto& id : participants) {
        auto it = nodes.find(id);
        if (it == nodes.end()) continue;
        NodeLayout foot = it->second;
        foot.y = lifeline_end;
        sequence_footboxes.push_back(std::move(foot));
    }

    std::vector<SequenceBoxLayout> sequence_boxes;
    if (!graph.sequence_boxes.empty()) {
        float pad_x = geometry.lane_side_pad_x;
        float pad_y = theme.font_size * 0.6f;
        float bottom = lifeline_end;
        for (const auto& foot : sequence_footboxes) bottom = fmax_(bottom, foot.y + foot.height);
        // (Rust folds with f32::max starting from lifeline_end over the footbox bottoms)
        for (const SequenceBox& seq_box : graph.sequence_boxes) {
            float min_center_x = kInf, max_center_x = -kInf;
            for (const auto& participant : seq_box.participants)
                if (auto it = nodes.find(participant); it != nodes.end()) {
                    float c = sequence_lane_center(it->second);
                    min_center_x = fmin_(min_center_x, c);
                    max_center_x = fmax_(max_center_x, c);
                }
            if (!std::isfinite(min_center_x) || !std::isfinite(max_center_x)) continue;
            SequenceBoxLayout bl;
            bl.x = min_center_x - pad_x;
            bl.y = 0.0f;
            bl.width = (max_center_x - min_center_x) + pad_x * 2.0f;
            bl.height = bottom + pad_y;
            if (seq_box.label) bl.label = measure_sequence_text(*seq_box.label, theme, config);
            bl.color = seq_box.color;
            sequence_boxes.push_back(std::move(bl));
        }
    }

    float activation_width = fmax_(theme.font_size * 0.625f, 10.0f);
    float activation_offset = fmax_(activation_width * 0.6f, 4.0f);
    float activation_end_default =
        (message_ys.empty() ? lifeline_start + base_spacing * 0.5f : message_ys.back()) + base_spacing * 0.6f;
    std::vector<SequenceActivationLayout> sequence_activations;
    std::map<std::string, std::vector<std::pair<float, size_t>>> activation_stacks;
    struct Event {
        size_t index, order;
        SequenceActivation event;
    };
    std::vector<Event> events;
    for (size_t i = 0; i < graph.sequence_activations.size(); ++i)
        events.push_back({graph.sequence_activations[i].index, i, graph.sequence_activations[i]});
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        if (a.index != b.index) return a.index < b.index;
        return a.order < b.order;
    });
    auto activation_y_for = [&](size_t idx) { return idx < message_ys.size() ? message_ys[idx] : activation_end_default; };
    for (const Event& ev : events) {
        float y = activation_y_for(ev.event.index);
        auto& stack = activation_stacks[ev.event.participant];
        if (ev.event.kind == SequenceActivationKind::Activate) {
            size_t depth = stack.size();
            stack.push_back({y, depth});
        } else if (!stack.empty()) {
            auto [start_y, depth] = stack.back();
            stack.pop_back();
            auto nit = nodes.find(ev.event.participant);
            if (nit != nodes.end()) {
                float base_x = sequence_lane_center(nit->second) - activation_width / 2.0f;
                float x = base_x + static_cast<float>(depth) * activation_offset;
                float y0 = fmin_(start_y, y);
                float height = std::fabs(y - start_y);
                if (height < base_spacing * 0.6f) height = base_spacing * 0.6f;
                if (y0 < lifeline_start) y0 = lifeline_start;
                sequence_activations.push_back({x, y0, activation_width, height, ev.event.participant, depth});
            }
        }
    }
    // Activations that were never deactivated run to the end. (HashMap iteration order in Rust:
    // the order of the resulting rectangles only affects drawing order of non-overlapping boxes.)
    for (auto& [participant, stack] : activation_stacks)
        for (auto& [start_y, depth] : stack) {
            auto nit = nodes.find(participant);
            if (nit == nodes.end()) continue;
            float base_x = sequence_lane_center(nit->second) - activation_width / 2.0f;
            float x = base_x + static_cast<float>(depth) * activation_offset;
            float y0 = fmin_(start_y, activation_end_default);
            float height = std::fabs(activation_end_default - start_y);
            if (height < base_spacing * 0.6f) height = base_spacing * 0.6f;
            if (y0 < lifeline_start) y0 = lifeline_start;
            sequence_activations.push_back({x, y0, activation_width, height, participant, depth});
        }

    std::vector<SequenceNumberLayout> sequence_numbers;
    if (graph.sequence_autonumber) {
        size_t value = *graph.sequence_autonumber;
        for (size_t idx = 0; idx < graph.edges.size(); ++idx) {
            const Edge& edge = graph.edges[idx];
            auto fit = nodes.find(edge.from);
            if (fit == nodes.end() || idx >= message_ys.size()) continue;
            float y = message_ys[idx];
            float from_x = sequence_lane_center(fit->second);
            auto tit = nodes.find(edge.to);
            float to_x = tit != nodes.end() ? sequence_lane_center(tit->second) : from_x;
            float offset = to_x >= from_x ? 16.0f : -16.0f;
            float number_y = y - fmax_(theme.font_size * 0.85f, 10.0f);
            sequence_numbers.push_back({from_x + offset, number_y, value});
            ++value;
        }
    }

    Layout layout;
    layout.kind = graph.kind;
    layout.nodes = std::move(nodes);
    layout.edges = std::move(edges);
    layout.width = 1.0f;
    layout.height = 1.0f;
    SequenceData data;
    data.lifelines = std::move(lifelines);
    data.footboxes = std::move(sequence_footboxes);
    data.boxes = std::move(sequence_boxes);
    data.frames = std::move(sequence_frames);
    data.notes = std::move(sequence_notes);
    data.activations = std::move(sequence_activations);
    data.numbers = std::move(sequence_numbers);
    layout.sequence = std::move(data);
    finalize_sequence_layout_bounds(layout);
    return layout;
}

}  // namespace mermaid
