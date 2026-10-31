// Mermaid port: edge label placement. Mirrors layout/label_placement.rs of mermaid-rs-renderer
// 0.3.1 (collision avoidance for edge labels): obstacle lists, anchors on an edge, the penalty
// function, the greedy centre-label placement, the tightening passes and endpoint labels.
// The flowchart-only global passes live in label_placement_flowchart.cpp.
#include "label_placement_internal.h"

namespace mermaid {

namespace lp {

// ---- obstacle lists -------------------------------------------------------------------------

// Rectangle of a subgraph's title (None when the title is empty). Used as an obstacle.
std::optional<Rect> subgraph_label_rect(const SubgraphLayout& sub, DiagramKind kind, const Theme& theme) {
    if (trim_view(sub.label).empty()) return std::nullopt;
    float width = sub.label_block.width;
    float height = sub.label_block.height;
    if (width <= 0.0f || height <= 0.0f) return std::nullopt;
    if (kind == DiagramKind::State) {
        float header_h = fmax_(height + theme.font_size * 0.75f, theme.font_size * 1.4f);
        float label_pad_x = fmax_(theme.font_size * 0.6f, height * 0.35f);
        float x = sub.x + label_pad_x;
        float y = sub.y + header_h / 2.0f - height / 2.0f;
        return Rect{x, y, width, height};
    }
    float x = sub.x + sub.width / 2.0f - width / 2.0f;
    float y = sub.y + 12.0f;
    return Rect{x, y, width, height};
}

std::vector<Rect> build_label_obstacles(const std::map<std::string, NodeLayout>& nodes,
                                        const std::vector<SubgraphLayout>& subgraphs, DiagramKind kind,
                                        const Theme& theme, float node_obstacle_pad, float subgraph_label_pad) {
    std::vector<Rect> occupied;
    for (const auto& [id, node] : nodes) {
        if (node.anchor_subgraph || node.hidden) continue;
        occupied.push_back({node.x - node_obstacle_pad, node.y - node_obstacle_pad,
                            node.width + 2.0f * node_obstacle_pad, node.height + 2.0f * node_obstacle_pad});
    }
    for (const SubgraphLayout& sub : subgraphs) {
        if (auto rect = subgraph_label_rect(sub, kind, theme)) {
            occupied.push_back({rect->x - subgraph_label_pad, rect->y - subgraph_label_pad,
                                rect->w + subgraph_label_pad * 2.0f, rect->h + subgraph_label_pad * 2.0f});
        }
    }
    return occupied;
}

std::vector<Rect> build_node_text_obstacles(const std::map<std::string, NodeLayout>& nodes, float pad) {
    std::vector<Rect> occupied;
    for (const auto& [id, node] : nodes) {
        if (node.anchor_subgraph || node.hidden) continue;
        if (node.label.width <= 0.0f || node.label.height <= 0.0f) continue;
        float cx = node.x + node.width * 0.5f;
        float cy = node.y + node.height * 0.5f;
        occupied.push_back({cx - node.label.width * 0.5f - pad, cy - node.label.height * 0.5f - pad,
                            node.label.width + pad * 2.0f, node.label.height + pad * 2.0f});
    }
    return occupied;
}

std::vector<EdgeObstacle> build_edge_obstacles(const std::vector<EdgeLayout>& edges, float pad) {
    std::vector<EdgeObstacle> obstacles;
    for (size_t idx = 0; idx < edges.size(); ++idx) {
        const auto& pts = edges[idx].points;
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            Point a = pts[i], b = pts[i + 1];
            float min_x = fmin_(a.first, b.first) - pad;
            float max_x = fmax_(a.first, b.first) + pad;
            float min_y = fmin_(a.second, b.second) - pad;
            float max_y = fmax_(a.second, b.second) + pad;
            obstacles.push_back({idx, {min_x, min_y, max_x - min_x, max_y - min_y}});
        }
    }
    return obstacles;
}

// ---- anchors on an edge ---------------------------------------------------------------------

Anchor edge_label_anchor(const EdgeLayout& edge) {
    if (edge.points.size() < 2) return {0.0f, 0.0f, 1.0f, 0.0f};
    size_t segment_count = edge.points.size() - 1;
    std::optional<size_t> best_idx;
    float best_len = 0.0f;

    size_t start_idx, end_idx;
    if (segment_count >= 3) { start_idx = 1; end_idx = segment_count - 1; }
    else { start_idx = 0; end_idx = segment_count; }

    for (size_t idx = start_idx; idx < end_idx; ++idx) {
        Point p1 = edge.points[idx], p2 = edge.points[idx + 1];
        float dx = p2.first - p1.first, dy = p2.second - p1.second;
        float len = dx * dx + dy * dy;
        if (len > best_len) { best_len = len; best_idx = idx; }
    }
    if (!best_idx) {
        for (size_t idx = 0; idx < segment_count; ++idx) {
            Point p1 = edge.points[idx], p2 = edge.points[idx + 1];
            float dx = p2.first - p1.first, dy = p2.second - p1.second;
            float len = dx * dx + dy * dy;
            if (len > best_len) { best_len = len; best_idx = idx; }
        }
    }
    size_t idx = best_idx.value_or(0);
    Point p1 = edge.points[idx], p2 = edge.points[idx + 1];
    float dx = p2.first - p1.first, dy = p2.second - p1.second;
    float len = fmax_(std::sqrt(dx * dx + dy * dy), 1e-3f);
    return {(p1.first + p2.first) / 2.0f, (p1.second + p2.second) / 2.0f, dx / len, dy / len};
}

std::optional<Anchor> edge_label_anchor_at_fraction(const EdgeLayout& edge, float t) {
    if (edge.points.size() < 2) return std::nullopt;
    size_t segment_count = edge.points.size() - 1;
    size_t start_idx, end_idx;
    if (segment_count >= 3) { start_idx = 1; end_idx = segment_count - 1; }
    else { start_idx = 0; end_idx = segment_count; }
    if (start_idx >= end_idx) { start_idx = 0; end_idx = segment_count; }

    float total_len = 0.0f;
    for (size_t idx = start_idx; idx < end_idx; ++idx) {
        Point p1 = edge.points[idx], p2 = edge.points[idx + 1];
        float dx = p2.first - p1.first, dy = p2.second - p1.second;
        total_len += std::sqrt(dx * dx + dy * dy);
    }
    if (total_len <= 1e-3f) return edge_label_anchor(edge);

    float remaining = total_len * clamp_(t, 0.0f, 1.0f);
    for (size_t idx = start_idx; idx < end_idx; ++idx) {
        Point p1 = edge.points[idx], p2 = edge.points[idx + 1];
        float dx = p2.first - p1.first, dy = p2.second - p1.second;
        float seg_len = std::sqrt(dx * dx + dy * dy);
        if (seg_len <= 1e-6f) continue;
        if (remaining <= seg_len) {
            float alpha = clamp_(remaining / seg_len, 0.0f, 1.0f);
            return Anchor{p1.first + dx * alpha, p1.second + dy * alpha, dx / seg_len, dy / seg_len};
        }
        remaining -= seg_len;
    }
    return edge_label_anchor(edge);
}

// Closest point on the edge to `point`, with the direction of that segment.
std::optional<Anchor> edge_label_anchor_from_point(const EdgeLayout& edge, Point point) {
    if (edge.points.size() < 2) return std::nullopt;
    float best_dist2 = INF;
    std::optional<Point> best_proj, best_dir;
    for (size_t i = 0; i + 1 < edge.points.size(); ++i) {
        Point p1 = edge.points[i], p2 = edge.points[i + 1];
        float dx = p2.first - p1.first, dy = p2.second - p1.second;
        float seg_len2 = dx * dx + dy * dy;
        if (seg_len2 <= 1e-6f) continue;
        float t = ((point.first - p1.first) * dx + (point.second - p1.second) * dy) / seg_len2;
        float t_clamped = clamp_(t, 0.0f, 1.0f);
        float proj_x = p1.first + dx * t_clamped;
        float proj_y = p1.second + dy * t_clamped;
        float dist2 = (point.first - proj_x) * (point.first - proj_x) + (point.second - proj_y) * (point.second - proj_y);
        if (dist2 < best_dist2) {
            best_dist2 = dist2;
            best_proj = Point{proj_x, proj_y};
            best_dir = Point{dx, dy};
        }
    }
    if (!best_proj || !best_dir) return std::nullopt;
    float dx = best_dir->first, dy = best_dir->second;
    float len = fmax_(std::sqrt(dx * dx + dy * dy), 1e-3f);
    return Anchor{best_proj->first, best_proj->second, dx / len, dy / len};
}

std::vector<Anchor> edge_segment_anchors(const EdgeLayout& edge, size_t max_count) {
    if (edge.points.size() < 2 || max_count == 0) return {};
    size_t segment_count = edge.points.size() - 1;
    size_t start_idx, end_idx;
    if (segment_count >= 3) { start_idx = 1; end_idx = segment_count - 1; }
    else { start_idx = 0; end_idx = segment_count; }
    if (start_idx >= end_idx) { start_idx = 0; end_idx = segment_count; }
    std::vector<std::pair<float, Anchor>> scored;
    for (size_t idx = start_idx; idx < end_idx; ++idx) {
        Point p1 = edge.points[idx], p2 = edge.points[idx + 1];
        float dx = p2.first - p1.first, dy = p2.second - p1.second;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 1.0f) continue;
        float dir_x = dx / len, dir_y = dy / len;
        scored.push_back({len, {(p1.first + p2.first) * 0.5f, (p1.second + p2.second) * 0.5f, dir_x, dir_y}});
    }
    // Longest segments first (stable, like Rust's sort_by).
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return cmp_f(b.first, a.first) < 0; });
    std::vector<Anchor> out;
    for (size_t i = 0; i < scored.size() && i < max_count; ++i) out.push_back(scored[i].second);
    return out;
}

