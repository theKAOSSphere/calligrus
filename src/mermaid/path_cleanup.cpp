// Mermaid port of layout/flowchart/path_cleanup.rs (see path_cleanup.h).
// Subgraph-related detours are omitted: layouts with subgraphs are "Unsupported" for now.
#include "path_cleanup.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace mermaid {

namespace {

using Path = std::vector<Point>;

float fmax_(float a, float b) { return a > b ? a : (a < b ? b : a); }
float fmin_(float a, float b) { return a < b ? a : (a > b ? b : a); }

float flowchart_path_overlap_with_prior(const Path& path, const Paths& prior, size_t prior_count) {
    float overlap = 0.0f;
    for (size_t s = 0; s + 1 < path.size(); ++s) {
        Point a1 = path[s], a2 = path[s + 1];
        for (size_t p = 0; p < prior_count; ++p) {
            const Path& other = prior[p];
            for (size_t o = 0; o + 1 < other.size(); ++o) overlap += collinear_overlap_length(a1, a2, other[o], other[o + 1]);
        }
    }
    return overlap;
}

void append_path_segments(const Path& path, std::vector<Segment>& segments) {
    if (path.size() < 2) return;
    for (size_t i = 0; i + 1 < path.size(); ++i) segments.push_back({path[i], path[i + 1]});
}

std::vector<Path> perimeter_route_candidates(Point start, Point end, float outer_left, float outer_right, float outer_top,
                                             float outer_bottom) {
    return {
        {start, {outer_right, start.second}, {outer_right, outer_bottom}, {outer_left, outer_bottom}, {outer_left, end.second}, end},
        {start, {outer_right, start.second}, {outer_right, outer_top}, {outer_left, outer_top}, {outer_left, end.second}, end},
        {start, {outer_left, start.second}, {outer_left, outer_bottom}, {outer_right, outer_bottom}, {outer_right, end.second}, end},
        {start, {outer_left, start.second}, {outer_left, outer_top}, {outer_right, outer_top}, {outer_right, end.second}, end},
    };
}

Obstacle node_rect_obstacle(const NodeLayout& node) { return {node.id, node.x, node.y, node.width, node.height, nullptr}; }

bool is_skipped(const NodeLayout& node, const std::string& from_id, const std::string& to_id) {
    return node.id == from_id || node.id == to_id || node.hidden || node.anchor_subgraph.has_value();
}

size_t flowchart_path_non_endpoint_hit_count(const Path& path, const std::string& from_id, const std::string& to_id,
                                             const std::map<std::string, NodeLayout>& nodes) {
    std::set<std::string> hit_ids;
    for (size_t s = 0; s + 1 < path.size(); ++s)
        for (const auto& [id, node] : nodes) {
            if (is_skipped(node, from_id, to_id)) continue;
            if (segment_intersects_rect(path[s], path[s + 1], node_rect_obstacle(node))) hit_ids.insert(node.id);
        }
    return hit_ids.size();
}

void merge_obstacle(Obstacle& target, const Obstacle& other) {
    float min_x = fmin_(target.x, other.x);
    float min_y = fmin_(target.y, other.y);
    float max_x = fmax_(target.x + target.width, other.x + other.width);
    float max_y = fmax_(target.y + target.height, other.y + other.height);
    target.id += "+";
    target.id += other.id;
    target.x = min_x;
    target.y = min_y;
    target.width = max_x - min_x;
    target.height = max_y - min_y;
}

struct NodeHit {
    size_t first, last;
    Obstacle obstacle;
};

std::optional<NodeHit> first_non_endpoint_node_hit(const Path& path, const std::string& from_id, const std::string& to_id,
                                                   const std::map<std::string, NodeLayout>& nodes) {
    for (size_t seg_idx = 0; seg_idx + 1 < path.size(); ++seg_idx) {
        Point a = path[seg_idx], b = path[seg_idx + 1];
        for (const auto& [id, node] : nodes) {
            if (is_skipped(node, from_id, to_id)) continue;
            Obstacle obstacle = node_rect_obstacle(node);
            if (!segment_intersects_rect(a, b, obstacle)) continue;
            Obstacle merged = obstacle;
            size_t last_idx = seg_idx;
            for (size_t later_idx = seg_idx; later_idx + 1 < path.size(); ++later_idx) {
                Point la = path[later_idx], lb = path[later_idx + 1];
                for (const auto& [oid, other] : nodes) {
                    if (is_skipped(other, from_id, to_id)) continue;
                    Obstacle other_obstacle = node_rect_obstacle(other);
                    if (segment_intersects_rect(la, lb, other_obstacle)) {
                        last_idx = later_idx;
                        merge_obstacle(merged, other_obstacle);
                    }
                }
            }
            return NodeHit{seg_idx, last_idx, merged};
        }
    }
    return std::nullopt;
}

std::vector<Path> node_detour_candidates(const Path& path, size_t first_seg_idx, size_t last_seg_idx, const Obstacle& obstacle,
                                         float clearance) {
    if (first_seg_idx + 1 >= path.size() || last_seg_idx + 1 >= path.size()) return {};
    float left = obstacle.x - clearance, right = obstacle.x + obstacle.width + clearance;
    float top = obstacle.y - clearance, bottom = obstacle.y + obstacle.height + clearance;
    Point entry = path[first_seg_idx];
    Point exit = path[last_seg_idx + 1];
    std::vector<Path> out;
    for (const Path& route : perimeter_route_candidates(entry, exit, left, right, top, bottom)) {
        Path candidate;
        candidate.insert(candidate.end(), path.begin(), path.begin() + static_cast<long>(first_seg_idx) + 1);
        if (route.size() > 2) candidate.insert(candidate.end(), route.begin() + 1, route.end() - 1);
        candidate.insert(candidate.end(), path.begin() + static_cast<long>(last_seg_idx) + 1, path.end());
        out.push_back(compress_path(candidate));
    }
    return out;
}

struct Hull {
    float left, right, top, bottom;
};

std::optional<Hull> non_endpoint_hull(const std::map<std::string, NodeLayout>& nodes, const std::string& from_id,
                                      const std::string& to_id) {
    float left = std::numeric_limits<float>::infinity(), right = -std::numeric_limits<float>::infinity();
    float top = std::numeric_limits<float>::infinity(), bottom = -std::numeric_limits<float>::infinity();
    for (const auto& [id, node] : nodes) {
        if (is_skipped(node, from_id, to_id)) continue;
        left = fmin_(left, node.x);
        right = fmax_(right, node.x + node.width);
        top = fmin_(top, node.y);
        bottom = fmax_(bottom, node.y + node.height);
    }
    if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) || !std::isfinite(bottom)) return std::nullopt;
    return Hull{left, right, top, bottom};
}

