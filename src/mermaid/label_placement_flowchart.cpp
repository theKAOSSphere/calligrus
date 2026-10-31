// Mermaid port: flowchart-only centre-label passes of layout/label_placement.rs.
// After the first greedy placement, labels of a flowchart get a second, global treatment:
//  1. every label builds a list of candidate centres and costs them (build_flowchart_candidate_set),
//  2. conflict components (labels near each other) are solved with a beam search over those
//     candidates (apply_flowchart_component_assignment),
//  3. an iterative refinement moves the most conflicted labels, then a pair-separation pass.
#include "label_placement_internal.h"

namespace mermaid::lp {

namespace {

struct FlowchartCenterCandidate {
    Point center{0.0f, 0.0f};
    Rect rect{0.0f, 0.0f, 0.0f, 0.0f};
    Cost cost{0.0f, 0.0f};
    float own_gap = 0, center_dist = 0, center_target = 0, center_soft_max = 0, center_hard_max = 0;
    unsigned fixed_overlap_count = 0;
    float fixed_overlap_area = 0;
    float s_norm = 0, d_signed = 0;
};

// One partial solution of the beam search: which candidate each label got so far.
struct FlowchartBeamState {
    std::vector<std::pair<size_t, size_t>> assignments;  // (entry index, candidate index)
    std::vector<Rect> rects;
    float primary = 0, drift = 0;
};

Rect flowchart_center_label_obstacle_rect(Point center, float label_w, float label_h, float label_pad_x,
                                          float label_pad_y) {
    return inflate_rect(flowchart_center_label_rect(center, label_w, label_h, label_pad_x, label_pad_y),
                        FLOWCHART_LABEL_CLEARANCE_PAD);
}

// (target, soft max, hard max) distance of a label centre from its edge.
struct DistLimits {
    float target, soft_max, hard_max;
};
DistLimits flowchart_center_distance_limits(float label_h, float label_pad_y) {
    float target = fmax_(edge_target_distance(DiagramKind::Flowchart, label_h, label_pad_y), 2.0f);
    float soft_add = clamp_(label_h * 0.18f + label_pad_y * 0.5f + 1.8f, 3.0f, 6.0f);
    float hard_add = clamp_(label_h * 0.26f + label_pad_y * 0.8f + 3.2f, 5.0f, 10.0f);
    float soft_max = target + soft_add;
    float hard_max = soft_max + hard_add;
    return {target, soft_max, hard_max};
}

// Position of `center` relative to the edge: (progress 0..1 along the path, signed distance).
std::optional<std::pair<float, float>> edge_relative_pose(const std::vector<Point>& points, Point center) {
    if (points.size() < 2) return std::nullopt;
    std::vector<float> seg_lengths;
    float total_len = 0.0f;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        float dx = points[i + 1].first - points[i].first;
        float dy = points[i + 1].second - points[i].second;
        float len = std::sqrt(dx * dx + dy * dy);
        seg_lengths.push_back(len);
        total_len += len;
    }
    if (total_len <= 1e-6f) return std::nullopt;

    float best_dist2 = INF;
    Point best_proj = center;
    float best_t = 0.0f, best_prefix = 0.0f, best_seg_len = 1.0f, best_dx = 1.0f, best_dy = 0.0f;
    float prefix = 0.0f;
    for (size_t seg_idx = 0; seg_idx + 1 < points.size(); ++seg_idx) {
        Point p1 = points[seg_idx], p2 = points[seg_idx + 1];
        float dx = p2.first - p1.first, dy = p2.second - p1.second;
        float seg_len = seg_lengths[seg_idx];
        if (seg_len <= 1e-6f) continue;
        float seg_len2 = seg_len * seg_len;
        float t = ((center.first - p1.first) * dx + (center.second - p1.second) * dy) / seg_len2;
        float t_clamped = clamp_(t, 0.0f, 1.0f);
        Point proj{p1.first + dx * t_clamped, p1.second + dy * t_clamped};
        float ddx = center.first - proj.first, ddy = center.second - proj.second;
        float dist2 = ddx * ddx + ddy * ddy;
        if (dist2 < best_dist2) {
            best_dist2 = dist2;
            best_proj = proj;
            best_t = t_clamped;
            best_prefix = prefix;
            best_seg_len = seg_len;
            best_dx = dx / seg_len;
            best_dy = dy / seg_len;
        }
        prefix += seg_len;
    }
    float s = clamp_((best_prefix + best_t * best_seg_len) / total_len, 0.0f, 1.0f);
    float nx = -best_dy, ny = best_dx;
    float d = (center.first - best_proj.first) * nx + (center.second - best_proj.second) * ny;
    return std::make_pair(s, d);
}

std::optional<Point> edge_nearest_segment_tangent(const std::vector<Point>& points, Point center) {
    if (points.size() < 2) return std::nullopt;
    float best_dist2 = INF;
    std::optional<Point> best_tangent;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        Point p1 = points[i], p2 = points[i + 1];
        float dx = p2.first - p1.first, dy = p2.second - p1.second;
        float seg_len2 = dx * dx + dy * dy;
        if (seg_len2 <= 1e-6f) continue;
        float t = ((center.first - p1.first) * dx + (center.second - p1.second) * dy) / seg_len2;
        float t_clamped = clamp_(t, 0.0f, 1.0f);
        float proj_x = p1.first + dx * t_clamped;
        float proj_y = p1.second + dy * t_clamped;
        float dist2 = (center.first - proj_x) * (center.first - proj_x) +
                      (center.second - proj_y) * (center.second - proj_y);
        if (dist2 < best_dist2) {
            best_dist2 = dist2;
            float seg_len = fmax_(std::sqrt(seg_len2), 1e-3f);
            best_tangent = Point{dx / seg_len, dy / seg_len};
        }
    }
    return best_tangent;
}

std::optional<float> flowchart_pose_center_target(const std::vector<Point>& points, Point center, float label_w,
                                                  float label_h, float label_pad_x, float label_pad_y) {
    auto tangent = edge_nearest_segment_tangent(points, center);
    if (!tangent) return std::nullopt;
    float tx = tangent->first, ty = tangent->second;
    float nx = -ty, ny = tx;
    float half_w = label_w * 0.5f + label_pad_x;
    float half_h = label_h * 0.5f + label_pad_y;
    float oriented_extent = std::fabs(nx) * half_w + std::fabs(ny) * half_h;
    float base_extent = half_h;
    float anisotropy_bonus = fmax_(oriented_extent - base_extent, 0.0f) * 0.45f;
    return edge_target_distance(DiagramKind::Flowchart, label_h, label_pad_y) + anisotropy_bonus;
}

