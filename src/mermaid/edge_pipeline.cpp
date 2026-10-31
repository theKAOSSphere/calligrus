// Mermaid port of layout/flowchart/edge_pipeline.rs (+ plan.rs lanes, roles.rs, route_labels.rs,
// post_route.rs): from placed nodes to routed edge polylines.
//
// Steps (same order as the Rust code): obstacles and channels -> choose the side of each edge
// at both nodes -> spread the ports along each side -> refine ports by trying routes -> plan
// lanes for parallel edges -> route every edge (candidates scored) -> global re-routing passes
// -> path clean-up -> endpoint repairs.
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

#include "layout_internal.h"
#include "path_cleanup.h"
#include "ranking.h"
#include "routing.h"
#include "strutil.h"

namespace mermaid {

namespace {

constexpr float MIN_NODE_SPACING_FLOOR = 16.0f;
constexpr float EDGE_OCCUPANCY_CELL_RATIO = 0.6f;
constexpr float MULTI_EDGE_OFFSET_RATIO = 0.35f;
constexpr float FLOWCHART_PORT_ROUTE_BIAS_RATIO = 0.5f;
constexpr float FLOWCHART_PORT_ROUTE_BIAS_MAX_RATIO = 0.8f;
constexpr size_t FLOWCHART_EDGE_LABEL_WRAP_TRIGGER_CHARS = 34;
constexpr size_t MAX_RESERVED_ROUTING_CHANNELS = 36;
constexpr float RANK_CHANNEL_MIN_GAP_RATIO = 0.70f;
constexpr size_t HUB_CHANNEL_MIN_DEGREE = 4;
constexpr float HUB_CHANNEL_PAD_RATIO = 0.78f;

float fmax_(float a, float b) { return a > b ? a : (a < b ? b : a); }
float fmin_(float a, float b) { return a < b ? a : (a > b ? b : a); }
float clamp_(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// partial_cmp(..).unwrap_or(Equal) as -1/0/1
int cmp_f(float a, float b) {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

using Path = std::vector<Point>;
using Sides = std::tuple<EdgeSide, EdgeSide, bool>;

// ======================================================================================
// roles.rs
// ======================================================================================

struct EdgeRole {
    bool is_cycle_edge = false, is_back_edge = false, crosses_subgraph_boundary = false;
    bool has_center_label = false, has_endpoint_label = false;
};

std::vector<EdgeRole> classify_edge_roles(const Graph& graph) {
    std::vector<EdgeRole> roles;
    auto memberships = node_subgraph_memberships(graph);
    if (graph.edges.empty()) return roles;
    std::vector<std::string> node_ids;
    for (const auto& [id, n] : graph.nodes) node_ids.push_back(id);
    RankMap ranks = compute_ranks_subset(node_ids, graph.edges, graph.node_order);
    auto comp = scc_component_ids(node_ids, graph.edges);
    auto nonblank = [](const std::optional<std::string>& s) { return s && !trim_view(*s).empty(); };
    for (const auto& edge : graph.edges) {
        EdgeRole r;
        auto fc = comp.find(edge.from), tc = comp.find(edge.to);
        bool is_self_loop = edge.from == edge.to;
        r.is_cycle_edge = is_self_loop || (fc != comp.end() && tc != comp.end() && fc->second == tc->second);
        auto fr = ranks.find(edge.from), tr = ranks.find(edge.to);
        r.is_back_edge = fr != ranks.end() && tr != ranks.end() && tr->second <= fr->second;
        r.crosses_subgraph_boundary = memberships_differ(memberships, edge.from, edge.to);
        r.has_center_label = nonblank(edge.label);
        r.has_endpoint_label = nonblank(edge.start_label) || nonblank(edge.end_label);
        roles.push_back(r);
    }
    return roles;
}

// ======================================================================================
// plan.rs: lanes for parallel edges
// ======================================================================================

struct EdgeLaneAssignments {
    std::map<std::pair<std::string, std::string>, size_t> pair_counts;
    std::vector<size_t> pair_index, pair_total;
    std::vector<float> cross_edge_offsets;

    std::vector<float> effective_offsets(const std::vector<EdgePortInfo>& edge_ports, DiagramKind kind,
                                         const LayoutConfig& config) const {
        std::vector<float> out;
        for (size_t idx = 0; idx < edge_ports.size(); ++idx) {
            const EdgePortInfo& port_info = edge_ports[idx];
            size_t total = idx < pair_total.size() ? pair_total[idx] : 1;
            size_t lane_index = idx < pair_index.size() ? pair_index[idx] : 0;
            float base_offset = 0.0f;
            if (total > 1)
                base_offset = (static_cast<float>(lane_index) - (static_cast<float>(total) - 1.0f) / 2.0f) *
                              (config.node_spacing * MULTI_EDGE_OFFSET_RATIO);
            float cross_edge_offset = idx < cross_edge_offsets.size() ? cross_edge_offsets[idx] : 0.0f;
            float offset = base_offset + cross_edge_offset;
            if (kind == DiagramKind::Flowchart) {
                float raw_bias = (port_info.start_offset - port_info.end_offset) * FLOWCHART_PORT_ROUTE_BIAS_RATIO;
                float max_bias = fmax_(config.node_spacing * FLOWCHART_PORT_ROUTE_BIAS_MAX_RATIO, 8.0f);
                offset += clamp_(raw_bias, -max_bias, max_bias);
            }
            out.push_back(offset);
        }
        return out;
    }
};

EdgeLaneAssignments plan_edge_lanes(const Graph& graph, const std::map<std::string, NodeLayout>& nodes,
                                    const std::vector<SubgraphLayout>& subgraphs, const LayoutConfig& config) {
    EdgeLaneAssignments a;
    a.pair_counts = build_edge_pair_counts(graph.edges);
    std::map<std::pair<std::string, std::string>, size_t> pair_seen;
    a.pair_index.assign(graph.edges.size(), 0);
    a.pair_total.assign(graph.edges.size(), 1);
    for (size_t idx = 0; idx < graph.edges.size(); ++idx) {
        auto key = edge_pair_key(graph.edges[idx]);
        auto it = a.pair_counts.find(key);
        a.pair_total[idx] = it == a.pair_counts.end() ? 1 : it->second;
        size_t& seen = pair_seen[key];
        a.pair_index[idx] = seen;
        ++seen;
    }
    a.cross_edge_offsets.assign(graph.edges.size(), 0.0f);
    if (graph.kind == DiagramKind::Flowchart) {
        bool horiz = is_horizontal(graph.direction);
        float band_size = fmax_(config.node_spacing * 2.0f, 30.0f);
        std::map<int, std::vector<std::pair<size_t, float>>> groups;
        for (size_t idx = 0; idx < graph.edges.size(); ++idx) {
            const Edge& edge = graph.edges[idx];
            auto endpoints = effective_edge_endpoint_layouts(graph, nodes, subgraphs, edge);
            if (!endpoints) continue;
            const NodeLayout& from = endpoints->first;
            const NodeLayout& to = endpoints->second;
            Point from_center{from.x + from.width / 2.0f, from.y + from.height / 2.0f};
            Point to_center{to.x + to.width / 2.0f, to.y + to.height / 2.0f};
            float dx = to_center.first - from_center.first;
            float dy = to_center.second - from_center.second;
            float cross_axis = horiz ? std::fabs(dy) : std::fabs(dx);
            float main_axis = horiz ? std::fabs(dx) : std::fabs(dy);
            bool is_secondary = edge.style == EdgeStyle::Dotted || edge.label.has_value();
            if (!is_secondary || cross_axis <= main_axis * 1.2f) continue;
            float band_coord = horiz ? (from_center.first + to_center.first) * 0.5f : (from_center.second + to_center.second) * 0.5f;
            int bucket = f2i(std::round(band_coord / band_size));
            float sort_key = horiz ? (from_center.second + to_center.second) * 0.5f : (from_center.first + to_center.first) * 0.5f;
            groups[bucket].push_back({idx, sort_key});
        }
        float spacing = fmax_(config.node_spacing * 0.45f, 8.0f);
        for (auto& [bucket, group] : groups) {
            if (group.size() <= 1) continue;
            std::stable_sort(group.begin(), group.end(), [](const auto& x, const auto& y) { return x.second < y.second; });
            float center = (static_cast<float>(group.size()) - 1.0f) * 0.5f;
            for (size_t pos = 0; pos < group.size(); ++pos)
                a.cross_edge_offsets[group[pos].first] = (static_cast<float>(pos) - center) * spacing;
        }
    }
    return a;
}

// ======================================================================================
// small helpers
// ======================================================================================

using NodeMap = std::map<std::string, NodeLayout>;

enum class TrackKind { Side, Axis };
struct PortTrack {
    TrackKind kind;
    int value;  // EdgeSide or PortAxis as int
    bool operator<(const PortTrack& o) const { return std::tie(kind, value) < std::tie(o.kind, o.value); }
};

bool is_branching_port_shape(const NodeLayout& n) {
    switch (n.shape) {
    case NodeShape::Diamond: case NodeShape::Hexagon: case NodeShape::Parallelogram: case NodeShape::ParallelogramAlt:
    case NodeShape::Trapezoid: case NodeShape::TrapezoidAlt: case NodeShape::Asymmetric:
        return true;
    default:
        return false;
    }
}

EdgeSide opposite_side(EdgeSide s) {
    switch (s) {
    case EdgeSide::Left: return EdgeSide::Right;
    case EdgeSide::Right: return EdgeSide::Left;
    case EdgeSide::Top: return EdgeSide::Bottom;
    default: return EdgeSide::Top;
    }
}

bool use_axis_wide_port_track(const NodeLayout& node, EdgeSide side, size_t degree, const std::array<size_t, 4>& side_counts) {
    if (degree <= 2) return false;
    size_t axis_total = side_counts[side_slot(side)] + side_counts[side_slot(opposite_side(side))];
    if (axis_total <= 2) return false;
    size_t side_count = side_counts[side_slot(side)];
    size_t opposite_count = side_counts[side_slot(opposite_side(side))];
    return side_count > 1 || opposite_count > 1 || (is_branching_port_shape(node) && axis_total >= 3);
}

PortTrack port_track_for_assignment(const NodeLayout& node, EdgeSide side, size_t degree, const std::array<size_t, 4>& side_counts) {
    if (use_axis_wide_port_track(node, side, degree, side_counts)) return {TrackKind::Axis, static_cast<int>(port_axis(side))};
    return {TrackKind::Side, static_cast<int>(side)};
}

float port_track_node_len(const NodeLayout& node, PortTrack track) {
    if (track.kind == TrackKind::Side) return side_is_vertical(static_cast<EdgeSide>(track.value)) ? node.height : node.width;
    return static_cast<PortAxis>(track.value) == PortAxis::X ? node.width : node.height;
}

float port_track_node_start(const NodeLayout& node, PortTrack track) {
    if (track.kind == TrackKind::Side) return side_is_vertical(static_cast<EdgeSide>(track.value)) ? node.y : node.x;
    return static_cast<PortAxis>(track.value) == PortAxis::X ? node.x : node.y;
}

struct NodeBounds {
    float min_x, max_x, min_y, max_y;
};

std::optional<NodeBounds> visible_node_bounds(const NodeMap& nodes) {
    NodeBounds b{std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::lowest()};
    bool any = false;
    for (const auto& [id, n] : nodes) {
        if (n.hidden || n.anchor_subgraph) continue;
        any = true;
        b.min_x = fmin_(b.min_x, n.x);
        b.max_x = fmax_(b.max_x, n.x + n.width);
        b.min_y = fmin_(b.min_y, n.y);
        b.max_y = fmax_(b.max_y, n.y + n.height);
    }
    if (!any) return std::nullopt;
    return b;
}

void push_reserved_channel(std::vector<ReservedRoutingChannel>& channels, ReservedRoutingChannel ch) {
    if (!std::isfinite(ch.coord) || !std::isfinite(ch.span_min) || !std::isfinite(ch.span_max) || ch.span_max <= ch.span_min) return;
    for (const auto& e : channels)
        if (e.axis == ch.axis && std::fabs(e.coord - ch.coord) <= 3.0f && e.span_min <= ch.span_max && ch.span_min <= e.span_max) return;
    channels.push_back(ch);
}

std::vector<ReservedRoutingChannel> build_flowchart_reserved_channels(const Graph& graph, const NodeMap& nodes,
                                                                      const LayoutConfig& config) {
    std::vector<ReservedRoutingChannel> channels;
    if (graph.kind != DiagramKind::Flowchart || nodes.size() < 4) return channels;
    auto bounds = visible_node_bounds(nodes);
    if (!bounds) return channels;
    bool horiz = is_horizontal(graph.direction);
    float span_pad = fmax_(config.node_spacing * 0.9f, 24.0f);
    float min_rank_gap = fmax_(config.node_spacing * RANK_CHANNEL_MIN_GAP_RATIO, 18.0f);

    std::vector<std::pair<float, float>> intervals;
    for (const auto& [id, n] : nodes) {
        if (n.hidden || n.anchor_subgraph) continue;
        intervals.push_back(horiz ? std::make_pair(n.x, n.x + n.width) : std::make_pair(n.y, n.y + n.height));
    }
    std::stable_sort(intervals.begin(), intervals.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::optional<float> prev_end;
    for (auto [start, end] : intervals) {
        if (prev_end) {
            float prev = *prev_end;
            float gap = start - prev;
            if (gap >= min_rank_gap) {
                float coord = (start + prev) * 0.5f;
                ReservedRoutingChannel ch = horiz ? ReservedRoutingChannel{ReservedRoutingChannelAxis::Vertical, coord, bounds->min_y - span_pad, bounds->max_y + span_pad}
                                                  : ReservedRoutingChannel{ReservedRoutingChannelAxis::Horizontal, coord, bounds->min_x - span_pad, bounds->max_x + span_pad};
                push_reserved_channel(channels, ch);
            }
            prev_end = fmax_(prev, end);
        } else {
            prev_end = end;
        }
    }

    std::unordered_map<std::string, size_t> degree_by_node;
    for (const auto& e : graph.edges) {
        ++degree_by_node[e.from];
        ++degree_by_node[e.to];
    }
    float hub_pad = fmax_(config.node_spacing * HUB_CHANNEL_PAD_RATIO, 24.0f);
    std::vector<std::pair<const NodeLayout*, size_t>> hubs;
    for (const auto& [id, n] : nodes) {
        if (n.hidden || n.anchor_subgraph) continue;
        auto it = degree_by_node.find(n.id);
        size_t degree = it == degree_by_node.end() ? 0 : it->second;
        if (degree >= HUB_CHANNEL_MIN_DEGREE) hubs.push_back({&n, degree});
    }
    std::stable_sort(hubs.begin(), hubs.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first->id < b.first->id;
    });
    size_t taken = 0;
    for (const auto& [node, degree] : hubs) {
        if (taken++ >= 8) break;
        float vsmin = node->y - hub_pad, vsmax = node->y + node->height + hub_pad;
        float hsmin = node->x - hub_pad, hsmax = node->x + node->width + hub_pad;
        for (float coord : {node->x - hub_pad, node->x + node->width + hub_pad})
            push_reserved_channel(channels, {ReservedRoutingChannelAxis::Vertical, coord, vsmin, vsmax});
        for (float coord : {node->y - hub_pad, node->y + node->height + hub_pad})
            push_reserved_channel(channels, {ReservedRoutingChannelAxis::Horizontal, coord, hsmin, hsmax});
        if (channels.size() >= MAX_RESERVED_ROUTING_CHANNELS) {
            channels.resize(MAX_RESERVED_ROUTING_CHANNELS);
            break;
        }
    }
    if (channels.size() > MAX_RESERVED_ROUTING_CHANNELS) channels.resize(MAX_RESERVED_ROUTING_CHANNELS);
    return channels;
}

// ======================================================================================
// routing performance profile and side choice
// ======================================================================================

enum class RoutingTier { Disabled, Exact, Bounded, Linear };

struct SideSearchProfile {
    size_t max_candidates;
    size_t port_offset_candidates;
    std::optional<size_t> max_refined_edges;
    bool fast_route;
    bool use_grid;
    bool use_existing_segments;
};

struct PerformanceProfile {
    RoutingTier tier;
    std::optional<SideSearchProfile> side_search;
    size_t global_route_passes;
    size_t negotiated_congestion_passes;
    bool use_route_occupancy;
    bool allow_exterior_fallback;
};

PerformanceProfile flowchart_routing_performance_profile(const Graph& graph, size_t layout_node_count, bool tiny_graph) {
    size_t edge_count = graph.edges.size();
    bool use_route_occupancy = !tiny_graph && edge_count > 2;
    if (graph.kind != DiagramKind::Flowchart || graph.edges.empty())
        return {RoutingTier::Disabled, std::nullopt, 0, 0, use_route_occupancy, false};
    size_t node_count = std::max<size_t>(layout_node_count, 1);
    bool dense = edge_count * 2 >= node_count * 3;
    bool compound = !graph.subgraphs.empty();

    RoutingTier tier;
    SideSearchProfile side;
    if (edge_count <= 64) {
        size_t offset_candidates = edge_count <= 32 ? 4 : 3;
        tier = RoutingTier::Exact;
        side = {5, offset_candidates, std::nullopt, tiny_graph, !tiny_graph, true};
    } else if (edge_count > 320) {
        tier = RoutingTier::Linear;
        side = {3, 2, 320, true, false, false};
    } else if (edge_count <= 160 || compound || dense) {
        tier = RoutingTier::Bounded;
        side = {4, 3, std::nullopt, true, false, edge_count <= 160};
    } else {
        tier = RoutingTier::Linear;
        side = {3, 2, 320, true, false, false};
    }

    size_t global_route_passes = 0;
    if (tier == RoutingTier::Exact && edge_count >= 3 && edge_count <= 48) global_route_passes = 2;
    else if (tier == RoutingTier::Exact && edge_count >= 3) global_route_passes = 1;
    else if (tier == RoutingTier::Bounded && edge_count >= 3 && edge_count <= 128) global_route_passes = 1;

    float density = static_cast<float>(edge_count) / static_cast<float>(node_count);
    size_t negotiated = 0;
    if (tier == RoutingTier::Exact && edge_count >= 12 && density >= 1.1f) negotiated = 2;
    else if (tier == RoutingTier::Bounded && edge_count <= 160 && density >= 1.35f) negotiated = 1;

    return {tier, side, global_route_passes, negotiated, use_route_occupancy, true};
}

struct SideChoiceContext {
    Direction graph_direction;
    std::optional<NodeBounds> content_bounds;
    const std::unordered_map<std::string, size_t>* node_degrees;
    const SideLoads* side_loads;
    const std::vector<Obstacle>* obstacles;
    const std::vector<Obstacle>* label_obstacles;
    const RoutingGrid* routing_grid;
    const std::vector<Segment>* existing_segments;
    const LayoutConfig* config;
    SideSearchProfile profile;
};

Sides choose_outer_back_edge_sides(const NodeLayout& from, const NodeLayout& to, Direction direction,
                                   const std::optional<NodeBounds>& bounds, const Sides& fallback) {
    if (!bounds) return fallback;
    if (is_horizontal(direction)) {
        float upper_clearance = fmin_(from.y, to.y) - bounds->min_y;
        float lower_clearance = bounds->max_y - fmax_(from.y + from.height, to.y + to.height);
        EdgeSide side;
        if (std::fabs(upper_clearance - lower_clearance) <= 1.0f) {
            float avg_y = (from.y + from.height * 0.5f + to.y + to.height * 0.5f) * 0.5f;
            side = avg_y <= (bounds->min_y + bounds->max_y) * 0.5f ? EdgeSide::Top : EdgeSide::Bottom;
        } else if (upper_clearance <= lower_clearance) {
            side = EdgeSide::Top;
        } else {
            side = EdgeSide::Bottom;
        }
        return {side, side, std::get<2>(fallback)};
    }
    float left_clearance = fmin_(from.x, to.x) - bounds->min_x;
    float right_clearance = bounds->max_x - fmax_(from.x + from.width, to.x + to.width);
    EdgeSide side;
    if (std::fabs(left_clearance - right_clearance) <= 1.0f) {
        float avg_x = (from.x + from.width * 0.5f + to.x + to.width * 0.5f) * 0.5f;
        side = avg_x <= (bounds->min_x + bounds->max_x) * 0.5f ? EdgeSide::Left : EdgeSide::Right;
    } else if (left_clearance <= right_clearance) {
        side = EdgeSide::Left;
    } else {
        side = EdgeSide::Right;
    }
    return {side, side, std::get<2>(fallback)};
}

Point side_direction(EdgeSide side) {
    switch (side) {
    case EdgeSide::Left: return {-1.0f, 0.0f};
    case EdgeSide::Right: return {1.0f, 0.0f};
    case EdgeSide::Top: return {0.0f, -1.0f};
    default: return {0.0f, 1.0f};
    }
}

float side_alignment_penalty(EdgeSide side, const NodeLayout& from, const NodeLayout& to) {
    Point f = node_center(from), t = node_center(to);
    float dx = t.first - f.first, dy = t.second - f.second;
    float len = fmax_(std::sqrt(dx * dx + dy * dy), 1.0f);
    Point dir = side_direction(side);
    float dot = (dir.first * dx + dir.second * dy) / len;
    return fmax_(1.0f - dot, 0.0f);
}

Sides candidate_horizontal_sides(const NodeLayout& from, const NodeLayout& to) {
    Point f = node_center(from), t = node_center(to);
    if (t.first >= f.first) return {EdgeSide::Right, EdgeSide::Left, to.x + to.width < from.x};
    return {EdgeSide::Left, EdgeSide::Right, to.x > from.x + from.width};
}

Sides candidate_vertical_sides(const NodeLayout& from, const NodeLayout& to) {
    Point f = node_center(from), t = node_center(to);
    if (t.second >= f.second) return {EdgeSide::Bottom, EdgeSide::Top, to.y + to.height < from.y};
    return {EdgeSide::Top, EdgeSide::Bottom, to.y > from.y + from.height};
}

void push_unique_side_candidate_limited(std::vector<Sides>& candidates, const Sides& c, size_t limit) {
    for (const auto& e : candidates)
        if (std::get<0>(e) == std::get<0>(c) && std::get<1>(e) == std::get<1>(c)) return;
    if (candidates.size() < std::max<size_t>(limit, 1)) candidates.push_back(c);
}

void push_priority_side_candidate_limited(std::vector<Sides>& candidates, const Sides& c, size_t limit) {
    for (const auto& e : candidates)
        if (std::get<0>(e) == std::get<0>(c) && std::get<1>(e) == std::get<1>(c)) return;
    if (candidates.size() < std::max<size_t>(limit, 1)) candidates.push_back(c);
    else if (!candidates.empty()) candidates.back() = c;
}

std::vector<Sides> collect_routed_side_candidates(const NodeLayout& from, const NodeLayout& to, const Sides& primary,
                                                  const Sides& balanced, const EdgeRole& role, Direction graph_direction,
                                                  const std::optional<NodeBounds>& content_bounds, size_t limit) {
    std::vector<Sides> candidates;
    push_unique_side_candidate_limited(candidates, primary, limit);
    push_unique_side_candidate_limited(candidates, balanced, limit);
    Sides horizontal = candidate_horizontal_sides(from, to);
    Sides vertical = candidate_vertical_sides(from, to);
    Point from_c = node_center(from), to_c = node_center(to);
    if (std::fabs(to_c.first - from_c.first) >= std::fabs(to_c.second - from_c.second)) {
        push_unique_side_candidate_limited(candidates, horizontal, limit);
        push_unique_side_candidate_limited(candidates, vertical, limit);
    } else {
        push_unique_side_candidate_limited(candidates, vertical, limit);
        push_unique_side_candidate_limited(candidates, horizontal, limit);
    }
    if (role.is_back_edge)
        push_priority_side_candidate_limited(candidates, choose_outer_back_edge_sides(from, to, graph_direction, content_bounds, balanced), limit);
    return candidates;
}

bool allow_low_degree_balancing_for_edge(const Edge& edge, const EdgeRole& role, size_t from_degree, size_t to_degree) {
    return from_degree <= 4 && to_degree <= 4 && (edge.style == EdgeStyle::Dotted || role.is_back_edge || role.crosses_subgraph_boundary);
}

float routed_side_candidate_score(const std::string& from_id, const std::string& to_id, const NodeLayout& from,
                                  const NodeLayout& to, const Sides& candidate, const Sides& primary, const EdgeRole& role,
                                  const SideChoiceContext& ctx) {
    RouteContext rc;
    rc.from_id = &from_id;
    rc.to_id = &to_id;
    rc.from = &from;
    rc.to = &to;
    rc.direction = ctx.graph_direction;
    rc.config = ctx.config;
    rc.obstacles = ctx.obstacles;
    rc.label_obstacles = ctx.label_obstacles;
    rc.fast_route = ctx.profile.fast_route;
    rc.base_offset = 0.0f;
    rc.start_side = std::get<0>(candidate);
    rc.end_side = std::get<1>(candidate);
    rc.start_offset = 0.0f;
    rc.end_offset = 0.0f;
    rc.stub_len = port_stub_length(*ctx.config, from, to);
    rc.start_inset = 0.0f;
    rc.end_inset = 0.0f;
    rc.prefer_shorter_ties = true;
    static const std::vector<ReservedRoutingChannel> kNoChannels;
    rc.reserved_channels = &kNoChannels;
    rc.force_preferred_label_via = false;
    rc.coarse_grid_retry = true;
    rc.allow_exterior_fallback = false;
    const std::vector<Segment>* existing = ctx.existing_segments->empty() ? nullptr : ctx.existing_segments;
    const RoutingGrid* grid = ctx.profile.use_grid ? ctx.routing_grid : nullptr;
    std::vector<Point> points = route_edge_with_avoidance(rc, nullptr, grid, existing);
    float hard_hits = static_cast<float>(path_obstacle_intersections(points, *ctx.obstacles, from_id, to_id));
    float label_hits = static_cast<float>(path_label_intersections(points, *ctx.label_obstacles, std::nullopt));
    size_t crossings = 0;
    float overlap = 0.0f;
    if (!ctx.existing_segments->empty()) {
        auto [c, o] = edge_crossings_with_existing(points, *ctx.existing_segments);
        crossings = c;
        overlap = o;
    }
    float bends = static_cast<float>(path_bend_count(points));
    float len = path_length(points);
    float from_load = static_cast<float>(side_load_for_node(*ctx.side_loads, from_id, std::get<0>(candidate)));
    float to_load = static_cast<float>(side_load_for_node(*ctx.side_loads, to_id, std::get<1>(candidate)));
    float congestion = from_load * from_load + to_load * to_load + (from_load + to_load) * 0.5f;
    auto deg = [&](const std::string& id) {
        auto it = ctx.node_degrees->find(id);
        return it == ctx.node_degrees->end() ? size_t(0) : it->second;
    };
    size_t from_degree = deg(from_id), to_degree = deg(to_id);
    bool low_degree_edge = from_degree <= 4 && to_degree <= 4;
    float primary_deviation;
    if (std::get<0>(candidate) == std::get<0>(primary) && std::get<1>(candidate) == std::get<1>(primary)) primary_deviation = 0.0f;
    else if (low_degree_edge) primary_deviation = 24.0f;
    else primary_deviation = 8.0f;
    float alignment = side_alignment_penalty(std::get<0>(candidate), from, to) + side_alignment_penalty(std::get<1>(candidate), to, from);
    float backward_penalty = (std::get<2>(candidate) && !std::get<2>(primary)) ? 8.0f : 0.0f;
    float back_edge_outer_bonus = (role.is_back_edge && std::get<0>(candidate) == std::get<1>(candidate) &&
                                   edge_axis_is_horizontal(std::get<0>(candidate)) != edge_axis_is_horizontal(std::get<0>(primary)))
                                      ? -10.0f
                                      : 0.0f;
    return hard_hits * 100000.0f + label_hits * 20000.0f + static_cast<float>(crossings) * 1600.0f + overlap * 70.0f + bends * 42.0f +
           len * 0.09f + congestion * 5.5f + alignment * 34.0f + primary_deviation + backward_penalty + back_edge_outer_bonus;
}

Sides choose_routed_flowchart_sides(const std::string& from_id, const std::string& to_id, const NodeLayout& from,
                                    const NodeLayout& to, const Sides& primary, const Sides& balanced, const EdgeRole& role,
                                    const SideChoiceContext& ctx) {
    std::vector<Sides> candidates = collect_routed_side_candidates(from, to, primary, balanced, role, ctx.graph_direction,
                                                                   ctx.content_bounds, ctx.profile.max_candidates);
    static const std::vector<Segment> kEmpty;
    SideChoiceContext scoring = ctx;
    scoring.existing_segments = ctx.profile.use_existing_segments ? ctx.existing_segments : &kEmpty;
    Sides best = primary;
    float best_score = std::numeric_limits<float>::infinity();
    for (const auto& cand : candidates) {
        float score = routed_side_candidate_score(from_id, to_id, from, to, cand, primary, role, scoring);
        if (score < best_score) {
            best_score = score;
            best = cand;
        }
    }
    return best;
}

// ======================================================================================
// port refinement
// ======================================================================================

struct PortRouteScore {
    size_t hard, endpoint_reentries, non_endpoint_hits, label_hits, port_collisions, crossings;
    float overlap;
    size_t bends;
    float len, offset_drift;
};

bool port_route_score_is_better(const PortRouteScore& c, const PortRouteScore& b) {
    if (c.hard != b.hard) return c.hard < b.hard;
    if (c.endpoint_reentries != b.endpoint_reentries) return c.endpoint_reentries < b.endpoint_reentries;
    if (c.non_endpoint_hits != b.non_endpoint_hits) return c.non_endpoint_hits < b.non_endpoint_hits;
    if (c.bends != b.bends) return c.bends < b.bends;
    if (std::fabs(c.len - b.len) > 1.0f) return c.len < b.len;
    if (std::fabs(c.offset_drift - b.offset_drift) > 0.5f) return c.offset_drift < b.offset_drift;
    if (c.label_hits != b.label_hits) return c.label_hits < b.label_hits;
    if (c.crossings != b.crossings) return c.crossings < b.crossings;
    if (std::fabs(c.overlap - b.overlap) > 0.05f) return c.overlap < b.overlap;
    if (c.port_collisions != b.port_collisions) return c.port_collisions < b.port_collisions;
    return false;
}

float port_axis_center(const NodeLayout& n, EdgeSide side) {
    return side_is_vertical(side) ? n.y + n.height / 2.0f : n.x + n.width / 2.0f;
}
float max_port_offset_for_side(const NodeLayout& n, EdgeSide side) {
    return side_is_vertical(side) ? fmax_(n.height / 2.0f - 1.0f, 0.0f) : fmax_(n.width / 2.0f - 1.0f, 0.0f);
}
float clamp_port_offset_for_side(const NodeLayout& n, EdgeSide side, float offset) {
    float max_offset = max_port_offset_for_side(n, side);
    return max_offset > 0.0f ? clamp_(offset, -max_offset, max_offset) : 0.0f;
}
float ideal_offset_for_side(Point remote, const NodeLayout& n, EdgeSide side) {
    return clamp_port_offset_for_side(n, side, ideal_port_pos(remote, n, side) - port_axis_center(n, side));
}

void push_unique_offset(std::vector<float>& candidates, float value, size_t limit) {
    for (float e : candidates)
        if (std::fabs(e - value) <= 0.75f) return;
    if (candidates.size() < std::max<size_t>(limit, 1)) candidates.push_back(value);
}

std::vector<float> port_offset_candidates(const NodeLayout& node, EdgeSide side, float current_offset, Point remote,
                                          const LayoutConfig& config, size_t limit) {
    std::vector<float> offsets;
    float current = clamp_port_offset_for_side(node, side, current_offset);
    float ideal = ideal_offset_for_side(remote, node, side);
    float step = fmin_(fmax_(routing_cell_size(config), config.node_spacing * 0.18f), fmax_(max_port_offset_for_side(node, side), 1.0f));
    push_unique_offset(offsets, current, limit);
    push_unique_offset(offsets, ideal, limit);
    push_unique_offset(offsets, 0.0f, limit);
    if (limit > 3 && step > 0.5f) {
        push_unique_offset(offsets, clamp_port_offset_for_side(node, side, ideal + step), limit);
        push_unique_offset(offsets, clamp_port_offset_for_side(node, side, ideal - step), limit);
    }
    return offsets;
}

struct PortRouteCandidateContext {
    const Graph* graph;
    const Edge* edge;
    const NodeLayout* from;
    const NodeLayout* to;
    const std::vector<Obstacle>* obstacles;
    const std::vector<Obstacle>* label_obstacles;
    const RoutingGrid* routing_grid;
    const LayoutConfig* config;
    SideSearchProfile profile;
};

std::vector<Point> route_points_for_port_candidate(const PortRouteCandidateContext& ctx, const EdgePortInfo& port, float base_offset) {
    RouteContext rc;
    rc.from_id = &ctx.edge->from;
    rc.to_id = &ctx.edge->to;
    rc.from = ctx.from;
    rc.to = ctx.to;
    rc.direction = ctx.graph->direction;
    rc.config = ctx.config;
    rc.obstacles = ctx.obstacles;
    rc.label_obstacles = ctx.label_obstacles;
    rc.fast_route = ctx.profile.fast_route;
    rc.base_offset = base_offset;
    rc.start_side = port.start_side;
    rc.end_side = port.end_side;
    rc.start_offset = port.start_offset;
    rc.end_offset = port.end_offset;
    rc.stub_len = port_stub_length(*ctx.config, *ctx.from, *ctx.to);
    rc.start_inset = ctx.edge->arrow_start ? arrowhead_inset(ctx.graph->kind, ctx.edge->arrow_start_kind) : 0.0f;
    rc.end_inset = ctx.edge->arrow_end ? arrowhead_inset(ctx.graph->kind, ctx.edge->arrow_end_kind) : 0.0f;
    rc.prefer_shorter_ties = true;
    static const std::vector<ReservedRoutingChannel> kNoChannels;
    rc.reserved_channels = &kNoChannels;
    rc.force_preferred_label_via = false;
    rc.coarse_grid_retry = true;
    rc.allow_exterior_fallback = false;
    const RoutingGrid* grid = ctx.profile.use_grid ? ctx.routing_grid : nullptr;
    return route_edge_with_avoidance(rc, nullptr, grid, nullptr);
}

std::vector<Segment> collect_port_choice_segments(const Graph& graph, const NodeMap& nodes, const std::vector<SubgraphLayout>& subgraphs,
                                                  const std::vector<EdgePortInfo>& edge_ports, size_t excluded_idx) {
    std::vector<Segment> segments;
    for (size_t idx = 0; idx < graph.edges.size(); ++idx) {
        const Edge& edge = graph.edges[idx];
        if (idx == excluded_idx || edge.from == edge.to) continue;
        if (idx >= edge_ports.size()) continue;
        const EdgePortInfo& port = edge_ports[idx];
        auto endpoints = effective_edge_endpoint_layouts(graph, nodes, subgraphs, edge);
        if (!endpoints) continue;
        segments.push_back({anchor_point_for_node(endpoints->first, port.start_side, port.start_offset),
                            anchor_point_for_node(endpoints->second, port.end_side, port.end_offset)});
    }
    return segments;
}

size_t port_collision_count(const Graph& graph, const NodeMap& nodes, const std::vector<EdgePortInfo>& edge_ports, size_t edge_idx,
                            const EdgePortInfo& candidate, const LayoutConfig& config) {
    if (edge_idx >= graph.edges.size()) return 0;
    const Edge& edge = graph.edges[edge_idx];
    float min_sep = fmin_(fmax_(routing_cell_size(config), config.node_spacing * 0.22f), 28.0f);
    size_t collisions = 0;
    for (size_t other_idx = 0; other_idx < graph.edges.size(); ++other_idx) {
        if (other_idx == edge_idx) continue;
        if (other_idx >= edge_ports.size()) continue;
        const Edge& other_edge = graph.edges[other_idx];
        const EdgePortInfo& other = edge_ports[other_idx];
        struct Probe {
            const std::string* node_id;
            EdgeSide side;
            float offset;
        };
        Probe mine[2] = {{&edge.from, candidate.start_side, candidate.start_offset}, {&edge.to, candidate.end_side, candidate.end_offset}};
        Probe theirs[2] = {{&other_edge.from, other.start_side, other.start_offset}, {&other_edge.to, other.end_side, other.end_offset}};
        for (const Probe& m : mine) {
            auto nit = nodes.find(*m.node_id);
            if (nit == nodes.end()) continue;
            const NodeLayout& node = nit->second;
            float candidate_axis = port_axis_center(node, m.side) + clamp_port_offset_for_side(node, m.side, m.offset);
            for (const Probe& o : theirs)
                if (*o.node_id == *m.node_id && o.side == m.side) {
                    float other_axis = port_axis_center(node, o.side) + clamp_port_offset_for_side(node, o.side, o.offset);
                    if (std::fabs(candidate_axis - other_axis) < min_sep) ++collisions;
                }
        }
    }
    return collisions;
}

PortRouteScore score_port_route_candidate(const std::vector<Point>& points, const Edge& edge, const NodeMap& nodes,
                                          const std::vector<Obstacle>& obstacles, const std::vector<Obstacle>& label_obstacles,
                                          const std::vector<Segment>& existing_segments, const EdgePortInfo& current,
                                          const EdgePortInfo& candidate, size_t port_collisions) {
    size_t direction_violations = flowchart_endpoint_direction_violation_count(points, edge, nodes);
    size_t obstacle_hits = path_obstacle_intersections(points, obstacles, edge.from, edge.to);
    size_t non_endpoint_hits = flowchart_path_hits_non_endpoint_nodes(points, edge.from, edge.to, nodes) ? 1 : 0;
    size_t endpoint_reentries = flowchart_endpoint_reentry_count(points, edge, nodes);
    size_t label_hits = path_label_intersections(points, label_obstacles, std::nullopt);
    size_t crossings = 0;
    float overlap = 0.0f;
    if (!existing_segments.empty()) {
        auto [c, o] = edge_crossings_with_existing(points, existing_segments);
        crossings = c;
        overlap = o;
    }
    float side_drift = (candidate.start_side == current.start_side ? std::fabs(candidate.start_offset - current.start_offset)
                                                                    : 32.0f + std::fabs(candidate.start_offset) * 0.25f) +
                       (candidate.end_side == current.end_side ? std::fabs(candidate.end_offset - current.end_offset)
                                                               : 32.0f + std::fabs(candidate.end_offset) * 0.25f);
    return {direction_violations + obstacle_hits, endpoint_reentries, non_endpoint_hits, label_hits, port_collisions,
            crossings, overlap, path_bend_count(points), path_length(points), side_drift};
}

struct PortRefinementContext {
    const Graph* graph;
    const NodeMap* nodes;
    const std::vector<SubgraphLayout>* subgraphs;
    const std::vector<Obstacle>* obstacles;
    const std::vector<Obstacle>* label_obstacles;
    const RoutingGrid* routing_grid;
    const std::vector<EdgeRole>* edge_roles;
    std::optional<NodeBounds> content_bounds;
    const std::unordered_map<std::string, size_t>* node_degrees;
    const SideLoads* side_loads;
    const LayoutConfig* config;
    SideSearchProfile profile;
};

void refine_flowchart_ports_with_route_candidates(const PortRefinementContext& ctx, std::vector<EdgePortInfo>& edge_ports) {
    const Graph& graph = *ctx.graph;
    size_t offset_limit = std::max<size_t>(ctx.profile.port_offset_candidates, 1);
    EdgeLaneAssignments predicted = plan_edge_lanes(graph, *ctx.nodes, *ctx.subgraphs, *ctx.config);
    std::vector<float> predicted_lane_offsets = predicted.effective_offsets(edge_ports, graph.kind, *ctx.config);
    size_t refined_edges = 0;
    for (size_t idx = 0; idx < graph.edges.size(); ++idx) {
        const Edge& edge = graph.edges[idx];
        if (edge.from == edge.to) continue;
        if (ctx.profile.max_refined_edges && refined_edges >= *ctx.profile.max_refined_edges) break;
        if (idx >= edge_ports.size()) continue;
        EdgePortInfo current = edge_ports[idx];
        auto endpoints = effective_edge_endpoint_layouts(graph, *ctx.nodes, *ctx.subgraphs, edge);
        if (!endpoints) continue;
        const NodeLayout& from = endpoints->first;
        const NodeLayout& to = endpoints->second;
        float base_offset = idx < predicted_lane_offsets.size() ? predicted_lane_offsets[idx] : 0.0f;
        auto deg = [&](const std::string& id) {
            auto it = ctx.node_degrees->find(id);
            return it == ctx.node_degrees->end() ? size_t(0) : it->second;
        };
        size_t from_degree = deg(edge.from), to_degree = deg(edge.to);
        EdgeRole role = idx < ctx.edge_roles->size() ? (*ctx.edge_roles)[idx] : EdgeRole{};
        Sides primary = edge_sides(from, to, graph.direction);
        Sides balanced = edge_sides_balanced(edge.from, edge.to, from, to, allow_low_degree_balancing_for_edge(edge, role, from_degree, to_degree),
                                             role.is_back_edge, graph.direction, *ctx.node_degrees, *ctx.side_loads);
        std::vector<Sides> side_candidates = collect_routed_side_candidates(from, to, primary, balanced, role, graph.direction,
                                                                            ctx.content_bounds, ctx.profile.max_candidates);
        // Back edges keep the outer lane chosen earlier; only offsets are tuned.
        if (role.is_back_edge) {
            std::vector<Sides> kept;
            for (const auto& s : side_candidates)
                if (std::get<0>(s) == current.start_side && std::get<1>(s) == current.end_side) kept.push_back(s);
            side_candidates = kept;
            if (side_candidates.empty()) side_candidates.push_back({current.start_side, current.end_side, true});
        }
        std::vector<Segment> existing_segments;
        if (ctx.profile.use_existing_segments) existing_segments = collect_port_choice_segments(graph, *ctx.nodes, *ctx.subgraphs, edge_ports, idx);
        PortRouteCandidateContext rcc{&graph, &edge, &from, &to, ctx.obstacles, ctx.label_obstacles, ctx.routing_grid, ctx.config, ctx.profile};
        std::vector<Point> baseline_points = route_points_for_port_candidate(rcc, current, base_offset);
        size_t baseline_collisions = port_collision_count(graph, *ctx.nodes, edge_ports, idx, current, *ctx.config);
        PortRouteScore best_score = score_port_route_candidate(baseline_points, edge, *ctx.nodes, *ctx.obstacles, *ctx.label_obstacles,
                                                               existing_segments, current, current, baseline_collisions);
        EdgePortInfo best_port = current;
        Point remote_to = node_center(to), remote_from = node_center(from);

        for (const auto& [start_side, end_side, ignored] : side_candidates) {
            float start_current = start_side == current.start_side ? current.start_offset : ideal_offset_for_side(remote_to, from, start_side);
            float end_current = end_side == current.end_side ? current.end_offset : ideal_offset_for_side(remote_from, to, end_side);
            std::vector<float> start_offsets = port_offset_candidates(from, start_side, start_current, remote_to, *ctx.config, offset_limit);
            std::vector<float> end_offsets = port_offset_candidates(to, end_side, end_current, remote_from, *ctx.config, offset_limit);
            for (float start_offset : start_offsets)
                for (float end_offset : end_offsets) {
                    EdgePortInfo candidate_port{start_side, end_side, start_offset, end_offset};
                    std::vector<Point> points = route_points_for_port_candidate(rcc, candidate_port, base_offset);
                    size_t collisions = port_collision_count(graph, *ctx.nodes, edge_ports, idx, candidate_port, *ctx.config);
                    PortRouteScore score = score_port_route_candidate(points, edge, *ctx.nodes, *ctx.obstacles, *ctx.label_obstacles,
                                                                      existing_segments, current, candidate_port, collisions);
                    if (best_score.hard == 0 && score.hard > 0) continue;
                    if (best_score.non_endpoint_hits == 0 && score.non_endpoint_hits > 0) continue;
                    if (score.endpoint_reentries > best_score.endpoint_reentries) continue;
                    if (score.len > best_score.len * 3.0f + ctx.config->node_spacing * 4.0f) continue;
                    if (port_route_score_is_better(score, best_score)) {
                        best_score = score;
                        best_port = candidate_port;
                    }
                }
        }
        edge_ports[idx] = best_port;
        ++refined_edges;
    }
}

// ======================================================================================
// global route passes
// ======================================================================================

struct GlobalRouteScore {
    size_t hard, endpoint_reentries, non_endpoint_hits, label_hits, crossings;
    float overlap;
    size_t bends;
    float len;
};

bool global_route_score_is_better(const GlobalRouteScore& c, const GlobalRouteScore& b) {
    if (c.hard != b.hard) return c.hard < b.hard;
    if (c.endpoint_reentries != b.endpoint_reentries) return c.endpoint_reentries < b.endpoint_reentries;
    if (c.non_endpoint_hits != b.non_endpoint_hits) return c.non_endpoint_hits < b.non_endpoint_hits;
    if (c.label_hits != b.label_hits) return c.label_hits < b.label_hits;
    if (c.crossings != b.crossings) return c.crossings < b.crossings;
    if (std::fabs(c.overlap - b.overlap) > 0.05f) return c.overlap < b.overlap;
    if (c.bends != b.bends) return c.bends < b.bends;
    return c.len + 1.0f < b.len;
}

GlobalRouteScore score_global_route_candidate(const std::vector<Point>& points, const Edge& edge, const NodeMap& nodes,
                                              const std::vector<Obstacle>& obstacles, const std::vector<Obstacle>& label_obstacles,
                                              const std::vector<Segment>& existing_segments,
                                              const std::optional<std::string>& preferred_label_id) {
    size_t direction_violations = flowchart_endpoint_direction_violation_count(points, edge, nodes);
    size_t obstacle_hits = path_obstacle_intersections(points, obstacles, edge.from, edge.to);
    size_t non_endpoint_hits = flowchart_path_hits_non_endpoint_nodes(points, edge.from, edge.to, nodes) ? 1 : 0;
    size_t endpoint_reentries = flowchart_endpoint_reentry_count(points, edge, nodes);
    size_t label_hits = path_label_intersections(points, label_obstacles, preferred_label_id);
    size_t crossings = 0;
    float overlap = 0.0f;
    if (!existing_segments.empty()) {
        auto [c, o] = edge_crossings_with_existing(points, existing_segments);
        crossings = c;
        overlap = o;
    }
    return {direction_violations + obstacle_hits, endpoint_reentries, non_endpoint_hits, label_hits, crossings, overlap,
            path_bend_count(points), path_length(points)};
}

std::vector<Segment> collect_other_flowchart_segments(const Paths& routed, size_t excluded_idx) {
    std::vector<Segment> segments;
    for (size_t idx = 0; idx < routed.size(); ++idx) {
        if (idx == excluded_idx) continue;
        for (size_t s = 0; s + 1 < routed[idx].size(); ++s) segments.push_back({routed[idx][s], routed[idx][s + 1]});
    }
    return segments;
}

std::optional<EdgeOccupancy> occupancy_from_other_routes(const Paths& routed, size_t excluded_idx, const LayoutConfig& config) {
    if (routed.size() <= 2) return std::nullopt;
    EdgeOccupancy occupancy(fmax_(config.node_spacing, MIN_NODE_SPACING_FLOOR) * EDGE_OCCUPANCY_CELL_RATIO);
    bool any = false;
    for (size_t idx = 0; idx < routed.size(); ++idx) {
        if (idx == excluded_idx || routed[idx].size() < 2) continue;
        occupancy.add_path(routed[idx]);
        any = true;
    }
    if (!any) return std::nullopt;
    return occupancy;
}

// ---- route label plans (route_labels.rs) ----

struct RouteLabelPlan {
    std::string obstacle_id;
    size_t obstacle_index;
    float progress;
    Point center;
};

bool flowchart_label_needs_reserved_route_gap(const TextBlock& label, const LayoutConfig& config) {
    if (label.lines.size() > 1) return true;
    size_t chars = 0;
    for (const auto& l : label.lines) chars += char_count(l);
    return label.width >= config.node_spacing * 0.9f || chars >= 14;
}

std::optional<Point> provisional_route_label_center(size_t idx, const TextBlock& label, const Graph& graph, const NodeMap& nodes,
                                                    const std::vector<SubgraphLayout>& subgraphs,
                                                    const std::vector<EdgePortInfo>& edge_ports, const std::vector<size_t>& pair_index,
                                                    const std::vector<float>& lane_offsets, float pad_x, float pad_y,
                                                    const LayoutConfig& config) {
    const Edge& edge = graph.edges[idx];
    auto endpoints = effective_edge_endpoint_layouts(graph, nodes, subgraphs, edge);
    if (!endpoints) return std::nullopt;
    const NodeLayout& from = endpoints->first;
    const NodeLayout& to = endpoints->second;
    EdgePortInfo port_info = idx < edge_ports.size() ? edge_ports[idx] : EdgePortInfo{};
    Point start = anchor_point_for_node(from, port_info.start_side, port_info.start_offset);
    Point end = anchor_point_for_node(to, port_info.end_side, port_info.end_offset);
    float base_offset = idx < lane_offsets.size() ? lane_offsets[idx] : 0.0f;
    Point center{(start.first + end.first) * 0.5f, (start.second + end.second) * 0.5f};
    bool horiz = is_horizontal(graph.direction);
    if (horiz) center.first += base_offset;
    else center.second += base_offset;
    if (graph.kind == DiagramKind::Flowchart) {
        float main_span = horiz ? std::fabs(end.first - start.first) : std::fabs(end.second - start.second);
        float label_main = horiz ? label.width + 2.0f * pad_x : label.height + 2.0f * pad_y;
        float label_cross = horiz ? label.height + 2.0f * pad_y : label.width + 2.0f * pad_x;
        float margin = fmax_(config.node_spacing * 0.35f, 14.0f);
        size_t pidx = idx < pair_index.size() ? pair_index[idx] : 0;
        float preferred_sign;
        if (horiz) {
            float dy = end.second - start.second;
            if (std::fabs(dy) > 2.0f) preferred_sign = signum(dy);
            else preferred_sign = pidx % 2 == 0 ? -1.0f : 1.0f;
        } else {
            float dx = end.first - start.first;
            if (std::fabs(dx) > 2.0f) preferred_sign = signum(dx);
            else preferred_sign = pidx % 2 == 0 ? -1.0f : 1.0f;
        }
        bool needs_cross_axis_lift = label_main + margin * 2.0f >= fmax_(main_span, 1.0f);
        if (needs_cross_axis_lift) {
            float multiline_lift = label.lines.size() > 1 ? label_cross * 0.75f : 0.0f;
            float clearance = label_cross * 0.5f + margin + multiline_lift;
            for (float sign : {preferred_sign, -preferred_sign}) {
                Point candidate = center;
                if (horiz) candidate.second += sign * clearance;
                else candidate.first += sign * clearance;
                float half_w = label.width * 0.5f + pad_x;
                float half_h = label.height * 0.5f + pad_y;
                bool overlaps_start = start.first >= candidate.first - half_w && start.first <= candidate.first + half_w &&
                                      start.second >= candidate.second - half_h && start.second <= candidate.second + half_h;
                bool overlaps_end = end.first >= candidate.first - half_w && end.first <= candidate.first + half_w &&
                                    end.second >= candidate.second - half_h && end.second <= candidate.second + half_h;
                if (!overlaps_start && !overlaps_end) {
                    center = candidate;
                    break;
                }
            }
        }
    }
    return center;
}

bool path_intersects_label_obstacle(const std::vector<Point>& points, const Obstacle& obstacle) {
    for (size_t s = 0; s + 1 < points.size(); ++s)
        if (segment_intersects_rect(points[s], points[s + 1], obstacle)) return true;
    return false;
}

std::optional<std::vector<Point>> detour_flowchart_path_around_label(const std::vector<Point>& points, const Obstacle& obstacle,
                                                                     float clearance) {
    if (points.size() < 2 || !path_intersects_label_obstacle(points, obstacle)) return std::nullopt;
    std::optional<size_t> first, last;
    for (size_t s = 0; s + 1 < points.size(); ++s)
        if (segment_intersects_rect(points[s], points[s + 1], obstacle)) {
            if (!first) first = s;
            last = s;
        }
    if (!first || !last) return std::nullopt;
    Point entry = points[*first];
    Point exit = points[*last + 1];
    float left = obstacle.x - clearance, right = obstacle.x + obstacle.width + clearance;
    float top = obstacle.y - clearance, bottom = obstacle.y + obstacle.height + clearance;
    std::vector<std::vector<Point>> candidates;
    float via_left = std::fabs(entry.first - left) + std::fabs(exit.first - left);
    float via_right = std::fabs(entry.first - right) + std::fabs(exit.first - right);
    float xs[2] = {left, right};
    if (!(via_left <= via_right)) {
        xs[0] = right;
        xs[1] = left;
    }
    for (float x : xs) {
        std::vector<Point> c(points.begin(), points.begin() + static_cast<long>(*first) + 1);
        c.push_back({x, entry.second});
        c.push_back({x, exit.second});
        c.insert(c.end(), points.begin() + static_cast<long>(*last) + 1, points.end());
        c = compress_path(c);
        if (!path_intersects_label_obstacle(c, obstacle)) candidates.push_back(c);
    }
    float via_top = std::fabs(entry.second - top) + std::fabs(exit.second - top);
    float via_bottom = std::fabs(entry.second - bottom) + std::fabs(exit.second - bottom);
    float ys[2] = {top, bottom};
    if (!(via_top <= via_bottom)) {
        ys[0] = bottom;
        ys[1] = top;
    }
    for (float y : ys) {
        std::vector<Point> c(points.begin(), points.begin() + static_cast<long>(*first) + 1);
        c.push_back({entry.first, y});
        c.push_back({exit.first, y});
        c.insert(c.end(), points.begin() + static_cast<long>(*last) + 1, points.end());
        c = compress_path(c);
        if (!path_intersects_label_obstacle(c, obstacle)) candidates.push_back(c);
    }
    if (candidates.empty()) return std::nullopt;
    size_t best = 0;  // Iterator::min_by keeps the first of equal minima
    for (size_t i = 1; i < candidates.size(); ++i)
        if (path_length(candidates[i]) < path_length(candidates[best])) best = i;
    return candidates[best];
}

struct RouteLabelState {
    std::vector<std::optional<RouteLabelPlan>> plans;
    std::vector<std::optional<Point>> label_anchors;
    std::vector<Obstacle> obstacles;  // label obstacles including the reserved label boxes
    DiagramKind kind = DiagramKind::Flowchart;
};

// sync_route_label_plan_with_points(): after an edge is (re)routed, keep its label plan in step.
// Flowcharts keep the reserved centre and detour the path around it; the other kinds move the
// label onto the routed path (and ask the path to pass through it, except for state diagrams).
void sync_route_label_plan_with_points(size_t idx, std::vector<Point>& points, RouteLabelState& st,
                                       const std::vector<std::optional<TextBlock>>& edge_route_labels, Direction direction,
                                       float pad_x, float pad_y, bool update_obstacle) {
    (void)direction;
    if (idx >= st.plans.size() || !st.plans[idx]) return;
    RouteLabelPlan& plan = *st.plans[idx];
    if (points.size() < 2) return;
    bool preserve_reserved_center = st.kind == DiagramKind::Flowchart;
    Point label_center = plan.center;
    if (!preserve_reserved_center) {
        if (auto at = path_point_at_progress(points, plan.progress)) label_center = *at;
        else if (auto anchor = edge_label_anchor_from_points(points)) label_center = *anchor;
    }
    plan.center = label_center;
    st.label_anchors[idx] = label_center;
    if (preserve_reserved_center && idx < edge_route_labels.size() && edge_route_labels[idx]) {
        const TextBlock& label = *edge_route_labels[idx];
        Obstacle label_obstacle{plan.obstacle_id, label_center.first - label.width / 2.0f - pad_x,
                                label_center.second - label.height / 2.0f - pad_y, label.width + 2.0f * pad_x,
                                label.height + 2.0f * pad_y, nullptr};
        if (auto detoured = detour_flowchart_path_around_label(points, label_obstacle, fmax_(fmax_(pad_y, pad_x) * 0.5f, 8.0f)))
            points = *detoured;
    }
    if (!preserve_reserved_center && st.kind != DiagramKind::Flowchart && st.kind != DiagramKind::State)
        insert_label_via_point(points, label_center);
    if (!update_obstacle) return;
    if (idx < edge_route_labels.size() && edge_route_labels[idx] && plan.obstacle_index < st.obstacles.size()) {
        const TextBlock& label = *edge_route_labels[idx];
        Obstacle& o = st.obstacles[plan.obstacle_index];
        o.x = label_center.first - label.width / 2.0f - pad_x;
        o.y = label_center.second - label.height / 2.0f - pad_y;
        o.width = label.width + 2.0f * pad_x;
        o.height = label.height + 2.0f * pad_y;
    }
}

// ---- re-routing one edge with the full router (used by the global passes) ----

struct ReroutePrep {
    const Graph* graph;
    const NodeMap* nodes;
    const std::vector<SubgraphLayout>* subgraphs;
    const std::vector<Obstacle>* obstacles;
    const RoutingGrid* routing_grid;
    const std::vector<EdgePortInfo>* edge_ports;
    const std::vector<float>* lane_offsets;
    const std::vector<ReservedRoutingChannel>* reserved_channels;
    const LayoutConfig* config;
    float pad_x, pad_y;
};

size_t max_label_chars_of(const Edge& edge) {
    size_t m = 0;
    for (const auto* l : {&edge.label, &edge.start_label, &edge.end_label})
        if (*l) m = std::max(m, char_count(**l));
    return m;
}

std::vector<Point> reroute_edge(const ReroutePrep& p, size_t idx, const EdgeOccupancy* occupancy, const std::vector<Segment>& existing_segments,
                                const RouteLabelState& st, const std::optional<std::string>& preferred_label_id,
                                const Obstacle* preferred_label_obstacle, float preferred_label_clearance) {
    const Edge& edge = p.graph->edges[idx];
    auto endpoints = effective_edge_endpoint_layouts(*p.graph, *p.nodes, *p.subgraphs, edge);
    const NodeLayout& from = endpoints->first;
    const NodeLayout& to = endpoints->second;
    EdgePortInfo port_info = idx < p.edge_ports->size() ? (*p.edge_ports)[idx] : EdgePortInfo{};
    float stub_len = port_stub_length(*p.config, from, to);
    size_t max_edge_label_chars = max_label_chars_of(edge);
    bool has_endpoint_label = edge.start_label.has_value() || edge.end_label.has_value();
    bool avoid_short_tie = has_endpoint_label || max_edge_label_chars >= FLOWCHART_EDGE_LABEL_WRAP_TRIGGER_CHARS;
    RouteContext rc;
    rc.from_id = &edge.from;
    rc.to_id = &edge.to;
    rc.from = &from;
    rc.to = &to;
    rc.direction = p.graph->direction;
    rc.config = p.config;
    rc.obstacles = p.obstacles;
    rc.label_obstacles = &st.obstacles;
    rc.fast_route = false;
    rc.base_offset = idx < p.lane_offsets->size() ? (*p.lane_offsets)[idx] : 0.0f;
    rc.start_side = port_info.start_side;
    rc.end_side = port_info.end_side;
    rc.start_offset = port_info.start_offset;
    rc.end_offset = port_info.end_offset;
    rc.stub_len = stub_len;
    rc.start_inset = edge.arrow_start ? arrowhead_inset(p.graph->kind, edge.arrow_start_kind) : 0.0f;
    rc.end_inset = edge.arrow_end ? arrowhead_inset(p.graph->kind, edge.arrow_end_kind) : 0.0f;
    rc.prefer_shorter_ties = !avoid_short_tie;
    rc.preferred_label_id = preferred_label_id;
    rc.preferred_label_center = std::nullopt;
    rc.preferred_label_obstacle = preferred_label_obstacle;
    rc.preferred_label_clearance = preferred_label_clearance;
    rc.reserved_channels = p.reserved_channels;
    rc.force_preferred_label_via = false;
    rc.coarse_grid_retry = true;
    rc.allow_exterior_fallback = false;
    return route_edge_with_avoidance(rc, occupancy, p.routing_grid, &existing_segments);
}

void optimize_flowchart_routes_globally(const ReroutePrep& p, const PerformanceProfile& performance,
                                        const std::vector<size_t>& route_order, bool route_labels_via, RouteLabelState& st,
                                        const std::vector<std::optional<TextBlock>>& edge_route_labels, Paths& routed) {
    const Graph& graph = *p.graph;
    size_t passes = performance.global_route_passes;
    if (passes == 0) return;
    std::vector<size_t> order = route_order;
    if (order.size() != graph.edges.size()) {
        order.clear();
        for (size_t i = 0; i < graph.edges.size(); ++i) order.push_back(i);
    }
    for (size_t pass = 0; pass < passes; ++pass) {
        bool changed = false;
        for (size_t idx : order) {
            if (idx >= graph.edges.size()) continue;
            const Edge& edge = graph.edges[idx];
            if (edge.from == edge.to || idx >= routed.size() || routed[idx].size() < 2) continue;
            std::vector<Segment> existing_segments = collect_other_flowchart_segments(routed, idx);
            std::optional<std::pair<std::string, size_t>> preferred_label;
            if (idx < st.plans.size() && st.plans[idx]) preferred_label = std::make_pair(st.plans[idx]->obstacle_id, st.plans[idx]->obstacle_index);
            std::optional<std::string> preferred_label_id;
            if (preferred_label) preferred_label_id = preferred_label->first;
            float preferred_label_clearance = fmax_(fmax_(p.pad_x, p.pad_y) + p.config->node_spacing * 0.25f, 8.0f);
            GlobalRouteScore baseline = score_global_route_candidate(routed[idx], edge, *p.nodes, *p.obstacles, st.obstacles,
                                                                     existing_segments, preferred_label_id);
            auto occupancy = occupancy_from_other_routes(routed, idx, *p.config);
            std::vector<Point> candidate;
            {
                const Obstacle* pref_obs = nullptr;
                if (preferred_label && preferred_label->second < st.obstacles.size()) pref_obs = &st.obstacles[preferred_label->second];
                candidate = reroute_edge(p, idx, occupancy ? &*occupancy : nullptr, existing_segments, st, preferred_label_id, pref_obs,
                                         preferred_label_clearance);
            }
            if (route_labels_via)
                sync_route_label_plan_with_points(idx, candidate, st, edge_route_labels, graph.direction, p.pad_x, p.pad_y, true);
            GlobalRouteScore score = score_global_route_candidate(candidate, edge, *p.nodes, *p.obstacles, st.obstacles,
                                                                  existing_segments, preferred_label_id);
            if (baseline.hard == 0 && score.hard > 0) continue;
            if (baseline.non_endpoint_hits == 0 && score.non_endpoint_hits > 0) continue;
            if (score.endpoint_reentries > baseline.endpoint_reentries) continue;
            if (score.bends > baseline.bends + 2 && score.len > baseline.len * 1.8f) continue;
            if (score.len > baseline.len * 3.0f + p.config->node_spacing * 4.0f) continue;
            if (global_route_score_is_better(score, baseline)) {
                routed[idx] = candidate;
                changed = true;
            }
        }
        if (!changed) break;
    }
}

// ---- negotiated congestion (edge_pipeline.rs negotiate_flowchart_route_congestion) ----
// Dense graphs (12+ edges): edges that sit in crowded routing cells are routed again against the
// cells the other edges use (plus a "history" of cells that stayed crowded), and kept if better.

std::optional<EdgeOccupancy> combined_occupancy_from_other_routes(const Paths& routed, size_t excluded_idx, const EdgeOccupancy& history,
                                                                  const LayoutConfig& config) {
    EdgeOccupancy occupancy(fmax_(config.node_spacing, MIN_NODE_SPACING_FLOOR) * EDGE_OCCUPANCY_CELL_RATIO);
    if (!history.is_empty()) occupancy.merge_from(history);
    for (size_t idx = 0; idx < routed.size(); ++idx) {
        if (idx == excluded_idx || routed[idx].size() < 2) continue;
        occupancy.add_path(routed[idx]);
    }
    if (occupancy.is_empty()) return std::nullopt;
    return occupancy;
}

unsigned congestion_overlap_trigger(const std::vector<Point>& points, const EdgeOccupancy& occupancy) {
    float v = (path_length(points) / occupancy.cell_size()) * 0.22f;
    return static_cast<unsigned>(std::ceil(fmax_(v, 3.0f)));
}

bool congestion_improves_enough(const GlobalRouteScore& c, const GlobalRouteScore& b, unsigned candidate_congestion,
                                unsigned baseline_congestion, const LayoutConfig& config) {
    if (b.hard == 0 && c.hard > 0) return false;
    if (b.non_endpoint_hits == 0 && c.non_endpoint_hits > 0) return false;
    if (c.endpoint_reentries > b.endpoint_reentries) return false;
    if (c.label_hits > b.label_hits) return false;
    if (c.len > b.len * 2.2f + config.node_spacing * 4.0f) return false;
    if (c.bends > b.bends) return false;
    if (c.len > b.len * 1.15f + config.node_spacing) return false;
    if (global_route_score_is_better(c, b)) return true;
    unsigned congestion_gain = baseline_congestion > candidate_congestion ? baseline_congestion - candidate_congestion : 0;
    if (congestion_gain < 5) return false;
    return c.crossings < b.crossings || c.overlap + 0.05f < b.overlap || candidate_congestion * 2u < baseline_congestion;
}

void negotiate_flowchart_route_congestion(const ReroutePrep& p, const PerformanceProfile& performance,
                                          const std::vector<size_t>& route_order, bool route_labels_via, RouteLabelState& st,
                                          const std::vector<std::optional<TextBlock>>& edge_route_labels, Paths& routed) {
    const Graph& graph = *p.graph;
    size_t passes = performance.negotiated_congestion_passes;
    if (passes == 0) return;
    std::vector<size_t> order = route_order;
    if (order.size() != graph.edges.size()) {
        order.clear();
        for (size_t i = 0; i < graph.edges.size(); ++i) order.push_back(i);
    }
    EdgeOccupancy history(fmax_(p.config->node_spacing, MIN_NODE_SPACING_FLOOR) * EDGE_OCCUPANCY_CELL_RATIO);

    for (size_t pass = 0; pass < passes; ++pass) {
        bool changed = false;
        for (size_t idx : order) {
            if (idx >= graph.edges.size()) continue;
            const Edge& edge = graph.edges[idx];
            if (edge.from == edge.to || idx >= routed.size() || routed[idx].size() < 2) continue;
            auto occupancy = combined_occupancy_from_other_routes(routed, idx, history, *p.config);
            if (!occupancy) continue;
            unsigned baseline_congestion = occupancy->score_path(routed[idx]);
            unsigned baseline_overlap = occupancy->overlap_count(routed[idx]);
            if (baseline_overlap < congestion_overlap_trigger(routed[idx], *occupancy) && baseline_congestion < 18) continue;

            std::vector<Segment> existing_segments = collect_other_flowchart_segments(routed, idx);
            std::optional<std::pair<std::string, size_t>> preferred_label;
            if (idx < st.plans.size() && st.plans[idx]) preferred_label = std::make_pair(st.plans[idx]->obstacle_id, st.plans[idx]->obstacle_index);
            std::optional<std::string> preferred_label_id;
            if (preferred_label) preferred_label_id = preferred_label->first;
            float preferred_label_clearance = fmax_(fmax_(p.pad_x, p.pad_y) + p.config->node_spacing * 0.25f, 8.0f);
            GlobalRouteScore baseline = score_global_route_candidate(routed[idx], edge, *p.nodes, *p.obstacles, st.obstacles,
                                                                     existing_segments, preferred_label_id);
            const Obstacle* pref_obs = nullptr;
            if (preferred_label && preferred_label->second < st.obstacles.size()) pref_obs = &st.obstacles[preferred_label->second];
            std::vector<Point> candidate = reroute_edge(p, idx, &*occupancy, existing_segments, st, preferred_label_id, pref_obs,
                                                        preferred_label_clearance);
            if (route_labels_via)
                sync_route_label_plan_with_points(idx, candidate, st, edge_route_labels, graph.direction, p.pad_x, p.pad_y, true);
            GlobalRouteScore score = score_global_route_candidate(candidate, edge, *p.nodes, *p.obstacles, st.obstacles,
                                                                  existing_segments, preferred_label_id);
            unsigned candidate_congestion = occupancy->score_path(candidate);
            if (congestion_improves_enough(score, baseline, candidate_congestion, baseline_congestion, *p.config)) {
                if (candidate_congestion >= baseline_congestion) history.add_path_with_weight(candidate, 2);
                routed[idx] = candidate;
                changed = true;
            } else if (baseline_overlap >= congestion_overlap_trigger(routed[idx], *occupancy)) {
                history.add_path_with_weight(routed[idx], 2);
            }
        }
        if (!changed) break;
    }
}

// ---- endpoint ports and repairs ----

void enforce_flowchart_endpoint_ports(const Graph& graph, const NodeMap& nodes, const std::vector<EdgePortInfo>& edge_ports,
                                      Paths& routed, const LayoutConfig& config) {
    float stub_len = fmin_(fmax_(routing_cell_size(config) * 0.6f, 6.0f), fmax_(config.node_spacing, MIN_NODE_SPACING_FLOOR) * 0.35f);
    for (size_t idx = 0; idx < graph.edges.size(); ++idx) {
        const Edge& edge = graph.edges[idx];
        if (idx >= routed.size()) continue;
        std::vector<Point>& points = routed[idx];
        if (points.size() < 2) continue;
        if (edge.from == edge.to) {
            auto nit = nodes.find(edge.from);
            if (nit != nodes.end()) {
                const NodeLayout& node = nit->second;
                EdgeSide start_side = endpoint_side_for_point(node, points[0]);
                if (!side_points_outward(start_side, points[0], points[1])) {
                    Point stub = port_stub_point(points[0], start_side, stub_len);
                    if (std::fabs(stub.first - points[1].first) > 0.5f || std::fabs(stub.second - points[1].second) > 0.5f)
                        points.insert(points.begin() + 1, stub);
                }
                size_t len = points.size();
                if (len >= 2) {
                    Point end = points[len - 1];
                    Point prev = points[len - 2];
                    EdgeSide end_side = endpoint_side_for_point(node, end);
                    if (!side_points_outward(end_side, end, prev)) {
                        Point stub = port_stub_point(end, end_side, stub_len);
                        if (std::fabs(stub.first - prev.first) > 0.5f || std::fabs(stub.second - prev.second) > 0.5f)
                            points.insert(points.begin() + static_cast<long>(len) - 1, stub);
                    }
                }
                points = compress_path(points);
            }
            continue;
        }
        if (idx >= edge_ports.size()) continue;
        const EdgePortInfo& port = edge_ports[idx];
        if (nodes.count(edge.from) && !side_points_outward(port.start_side, points[0], points[1])) {
            Point stub = port_stub_point(points[0], port.start_side, stub_len);
            if (std::fabs(stub.first - points[1].first) > 0.5f || std::fabs(stub.second - points[1].second) > 0.5f)
                points.insert(points.begin() + 1, stub);
        }
        size_t len = points.size();
        if (len >= 2 && nodes.count(edge.to) && !side_points_outward(port.end_side, points[len - 1], points[len - 2])) {
            Point stub = port_stub_point(points[len - 1], port.end_side, stub_len);
            if (std::fabs(stub.first - points[len - 2].first) > 0.5f || std::fabs(stub.second - points[len - 2].second) > 0.5f)
                points.insert(points.begin() + static_cast<long>(len) - 1, stub);
        }
        points = compress_path(points);
    }
}

struct EndpointRepairScore {
    size_t hard, endpoint_reentries, crossings;
    float overlap;
    size_t bends;
    float len;
};

EndpointRepairScore endpoint_repair_score(const std::vector<Point>& points, const Edge& edge, const NodeMap& nodes,
                                          const std::vector<Segment>& other_segments) {
    size_t violations = flowchart_endpoint_direction_violation_count(points, edge, nodes);
    size_t non_endpoint_hits = flowchart_path_hits_non_endpoint_nodes(points, edge.from, edge.to, nodes) ? 1 : 0;
    size_t debt = flowchart_endpoint_reentry_count(points, edge, nodes);
    auto [crossings, overlap] = edge_crossings_with_existing(points, other_segments);
    return {violations + non_endpoint_hits, debt, crossings, overlap, path_bend_count(points), path_length(points)};
}

bool endpoint_score_is_better(const EndpointRepairScore& c, const EndpointRepairScore& b) {
    if (c.hard != b.hard) return c.hard < b.hard;
    if (c.endpoint_reentries != b.endpoint_reentries) return c.endpoint_reentries < b.endpoint_reentries;
    if (c.crossings != b.crossings) return c.crossings < b.crossings;
    if (std::fabs(c.overlap - b.overlap) > 0.05f) return c.overlap < b.overlap;
    if (c.bends != b.bends) return c.bends < b.bends;
    return c.len + 1.0f < b.len;
}

bool endpoint_score_repairs_baseline(const EndpointRepairScore& c, const EndpointRepairScore& b) {
    return c.hard < b.hard || (c.hard == b.hard && c.endpoint_reentries < b.endpoint_reentries);
}

void repair_flowchart_endpoint_reentries_by_rerouting(const ReroutePrep& p, const std::vector<Obstacle>& label_obstacles,
                                                      std::vector<EdgePortInfo>& edge_ports, Paths& routed) {
    const Graph& graph = *p.graph;
    const EdgeSide SIDES[4] = {EdgeSide::Left, EdgeSide::Right, EdgeSide::Top, EdgeSide::Bottom};
    for (size_t idx = 0; idx < routed.size(); ++idx) {
        if (idx >= graph.edges.size()) continue;
        const Edge& edge = graph.edges[idx];
        if (edge.from == edge.to || routed[idx].size() < 2) continue;
        std::vector<Segment> other_segments = collect_other_flowchart_segments(routed, idx);
        EndpointRepairScore baseline = endpoint_repair_score(routed[idx], edge, *p.nodes, other_segments);
        if (baseline.hard == 0 && baseline.endpoint_reentries == 0) continue;
        auto endpoints = effective_edge_endpoint_layouts(graph, *p.nodes, *p.subgraphs, edge);
        if (!endpoints) continue;
        const NodeLayout& from = endpoints->first;
        const NodeLayout& to = endpoints->second;
        EdgePortInfo current_port = idx < edge_ports.size() ? edge_ports[idx] : EdgePortInfo{};
        float stub_len = port_stub_length(*p.config, from, to);
        EndpointRepairScore best_score = baseline;
        std::optional<std::vector<Point>> best_points;
        EdgePortInfo best_port = current_port;
        for (EdgeSide start_side : SIDES)
            for (EdgeSide end_side : SIDES) {
                EdgePortInfo cp{start_side, end_side, start_side == current_port.start_side ? current_port.start_offset : 0.0f,
                                end_side == current_port.end_side ? current_port.end_offset : 0.0f};
                RouteContext rc;
                rc.from_id = &edge.from;
                rc.to_id = &edge.to;
                rc.from = &from;
                rc.to = &to;
                rc.direction = graph.direction;
                rc.config = p.config;
                rc.obstacles = p.obstacles;
                rc.label_obstacles = &label_obstacles;
                rc.fast_route = false;
                rc.base_offset = idx < p.lane_offsets->size() ? (*p.lane_offsets)[idx] : 0.0f;
                rc.start_side = cp.start_side;
                rc.end_side = cp.end_side;
                rc.start_offset = cp.start_offset;
                rc.end_offset = cp.end_offset;
                rc.stub_len = stub_len;
                rc.start_inset = edge.arrow_start ? arrowhead_inset(graph.kind, edge.arrow_start_kind) : 0.0f;
                rc.end_inset = edge.arrow_end ? arrowhead_inset(graph.kind, edge.arrow_end_kind) : 0.0f;
                rc.prefer_shorter_ties = true;
                rc.reserved_channels = p.reserved_channels;
                rc.force_preferred_label_via = false;
                rc.coarse_grid_retry = true;
                rc.allow_exterior_fallback = false;
                std::vector<Point> candidate = route_edge_with_avoidance(rc, nullptr, p.routing_grid, &other_segments);
                EndpointRepairScore score = endpoint_repair_score(candidate, edge, *p.nodes, other_segments);
                if (!endpoint_score_repairs_baseline(score, baseline)) continue;
                if (score.len > baseline.len * 4.0f + p.config->node_spacing * 4.0f) continue;
                if (endpoint_score_is_better(score, best_score)) {
                    best_score = score;
                    best_points = candidate;
                    best_port = cp;
                }
            }
        if (best_points) {
            routed[idx] = *best_points;
            if (idx < edge_ports.size()) edge_ports[idx] = best_port;
        }
    }
}

// ---- post_route.rs: label anchor adjustment and EdgeLayout construction ----

bool segment_intersects_rect_liangbarsky(Point a, Point b, Rect4 rect) {
    auto [rx, ry, rw, rh] = rect;
    float dx = b.first - a.first, dy = b.second - a.second;
    float p[4] = {-dx, dx, -dy, dy};
    float q[4] = {a.first - rx, rx + rw - a.first, a.second - ry, ry + rh - a.second};
    float u1 = 0.0f, u2 = 1.0f;
    for (int i = 0; i < 4; ++i) {
        float pi = p[i], qi = q[i];
        if (std::fabs(pi) <= 1.1920929e-7f) {
            if (qi < 0.0f) return false;
            continue;
        }
        float t = qi / pi;
        if (pi < 0.0f) {
            if (t > u2) return false;
            u1 = fmax_(u1, t);
        } else {
            if (t < u1) return false;
            u2 = fmin_(u2, t);
        }
    }
    return true;
}

std::optional<Point> adjusted_flowchart_label_anchor(std::optional<Point> anchor, const std::optional<TextBlock>& label,
                                                     const std::vector<Point>& points) {
    if (!anchor || !label) return anchor;
    float clearance = 8.0f;
    auto overlaps = [&](Point candidate) {
        Rect4 rect{candidate.first - label->width / 2.0f - clearance, candidate.second - label->height / 2.0f - clearance,
                   label->width + clearance * 2.0f, label->height + clearance * 2.0f};
        for (size_t s = 0; s + 1 < points.size(); ++s)
            if (segment_intersects_rect_liangbarsky(points[s], points[s + 1], rect)) return true;
        return false;
    };
    if (!overlaps(*anchor) && label->height < 80.0f) return anchor;
    if (label->height >= 80.0f) return Point{anchor->first, anchor->second - label->height - clearance * 3.0f};
    float offset_y = label->height + clearance * 3.0f;
    float offset_x = label->width * 0.5f + clearance * 3.0f;
    Point cands[8] = {{anchor->first, anchor->second - offset_y},
                      {anchor->first, anchor->second + offset_y},
                      {anchor->first - offset_x, anchor->second},
                      {anchor->first + offset_x, anchor->second},
                      {anchor->first - offset_x, anchor->second - offset_y},
                      {anchor->first + offset_x, anchor->second - offset_y},
                      {anchor->first - offset_x, anchor->second + offset_y},
                      {anchor->first + offset_x, anchor->second + offset_y}};
    for (const Point& c : cands)
        if (!overlaps(c)) return c;
    return anchor;
}

// merge_edge_style / resolve_edge_style (layout/mod.rs)
EdgeStyleOverride resolve_edge_style(size_t idx, const Graph& graph) {
    EdgeStyleOverride style = graph.edge_style_default ? *graph.edge_style_default : EdgeStyleOverride{};
    auto it = graph.edge_styles.find(idx);
    if (it != graph.edge_styles.end()) {
        const EdgeStyleOverride& s = it->second;
        if (s.stroke) style.stroke = s.stroke;
        if (s.stroke_width && std::isfinite(*s.stroke_width)) style.stroke_width = fmax_(*s.stroke_width, 0.0f);
        if (s.dasharray) style.dasharray = s.dasharray;
        if (s.label_color) style.label_color = s.label_color;
    }
    return style;
}

}  // namespace

// Exported for sequence.cpp (the anonymous-namespace function above does the work).
EdgeStyleOverride resolve_edge_style_of(size_t idx, const Graph& graph) { return resolve_edge_style(idx, graph); }

// ======================================================================================
// build_routed_edges
// ======================================================================================

std::vector<EdgeLayout> build_routed_edges(const Graph& graph, const std::map<std::string, NodeLayout>& nodes,
                                           const std::vector<SubgraphLayout>& subgraphs, const LayoutConfig& config, size_t layout_node_count,
                                           const std::vector<std::optional<TextBlock>>& edge_route_labels,
                                           const std::vector<std::optional<TextBlock>>& edge_start_labels,
                                           const std::vector<std::optional<TextBlock>>& edge_end_labels, bool tiny_graph) {
    std::vector<Obstacle> obstacles = build_obstacles(nodes, subgraphs, config);
    std::vector<Obstacle> label_obstacles = build_label_obstacles_for_routing(nodes, subgraphs);
    std::unique_ptr<RoutingGrid> routing_grid_owner;
    if (config.flowchart.routing.enable_grid_router && !tiny_graph) routing_grid_owner = build_routing_grid(obstacles, config);
    const RoutingGrid* routing_grid = routing_grid_owner.get();
    std::vector<ReservedRoutingChannel> reserved_channels = build_flowchart_reserved_channels(graph, nodes, config);

    std::optional<NodeBounds> content_bounds = visible_node_bounds(nodes);
    std::unordered_map<std::string, size_t> node_degrees;
    for (const auto& e : graph.edges) {
        ++node_degrees[e.from];
        ++node_degrees[e.to];
    }
    std::vector<EdgeRole> edge_roles = classify_edge_roles(graph);
    SideLoads side_loads;
    PerformanceProfile performance = flowchart_routing_performance_profile(graph, layout_node_count, tiny_graph);
    std::optional<SideSearchProfile> side_search = performance.side_search;
    std::vector<EdgePortInfo> edge_ports(graph.edges.size());
    std::vector<std::pair<EdgeSide, EdgeSide>> selected_edge_sides(graph.edges.size(), {EdgeSide::Right, EdgeSide::Left});
    std::vector<Segment> side_choice_segments;

    auto degree_of = [&](const std::string& id) {
        auto it = node_degrees.find(id);
        return it == node_degrees.end() ? size_t(0) : it->second;
    };

    for (size_t idx = 0; idx < graph.edges.size(); ++idx) {
        const Edge& edge = graph.edges[idx];
        auto endpoints = effective_edge_endpoint_layouts(graph, nodes, subgraphs, edge);
        if (!endpoints) continue;
        const NodeLayout& from = endpoints->first;
        const NodeLayout& to = endpoints->second;
        size_t from_degree = degree_of(edge.from), to_degree = degree_of(edge.to);
        EdgeRole role = idx < edge_roles.size() ? edge_roles[idx] : EdgeRole{};
        bool allow_low_degree_balancing = allow_low_degree_balancing_for_edge(edge, role, from_degree, to_degree);
        Sides primary_sides = edge_sides(from, to, graph.direction);
        Sides balanced = edge_sides_balanced(edge.from, edge.to, from, to, allow_low_degree_balancing, role.is_back_edge,
                                             graph.direction, node_degrees, side_loads);
        Sides selected_sides;
        if (edge.from != edge.to && side_search) {
            SideChoiceContext sc{graph.direction, content_bounds, &node_degrees, &side_loads, &obstacles, &label_obstacles,
                                 routing_grid, &side_choice_segments, &config, *side_search};
            selected_sides = choose_routed_flowchart_sides(edge.from, edge.to, from, to, primary_sides, balanced, role, sc);
        } else if (role.is_back_edge) {
            selected_sides = choose_outer_back_edge_sides(from, to, graph.direction, content_bounds, balanced);
        } else {
            selected_sides = balanced;
        }
        if (!role.is_back_edge && (std::get<0>(selected_sides) != std::get<0>(primary_sides) ||
                                   std::get<1>(selected_sides) != std::get<1>(primary_sides))) {
            std::vector<Point> candidate_points = {anchor_point_for_node(from, std::get<0>(selected_sides), 0.0f),
                                                   anchor_point_for_node(to, std::get<1>(selected_sides), 0.0f)};
            std::vector<Point> primary_points = {anchor_point_for_node(from, std::get<0>(primary_sides), 0.0f),
                                                 anchor_point_for_node(to, std::get<1>(primary_sides), 0.0f)};
            auto [candidate_crossings, o1] = edge_crossings_with_existing(candidate_points, side_choice_segments);
            auto [primary_crossings, o2] = edge_crossings_with_existing(primary_points, side_choice_segments);
            (void)o1;
            (void)o2;
            if (candidate_crossings > primary_crossings) selected_sides = primary_sides;
        }
        EdgeSide start_side = std::get<0>(selected_sides), end_side = std::get<1>(selected_sides);
        bump_side_load(side_loads, edge.from, start_side);
        bump_side_load(side_loads, edge.to, end_side);
        edge_ports[idx] = EdgePortInfo{start_side, end_side, 0.0f, 0.0f};
        selected_edge_sides[idx] = {start_side, end_side};
        side_choice_segments.push_back({anchor_point_for_node(from, start_side, 0.0f), anchor_point_for_node(to, end_side, 0.0f)});
    }

    SideLoads node_side_counts;
    for (size_t idx = 0; idx < graph.edges.size(); ++idx) {
        bump_side_load(node_side_counts, graph.edges[idx].from, selected_edge_sides[idx].first);
        bump_side_load(node_side_counts, graph.edges[idx].to, selected_edge_sides[idx].second);
    }

    std::map<std::pair<std::string, PortTrack>, std::vector<PortCandidate>> port_candidates;
    for (size_t idx = 0; idx < graph.edges.size(); ++idx) {
        const Edge& edge = graph.edges[idx];
        auto endpoints = effective_edge_endpoint_layouts(graph, nodes, subgraphs, edge);
        if (!endpoints) continue;
        const NodeLayout& from = endpoints->first;
        const NodeLayout& to = endpoints->second;
        size_t from_degree = degree_of(edge.from), to_degree = degree_of(edge.to);
        auto [start_side, end_side] = selected_edge_sides[idx];
        std::array<size_t, 4> zero{0, 0, 0, 0};
        auto sc = node_side_counts.find(edge.from);
        auto ec = node_side_counts.find(edge.to);
        std::array<size_t, 4> start_counts = sc == node_side_counts.end() ? zero : sc->second;
        std::array<size_t, 4> end_counts = ec == node_side_counts.end() ? zero : ec->second;
        Point from_anchor = anchor_point_for_node(from, start_side, 0.0f);
        Point to_anchor = anchor_point_for_node(to, end_side, 0.0f);
        float start_other = ideal_port_pos(to_anchor, from, start_side);
        float end_other = ideal_port_pos(from_anchor, to, end_side);
        PortTrack start_track = port_track_for_assignment(from, start_side, from_degree, start_counts);
        PortTrack end_track = port_track_for_assignment(to, end_side, to_degree, end_counts);
        port_candidates[{edge.from, start_track}].push_back({idx, true, start_other});
        port_candidates[{edge.to, end_track}].push_back({idx, false, end_other});
    }

    float routing_cell = routing_cell_size(config);
    for (auto& [key, candidates] : port_candidates) {
        auto nit = nodes.find(key.first);
        if (nit == nodes.end()) continue;
        const NodeLayout& node = nit->second;
        PortTrack track = key.second;
        float min_other = std::numeric_limits<float>::max(), max_other = std::numeric_limits<float>::lowest();
        for (const auto& c : candidates) {
            min_other = fmin_(min_other, c.other_pos);
            max_other = fmax_(max_other, c.other_pos);
        }
        float span = fmax_(max_other - min_other, 0.0f);
        std::vector<size_t> order(candidates.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return candidates[a].other_pos < candidates[b].other_pos; });
        float node_len = port_track_node_len(node, track);
        float pad = fmax_(fmin_(node_len * config.flowchart.port_pad_ratio, config.flowchart.port_pad_max), config.flowchart.port_pad_min);
        float usable = fmax_(node_len - 2.0f * pad, 1.0f);
        float nominal_sep = usable / (static_cast<float>(candidates.size()) + 1.0f);
        size_t labeled_count = 0;
        for (const auto& c : candidates) {
            if (c.edge_idx >= graph.edges.size()) continue;
            const Edge& e = graph.edges[c.edge_idx];
            auto nonblank = [](const std::optional<std::string>& s) { return s && !trim_view(*s).empty(); };
            if (nonblank(e.label) || nonblank(e.start_label) || nonblank(e.end_label)) ++labeled_count;
        }
        float labeled_edges = static_cast<float>(labeled_count);
        float congestion = static_cast<float>(candidates.size());
        float sep_boost = 1.0f + fmin_(labeled_edges * 0.07f, 0.35f) + fmin_(fmax_(congestion - 3.0f, 0.0f) * 0.03f, 0.25f);
        float grid_floor = routing_cell > 0.0f ? routing_cell * 0.85f : 0.0f;
        float desired_sep = fmax_(nominal_sep * sep_boost, grid_floor);
        float feasible_sep = candidates.size() <= 1 ? usable : usable / (static_cast<float>(candidates.size()) - 0.15f);
        float min_sep = fmin_(desired_sep, fmax_(feasible_sep, nominal_sep));
        bool snap_to_grid = config.flowchart.routing.snap_ports_to_grid && routing_cell > 0.0f && min_sep >= routing_cell * 0.75f;
        float node_start = port_track_node_start(node, track);
        float ideal_span = span;
        float span_frac = usable > 1.0f ? fmin_(ideal_span / usable, 2.0f) : 1.0f;
        float position_weight = clamp_(0.5f + 0.35f * span_frac, 0.50f, 0.85f);
        float rank_weight = 1.0f - position_weight;
        std::vector<std::pair<size_t, float>> desired;
        for (size_t rank = 0; rank < order.size(); ++rank) {
            size_t idx = order[rank];
            const PortCandidate& c = candidates[idx];
            float pos_in_node = c.other_pos - node_start;
            float t_pos = clamp_((pos_in_node - pad) / usable, 0.0f, 1.0f);
            float t_rank = (static_cast<float>(rank) + 0.5f) / static_cast<float>(candidates.size());
            float t = t_pos * position_weight + t_rank * rank_weight;
            float pos = pad + t * usable;
            desired.push_back({idx, pos});
        }
        std::vector<float> assigned(candidates.size(), 0.0f);
        float prev = pad;
        for (size_t order_idx = 0; order_idx < desired.size(); ++order_idx) {
            float p = desired[order_idx].second;
            if (order_idx == 0) p = fmax_(p, pad);
            else p = fmax_(p, prev + min_sep);
            assigned[desired[order_idx].first] = p;
            prev = p;
        }
        float next = pad + usable;
        for (size_t order_idx = desired.size(); order_idx-- > 0;) {
            float p = assigned[desired[order_idx].first];
            if (order_idx + 1 == desired.size()) p = fmin_(p, next);
            else p = fmin_(p, next - min_sep);
            assigned[desired[order_idx].first] = p;
            next = p;
        }
        for (size_t rank = 0; rank < order.size(); ++rank) {
            size_t cand_idx = order[rank];
            const PortCandidate& c = candidates[cand_idx];
            float offset = assigned[cand_idx] - node_len / 2.0f;
            if (snap_to_grid) offset = std::round(offset / routing_cell) * routing_cell;
            if (config.flowchart.port_side_bias != 0.0f) {
                float side_bias_scale = candidates.size() > 2 ? 1.0f + fmin_((static_cast<float>(candidates.size()) - 2.0f) * 0.08f, 0.6f) : 1.0f;
                offset += config.flowchart.port_side_bias * side_bias_scale *
                          (static_cast<float>(rank) - (static_cast<float>(candidates.size()) - 1.0f) / 2.0f);
            }
            if (c.edge_idx < edge_ports.size()) {
                if (c.is_start) edge_ports[c.edge_idx].start_offset = offset;
                else edge_ports[c.edge_idx].end_offset = offset;
            }
        }
    }

    if (side_search) {
        PortRefinementContext prc{&graph, &nodes, &subgraphs, &obstacles, &label_obstacles, routing_grid, &edge_roles, content_bounds,
                                  &node_degrees, &side_loads, &config, *side_search};
        refine_flowchart_ports_with_route_candidates(prc, edge_ports);
    }

    // ---- route every edge ----
    EdgeLaneAssignments lane_assignments = plan_edge_lanes(graph, nodes, subgraphs, config);
    std::vector<float> lane_offsets = lane_assignments.effective_offsets(edge_ports, graph.kind, config);
    const auto& pair_counts = lane_assignments.pair_counts;
    const auto& pair_index = lane_assignments.pair_index;
    const auto& cross_edge_offsets = lane_assignments.cross_edge_offsets;
    (void)pair_counts;
    (void)cross_edge_offsets;

    struct OrderEntry {
        unsigned char priority;
        float cross_axis, main_axis;
        size_t idx;
    };
    std::vector<OrderEntry> route_order;
    bool dense_flowchart_routing = graph.kind == DiagramKind::Flowchart && graph.edges.size() >= 18 &&
                                   graph.edges.size() * 2 >= layout_node_count * 3;
    for (size_t idx = 0; idx < graph.edges.size(); ++idx) {
        const Edge& edge = graph.edges[idx];
        auto endpoints = effective_edge_endpoint_layouts(graph, nodes, subgraphs, edge);
        if (!endpoints) continue;
        const NodeLayout& from = endpoints->first;
        const NodeLayout& to = endpoints->second;
        Point from_center{from.x + from.width / 2.0f, from.y + from.height / 2.0f};
        Point to_center{to.x + to.width / 2.0f, to.y + to.height / 2.0f};
        float dx = to_center.first - from_center.first;
        float dy = to_center.second - from_center.second;
        float cross_axis = is_horizontal(graph.direction) ? std::fabs(dy) : std::fabs(dx);
        float main_axis = is_horizontal(graph.direction) ? std::fabs(dx) : std::fabs(dy);
        bool is_backward = std::get<2>(edge_sides(from, to, graph.direction));
        bool is_dotted = edge.style == EdgeStyle::Dotted;
        bool has_label = edge.label.has_value();
        bool has_open_triangle = edge.arrow_start_kind == EdgeArrowhead::OpenTriangle || edge.arrow_end_kind == EdgeArrowhead::OpenTriangle;
        bool is_secondary = is_dotted || has_label;
        unsigned char priority;
        if (graph.kind == DiagramKind::Class) {
            if (has_open_triangle) priority = 0;
            else if (is_secondary || is_backward) priority = 1;
            else priority = 2;
        } else if (graph.kind == DiagramKind::State) {
            if (is_backward) priority = 0;
            else if (has_label || is_dotted) priority = 1;
            else priority = 2;
        } else if (is_dotted) {
            priority = dense_flowchart_routing ? 1 : 2;
        } else if (has_label || is_backward) {
            priority = 1;
        } else {
            priority = 0;
        }
        route_order.push_back({priority, cross_axis, main_axis, idx});
    }
    size_t steep_count = 0;
    for (const auto& r : route_order)
        if (r.cross_axis > r.main_axis * 0.8f) ++steep_count;
    bool use_cross_axis_order = graph.edges.size() >= 10 && steep_count * 4 >= graph.edges.size();
    if (use_cross_axis_order) {
        std::stable_sort(route_order.begin(), route_order.end(), [](const OrderEntry& a, const OrderEntry& b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            int c1 = cmp_f(a.cross_axis, b.cross_axis);
            if (c1 != 0) return c1 < 0;
            int c2 = cmp_f(b.main_axis, a.main_axis);
            if (c2 != 0) return c2 < 0;
            return a.idx < b.idx;
        });
    } else {
        bool use_priority_preorder = graph.edges.size() >= 10;
        std::stable_sort(route_order.begin(), route_order.end(), [&](const OrderEntry& a, const OrderEntry& b) {
            float len_a = a.cross_axis * a.cross_axis + a.main_axis * a.main_axis;
            float len_b = b.cross_axis * b.cross_axis + b.main_axis * b.main_axis;
            int by_length = cmp_f(len_b, len_a);
            int dense_by_length = cmp_f(len_a, len_b);
            if (use_priority_preorder) {
                if (a.priority != b.priority) return a.priority < b.priority;
                int c = dense_flowchart_routing ? dense_by_length : by_length;
                if (c != 0) return c < 0;
                return a.idx < b.idx;
            }
            if (by_length != 0) return by_length < 0;
            return a.idx < b.idx;
        });
    }

    Paths routed_points(graph.edges.size());
    bool use_occupancy = performance.use_route_occupancy;
    std::optional<EdgeOccupancy> edge_occupancy;
    if (use_occupancy) edge_occupancy.emplace(fmax_(config.node_spacing, MIN_NODE_SPACING_FLOOR) * EDGE_OCCUPANCY_CELL_RATIO);

    // Route-label planning (these diagram kinds have no label dummy nodes, so labels are routed "via"; ER skips it).
    bool route_labels_via = graph.kind != DiagramKind::Er;
    auto [edge_label_pad_x, edge_label_pad_y] = edge_label_padding(graph.kind, config);
    RouteLabelState label_state;
    label_state.plans.assign(graph.edges.size(), std::nullopt);
    label_state.label_anchors.assign(graph.edges.size(), std::nullopt);
    label_state.obstacles = label_obstacles;
    label_state.kind = graph.kind;
    for (size_t idx = 0; idx < graph.edges.size() && route_labels_via; ++idx) {
        if (idx >= edge_route_labels.size() || !edge_route_labels[idx]) continue;
        const TextBlock& label = *edge_route_labels[idx];
        if (label.width <= 0.0f || label.height <= 0.0f) continue;
        if (graph.kind == DiagramKind::Flowchart && !flowchart_label_needs_reserved_route_gap(label, config)) continue;
        auto center = provisional_route_label_center(idx, label, graph, nodes, subgraphs, edge_ports, pair_index, lane_offsets, edge_label_pad_x,
                                                     edge_label_pad_y, config);
        if (!center) continue;
        std::string obstacle_id = "edge-label-reserved:" + std::to_string(idx);
        size_t obstacle_index = label_state.obstacles.size();
        label_state.obstacles.push_back({obstacle_id, center->first - label.width / 2.0f - edge_label_pad_x,
                                         center->second - label.height / 2.0f - edge_label_pad_y, label.width + 2.0f * edge_label_pad_x,
                                         label.height + 2.0f * edge_label_pad_y, nullptr});
        label_state.plans[idx] = RouteLabelPlan{obstacle_id, obstacle_index, 0.5f, *center};
    }

    std::vector<Segment> existing_segments;
    for (const OrderEntry& oe : route_order) {
        size_t idx = oe.idx;
        const Edge& edge = graph.edges[idx];
        float base_offset;
        if (graph.kind == DiagramKind::Flowchart) {
            base_offset = idx < lane_offsets.size() ? lane_offsets[idx] : 0.0f;
        } else {
            auto pc = pair_counts.find(edge_pair_key(edge));
            float total = pc == pair_counts.end() ? 1.0f : static_cast<float>(pc->second);
            float idx_in_pair = static_cast<float>(pair_index[idx]);
            if (total > 1.0f)
                base_offset = (idx_in_pair - (total - 1.0f) / 2.0f) * (config.node_spacing * MULTI_EDGE_OFFSET_RATIO) + cross_edge_offsets[idx];
            else
                base_offset = cross_edge_offsets[idx];
        }
        auto endpoints = effective_edge_endpoint_layouts(graph, nodes, subgraphs, edge);
        if (!endpoints) continue;
        const NodeLayout& from = endpoints->first;
        const NodeLayout& to = endpoints->second;
        EdgePortInfo port_info = idx < edge_ports.size() ? edge_ports[idx] : EdgePortInfo{};
        float default_stub = port_stub_length(config, from, to);
        float stub_len = (graph.kind == DiagramKind::Class || graph.kind == DiagramKind::Er || graph.kind == DiagramKind::Requirement) ? 0.0f : default_stub;
        size_t max_edge_label_chars = max_label_chars_of(edge);
        bool has_endpoint_label = edge.start_label.has_value() || edge.end_label.has_value();
        bool avoid_short_tie = graph.kind == DiagramKind::Flowchart &&
                               (has_endpoint_label || max_edge_label_chars >= FLOWCHART_EDGE_LABEL_WRAP_TRIGGER_CHARS);
        const RouteLabelPlan* plan = (idx < label_state.plans.size() && label_state.plans[idx]) ? &*label_state.plans[idx] : nullptr;
        std::optional<std::string> preferred_label_id;
        const Obstacle* preferred_label_obstacle = nullptr;
        if (plan) {
            preferred_label_id = plan->obstacle_id;
            if (plan->obstacle_index < label_state.obstacles.size()) preferred_label_obstacle = &label_state.obstacles[plan->obstacle_index];
        }
        float preferred_label_clearance = fmax_(fmax_(edge_label_pad_x, edge_label_pad_y) + config.node_spacing * 0.25f, 8.0f);
        RouteContext rc;
        rc.from_id = &edge.from;
        rc.to_id = &edge.to;
        rc.from = &from;
        rc.to = &to;
        rc.direction = graph.direction;
        rc.config = &config;
        rc.obstacles = &obstacles;
        rc.label_obstacles = &label_state.obstacles;
        rc.fast_route = tiny_graph;
        rc.base_offset = base_offset;
        rc.start_side = port_info.start_side;
        rc.end_side = port_info.end_side;
        rc.start_offset = port_info.start_offset;
        rc.end_offset = port_info.end_offset;
        rc.stub_len = stub_len;
        rc.start_inset = edge.arrow_start ? arrowhead_inset(graph.kind, edge.arrow_start_kind) : 0.0f;
        rc.end_inset = edge.arrow_end ? arrowhead_inset(graph.kind, edge.arrow_end_kind) : 0.0f;
        rc.prefer_shorter_ties = !avoid_short_tie;
        rc.preferred_label_id = preferred_label_id;
        // Class/state/ER/flowchart labels are anchored onto the routed path afterwards.
        rc.preferred_label_center = std::nullopt;
        if (plan && graph.kind != DiagramKind::State && graph.kind != DiagramKind::Er && graph.kind != DiagramKind::Flowchart &&
            graph.kind != DiagramKind::Class)
            rc.preferred_label_center = plan->center;
        rc.preferred_label_obstacle = preferred_label_obstacle;
        rc.preferred_label_clearance = graph.kind == DiagramKind::Flowchart ? preferred_label_clearance : 0.0f;
        rc.reserved_channels = &reserved_channels;
        rc.force_preferred_label_via = graph.kind != DiagramKind::Flowchart;
        rc.coarse_grid_retry = graph.kind == DiagramKind::Flowchart;
        rc.allow_exterior_fallback = performance.allow_exterior_fallback;
        // (preferred_label_clearance for flowcharts: (max(pad)+spacing*0.25).max(8))
        bool class_or_er = graph.kind == DiagramKind::Class || graph.kind == DiagramKind::Er;
        bool use_existing_for_edge = !(class_or_er && edge.style == EdgeStyle::Dotted);
        const std::vector<Segment>* existing_for_edge = use_existing_for_edge ? &existing_segments : nullptr;
        std::vector<Point> points = route_edge_with_avoidance(rc, edge_occupancy ? &*edge_occupancy : nullptr, routing_grid, existing_for_edge);
        if (class_or_er) {
            // A fast (grid-less) route competes with the grid route: it wins when it is clean and crosses less.
            RouteContext fast_ctx = rc;
            fast_ctx.fast_route = true;
            fast_ctx.allow_exterior_fallback = false;
            std::vector<Point> fast_points = route_edge_with_avoidance(fast_ctx, nullptr, nullptr, existing_for_edge);
            size_t fast_hits = path_obstacle_intersections(fast_points, *rc.obstacles, *rc.from_id, *rc.to_id);
            size_t fast_label_hits = path_label_intersections(fast_points, *rc.label_obstacles, rc.preferred_label_id);
            if (fast_hits == 0 && fast_label_hits == 0) {
                auto [fast_cross, fast_overlap] = edge_crossings_with_existing(fast_points, existing_segments);
                auto [cur_cross, cur_overlap] = edge_crossings_with_existing(points, existing_segments);
                if (fast_cross < cur_cross || (fast_cross == cur_cross && fast_overlap + 0.25f < cur_overlap)) points = fast_points;
            }
        }
        if (route_labels_via)
            sync_route_label_plan_with_points(idx, points, label_state, edge_route_labels, graph.direction, edge_label_pad_x,
                                              edge_label_pad_y, true);
        if (edge_occupancy) edge_occupancy->add_path(points);
        if (points.size() >= 2)
            for (size_t s = 0; s + 1 < points.size(); ++s) existing_segments.push_back({points[s], points[s + 1]});
        routed_points[idx] = points;
    }

    std::vector<size_t> order_only;
    for (const auto& r : route_order) order_only.push_back(r.idx);
    ReroutePrep prep{&graph, &nodes, &subgraphs, &obstacles, routing_grid, &edge_ports, &lane_offsets, &reserved_channels, &config,
                     edge_label_pad_x, edge_label_pad_y};

    if (graph.kind == DiagramKind::Flowchart) {
        optimize_flowchart_routes_globally(prep, performance, order_only, route_labels_via, label_state, edge_route_labels, routed_points);
        negotiate_flowchart_route_congestion(prep, performance, order_only, route_labels_via, label_state, edge_route_labels, routed_points);
    }

    // post_route::apply_edge_path_cleanup (flowcharts)
    if (graph.kind == DiagramKind::Flowchart) {
        reduce_orthogonal_path_crossings(graph, nodes, routed_points, config);
        deoverlap_flowchart_paths(graph, nodes, routed_points, config);
        simplify_flowchart_detour_rectangles(graph, nodes, routed_points);
        simplify_flowchart_axis_oscillations(routed_points);
        detour_flowchart_paths_around_non_endpoint_nodes(graph, nodes, routed_points, config);
        detour_flowchart_paths_around_foreign_subgraphs(graph, nodes, subgraphs, routed_points, config);
        simplify_flowchart_axis_oscillations(routed_points);
    } else if (graph.kind == DiagramKind::Class || graph.kind == DiagramKind::Er || graph.kind == DiagramKind::State) {
        reduce_orthogonal_path_crossings(graph, nodes, routed_points, config);
        if (graph.kind == DiagramKind::Er) deoverlap_flowchart_paths(graph, nodes, routed_points, config);
        // Rank-adjacent ports can differ by a fraction of a pixel on the cross axis, which turns
        // visually straight connectors into two-bend doglegs: collapse those paths like flowcharts do.
        simplify_flowchart_axis_oscillations(routed_points);
    }

    if (route_labels_via)
        for (size_t idx = 0; idx < routed_points.size(); ++idx)
            sync_route_label_plan_with_points(idx, routed_points[idx], label_state, edge_route_labels, graph.direction,
                                              edge_label_pad_x, edge_label_pad_y, false);
    if (graph.kind == DiagramKind::Flowchart) {
        detour_flowchart_paths_around_non_endpoint_nodes(graph, nodes, routed_points, config);
        simplify_flowchart_axis_oscillations(routed_points);
        detour_flowchart_paths_around_non_endpoint_nodes(graph, nodes, routed_points, config);
    }
    // apply_label_dummy_anchors: flowcharts never have label dummy nodes.
    if (graph.kind == DiagramKind::Flowchart) {
        detour_flowchart_paths_around_non_endpoint_nodes(graph, nodes, routed_points, config);
        // Containment pass: label/anchor sync can drag paths through foreign subgraph boxes.
        detour_flowchart_paths_around_foreign_subgraphs(graph, nodes, subgraphs, routed_points, config);
        simplify_flowchart_axis_oscillations(routed_points);
        enforce_flowchart_endpoint_ports(graph, nodes, edge_ports, routed_points, config);
        repair_flowchart_endpoint_reentries(graph, nodes, routed_points, config);
        repair_flowchart_endpoint_reentries_by_rerouting(prep, label_state.obstacles, edge_ports, routed_points);
        repair_flowchart_endpoint_reentries(graph, nodes, routed_points, config);
    }

    // build_edge_layouts
    std::vector<EdgeLayout> edges;
    edges.reserve(graph.edges.size());
    for (size_t idx = 0; idx < graph.edges.size(); ++idx) {
        const Edge& edge = graph.edges[idx];
        EdgeLayout el;
        el.from = edge.from;
        el.to = edge.to;
        el.label = idx < edge_route_labels.size() ? edge_route_labels[idx] : std::nullopt;
        el.start_label = idx < edge_start_labels.size() ? edge_start_labels[idx] : std::nullopt;
        el.end_label = idx < edge_end_labels.size() ? edge_end_labels[idx] : std::nullopt;
        el.points = routed_points[idx];
        el.directed = edge.directed;
        el.arrow_start = edge.arrow_start;
        el.arrow_end = edge.arrow_end;
        el.arrow_start_kind = edge.arrow_start_kind;
        el.arrow_end_kind = edge.arrow_end_kind;
        el.start_decoration = edge.start_decoration;
        el.end_decoration = edge.end_decoration;
        el.style = edge.style;
        el.override_style = resolve_edge_style(idx, graph);
        if (graph.kind == DiagramKind::Requirement) {
            bool is_contains = edge.label && *edge.label == "contains";
            if (!el.override_style.stroke) el.override_style.stroke = config.requirement.edge_stroke;
            el.override_style.stroke_width = el.override_style.stroke_width ? *el.override_style.stroke_width : config.requirement.edge_stroke_width;
            if (!is_contains && !el.override_style.dasharray) el.override_style.dasharray = config.requirement.edge_dasharray;
            if (!el.override_style.label_color) el.override_style.label_color = config.requirement.edge_label_color;
        }
        if (graph.kind == DiagramKind::Flowchart)
            el.label_anchor = adjusted_flowchart_label_anchor(label_state.label_anchors[idx], el.label, routed_points[idx]);
        else
            el.label_anchor = label_state.label_anchors[idx];
        edges.push_back(std::move(el));
    }
    return edges;
}

}  // namespace mermaid