std::vector<Path> graph_detour_candidates(const Path& path, size_t first_seg_idx, size_t last_seg_idx,
                                          const std::map<std::string, NodeLayout>& nodes, const std::string& from_id,
                                          const std::string& to_id, float clearance) {
    if (first_seg_idx + 1 >= path.size() || last_seg_idx + 1 >= path.size()) return {};
    auto hull = non_endpoint_hull(nodes, from_id, to_id);
    if (!hull) return {};
    Point entry = path[first_seg_idx];
    Point exit = path[last_seg_idx + 1];
    std::vector<Path> out;
    for (const Path& route : perimeter_route_candidates(entry, exit, hull->left - clearance, hull->right + clearance,
                                                         hull->top - clearance, hull->bottom + clearance)) {
        Path candidate;
        candidate.insert(candidate.end(), path.begin(), path.begin() + static_cast<long>(first_seg_idx) + 1);
        if (route.size() > 2) candidate.insert(candidate.end(), route.begin() + 1, route.end() - 1);
        candidate.insert(candidate.end(), path.begin() + static_cast<long>(last_seg_idx) + 1, path.end());
        out.push_back(compress_path(candidate));
    }
    return out;
}

std::vector<Path> graph_perimeter_detour_candidates(const Path& path, const std::map<std::string, NodeLayout>& nodes,
                                                    const std::string& from_id, const std::string& to_id, float clearance) {
    if (path.size() < 2) return {};
    auto hull = non_endpoint_hull(nodes, from_id, to_id);
    if (!hull) return {};
    Point start = path[0];
    Point end = path.back();
    std::vector<Path> out;
    for (const Path& c : perimeter_route_candidates(start, end, hull->left - clearance, hull->right + clearance,
                                                    hull->top - clearance, hull->bottom + clearance))
        out.push_back(compress_path(c));
    return out;
}

// ---- endpoint re-entry ----

size_t endpoint_reentry_count(const Path& points, const NodeLayout& node, bool is_source) {
    if (points.size() < 3) return 0;
    size_t last_segment_idx = points.size() - 2;
    size_t count = 0;
    for (size_t idx = 0; idx + 1 < points.size(); ++idx) {
        bool allowed = is_source ? idx == 0 : idx == last_segment_idx;
        if (!allowed && segment_hits_node_shape_interior(points[idx], points[idx + 1], node)) ++count;
    }
    return count;
}

std::optional<std::pair<size_t, size_t>> first_endpoint_reentry_span(const Path& points, const NodeLayout& node, bool is_source) {
    if (points.size() < 3) return std::nullopt;
    size_t last_segment_idx = points.size() - 2;
    size_t idx = 0;
    while (idx < last_segment_idx + 1) {
        bool allowed = is_source ? idx == 0 : idx == last_segment_idx;
        if (!allowed && segment_hits_node_shape_interior(points[idx], points[idx + 1], node)) {
            size_t first = idx, last = idx;
            while (last < last_segment_idx) {
                size_t next_idx = last + 1;
                bool next_allowed = is_source ? next_idx == 0 : next_idx == last_segment_idx;
                if (next_allowed || !segment_hits_node_shape_interior(points[next_idx], points[next_idx + 1], node)) break;
                last = next_idx;
            }
            return std::make_pair(first, last);
        }
        ++idx;
    }
    return std::nullopt;
}

std::vector<Path> endpoint_reentry_detour_candidates(const Path& path, size_t first_seg_idx, size_t last_seg_idx,
                                                     const Obstacle& obstacle, float clearance, bool is_source) {
    if (first_seg_idx + 1 >= path.size() || last_seg_idx + 1 >= path.size()) return {};
    size_t start_idx = (first_seg_idx > 0 && (!is_source || first_seg_idx > 1)) ? first_seg_idx - 1 : first_seg_idx;
    size_t end_idx = last_seg_idx + 1;
    if (start_idx >= end_idx || end_idx >= path.size()) return {};
    float left = obstacle.x - clearance, right = obstacle.x + obstacle.width + clearance;
    float top = obstacle.y - clearance, bottom = obstacle.y + obstacle.height + clearance;
    Point entry = path[start_idx];
    Point exit = path[end_idx];
    std::vector<Path> out;
    for (const Path& route : perimeter_route_candidates(entry, exit, left, right, top, bottom)) {
        Path candidate;
        candidate.insert(candidate.end(), path.begin(), path.begin() + static_cast<long>(start_idx) + 1);
        if (route.size() > 2) candidate.insert(candidate.end(), route.begin() + 1, route.end() - 1);
        candidate.insert(candidate.end(), path.begin() + static_cast<long>(end_idx), path.end());
        out.push_back(compress_path(candidate));
    }
    return out;
}