float flowchart_pose_center_hard_max(const std::vector<Point>& points, Point center, float label_w, float label_h,
                                     float label_pad_x, float label_pad_y) {
    DistLimits base = flowchart_center_distance_limits(label_h, label_pad_y);
    float soft_delta = fmax_(base.soft_max - base.target, 0.0f);
    float hard_delta = fmax_(base.hard_max - base.soft_max, 0.0f);
    float target = flowchart_pose_center_target(points, center, label_w, label_h, label_pad_x, label_pad_y)
                       .value_or(base.target);
    return target + soft_delta + hard_delta;
}

std::pair<float, float> flowchart_path_span(const std::vector<Point>& points) {
    if (points.empty()) return {0.0f, 0.0f};
    float min_x = INF, min_y = INF, max_x = -INF, max_y = -INF;
    for (const Point& p : points) {
        min_x = fmin_(min_x, p.first);
        min_y = fmin_(min_y, p.second);
        max_x = fmax_(max_x, p.first);
        max_y = fmax_(max_y, p.second);
    }
    return {fmax_(max_x - min_x, 0.0f), fmax_(max_y - min_y, 0.0f)};
}

CenterLabelOverlapScore flowchart_entry_label_overlap_score(const std::vector<FlowchartCenterLabelEntry>& entries,
                                                            float label_pad_x, float label_pad_y) {
    std::vector<Rect> rects;
    for (const auto& e : entries)
        rects.push_back(flowchart_center_label_rect(e.current_center, e.label_w, e.label_h, label_pad_x, label_pad_y));
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

// Extra candidates placed just left/right/above/below the rectangle that blocks this label.
std::vector<Point> flowchart_pair_separation_candidates(const FlowchartCenterLabelEntry& entry, Rect blocking_rect,
                                                        float label_pad_x, float label_pad_y, Bounds bounds) {
    std::vector<Point> candidates;
    float obstacle_half_w = entry.label_w * 0.5f + label_pad_x + FLOWCHART_LABEL_CLEARANCE_PAD;
    float obstacle_half_h = entry.label_h * 0.5f + label_pad_y + FLOWCHART_LABEL_CLEARANCE_PAD;
    float gap = 2.0f;
    float left_x = blocking_rect.x - gap - obstacle_half_w;
    float right_x = blocking_rect.x + blocking_rect.w + gap + obstacle_half_w;
    float above_y = blocking_rect.y - gap - obstacle_half_h;
    float below_y = blocking_rect.y + blocking_rect.h + gap + obstacle_half_h;
    float x_refs[2] = {entry.current_center.first, entry.initial_center.first};
    float y_refs[2] = {entry.current_center.second, entry.initial_center.second};

    auto push_candidate = [&](Point center) {
        if (bounds)
            center = clamp_label_center_to_bounds(center, entry.label_w, entry.label_h, label_pad_x, label_pad_y,
                                                  *bounds);
        push_center_unique(candidates, center);
    };
    for (float y : y_refs) {
        push_candidate({left_x, y});
        push_candidate({right_x, y});
    }
    for (float x : x_refs) {
        push_candidate({x, above_y});
        push_candidate({x, below_y});
    }
    return candidates;
}

// The big candidate grid around the edge for one label (anchors x normal steps x tangent steps).
std::vector<Point> flowchart_center_label_candidates(const EdgeLayout& edge, Point initial_center, float label_w,
                                                     float label_h, float label_pad_x, float label_pad_y,
                                                     float step_normal_pad, float step_tangent_pad, Bounds bounds) {
    std::vector<Point> candidates;
    auto push_candidate = [&](Point center) {
        if (bounds)
            center = clamp_label_center_to_bounds(center, label_w, label_h, label_pad_x, label_pad_y, *bounds);
        push_center_unique(candidates, center);
    };
    push_candidate(initial_center);

    std::vector<Anchor> anchors;
    if (auto a = edge_label_anchor_from_point(edge, initial_center)) push_anchor_unique(anchors, *a);
    for (float frac : LABEL_ANCHOR_FRACTIONS)
        if (auto a = edge_label_anchor_at_fraction(edge, frac)) push_anchor_unique(anchors, *a);
    for (const Anchor& a : edge_segment_anchors(edge, LABEL_EXTRA_SEGMENT_ANCHORS)) push_anchor_unique(anchors, a);
    for (const Anchor& a : edge_terminal_segment_anchors(edge, 2)) push_anchor_unique(anchors, a);
    if (anchors.empty()) anchors.push_back(edge_label_anchor(edge));
    else push_anchor_unique(anchors, edge_label_anchor(edge));

    if (auto pose = edge_relative_pose(edge.points, initial_center)) {
        float initial_s = pose->first;
        // Keep the anchors closest (in progress along the edge) to where the label started.
        std::vector<std::pair<Anchor, float>> scored_anchors;
        for (const Anchor& anchor : anchors) {
            auto p = edge_relative_pose(edge.points, {anchor.x, anchor.y});
            float s = p ? p->first : initial_s;
            scored_anchors.push_back({anchor, std::fabs(s - initial_s)});
        }
        std::stable_sort(scored_anchors.begin(), scored_anchors.end(),
                         [](const auto& a, const auto& b) { return cmp_f(a.second, b.second) < 0; });
        std::vector<Anchor> filtered;
        for (const auto& [anchor, s_delta] : scored_anchors) {
            if (s_delta <= 0.18f || filtered.size() < 3) filtered.push_back(anchor);
            if (filtered.size() >= 6) break;
        }
        if (!filtered.empty()) anchors = filtered;
    }

    static const float normal_steps[] = {0.45f, -0.45f, 0.8f, -0.8f, 1.2f, -1.2f, 0.25f, -0.25f, 1.8f, -1.8f, 2.5f,
                                         -2.5f, 3.4f,  -3.4f, 4.4f, -4.4f, 5.6f, -5.6f, 7.0f,  -7.0f, 0.0f};
    static const float tangent_steps[] = {0.0f, 0.22f, -0.22f, 0.55f, -0.55f, 1.0f, -1.0f,
                                          1.6f, -1.6f, 2.3f,   -2.3f, 2.8f,   -2.8f};
    for (const Anchor& an : anchors) {
        float normal_x = -an.dy, normal_y = an.dx;
        float step_n = std::fabs(normal_x) > std::fabs(normal_y) ? label_w + label_pad_x + step_normal_pad
                                                                 : label_h + label_pad_y + step_normal_pad;
        float step_t = std::fabs(an.dx) > std::fabs(an.dy) ? label_w + label_pad_x + step_tangent_pad
                                                           : label_h + label_pad_y + step_tangent_pad;
        for (float t : tangent_steps) {
            float base_x = an.x + an.dx * step_t * t;
            float base_y = an.y + an.dy * step_t * t;
            for (float n : normal_steps)
                push_candidate({base_x + normal_x * step_n * n, base_y + normal_y * step_n * n});
        }
    }
    return candidates;
}

// Cost of putting `entry`'s label at `center`, given the other labels and fixed obstacles.
Cost flowchart_center_label_refine_cost(const FlowchartCenterLabelEntry& entry, Point center, float label_pad_x,
                                        float label_pad_y, const std::vector<Rect>& others,
                                        const std::vector<Rect>& fixed_obstacles,
                                        const std::vector<EdgeObstacle>& edge_obstacles,
                                        const ObstacleGrid& edge_grid) {
    Rect core_rect = flowchart_center_label_rect(center, entry.label_w, entry.label_h, label_pad_x, label_pad_y);
    Rect obstacle_rect =
        flowchart_center_label_obstacle_rect(center, entry.label_w, entry.label_h, label_pad_x, label_pad_y);
    float area = fmax_(entry.label_w * entry.label_h, 1.0f);

    float overlap_area_sum = 0.0f;
    unsigned overlap_count = 0;
    float near_overlap_gap_sum = 0.0f;
    for (const Rect& other : others) {
        float ov = overlap_area(obstacle_rect, other);
        if (ov > 0.0f) {
            overlap_area_sum += ov;
            overlap_count += 1;
            continue;
        }
        float gap = rect_gap(obstacle_rect, other);
        if (gap < FLOWCHART_LABEL_SOFT_GAP) near_overlap_gap_sum += FLOWCHART_LABEL_SOFT_GAP - gap;
    }

    float fixed_overlap_area = 0.0f;
    unsigned fixed_overlap_count = 0;
    float fixed_near_gap_sum = 0.0f;
    for (const Rect& obstacle : fixed_obstacles) {
        float ov = overlap_area(obstacle_rect, obstacle);
        if (ov > 0.0f) {
            fixed_overlap_area += ov;
            fixed_overlap_count += 1;
            continue;
        }
        float gap = rect_gap(obstacle_rect, obstacle);
        if (gap < FLOWCHART_LABEL_SOFT_GAP) fixed_near_gap_sum += FLOWCHART_LABEL_SOFT_GAP - gap;
    }

    float own_edge_dist = polyline_rect_distance(entry.edge_points, core_rect);
    float own_edge_penalty = 0.0f;
    if (auto pose = edge_relative_pose(entry.edge_points, center)) {
        auto [s_norm, d_signed] = *pose;
        float s_drift = std::fabs(s_norm - entry.initial_s_norm);
        own_edge_penalty += s_drift * s_drift * 2.2f;
        float normal_scale = fmax_(entry.label_h + 2.0f * label_pad_y, 1.0f);
        float normal_drift = std::fabs(d_signed - entry.initial_d_signed) / normal_scale;
        own_edge_penalty += normal_drift * normal_drift * 1.6f;
    }
    if (std::isfinite(own_edge_dist)) {
        float target_gap = fmax_(OWN_EDGE_GAP_TARGET_FLOWCHART, 1e-3f);
        if (own_edge_dist < target_gap) {
            float shortage = (target_gap - own_edge_dist) / target_gap;
            own_edge_penalty += shortage * shortage * 7.8f;
            if (own_edge_dist < 1.0f) {
                float under = 1.0f - own_edge_dist;
                own_edge_penalty += under * under * 8.0f;
            }
        } else {
            float excess = (own_edge_dist - target_gap) / target_gap;
            own_edge_penalty += excess * excess * 1.35f;
            if (excess > 2.0f) own_edge_penalty += (excess - 2.0f) * 2.8f;
        }
        if (own_edge_dist > FLOWCHART_OWN_EDGE_SOFT_MAX_GAP) {
            float over = own_edge_dist - FLOWCHART_OWN_EDGE_SOFT_MAX_GAP;
            own_edge_penalty += over * over * FLOWCHART_OWN_EDGE_SOFT_MAX_GAP_WEIGHT;
        }
        if (own_edge_dist > FLOWCHART_OWN_EDGE_HARD_MAX_GAP) {
            float over = own_edge_dist - FLOWCHART_OWN_EDGE_HARD_MAX_GAP;
            own_edge_penalty += over * FLOWCHART_OWN_EDGE_HARD_MAX_GAP_WEIGHT;
        }
        if (own_edge_dist <= 0.35f) own_edge_penalty += 80.0f;
    }
    float foreign_edge_overlap_area = 0.0f;
    bool foreign_edge_touch = false;
    float foreign_edge_near_gap_sum = 0.0f;
    for (size_t edge_obs_idx : edge_grid.query(obstacle_rect)) {
        const EdgeObstacle& eo = edge_obstacles[edge_obs_idx];
        if (eo.edge == entry.edge_idx) continue;
        float ov = overlap_area(obstacle_rect, eo.rect);
        if (ov > 0.0f) {
            foreign_edge_overlap_area += ov;
            foreign_edge_touch = true;
            continue;
        }
        float gap = rect_gap(obstacle_rect, eo.rect);
        if (gap < FLOWCHART_LABEL_SOFT_GAP) foreign_edge_near_gap_sum += FLOWCHART_LABEL_SOFT_GAP - gap;
    }
    float foreign_edge_penalty = (foreign_edge_overlap_area / area) * 48.0f + (foreign_edge_touch ? 130.0f : 0.0f) +
                                 foreign_edge_near_gap_sum * 6.5f;
    float edge_center_dist = point_polyline_distance(center, entry.edge_points);
    DistLimits base = flowchart_center_distance_limits(entry.label_h, label_pad_y);
    float edge_target = flowchart_pose_center_target(entry.edge_points, center, entry.label_w, entry.label_h,
                                                     label_pad_x, label_pad_y)
                            .value_or(base.target);
    float center_soft_max = edge_target + fmax_(base.soft_max - base.target, 0.0f);
    float center_hard_max = center_soft_max + fmax_(base.hard_max - base.soft_max, 0.0f);
    float edge_center_penalty = 0.0f;
    if (edge_center_dist < edge_target) {
        float shortage = (edge_target - edge_center_dist) / fmax_(edge_target, 1e-3f);
        edge_center_penalty += shortage * shortage * 2.4f;
    }
    if (edge_center_dist > edge_target) {
        float excess = (edge_center_dist - edge_target) / fmax_(edge_target, 1e-3f);
        edge_center_penalty += excess * excess * 1.3f;
    }
    if (edge_center_dist > center_soft_max) {
        float over = edge_center_dist - center_soft_max;
        edge_center_penalty += over * over * 2.8f;
    }
    if (edge_center_dist > center_hard_max) {
        float over = edge_center_dist - center_hard_max;
        edge_center_penalty += over * 20.0f;
    }
    if (auto pose = edge_relative_pose(entry.edge_points, center)) {
        auto [s_norm, d_signed] = *pose;
        float path_len = fmax_(polyline_path_length(entry.edge_points), 1.0f);
        float tangent_shift_px = std::fabs(s_norm - entry.initial_s_norm) * path_len;
        float tangent_soft_px =
            fmin_(clamp_(entry.label_w * 0.95f + entry.label_h * 0.35f + 8.0f, 10.0f, 28.0f), path_len * 0.28f + 2.0f);
        float tangent_hard_px = fmax_(tangent_soft_px * 2.2f, tangent_soft_px + 10.0f);
        if (tangent_shift_px > tangent_soft_px) {
            float over = tangent_shift_px - tangent_soft_px;
            float q = over / fmax_(tangent_soft_px, 1.0f);
            edge_center_penalty += (q * q) * 12.0f;  // powi(2)
        }
        if (tangent_shift_px > tangent_hard_px) {
            float over = tangent_shift_px - tangent_hard_px;
            edge_center_penalty += over * 0.45f;
        }
        float d_shift = std::fabs(d_signed - entry.initial_d_signed);
        edge_center_penalty += d_shift * 0.03f;
    }
    float primary = static_cast<float>(fixed_overlap_count) * 130.0f + (fixed_overlap_area / area) * 48.0f +
                    fixed_near_gap_sum * 6.0f + static_cast<float>(overlap_count) * 115.0f +
                    (overlap_area_sum / area) * 42.0f + near_overlap_gap_sum * 5.0f + own_edge_penalty +
                    foreign_edge_penalty + edge_center_penalty;
    float dx = center.first - entry.initial_center.first;
    float dy = center.second - entry.initial_center.second;
    float drift = std::sqrt(dx * dx + dy * dy) / (entry.label_w + entry.label_h + 1.0f);
    return {primary, drift};
}

// Scored candidate centres for one label, best first (at most 220).
std::vector<FlowchartCenterCandidate> build_flowchart_candidate_set(const FlowchartCenterLabelEntry& entry,
                                                                   float label_pad_x, float label_pad_y,
                                                                   const std::vector<Rect>& fixed_obstacles,
                                                                   const std::vector<EdgeObstacle>& edge_obstacles,
                                                                   const ObstacleGrid& edge_grid) {
    std::vector<Point> centers = entry.candidates;
    push_center_unique(centers, entry.current_center);
    push_center_unique(centers, entry.initial_center);
    DistLimits base = flowchart_center_distance_limits(entry.label_h, label_pad_y);
    float center_soft_delta = fmax_(base.soft_max - base.target, 0.0f);
    float center_hard_delta = fmax_(base.hard_max - base.soft_max, 0.0f);

    std::vector<FlowchartCenterCandidate> scored;
    for (const Point& center : centers) {
        Rect core_rect = flowchart_center_label_rect(center, entry.label_w, entry.label_h, label_pad_x, label_pad_y);
        Rect obstacle_rect =
            flowchart_center_label_obstacle_rect(center, entry.label_w, entry.label_h, label_pad_x, label_pad_y);
        float own_gap = polyline_rect_distance(entry.edge_points, core_rect);
        if (std::isfinite(own_gap) && own_gap > FLOWCHART_OWN_EDGE_HARD_MAX_GAP + 4.0f) continue;
        float center_dist = point_polyline_distance(center, entry.edge_points);
        float center_target = flowchart_pose_center_target(entry.edge_points, center, entry.label_w, entry.label_h,
                                                           label_pad_x, label_pad_y)
                                  .value_or(base.target);
        float center_soft_max = center_target + center_soft_delta;
        float center_hard_max = center_soft_max + center_hard_delta;
        auto [fixed_overlap_count, fixed_overlap_area] =
            overlap_stats(obstacle_rect, fixed_obstacles, LABEL_OVERLAP_WIDE_THRESHOLD);
        Cost cost = flowchart_center_label_refine_cost(entry, center, label_pad_x, label_pad_y, {}, fixed_obstacles,
                                                       edge_obstacles, edge_grid);
        if (std::isfinite(own_gap) && own_gap > 4.0f) {
            float over = own_gap - 4.0f;
            cost.first += over * over * 2.0f;
        }
        if (std::isfinite(center_dist)) {
            if (center_dist > center_soft_max) {
                float over = center_dist - center_soft_max;
                cost.first += over * over * 2.2f;
            }
            if (center_dist > center_hard_max) {
                float over = center_dist - center_hard_max;
                cost.first += over * 18.0f;
            }
        }
        cost.first += static_cast<float>(fixed_overlap_count) * 1.5f + fixed_overlap_area * 0.002f;
        auto pose = edge_relative_pose(entry.edge_points, center).value_or(std::make_pair(0.5f, 0.0f));
        float s_norm = pose.first, d_signed = pose.second;
        float s_shift = std::fabs(s_norm - entry.initial_s_norm);
        float d_shift = std::fabs(d_signed - entry.initial_d_signed);
        cost.first += s_shift * 0.045f + d_shift * 0.012f;
        FlowchartCenterCandidate c;
        c.center = center;
        c.rect = obstacle_rect;
        c.cost = cost;
        c.own_gap = own_gap;
        c.center_dist = center_dist;
        c.center_target = center_target;
        c.center_soft_max = center_soft_max;
        c.center_hard_max = center_hard_max;
        c.fixed_overlap_count = fixed_overlap_count;
        c.fixed_overlap_area = fixed_overlap_area;
        c.s_norm = s_norm;
        c.d_signed = d_signed;
        scored.push_back(c);
    }
    std::stable_sort(scored.begin(), scored.end(), [](const FlowchartCenterCandidate& a, const FlowchartCenterCandidate& b) {
        int c = cmp_f(a.cost.first, b.cost.first);
        if (c != 0) return c < 0;
        c = cmp_f(std::fabs(a.own_gap - OWN_EDGE_GAP_TARGET_FLOWCHART), std::fabs(b.own_gap - OWN_EDGE_GAP_TARGET_FLOWCHART));
        if (c != 0) return c < 0;
        c = cmp_f(a.cost.second, b.cost.second);
        if (c != 0) return c < 0;
        c = cmp_f(a.center.first, b.center.first);
        if (c != 0) return c < 0;
        return cmp_f(a.center.second, b.center.second) < 0;
    });
    if (scored.size() > 220) scored.resize(220);
    return scored;
}

// Groups of labels that overlap or lie within 24px of each other (connected components).
std::vector<std::vector<size_t>> flowchart_entry_components(const std::vector<FlowchartCenterLabelEntry>& entries,
                                                            float label_pad_x, float label_pad_y) {
    if (entries.empty()) return {};
    std::vector<Rect> rects;
    for (const auto& e : entries)
        rects.push_back(
            flowchart_center_label_obstacle_rect(e.current_center, e.label_w, e.label_h, label_pad_x, label_pad_y));

    std::vector<std::vector<size_t>> neighbors(entries.size());
    for (size_t i = 0; i < entries.size(); ++i)
        for (size_t j = i + 1; j < entries.size(); ++j) {
            bool overlap = overlap_area(rects[i], rects[j]) > LABEL_OVERLAP_WIDE_THRESHOLD;
            bool near = rect_gap(rects[i], rects[j]) <= 24.0f;
            if (!overlap && !near) continue;
            neighbors[i].push_back(j);
            neighbors[j].push_back(i);
        }

    std::vector<std::vector<size_t>> components;
    std::vector<bool> seen(entries.size(), false);
    for (size_t start = 0; start < entries.size(); ++start) {
        if (seen[start]) continue;
        seen[start] = true;
        std::vector<size_t> stack{start};
        std::vector<size_t> comp{start};
        while (!stack.empty()) {
            size_t idx = stack.back();
            stack.pop_back();
            for (size_t next : neighbors[idx]) {
                if (seen[next]) continue;
                seen[next] = true;
                stack.push_back(next);
                comp.push_back(next);
            }
        }
        std::sort(comp.begin(), comp.end());
        components.push_back(std::move(comp));
    }
    return components;
}

using Assignment = std::vector<std::pair<size_t, Point>>;

// Beam search over candidate choices for one component. nullopt when no consistent choice exists.
std::optional<Assignment> solve_flowchart_component_assignment(
    const std::vector<size_t>& component, const std::vector<std::vector<FlowchartCenterCandidate>>& candidate_table,
    const std::vector<FlowchartCenterLabelEntry>& entries, bool enforce_no_fixed_overlap,
    bool enforce_center_band) {
    // Hardest labels first: fewest viable candidates.
    std::vector<size_t> order = component;
    auto count_if_ = [&](size_t i, auto pred) {
        size_t n = 0;
        for (const auto& c : candidate_table[i])
            if (pred(c)) ++n;
        return n;
    };
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        auto central = [](const FlowchartCenterCandidate& c) { return c.center_dist <= c.center_hard_max; };
        auto strict = [](const FlowchartCenterCandidate& c) { return c.fixed_overlap_count == 0; };
        auto central_strict = [](const FlowchartCenterCandidate& c) {
            return c.fixed_overlap_count == 0 && c.center_dist <= c.center_hard_max;
        };
        size_t a_center = count_if_(a, central), b_center = count_if_(b, central);
        if (a_center != b_center) return a_center < b_center;
        size_t a_strict = count_if_(a, strict), b_strict = count_if_(b, strict);
        if (a_strict != b_strict) return a_strict < b_strict;
        size_t a_cs = count_if_(a, central_strict), b_cs = count_if_(b, central_strict);
        if (a_cs != b_cs) return a_cs < b_cs;
        if (candidate_table[a].size() != candidate_table[b].size()) return candidate_table[a].size() < candidate_table[b].size();
        float area_a = entries[a].label_w * entries[a].label_h;
        float area_b = entries[b].label_w * entries[b].label_h;
        int c = cmp_f(area_b, area_a);
        if (c != 0) return c < 0;
        return entries[a].edge_idx < entries[b].edge_idx;
    });

    size_t n = component.size();
    size_t beam_width = n <= 1 ? 1 : (n == 2 ? 36 : (n <= 4 ? 64 : (n <= 6 ? 84 : 108)));
    size_t cand_limit = n <= 1 ? 48 : (n <= 4 ? 72 : (n <= 7 ? 92 : 112));

    std::vector<FlowchartBeamState> beam(1);

    for (size_t entry_idx : order) {
        const auto& candidates = candidate_table[entry_idx];
        if (candidates.empty()) return std::nullopt;
        std::vector<FlowchartBeamState> next;
        for (const FlowchartBeamState& state : beam) {
            for (size_t cand_idx = 0; cand_idx < candidates.size() && cand_idx < cand_limit; ++cand_idx) {
                const FlowchartCenterCandidate& cand = candidates[cand_idx];
                if (enforce_no_fixed_overlap && cand.fixed_overlap_count > 0) continue;
                if (enforce_center_band && std::isfinite(cand.center_dist) && cand.center_dist > cand.center_hard_max)
                    continue;
                bool blocked = false;
                for (const Rect& r : state.rects)
                    if (overlap_area(r, cand.rect) > LABEL_OVERLAP_WIDE_THRESHOLD) { blocked = true; break; }
                if (blocked) continue;

                float primary = state.primary + cand.cost.first;
                float drift = state.drift + cand.cost.second;

                if (std::isfinite(cand.own_gap)) {
                    if (cand.own_gap > 3.5f) {
                        float over = cand.own_gap - 3.5f;
                        primary += over * over * 1.2f;
                    }
                    if (cand.own_gap < 1.1f) {
                        float under = 1.1f - cand.own_gap;
                        primary += under * under * 16.0f;
                    }
                    if (cand.own_gap <= 0.35f) primary += 48.0f;
                }
                if (std::isfinite(cand.center_dist)) {
                    float norm = fmax_(cand.center_dist / fmax_(cand.center_target, 1e-3f), 0.0f);
                    if (norm < 0.92f) {
                        float shortage = 0.92f - norm;
                        primary += shortage * shortage * 4.5f;
                    }
                    if (cand.center_dist > cand.center_soft_max) {
                        float over = cand.center_dist - cand.center_soft_max;
                        primary += over * over * 2.0f;
                    }
                    if (cand.center_dist > cand.center_hard_max) {
                        float over = cand.center_dist - cand.center_hard_max;
                        primary += over * 14.0f;
                    }
                    if (norm > 1.0f) {
                        float excess = norm - 1.0f;
                        primary += excess * excess * 0.75f;
                    }
                }
                float s_shift = std::fabs(cand.s_norm - entries[entry_idx].initial_s_norm);
                float d_shift = std::fabs(cand.d_signed - entries[entry_idx].initial_d_signed);
                primary += s_shift * 0.26f;
                primary += d_shift * 0.02f;

                if (!enforce_no_fixed_overlap && cand.fixed_overlap_count > 0) {
                    primary += static_cast<float>(cand.fixed_overlap_count) * 2.0f;
                    primary += cand.fixed_overlap_area * 0.005f;
                }
                for (const Rect& r : state.rects) {
                    float gap = rect_gap(r, cand.rect);
                    if (gap < 4.0f) {
                        float shortage = 4.0f - gap;
                        primary += shortage * shortage * 0.08f;
                    }
                }

                FlowchartBeamState ns;
                ns.assignments = state.assignments;
                ns.assignments.push_back({entry_idx, cand_idx});
                std::sort(ns.assignments.begin(), ns.assignments.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });
                ns.rects = state.rects;
                ns.rects.push_back(cand.rect);
                ns.primary = primary;
                ns.drift = drift;
                next.push_back(std::move(ns));
            }
        }
        if (next.empty()) return std::nullopt;
        std::stable_sort(next.begin(), next.end(), [](const FlowchartBeamState& a, const FlowchartBeamState& b) {
            int c = cmp_f(a.primary, b.primary);
            if (c != 0) return c < 0;
            c = cmp_f(a.drift, b.drift);
            if (c != 0) return c < 0;
            return a.assignments < b.assignments;  // lexicographic, like Vec::cmp
        });
        if (next.size() > beam_width) next.resize(beam_width);
        beam = std::move(next);
    }

    if (beam.empty()) return std::nullopt;
    Assignment resolved;
    for (const auto& [entry_idx, cand_idx] : beam[0].assignments)
        resolved.push_back({entry_idx, candidate_table[entry_idx][cand_idx].center});
    return resolved;
}