std::vector<Anchor> edge_terminal_segment_anchors(const EdgeLayout& edge, size_t max_count) {
    if (edge.points.size() < 2 || max_count == 0) return {};
    std::vector<Anchor> result;
    size_t seg_count = edge.points.size() - 1;
    size_t seg_indices[2] = {0, seg_count - 1};
    for (size_t seg_idx : seg_indices) {
        if (result.size() >= max_count) break;
        if (seg_idx >= seg_count) continue;
        Point p1 = edge.points[seg_idx], p2 = edge.points[seg_idx + 1];
        float dx = p2.first - p1.first, dy = p2.second - p1.second;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 8.0f) continue;
        float dir_x = dx / len, dir_y = dy / len;
        Anchor anchor{(p1.first + p2.first) * 0.5f, (p1.second + p2.second) * 0.5f, dir_x, dir_y};
        bool dup = false;
        for (const Anchor& e : result)
            if (std::fabs(e.x - anchor.x) <= LABEL_ANCHOR_POS_EPS && std::fabs(e.y - anchor.y) <= LABEL_ANCHOR_POS_EPS &&
                std::fabs(e.dx - anchor.dx) <= LABEL_ANCHOR_DIR_EPS && std::fabs(e.dy - anchor.dy) <= LABEL_ANCHOR_DIR_EPS)
                dup = true;
        if (!dup) result.push_back(anchor);
    }
    if (result.size() > max_count) result.resize(max_count);
    return result;
}

void push_anchor_unique(std::vector<Anchor>& anchors, Anchor c) {
    for (const Anchor& a : anchors)
        if (std::fabs(a.x - c.x) <= LABEL_ANCHOR_POS_EPS && std::fabs(a.y - c.y) <= LABEL_ANCHOR_POS_EPS &&
            std::fabs(a.dx - c.dx) <= LABEL_ANCHOR_DIR_EPS && std::fabs(a.dy - c.dy) <= LABEL_ANCHOR_DIR_EPS)
            return;
    anchors.push_back(c);
}

// Parallel labelled edges between the same two nodes get spread fractions along the edge.
std::vector<std::optional<float>> edge_label_bundle_fractions(const std::vector<EdgeLayout>& edges) {
    std::map<std::pair<std::string, std::string>, std::vector<size_t>> bundle_map;
    for (size_t idx = 0; idx < edges.size(); ++idx) {
        if (!edges[idx].label) continue;
        bundle_map[{edges[idx].from, edges[idx].to}].push_back(idx);
    }
    std::vector<std::optional<float>> preferred(edges.size());
    for (auto& [key, indices] : bundle_map) {
        if (indices.size() <= 1) continue;
        std::sort(indices.begin(), indices.end());
        size_t count = indices.size();
        float left = 0.16f, right = 0.84f;
        float span = fmax_(right - left, 0.0f);
        for (size_t rank = 0; rank < indices.size(); ++rank) {
            float fraction;
            if (count == 2) fraction = rank == 0 ? 0.34f : 0.66f;
            else fraction = left + span * (static_cast<float>(rank) / static_cast<float>(count - 1));
            preferred[indices[rank]] = clamp_(fraction, 0.05f, 0.95f);
        }
    }
    return preferred;
}

// ---- the penalty function every search uses -------------------------------------------------

Cost label_penalties(Rect rect, Point anchor, float label_w, float label_h, const LabelPenaltyContext& ctx) {
    DiagramKind kind = ctx.kind;
    float area = fmax_(label_w * label_h, 1.0f);
    float overlap = 0.0f;
    float label_weight = kind == DiagramKind::Flowchart ? WEIGHT_FLOWCHART_LABEL_OVERLAP : WEIGHT_LABEL_OVERLAP;
    float edge_weight = kind == DiagramKind::Flowchart ? WEIGHT_FLOWCHART_EDGE_OVERLAP : WEIGHT_EDGE_OVERLAP;
    float foreign_edge_overlap = 0.0f;
    bool foreign_edge_touch = false;
    for (size_t i : ctx.occupied_grid.query(rect)) {
        float ov = overlap_area(rect, ctx.occupied[i]);
        if (ov > 0.0f) {
            float weight;
            if (i < ctx.node_obstacle_count)
                weight = kind == DiagramKind::Flowchart ? WEIGHT_NODE_OVERLAP_FLOWCHART : WEIGHT_NODE_OVERLAP;
            else
                weight = label_weight;
            overlap += ov * weight;
        }
    }
    for (size_t i : ctx.edge_grid.query(rect)) {
        const EdgeObstacle& eo = ctx.edge_obstacles[i];
        if (eo.edge == ctx.edge_idx) continue;
        float ov = overlap_area(rect, eo.rect);
        overlap += ov * edge_weight;
        if (kind == DiagramKind::Flowchart && ov > 0.0f) {
            foreign_edge_overlap += ov;
            foreign_edge_touch = true;
        }
    }
    if (kind == DiagramKind::Flowchart) {
        overlap += foreign_edge_overlap * FLOWCHART_FOREIGN_EDGE_OVERLAP_WEIGHT;
        if (foreign_edge_touch) overlap += area * FLOWCHART_FOREIGN_EDGE_TOUCH_HARD_PENALTY;
    }
    if (ctx.bounds) overlap += outside_area(rect, *ctx.bounds) * WEIGHT_OUTSIDE;
    Rect own_edge_rect = rect;
    if (kind == DiagramKind::State) {
        float pad_x = fmax_(rect.w - label_w, 0.0f) * 0.5f;
        float pad_y = fmax_(rect.h - label_h, 0.0f) * 0.5f;
        own_edge_rect = {rect.x + pad_x, rect.y + pad_y, label_w, label_h};
    }
    float own_edge_dist = polyline_rect_distance(ctx.own_edge_points, own_edge_rect);
    if (std::isfinite(own_edge_dist)) {
        float target_gap, under_weight, over_weight, hard_penalty;
        if (kind == DiagramKind::Flowchart) {
            target_gap = OWN_EDGE_GAP_TARGET_FLOWCHART;
            under_weight = OWN_EDGE_GAP_UNDER_WEIGHT_FLOWCHART;
            over_weight = OWN_EDGE_GAP_OVER_WEIGHT_FLOWCHART;
            hard_penalty = OWN_EDGE_TOUCH_HARD_PENALTY_FLOWCHART;
        } else if (kind == DiagramKind::Class) {
            target_gap = OWN_EDGE_GAP_TARGET_CLASS;
            under_weight = OWN_EDGE_GAP_UNDER_WEIGHT_CLASS;
            over_weight = OWN_EDGE_GAP_OVER_WEIGHT_CLASS;
            hard_penalty = OWN_EDGE_TOUCH_HARD_PENALTY_CLASS;
        } else {
            target_gap = OWN_EDGE_GAP_TARGET;
            under_weight = OWN_EDGE_GAP_UNDER_WEIGHT;
            over_weight = OWN_EDGE_GAP_OVER_WEIGHT;
            hard_penalty = OWN_EDGE_TOUCH_HARD_PENALTY;
        }
        if (own_edge_dist < target_gap) {
            float shortage = (target_gap - own_edge_dist) / fmax_(target_gap, 1e-3f);
            overlap += area * (shortage * shortage * under_weight);
        }
        if (own_edge_dist > target_gap) {
            float excess = (own_edge_dist - target_gap) / fmax_(target_gap, 1e-3f);
            overlap += area * (excess * excess * over_weight);
        }
        if (kind == DiagramKind::Flowchart && own_edge_dist > FLOWCHART_OWN_EDGE_SOFT_MAX_GAP) {
            float over = own_edge_dist - FLOWCHART_OWN_EDGE_SOFT_MAX_GAP;
            overlap += area * (over * over * FLOWCHART_OWN_EDGE_SOFT_MAX_GAP_WEIGHT);
        }
        if (kind == DiagramKind::Flowchart && own_edge_dist > FLOWCHART_OWN_EDGE_HARD_MAX_GAP) {
            float over = own_edge_dist - FLOWCHART_OWN_EDGE_HARD_MAX_GAP;
            overlap += area * (over * FLOWCHART_OWN_EDGE_HARD_MAX_GAP_WEIGHT);
        }
        if (own_edge_dist <= 0.35f) overlap += area * hard_penalty;
    }
    float dx = (rect.x + rect.w * 0.5f) - anchor.first;
    float dy = (rect.y + rect.h * 0.5f) - anchor.second;
    float dist = std::sqrt(dx * dx + dy * dy);
    return {overlap / area, dist / (label_w + label_h + 1.0f)};
}

}  // namespace lp