std::optional<Path> repair_endpoint_reentry_once(const Path& points, const Edge& edge,
                                                 const std::map<std::string, NodeLayout>& nodes, const LayoutConfig& config) {
    size_t baseline_reentries = flowchart_endpoint_reentry_count(points, edge, nodes);
    if (baseline_reentries == 0 || flowchart_endpoint_direction_violation_count(points, edge, nodes) > 0) return std::nullopt;
    float baseline_len = path_length(points);
    float clearance = fmax_(config.node_spacing * 0.12f, 8.0f);
    std::optional<Path> best;
    size_t best_reentries = baseline_reentries;
    float best_cost = std::numeric_limits<float>::infinity();

    std::vector<std::pair<const NodeLayout*, bool>> endpoint_specs;
    if (auto it = nodes.find(edge.from); it != nodes.end()) endpoint_specs.push_back({&it->second, true});
    if (edge.to != edge.from)
        if (auto it = nodes.find(edge.to); it != nodes.end()) endpoint_specs.push_back({&it->second, false});

    for (auto [node, is_source] : endpoint_specs) {
        auto span = first_endpoint_reentry_span(points, *node, is_source);
        if (!span) continue;
        auto [first_seg_idx, last_seg_idx] = *span;
        Obstacle obstacle = node_rect_obstacle(*node);
        for (float clearance_scale : {1.0f, 1.5f, 2.0f, 3.0f, 4.0f}) {
            float candidate_clearance = clearance * clearance_scale;
            std::vector<Path> candidates = node_detour_candidates(points, first_seg_idx, last_seg_idx, obstacle, candidate_clearance);
            auto more = endpoint_reentry_detour_candidates(points, first_seg_idx, last_seg_idx, obstacle, candidate_clearance, is_source);
            candidates.insert(candidates.end(), more.begin(), more.end());
            for (const Path& candidate : candidates) {
                size_t violations = flowchart_endpoint_direction_violation_count(candidate, edge, nodes);
                bool hits_non_endpoint = flowchart_path_hits_non_endpoint_nodes(candidate, edge.from, edge.to, nodes);
                size_t reentries = flowchart_endpoint_reentry_count(candidate, edge, nodes);
                if (violations > 0) continue;
                if (hits_non_endpoint) continue;
                if (reentries >= best_reentries) continue;
                float len = path_length(candidate);
                if (len > baseline_len * 4.0f + clearance * 8.0f) continue;
                size_t bends = path_bend_count(candidate);
                float cost = len + static_cast<float>(bends) * clearance + static_cast<float>(reentries) * clearance * 20.0f;
                if (reentries < best_reentries || cost < best_cost) {
                    best_reentries = reentries;
                    best_cost = cost;
                    best = candidate;
                }
            }
        }
    }
    if (best_reentries < baseline_reentries) return best;
    return std::nullopt;
}

// ---- crossing reduction ----

std::optional<Path> bump_orthogonal_segment(const Path& points, size_t seg_idx, float delta) {
    if (seg_idx + 1 >= points.size()) return std::nullopt;
    Point a = points[seg_idx], b = points[seg_idx + 1];
    bool horizontal = std::fabs(a.second - b.second) < 1e-3f;
    bool vertical = std::fabs(a.first - b.first) < 1e-3f;
    if (!horizontal && !vertical) return std::nullopt;
    Path bumped;
    bumped.insert(bumped.end(), points.begin(), points.begin() + static_cast<long>(seg_idx) + 1);
    if (horizontal) {
        float y = a.second + delta;
        bumped.push_back({a.first, y});
        bumped.push_back({b.first, y});
    } else {
        float x = a.first + delta;
        bumped.push_back({x, a.second});
        bumped.push_back({x, b.second});
    }
    bumped.insert(bumped.end(), points.begin() + static_cast<long>(seg_idx) + 1, points.end());
    return compress_path(bumped);
}

struct OuterBox {
    bool use_perimeter_candidates;
    float left, right, top, bottom;
};

