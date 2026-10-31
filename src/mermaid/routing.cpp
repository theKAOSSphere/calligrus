// Mermaid port of layout/routing.rs, part 1: sides, ports, occupancy map, routing grid, obstacles
// and the scoring helpers. The router itself (candidate paths, A*) is in router.cpp.
#include "routing.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "strutil.h"

namespace mermaid {

namespace {

constexpr float DIRECTION_PREF_RATIO = 1.35f;
constexpr float ROUTE_DETOUR_THRESHOLD = 120.0f;
constexpr float SIDE_LOAD_SOFT_CAP = 6.0f;
constexpr float HUB_DIVERSIFY_GEOM_RATIO = 1.35f;
constexpr size_t HUB_DIVERSIFY_LOAD_SUM = 14;
constexpr size_t LOW_DEGREE_BALANCE_MIN_PRIMARY_LOAD = 4;

constexpr float PORT_STUB_RATIO = 0.35f;
constexpr float PORT_STUB_SIZE_CAP_RATIO = 0.35f;
constexpr float PORT_STUB_DEFAULT_MAX = 18.0f;
constexpr float PORT_STUB_MIN = 6.0f;
constexpr float PORT_STUB_MAX = 22.0f;

constexpr float ROUTING_CELL_RATIO = 0.35f;
constexpr float ROUTING_CELL_MIN = 8.0f;
constexpr float GRID_MARGIN_MIN_SPACING = 24.0f;

constexpr float OBSTACLE_PAD_RATIO = 0.35f;
constexpr float OBSTACLE_PAD_MIN = 6.0f;
constexpr float LABEL_OBSTACLE_NODE_PAD = 2.0f;
constexpr float LABEL_OBSTACLE_SUB_PAD = 3.0f;

float fmax_(float a, float b) { return a > b ? a : (a < b ? b : a); }
float fmin_(float a, float b) { return a < b ? a : (a > b ? b : a); }
float clamp_(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

// ======================================================================================
// sides
// ======================================================================================

bool side_is_vertical(EdgeSide side) { return side == EdgeSide::Left || side == EdgeSide::Right; }
PortAxis port_axis(EdgeSide side) { return side_is_vertical(side) ? PortAxis::Y : PortAxis::X; }
bool edge_axis_is_horizontal(EdgeSide side) { return side_is_vertical(side); }

size_t side_slot(EdgeSide side) {
    switch (side) {
    case EdgeSide::Left: return 0;
    case EdgeSide::Right: return 1;
    case EdgeSide::Top: return 2;
    case EdgeSide::Bottom: return 3;
    }
    return 0;
}

size_t side_load_for_node(const SideLoads& loads, const std::string& node_id, EdgeSide side) {
    auto it = loads.find(node_id);
    return it == loads.end() ? 0 : it->second[side_slot(side)];
}

void bump_side_load(SideLoads& loads, const std::string& node_id, EdgeSide side) {
    auto it = loads.find(node_id);
    if (it == loads.end()) it = loads.emplace(node_id, std::array<size_t, 4>{0, 0, 0, 0}).first;
    it->second[side_slot(side)] += 1;
}

std::tuple<EdgeSide, EdgeSide, bool> edge_sides(const NodeLayout& from, const NodeLayout& to, Direction direction) {
    float from_cx = from.x + from.width / 2.0f;
    float from_cy = from.y + from.height / 2.0f;
    float to_cx = to.x + to.width / 2.0f;
    float to_cy = to.y + to.height / 2.0f;
    float dx = to_cx - from_cx;
    float dy = to_cy - from_cy;
    bool x_overlap = std::fabs(fmax_(from.x, to.x) - fmin_(from.x + from.width, to.x + to.width)) < 1e-3f ||
                     (from.x < to.x + to.width && to.x < from.x + from.width);
    bool y_overlap = std::fabs(fmax_(from.y, to.y) - fmin_(from.y + from.height, to.y + to.height)) < 1e-3f ||
                     (from.y < to.y + to.height && to.y < from.y + from.height);

    float ratio = std::fabs(dx) / fmax_(std::fabs(dy), 1e-3f);
    bool horiz_pref = ratio > DIRECTION_PREF_RATIO || (y_overlap && ratio > 0.9f);
    bool vert_pref = ratio < (1.0f / DIRECTION_PREF_RATIO) || (x_overlap && ratio < 1.1f);
    bool use_horizontal;
    if (horiz_pref && !vert_pref) use_horizontal = true;
    else if (vert_pref && !horiz_pref) use_horizontal = false;
    else use_horizontal = is_horizontal(direction);

    if (use_horizontal) {
        bool is_backward = to.x + to.width < from.x;
        if (dx >= 0.0f) return {EdgeSide::Right, EdgeSide::Left, is_backward};
        return {EdgeSide::Left, EdgeSide::Right, is_backward};
    }
    bool is_backward = to.y + to.height < from.y;
    if (dy >= 0.0f) return {EdgeSide::Bottom, EdgeSide::Top, is_backward};
    return {EdgeSide::Top, EdgeSide::Bottom, is_backward};
}

std::tuple<EdgeSide, EdgeSide, bool> edge_sides_balanced(
    const std::string& from_id, const std::string& to_id, const NodeLayout& from, const NodeLayout& to,
    bool allow_low_degree_balancing, bool prefer_outer_sides, Direction direction,
    const std::unordered_map<std::string, size_t>& node_degrees, const SideLoads& side_loads) {
    auto primary = edge_sides(from, to, direction);
    if (prefer_outer_sides) {
        if (is_horizontal(direction)) {
            if ((to.y + to.height / 2.0f) >= (from.y + from.height / 2.0f))
                return {EdgeSide::Bottom, EdgeSide::Bottom, std::get<2>(primary)};
            return {EdgeSide::Top, EdgeSide::Top, std::get<2>(primary)};
        }
        if ((to.x + to.width / 2.0f) >= (from.x + from.width / 2.0f))
            return {EdgeSide::Right, EdgeSide::Right, std::get<2>(primary)};
        return {EdgeSide::Left, EdgeSide::Left, std::get<2>(primary)};
    }
    auto deg = [&](const std::string& id) {
        auto it = node_degrees.find(id);
        return it == node_degrees.end() ? size_t(0) : it->second;
    };
    size_t from_degree = deg(from_id), to_degree = deg(to_id);
    if (from_degree < 6 && to_degree < 6) {
        if (!allow_low_degree_balancing) return primary;
        size_t primary_load = side_load_for_node(side_loads, from_id, std::get<0>(primary)) +
                              side_load_for_node(side_loads, to_id, std::get<1>(primary));
        if (primary_load < LOW_DEGREE_BALANCE_MIN_PRIMARY_LOAD && !prefer_outer_sides) return primary;
    }

    float from_cx = from.x + from.width / 2.0f, from_cy = from.y + from.height / 2.0f;
    float to_cx = to.x + to.width / 2.0f, to_cy = to.y + to.height / 2.0f;
    float dx = to_cx - from_cx, dy = to_cy - from_cy;

    // Hub-to-leaf edges: keep the diagram's main axis unless it is clearly better to spread out.
    if ((from_degree >= 10 && to_degree <= 4) || (to_degree >= 10 && from_degree <= 4)) {
        std::tuple<EdgeSide, EdgeSide, bool> forced;
        if (is_horizontal(direction)) {
            bool is_backward = to.x + to.width < from.x;
            forced = dx >= 0.0f ? std::make_tuple(EdgeSide::Right, EdgeSide::Left, is_backward)
                                : std::make_tuple(EdgeSide::Left, EdgeSide::Right, is_backward);
        } else {
            bool is_backward = to.y + to.height < from.y;
            forced = dy >= 0.0f ? std::make_tuple(EdgeSide::Bottom, EdgeSide::Top, is_backward)
                                : std::make_tuple(EdgeSide::Top, EdgeSide::Bottom, is_backward);
        }
        size_t forced_load = side_load_for_node(side_loads, from_id, std::get<0>(forced)) +
                             side_load_for_node(side_loads, to_id, std::get<1>(forced));
        float main_axis = is_horizontal(direction) ? std::fabs(dx) : std::fabs(dy);
        float cross_axis = is_horizontal(direction) ? std::fabs(dy) : std::fabs(dx);
        bool can_diversify = forced_load >= HUB_DIVERSIFY_LOAD_SUM && cross_axis > main_axis * HUB_DIVERSIFY_GEOM_RATIO;
        if (!can_diversify) return forced;
    }

    std::tuple<EdgeSide, EdgeSide, bool> horizontal =
        dx >= 0.0f ? std::make_tuple(EdgeSide::Right, EdgeSide::Left, to.x + to.width < from.x)
                   : std::make_tuple(EdgeSide::Left, EdgeSide::Right, to.x > from.x + from.width);
    std::tuple<EdgeSide, EdgeSide, bool> vertical =
        dy >= 0.0f ? std::make_tuple(EdgeSide::Bottom, EdgeSide::Top, to.y + to.height < from.y)
                   : std::make_tuple(EdgeSide::Top, EdgeSide::Bottom, to.y > from.y + from.height);

    std::vector<std::tuple<EdgeSide, EdgeSide, bool>> options{primary};
    auto has = [&](const std::tuple<EdgeSide, EdgeSide, bool>& t) {
        for (const auto& o : options)
            if (std::get<0>(o) == std::get<0>(t) && std::get<1>(o) == std::get<1>(t)) return true;
        return false;
    };
    if (!has(horizontal)) options.push_back(horizontal);
    if (!has(vertical)) options.push_back(vertical);

    bool primary_axis = edge_axis_is_horizontal(std::get<0>(primary));
    Point primary_from_anchor = anchor_point_for_node(from, std::get<0>(primary), 0.0f);
    Point primary_to_anchor = anchor_point_for_node(to, std::get<1>(primary), 0.0f);
    float primary_manhattan = std::fabs(primary_to_anchor.first - primary_from_anchor.first) +
                              std::fabs(primary_to_anchor.second - primary_from_anchor.second);
    auto best = primary;
    float best_score = std::numeric_limits<float>::max();
    float best_tiebreak = std::numeric_limits<float>::max();
    for (const auto& opt : options) {
        auto [start_side, end_side, is_backward] = opt;
        float from_load = static_cast<float>(side_load_for_node(side_loads, from_id, start_side));
        float to_load = static_cast<float>(side_load_for_node(side_loads, to_id, end_side));
        float load_score = from_load * from_load + to_load * to_load + (from_load + to_load) * 0.5f;
        float overload = fmax_(from_load - SIDE_LOAD_SOFT_CAP, 0.0f) + fmax_(to_load - SIDE_LOAD_SOFT_CAP, 0.0f);
        float overload_penalty = overload * overload * 6.0f;
        Point from_anchor = anchor_point_for_node(from, start_side, 0.0f);
        Point to_anchor = anchor_point_for_node(to, end_side, 0.0f);
        float manhattan = std::fabs(to_anchor.first - from_anchor.first) + std::fabs(to_anchor.second - from_anchor.second);
        bool is_primary = start_side == std::get<0>(primary) && end_side == std::get<1>(primary);
        if (!is_primary && manhattan > primary_manhattan * DIRECTION_PREF_RATIO + ROUTE_DETOUR_THRESHOLD) continue;
        float axis_penalty = edge_axis_is_horizontal(start_side) == primary_axis ? 0.0f : 5.0f;
        float outer_side_penalty = 0.0f;
        if (prefer_outer_sides && edge_axis_is_horizontal(start_side) == primary_axis) outer_side_penalty = 6.5f;
        float primary_penalty = is_primary ? 0.0f : 2.0f;
        float backward_penalty = (is_backward && !std::get<2>(primary)) ? 4.0f : 0.0f;
        float score = load_score * 9.0f + overload_penalty + manhattan * 0.22f + axis_penalty + outer_side_penalty +
                      primary_penalty + backward_penalty;
        float tiebreak = manhattan + from_load + to_load;
        if (score < best_score || (std::fabs(score - best_score) < 1e-4f && tiebreak < best_tiebreak)) {
            best = opt;
            best_score = score;
            best_tiebreak = tiebreak;
        }
    }
    return best;
}

// ======================================================================================
// ports
// ======================================================================================

Point apply_port_offset(Point point, EdgeSide side, float offset) {
    if (side == EdgeSide::Left || side == EdgeSide::Right) return {point.first, point.second + offset};
    return {point.first + offset, point.second};
}

float port_stub_length(const LayoutConfig& config, const NodeLayout& from, const NodeLayout& to) {
    float base = config.node_spacing * PORT_STUB_RATIO;
    float size_cap = fmin_(fmin_(from.width, from.height), fmin_(to.width, to.height)) * PORT_STUB_SIZE_CAP_RATIO;
    float max_len = (std::isfinite(size_cap) && size_cap > 0.0f) ? size_cap : PORT_STUB_DEFAULT_MAX;
    return clamp_(fmin_(base, max_len), PORT_STUB_MIN, PORT_STUB_MAX);
}

Point port_stub_point(Point point, EdgeSide side, float length) {
    switch (side) {
    case EdgeSide::Left: return {point.first - length, point.second};
    case EdgeSide::Right: return {point.first + length, point.second};
    case EdgeSide::Top: return {point.first, point.second - length};
    case EdgeSide::Bottom: return {point.first, point.second + length};
    }
    return point;
}

// Where a straight line from `remote` to the node's centre crosses the node's boundary on `side`
// (coordinate along the side's axis).
float ideal_port_pos(Point remote, const NodeLayout& node, EdgeSide side) {
    float cx = node.x + node.width / 2.0f;
    float cy = node.y + node.height / 2.0f;
    if (side_is_vertical(side)) {
        float edge_x = side == EdgeSide::Left ? node.x : node.x + node.width;
        float dx = cx - remote.first;
        if (std::fabs(dx) < 1.0f) return cy;
        float t = (edge_x - remote.first) / dx;
        return remote.second + t * (cy - remote.second);
    }
    float edge_y = side == EdgeSide::Top ? node.y : node.y + node.height;
    float dy = cy - remote.second;
    if (std::fabs(dy) < 1.0f) return cx;
    float t = (edge_y - remote.second) / dy;
    return remote.first + t * (cx - remote.first);
}

Point anchor_point_for_node(const NodeLayout& node, EdgeSide side, float offset) {
    float cx = node.x + node.width / 2.0f;
    float cy = node.y + node.height / 2.0f;
    Point dir, perp;
    float max_offset;
    switch (side) {
    case EdgeSide::Left: dir = {-1.0f, 0.0f}; perp = {0.0f, 1.0f}; max_offset = node.height / 2.0f - 1.0f; break;
    case EdgeSide::Right: dir = {1.0f, 0.0f}; perp = {0.0f, 1.0f}; max_offset = node.height / 2.0f - 1.0f; break;
    case EdgeSide::Top: dir = {0.0f, -1.0f}; perp = {1.0f, 0.0f}; max_offset = node.width / 2.0f - 1.0f; break;
    default: dir = {0.0f, 1.0f}; perp = {1.0f, 0.0f}; max_offset = node.width / 2.0f - 1.0f; break;
    }
    float clamp = max_offset > 0.0f ? clamp_(offset, -max_offset, max_offset) : 0.0f;
    Point origin{cx + perp.first * clamp, cy + perp.second * clamp};

    if (node.shape == NodeShape::Circle || node.shape == NodeShape::DoubleCircle) {
        float rx = node.width / 2.0f;
        float ry = node.height / 2.0f;
        if (auto p = ray_ellipse_intersection(origin, dir, {cx, cy}, rx, ry)) return *p;
    }
    if (auto poly = shape_polygon_points(node))
        if (auto p = ray_polygon_intersection(origin, dir, *poly)) return *p;

    // Fall back to the bounding-box anchor.
    Point base;
    switch (side) {
    case EdgeSide::Left: base = {node.x, cy}; break;
    case EdgeSide::Right: base = {node.x + node.width, cy}; break;
    case EdgeSide::Top: base = {cx, node.y}; break;
    default: base = {cx, node.y + node.height}; break;
    }
    return apply_port_offset(base, side, clamp);
}

// ======================================================================================
// occupancy map
// ======================================================================================

EdgeOccupancy::EdgeOccupancy(float cell) : cell_(fmax_(cell, 8.0f)) {}

std::pair<int, int> EdgeOccupancy::cell_index(float x, float y) const {
    return {f2i(std::floor(x / cell_)), f2i(std::floor(y / cell_))};
}

unsigned EdgeOccupancy::score_path(const std::vector<Point>& points) const {
    unsigned score = 0;
    for (size_t s = 0; s + 1 < points.size(); ++s) {
        auto [x1, y1] = points[s];
        auto [x2, y2] = points[s + 1];
        float dx = x2 - x1, dy = y2 - y1;
        float len = std::sqrt(dx * dx + dy * dy);
        size_t steps = std::max<size_t>(f2usize(std::ceil(len / cell_)), 1);
        size_t stride = steps > 32 ? std::max<size_t>(steps / 32, 1) : 1;
        for (size_t i = 0; i <= steps; i += stride) {
            float t = static_cast<float>(i) / static_cast<float>(steps);
            float x = x1 + dx * t, y = y1 + dy * t;
            auto it = weights_.find(cell_index(x, y));
            if (it != weights_.end()) score += it->second;
        }
    }
    return score;
}

unsigned EdgeOccupancy::overlap_count(const std::vector<Point>& points) const {
    unsigned count = 0;
    for (size_t s = 0; s + 1 < points.size(); ++s) {
        auto [x1, y1] = points[s];
        auto [x2, y2] = points[s + 1];
        float dx = x2 - x1, dy = y2 - y1;
        float len = std::sqrt(dx * dx + dy * dy);
        size_t steps = std::max<size_t>(f2usize(std::ceil(len / cell_)), 1);
        for (size_t i = 0; i <= steps; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(steps);
            float x = x1 + dx * t, y = y1 + dy * t;
            auto it = weights_.find(cell_index(x, y));
            if (it != weights_.end() && it->second > 0) ++count;
        }
    }
    return count;
}

void EdgeOccupancy::add_path_with_weight(const std::vector<Point>& points, unsigned multiplier) {
    multiplier = std::max(multiplier, 1u);
    for (size_t s = 0; s + 1 < points.size(); ++s) {
        auto [x1, y1] = points[s];
        auto [x2, y2] = points[s + 1];
        float dx = x2 - x1, dy = y2 - y1;
        float len = std::sqrt(dx * dx + dy * dy);
        size_t steps = std::max<size_t>(f2usize(std::ceil(len / cell_)), 1);
        for (size_t i = 0; i <= steps; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(steps);
            float x = x1 + dx * t, y = y1 + dy * t;
            auto [ix, iy] = cell_index(x, y);
            for (int dxc = -1; dxc <= 1; ++dxc)
                for (int dyc = -1; dyc <= 1; ++dyc) {
                    unsigned base;
                    int ax = std::abs(dxc), ay = std::abs(dyc);
                    if (ax == 0 && ay == 0) base = 3;
                    else if ((ax == 1 && ay == 0) || (ax == 0 && ay == 1)) base = 2;
                    else base = 1;
                    unsigned weight = std::min(base * multiplier, 65535u);  // u16 saturating_mul
                    unsigned& entry = weights_[{ix + dxc, iy + dyc}];
                    entry = std::min(entry + weight, 65535u);  // u16 saturating_add
                }
        }
    }
}

void EdgeOccupancy::merge_from(const EdgeOccupancy& other) {
    for (const auto& [cell, weight] : other.weights_) {
        unsigned& entry = weights_[cell];
        entry = std::min(entry + weight, 65535u);
    }
}

unsigned EdgeOccupancy::weight_at(float x, float y) const {
    auto it = weights_.find(cell_index(x, y));
    return it == weights_.end() ? 0 : it->second;
}

// ======================================================================================
// routing grid
// ======================================================================================

std::unique_ptr<RoutingGrid> RoutingGrid::create(const std::vector<Obstacle>& obstacles, float cell_in, float margin,
                                                 size_t max_cells) {
    float min_x = std::numeric_limits<float>::max(), min_y = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest(), max_y = std::numeric_limits<float>::lowest();
    for (const auto& o : obstacles) {
        min_x = fmin_(min_x, o.x);
        min_y = fmin_(min_y, o.y);
        max_x = fmax_(max_x, o.x + o.width);
        max_y = fmax_(max_y, o.y + o.height);
    }
    if (min_x == std::numeric_limits<float>::max()) return nullptr;
    min_x -= margin;
    min_y -= margin;
    max_x += margin;
    max_y += margin;
    float cell = fmax_(cell_in, 6.0f);
    int cols = f2i(std::ceil((max_x - min_x) / cell)) + 1;
    int rows = f2i(std::ceil((max_y - min_y) / cell)) + 1;
    if (cols <= 1 || rows <= 1) return nullptr;
    size_t total_cells = static_cast<size_t>(cols) * static_cast<size_t>(rows);
    if (total_cells > max_cells) return nullptr;
    auto g = std::make_unique<RoutingGrid>();
    g->cell = cell;
    g->min_x = min_x;
    g->min_y = min_y;
    g->cols = cols;
    g->rows = rows;
    g->cell_obstacles.assign(static_cast<size_t>(cols) * static_cast<size_t>(rows), {});
    for (size_t idx = 0; idx < obstacles.size(); ++idx) {
        const Obstacle& obs = obstacles[idx];
        int start_x = f2i(fmax_(std::floor((obs.x - min_x) / cell), 0.0f));
        int end_x = f2i(fmin_(std::floor((obs.x + obs.width - min_x) / cell), static_cast<float>(cols - 1)));
        int start_y = f2i(fmax_(std::floor((obs.y - min_y) / cell), 0.0f));
        int end_y = f2i(fmin_(std::floor((obs.y + obs.height - min_y) / cell), static_cast<float>(rows - 1)));
        for (int iy = start_y; iy <= end_y; ++iy)
            for (int ix = start_x; ix <= end_x; ++ix) g->cell_obstacles[static_cast<size_t>(iy * cols + ix)].push_back(idx);
    }
    return g;
}

std::optional<std::pair<int, int>> RoutingGrid::cell_for_point(float x, float y) const {
    int ix = f2i(std::floor((x - min_x) / cell));
    int iy = f2i(std::floor((y - min_y) / cell));
    if (ix < 0 || iy < 0 || ix >= cols || iy >= rows) return std::nullopt;
    return std::make_pair(ix, iy);
}

Point RoutingGrid::cell_center(int ix, int iy) const {
    return {min_x + (static_cast<float>(ix) + 0.5f) * cell, min_y + (static_cast<float>(iy) + 0.5f) * cell};
}

float routing_cell_size(const LayoutConfig& config) {
    float cell = config.flowchart.routing.grid_cell;
    if (cell <= 0.0f) cell = config.node_spacing * ROUTING_CELL_RATIO;
    return fmax_(cell, ROUTING_CELL_MIN);
}

std::unique_ptr<RoutingGrid> build_routing_grid(const std::vector<Obstacle>& obstacles, const LayoutConfig& config) {
    float cell = routing_cell_size(config);
    float margin = fmax_(config.node_spacing, GRID_MARGIN_MIN_SPACING) * 2.0f;
    size_t max_cells = std::max<size_t>(config.flowchart.routing.max_steps / 16, 3000);
    return RoutingGrid::create(obstacles, cell, margin, max_cells);
}

// ======================================================================================
// obstacles
// ======================================================================================

std::vector<Obstacle> build_obstacles(const std::map<std::string, NodeLayout>& nodes,
                                      const std::vector<SubgraphLayout>& subgraphs, const LayoutConfig& config) {
    std::vector<Obstacle> obstacles;
    float pad = fmax_(config.node_spacing * OBSTACLE_PAD_RATIO, OBSTACLE_PAD_MIN);
    for (const auto& [id, node] : nodes) {
        if (node.hidden || node.anchor_subgraph) continue;
        obstacles.push_back({node.id, node.x - pad, node.y - pad, node.width + pad * 2.0f, node.height + pad * 2.0f, nullptr});
    }
    for (size_t idx = 0; idx < subgraphs.size(); ++idx) {
        const SubgraphLayout& sub = subgraphs[idx];
        bool invisible_region = trim_view(sub.label).empty() && sub.style.stroke == std::optional<std::string>("none") &&
                                sub.style.fill == std::optional<std::string>("none");
        if (invisible_region) continue;
        auto members = std::make_shared<std::set<std::string>>(sub.nodes.begin(), sub.nodes.end());
        for (const auto& [id, node] : nodes)
            if (node.anchor_subgraph == idx) members->insert(node.id);
        obstacles.push_back({"subgraph:" + sub.label, sub.x - pad, sub.y - pad, sub.width + pad * 2.0f,
                             sub.height + pad * 2.0f, members});
    }
    return obstacles;
}

std::vector<Obstacle> build_label_obstacles_for_routing(const std::map<std::string, NodeLayout>& nodes,
                                                        const std::vector<SubgraphLayout>& subgraphs) {
    std::vector<Obstacle> obstacles;
    float node_pad = LABEL_OBSTACLE_NODE_PAD;
    for (const auto& [id, node] : nodes) {
        if (node.hidden || node.anchor_subgraph) continue;
        bool all_blank = true;
        for (const auto& line : node.label.lines)
            if (!trim_view(line).empty()) all_blank = false;
        if (node.label.width <= 0.0f || node.label.height <= 0.0f || all_blank) continue;
        float x = node.x + (node.width - node.label.width) / 2.0f - node_pad;
        float y = node.y + (node.height - node.label.height) / 2.0f - node_pad;
        obstacles.push_back({"node-label:" + node.id, x, y, node.label.width + node_pad * 2.0f,
                             node.label.height + node_pad * 2.0f, nullptr});
    }
    float sub_pad = LABEL_OBSTACLE_SUB_PAD;
    for (const auto& sub : subgraphs) {
        if (trim_view(sub.label).empty() || sub.label_block.width <= 0.0f || sub.label_block.height <= 0.0f) continue;
        float x = sub.x + 12.0f - sub_pad;
        float y = sub.y + 6.0f - sub_pad;
        obstacles.push_back({"subgraph-label:" + sub.label, x, y, sub.label_block.width + sub_pad * 2.0f,
                             sub.label_block.height + sub_pad * 2.0f, nullptr});
    }
    return obstacles;
}

std::pair<std::string, std::string> edge_pair_key(const Edge& edge) {
    if (edge.from <= edge.to) return {edge.from, edge.to};
    return {edge.to, edge.from};
}

std::map<std::pair<std::string, std::string>, size_t> build_edge_pair_counts(const std::vector<Edge>& edges) {
    std::map<std::pair<std::string, std::string>, size_t> counts;
    for (const auto& e : edges) ++counts[edge_pair_key(e)];
    return counts;
}

bool segment_intersects_rect(Point a, Point b, const Obstacle& rect) {
    return segment_intersects_rect_bounds(a, b, {rect.x, rect.y, rect.width, rect.height});
}

// ======================================================================================
// scoring helpers
// ======================================================================================

float collinear_overlap_length(Point a, Point b, Point c, Point d) {
    float cross1 = (b.first - a.first) * (c.second - a.second) - (b.second - a.second) * (c.first - a.first);
    float cross2 = (b.first - a.first) * (d.second - a.second) - (b.second - a.second) * (d.first - a.first);
    if (std::fabs(cross1) > 1e-6f || std::fabs(cross2) > 1e-6f) return 0.0f;
    float dx = b.first - a.first;
    float dy = b.second - a.second;
    float seg_len_sq = dx * dx + dy * dy;
    if (seg_len_sq < 1e-6f) return 0.0f;
    auto proj = [&](Point p) { return ((p.first - a.first) * dx + (p.second - a.second) * dy) / seg_len_sq; };
    float t1 = proj(c), t2 = proj(d);
    float tmin = fmin_(t1, t2), tmax = fmax_(t1, t2);
    float overlap = fmax_(fmin_(tmax, 1.0f) - fmax_(tmin, 0.0f), 0.0f);
    return overlap * std::sqrt(seg_len_sq);
}

std::pair<size_t, float> edge_crossings_with_existing(const std::vector<Point>& points, const std::vector<Segment>& existing) {
    if (points.size() < 2 || existing.empty()) return {0, 0.0f};
    size_t crossings = 0;
    float overlap = 0.0f;
    for (size_t s = 0; s + 1 < points.size(); ++s) {
        Point a1 = points[s], a2 = points[s + 1];
        for (const auto& [b1, b2] : existing) {
            if ((std::fabs(a1.first - b1.first) < 1e-6f && std::fabs(a1.second - b1.second) < 1e-6f) ||
                (std::fabs(a1.first - b2.first) < 1e-6f && std::fabs(a1.second - b2.second) < 1e-6f) ||
                (std::fabs(a2.first - b1.first) < 1e-6f && std::fabs(a2.second - b1.second) < 1e-6f) ||
                (std::fabs(a2.first - b2.first) < 1e-6f && std::fabs(a2.second - b2.second) < 1e-6f))
                continue;
            overlap += collinear_overlap_length(a1, a2, b1, b2);
            if (segments_intersect(a1, a2, b1, b2)) ++crossings;
        }
    }
    return {crossings, overlap};
}

size_t path_obstacle_intersections(const std::vector<Point>& points, const std::vector<Obstacle>& obstacles,
                                   const std::string& from_id, const std::string& to_id) {
    if (points.size() < 2) return 0;
    size_t count = 0;
    for (size_t s = 0; s + 1 < points.size(); ++s) {
        for (const auto& o : obstacles) {
            if (o.id == from_id || o.id == to_id) continue;
            if (o.members && (o.members->count(from_id) || o.members->count(to_id))) continue;
            if (segment_intersects_rect(points[s], points[s + 1], o)) ++count;
        }
    }
    return count;
}

size_t path_label_intersections(const std::vector<Point>& points, const std::vector<Obstacle>& label_obstacles,
                                const std::optional<std::string>& ignore_label_id) {
    if (points.size() < 2 || label_obstacles.empty()) return 0;
    size_t count = 0;
    for (size_t s = 0; s + 1 < points.size(); ++s) {
        for (const auto& o : label_obstacles) {
            if (ignore_label_id && *ignore_label_id == o.id) continue;
            if (segment_intersects_rect(points[s], points[s + 1], o)) ++count;
        }
    }
    return count;
}

std::vector<Point> compress_path(const std::vector<Point>& points) {
    if (points.size() <= 2) return points;
    std::vector<Point> out;
    out.reserve(points.size());
    out.push_back(points[0]);
    for (size_t idx = 1; idx + 1 < points.size(); ++idx) {
        Point prev = out.back();
        Point curr = points[idx];
        if (std::fabs(curr.first - prev.first) <= 1e-4f && std::fabs(curr.second - prev.second) <= 1e-4f) continue;
        if (idx == 1 || idx == points.size() - 2) {
            out.push_back(curr);
            continue;
        }
        Point next = points[idx + 1];
        float dx1 = curr.first - prev.first, dy1 = curr.second - prev.second;
        float dx2 = next.first - curr.first, dy2 = next.second - curr.second;
        if ((std::fabs(dx1) <= 1e-4f && std::fabs(dx2) <= 1e-4f) || (std::fabs(dy1) <= 1e-4f && std::fabs(dy2) <= 1e-4f)) {
            // Keep explicit reversal points (U-turns).
            float dot = dx1 * dx2 + dy1 * dy2;
            if (dot >= 0.0f) continue;
        }
        out.push_back(curr);
    }
    Point last = points.back();
    if (std::fabs(last.first - out.back().first) > 1e-4f || std::fabs(last.second - out.back().second) > 1e-4f)
        out.push_back(last);
    return out;
}

namespace {
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
}  // namespace

float polyline_point_distance(const std::vector<Point>& points, Point point) {
    if (points.empty()) return std::numeric_limits<float>::infinity();
    if (points.size() == 1) {
        float dx = points[0].first - point.first, dy = points[0].second - point.second;
        return std::sqrt(dx * dx + dy * dy);
    }
    float best = std::numeric_limits<float>::infinity();
    for (size_t s = 0; s + 1 < points.size(); ++s) best = fmin_(best, point_segment_distance(points[s], points[s + 1], point));
    return best;
}

void insert_label_via_point(std::vector<Point>& points, Point via) {
    if (points.size() < 2) return;
    if (polyline_point_distance(points, via) <= 0.6f) return;
    // Insert on the segment where the detour through `via` adds the least length.
    std::optional<size_t> best_idx;
    float best_delta = std::numeric_limits<float>::infinity();
    for (size_t i = 1; i < points.size(); ++i) {
        Point a = points[i - 1], b = points[i];
        float base_len = std::sqrt((b.first - a.first) * (b.first - a.first) + (b.second - a.second) * (b.second - a.second));
        if (base_len <= 1e-4f) continue;
        float via_len_a = std::sqrt((via.first - a.first) * (via.first - a.first) + (via.second - a.second) * (via.second - a.second));
        float via_len_b = std::sqrt((via.first - b.first) * (via.first - b.first) + (via.second - b.second) * (via.second - b.second));
        float delta = fmax_(via_len_a + via_len_b - base_len, 0.0f);
        if (delta < best_delta) {
            best_delta = delta;
            best_idx = i;
        }
    }
    if (best_idx) {
        size_t i = *best_idx;
        float dist_a = std::sqrt((via.first - points[i - 1].first) * (via.first - points[i - 1].first) +
                                 (via.second - points[i - 1].second) * (via.second - points[i - 1].second));
        float dist_b = std::sqrt((via.first - points[i].first) * (via.first - points[i].first) +
                                 (via.second - points[i].second) * (via.second - points[i].second));
        if (dist_a > 2.0f && dist_b > 2.0f) points.insert(points.begin() + static_cast<long>(i), via);
        return;
    }
    size_t mid = points.size() / 2;
    points.insert(points.begin() + static_cast<long>(mid), via);
}

std::optional<Point> edge_label_anchor_from_points(const std::vector<Point>& points) {
    // Centre labels stay on the arc-length midpoint of the routed path.
    return path_point_at_progress(points, 0.5f);
}

// ======================================================================================
// edge_geometry.rs
// ======================================================================================

float arrowhead_inset(DiagramKind kind, std::optional<EdgeArrowhead> arrow_kind) {
    if (kind == DiagramKind::Class) {
        if (!arrow_kind) return 4.0f;
        return *arrow_kind == EdgeArrowhead::OpenTriangle ? 17.0f : 5.0f;
    }
    return 0.0f;
}

std::vector<Point> apply_endpoint_insets(std::vector<Point> path, float start_inset, float end_inset) {
    if (start_inset > 0.0f && path.size() >= 2) {
        auto [sx, sy] = path[0];
        auto [nx, ny] = path[1];
        float dx = sx - nx, dy = sy - ny;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len > start_inset) {
            float r = start_inset / len;
            path[0] = {sx - dx * r, sy - dy * r};
        }
    }
    if (end_inset > 0.0f && path.size() >= 2) {
        size_t n = path.size();
        auto [px, py] = path[n - 2];
        auto [ex, ey] = path[n - 1];
        float dx = ex - px, dy = ey - py;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len > end_inset) {
            float r = end_inset / len;
            path[n - 1] = {ex - dx * r, ey - dy * r};
        }
    }
    return path;
}

}  // namespace mermaid