namespace {

using namespace lp;

// ---- small passes ---------------------------------------------------------------------------

// If a flowchart label ended up touching its own edge, hop it to the first free spot around it.
void move_flowchart_labels_off_own_edges(std::vector<EdgeLayout>& edges) {
    for (EdgeLayout& edge : edges) {
        if (!edge.label || !edge.label_anchor) continue;
        const TextBlock& label = *edge.label;
        Point anchor = *edge.label_anchor;
        Rect rect = {anchor.first - label.width / 2.0f, anchor.second - label.height / 2.0f, label.width, label.height};
        if (polyline_rect_distance(edge.points, rect) > 0.0f) continue;
        float dy = label.height + 24.0f;
        float dx = label.width * 0.5f + 24.0f;
        Point candidates[8] = {{anchor.first, anchor.second - dy},       {anchor.first, anchor.second + dy},
                               {anchor.first - dx, anchor.second},       {anchor.first + dx, anchor.second},
                               {anchor.first - dx, anchor.second - dy},  {anchor.first + dx, anchor.second - dy},
                               {anchor.first - dx, anchor.second + dy},  {anchor.first + dx, anchor.second + dy}};
        for (const Point& candidate : candidates) {
            Rect crect = {candidate.first - label.width / 2.0f, candidate.second - label.height / 2.0f, label.width,
                          label.height};
            if (polyline_rect_distance(edge.points, crect) > 0.0f) {
                edge.label_anchor = candidate;
                break;
            }
        }
    }
}

CenterLabelOverlapScore center_label_overlap_score(const std::vector<EdgeLayout>& edges, float label_pad_x,
                                                   float label_pad_y) {
    std::vector<Rect> rects;
    for (const EdgeLayout& edge : edges) {
        if (!edge.label || !edge.label_anchor) continue;
        rects.push_back(flowchart_center_label_rect(*edge.label_anchor, edge.label->width, edge.label->height,
                                                    label_pad_x, label_pad_y));
    }
    CenterLabelOverlapScore s;
    for (size_t i = 0; i < rects.size(); ++i)
        for (size_t j = i + 1; j < rects.size(); ++j) {
            float overlap = overlap_area(rects[i], rects[j]);
            if (overlap > LABEL_OVERLAP_WIDE_THRESHOLD) {
                s.count += 1;
                s.total += overlap;
                s.max = fmax_(s.max, overlap);
            }
        }
    return s;
}

// Move labels that sit on their own edge to the nearest spot (small steps outward) that does not.
void nudge_flowchart_labels_clear_of_own_paths(std::vector<EdgeLayout>& edges, Bounds bounds) {
    std::vector<std::optional<Rect>> label_rects;
    for (const EdgeLayout& edge : edges) {
        if (!edge.label || !edge.label_anchor) label_rects.push_back(std::nullopt);
        else label_rects.push_back(label_core_rect(*edge.label_anchor, *edge.label));
    }

    for (size_t idx = 0; idx < edges.size(); ++idx) {
        if (!edges[idx].label || !edges[idx].label_anchor) continue;
        const TextBlock label = *edges[idx].label;
        Point center = *edges[idx].label_anchor;
        Rect current_rect = label_core_rect(center, label);
        if (!path_intersects_rect(edges[idx].points, current_rect)) continue;

        std::optional<Point> best_center;
        float best_cost = INF;
        const float dirs[8][2] = {{0.0f, -1.0f},    {0.0f, 1.0f},     {-1.0f, 0.0f},   {1.0f, 0.0f},
                                  {-0.707f, -0.707f}, {0.707f, -0.707f}, {-0.707f, 0.707f}, {0.707f, 0.707f}};
        const float steps[8] = {2.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 24.0f, 32.0f};
        for (float step : steps) {
            for (const auto& d : dirs) {
                Point candidate = {center.first + d[0] * step, center.second + d[1] * step};
                if (bounds)
                    candidate = clamp_label_center_to_bounds(candidate, label.width, label.height, 0.0f, 0.0f, *bounds);
                Rect rect = label_core_rect(candidate, label);
                if (path_intersects_rect(edges[idx].points, rect)) continue;
                float overlap = 0.0f;
                for (size_t other_idx = 0; other_idx < label_rects.size(); ++other_idx) {
                    if (other_idx == idx) continue;
                    if (label_rects[other_idx]) overlap += overlap_area(rect, *label_rects[other_idx]);
                }
                float outside = bounds ? outside_area(rect, *bounds) : 0.0f;
                float cost = step + overlap * 10.0f + outside * 20.0f;
                if (cost < best_cost) {
                    best_cost = cost;
                    best_center = candidate;
                }
            }
            if (best_center) break;
        }
        if (best_center) {
            edges[idx].label_anchor = best_center;
            label_rects[idx] = label_core_rect(*best_center, label);
        }
    }
}

// ---- tightening: pull labels closer to their own edge ---------------------------------------

struct GapLimits {
    float target, soft_max, hard_max;
};
GapLimits center_label_gap_limits(DiagramKind kind) {
    switch (kind) {
        case DiagramKind::Flowchart:
            return {OWN_EDGE_GAP_TARGET_FLOWCHART, FLOWCHART_OWN_EDGE_SOFT_MAX_GAP, FLOWCHART_OWN_EDGE_HARD_MAX_GAP};
        case DiagramKind::State: return {1.7f, 4.8f, STATE_OWN_EDGE_HARD_MAX_GAP};
        case DiagramKind::Class: return {1.6f, 4.6f, CLASS_OWN_EDGE_HARD_MAX_GAP};
        default: return {1.5f, 5.0f, DEFAULT_OWN_EDGE_HARD_MAX_GAP};
    }
}

Rect center_label_obstacle_rect(DiagramKind kind, Point center, float label_w, float label_h, float label_pad_x,
                                float label_pad_y) {
    Rect rect = padded_label_rect(center, label_w, label_h, label_pad_x, label_pad_y);
    if (kind == DiagramKind::Flowchart) return inflate_rect(rect, FLOWCHART_LABEL_CLEARANCE_PAD);
    return rect;
}

float center_label_gap_penalty(float gap, float target_gap, float soft_max_gap, float hard_max_gap) {
    if (!std::isfinite(gap)) return 120.0f;
    float target = fmax_(target_gap, 1e-3f);
    float penalty = 0.0f;
    if (gap <= 0.35f) penalty += 28.0f;
    float dev = (gap - target) / target;
    penalty += dev * dev * 0.8f;
    if (gap > soft_max_gap) {
        float over = gap - soft_max_gap;
        penalty += over * over * 2.0f;
    }
    if (gap > hard_max_gap) {
        float over = gap - hard_max_gap;
        penalty += over * 10.0f;
    }
    return penalty;
}

std::vector<Point> center_label_tighten_candidates(const EdgeLayout& edge, Point current_center, float label_w,
                                                   float label_h, float label_pad_x, float label_pad_y,
                                                   DiagramKind kind, Bounds bounds) {
    std::vector<Point> candidates;
    auto push_candidate = [&](Point center) {
        if (bounds) center = clamp_label_center_to_bounds(center, label_w, label_h, label_pad_x, label_pad_y, *bounds);
        push_center_unique(candidates, center);
    };
    push_candidate(current_center);

    std::vector<Anchor> anchors;
    if (auto a = edge_label_anchor_from_point(edge, current_center)) push_anchor_unique(anchors, *a);
    for (float frac : LABEL_ANCHOR_FRACTIONS)
        if (auto a = edge_label_anchor_at_fraction(edge, frac)) push_anchor_unique(anchors, *a);
    for (const Anchor& a : edge_segment_anchors(edge, LABEL_EXTRA_SEGMENT_ANCHORS)) push_anchor_unique(anchors, a);
    if (kind == DiagramKind::Flowchart || kind == DiagramKind::State)
        for (const Anchor& a : edge_terminal_segment_anchors(edge, 2)) push_anchor_unique(anchors, a);
    if (anchors.empty()) anchors.push_back(edge_label_anchor(edge));
    else push_anchor_unique(anchors, edge_label_anchor(edge));

    std::vector<float> gap_targets, tangent_steps;
    if (kind == DiagramKind::Flowchart) {
        gap_targets = {0.9f, 1.4f, 1.9f, 2.6f, 3.6f, 4.8f, 6.2f};
        tangent_steps = {0.0f, 0.25f, -0.25f, 0.7f, -0.7f, 1.3f, -1.3f, 2.1f, -2.1f, 3.2f, -3.2f, 4.4f, -4.4f};
    } else if (kind == DiagramKind::State) {
        gap_targets = {0.9f, 1.4f, 1.9f, 2.5f, 3.4f, 4.4f, 5.8f};
        tangent_steps = {0.0f, 0.18f, -0.18f, 0.5f, -0.5f, 1.0f, -1.0f, 1.8f, -1.8f, 2.8f, -2.8f, 4.0f, -4.0f, 5.5f, -5.5f};
    } else {
        gap_targets = {0.8f, 1.3f, 1.8f, 2.4f, 3.2f, 4.2f, 5.6f};
        tangent_steps = {0.0f, 0.2f, -0.2f, 0.6f, -0.6f, 1.2f, -1.2f, 2.0f, -2.0f, 3.0f, -3.0f};
    }
    const float local_tangent_steps[5] = {0.0f, 0.35f, -0.35f, 0.8f, -0.8f};
    const float local_normal_steps[5] = {0.0f, 0.2f, -0.2f, 0.45f, -0.45f};

    for (const Anchor& an : anchors) {
        float normal_x = -an.dy, normal_y = an.dx;
        float step_n = std::fabs(normal_x) > std::fabs(normal_y) ? label_w + label_pad_x : label_h + label_pad_y;
        float step_t = std::fabs(an.dx) > std::fabs(an.dy) ? label_w + label_pad_x : label_h + label_pad_y;
        float half_w = label_w * 0.5f + label_pad_x;
        float half_h = label_h * 0.5f + label_pad_y;
        float normal_extent = std::fabs(normal_x) * half_w + std::fabs(normal_y) * half_h;
        for (float t : tangent_steps) {
            float base_x = an.x + an.dx * step_t * t;
            float base_y = an.y + an.dy * step_t * t;
            for (float gap : gap_targets) {
                float offset = normal_extent + gap;
                push_candidate({base_x + normal_x * offset, base_y + normal_y * offset});
                push_candidate({base_x - normal_x * offset, base_y - normal_y * offset});
            }
        }
        for (float t : local_tangent_steps) {
            float base_x = an.x + an.dx * step_t * t;
            float base_y = an.y + an.dy * step_t * t;
            for (float n : local_normal_steps)
                push_candidate({base_x + normal_x * step_n * n, base_y + normal_y * step_n * n});
        }
    }
    return candidates;
}

void tighten_center_label_gaps(std::vector<EdgeLayout>& edges, const std::map<std::string, NodeLayout>& nodes,
                               const std::vector<SubgraphLayout>& subgraphs, Bounds bounds, DiagramKind kind,
                               const Theme& theme, float label_pad_x, float label_pad_y,
                               const std::set<size_t>& locked_indices) {
    bool any = false;
    for (const EdgeLayout& e : edges)
        if (e.label && e.label_anchor) any = true;
    if (!any) return;

    GapLimits lim = center_label_gap_limits(kind);
    float target_gap = lim.target, soft_max_gap = lim.soft_max, hard_max_gap = lim.hard_max;
    int iterations = kind == DiagramKind::Flowchart ? 4 : (kind == DiagramKind::State || kind == DiagramKind::Class ? 6 : 4);
    float node_obstacle_pad = center_label_node_obstacle_pad(kind, theme, label_pad_x, label_pad_y);
    float subgraph_label_pad = fmax_(theme.font_size * 0.35f, 3.0f);
    std::vector<Rect> static_obstacles =
        build_label_obstacles(nodes, subgraphs, kind, theme, node_obstacle_pad, subgraph_label_pad);
    if (kind == DiagramKind::Flowchart)
        for (const Rect& r : build_node_text_obstacles(nodes, fmax_(theme.font_size * 0.2f, 2.0f)))
            static_obstacles.push_back(r);
    size_t node_obstacle_count = static_obstacles.size();
    float edge_obstacle_pad = fmax_(theme.font_size * 0.35f, label_pad_y);
    std::vector<EdgeObstacle> edge_obstacles = build_edge_obstacles(edges, edge_obstacle_pad);
    ObstacleGrid edge_grid(48.0f, obstacle_rects(edge_obstacles));

    for (int iter = 0; iter < iterations; ++iter) {
        std::vector<std::pair<size_t, float>> order;
        for (size_t idx = 0; idx < edges.size(); ++idx) {
            const EdgeLayout& edge = edges[idx];
            if (locked_indices.count(idx)) continue;
            if (!edge.label || !edge.label_anchor) continue;
            Rect rect = padded_label_rect(*edge.label_anchor, edge.label->width, edge.label->height, label_pad_x,
                                          label_pad_y);
            float gap = polyline_rect_distance(edge.points, rect);
            if (std::isfinite(gap) && gap > target_gap + 0.3f) order.push_back({idx, gap});
        }
        if (order.empty()) break;
        std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return cmp_f(b.second, a.second) < 0; });