bool reduce_crossing_sweep(const std::vector<size_t>& order, const Graph& graph, const std::map<std::string, NodeLayout>& nodes,
                           Paths& routed, const std::vector<float>& deltas, const OuterBox& outer) {
    bool changed = false;
    std::vector<Segment> existing_segments;
    const float MAX_LEN_RATIO_HARD = 2.8f, MAX_LEN_RATIO_NO_GAIN = 1.12f, MAX_LEN_RATIO_ONE_GAIN = 1.8f,
                MAX_LEN_RATIO_MULTI_GAIN = 2.6f;
    for (size_t idx : order) {
        if (routed[idx].size() < 2) {
            append_path_segments(routed[idx], existing_segments);
            continue;
        }
        const std::string& from_id = graph.edges[idx].from;
        const std::string& to_id = graph.edges[idx].to;
        auto [baseline_cross, baseline_overlap] = edge_crossings_with_existing(routed[idx], existing_segments);
        if (baseline_cross == 0) {
            append_path_segments(routed[idx], existing_segments);
            continue;
        }
        size_t best_cross = baseline_cross;
        float best_overlap = baseline_overlap;
        float baseline_len = path_length(routed[idx]);
        float best_len = baseline_len;
        Path best_points = routed[idx];
        size_t segment_count = routed[idx].size() - 1;
        for (size_t seg_idx = 0; seg_idx < segment_count; ++seg_idx)
            for (float delta : deltas) {
                auto candidate = bump_orthogonal_segment(routed[idx], seg_idx, delta);
                if (!candidate) continue;
                if (flowchart_path_hits_non_endpoint_nodes(*candidate, from_id, to_id, nodes)) continue;
                auto [crossings, overlap] = edge_crossings_with_existing(*candidate, existing_segments);
                float len = path_length(*candidate);
                if (len > baseline_len * MAX_LEN_RATIO_HARD) continue;
                if (crossings < best_cross || (crossings == best_cross && overlap + 0.05f < best_overlap) ||
                    (crossings == best_cross && std::fabs(overlap - best_overlap) <= 0.05f && len + 1.0f < best_len)) {
                    best_cross = crossings;
                    best_overlap = overlap;
                    best_len = len;
                    best_points = *candidate;
                }
            }
        // ER / state diagrams also try routes around the whole diagram.
        if (outer.use_perimeter_candidates && !routed[idx].empty()) {
            Point start = routed[idx].front(), end = routed[idx].back();
            for (const Path& raw : perimeter_route_candidates(start, end, outer.left, outer.right, outer.top, outer.bottom)) {
                Path candidate = compress_path(raw);
                if (flowchart_path_hits_non_endpoint_nodes(candidate, from_id, to_id, nodes)) continue;
                auto [crossings, overlap] = edge_crossings_with_existing(candidate, existing_segments);
                float len = path_length(candidate);
                if (len > baseline_len * MAX_LEN_RATIO_HARD) continue;
                size_t crossing_gain = baseline_cross >= crossings ? baseline_cross - crossings : 0;
                float max_ratio = crossing_gain >= 2 ? MAX_LEN_RATIO_MULTI_GAIN : (crossing_gain == 1 ? MAX_LEN_RATIO_ONE_GAIN : MAX_LEN_RATIO_NO_GAIN);
                if (len > baseline_len * max_ratio) continue;
                if (crossings < best_cross || (crossings == best_cross && overlap + 0.05f < best_overlap) ||
                    (crossings == best_cross && std::fabs(overlap - best_overlap) <= 0.05f && len + 1.0f < best_len)) {
                    best_cross = crossings;
                    best_overlap = overlap;
                    best_len = len;
                    best_points = candidate;
                }
            }
        }

        size_t best_gain = baseline_cross >= best_cross ? baseline_cross - best_cross : 0;
        float max_ratio = best_gain >= 2 ? MAX_LEN_RATIO_MULTI_GAIN : (best_gain == 1 ? MAX_LEN_RATIO_ONE_GAIN : MAX_LEN_RATIO_NO_GAIN);
        bool allow_apply = best_len <= baseline_len * max_ratio;
        if (best_cross < baseline_cross || (best_cross == baseline_cross && best_overlap + 0.05f < baseline_overlap)) {
            if (!allow_apply) {
                append_path_segments(routed[idx], existing_segments);
                continue;
            }
            routed[idx] = best_points;
            changed = true;
        }
        append_path_segments(routed[idx], existing_segments);
    }
    return changed;
}

// ---- axis simplification ----

bool is_axis_aligned_segment(Point a, Point b) { return std::fabs(a.first - b.first) <= 1e-3f || std::fabs(a.second - b.second) <= 1e-3f; }

std::optional<Path> collapse_near_axis_aligned_path(const Path& points) {
    if (points.size() < 3) return std::nullopt;
    float min_x = std::numeric_limits<float>::max(), max_x = std::numeric_limits<float>::lowest();
    float min_y = std::numeric_limits<float>::max(), max_y = std::numeric_limits<float>::lowest();
    for (const auto& p : points) {
        min_x = fmin_(min_x, p.first);
        max_x = fmax_(max_x, p.first);
        min_y = fmin_(min_y, p.second);
        max_y = fmax_(max_y, p.second);
    }
    float x_span = max_x - min_x, y_span = max_y - min_y;
    float axis_epsilon = 1.0f;
    bool nearly_vertical = true, nearly_horizontal = true;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        if (!(std::fabs(points[i + 1].first - points[i].first) <= axis_epsilon)) nearly_vertical = false;
        if (!(std::fabs(points[i + 1].second - points[i].second) <= axis_epsilon)) nearly_horizontal = false;
    }
    if (x_span <= axis_epsilon && y_span > axis_epsilon && nearly_vertical) {
        float x = (min_x + max_x) * 0.5f;
        return Path{{x, points[0].second}, {x, points.back().second}};
    }
    if (y_span <= axis_epsilon && x_span > axis_epsilon && nearly_horizontal) {
        float y = (min_y + max_y) * 0.5f;
        return Path{{points[0].first, y}, {points.back().first, y}};
    }
    return std::nullopt;
}

Path collapse_axis_aligned_runs(const Path& points) {
    if (points.size() <= 2) return points;
    Path collapsed;
    size_t idx = 0;
    collapsed.push_back(points[0]);
    while (idx + 1 < points.size()) {
        Point current = points[idx], next = points[idx + 1];
        bool same_x = std::fabs(next.first - current.first) <= 1e-3f;
        bool same_y = std::fabs(next.second - current.second) <= 1e-3f;
        if (!same_x && !same_y) {
            if (std::fabs(next.first - collapsed.back().first) > 1e-3f || std::fabs(next.second - collapsed.back().second) > 1e-3f)
                collapsed.push_back(next);
            ++idx;
            continue;
        }
        size_t end_idx = idx + 1;
        while (end_idx + 1 < points.size()) {
            Point candidate = points[end_idx + 1];
            bool continues_run = same_x ? std::fabs(candidate.first - current.first) <= 1e-3f
                                        : std::fabs(candidate.second - current.second) <= 1e-3f;
            if (!continues_run) break;
            ++end_idx;
        }
        Point terminal = points[end_idx];
        if (std::fabs(terminal.first - collapsed.back().first) > 1e-3f || std::fabs(terminal.second - collapsed.back().second) > 1e-3f)
            collapsed.push_back(terminal);
        idx = end_idx;
    }
    return compress_path(collapsed);
}