void apply_flowchart_component_assignment(std::vector<FlowchartCenterLabelEntry>& entries, float label_pad_x,
                                          float label_pad_y, const std::vector<Rect>& fixed_obstacles,
                                          const std::vector<EdgeObstacle>& edge_obstacles,
                                          const ObstacleGrid& edge_grid) {
    if (entries.empty()) return;
    std::vector<std::vector<FlowchartCenterCandidate>> candidate_table;
    for (const auto& entry : entries)
        candidate_table.push_back(build_flowchart_candidate_set(entry, label_pad_x, label_pad_y, fixed_obstacles,
                                                                edge_obstacles, edge_grid));
    auto components = flowchart_entry_components(entries, label_pad_x, label_pad_y);
    for (const auto& component : components) {
        if (component.empty()) continue;
        // Very large components are better handled by the iterative passes.
        if (component.size() > 12) continue;
        auto assignment = solve_flowchart_component_assignment(component, candidate_table, entries, false, true);
        if (!assignment) assignment = solve_flowchart_component_assignment(component, candidate_table, entries, false, false);
        if (!assignment) continue;
        for (const auto& [entry_idx, center] : *assignment) entries[entry_idx].current_center = center;
    }
}

}  // namespace

void deoverlap_flowchart_center_labels(std::vector<EdgeLayout>& edges, const std::map<std::string, NodeLayout>& nodes,
                                       const std::vector<SubgraphLayout>& subgraphs, Bounds bounds,
                                       const Theme& theme, float label_pad_x, float label_pad_y,
                                       const std::set<size_t>& locked_indices) {
    float step_normal_pad = fmax_(theme.font_size * 0.25f, label_pad_y);
    float step_tangent_pad = fmax_(theme.font_size * 0.35f, label_pad_x);
    std::vector<FlowchartCenterLabelEntry> entries;
    for (size_t idx = 0; idx < edges.size(); ++idx) {
        const EdgeLayout& edge = edges[idx];
        if (locked_indices.count(idx)) continue;
        if (!edge.label || !edge.label_anchor) continue;
        const TextBlock& label = *edge.label;
        Point anchor = *edge.label_anchor;
        Point current_center = bounds ? clamp_label_center_to_bounds(anchor, label.width, label.height, label_pad_x,
                                                                      label_pad_y, *bounds)
                                      : anchor;
        Point initial_center;
        if (auto a = edge_label_anchor_from_point(edge, current_center)) initial_center = {a->x, a->y};
        else {
            Anchor a2 = edge_label_anchor(edge);
            initial_center = {a2.x, a2.y};
        }
        auto pose = edge_relative_pose(edge.points, initial_center).value_or(std::make_pair(0.5f, 0.0f));
        auto candidates = flowchart_center_label_candidates(edge, current_center, label.width, label.height,
                                                            label_pad_x, label_pad_y, step_normal_pad,
                                                            step_tangent_pad, bounds);
        FlowchartCenterLabelEntry e;
        e.edge_idx = idx;
        e.label_w = label.width;
        e.label_h = label.height;
        e.initial_center = initial_center;
        e.initial_s_norm = pose.first;
        e.initial_d_signed = pose.second;
        e.current_center = current_center;
        e.edge_points = edge.points;
        e.candidates = std::move(candidates);
        entries.push_back(std::move(e));
    }
    if (entries.size() < 2) return;

    float node_obstacle_pad =
        fmax_(theme.font_size * 0.55f, fmax_(label_pad_x, label_pad_y + FLOWCHART_LABEL_CLEARANCE_PAD));
    float subgraph_label_pad = fmax_(theme.font_size * 0.35f, 3.0f);
    std::vector<Rect> fixed_obstacles = build_label_obstacles(nodes, subgraphs, DiagramKind::Flowchart, theme,
                                                              node_obstacle_pad, subgraph_label_pad);
    for (const Rect& r : build_node_text_obstacles(nodes, fmax_(theme.font_size * 0.2f, 2.0f)))
        fixed_obstacles.push_back(r);
    for (size_t idx = 0; idx < edges.size(); ++idx) {
        if (!locked_indices.count(idx)) continue;
        const EdgeLayout& edge = edges[idx];
        if (!edge.label || !edge.label_anchor) continue;
        Point center = *edge.label_anchor;
        Rect rect = {center.first - edge.label->width / 2.0f - label_pad_x,
                     center.second - edge.label->height / 2.0f - label_pad_y, edge.label->width + 2.0f * label_pad_x,
                     edge.label->height + 2.0f * label_pad_y};
        fixed_obstacles.push_back(inflate_rect(rect, FLOWCHART_LABEL_CLEARANCE_PAD));
    }
    float edge_obstacle_pad = fmax_(theme.font_size * 0.35f, label_pad_y);
    std::vector<EdgeObstacle> edge_obstacles = build_edge_obstacles(edges, edge_obstacle_pad);
    ObstacleGrid edge_grid(48.0f, obstacle_rects(edge_obstacles));

    // Deterministic global assignment pass: solve per conflict component.
    apply_flowchart_component_assignment(entries, label_pad_x, label_pad_y, fixed_obstacles, edge_obstacles, edge_grid);

    auto obstacle_rect_of = [&](const FlowchartCenterLabelEntry& e) {
        return flowchart_center_label_obstacle_rect(e.current_center, e.label_w, e.label_h, label_pad_x, label_pad_y);
    };

    // Iterative global refinement: resolve the most conflicted labels first.
    for (int iter = 0; iter < 10; ++iter) {
        std::vector<Rect> current_rects;
        for (const auto& e : entries) current_rects.push_back(obstacle_rect_of(e));
        std::vector<std::pair<float, size_t>> conflict_order;
        for (size_t i = 0; i < current_rects.size(); ++i) {
            float conflict_score = 0.0f;
            for (size_t j = 0; j < current_rects.size(); ++j) {
                if (i == j) continue;
                float ov = overlap_area(current_rects[i], current_rects[j]);
                if (ov > 0.0f) conflict_score += ov + 1.0f;
            }
            for (const Rect& obstacle : fixed_obstacles) {
                float ov = overlap_area(current_rects[i], obstacle);
                if (ov > 0.0f) conflict_score += ov * 1.6f + 1.0f;
            }
            if (conflict_score > 0.0f) conflict_order.push_back({conflict_score, i});
        }
        if (conflict_order.empty()) break;
        std::stable_sort(conflict_order.begin(), conflict_order.end(),
                         [](const auto& a, const auto& b) { return cmp_f(b.first, a.first) < 0; });

        bool moved = false;
        for (const auto& [score, entry_idx] : conflict_order) {
            (void)score;
            FlowchartCenterLabelEntry entry_snapshot = entries[entry_idx];
            std::vector<Rect> others;
            for (size_t i = 0; i < entries.size(); ++i)
                if (i != entry_idx) others.push_back(obstacle_rect_of(entries[i]));
            Point best_center = entry_snapshot.current_center;
            Cost best_cost = flowchart_center_label_refine_cost(entry_snapshot, entry_snapshot.current_center,
                                                               label_pad_x, label_pad_y, others, fixed_obstacles,
                                                               edge_obstacles, edge_grid);
            auto evaluate = [&](bool enforce_gap_limit, bool enforce_center_band) -> bool {
                bool considered = false;
                for (const Point& candidate : entry_snapshot.candidates) {
                    if (std::fabs(candidate.first - entry_snapshot.current_center.first) <= 0.2f &&
                        std::fabs(candidate.second - entry_snapshot.current_center.second) <= 0.2f)
                        continue;
                    if (enforce_center_band) {
                        float center_dist = point_polyline_distance(candidate, entry_snapshot.edge_points);
                        float center_hard_max = flowchart_pose_center_hard_max(
                            entry_snapshot.edge_points, candidate, entry_snapshot.label_w, entry_snapshot.label_h,
                            label_pad_x, label_pad_y);
                        if (std::isfinite(center_dist) && center_dist > center_hard_max) continue;
                    }
                    if (enforce_gap_limit) {
                        Rect rect = flowchart_center_label_rect(candidate, entry_snapshot.label_w,
                                                                entry_snapshot.label_h, label_pad_x, label_pad_y);
                        float own_gap = polyline_rect_distance(entry_snapshot.edge_points, rect);
                        if (!flowchart_own_gap_allowed(own_gap, FLOWCHART_OWN_EDGE_HARD_MAX_GAP)) continue;
                    }
                    considered = true;
                    Cost cost = flowchart_center_label_refine_cost(entry_snapshot, candidate, label_pad_x,
                                                                   label_pad_y, others, fixed_obstacles,
                                                                   edge_obstacles, edge_grid);
                    if (candidate_better(cost, best_cost)) {
                        best_cost = cost;
                        best_center = candidate;
                    }
                }
                return considered;
            };
            bool considered_strict = evaluate(true, true);
            if (!considered_strict) {
                bool considered_center_relaxed = evaluate(true, false);
                if (!considered_center_relaxed) (void)evaluate(false, false);
            }
            if (std::fabs(best_center.first - entries[entry_idx].current_center.first) > 0.2f ||
                std::fabs(best_center.second - entries[entry_idx].current_center.second) > 0.2f) {
                entries[entry_idx].current_center = best_center;
                moved = true;
            }
        }
        if (!moved) break;
    }

    // If overlaps still remain, force-separate conflicted pairs by selecting a non-overlapping
    // candidate for one side of the pair.
    CenterLabelOverlapScore residual_overlap = flowchart_entry_label_overlap_score(entries, label_pad_x, label_pad_y);
    size_t horizontal_entries = 0;
    for (const auto& e : entries) {
        auto [span_x, span_y] = flowchart_path_span(e.edge_points);
        if (span_x >= span_y) ++horizontal_entries;
    }
    bool use_pair_separation_candidates =
        horizontal_entries * 2 >= entries.size() && residual_overlap.count > 0 && residual_overlap.total >= 10.0f;
    for (int iter = 0; iter < 6; ++iter) {
        std::vector<Rect> current_rects;
        for (const auto& e : entries) current_rects.push_back(obstacle_rect_of(e));
        bool adjusted = false;
        for (size_t i = 0; i < entries.size() && !adjusted; ++i) {
            for (size_t j = i + 1; j < entries.size() && !adjusted; ++j) {
                if (overlap_area(current_rects[i], current_rects[j]) <= LABEL_OVERLAP_WIDE_THRESHOLD) continue;
                for (size_t move_idx : {i, j}) {
                    FlowchartCenterLabelEntry entry_snapshot = entries[move_idx];
                    Rect blocking_rect = current_rects[move_idx == i ? j : i];
                    std::vector<Point> candidate_centers = entry_snapshot.candidates;
                    if (use_pair_separation_candidates)
                        for (const Point& c : flowchart_pair_separation_candidates(entry_snapshot, blocking_rect,
                                                                                    label_pad_x, label_pad_y, bounds))
                            push_center_unique(candidate_centers, c);
                    std::vector<Rect> others;
                    for (size_t k = 0; k < current_rects.size(); ++k)
                        if (k != move_idx) others.push_back(current_rects[k]);
                    std::optional<Point> best_center;
                    Cost best_cost{INF, INF};
                    float rect_area = fmax_(entry_snapshot.label_w * entry_snapshot.label_h, 1.0f);
                    auto evaluate = [&](bool enforce_gap_limit, bool enforce_no_overlap,
                                        bool enforce_center_band) -> bool {
                        bool considered = false;
                        for (const Point& candidate : candidate_centers) {
                            if (enforce_center_band) {
                                float center_dist = point_polyline_distance(candidate, entry_snapshot.edge_points);
                                float center_hard_max = flowchart_pose_center_hard_max(
                                    entry_snapshot.edge_points, candidate, entry_snapshot.label_w,
                                    entry_snapshot.label_h, label_pad_x, label_pad_y);
                                if (std::isfinite(center_dist) && center_dist > center_hard_max) continue;
                            }
                            Rect rect = flowchart_center_label_rect(candidate, entry_snapshot.label_w,
                                                                    entry_snapshot.label_h, label_pad_x, label_pad_y);
                            Rect obstacle_rect = flowchart_center_label_obstacle_rect(
                                candidate, entry_snapshot.label_w, entry_snapshot.label_h, label_pad_x, label_pad_y);
                            float overlap_penalty = 0.0f;
                            bool has_overlap = false;
                            for (const Rect& other : others) {
                                float ov = overlap_area(obstacle_rect, other);
                                if (ov > LABEL_OVERLAP_WIDE_THRESHOLD) {
                                    has_overlap = true;
                                    overlap_penalty += (ov / rect_area) * 140.0f;
                                }
                            }
                            for (const Rect& obstacle : fixed_obstacles) {
                                float ov = overlap_area(obstacle_rect, obstacle);
                                if (ov > LABEL_OVERLAP_WIDE_THRESHOLD) {
                                    has_overlap = true;
                                    overlap_penalty += (ov / rect_area) * 180.0f;
                                }
                            }
                            if (enforce_no_overlap && has_overlap) continue;
                            if (enforce_gap_limit) {
                                float own_gap = polyline_rect_distance(entry_snapshot.edge_points, rect);
                                if (!flowchart_own_gap_allowed(own_gap, FLOWCHART_OWN_EDGE_HARD_MAX_GAP)) continue;
                            }
                            considered = true;
                            Cost cost = flowchart_center_label_refine_cost(entry_snapshot, candidate, label_pad_x,
                                                                           label_pad_y, others, fixed_obstacles,
                                                                           edge_obstacles, edge_grid);
                            cost = {cost.first + overlap_penalty, cost.second};
                            if (!best_center || candidate_better(cost, best_cost)) {
                                best_center = candidate;
                                best_cost = cost;
                            }
                        }
                        return considered;
                    };
                    bool considered_strict = evaluate(true, true, true);
                    if (!considered_strict && use_pair_separation_candidates) {
                        if (!evaluate(true, true, false)) {
                            if (!evaluate(false, true, false)) {
                                if (!evaluate(true, false, true)) {
                                    if (!evaluate(true, false, false)) (void)evaluate(false, false, false);
                                }
                            }
                        }
                    } else if (!considered_strict) {
                        if (!evaluate(true, false, true)) {
                            if (!evaluate(true, false, false)) (void)evaluate(false, false, false);
                        }
                    }
                    if (best_center && (std::fabs(best_center->first - entries[move_idx].current_center.first) > 0.2f ||
                                        std::fabs(best_center->second - entries[move_idx].current_center.second) > 0.2f)) {
                        entries[move_idx].current_center = *best_center;
                        adjusted = true;  // `break 'pair_search`
                        break;
                    }
                }
            }
        }
        if (!adjusted) break;
    }

    for (const auto& entry : entries) edges[entry.edge_idx].label_anchor = entry.current_center;
}

}  // namespace mermaid::lp