        bool moved = false;
        for (const auto& [idx, ignored_gap] : order) {
            (void)ignored_gap;
            if (!edges[idx].label || !edges[idx].label_anchor) continue;
            TextBlock label = *edges[idx].label;
            Point current_center = *edges[idx].label_anchor;
            std::vector<Point> edge_points = edges[idx].points;
            if (edge_points.size() < 2) continue;

            std::vector<Rect> occupied = static_obstacles;
            for (size_t other_idx = 0; other_idx < edges.size(); ++other_idx) {
                if (other_idx == idx) continue;
                const EdgeLayout& other = edges[other_idx];
                if (!other.label || !other.label_anchor) continue;
                occupied.push_back(center_label_obstacle_rect(kind, *other.label_anchor, other.label->width,
                                                              other.label->height, label_pad_x, label_pad_y));
            }
            ObstacleGrid occupied_grid(48.0f, occupied);
            Rect current_rect = padded_label_rect(current_center, label.width, label.height, label_pad_x, label_pad_y);
            Anchor current_anchor = edge_label_anchor_from_point(edges[idx], current_center)
                                        .value_or(edge_label_anchor(edges[idx]));
            float current_gap = polyline_rect_distance(edge_points, current_rect);
            LabelPenaltyContext penalty_ctx{kind,         occupied, occupied_grid, node_obstacle_count,
                                            edge_obstacles, edge_grid, idx,          edge_points,
                                            bounds};
            Cost current_cost = label_penalties(current_rect, {current_anchor.x, current_anchor.y}, label.width,
                                                label.height, penalty_ctx);
            current_cost.first += center_label_gap_penalty(current_gap, target_gap, soft_max_gap, hard_max_gap);

            std::vector<Point> candidates = center_label_tighten_candidates(
                edges[idx], current_center, label.width, label.height, label_pad_x, label_pad_y, kind, bounds);
            Point best_center = current_center;
            Cost best_cost = current_cost;
            float best_gap = current_gap;
            auto evaluate = [&](Point center, bool allow_above_hard) {
                if (std::fabs(center.first - current_center.first) <= 0.2f &&
                    std::fabs(center.second - current_center.second) <= 0.2f)
                    return;
                Rect rect = padded_label_rect(center, label.width, label.height, label_pad_x, label_pad_y);
                float gap = polyline_rect_distance(edge_points, rect);
                if (!allow_above_hard && std::isfinite(gap) && gap > hard_max_gap) return;
                Anchor anchor = edge_label_anchor_from_point(edges[idx], center).value_or(edge_label_anchor(edges[idx]));
                Cost cost = label_penalties(rect, {anchor.x, anchor.y}, label.width, label.height, penalty_ctx);
                cost.first += center_label_gap_penalty(gap, target_gap, soft_max_gap, hard_max_gap);
                float dx = center.first - current_center.first;
                float dy = center.second - current_center.second;
                cost.second += std::sqrt(dx * dx + dy * dy) / (label.width + label.height + 1.0f) * 0.35f;
                if (candidate_better(cost, best_cost)) {
                    best_center = center;
                    best_cost = cost;
                    best_gap = gap;
                }
            };
            for (const Point& c : candidates) evaluate(c, false);
            if (std::fabs(best_center.first - current_center.first) <= 0.2f &&
                std::fabs(best_center.second - current_center.second) <= 0.2f)
                for (const Point& c : candidates) evaluate(c, true);

            if (std::fabs(best_center.first - current_center.first) <= 0.2f &&
                std::fabs(best_center.second - current_center.second) <= 0.2f)
                continue;
            bool gap_improved = best_gap + 0.05f < current_gap;
            bool needs_tightening = current_gap > soft_max_gap + 0.2f;
            bool cost_improved = candidate_better(best_cost, current_cost);
            bool acceptable = gap_improved ? best_cost.first <= current_cost.first + 0.35f : false;
            if ((cost_improved && (gap_improved || needs_tightening)) || acceptable) {
                edges[idx].label_anchor = best_center;
                moved = true;
            }
        }
        if (!moved) break;
    }
}

