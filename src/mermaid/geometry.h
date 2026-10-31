// Mermaid port of layout/geometry.rs: edge sides, node outlines, segment tests, path measures.
#pragma once

#include <cmath>
#include <optional>
#include <tuple>
#include <vector>

#include "layout_types.h"

namespace mermaid {

enum class EdgeSide { Left, Right, Top, Bottom };

using Segment = std::pair<Point, Point>;

// Rust `x as i32` / `x as usize`: saturating, NaN -> 0 (C++ would be undefined).
int f2i(float v);
inline size_t f2usize(float v) { return v <= 0.0f || v != v ? 0 : static_cast<size_t>(v); }

// f32::signum (1.0 for +0.0, -1.0 for -0.0).
inline float signum(float v) { return std::signbit(v) ? -1.0f : 1.0f; }

Point node_center(const NodeLayout& node);

EdgeSide endpoint_side_for_point(const NodeLayout& node, Point point);
bool side_points_outward(EdgeSide side, Point endpoint, Point outside);
bool source_exits_outward(EdgeSide side, Point start, Point next);
bool target_enters_from_outside(EdgeSide side, Point prev, Point end);
bool segment_intrudes_endpoint_rect(EdgeSide side, Point outside, Point endpoint, const NodeLayout& node);

// Outline of a node as a polygon; nullopt for round shapes.
std::optional<std::vector<Point>> shape_polygon_points(const NodeLayout& node);
std::optional<Point> ray_polygon_intersection(Point origin, Point dir, const std::vector<Point>& poly);
std::optional<Point> ray_ellipse_intersection(Point origin, Point dir, Point center, float rx, float ry);

bool segment_hits_node_shape_interior(Point a, Point b, const NodeLayout& node);

float path_length(const std::vector<Point>& points);
std::optional<Point> path_point_at_progress(const std::vector<Point>& points, float progress);
size_t path_bend_count(const std::vector<Point>& points);

using Rect4 = std::tuple<float, float, float, float>;  // x, y, w, h
bool segment_intersects_rect_bounds(Point a, Point b, Rect4 rect);
bool segments_intersect(Point a, Point b, Point c, Point d);

}  // namespace mermaid
