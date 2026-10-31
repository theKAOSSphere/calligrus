// Mermaid port of layout/routing.rs: ports, obstacles, candidate routes and the grid router.
#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "geometry.h"
#include "layout_types.h"

namespace mermaid {

enum class PortAxis { X, Y };

struct EdgePortInfo {
    EdgeSide start_side = EdgeSide::Right;
    EdgeSide end_side = EdgeSide::Left;
    float start_offset = 0.0f;
    float end_offset = 0.0f;
};

struct PortCandidate {
    size_t edge_idx;
    bool is_start;
    float other_pos;
};

struct Obstacle {
    std::string id;
    float x = 0, y = 0, width = 0, height = 0;
    std::shared_ptr<const std::set<std::string>> members;  // Option<HashSet<String>>
};

enum class ReservedRoutingChannelAxis { Vertical, Horizontal };

struct ReservedRoutingChannel {
    ReservedRoutingChannelAxis axis;
    float coord;  // x for vertical channels, y for horizontal ones
    float span_min, span_max;
};

// Per-cell weights of already routed edges (a "heat map" that later routes avoid).
class EdgeOccupancy {
public:
    explicit EdgeOccupancy(float cell);
    std::pair<int, int> cell_index(float x, float y) const;
    unsigned score_path(const std::vector<Point>& points) const;
    unsigned overlap_count(const std::vector<Point>& points) const;
    void add_path(const std::vector<Point>& points) { add_path_with_weight(points, 1); }
    void add_path_with_weight(const std::vector<Point>& points, unsigned multiplier);
    void merge_from(const EdgeOccupancy& other);
    bool is_empty() const { return weights_.empty(); }
    float cell_size() const { return cell_; }
    unsigned weight_at(float x, float y) const;

private:
    struct PairHash {
        size_t operator()(const std::pair<int, int>& p) const {
            return std::hash<long long>()((static_cast<long long>(p.first) << 32) ^ static_cast<unsigned>(p.second));
        }
    };
    float cell_;
    std::unordered_map<std::pair<int, int>, unsigned, PairHash> weights_;  // saturating u16 in Rust
};

class RoutingGrid {
public:
    static std::unique_ptr<RoutingGrid> create(const std::vector<Obstacle>& obstacles, float cell, float margin,
                                               size_t max_cells);
    float cell = 0, min_x = 0, min_y = 0;
    int cols = 0, rows = 0;
    std::vector<std::vector<size_t>> cell_obstacles;
    int index(int ix, int iy) const { return iy * cols + ix; }
    std::optional<std::pair<int, int>> cell_for_point(float x, float y) const;
    Point cell_center(int ix, int iy) const;
};

struct RouteContext {
    const std::string* from_id = nullptr;
    const std::string* to_id = nullptr;
    const NodeLayout* from = nullptr;
    const NodeLayout* to = nullptr;
    Direction direction = Direction::TopDown;
    const LayoutConfig* config = nullptr;
    const std::vector<Obstacle>* obstacles = nullptr;
    const std::vector<Obstacle>* label_obstacles = nullptr;
    bool fast_route = false;
    float base_offset = 0.0f;
    EdgeSide start_side = EdgeSide::Right, end_side = EdgeSide::Left;
    float start_offset = 0.0f, end_offset = 0.0f;
    float stub_len = 0.0f;
    float start_inset = 0.0f, end_inset = 0.0f;
    bool prefer_shorter_ties = true;
    std::optional<std::string> preferred_label_id;
    std::optional<Point> preferred_label_center;
    const Obstacle* preferred_label_obstacle = nullptr;
    float preferred_label_clearance = 0.0f;
    const std::vector<ReservedRoutingChannel>* reserved_channels = nullptr;
    bool force_preferred_label_via = false;
    bool coarse_grid_retry = false;
    bool allow_exterior_fallback = false;
};

// ---- sides and ports ----
std::tuple<EdgeSide, EdgeSide, bool> edge_sides(const NodeLayout& from, const NodeLayout& to, Direction direction);
bool side_is_vertical(EdgeSide side);
PortAxis port_axis(EdgeSide side);
bool edge_axis_is_horizontal(EdgeSide side);
size_t side_slot(EdgeSide side);
using SideLoads = std::unordered_map<std::string, std::array<size_t, 4>>;
size_t side_load_for_node(const SideLoads& loads, const std::string& node_id, EdgeSide side);
void bump_side_load(SideLoads& loads, const std::string& node_id, EdgeSide side);
std::tuple<EdgeSide, EdgeSide, bool> edge_sides_balanced(
    const std::string& from_id, const std::string& to_id, const NodeLayout& from, const NodeLayout& to,
    bool allow_low_degree_balancing, bool prefer_outer_sides, Direction direction,
    const std::unordered_map<std::string, size_t>& node_degrees, const SideLoads& side_loads);

Point apply_port_offset(Point point, EdgeSide side, float offset);
float port_stub_length(const LayoutConfig& config, const NodeLayout& from, const NodeLayout& to);
Point port_stub_point(Point point, EdgeSide side, float length);
float ideal_port_pos(Point remote, const NodeLayout& node, EdgeSide side);
Point anchor_point_for_node(const NodeLayout& node, EdgeSide side, float offset);

// ---- grid, obstacles, scoring helpers ----
float routing_cell_size(const LayoutConfig& config);
std::unique_ptr<RoutingGrid> build_routing_grid(const std::vector<Obstacle>& obstacles, const LayoutConfig& config);
std::vector<Obstacle> build_obstacles(const std::map<std::string, NodeLayout>& nodes,
                                      const std::vector<SubgraphLayout>& subgraphs, const LayoutConfig& config);
std::vector<Obstacle> build_label_obstacles_for_routing(const std::map<std::string, NodeLayout>& nodes,
                                                        const std::vector<SubgraphLayout>& subgraphs);
std::pair<std::string, std::string> edge_pair_key(const Edge& edge);
std::map<std::pair<std::string, std::string>, size_t> build_edge_pair_counts(const std::vector<Edge>& edges);
bool segment_intersects_rect(Point a, Point b, const Obstacle& rect);
float collinear_overlap_length(Point a, Point b, Point c, Point d);
std::pair<size_t, float> edge_crossings_with_existing(const std::vector<Point>& points, const std::vector<Segment>& existing);
size_t path_obstacle_intersections(const std::vector<Point>& points, const std::vector<Obstacle>& obstacles,
                                   const std::string& from_id, const std::string& to_id);
size_t path_label_intersections(const std::vector<Point>& points, const std::vector<Obstacle>& label_obstacles,
                                const std::optional<std::string>& ignore_label_id);
std::vector<Point> compress_path(const std::vector<Point>& points);
float polyline_point_distance(const std::vector<Point>& points, Point point);
void insert_label_via_point(std::vector<Point>& points, Point via);
std::optional<Point> edge_label_anchor_from_points(const std::vector<Point>& points);

// The router entry point: best path for one edge among straight/orthogonal/grid candidates.
std::vector<Point> route_edge_with_avoidance(const RouteContext& ctx, const EdgeOccupancy* occupancy,
                                             const RoutingGrid* grid, const std::vector<Segment>* existing);

// edge_geometry.rs
float arrowhead_inset(DiagramKind kind, std::optional<EdgeArrowhead> arrow_kind);
std::vector<Point> apply_endpoint_insets(std::vector<Point> path, float start_inset, float end_inset);

}  // namespace mermaid