std::optional<float> center_label_attachment_cap(DiagramKind kind) {
    switch (kind) {
        case DiagramKind::Flowchart:
        case DiagramKind::Sequence:
        case DiagramKind::ZenUML: return std::nullopt;
        case DiagramKind::Er: return 12.0f;
        case DiagramKind::Class: return 10.0f;
        case DiagramKind::State: return 11.0f;
        default: return 12.0f;
    }
}

void enforce_center_label_attachment_caps(std::vector<EdgeLayout>& edges,
                                          const std::map<std::string, NodeLayout>& nodes,
                                          const std::vector<SubgraphLayout>& subgraphs, Bounds bounds,
                                          DiagramKind kind, const Theme& theme, float label_pad_x, float label_pad_y,
                                          const std::set<size_t>& locked_indices) {
    auto cap = center_label_attachment_cap(kind);
    if (!cap) return;
    float max_gap = *cap;
    bool any = false;
    for (const EdgeLayout& e : edges)
        if (e.label && e.label_anchor) any = true;
    if (!any) return;

    float node_obstacle_pad = center_label_node_obstacle_pad(kind, theme, label_pad_x, label_pad_y);
    float subgraph_label_pad = fmax_(theme.font_size * 0.35f, 3.0f);
    std::vector<Rect> static_obstacles =
        build_label_obstacles(nodes, subgraphs, kind, theme, node_obstacle_pad, subgraph_label_pad);
    float nudge_weight = kind == DiagramKind::Er ? 0.04f : 0.06f;

    for (int pass = 0; pass < 2; ++pass) {
        std::vector<std::optional<Rect>> current_label_rects;
        for (const EdgeLayout& e : edges) {
            if (!e.label || !e.label_anchor) current_label_rects.push_back(std::nullopt);
            else
                current_label_rects.push_back(
                    padded_label_rect(*e.label_anchor, e.label->width, e.label->height, label_pad_x, label_pad_y));
        }

        for (size_t idx = 0; idx < edges.size(); ++idx) {
            EdgeLayout& edge = edges[idx];
            if (locked_indices.count(idx)) continue;
            if (!edge.label || !edge.label_anchor) continue;
            const TextBlock& label = *edge.label;
            Point center = *edge.label_anchor;
            Rect current_rect = padded_label_rect(center, label.width, label.height, label_pad_x, label_pad_y);
            float current_gap = polyline_rect_distance(edge.points, current_rect);
            float current_center_dist = point_polyline_distance(center, edge.points);
            if (!std::isfinite(current_gap) || !std::isfinite(current_center_dist)) continue;
            if (current_gap <= max_gap + 0.05f && current_center_dist <= max_gap + 0.05f) continue;
            Anchor an = edge_label_anchor_from_point(edge, center).value_or(edge_label_anchor(edge));
            float normal_x = -an.dy, normal_y = an.dx;
            float rel_x = center.first - an.x, rel_y = center.second - an.y;
            float sign = (rel_x * normal_x + rel_y * normal_y >= 0.0f) ? 1.0f : -1.0f;
            float target_gap = clamp_(edge_target_distance(kind, label.height, label_pad_y), 1.2f, max_gap * 0.75f);
            float offsets[4] = {target_gap, fmax_(target_gap * 0.72f, 0.9f), fmin_(target_gap * 1.24f, max_gap * 0.92f),
                                0.0f};

            std::vector<Point> candidates;
            for (float offset : offsets) {
                for (float side : {sign, -sign}) {
                    Point cand = {an.x + normal_x * offset * side, an.y + normal_y * offset * side};
                    if (bounds)
                        cand = clamp_label_center_to_bounds(cand, label.width, label.height, label_pad_x, label_pad_y,
                                                            *bounds);
                    push_center_unique(candidates, cand);
                }
            }
            push_center_unique(candidates, center);
            if (candidates.empty()) continue;

            Point best = center;
            float best_score = INF;
            for (const Point& cand : candidates) {
                Rect rect = padded_label_rect(cand, label.width, label.height, label_pad_x, label_pad_y);
                float gap = polyline_rect_distance(edge.points, rect);
                float center_dist = point_polyline_distance(cand, edge.points);
                if (!std::isfinite(gap)) continue;
                float overlap = 0.0f;
                for (const Rect& obstacle : static_obstacles) overlap += overlap_area(rect, obstacle);
                for (size_t other_idx = 0; other_idx < current_label_rects.size(); ++other_idx) {
                    if (other_idx == idx) continue;
                    if (current_label_rects[other_idx]) overlap += overlap_area(rect, *current_label_rects[other_idx]);
                }
                if (bounds) overlap += outside_area(rect, *bounds);
                float gap_over = fmax_(gap - max_gap, 0.0f);
                float move_dx = cand.first - center.first;
                float move_dy = cand.second - center.second;
                float move_dist = std::sqrt(move_dx * move_dx + move_dy * move_dy);
                float center_over = fmax_(center_dist - max_gap, 0.0f);
                float score = gap_over * 32.0f + center_over * 40.0f + std::fabs(gap - target_gap) * 0.9f +
                              overlap * 0.06f + move_dist * nudge_weight;
                if (score < best_score) {
                    best_score = score;
                    best = cand;
                }
            }
            edge.label_anchor = best;
        }
    }
}

// ---- greedy centre-label placement ----------------------------------------------------------