// ---- detour-rectangle simplification ----

std::vector<Path> detour_rectangle_simplification_candidates(const Path& points) {
    if (points.size() != 6) return {};
    for (size_t i = 0; i + 1 < points.size(); ++i)
        if (!is_axis_aligned_segment(points[i], points[i + 1])) return {};
    bool vertical_first = std::fabs(points[0].first - points[1].first) <= 1e-3f;
    bool vertical_pattern[5] = {vertical_first, !vertical_first, vertical_first, !vertical_first, vertical_first};
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        bool is_vertical = std::fabs(points[i].first - points[i + 1].first) <= 1e-3f;
        if (is_vertical != vertical_pattern[i]) return {};
    }
    std::vector<Path> candidates;
    if (vertical_first) {
        for (float cross_y : {points[1].second, points[3].second})
            candidates.push_back(compress_path({points[0], {points[0].first, cross_y}, {points[5].first, cross_y}, points[5]}));
    } else {
        for (float cross_x : {points[1].first, points[3].first})
            candidates.push_back(compress_path({points[0], {cross_x, points[0].second}, {cross_x, points[5].second}, points[5]}));
    }
    return candidates;
}

std::vector<Path> shoulder_simplification_candidates(const Path& points) {
    if (points.size() != 6) return {};
    std::vector<Path> candidates;
    std::vector<bool> vp;
    for (size_t i = 0; i + 1 < points.size(); ++i) vp.push_back(std::fabs(points[i].first - points[i + 1].first) <= 1e-3f);
    if (vp == std::vector<bool>{true, false, true, false, true}) {
        candidates.push_back(compress_path({points[0], points[1], points[2], {points[2].first, points[5].second}, points[5]}));
        candidates.push_back(compress_path({points[0], {points[0].first, points[3].second}, points[3], points[4], points[5]}));
    } else if (vp == std::vector<bool>{false, true, false, true, false}) {
        candidates.push_back(compress_path({points[0], points[1], points[2], {points[5].first, points[2].second}, points[5]}));
        candidates.push_back(compress_path({points[0], {points[3].first, points[0].second}, points[3], points[4], points[5]}));
    }
    return candidates;
}

bool point_on_vertical_edge(Point p, const NodeLayout& n) {
    bool on_top = std::fabs(p.second - n.y) <= 3.0f;
    bool on_bottom = std::fabs(p.second - (n.y + n.height)) <= 3.0f;
    return (on_top || on_bottom) && p.first >= n.x - 3.0f && p.first <= n.x + n.width + 3.0f;
}
bool point_on_horizontal_edge(Point p, const NodeLayout& n) {
    bool on_left = std::fabs(p.first - n.x) <= 3.0f;
    bool on_right = std::fabs(p.first - (n.x + n.width)) <= 3.0f;
    return (on_left || on_right) && p.second >= n.y - 3.0f && p.second <= n.y + n.height + 3.0f;
}

std::vector<Path> spine_simplification_candidates(const Path& points, const NodeLayout& from, const NodeLayout& to) {
    if (points.size() < 4) return {};
    Point from_center{from.x + from.width * 0.5f, from.y + from.height * 0.5f};
    Point to_center{to.x + to.width * 0.5f, to.y + to.height * 0.5f};
    bool dominant_vertical = std::fabs(to_center.second - from_center.second) >= std::fabs(to_center.first - from_center.first);
    bool first_on_vertical = point_on_vertical_edge(points[0], from);
    bool last_on_vertical = point_on_vertical_edge(points.back(), to);
    bool first_on_horizontal = point_on_horizontal_edge(points[0], from);
    bool last_on_horizontal = point_on_horizontal_edge(points.back(), to);
    bool first_vertical = first_on_vertical || (!first_on_horizontal && dominant_vertical);
    bool last_vertical = last_on_vertical || (!last_on_horizontal && dominant_vertical);
    bool first_horizontal = first_on_horizontal || (!first_on_vertical && !dominant_vertical);
    bool last_horizontal = last_on_horizontal || (!last_on_vertical && !dominant_vertical);
    std::vector<Path> candidates;
    auto dedup_sorted = [](std::vector<float>& v) {
        std::stable_sort(v.begin(), v.end());
        std::vector<float> out;
        for (float x : v)
            if (out.empty() || !(std::fabs(x - out.back()) <= 1e-3f)) out.push_back(x);
        v = out;
    };
    if (first_vertical && last_vertical) {
        std::vector<float> levels;
        for (size_t i = 1; i + 1 < points.size(); ++i) levels.push_back(points[i].second);
        dedup_sorted(levels);
        for (float cross_y : levels)
            candidates.push_back(compress_path({points[0], {points[0].first, cross_y}, {points.back().first, cross_y}, points.back()}));
    } else if (first_horizontal && last_horizontal) {
        std::vector<float> levels;
        for (size_t i = 1; i + 1 < points.size(); ++i) levels.push_back(points[i].first);
        dedup_sorted(levels);
        for (float cross_x : levels)
            candidates.push_back(compress_path({points[0], {cross_x, points[0].second}, {cross_x, points.back().second}, points.back()}));
    }
    return candidates;
}

}  // namespace

