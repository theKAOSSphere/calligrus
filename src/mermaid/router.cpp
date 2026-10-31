// Mermaid port of layout/routing.rs, part 2: candidate generation and choice for one edge
// (route_edge_with_avoidance), the A* grid router and the self-loop shapes.
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

#include "routing.h"
#include "strutil.h"

namespace mermaid {

namespace {

constexpr float ROUTING_CELL_FALLBACK_SCALE = 1.45f;
constexpr float ROUTING_CELL_MIN = 8.0f;
constexpr float GRID_MARGIN_MIN_SPACING = 24.0f;
constexpr float ASTAR_COST_SCALE = 1000.0f;
constexpr float ROUTING_PAD_RATIO = 0.6f;
constexpr float ROUTING_PAD_MIN_SPACING = 20.0f;
constexpr float ORTHO_STEP_MIN_SPACING = 16.0f;
constexpr float CHANNEL_CANDIDATE_RATIO = 0.75f;
constexpr float OVERLAP_TRIGGER_RATIO = 0.35f;
constexpr float OVERLAP_TRIGGER_MIN = 4.0f;
constexpr float OVERLAP_DETOUR_MIN = 3.0f;
constexpr float ROUTE_LENGTH_TIE_EPS = 2.0f;
constexpr float ROUTE_CROSSING_DETOUR_RATIO = 2.0f;
constexpr float ROUTE_CROSSING_DETOUR_MIN_SPACING = 40.0f;
constexpr float ROUTE_CROSSING_DETOUR_RELATIVE = 1.0f;
constexpr float ROUTE_VIA_TIE_EPS = 0.4f;
constexpr float ROUTE_SOFT_NODE_CLEARANCE = 3.0f;
constexpr float ROUTE_SOFT_LABEL_CLEARANCE = 4.0f;
constexpr float ROUTE_SOFT_EDGE_CLEARANCE = 4.0f;
constexpr size_t ROUTE_OWN_LABEL_HARD_WEIGHT = 6;
constexpr size_t ROUTE_OWN_LABEL_NEAR_WEIGHT = 3;
constexpr float EXTERIOR_FALLBACK_PAD_RATIO = 1.35f;
constexpr float EXTERIOR_FALLBACK_PAD_MIN = 36.0f;

float fmax_(float a, float b) { return a > b ? a : (a < b ? b : a); }
float fmin_(float a, float b) { return a < b ? a : (a > b ? b : a); }
float clamp_(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
unsigned sat_add(unsigned a, unsigned b) {
    unsigned long long s = static_cast<unsigned long long>(a) + b;
    return s > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<unsigned>(s);
}
unsigned sat_mul(unsigned a, unsigned b) {
    unsigned long long s = static_cast<unsigned long long>(a) * b;
    return s > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<unsigned>(s);
}
unsigned f2u32(float v) {
    if (v != v || v <= 0.0f) return 0;
    if (v >= 4294967296.0f) return 0xFFFFFFFFu;
    return static_cast<unsigned>(v);
}

Obstacle expand_obstacle(const Obstacle& o, float pad) {
    return {o.id, o.x - pad, o.y - pad, o.width + pad * 2.0f, o.height + pad * 2.0f, o.members};
}

struct PreferredLabelMetrics {
    size_t hard_hits = 0, near_hits = 0;
};

struct RouteCandidate {
    std::vector<Point> points;
    size_t hard_hits = 0, hits = 0;
    PreferredLabelMetrics own_label;
    size_t cross = 0;
    size_t label_hits = 0;
    float overlap = 0.0f;
    float via_dist = 0.0f;
    size_t bends = 0;
    float len = 0.0f;
};

struct RouteCandidateOrderKey {
    size_t hits;
    size_t own_label_score;
    size_t cross;
    size_t label_hits;
    float overlap;
    float via_dist;
    size_t bends;
    float len;
    std::optional<unsigned> occupancy_score;
};

size_t path_obstacle_near_intersections(const std::vector<Point>& points, const std::vector<Obstacle>& obstacles,
                                        const std::string& from_id, const std::string& to_id, float pad) {
    if (points.size() < 2 || obstacles.empty() || pad <= 0.0f) return 0;
    size_t count = 0;
    for (size_t s = 0; s + 1 < points.size(); ++s) {
        Point a = points[s], b = points[s + 1];
        for (const auto& o : obstacles) {
            if (o.id == from_id || o.id == to_id) continue;
            if (o.members && (o.members->count(from_id) || o.members->count(to_id))) continue;
            if (segment_intersects_rect(a, b, o)) continue;
            Obstacle expanded{"", o.x - pad, o.y - pad, o.width + pad * 2.0f, o.height + pad * 2.0f, nullptr};
            if (segment_intersects_rect(a, b, expanded)) ++count;
        }
    }
    return count;
}

size_t path_label_near_intersections(const std::vector<Point>& points, const std::vector<Obstacle>& label_obstacles,
                                     const std::optional<std::string>& ignore_label_id, float pad) {
    if (points.size() < 2 || label_obstacles.empty() || pad <= 0.0f) return 0;
    size_t count = 0;
    for (size_t s = 0; s + 1 < points.size(); ++s) {
        Point a = points[s], b = points[s + 1];
        for (const auto& o : label_obstacles) {
            if (ignore_label_id && *ignore_label_id == o.id) continue;
            if (segment_intersects_rect(a, b, o)) continue;
            Obstacle expanded{"", o.x - pad, o.y - pad, o.width + pad * 2.0f, o.height + pad * 2.0f, nullptr};
            if (segment_intersects_rect(a, b, expanded)) ++count;
        }
    }
    return count;
}

float point_segment_distance(Point a, Point b, Point p) {
    float vx = b.first - a.first, vy = b.second - a.second;
    float wx = p.first - a.first, wy = p.second - a.second;
    float vv = vx * vx + vy * vy;
    if (vv <= 1e-6f) {
        float dx = p.first - a.first, dy = p.second - a.second;
        return std::sqrt(dx * dx + dy * dy);
    }
    float t = clamp_((wx * vx + wy * vy) / vv, 0.0f, 1.0f);
    float proj_x = a.first + t * vx, proj_y = a.second + t * vy;
    float dx = p.first - proj_x, dy = p.second - proj_y;
    return std::sqrt(dx * dx + dy * dy);
}

float segment_to_segment_distance(Point a1, Point a2, Point b1, Point b2) {
    if (segments_intersect(a1, a2, b1, b2)) return 0.0f;
    float d1 = point_segment_distance(a1, a2, b1);
    float d2 = point_segment_distance(a1, a2, b2);
    float d3 = point_segment_distance(b1, b2, a1);
    float d4 = point_segment_distance(b1, b2, a2);
    return fmin_(fmin_(fmin_(d1, d2), d3), d4);
}

float path_existing_proximity_penalty(const std::vector<Point>& points, const std::vector<Segment>& existing, float clearance) {
    if (points.size() < 2 || existing.empty() || clearance <= 0.0f) return 0.0f;
    float penalty = 0.0f;
    for (size_t s = 0; s + 1 < points.size(); ++s) {
        for (const auto& [b1, b2] : existing) {
            float dist = segment_to_segment_distance(points[s], points[s + 1], b1, b2);
            if (dist < clearance) penalty += (clearance - dist) / clearance;
        }
    }
    return penalty;
}

PreferredLabelMetrics preferred_label_metrics(const std::vector<Point>& points, const RouteContext& ctx) {
    if (!ctx.preferred_label_obstacle) return {};
    std::vector<Obstacle> one{*ctx.preferred_label_obstacle};
    size_t hard = path_label_intersections(points, one, std::nullopt);
    size_t near_ = path_label_near_intersections(points, one, std::nullopt, fmax_(ctx.preferred_label_clearance, 0.0f));
    return {hard, near_};
}

size_t own_label_score(const PreferredLabelMetrics& m) {
    return m.hard_hits * ROUTE_OWN_LABEL_HARD_WEIGHT + m.near_hits * ROUTE_OWN_LABEL_NEAR_WEIGHT;
}

RouteCandidateOrderKey route_candidate_key(const RouteCandidate& c, std::optional<unsigned> occupancy_score) {
    return {c.hits, own_label_score(c.own_label), c.cross, c.label_hits, c.overlap, c.via_dist, c.bends, c.len, occupancy_score};
}

bool route_candidate_better(const RouteContext& ctx, const RouteCandidateOrderKey& candidate,
                            const std::optional<RouteCandidateOrderKey>& best_opt) {
    if (!best_opt) return true;
    const RouteCandidateOrderKey& best = *best_opt;
    if (candidate.hits != best.hits) return candidate.hits < best.hits;
    if (candidate.own_label_score != best.own_label_score) return candidate.own_label_score < best.own_label_score;
    if (candidate.cross != best.cross) {
        // Avoiding crossings is worth some extra length, but not an unbounded detour.
        const RouteCandidateOrderKey& fewer = candidate.cross < best.cross ? candidate : best;
        const RouteCandidateOrderKey& more = candidate.cross < best.cross ? best : candidate;
        float crossings_avoided = static_cast<float>(more.cross >= fewer.cross ? more.cross - fewer.cross : 0);
        float extra_len = fewer.len - more.len;
        float allowance = fmax_(ctx.config->node_spacing, ROUTE_CROSSING_DETOUR_MIN_SPACING) * ROUTE_CROSSING_DETOUR_RATIO;
        if (extra_len > crossings_avoided * allowance && extra_len > more.len * ROUTE_CROSSING_DETOUR_RELATIVE)
            return candidate.len < best.len;
        return candidate.cross < best.cross;
    }
    if (candidate.label_hits != best.label_hits) return candidate.label_hits < best.label_hits;
    if (std::fabs(candidate.overlap - best.overlap) > 1e-4f) return candidate.overlap < best.overlap;
    if (candidate.via_dist + ROUTE_VIA_TIE_EPS < best.via_dist) return true;
    if (std::fabs(candidate.via_dist - best.via_dist) > ROUTE_VIA_TIE_EPS) return false;
    if (ctx.prefer_shorter_ties) {
        if (candidate.len + ROUTE_LENGTH_TIE_EPS < best.len) return true;
        if (std::fabs(candidate.len - best.len) > ROUTE_LENGTH_TIE_EPS) return false;
        if (candidate.occupancy_score != best.occupancy_score && candidate.occupancy_score && best.occupancy_score &&
            *candidate.occupancy_score != *best.occupancy_score)
            return *candidate.occupancy_score < *best.occupancy_score;
        return candidate.bends < best.bends;
    }
    if (candidate.bends != best.bends) return candidate.bends < best.bends;
    if (candidate.occupancy_score != best.occupancy_score && candidate.occupancy_score && best.occupancy_score &&
        *candidate.occupancy_score != *best.occupancy_score)
        return *candidate.occupancy_score < *best.occupancy_score;
    return candidate.len < best.len;
}

bool path_coords_reasonable(const std::vector<Point>& points) {
    const float LIMIT = 100000.0f;
    for (const auto& [x, y] : points)
        if (!(std::isfinite(x) && std::isfinite(y) && std::fabs(x) <= LIMIT && std::fabs(y) <= LIMIT)) return false;
    return true;
}

size_t path_endpoint_intrusions(const std::vector<Point>& points, const RouteContext& ctx) {
    if (points.size() < 2 || *ctx.from_id == *ctx.to_id) return 0;
    size_t hits = 0;
    for (size_t s = 0; s + 1 < points.size(); ++s) {
        if (segment_hits_node_shape_interior(points[s], points[s + 1], *ctx.from)) ++hits;
        if (segment_hits_node_shape_interior(points[s], points[s + 1], *ctx.to)) ++hits;
    }
    return hits;
}

void push_route_candidate(std::vector<Point> points, const RouteContext& ctx, const std::vector<Segment>& existing,
                          bool use_existing, std::vector<RouteCandidate>& candidates) {
    if (points.size() < 2 || !path_coords_reasonable(points)) return;
    size_t hard_hits = path_obstacle_intersections(points, *ctx.obstacles, *ctx.from_id, *ctx.to_id);
    size_t endpoint_hits = path_endpoint_intrusions(points, ctx);
    size_t hard_route_hits = hard_hits + endpoint_hits;
    size_t soft_hits = path_obstacle_near_intersections(points, *ctx.obstacles, *ctx.from_id, *ctx.to_id, ROUTE_SOFT_NODE_CLEARANCE);
    size_t hits = hard_hits * 4 + endpoint_hits * 16 + soft_hits;
    PreferredLabelMetrics own_label = preferred_label_metrics(points, ctx);
    size_t hard_labels = path_label_intersections(points, *ctx.label_obstacles, ctx.preferred_label_id);
    size_t soft_labels = path_label_near_intersections(points, *ctx.label_obstacles, ctx.preferred_label_id, ROUTE_SOFT_LABEL_CLEARANCE);
    size_t labels = hard_labels * 4 + soft_labels;
    float via_dist = ctx.preferred_label_center ? polyline_point_distance(points, *ctx.preferred_label_center) : 0.0f;
    size_t cross = 0;
    float overlap = 0.0f;
    if (use_existing) {
        auto [c, o] = edge_crossings_with_existing(points, existing);
        cross = c;
        overlap = o;
        overlap += path_existing_proximity_penalty(points, existing, ROUTE_SOFT_EDGE_CLEARANCE);
    }
    size_t bends = path_bend_count(points);
    float len = path_length(points);
    RouteCandidate cand;
    cand.points = std::move(points);
    cand.hard_hits = hard_route_hits;
    cand.hits = hits;
    cand.own_label = own_label;
    cand.cross = cross;
    cand.label_hits = labels;
    cand.overlap = overlap;
    cand.via_dist = via_dist;
    cand.bends = bends;
    cand.len = len;
    candidates.push_back(std::move(cand));
}

// ---- endpoints ----

struct RouteEndpoints {
    Point start, end, route_start, route_end;
};

RouteEndpoints resolve_route_endpoints(const RouteContext& ctx) {
    Point start = anchor_point_for_node(*ctx.from, ctx.start_side, ctx.start_offset);
    Point end = anchor_point_for_node(*ctx.to, ctx.end_side, ctx.end_offset);
    Point route_start = port_stub_point(start, ctx.start_side, ctx.stub_len);
    Point route_end = port_stub_point(end, ctx.end_side, ctx.stub_len);
    // A stub that would immediately cross an unrelated node is collapsed.
    auto stub_hits_node = [&](Point a, Point b) {
        for (const auto& o : *ctx.obstacles) {
            if (o.members) continue;
            if (o.id == *ctx.from_id || o.id == *ctx.to_id) continue;
            if (segment_intersects_rect(a, b, o)) return true;
        }
        return false;
    };
    if (ctx.obstacles->size() <= 10) {
        if (stub_hits_node(start, route_start)) route_start = start;
        if (stub_hits_node(route_end, end)) route_end = end;
    }
    return {start, end, route_start, route_end};
}

void enforce_preferred_label_via(std::vector<Point>& points, const RouteContext& ctx) {
    if (!ctx.force_preferred_label_via) return;
    if (!ctx.preferred_label_center) return;
    if (points.size() < 2) return;
    if (polyline_point_distance(points, *ctx.preferred_label_center) <= 0.6f) return;
    insert_label_via_point(points, *ctx.preferred_label_center);
}

std::vector<Point> finish(const RouteEndpoints& ep, const RouteContext& ctx, const std::vector<Point>& routed) {
    std::vector<Point> combined;
    combined.reserve(routed.size() + 2);
    combined.push_back(ep.start);
    combined.insert(combined.end(), routed.begin(), routed.end());
    combined.push_back(ep.end);
    enforce_preferred_label_via(combined, ctx);
    return apply_endpoint_insets(compress_path(combined), ctx.start_inset, ctx.end_inset);
}

// ---- self loops ----

std::vector<Point> route_self_loop(const NodeLayout& node, Direction direction, const LayoutConfig& config) {
    float pad = fmax_(config.node_spacing, ROUTING_PAD_MIN_SPACING) * ROUTING_PAD_RATIO;
    if (is_horizontal(direction)) {
        Point start{node.x + node.width, node.y + node.height / 2.0f};
        Point p1{node.x + node.width + pad, node.y + node.height / 2.0f};
        Point p2{node.x + node.width + pad, node.y - pad};
        Point p3{node.x + node.width / 2.0f, node.y - pad};
        Point end{node.x + node.width / 2.0f, node.y};
        return {start, p1, p2, p3, end};
    }
    Point start{node.x + node.width / 2.0f, node.y + node.height};
    Point p1{node.x + node.width / 2.0f, node.y + node.height + pad};
    Point p2{node.x + node.width + pad, node.y + node.height + pad};
    Point p3{node.x + node.width + pad, node.y + node.height / 2.0f};
    Point end{node.x + node.width, node.y + node.height / 2.0f};
    return {start, p1, p2, p3, end};
}

std::vector<std::vector<Point>> route_self_loop_candidates(const NodeLayout& node, float pad) {
    float x = node.x, y = node.y, w = node.width, h = node.height;
    float cx = x + w / 2.0f, cy = y + h / 2.0f;
    Point left{x, cy}, right{x + w, cy}, top{cx, y}, bottom{cx, y + h};
    float left_x = x - pad, right_x = x + w + pad, top_y = y - pad, bottom_y = y + h + pad;
    return {
        {right, {right_x, cy}, {right_x, top_y}, {cx, top_y}, top},
        {right, {right_x, cy}, {right_x, bottom_y}, {cx, bottom_y}, bottom},
        {left, {left_x, cy}, {left_x, top_y}, {cx, top_y}, top},
        {left, {left_x, cy}, {left_x, bottom_y}, {cx, bottom_y}, bottom},
        {top, {cx, top_y}, {right_x, top_y}, {right_x, cy}, right},
        {top, {cx, top_y}, {left_x, top_y}, {left_x, cy}, left},
        {bottom, {cx, bottom_y}, {right_x, bottom_y}, {right_x, cy}, right},
        {bottom, {cx, bottom_y}, {left_x, bottom_y}, {left_x, cy}, left},
    };
}

// ---- grid router (A*) ----

struct GridState {
    int x, y;
    unsigned char dir;
};

struct GridEntry {
    unsigned est, cost;
    GridState state;
};

// Rust: BinaryHeap<GridEntry> with Ord reversed on est/cost: the entry popped first has the
// smallest est, then smallest cost, then the largest y, x, dir.
struct GridEntryLess {
    bool operator()(const GridEntry& a, const GridEntry& b) const {
        // "a < b" in the Rust Ord means a is popped later than b.
        if (a.est != b.est) return a.est > b.est;
        if (a.cost != b.cost) return a.cost > b.cost;
        if (a.state.y != b.state.y) return a.state.y < b.state.y;
        if (a.state.x != b.state.x) return a.state.x < b.state.x;
        return a.state.dir < b.state.dir;
    }
};

bool cell_blocked(const RoutingGrid& grid, const std::vector<Obstacle>& obstacles, int ix, int iy, const RouteContext& ctx) {
    auto [cx, cy] = grid.cell_center(ix, iy);
    for (size_t obs_idx : grid.cell_obstacles[static_cast<size_t>(grid.index(ix, iy))]) {
        const Obstacle& o = obstacles[obs_idx];
        if (o.id == *ctx.from_id || o.id == *ctx.to_id) continue;
        if (o.members && (o.members->count(*ctx.from_id) || o.members->count(*ctx.to_id))) continue;
        if (cx >= o.x && cx <= o.x + o.width && cy >= o.y && cy <= o.y + o.height) return true;
    }
    if (ctx.preferred_label_obstacle) {
        Obstacle e = expand_obstacle(*ctx.preferred_label_obstacle, fmax_(ctx.preferred_label_clearance, 0.0f));
        if (cx >= e.x && cx <= e.x + e.width && cy >= e.y && cy <= e.y + e.height) return true;
    }
    return false;
}

std::optional<std::vector<Point>> route_edge_with_grid(const RouteContext& ctx, const RoutingGrid& grid,
                                                       const EdgeOccupancy* occupancy, Point start, Point end) {
    if (!ctx.config->flowchart.routing.enable_grid_router) return std::nullopt;
    auto sc = grid.cell_for_point(start.first, start.second);
    if (!sc) return std::nullopt;
    auto ec = grid.cell_for_point(end.first, end.second);
    if (!ec) return std::nullopt;
    auto [start_ix, start_iy] = *sc;
    auto [end_ix, end_iy] = *ec;
    if (start_ix == end_ix && start_iy == end_iy) return std::vector<Point>{start, end};

    const int dirs[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
    unsigned step_cost = f2u32(std::round(grid.cell * ASTAR_COST_SCALE));
    int manhattan_cells = std::abs(end_ix - start_ix) + std::abs(end_iy - start_iy);
    float turn_scale;
    if (manhattan_cells <= 8) turn_scale = 0.85f;
    else if (manhattan_cells >= 42) turn_scale = 1.35f;
    else turn_scale = 0.85f + (static_cast<float>(manhattan_cells) - 8.0f) * (0.50f / 34.0f);
    float occupancy_scale;
    if (manhattan_cells <= 10) occupancy_scale = 0.90f;
    else if (manhattan_cells >= 36) occupancy_scale = 0.60f;
    else occupancy_scale = 0.90f - (static_cast<float>(manhattan_cells) - 10.0f) * (0.30f / 26.0f);
    unsigned turn_penalty =
        f2u32(std::round(ctx.config->flowchart.routing.turn_penalty * turn_scale * grid.cell * ASTAR_COST_SCALE));
    unsigned occupancy_weight = std::max(
        f2u32(std::round(ctx.config->flowchart.routing.occupancy_weight * occupancy_scale * grid.cell * ASTAR_COST_SCALE)), 1u);
    size_t max_steps = std::max<size_t>(ctx.config->flowchart.routing.max_steps, 10000);

    int cols = grid.cols, rows = grid.rows;
    size_t states = static_cast<size_t>(cols) * static_cast<size_t>(rows) * 4;
    std::vector<unsigned> best_cost(states, 0xFFFFFFFFu);
    std::vector<std::optional<GridState>> prev(states);
    std::priority_queue<GridEntry, std::vector<GridEntry>, GridEntryLess> heap;

    for (unsigned char dir = 0; dir < 4; ++dir) {
        size_t idx = (static_cast<size_t>(start_iy) * static_cast<size_t>(cols) + static_cast<size_t>(start_ix)) * 4 + dir;
        best_cost[idx] = 0;
        heap.push({0, 0, {start_ix, start_iy, dir}});
    }

    std::optional<GridState> end_state;
    size_t steps = 0;
    while (!heap.empty()) {
        GridEntry entry = heap.top();
        heap.pop();
        ++steps;
        if (steps > max_steps) break;
        unsigned cost = entry.cost;
        GridState state = entry.state;
        size_t state_idx = (static_cast<size_t>(state.y) * static_cast<size_t>(cols) + static_cast<size_t>(state.x)) * 4 + state.dir;
        if (cost != best_cost[state_idx]) continue;
        if (state.x == end_ix && state.y == end_iy) {
            end_state = state;
            break;
        }
        for (int dir_idx = 0; dir_idx < 4; ++dir_idx) {
            int nx = state.x + dirs[dir_idx][0];
            int ny = state.y + dirs[dir_idx][1];
            if (nx < 0 || ny < 0 || nx >= cols || ny >= rows) continue;
            if ((nx != end_ix || ny != end_iy) && (nx != start_ix || ny != start_iy) &&
                cell_blocked(grid, *ctx.obstacles, nx, ny, ctx))
                continue;
            unsigned next_cost = sat_add(cost, step_cost);
            if (state.dir != dir_idx) next_cost = sat_add(next_cost, turn_penalty);
            if (occupancy) {
                auto [cx, cy] = grid.cell_center(nx, ny);
                float raw_weight = static_cast<float>(occupancy->weight_at(cx, cy));
                if (raw_weight > 0.0f) {
                    unsigned compressed_weight = f2u32(std::round(fmax_(std::sqrt(raw_weight), 1.0f)));
                    next_cost = sat_add(next_cost, sat_mul(compressed_weight, occupancy_weight));
                }
            }
            size_t next_idx = (static_cast<size_t>(ny) * static_cast<size_t>(cols) + static_cast<size_t>(nx)) * 4 + static_cast<size_t>(dir_idx);
            if (next_cost >= best_cost[next_idx]) continue;
            best_cost[next_idx] = next_cost;
            prev[next_idx] = state;
            unsigned manhattan = static_cast<unsigned>(std::abs(nx - end_ix)) + static_cast<unsigned>(std::abs(ny - end_iy));
            unsigned est = sat_add(next_cost, sat_mul(manhattan, step_cost));
            heap.push({est, next_cost, {nx, ny, static_cast<unsigned char>(dir_idx)}});
        }
    }

    if (!end_state) return std::nullopt;
    std::vector<std::pair<int, int>> cells;
    GridState cur = *end_state;
    while (true) {
        cells.push_back({cur.x, cur.y});
        size_t cur_idx = (static_cast<size_t>(cur.y) * static_cast<size_t>(cols) + static_cast<size_t>(cur.x)) * 4 + cur.dir;
        if (prev[cur_idx]) cur = *prev[cur_idx];
        else break;
    }
    std::reverse(cells.begin(), cells.end());
    if (cells.empty()) return std::nullopt;

    std::vector<Point> points;
    points.reserve(cells.size() + 4);
    points.push_back(start);
    {
        auto [ix, iy] = cells.front();
        auto [cx, cy] = grid.cell_center(ix, iy);
        if (ctx.start_side == EdgeSide::Left || ctx.start_side == EdgeSide::Right) points.push_back({cx, start.second});
        else points.push_back({start.first, cy});
        points.push_back({cx, cy});
    }
    for (size_t i = 1; i < cells.size(); ++i) points.push_back(grid.cell_center(cells[i].first, cells[i].second));
    {
        auto [ix, iy] = cells.back();
        auto [cx, cy] = grid.cell_center(ix, iy);
        if (ctx.end_side == EdgeSide::Left || ctx.end_side == EdgeSide::Right) points.push_back({cx, end.second});
        else points.push_back({end.first, cy});
    }
    points.push_back(end);
    return compress_path(points);
}

std::unique_ptr<RoutingGrid> build_fallback_routing_grid(const std::vector<Obstacle>& obstacles, const LayoutConfig& config,
                                                        float base_cell) {
    float fallback_cell = fmax_(fmax_(base_cell * ROUTING_CELL_FALLBACK_SCALE, base_cell + 2.0f), ROUTING_CELL_MIN);
    if (fallback_cell <= base_cell + 0.5f) return nullptr;
    float margin = fmax_(config.node_spacing, GRID_MARGIN_MIN_SPACING) * 2.0f;
    size_t max_cells = std::max<size_t>(config.flowchart.routing.max_steps / 14, 3000);
    return RoutingGrid::create(obstacles, fallback_cell, margin, max_cells);
}

// ---- extra candidate families ----

void push_preferred_label_detour_candidates(const RouteContext& ctx, Point route_start, Point route_end,
                                            const std::vector<Segment>& existing, bool use_existing,
                                            std::vector<RouteCandidate>& candidates) {
    if (!ctx.preferred_label_obstacle) return;
    Obstacle expanded = expand_obstacle(*ctx.preferred_label_obstacle, fmax_(ctx.preferred_label_clearance, 0.0f));
    float left = expanded.x, right = expanded.x + expanded.width, top = expanded.y, bottom = expanded.y + expanded.height;
    if (is_horizontal(ctx.direction)) {
        bool forward = route_end.first >= route_start.first;
        float near_x = forward ? left : right, far_x = forward ? right : left;
        for (float y : {top, bottom})
            push_route_candidate({route_start, {near_x, route_start.second}, {near_x, y}, {far_x, y}, {far_x, route_end.second}, route_end},
                                 ctx, existing, use_existing, candidates);
    } else {
        bool forward = route_end.second >= route_start.second;
        float near_y = forward ? top : bottom, far_y = forward ? bottom : top;
        for (float x : {left, right})
            push_route_candidate({route_start, {route_start.first, near_y}, {x, near_y}, {x, far_y}, {route_end.first, far_y}, route_end},
                                 ctx, existing, use_existing, candidates);
    }
}

std::optional<float> channel_candidate_score(const ReservedRoutingChannel& channel, Point route_start, Point route_end,
                                             const LayoutConfig& config) {
    float channel_pad = fmax_(config.node_spacing * 1.2f, 24.0f);
    float span_min, span_max, mid_coord;
    if (channel.axis == ReservedRoutingChannelAxis::Vertical) {
        span_min = fmin_(route_start.second, route_end.second);
        span_max = fmax_(route_start.second, route_end.second);
        mid_coord = (route_start.first + route_end.first) * 0.5f;
    } else {
        span_min = fmin_(route_start.first, route_end.first);
        span_max = fmax_(route_start.first, route_end.first);
        mid_coord = (route_start.second + route_end.second) * 0.5f;
    }
    float span_miss;
    if (span_max < channel.span_min - channel_pad) span_miss = channel.span_min - channel_pad - span_max;
    else if (span_min > channel.span_max + channel_pad) span_miss = span_min - channel.span_max - channel_pad;
    else span_miss = 0.0f;
    if (span_miss > channel_pad * 2.0f) return std::nullopt;
    return std::fabs(channel.coord - mid_coord) + span_miss * 1.5f;
}

void push_reserved_channel_candidates(const RouteContext& ctx, Point route_start, Point route_end,
                                      const std::vector<Segment>& existing, bool use_existing,
                                      std::vector<RouteCandidate>& candidates) {
    if (!ctx.reserved_channels || ctx.reserved_channels->empty()) return;
    float direct_manhattan = std::fabs(route_end.first - route_start.first) + std::fabs(route_end.second - route_start.second);
    float max_detour = direct_manhattan * 2.4f + ctx.config->node_spacing * 5.0f;
    std::vector<std::pair<float, ReservedRoutingChannel>> channels;
    for (const auto& channel : *ctx.reserved_channels)
        if (auto score = channel_candidate_score(channel, route_start, route_end, *ctx.config)) channels.push_back({*score, channel});
    std::stable_sort(channels.begin(), channels.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    size_t taken = 0;
    for (const auto& [score, channel] : channels) {
        if (taken++ >= 8) break;
        std::vector<Point> points;
        if (channel.axis == ReservedRoutingChannelAxis::Vertical) {
            float x = channel.coord;
            points = {route_start, {x, route_start.second}, {x, route_end.second}, route_end};
        } else {
            float y = channel.coord;
            points = {route_start, {route_start.first, y}, {route_end.first, y}, route_end};
        }
        if (path_length(points) > max_detour) continue;
        push_route_candidate(points, ctx, existing, use_existing, candidates);
    }
}

std::optional<std::tuple<float, float, float, float>> obstacle_bounds_for_exterior_fallback(const RouteContext& ctx, Point route_start,
                                                                                            Point route_end) {
    float min_x = fmin_(route_start.first, route_end.first), max_x = fmax_(route_start.first, route_end.first);
    float min_y = fmin_(route_start.second, route_end.second), max_y = fmax_(route_start.second, route_end.second);
    bool any = false;
    auto visit = [&](const Obstacle& o) {
        if (!std::isfinite(o.x) || !std::isfinite(o.y) || !std::isfinite(o.width) || !std::isfinite(o.height) ||
            o.width <= 0.0f || o.height <= 0.0f)
            return;
        min_x = fmin_(min_x, o.x);
        max_x = fmax_(max_x, o.x + o.width);
        min_y = fmin_(min_y, o.y);
        max_y = fmax_(max_y, o.y + o.height);
        any = true;
    };
    for (const auto& o : *ctx.obstacles) visit(o);
    for (const auto& o : *ctx.label_obstacles) visit(o);
    if (!any) return std::nullopt;
    return std::make_tuple(min_x, max_x, min_y, max_y);
}

void push_exterior_fallback_candidates(const RouteContext& ctx, Point route_start, Point route_end,
                                       const std::vector<Segment>& existing, bool use_existing,
                                       std::vector<RouteCandidate>& candidates) {
    auto bounds = obstacle_bounds_for_exterior_fallback(ctx, route_start, route_end);
    if (!bounds) return;
    auto [min_x, max_x, min_y, max_y] = *bounds;
    float pad = fmax_(ctx.config->node_spacing * EXTERIOR_FALLBACK_PAD_RATIO, EXTERIOR_FALLBACK_PAD_MIN);
    float left = min_x - pad, right = max_x + pad, top = min_y - pad, bottom = max_y + pad;
    for (float x : {left, right})
        push_route_candidate({route_start, {x, route_start.second}, {x, route_end.second}, route_end}, ctx, existing, use_existing, candidates);
    for (float y : {top, bottom})
        push_route_candidate({route_start, {route_start.first, y}, {route_end.first, y}, route_end}, ctx, existing, use_existing, candidates);
    for (float x : {left, right})
        for (float y : {top, bottom}) {
            push_route_candidate({route_start, {x, route_start.second}, {x, y}, {route_end.first, y}, route_end}, ctx, existing, use_existing, candidates);
            push_route_candidate({route_start, {route_start.first, y}, {x, y}, {x, route_end.second}, route_end}, ctx, existing, use_existing, candidates);
        }
}

}  // namespace

std::vector<Point> route_edge_with_avoidance(const RouteContext& ctx, const EdgeOccupancy* occupancy,
                                             const RoutingGrid* grid, const std::vector<Segment>* existing) {
    static const std::vector<Segment> kNoSegments;
    const std::vector<Segment>& existing_segments = existing ? *existing : kNoSegments;
    bool use_existing = !existing_segments.empty();

    if (*ctx.from_id == *ctx.to_id) {
        std::vector<RouteCandidate> candidates;
        float pad = fmax_(ctx.config->node_spacing, ROUTING_PAD_MIN_SPACING) * ROUTING_PAD_RATIO;
        for (auto& points : route_self_loop_candidates(*ctx.from, pad))
            push_route_candidate(points, ctx, existing_segments, use_existing, candidates);
        push_route_candidate(route_self_loop(*ctx.from, ctx.direction, *ctx.config), ctx, existing_segments, use_existing, candidates);

        if (candidates.empty()) return route_self_loop(*ctx.from, ctx.direction, *ctx.config);

        std::optional<size_t> best_idx;
        std::optional<RouteCandidateOrderKey> best_key;
        for (size_t idx = 0; idx < candidates.size(); ++idx) {
            std::optional<unsigned> occ;
            if (occupancy) occ = occupancy->score_path(candidates[idx].points);
            auto key = route_candidate_key(candidates[idx], occ);
            if (route_candidate_better(ctx, key, best_key)) {
                best_key = key;
                best_idx = idx;
            }
        }
        if (!best_idx) {
            auto fallback = compress_path(route_self_loop(*ctx.from, ctx.direction, *ctx.config));
            enforce_preferred_label_via(fallback, ctx);
            return apply_endpoint_insets(compress_path(fallback), ctx.start_inset, ctx.end_inset);
        }
        auto best = compress_path(candidates[*best_idx].points);
        enforce_preferred_label_via(best, ctx);
        return apply_endpoint_insets(compress_path(best), ctx.start_inset, ctx.end_inset);
    }

    bool is_backward = std::get<2>(edge_sides(*ctx.from, *ctx.to, ctx.direction));

    RouteEndpoints ep = resolve_route_endpoints(ctx);
    Point start = ep.start, end = ep.end, route_start = ep.route_start, route_end = ep.route_end;
    if (ctx.fast_route) {
        std::vector<Point> fast = compress_path({ep.start, ep.route_start, ep.route_end, ep.end});
        enforce_preferred_label_via(fast, ctx);
        return apply_endpoint_insets(compress_path(fast), ctx.start_inset, ctx.end_inset);
    }
    std::vector<RouteCandidate> candidates;

    // Backward edges: try going around the blocking obstacles on every side.
    if (is_backward) {
        float pad = fmax_(ctx.config->node_spacing, 30.0f);
        float min_left = std::numeric_limits<float>::max(), max_right = 0.0f;
        float min_top = std::numeric_limits<float>::max(), max_bottom = 0.0f;
        for (const auto& o : *ctx.obstacles) {
            if (o.id == *ctx.from_id || o.id == *ctx.to_id) continue;
            if (o.members && (o.members->count(*ctx.from_id) || o.members->count(*ctx.to_id))) continue;
            float obs_top = o.y, obs_bottom = o.y + o.height;
            float path_top = fmin_(end.second, start.second), path_bottom = fmax_(start.second, end.second);
            if (obs_top < path_bottom && obs_bottom > path_top) {
                min_left = fmin_(min_left, o.x);
                max_right = fmax_(max_right, o.x + o.width);
            }
            float obs_left = o.x, obs_right = o.x + o.width;
            float path_left = fmin_(start.first, end.first), path_right = fmax_(start.first, end.first);
            if (obs_left < path_right && obs_right > path_left) {
                min_top = fmin_(min_top, obs_top);
                max_bottom = fmax_(max_bottom, obs_bottom);
            }
        }
        if (max_right > 0.0f) {
            float route_x = max_right + pad;
            push_route_candidate({route_start, {route_x, route_start.second}, {route_x, route_end.second}, route_end}, ctx,
                                 existing_segments, use_existing, candidates);
        }
        if (max_bottom > 0.0f) {
            float route_y = max_bottom + pad;
            push_route_candidate({route_start, {route_start.first, route_y}, {route_end.first, route_y}, route_end}, ctx,
                                 existing_segments, use_existing, candidates);
        }
        if (min_top < std::numeric_limits<float>::max()) {
            float route_y = min_top - pad;
            push_route_candidate({route_start, {route_start.first, route_y}, {route_end.first, route_y}, route_end}, ctx,
                                 existing_segments, use_existing, candidates);
        }
        if (min_left < std::numeric_limits<float>::max()) {
            float route_x = min_left - pad;
            push_route_candidate({route_start, {route_x, route_start.second}, {route_x, route_end.second}, route_end}, ctx,
                                 existing_segments, use_existing, candidates);
        }
    }

    // Direct line (same-side back edges are excluded from diagonal shortcuts).
    bool direct_axis_aligned = std::fabs(route_start.first - route_end.first) <= 1e-3f ||
                               std::fabs(route_start.second - route_end.second) <= 1e-3f;
    if (direct_axis_aligned || !(is_backward && ctx.start_side == ctx.end_side))
        push_route_candidate({route_start, route_end}, ctx, existing_segments, use_existing, candidates);

    push_preferred_label_detour_candidates(ctx, route_start, route_end, existing_segments, use_existing, candidates);
    push_reserved_channel_candidates(ctx, route_start, route_end, existing_segments, use_existing, candidates);

    if (ctx.preferred_label_center) {
        float via_mid_x = ctx.preferred_label_center->first, via_mid_y = ctx.preferred_label_center->second;
        push_route_candidate({route_start, {via_mid_x, route_start.second}, {via_mid_x, via_mid_y}, {via_mid_x, route_end.second}, route_end},
                             ctx, existing_segments, use_existing, candidates);
        push_route_candidate({route_start, {route_start.first, via_mid_y}, {via_mid_x, via_mid_y}, {route_end.first, via_mid_y}, route_end},
                             ctx, existing_segments, use_existing, candidates);
    }

    // Orthogonal routes with control points at growing offsets.
    float step = fmax_(ctx.config->node_spacing, ORTHO_STEP_MIN_SPACING) * ROUTING_PAD_RATIO;
    std::vector<float> offsets{ctx.base_offset};
    for (int i = 1; i <= 6; ++i) {
        float delta = step * static_cast<float>(i);
        offsets.push_back(ctx.base_offset + delta);
        offsets.push_back(ctx.base_offset - delta);
    }
    float cross_axis_delta = is_horizontal(ctx.direction) ? std::fabs(route_end.second - route_start.second)
                                                          : std::fabs(route_end.first - route_start.first);
    bool use_channel_candidates = (cross_axis_delta > step * CHANNEL_CANDIDATE_RATIO && ctx.obstacles->size() > 10) ||
                                  is_backward || (ctx.start_side == ctx.end_side && ctx.obstacles->size() > 4);

    for (size_t offset_rank = 0; offset_rank < offsets.size(); ++offset_rank) {
        float offset = offsets[offset_rank];
        if (is_horizontal(ctx.direction)) {
            float mid_x = (route_start.first + route_end.first) / 2.0f + offset;
            push_route_candidate({route_start, {mid_x, route_start.second}, {mid_x, route_end.second}, route_end}, ctx,
                                 existing_segments, use_existing, candidates);
            float mid_y = (route_start.second + route_end.second) / 2.0f + offset;
            push_route_candidate({route_start, {route_start.first, mid_y}, {route_end.first, mid_y}, route_end}, ctx,
                                 existing_segments, use_existing, candidates);
            if (use_channel_candidates && offset_rank <= 3) {
                float near_start_x = route_start.first + offset;
                push_route_candidate({route_start, {near_start_x, route_start.second}, {near_start_x, route_end.second}, route_end},
                                     ctx, existing_segments, use_existing, candidates);
                float near_end_x = route_end.first + offset;
                push_route_candidate({route_start, {near_end_x, route_start.second}, {near_end_x, route_end.second}, route_end},
                                     ctx, existing_segments, use_existing, candidates);
            }
        } else {
            float mid_y = (route_start.second + route_end.second) / 2.0f + offset;
            push_route_candidate({route_start, {route_start.first, mid_y}, {route_end.first, mid_y}, route_end}, ctx,
                                 existing_segments, use_existing, candidates);
            float mid_x = (route_start.first + route_end.first) / 2.0f + offset;
            push_route_candidate({route_start, {mid_x, route_start.second}, {mid_x, route_end.second}, route_end}, ctx,
                                 existing_segments, use_existing, candidates);
            if (use_channel_candidates && offset_rank <= 3) {
                float near_start_y = route_start.second + offset;
                push_route_candidate({route_start, {route_start.first, near_start_y}, {route_end.first, near_start_y}, route_end},
                                     ctx, existing_segments, use_existing, candidates);
                float near_end_y = route_end.second + offset;
                push_route_candidate({route_start, {route_start.first, near_end_y}, {route_end.first, near_end_y}, route_end},
                                     ctx, existing_segments, use_existing, candidates);
            }
        }
    }

    auto min_of = [&](auto getter, auto init) {
        auto m = init;
        for (const auto& c : candidates) {
            auto v = getter(c);
            if (v < m) m = v;
        }
        return m;
    };
    size_t min_hits = candidates.empty() ? 0 : min_of([](const RouteCandidate& c) { return c.hits; }, std::numeric_limits<size_t>::max());
    size_t min_own_label_score = candidates.empty() ? 0 : min_of([](const RouteCandidate& c) { return own_label_score(c.own_label); }, std::numeric_limits<size_t>::max());
    size_t min_crossings = candidates.empty() ? 0 : min_of([](const RouteCandidate& c) { return c.cross; }, std::numeric_limits<size_t>::max());
    size_t min_label_hits = candidates.empty() ? 0 : min_of([](const RouteCandidate& c) { return c.label_hits; }, std::numeric_limits<size_t>::max());
    float min_overlap = std::numeric_limits<float>::infinity();
    for (const auto& c : candidates) min_overlap = fmin_(min_overlap, c.overlap);
    bool needs_detour = min_crossings > 0 || min_own_label_score > 0 || min_label_hits > 0 ||
                        (std::isfinite(min_overlap) && min_overlap >= OVERLAP_DETOUR_MIN);
    if (min_hits == 0 && occupancy) {
        size_t best_idx = 0;
        unsigned best_score = 0xFFFFFFFFu;
        size_t best_bends = std::numeric_limits<size_t>::max();
        float best_len = std::numeric_limits<float>::max();
        for (size_t idx = 0; idx < candidates.size(); ++idx) {
            unsigned score = occupancy->score_path(candidates[idx].points);
            size_t bends = candidates[idx].bends;
            float len = candidates[idx].len;
            bool better;
            if (ctx.prefer_shorter_ties)
                better = score < best_score || (score == best_score && len + ROUTE_LENGTH_TIE_EPS < best_len) ||
                         (score == best_score && std::fabs(len - best_len) <= ROUTE_LENGTH_TIE_EPS && bends < best_bends);
            else
                better = score < best_score || (score == best_score && bends < best_bends) ||
                         (score == best_score && bends == best_bends && len < best_len);
            if (better) {
                best_score = score;
                best_bends = bends;
                best_len = len;
                best_idx = idx;
            }
        }
        if (best_idx < candidates.size()) {
            unsigned overlap = occupancy->overlap_count(candidates[best_idx].points);
            float path_len = candidates[best_idx].len;
            unsigned overlap_trigger =
                f2u32(std::ceil(fmax_((path_len / occupancy->cell_size()) * OVERLAP_TRIGGER_RATIO, OVERLAP_TRIGGER_MIN)));
            if (overlap >= overlap_trigger) needs_detour = true;
        }
    }

    if (min_hits > 0 || needs_detour) {
        for (int i = 7; i <= 9; ++i) {
            float delta = step * static_cast<float>(i);
            for (float sign : {1.0f, -1.0f}) {
                float offset = ctx.base_offset + sign * delta;
                std::vector<Point> points;
                if (is_horizontal(ctx.direction)) {
                    float mid_x = (route_start.first + route_end.first) / 2.0f + offset;
                    points = {route_start, {mid_x, route_start.second}, {mid_x, route_end.second}, route_end};
                } else {
                    float mid_y = (route_start.second + route_end.second) / 2.0f + offset;
                    points = {route_start, {route_start.first, mid_y}, {route_end.first, mid_y}, route_end};
                }
                push_route_candidate(points, ctx, existing_segments, use_existing, candidates);
            }
        }
    }

    min_hits = candidates.empty() ? 0 : min_of([](const RouteCandidate& c) { return c.hits; }, std::numeric_limits<size_t>::max());
    if ((min_hits > 0 || needs_detour) && grid) {
        bool coarse_retry = false;
        if (auto points = route_edge_with_grid(ctx, *grid, occupancy, route_start, route_end)) {
            size_t before = candidates.size();
            push_route_candidate(*points, ctx, existing_segments, use_existing, candidates);
            if (candidates.size() > before) {
                const RouteCandidate& c = candidates.back();
                coarse_retry = c.hits > 0 || own_label_score(c.own_label) > 0 || c.label_hits > 0 || c.cross > 0 ||
                               c.overlap >= OVERLAP_DETOUR_MIN;
            }
        } else {
            coarse_retry = true;
        }
        if (coarse_retry && ctx.coarse_grid_retry) {
            if (auto coarse = build_fallback_routing_grid(*ctx.obstacles, *ctx.config, grid->cell))
                if (auto points = route_edge_with_grid(ctx, *coarse, occupancy, route_start, route_end))
                    push_route_candidate(*points, ctx, existing_segments, use_existing, candidates);
        }
    }

    size_t min_hard_hits = candidates.empty() ? std::numeric_limits<size_t>::max()
                                              : min_of([](const RouteCandidate& c) { return c.hard_hits; }, std::numeric_limits<size_t>::max());
    if (ctx.allow_exterior_fallback && !occupancy && (candidates.empty() || min_hard_hits > 0))
        push_exterior_fallback_candidates(ctx, route_start, route_end, existing_segments, use_existing, candidates);

    std::optional<size_t> best_idx;
    std::optional<RouteCandidateOrderKey> best_key;
    for (size_t idx = 0; idx < candidates.size(); ++idx) {
        std::optional<unsigned> occ;
        if (occupancy) occ = occupancy->score_path(candidates[idx].points);
        auto key = route_candidate_key(candidates[idx], occ);
        if (route_candidate_better(ctx, key, best_key)) {
            best_idx = idx;
            best_key = key;
        }
    }
    if (!best_idx) return finish(ep, ctx, {route_start, route_end});
    return finish(ep, ctx, candidates[*best_idx].points);
}

}  // namespace mermaid