void resolve_center_labels(std::vector<EdgeLayout>& edges, const std::map<std::string, NodeLayout>& nodes,
                           const std::vector<SubgraphLayout>& subgraphs, Bounds bounds, DiagramKind kind,
                           const Theme& theme, const LayoutConfig& config) {
    auto [label_pad_x, label_pad_y] = edge_label_padding(kind, config);
    float node_obstacle_pad = center_label_node_obstacle_pad(kind, theme, label_pad_x, label_pad_y);
    float edge_obstacle_pad = fmax_(theme.font_size * 0.35f, label_pad_y);
    float step_normal_pad = fmax_(theme.font_size * 0.22f, label_pad_y);
    float step_tangent_pad = fmax_(theme.font_size * 0.28f, label_pad_x);
    float subgraph_label_pad = fmax_(theme.font_size * 0.35f, 3.0f);

    std::vector<Rect> occupied =
        build_label_obstacles(nodes, subgraphs, kind, theme, node_obstacle_pad, subgraph_label_pad);
    if (kind == DiagramKind::Flowchart)
        for (const Rect& r : build_node_text_obstacles(nodes, fmax_(theme.font_size * 0.2f, 2.0f)))
            occupied.push_back(r);
    size_t node_obstacle_count = occupied.size();
    std::vector<EdgeObstacle> edge_obstacles = build_edge_obstacles(edges, edge_obstacle_pad);
    ObstacleGrid edge_grid(48.0f, obstacle_rects(edge_obstacles));
    ObstacleGrid occupied_grid(48.0f, occupied);
    std::vector<std::optional<float>> bundle_fractions;
    if (kind == DiagramKind::Flowchart) bundle_fractions = edge_label_bundle_fractions(edges);
    else bundle_fractions.assign(edges.size(), std::nullopt);
    std::set<size_t> fixed_center_indices;
    for (size_t idx = 0; idx < edges.size(); ++idx) {
        EdgeLayout& edge = edges[idx];
        if (!edge.label || !edge.label_anchor) continue;
        const TextBlock& label = *edge.label;
        Point anchor = *edge.label_anchor;
        Point clamped = bounds ? clamp_label_center_to_bounds(anchor, label.width, label.height, label_pad_x,
                                                              label_pad_y, *bounds)
                               : anchor;
        Rect rect = padded_label_rect(clamped, label.width, label.height, label_pad_x, label_pad_y);
        Rect occupied_rect = kind == DiagramKind::Flowchart ? inflate_rect(rect, FLOWCHART_LABEL_CLEARANCE_PAD) : rect;
        edge.label_anchor = clamped;
        // Flowchart routing seeds labels with a reserved centre; that is only a starting point,
        // so it does not become an obstacle or a fixed index here.
        if (kind == DiagramKind::Flowchart) continue;
        occupied_grid.insert(occupied.size(), occupied_rect);
        occupied.push_back(occupied_rect);
        fixed_center_indices.insert(idx);
    }

    // Movable edges, most constrained first: pre-seeded anchors, then larger labels, then shorter edges.
    std::vector<size_t> order;
    for (size_t i = 0; i < edges.size(); ++i)
        if (edges[i].label && !fixed_center_indices.count(i)) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        bool a_fixed = edges[a].label_anchor.has_value();
        bool b_fixed = edges[b].label_anchor.has_value();
        if (a_fixed && !b_fixed) return true;
        if (!a_fixed && b_fixed) return false;
        float area_a = edges[a].label ? edges[a].label->width * edges[a].label->height : 0.0f;
        float area_b = edges[b].label ? edges[b].label->width * edges[b].label->height : 0.0f;
        if (std::fabs(area_a - area_b) > 1e-3f) return cmp_f(area_b, area_a) < 0;
        return cmp_f(edge_path_length(edges[a]), edge_path_length(edges[b])) < 0;
    });

    for (size_t idx : order) {
        if (!edges[idx].label) continue;
        const TextBlock label = *edges[idx].label;
        float pad_w = label.width + 2.0f * label_pad_x;
        float pad_h = label.height + 2.0f * label_pad_y;

        const EdgeLayout& edge = edges[idx];
        std::vector<Anchor> anchors;

        if (edge.label_anchor)
            if (auto candidate = edge_label_anchor_from_point(edge, *edge.label_anchor))
                push_anchor_unique(anchors, *candidate);
        if (bundle_fractions[idx]) {
            const float side_bias[3] = {0.0f, -0.08f, 0.08f};
            for (float delta : side_bias) {
                float frac = clamp_(*bundle_fractions[idx] + delta, 0.05f, 0.95f);
                if (auto candidate = edge_label_anchor_at_fraction(edge, frac)) push_anchor_unique(anchors, *candidate);
            }
        }
        for (float frac : LABEL_ANCHOR_FRACTIONS)
            if (auto candidate = edge_label_anchor_at_fraction(edge, frac)) push_anchor_unique(anchors, *candidate);
        for (const Anchor& c : edge_segment_anchors(edge, LABEL_EXTRA_SEGMENT_ANCHORS)) push_anchor_unique(anchors, c);
        if (kind == DiagramKind::Flowchart)
            for (const Anchor& c : edge_terminal_segment_anchors(edge, 2)) push_anchor_unique(anchors, c);
        if (anchors.empty()) anchors.push_back(edge_label_anchor(edge));
        else push_anchor_unique(anchors, edge_label_anchor(edge));

        std::vector<float> normal_steps, tangent_steps;
        if (kind == DiagramKind::Flowchart) {
            normal_steps = {0.6f, -0.6f, 1.0f, -1.0f, 1.4f, -1.4f, 0.35f, -0.35f, 2.0f, -2.0f, 2.8f, -2.8f, 0.0f};
            tangent_steps = {0.0f, 0.3f, -0.3f, 0.8f, -0.8f, 1.4f, -1.4f, 2.2f, -2.2f, 3.2f, -3.2f};
        } else if (kind == DiagramKind::State) {
            normal_steps = {0.0f, 0.12f, -0.12f, 0.22f, -0.22f, 0.35f, -0.35f, 0.45f, -0.45f, 0.5f, -0.5f, 0.55f,
                            -0.55f, 0.6f, -0.6f, 1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f};
            tangent_steps = {0.0f, 0.12f, -0.12f, 0.2f, -0.2f, 0.35f, -0.35f, 0.6f, -0.6f, 1.2f, -1.2f, 2.0f, -2.0f, 3.0f, -3.0f};
        } else {
            normal_steps = {0.0f, 0.15f, -0.15f, 0.35f, -0.35f, 0.6f, -0.6f, 1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f};
            tangent_steps = {0.0f, 0.2f, -0.2f, 0.6f, -0.6f, 1.2f, -1.2f, 2.0f, -2.0f, 3.0f, -3.0f};
        }
        Point best_pos = {anchors[0].x, anchors[0].y};
        Cost best_penalty = {INF, INF};
        std::optional<float> center_max_gap = center_label_hard_max_gap(kind);
        LabelPenaltyContext penalty_ctx{kind,         occupied,  occupied_grid, node_obstacle_count,
                                        edge_obstacles, edge_grid, idx,           edge.points,
                                        bounds};

        auto evaluate_candidates = [&](Anchor anchor, const std::vector<float>& tangents,
                                       const std::vector<float>& normals, std::optional<float> max_own_gap) -> bool {
            bool evaluated = false;
            float anchor_x = anchor.x, anchor_y = anchor.y, dir_x = anchor.dx, dir_y = anchor.dy;
            float normal_x = -dir_y;
            float normal_y = dir_x;
            float step_n = std::fabs(normal_x) > std::fabs(normal_y) ? label.width + label_pad_x + step_normal_pad
                                                                     : label.height + label_pad_y + step_normal_pad;
            float step_t = std::fabs(dir_x) > std::fabs(dir_y) ? label.width + label_pad_x + step_tangent_pad
                                                               : label.height + label_pad_y + step_tangent_pad;
            float half_w = label.width * 0.5f + label_pad_x;
            float half_h = label.height * 0.5f + label_pad_y;
            float normal_extent = std::fabs(normal_x) * half_w + std::fabs(normal_y) * half_h;
            auto score_center = [&](float x, float y, float tangent_metric, float normal_metric) {
                Point center = bounds ? clamp_label_center_to_bounds({x, y}, label.width, label.height, label_pad_x,
                                                                      label_pad_y, *bounds)
                                      : Point{x, y};
                Rect rect = {center.first - label.width / 2.0f - label_pad_x,
                             center.second - label.height / 2.0f - label_pad_y, pad_w, pad_h};
                if (max_own_gap) {
                    float own_gap = polyline_rect_distance(edge.points, rect);
                    if (kind == DiagramKind::Flowchart) {
                        if (!flowchart_own_gap_allowed(own_gap, *max_own_gap)) return;
                    } else if (std::isfinite(own_gap) && own_gap > *max_own_gap) {
                        return;
                    }
                }
                evaluated = true;
                Cost penalty = label_penalties(rect, {anchor_x, anchor_y}, label.width, label.height, penalty_ctx);
                float overlap_pressure = penalty.first;
                float edge_dist = point_polyline_distance(center, edge.points);
                float edge_target = edge_target_distance(kind, label.height, label_pad_y);
                float edge_dist_weight = edge_distance_weight(kind, overlap_pressure);
                float edge_dist_penalty = (fmax_(edge_dist - edge_target, 0.0f) / edge_target) * edge_dist_weight;
                float sweep_penalty = sweep_bias(kind, tangent_metric, normal_metric);
                penalty = {penalty.first + edge_dist_penalty + sweep_penalty, penalty.second};
                if (candidate_better(penalty, best_penalty)) {
                    best_penalty = penalty;
                    best_pos = center;
                }
            };

            // Geometry-aware bands: explicit edge-clearance gaps, independent of label size.
            std::vector<float> gap_targets, tangent_focus;
            if (kind == DiagramKind::Flowchart) {
                gap_targets = {0.8f, 1.4f, 2.0f, 3.0f, 4.4f, 6.2f};
                tangent_focus = {0.0f, 0.3f, -0.3f, 0.8f, -0.8f, 1.4f, -1.4f};
            } else if (kind == DiagramKind::State) {
                gap_targets = {0.8f, 1.3f, 1.9f, 2.8f, 3.9f, 5.3f};
                tangent_focus = {0.0f, 0.12f, -0.12f, 0.35f, -0.35f, 0.8f, -0.8f};
            } else {
                gap_targets = {0.8f, 1.4f, 2.1f, 3.0f, 4.1f, 5.6f};
                tangent_focus = {0.0f, 0.2f, -0.2f, 0.6f, -0.6f, 1.2f, -1.2f};
            }
            for (float t : tangent_focus) {
                float base_x = anchor_x + dir_x * step_t * t;
                float base_y = anchor_y + dir_y * step_t * t;
                for (float gap : gap_targets) {
                    float offset = normal_extent + gap;
                    float approx_normal = offset / fmax_(step_n, 1.0f);
                    score_center(base_x + normal_x * offset, base_y + normal_y * offset, t, approx_normal);
                    score_center(base_x - normal_x * offset, base_y - normal_y * offset, t, -approx_normal);
                }
            }
            for (float t : tangents) {
                float base_x = anchor_x + dir_x * step_t * t;
                float base_y = anchor_y + dir_y * step_t * t;
                for (float n : normals)
                    score_center(base_x + normal_x * step_n * n, base_y + normal_y * step_n * n, t, n);
            }
            return evaluated;
        };
        bool evaluated = false;
        for (const Anchor& a : anchors)
            if (evaluate_candidates(a, tangent_steps, normal_steps, center_max_gap)) evaluated = true;
        if (!evaluated && center_max_gap)
            for (const Anchor& a : anchors) evaluate_candidates(a, tangent_steps, normal_steps, std::nullopt);
        if (best_penalty.first > LABEL_OVERLAP_WIDE_THRESHOLD) {
            std::vector<float> normal_wide, tangent_wide;
            if (kind == DiagramKind::Flowchart) {
                normal_wide = {0.6f, -0.6f, 1.2f, -1.2f, 2.0f, -2.0f, 3.0f, -3.0f, 4.0f, -4.0f, 5.2f, -5.2f, 6.5f, -6.5f, 0.0f};
                tangent_wide = {0.0f, 0.8f, -0.8f, 1.6f, -1.6f, 2.6f, -2.6f, 3.8f, -3.8f, 5.2f, -5.2f, 6.6f, -6.6f, 8.0f, -8.0f, 10.0f, -10.0f};
            } else if (kind == DiagramKind::State) {
                normal_wide = {0.0f, 0.45f, -0.45f, 0.5f, -0.5f, 0.55f, -0.55f, 1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f, 4.0f, -4.0f, 5.0f, -5.0f};
                tangent_wide = {0.0f, 0.12f, -0.12f, 0.35f, -0.35f, 0.8f, -0.8f, 1.6f, -1.6f, 2.4f, -2.4f, 3.2f, -3.2f, 4.2f, -4.2f, 5.4f, -5.4f};
            } else {
                normal_wide = {0.0f, 1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f, 4.0f, -4.0f, 5.0f, -5.0f};
                tangent_wide = {0.0f, 0.8f, -0.8f, 1.6f, -1.6f, 2.4f, -2.4f, 3.2f, -3.2f, 4.2f, -4.2f, 5.4f, -5.4f};
            }
            bool evaluated_wide = false;
            for (const Anchor& a : anchors)
                if (evaluate_candidates(a, tangent_wide, normal_wide, center_max_gap)) evaluated_wide = true;
            if (!evaluated_wide && center_max_gap)
                for (const Anchor& a : anchors) evaluate_candidates(a, tangent_wide, normal_wide, std::nullopt);
        }
        Point clamped_pos = bounds ? clamp_label_center_to_bounds(best_pos, label.width, label.height, label_pad_x,
                                                                  label_pad_y, *bounds)
                                   : best_pos;
        Rect rect = {clamped_pos.first - label.width / 2.0f - label_pad_x,
                     clamped_pos.second - label.height / 2.0f - label_pad_y, pad_w, pad_h};
        Rect occupied_rect = kind == DiagramKind::Flowchart ? inflate_rect(rect, FLOWCHART_LABEL_CLEARANCE_PAD) : rect;
        occupied_grid.insert(occupied.size(), occupied_rect);
        occupied.push_back(occupied_rect);
        edges[idx].label_anchor = clamped_pos;
    }

    if (kind == DiagramKind::Flowchart)
        deoverlap_flowchart_center_labels(edges, nodes, subgraphs, bounds, theme, label_pad_x, label_pad_y,
                                          fixed_center_indices);

    tighten_center_label_gaps(edges, nodes, subgraphs, bounds, kind, theme, label_pad_x, label_pad_y,
                              fixed_center_indices);
    enforce_center_label_attachment_caps(edges, nodes, subgraphs, bounds, kind, theme, label_pad_x, label_pad_y,
                                         fixed_center_indices);
    if (kind == DiagramKind::Flowchart) {
        nudge_flowchart_labels_clear_of_own_paths(edges, bounds);
        CenterLabelOverlapScore before_score = center_label_overlap_score(edges, label_pad_x, label_pad_y);
        std::vector<EdgeLayout> candidate_edges = edges;
        deoverlap_flowchart_center_labels(candidate_edges, nodes, subgraphs, bounds, theme, label_pad_x, label_pad_y,
                                          fixed_center_indices);
        CenterLabelOverlapScore after_score = center_label_overlap_score(candidate_edges, label_pad_x, label_pad_y);
        if (after_score.improved_by(before_score)) edges = candidate_edges;
        // The last de-overlap can trade own-edge clearance for less label overlap: finish with
        // the route-clearance nudge so labels do not sit on their carrying paths.
        nudge_flowchart_labels_clear_of_own_paths(edges, bounds);
    }
}