// ======================================================================================
// public API
// ======================================================================================

bool flowchart_path_hits_non_endpoint_nodes(const std::vector<Point>& path, const std::string& from_id,
                                            const std::string& to_id, const std::map<std::string, NodeLayout>& nodes) {
    for (size_t s = 0; s + 1 < path.size(); ++s)
        for (const auto& [id, node] : nodes) {
            if (is_skipped(node, from_id, to_id)) continue;
            if (segment_intersects_rect(path[s], path[s + 1], node_rect_obstacle(node))) return true;
        }
    return false;
}

size_t flowchart_endpoint_reentry_count(const std::vector<Point>& points, const Edge& edge,
                                        const std::map<std::string, NodeLayout>& nodes) {
    size_t count = 0;
    if (auto it = nodes.find(edge.from); it != nodes.end()) count += endpoint_reentry_count(points, it->second, true);
    if (edge.to != edge.from)
        if (auto it = nodes.find(edge.to); it != nodes.end()) count += endpoint_reentry_count(points, it->second, false);
    return count;
}

size_t flowchart_endpoint_direction_violation_count(const std::vector<Point>& points, const Edge& edge,
                                                    const std::map<std::string, NodeLayout>& nodes) {
    if (points.size() < 2) return 2;
    size_t violations = 0;
    if (auto it = nodes.find(edge.from); it != nodes.end()) {
        const NodeLayout& from = it->second;
        Point start = points[0], next = points[1];
        EdgeSide side = endpoint_side_for_point(from, start);
        if (!source_exits_outward(side, start, next)) ++violations;
        if (segment_intrudes_endpoint_rect(side, next, start, from)) ++violations;
    }
    if (auto it = nodes.find(edge.to); it != nodes.end()) {
        const NodeLayout& to = it->second;
        Point end = points.back(), prev = points[points.size() - 2];
        EdgeSide side = endpoint_side_for_point(to, end);
        if (!target_enters_from_outside(side, prev, end)) ++violations;
        if (segment_intrudes_endpoint_rect(side, prev, end, to)) ++violations;
    }
    return violations;
}

void reduce_orthogonal_path_crossings(const Graph& graph, const std::map<std::string, NodeLayout>& nodes, Paths& routed,
                                      const LayoutConfig& config) {
    if (graph.edges.size() < 2) return;
    float base_delta = fmax_(config.node_spacing * 0.22f, 8.0f);
    std::vector<float> deltas = {base_delta,        -base_delta,        base_delta * 1.5f, -base_delta * 1.5f, base_delta * 2.0f,
                                 -base_delta * 2.0f, base_delta * 3.0f, -base_delta * 3.0f, base_delta * 4.0f, -base_delta * 4.0f};
    // The outer perimeter box (only ER / state diagrams route around it).
    float min_x = std::numeric_limits<float>::max(), max_x = std::numeric_limits<float>::lowest();
    float min_y = std::numeric_limits<float>::max(), max_y = std::numeric_limits<float>::lowest();
    for (const auto& [id, node] : nodes) {
        if (node.hidden || node.anchor_subgraph) continue;
        min_x = fmin_(min_x, node.x);
        max_x = fmax_(max_x, node.x + node.width);
        min_y = fmin_(min_y, node.y);
        max_y = fmax_(max_y, node.y + node.height);
    }
    float outer_pad = fmax_(config.node_spacing * 0.8f, 24.0f);
    OuterBox outer{graph.kind == DiagramKind::Er || graph.kind == DiagramKind::State, min_x - outer_pad, max_x + outer_pad,
                   min_y - outer_pad, max_y + outer_pad};
    std::vector<size_t> forward(routed.size()), reverse(routed.size());
    for (size_t i = 0; i < routed.size(); ++i) {
        forward[i] = i;
        reverse[i] = routed.size() - 1 - i;
    }
    for (int pass = 0; pass < 3; ++pass) {
        bool changed = reduce_crossing_sweep(forward, graph, nodes, routed, deltas, outer);
        changed |= reduce_crossing_sweep(reverse, graph, nodes, routed, deltas, outer);
        if (!changed) break;
    }
}