// ---- endpoint labels (start / end of an edge) -----------------------------------------------

struct EndpointLabelAvoidContext {
    DiagramKind kind;
    float offset;
    const std::vector<Rect>& occupied;
    const ObstacleGrid& occupied_grid;
    size_t node_obstacle_count;
    const std::vector<EdgeObstacle>& edge_obstacles;
    const ObstacleGrid& edge_grid;
    Bounds bounds;
};

std::optional<Point> edge_endpoint_label_position_with_avoid(const EdgeLayout& edge, size_t edge_idx, bool start,
                                                             float label_w, float label_h, float pad_x, float pad_y,
                                                             const EndpointLabelAvoidContext& ctx) {
    if (edge.points.size() < 2) return std::nullopt;
    Point p0, p1;
    if (start) { p0 = edge.points[0]; p1 = edge.points[1]; }
    else { p0 = edge.points[edge.points.size() - 1]; p1 = edge.points[edge.points.size() - 2]; }
    float dx = p1.first - p0.first, dy = p1.second - p0.second;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len <= std::numeric_limits<float>::epsilon()) return std::nullopt;
    DiagramKind kind = ctx.kind;
    float offset = ctx.offset;
    float dir_x = dx / len, dir_y = dy / len;
    float perp_x = -dir_y, perp_y = dir_x;
    float anchor_along_factor;
    switch (kind) {
        case DiagramKind::Class: anchor_along_factor = 0.55f; break;
        case DiagramKind::Flowchart: anchor_along_factor = 1.4f; break;
        case DiagramKind::Requirement: anchor_along_factor = 1.1f; break;
        default: anchor_along_factor = 1.0f; break;
    }
    float anchor_x = p0.first + dir_x * offset * anchor_along_factor;
    float anchor_y = p0.second + dir_y * offset * anchor_along_factor;
    std::vector<float> along_steps, perp_steps;
    if (kind == DiagramKind::Class) {
        along_steps = {0.0f, 0.15f, -0.15f, 0.35f, -0.35f, 0.55f, -0.55f};
        perp_steps = {0.0f, 0.35f, -0.35f, 0.7f, -0.7f, 1.05f, -1.05f, 1.5f, -1.5f};
    } else {
        along_steps = {0.0f, 0.8f, -0.8f, 1.6f, -1.6f};
        perp_steps = {1.0f, -1.0f, 1.7f, -1.7f, 2.4f, -2.4f, 3.2f, -3.2f, 3.9f, -3.9f, 4.6f, -4.6f};
    }
    Point best_pos = {anchor_x, anchor_y};
    Cost best_penalty = {INF, INF};
    LabelPenaltyContext penalty_ctx{kind,           ctx.occupied, ctx.occupied_grid, ctx.node_obstacle_count,
                                    ctx.edge_obstacles, ctx.edge_grid, edge_idx,        edge.points,
                                    ctx.bounds};
    for (float along_step : along_steps) {
        float base_x = p0.first + dir_x * offset * (1.4f + along_step);
        float base_y = p0.second + dir_y * offset * (1.4f + along_step);
        for (float step : perp_steps) {
            float x = base_x + perp_x * offset * step;
            float y = base_y + perp_y * offset * step;
            Rect rect = {x - label_w / 2.0f - pad_x, y - label_h / 2.0f - pad_y, label_w + pad_x * 2.0f,
                         label_h + pad_y * 2.0f};
            Cost penalty = label_penalties(rect, {anchor_x, anchor_y}, label_w, label_h, penalty_ctx);
            // Keep endpoint labels near the endpoint along their carrying edge.
            float along = (x - p0.first) * dir_x + (y - p0.second) * dir_y;
            float soft_max, under_weight, over_weight;
            switch (kind) {
                case DiagramKind::Class: soft_max = offset * 0.9f; under_weight = 0.45f; over_weight = 1.2f; break;
                case DiagramKind::State: soft_max = offset * 1.3f; under_weight = 0.28f; over_weight = 0.7f; break;
                case DiagramKind::Flowchart: soft_max = offset * 2.6f; under_weight = 0.14f; over_weight = 0.28f; break;
                default: soft_max = offset * 1.7f; under_weight = 0.22f; over_weight = 0.45f; break;
            }
            float under = fmax_(0.0f - along, 0.0f);
            float over = fmax_(along - soft_max, 0.0f);
            if (under > 0.0f || over > 0.0f) {
                float endpoint_drift_penalty =
                    (under * under * under_weight + over * over * over_weight) / fmax_(label_w + label_h + 1.0f, 1.0f);
                penalty.first += endpoint_drift_penalty;
            }
            if (candidate_better(penalty, best_penalty)) {
                best_penalty = penalty;
                best_pos = {x, y};
            }
        }
    }
    if (ctx.bounds) return clamp_label_center_to_bounds(best_pos, label_w, label_h, pad_x, pad_y, *ctx.bounds);
    return best_pos;
}

void resolve_endpoint_labels(std::vector<EdgeLayout>& edges, const std::map<std::string, NodeLayout>& nodes,
                             const std::vector<SubgraphLayout>& subgraphs, Bounds bounds, DiagramKind kind,
                             const Theme& theme, const LayoutConfig& config) {
    bool has_endpoint_labels = false;
    for (const EdgeLayout& e : edges)
        if (e.start_label || e.end_label) has_endpoint_labels = true;
    if (!has_endpoint_labels) return;

    auto [center_pad_x, center_pad_y] = edge_label_padding(kind, config);
    float node_obstacle_pad = kind == DiagramKind::Class ? fmax_(theme.font_size * 0.12f, 1.5f)
                                                         : fmax_(theme.font_size * 0.45f, fmax_(center_pad_x, center_pad_y));
    float edge_obstacle_pad = fmax_(theme.font_size * 0.35f, center_pad_y);
    float subgraph_label_pad = fmax_(theme.font_size * 0.35f, 3.0f);
    auto [endpoint_pad_x, endpoint_pad_y] = endpoint_label_padding(kind);

    std::vector<EdgeObstacle> edge_obstacles = build_edge_obstacles(edges, edge_obstacle_pad);
    ObstacleGrid endpoint_edge_grid(48.0f, obstacle_rects(edge_obstacles));

    float endpoint_node_obstacle_pad = kind == DiagramKind::Class   ? node_obstacle_pad * 0.4f
                                       : kind == DiagramKind::State ? node_obstacle_pad * 0.65f
                                                                    : node_obstacle_pad;
    std::vector<Rect> endpoint_occupied =
        build_label_obstacles(nodes, subgraphs, kind, theme, endpoint_node_obstacle_pad, subgraph_label_pad);
    size_t endpoint_node_obstacle_count = endpoint_occupied.size();
    for (const EdgeLayout& edge : edges) {
        if (edge.label && edge.label_anchor) {
            Rect rect = {edge.label_anchor->first - edge.label->width / 2.0f - center_pad_x,
                         edge.label_anchor->second - edge.label->height / 2.0f - center_pad_y,
                         edge.label->width + 2.0f * center_pad_x, edge.label->height + 2.0f * center_pad_y};
            endpoint_occupied.push_back(kind == DiagramKind::Flowchart ? inflate_rect(rect, FLOWCHART_LABEL_CLEARANCE_PAD) : rect);
        }
    }

    float end_label_offset = kind == DiagramKind::Class       ? fmax_(theme.font_size * 0.18f, 2.8f)
                             : kind == DiagramKind::Flowchart ? fmax_(theme.font_size * 0.75f, 9.0f)
                                                              : fmax_(theme.font_size * 0.6f, 8.0f);
    float state_font_size = kind == DiagramKind::State ? theme.font_size * 0.85f : theme.font_size;
    float endpoint_label_scale = kind == DiagramKind::State ? fmin_(state_font_size / theme.font_size, 1.0f) : 1.0f;

    ObstacleGrid endpoint_grid(48.0f, endpoint_occupied);

    for (size_t idx = 0; idx < edges.size(); ++idx) {
        for (int which = 0; which < 2; ++which) {
            bool start = which == 0;
            std::optional<TextBlock> label = start ? edges[idx].start_label : edges[idx].end_label;
            if (!label) continue;
            float label_w = label->width * endpoint_label_scale;
            float label_h = label->height * endpoint_label_scale;
            EndpointLabelAvoidContext ctx{kind, end_label_offset, endpoint_occupied, endpoint_grid,
                                          endpoint_node_obstacle_count, edge_obstacles, endpoint_edge_grid, bounds};
            auto pos = edge_endpoint_label_position_with_avoid(edges[idx], idx, start, label_w, label_h,
                                                               endpoint_pad_x, endpoint_pad_y, ctx);
            if (!pos) continue;
            (start ? edges[idx].start_label_anchor : edges[idx].end_label_anchor) = *pos;
            Rect rect = {pos->first - label_w / 2.0f - endpoint_pad_x, pos->second - label_h / 2.0f - endpoint_pad_y,
                         label_w + endpoint_pad_x * 2.0f, label_h + endpoint_pad_y * 2.0f};
            Rect occupied_rect = kind == DiagramKind::Flowchart ? inflate_rect(rect, FLOWCHART_LABEL_CLEARANCE_PAD) : rect;
            endpoint_grid.insert(endpoint_occupied.size(), occupied_rect);
            endpoint_occupied.push_back(occupied_rect);
        }
    }
}

}  // namespace

// Resolve all edge label positions using collision avoidance (entry point).
void resolve_all_label_positions(Layout& layout, const Theme& theme, const LayoutConfig& config) {
    if (layout.kind == DiagramKind::Sequence || layout.kind == DiagramKind::ZenUML) {
        resolve_sequence_label_positions(layout, theme);
        return;
    }

    Bounds bounds = std::make_pair(layout.width, layout.height);

    // Flowchart centre labels may stack outside the initial routing rectangle; the bounds
    // finalisation pass after label placement grows the layout, so do not clamp them here.
    Bounds center_bounds = layout.kind == DiagramKind::Flowchart ? Bounds{} : bounds;
    std::vector<EdgeLayout> edges = std::move(layout.edges);
    resolve_center_labels(edges, layout.nodes, layout.subgraphs, center_bounds, layout.kind, theme, config);
    if (layout.kind == DiagramKind::Flowchart) move_flowchart_labels_off_own_edges(edges);
    resolve_endpoint_labels(edges, layout.nodes, layout.subgraphs, bounds, layout.kind, theme, config);
    layout.edges = std::move(edges);
}

}  // namespace mermaid