void deoverlap_flowchart_paths(const Graph& graph, const std::map<std::string, NodeLayout>& nodes, Paths& routed,
                               const LayoutConfig& config) {
    if (graph.edges.size() < 2) return;
    float overlap_threshold = 0.68f;
    float base_delta = fmax_(config.node_spacing * 0.25f, 8.0f);
    float deltas[8] = {base_delta,         -base_delta,        base_delta * 1.5f, -base_delta * 1.5f,
                       base_delta * 2.0f,  -base_delta * 2.0f, base_delta * 2.8f, -base_delta * 2.8f};
    float min_segment_len = fmax_(base_delta * 1.2f, 6.0f);

    for (int pass = 0; pass < 4; ++pass) {
        bool changed = false;
        for (size_t idx = 1; idx < routed.size(); ++idx) {
            if (routed[idx].size() < 2) continue;
            const std::string& from_id = graph.edges[idx].from;
            const std::string& to_id = graph.edges[idx].to;
            float baseline = flowchart_path_overlap_with_prior(routed[idx], routed, idx);
            if (baseline < overlap_threshold) continue;
            float best_overlap = baseline;
            Path best_points = routed[idx];
            std::vector<std::pair<size_t, float>> segment_order;
            for (size_t s = 0; s + 1 < routed[idx].size(); ++s) {
                float dx = routed[idx][s + 1].first - routed[idx][s].first;
                float dy = routed[idx][s + 1].second - routed[idx][s].second;
                segment_order.push_back({s, std::sqrt(dx * dx + dy * dy)});
            }
            std::stable_sort(segment_order.begin(), segment_order.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            for (auto [seg_idx, seg_len] : segment_order) {
                if (seg_len < min_segment_len) continue;
                for (float delta : deltas) {
                    auto candidate = bump_orthogonal_segment(routed[idx], seg_idx, delta);
                    if (!candidate) continue;
                    if (flowchart_path_hits_non_endpoint_nodes(*candidate, from_id, to_id, nodes)) continue;
                    float overlap = flowchart_path_overlap_with_prior(*candidate, routed, idx);
                    if (overlap + 0.03f < best_overlap) {
                        best_overlap = overlap;
                        best_points = *candidate;
                    }
                }
            }
            if (best_overlap + 0.03f < baseline) {
                routed[idx] = best_points;
                changed = true;
            }
        }
        if (!changed) break;
    }
}

void simplify_flowchart_axis_oscillations(Paths& routed) {
    for (Path& path : routed) {
        Path collapsed = collapse_axis_aligned_runs(path);
        auto near = collapse_near_axis_aligned_path(collapsed);
        path = near ? *near : collapsed;
    }
}

void simplify_flowchart_detour_rectangles(const Graph& graph, const std::map<std::string, NodeLayout>& nodes, Paths& routed) {
    if (graph.edges.size() < 2) return;
    for (size_t idx = 0; idx < routed.size(); ++idx) {
        Path baseline = routed[idx];
        size_t baseline_bends = path_bend_count(baseline);
        if (baseline_bends < 4) continue;
        const std::string& from_id = graph.edges[idx].from;
        const std::string& to_id = graph.edges[idx].to;
        std::vector<Segment> other_segments;
        for (size_t o = 0; o < routed.size(); ++o)
            if (o != idx) append_path_segments(routed[o], other_segments);
        auto [baseline_cross, baseline_overlap] = edge_crossings_with_existing(baseline, other_segments);
        float baseline_len = path_length(baseline);
        Path best = baseline;
        size_t best_bends = baseline_bends, best_cross = baseline_cross;
        float best_overlap = baseline_overlap, best_len = baseline_len;

        auto fit = nodes.find(from_id);
        if (fit == nodes.end()) continue;
        auto tit = nodes.find(to_id);
        if (tit == nodes.end()) continue;
        std::vector<Path> candidates = detour_rectangle_simplification_candidates(baseline);
        for (auto& c : shoulder_simplification_candidates(baseline)) candidates.push_back(c);
        for (auto& c : spine_simplification_candidates(baseline, fit->second, tit->second)) candidates.push_back(c);
        for (const Path& candidate : candidates) {
            if (candidate.size() >= baseline.size()) continue;
            if (flowchart_path_hits_non_endpoint_nodes(candidate, from_id, to_id, nodes)) continue;
            size_t bends = path_bend_count(candidate);
            if (bends >= best_bends) continue;
            auto [crossings, overlap] = edge_crossings_with_existing(candidate, other_segments);
            float len = path_length(candidate);
            bool better = crossings < best_cross || (crossings == best_cross && overlap <= best_overlap + 0.05f && bends < best_bends) ||
                          (crossings == best_cross && std::fabs(overlap - best_overlap) <= 0.05f && bends == best_bends &&
                           len + 1.0f < best_len);
            if (better) {
                best = candidate;
                best_bends = bends;
                best_cross = crossings;
                best_overlap = overlap;
                best_len = len;
            }
        }
        if (best_bends < baseline_bends && best_cross <= baseline_cross) routed[idx] = best;
    }
}

void detour_flowchart_paths_around_non_endpoint_nodes(const Graph& graph, const std::map<std::string, NodeLayout>& nodes,
                                                      Paths& routed, const LayoutConfig& config) {
    float clearance = fmax_(config.node_spacing * 0.12f, 8.0f);
    for (size_t idx = 0; idx < routed.size(); ++idx) {
        if (idx >= graph.edges.size()) continue;
        const Edge& edge = graph.edges[idx];
        Path& points = routed[idx];
        for (int iter = 0; iter < 8; ++iter) {
            auto hit = first_non_endpoint_node_hit(points, edge.from, edge.to, nodes);
            if (!hit) break;
            std::optional<Path> best;
            float best_cost = std::numeric_limits<float>::infinity();
            size_t best_hits = std::numeric_limits<size_t>::max();
            for (float clearance_scale : {1.0f, 1.5f, 2.0f, 3.0f, 4.0f}) {
                float candidate_clearance = clearance * clearance_scale;
                std::vector<Path> candidates = node_detour_candidates(points, hit->first, hit->last, hit->obstacle, candidate_clearance);
                auto more = graph_detour_candidates(points, hit->first, hit->last, nodes, edge.from, edge.to, candidate_clearance);
                candidates.insert(candidates.end(), more.begin(), more.end());
                for (const Path& candidate : candidates) {
                    size_t hits = flowchart_path_non_endpoint_hit_count(candidate, edge.from, edge.to, nodes);
                    float cost = path_length(candidate) + static_cast<float>(path_bend_count(candidate)) * candidate_clearance;
                    if (hits < best_hits || (hits == best_hits && cost < best_cost)) {
                        best_hits = hits;
                        best_cost = cost;
                        best = candidate;
                    }
                }
            }
            if (!best) {
                for (const Path& candidate : graph_perimeter_detour_candidates(points, nodes, edge.from, edge.to, clearance * 2.0f)) {
                    if (flowchart_path_hits_non_endpoint_nodes(candidate, edge.from, edge.to, nodes)) continue;
                    float cost = path_length(candidate) + static_cast<float>(path_bend_count(candidate)) * clearance;
                    if (cost < best_cost) {
                        best_cost = cost;
                        best = candidate;
                    }
                }
            }
            if (!best) break;
            points = *best;
        }
    }
}

namespace {

// Subgraph rects an edge must stay out of: every subgraph that contains neither endpoint.
std::vector<Obstacle> foreign_subgraph_obstacles(const std::string& edge_from, const std::string& edge_to,
                                                 const std::vector<SubgraphLayout>& subgraphs, float clearance) {
    std::vector<Obstacle> obstacles;
    for (const SubgraphLayout& sub : subgraphs) {
        bool has_member = false;
        for (const auto& m : sub.nodes)
            if (m == edge_from || m == edge_to) has_member = true;
        if (has_member) continue;
        // Cluster-attached edges use the subgraph label as the endpoint id.
        if (sub.label == edge_from || sub.label == edge_to) continue;
        if (sub.width <= 0.0f || sub.height <= 0.0f) continue;
        obstacles.push_back({"subgraph:" + sub.label, sub.x - clearance, sub.y - clearance, sub.width + clearance * 2.0f,
                             sub.height + clearance * 2.0f, nullptr});
    }
    return obstacles;
}

size_t path_foreign_subgraph_hits(const Path& path, const std::vector<Obstacle>& obstacles) {
    size_t hits = 0;
    for (const Obstacle& o : obstacles) {
        bool hit = false;
        for (size_t s = 0; s + 1 < path.size(); ++s)
            if (segment_intersects_rect(path[s], path[s + 1], o)) { hit = true; break; }
        if (hit) ++hits;
    }
    return hits;
}

struct ForeignHit {
    size_t first, last;
    Obstacle obstacle;
};
std::optional<ForeignHit> first_foreign_subgraph_hit(const Path& path, const std::vector<Obstacle>& obstacles) {
    for (size_t seg = 0; seg + 1 < path.size(); ++seg)
        for (const Obstacle& o : obstacles)
            if (segment_intersects_rect(path[seg], path[seg + 1], o)) {
                size_t last_idx = seg;
                for (size_t later = seg; later + 1 < path.size(); ++later)
                    if (segment_intersects_rect(path[later], path[later + 1], o)) last_idx = later;
                return ForeignHit{seg, last_idx, o};
            }
    return std::nullopt;
}

}  // namespace

// Reroute edges that cut through subgraph boxes they have no business in.
void detour_flowchart_paths_around_foreign_subgraphs(const Graph& graph, const std::map<std::string, NodeLayout>& nodes,
                                                     const std::vector<SubgraphLayout>& subgraphs, Paths& routed,
                                                     const LayoutConfig& config) {
    if (subgraphs.empty()) return;
    float clearance = fmax_(config.node_spacing * 0.12f, 8.0f);
    for (size_t idx = 0; idx < routed.size(); ++idx) {
        if (idx >= graph.edges.size()) continue;
        const Edge& edge = graph.edges[idx];
        Path& points = routed[idx];
        if (edge.from == edge.to) continue;
        std::vector<Obstacle> obstacles = foreign_subgraph_obstacles(edge.from, edge.to, subgraphs, 0.0f);
        if (obstacles.empty()) continue;
        for (int iter = 0; iter < 6; ++iter) {
            auto hit = first_foreign_subgraph_hit(points, obstacles);
            if (!hit) break;
            size_t current_node_hits = flowchart_path_non_endpoint_hit_count(points, edge.from, edge.to, nodes);
            size_t current_sub_hits = path_foreign_subgraph_hits(points, obstacles);
            std::optional<Path> best;
            float best_cost = std::numeric_limits<float>::infinity();
            std::pair<size_t, size_t> best_hits = {current_sub_hits, current_node_hits};
            for (float clearance_scale : {1.0f, 1.5f, 2.0f, 3.0f}) {
                float candidate_clearance = clearance * clearance_scale;
                for (const Path& candidate : node_detour_candidates(points, hit->first, hit->last, hit->obstacle, candidate_clearance)) {
                    size_t sub_hits = path_foreign_subgraph_hits(candidate, obstacles);
                    size_t node_hits = flowchart_path_non_endpoint_hit_count(candidate, edge.from, edge.to, nodes);
                    std::pair<size_t, size_t> hits = {sub_hits, node_hits};
                    float cost = path_length(candidate) + static_cast<float>(path_bend_count(candidate)) * candidate_clearance;
                    if (hits < best_hits || (hits == best_hits && cost < best_cost)) {
                        best_hits = hits;
                        best_cost = cost;
                        best = candidate;
                    }
                }
            }
            if (!best) break;
            points = *best;
        }
    }
}

void repair_flowchart_endpoint_reentries(const Graph& graph, const std::map<std::string, NodeLayout>& nodes, Paths& routed,
                                         const LayoutConfig& config) {
    for (size_t idx = 0; idx < routed.size(); ++idx) {
        if (idx >= graph.edges.size()) continue;
        const Edge& edge = graph.edges[idx];
        for (int iter = 0; iter < 6; ++iter) {
            auto candidate = repair_endpoint_reentry_once(routed[idx], edge, nodes, config);
            if (!candidate) break;
            routed[idx] = *candidate;
        }
    }
}

}  // namespace mermaid
