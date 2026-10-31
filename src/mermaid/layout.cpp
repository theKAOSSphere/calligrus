// Mermaid port: flowchart layout, node placement part. See layout.h.
//
// Rust sources: layout/mod.rs (compute_layout, compute_flowchart_layout, shape_size, finalize),
// layout/flowchart/policy.rs, analysis.rs, manual_layout.rs, objectives.rs (relax_edge_span_constraints),
// flowchart/finalize.rs. Everything that depends on subgraphs is "Unsupported" for now.
#include "layout.h"
#include "kinds.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "geometry.h"
#include "layout_internal.h"
#include "ranking.h"
#include "strutil.h"

namespace mermaid {

namespace {

constexpr float kF32Eps = 1.1920929e-7f;  // f32::EPSILON

// ---- constants from layout/mod.rs ----
constexpr float DIAMOND_SCALE = 0.95f;
constexpr float FORK_JOIN_MIN_WIDTH = 50.0f;
constexpr float FORK_JOIN_HEIGHT_SCALE = 0.4f;
constexpr float FORK_JOIN_MIN_HEIGHT = 8.0f;
constexpr float CIRCLE_EMPTY_HEIGHT_SCALE = 1.4f;
constexpr float CIRCLE_EMPTY_MIN_SIZE = 14.0f;
constexpr float ROUND_RECT_WIDTH_SCALE = 1.1f;
constexpr float ROUND_RECT_HEIGHT_SCALE = 1.05f;
constexpr float CYLINDER_SCALE = 1.1f;
constexpr float HEXAGON_WIDTH_SCALE = 1.2f;
constexpr float HEXAGON_HEIGHT_SCALE = 1.1f;
constexpr float TRAPEZOID_WIDTH_SCALE = 1.2f;
constexpr float CLASS_MIN_HEIGHT_SCALE = 6.5f;
constexpr float REQUIREMENT_MIN_WIDTH_SCALE = 9.5f;
constexpr float KANBAN_MIN_WIDTH_SCALE = 11.0f;
constexpr float KANBAN_MIN_HEIGHT_SCALE = 2.6f;
// ---- state diagram constants ----
constexpr float STATE_MARKER_FONT_SCALE = 0.75f;
constexpr float STATE_MARKER_MIN_SIZE = 10.0f;
constexpr float STATE_DEFAULT_HEIGHT_SCALE = 2.4f;
constexpr float STATE_MARKER_DIV = 3.0f;
constexpr float STATE_MARKER_MIN_SCALE = 0.5f;
constexpr float STATE_MARKER_MAX_SCALE = 0.95f;
constexpr float STATE_PAD_X_SCALE = 0.9f;
constexpr float STATE_PAD_Y_SCALE = 0.65f;
constexpr float STATE_PAD_X_LABEL_RATIO = 0.12f;
constexpr float STATE_PAD_Y_LABEL_RATIO = 0.22f;
constexpr float EDGE_LABEL_PAD_SCALE = 0.35f;
constexpr float ENDPOINT_LABEL_PAD_SCALE = 0.2f;
constexpr float DUAL_ENDPOINT_EXTRA_PAD_SCALE = 0.45f;
constexpr float EDGE_RELAX_STEP_MIN = 24.0f;
constexpr float EDGE_RELAX_GAP_TOLERANCE = 0.5f;
constexpr float MAX_MAIN_GAP_FACTOR = 6.0f;
constexpr size_t FLOWCHART_EDGE_LABEL_WRAP_TRIGGER_CHARS = 34;
constexpr size_t FLOWCHART_EDGE_LABEL_WRAP_MAX_CHARS = 18;

// ---- constants from manual_layout.rs ----
constexpr float LABEL_RANK_FONT_SCALE = 0.5f;  // (only used for label ranks, which flowcharts do not have)
constexpr float LABEL_RANK_MIN_GAP = 8.0f;
constexpr float EDGE_AWARE_CENTER_PULL_LIMIT_RATIO = 0.28f;
constexpr float EDGE_AWARE_CENTER_PULL_BLEND = 0.35f;
constexpr float FLOWCHART_LABEL_MAIN_GAP_PAD_RATIO = 0.35f;
constexpr float FLOWCHART_LABEL_MAIN_GAP_MAX_SCALE = 3.0f;

constexpr size_t kMax = std::numeric_limits<size_t>::max();

float f32_max(float a, float b) { return a > b ? a : (a < b ? b : a); }  // f32::max without NaN care
float f32_min(float a, float b) { return a < b ? a : (a > b ? b : a); }
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ======================================================================================
// policy.rs
// ======================================================================================

struct DensityProfile {
    size_t node_count;
    float edge_count;
    float density;
    float hub_ratio;
};

DensityProfile flowchart_density_profile(const Graph& graph) {
    size_t node_count = graph.nodes.size();
    float edge_count = static_cast<float>(graph.edges.size());
    float density = node_count > 0 ? edge_count / static_cast<float>(node_count) : 0.0f;
    std::unordered_map<std::string, size_t> degree;
    for (const auto& e : graph.edges) {
        ++degree[e.from];
        ++degree[e.to];
    }
    size_t max_degree = 0;
    for (const auto& [id, d] : degree) max_degree = std::max(max_degree, d);
    float hub_ratio = node_count > 0 ? static_cast<float>(max_degree) / static_cast<float>(node_count) : 0.0f;
    return {node_count, edge_count, density, hub_ratio};
}

bool dfs_has_cycle(const std::string& id, const std::unordered_map<std::string, std::vector<std::string>>& out,
                   std::unordered_set<std::string>& visited, std::unordered_set<std::string>& active) {
    if (active.count(id)) return true;
    if (!visited.insert(id).second) return false;
    active.insert(id);
    auto it = out.find(id);
    if (it != out.end())
        for (const auto& next : it->second)
            if (dfs_has_cycle(next, out, visited, active)) return true;
    active.erase(id);
    return false;
}

bool graph_has_directed_cycle(const Graph& graph) {
    std::unordered_map<std::string, std::vector<std::string>> out;
    for (const auto& e : graph.edges) out[e.from].push_back(e.to);
    std::unordered_set<std::string> visited, active;
    for (const auto& [id, node] : graph.nodes)
        if (dfs_has_cycle(id, out, visited, active)) return true;
    return false;
}

bool is_tiny_graph_layout(const Graph& graph) {
    return graph.subgraphs.empty() && graph.nodes.size() <= 4 && graph.edges.size() <= 4 &&
           !graph_has_directed_cycle(graph);
}

LayoutConfig apply_initial_config_heuristics(const Graph& graph, const LayoutConfig& config) {
    LayoutConfig eff = config;
    if (graph.kind == DiagramKind::Requirement) eff.max_label_width_chars = std::max<size_t>(eff.max_label_width_chars, 32);
    if (graph.kind == DiagramKind::Er) {
        // ER diagrams are relationship-dense: tighter packing, more rank-order sweeps.
        eff.node_spacing *= 0.80f;
        eff.rank_spacing *= 0.80f;
        eff.flowchart.order_passes = std::max<size_t>(eff.flowchart.order_passes, 10);
    }
    if (graph.kind == DiagramKind::Flowchart) {
        DensityProfile p = flowchart_density_profile(graph);
        const auto& auto_cfg = config.flowchart.auto_spacing;
        if (auto_cfg.enabled && !auto_cfg.buckets.empty()) {
            float scale = auto_cfg.buckets[0].scale;
            for (const auto& b : auto_cfg.buckets)
                if (p.node_count >= b.min_nodes) scale = b.scale;
            if (p.density > auto_cfg.density_threshold) scale = f32_max(scale, auto_cfg.dense_scale_floor);
            eff.node_spacing = f32_max(eff.node_spacing * scale, auto_cfg.min_spacing);
            eff.rank_spacing = f32_max(eff.rank_spacing * scale, auto_cfg.min_spacing);
        }
        if (p.node_count >= 12 && p.hub_ratio >= 0.40f && p.density <= 2.5f) {
            eff.flowchart.routing.enable_grid_router = false;
            eff.flowchart.routing.snap_ports_to_grid = false;
        }
        if (is_tiny_graph_layout(graph)) {
            eff.flowchart.order_passes = 1;
            eff.flowchart.routing.enable_grid_router = false;
            eff.flowchart.routing.snap_ports_to_grid = false;
        }
    }
    return eff;
}

float adaptive_spacing_for_nodes(const std::map<std::string, NodeLayout>& nodes, float min_spacing,
                                 float max_spacing) {
    float total = 0.0f;
    size_t count = 0;
    for (const auto& [id, node] : nodes) {
        if (node.hidden || node.anchor_subgraph) continue;
        total += (node.width + node.height) * 0.5f;
        ++count;
    }
    if (count == 0) return max_spacing;
    float avg = total / static_cast<float>(count);
    float target = f32_max(avg * 0.5f, min_spacing);
    return f32_min(target, max_spacing);
}

// None when there are no labels or no edges (the Rust Option<f32>).
std::optional<float> flowchart_label_spacing_floor(const Graph& graph, const Theme& theme, const LayoutConfig& config,
                                                   float edge_count) {
    if (edge_count <= 0.0f) return std::nullopt;
    size_t label_char_total = 0, label_count = 0, endpoint_labeled_edges = 0;
    for (const auto& e : graph.edges) {
        bool has_endpoint_label = false;
        if (e.label) { label_char_total += char_count(*e.label); ++label_count; }
        if (e.start_label) { label_char_total += char_count(*e.start_label); ++label_count; has_endpoint_label = true; }
        if (e.end_label) { label_char_total += char_count(*e.end_label); ++label_count; has_endpoint_label = true; }
        if (has_endpoint_label) ++endpoint_labeled_edges;
    }
    if (label_count == 0) return std::nullopt;
    float avg_chars = static_cast<float>(label_char_total) / static_cast<float>(label_count);
    float text_pressure = clampf((avg_chars - 10.0f) / 26.0f, 0.0f, 1.0f);
    float endpoint_pressure = clampf(static_cast<float>(endpoint_labeled_edges) / f32_max(edge_count, 1.0f), 0.0f, 1.0f);
    float pressure = clampf(text_pressure * 0.7f + endpoint_pressure * 0.3f, 0.0f, 1.0f);
    if (pressure <= 0.0f) return std::nullopt;
    float boost = pressure * f32_max(theme.font_size * 1.1f, 8.0f);
    return config.flowchart.auto_spacing.min_spacing + boost;
}

void apply_measured_spacing_heuristics(const Graph& graph, const Theme& theme, LayoutConfig& config,
                                       const std::map<std::string, NodeLayout>& nodes) {
    const auto auto_cfg = config.flowchart.auto_spacing;  // copy: config is modified below
    if (auto_cfg.enabled) {
        float adaptive_node = adaptive_spacing_for_nodes(nodes, auto_cfg.min_spacing, config.node_spacing);
        float adaptive_rank = adaptive_spacing_for_nodes(nodes, auto_cfg.min_spacing, config.rank_spacing);
        if (adaptive_node < config.node_spacing) config.node_spacing = adaptive_node;
        if (adaptive_rank < config.rank_spacing) config.rank_spacing = adaptive_rank;
    }
    if (graph.kind != DiagramKind::Flowchart) return;
    DensityProfile p = flowchart_density_profile(graph);
    if (auto_cfg.enabled && graph.nodes.size() >= 10 && p.hub_ratio >= 0.30f && p.density <= 3.0f) {
        float hub_scale = clampf(0.92f - (p.hub_ratio - 0.30f) * 0.55f, 0.62f, 0.92f);
        float floor = f32_max(auto_cfg.min_spacing * 0.5f, 14.0f);
        config.node_spacing = f32_max(config.node_spacing * hub_scale, floor);
        config.rank_spacing = f32_max(config.rank_spacing * hub_scale, floor);
    }
    if (auto label_floor = flowchart_label_spacing_floor(graph, theme, config, p.edge_count)) {
        config.node_spacing = f32_max(config.node_spacing, *label_floor);
        config.rank_spacing = f32_max(config.rank_spacing, *label_floor);
    }
}

// ======================================================================================
// layout/mod.rs: node sizing
// ======================================================================================

std::pair<float, float> shape_padding_factors(NodeShape shape) {
    switch (shape) {
    case NodeShape::Stadium: return {0.43f, 0.5f};
    case NodeShape::Subroutine: return {0.54f, 0.5f};
    case NodeShape::Parallelogram: return {0.894f, 0.5f};
    case NodeShape::ParallelogramAlt: return {0.904f, 0.5f};
    default: return {1.0f, 1.0f};
    }
}

bool has_divider_line(const TextBlock& label) {
    for (const auto& l : label.lines)
        if (trim_view(l) == "---") return true;
    return false;
}

// Port of shape_size(): the node box for a label; each diagram kind adds its own padding scales.
std::pair<float, float> shape_size(NodeShape shape, const TextBlock& label, const LayoutConfig& config, const Theme& theme,
                                   DiagramKind kind) {
    auto [pad_x_factor, pad_y_factor] = shape_padding_factors(shape);
    float kind_pad_x_scale = 1.0f, kind_pad_y_scale = 1.0f;
    switch (kind) {
    case DiagramKind::Class:
        kind_pad_x_scale = has_divider_line(label) ? 0.85f : 0.4f;
        kind_pad_y_scale = 0.8f;
        break;
    case DiagramKind::Er: kind_pad_x_scale = 1.05f; kind_pad_y_scale = 1.15f; break;
    case DiagramKind::Kanban: kind_pad_x_scale = 2.3f; kind_pad_y_scale = 0.67f; break;
    case DiagramKind::Requirement: kind_pad_x_scale = 0.1f; kind_pad_y_scale = 1.0f; break;
    case DiagramKind::Block: kind_pad_x_scale = 0.5f; kind_pad_y_scale = 0.35f; break;
    default: break;
    }
    float pad_x = config.node_padding_x * pad_x_factor * kind_pad_x_scale;
    float pad_y = config.node_padding_y * pad_y_factor * kind_pad_y_scale;
    if (kind == DiagramKind::State) {
        pad_x = f32_max(theme.font_size * STATE_PAD_X_SCALE, label.width * STATE_PAD_X_LABEL_RATIO);
        pad_y = f32_max(theme.font_size * STATE_PAD_Y_SCALE, label.height * STATE_PAD_Y_LABEL_RATIO);
    }
    float base_width = label.width + pad_x * 2.0f;
    float base_height = label.height + pad_y * 2.0f;
    float width = base_width, height = base_height;
    bool label_empty = label.lines.size() == 1 && trim_view(label.lines[0]).empty();

    switch (shape) {
    case NodeShape::Diamond: {
        // Mermaid draws diamonds as squares sized off the larger dimension.
        float size = f32_max(base_width, base_height) * DIAMOND_SCALE;
        width = size;
        height = size;
        break;
    }
    case NodeShape::ForkJoin:
        width = f32_max(width, FORK_JOIN_MIN_WIDTH);
        height = f32_max(config.node_padding_y * FORK_JOIN_HEIGHT_SCALE, FORK_JOIN_MIN_HEIGHT);
        break;
    case NodeShape::Circle:
    case NodeShape::DoubleCircle: {
        float size = label_empty ? f32_max(config.node_padding_y * CIRCLE_EMPTY_HEIGHT_SCALE, CIRCLE_EMPTY_MIN_SIZE)
                                 : f32_max(width, height);
        width = size;
        height = size;
        break;
    }
    case NodeShape::RoundRect:
        width *= ROUND_RECT_WIDTH_SCALE;
        height *= ROUND_RECT_HEIGHT_SCALE;
        break;
    case NodeShape::Cylinder:
        width *= CYLINDER_SCALE;
        height *= CYLINDER_SCALE;
        break;
    case NodeShape::Hexagon:
        width *= HEXAGON_WIDTH_SCALE;
        height *= HEXAGON_HEIGHT_SCALE;
        break;
    case NodeShape::Trapezoid:
    case NodeShape::TrapezoidAlt:
    case NodeShape::Asymmetric:
        width *= TRAPEZOID_WIDTH_SCALE;
        break;
    default:
        break;
    }
    if (kind == DiagramKind::Class) height = f32_max(height, theme.font_size * CLASS_MIN_HEIGHT_SCALE);
    if (kind == DiagramKind::Requirement) width = f32_max(width, theme.font_size * REQUIREMENT_MIN_WIDTH_SCALE);
    if (kind == DiagramKind::Kanban) {
        width = f32_max(width, theme.font_size * KANBAN_MIN_WIDTH_SCALE);
        height = f32_max(height, theme.font_size * KANBAN_MIN_HEIGHT_SCALE);
    }
    return {width, height};
}

}  // namespace

// Rust merge_node_style (also used by subgraphs.cpp).
void merge_node_style(NodeStyle& target, const NodeStyle& source) {
    if (source.fill) target.fill = source.fill;
    if (source.stroke) target.stroke = source.stroke;
    if (source.text_color) target.text_color = source.text_color;
    if (source.stroke_width && std::isfinite(*source.stroke_width))
        target.stroke_width = f32_max(*source.stroke_width, 0.0f);
    if (source.stroke_dasharray) target.stroke_dasharray = source.stroke_dasharray;
    if (source.line_color) target.line_color = source.line_color;
}

namespace {

NodeStyle resolve_node_style(const std::string& node_id, const Graph& graph) {
    NodeStyle style;
    if (auto it = graph.node_classes.find(node_id); it != graph.node_classes.end())
        for (const auto& cn : it->second)
            if (auto def = graph.class_defs.find(cn); def != graph.class_defs.end()) merge_node_style(style, def->second);
    if (auto it = graph.node_styles.find(node_id); it != graph.node_styles.end()) merge_node_style(style, it->second);
    return style;
}

// ======================================================================================
// analysis.rs (the parts manual_layout.rs reads)
// ======================================================================================

struct NodeRelation {
    size_t degree = 0;
    float incident_label_area = 0.0f;
    float extra_cross_padding(const LayoutConfig& config) const {
        if (degree <= 3) return 0.0f;
        float fanout_pad = f32_min(static_cast<float>(degree - 3) * config.node_spacing * 0.06f, config.node_spacing * 0.24f);
        float label_pad = f32_min(std::sqrt(incident_label_area) * 0.04f, config.node_spacing * 0.12f);
        return f32_min(f32_min(fanout_pad + label_pad, config.node_spacing * 0.32f), 24.0f);
    }
};

float edge_weight_formula(EdgeStyle style, bool is_cycle_edge, bool is_back_edge, bool crosses, bool has_center_label,
                          bool has_endpoint_label, float label_area, size_t label_chars) {
    float style_weight = style == EdgeStyle::Dotted ? 0.72f : (style == EdgeStyle::Thick ? 1.25f : 1.0f);
    float label_weight = f32_min(std::sqrt(label_area) / 110.0f, 0.9f) +
                         f32_min(static_cast<float>(label_chars) / 36.0f, 0.55f) + (has_center_label ? 0.22f : 0.0f) +
                         (has_endpoint_label ? 0.18f : 0.0f);
    float structure_weight = (crosses ? 0.35f : 0.0f) + (is_cycle_edge ? 0.18f : 0.0f) + (is_back_edge ? 0.16f : 0.0f);
    return clampf(style_weight + label_weight + structure_weight, 0.45f, 3.25f);
}

struct RelationshipAnalysis {
    std::unordered_map<std::string, NodeRelation> nodes;
    std::map<std::pair<std::string, std::string>, float> edge_pair_weight;
    bool disabled = false;  // analysis is None for non-flowchart diagrams

    float edge_weight_between(const std::string& from, const std::string& to) const {
        if (disabled) return 1.0f;
        auto it = edge_pair_weight.find({from, to});
        return it == edge_pair_weight.end() ? 1.0f : it->second;
    }
};

// (Rust returns None for every diagram kind but flowcharts: no weights, no extra padding.)
RelationshipAnalysis analyze(const Graph& graph, const std::vector<std::string>& node_ids,
                             const std::vector<Edge>& layout_edges,
                             const std::vector<std::optional<TextBlock>>& center_labels, const RankMap& ranks) {
    RelationshipAnalysis a;
    if (graph.kind != DiagramKind::Flowchart || node_ids.empty()) {
        a.disabled = true;
        return a;
    }
    auto memberships = node_subgraph_memberships(graph);
    std::unordered_set<std::string> node_set(node_ids.begin(), node_ids.end());
    auto comp = scc_component_ids(node_ids, layout_edges);
    for (const auto& id : node_ids) a.nodes[id] = NodeRelation{};

    for (size_t edge_idx = 0; edge_idx < layout_edges.size(); ++edge_idx) {
        const Edge& edge = layout_edges[edge_idx];
        if (!node_set.count(edge.from) || !node_set.count(edge.to)) continue;
        size_t from_rank = ranks.count(edge.from) ? ranks.at(edge.from) : 0;
        size_t to_rank = ranks.count(edge.to) ? ranks.at(edge.to) : 0;
        auto fc = comp.find(edge.from), tc = comp.find(edge.to);
        bool is_cycle_edge = edge.from == edge.to || (fc != comp.end() && tc != comp.end() && fc->second == tc->second);
        bool is_back_edge = to_rank <= from_rank;
        bool crosses = memberships_differ(memberships, edge.from, edge.to);
        auto nonblank = [](const std::optional<std::string>& s) { return s && !trim_view(*s).empty(); };
        bool has_center_label = nonblank(edge.label);
        bool has_endpoint_label = nonblank(edge.start_label) || nonblank(edge.end_label);
        float label_area = 0.0f;
        if (edge_idx < center_labels.size() && center_labels[edge_idx])
            label_area = center_labels[edge_idx]->width * center_labels[edge_idx]->height;
        size_t label_chars = (edge.label ? char_count(*edge.label) : 0) + (edge.start_label ? char_count(*edge.start_label) : 0) +
                             (edge.end_label ? char_count(*edge.end_label) : 0);
        float weight = edge_weight_formula(edge.style, is_cycle_edge, is_back_edge, crosses, has_center_label,
                                           has_endpoint_label, label_area, label_chars);
        if (auto it = a.nodes.find(edge.from); it != a.nodes.end()) {
            it->second.degree += 1;
            it->second.incident_label_area += label_area;
        }
        if (auto it = a.nodes.find(edge.to); it != a.nodes.end()) {
            it->second.degree += 1;
            it->second.incident_label_area += label_area;
        }
        auto key = std::make_pair(edge.from, edge.to);
        auto pit = a.edge_pair_weight.find(key);
        if (pit == a.edge_pair_weight.end()) a.edge_pair_weight[key] = weight;
        else pit->second = f32_max(pit->second, weight);
    }
    return a;
}

// ======================================================================================
// manual_layout.rs
// ======================================================================================

// Sorting floats the way Rust does with partial_cmp(..).unwrap_or(Equal).
std::optional<float> median_center(std::vector<float>& values) {
    if (values.empty()) return std::nullopt;
    std::stable_sort(values.begin(), values.end(), [](float a, float b) { return a < b; });
    size_t mid = values.size() / 2;
    if (values.size() % 2 == 1) return values[mid];
    return (values[mid - 1] + values[mid]) * 0.5f;
}

std::optional<float> weighted_median_center(std::vector<std::pair<float, float>>& values) {
    if (values.empty()) return std::nullopt;
    std::stable_sort(values.begin(), values.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    float total_weight = 0.0f;
    for (const auto& v : values) total_weight += f32_max(v.second, 0.0f);
    if (total_weight <= kF32Eps) return values[values.size() / 2].first;
    float threshold = total_weight * 0.5f;
    float cumulative = 0.0f;
    for (size_t idx = 0; idx < values.size(); ++idx) {
        auto [center, weight] = values[idx];
        cumulative += f32_max(weight, 0.0f);
        if (std::fabs(cumulative - threshold) <= kF32Eps) {
            std::optional<float> next_center;
            for (size_t j = idx + 1; j < values.size(); ++j)
                if (values[j].second > 0.0f) { next_center = values[j].first; break; }
            return next_center ? (center + *next_center) * 0.5f : center;
        }
        if (cumulative > threshold) return center;
    }
    return values.back().first;
}

float bounded_edge_aware_desired_center(float unweighted, std::optional<float> weighted, float current,
                                        float node_spacing) {
    if (!weighted) return unweighted;
    float max_pull = f32_max(node_spacing * EDGE_AWARE_CENTER_PULL_LIMIT_RATIO, 4.0f);
    float bounded_pull = clampf(*weighted - unweighted, -max_pull, max_pull);
    float relationship_target = unweighted + bounded_pull * EDGE_AWARE_CENTER_PULL_BLEND;
    return relationship_target * 0.85f + current * 0.15f;
}

std::unordered_map<size_t, float> flowchart_label_main_gap_budgets(const Graph& graph, const std::vector<Edge>& layout_edges,
                                                                   const std::vector<std::optional<TextBlock>>& edge_labels,
                                                                   const RankMap& shifted_ranks,
                                                                   const LayoutConfig& config) {
    std::unordered_map<size_t, float> gap_budgets;
    if (graph.kind != DiagramKind::Flowchart) return gap_budgets;
    bool horizontal = is_horizontal(graph.direction);
    auto [label_pad_x, label_pad_y] = edge_label_padding_flowchart();
    std::unordered_map<size_t, size_t> label_counts;
    for (size_t idx = 0; idx < layout_edges.size(); ++idx) {
        const Edge& edge = layout_edges[idx];
        if (idx >= edge_labels.size() || !edge_labels[idx]) continue;
        const TextBlock& label = *edge_labels[idx];
        if (label.width <= 0.0f || label.height <= 0.0f) continue;
        auto fr = shifted_ranks.find(edge.from);
        auto tr = shifted_ranks.find(edge.to);
        if (fr == shifted_ranks.end() || tr == shifted_ranks.end()) continue;
        size_t lo = std::min(fr->second, tr->second), hi = std::max(fr->second, tr->second);
        if (hi <= lo) continue;
        size_t gap_after_rank = lo + (hi - lo - 1) / 2;
        float label_main = horizontal ? label.width + 2.0f * label_pad_x : label.height + 2.0f * label_pad_y;
        float label_cross = horizontal ? label.height + 2.0f * label_pad_y : label.width + 2.0f * label_pad_x;
        size_t& stack_index = label_counts[gap_after_rank];
        float stacked_cross_allowance = static_cast<float>(stack_index) * label_cross * 0.35f;
        ++stack_index;
        float desired_gap = f32_max(f32_min(label_main + config.node_spacing * FLOWCHART_LABEL_MAIN_GAP_PAD_RATIO +
                                                stacked_cross_allowance,
                                            config.rank_spacing * FLOWCHART_LABEL_MAIN_GAP_MAX_SCALE),
                                    config.rank_spacing);
        float extra = f32_max(desired_gap - config.rank_spacing, 0.0f);
        auto it = gap_budgets.find(gap_after_rank);
        if (it == gap_budgets.end()) gap_budgets[gap_after_rank] = extra;
        else it->second = f32_max(it->second, extra);
    }
    return gap_budgets;
}

std::vector<Edge> build_ordering_edges(const std::vector<Edge>& layout_edges, const RankMap& shifted_ranks,
                                       std::vector<std::vector<std::string>>& rank_nodes, OrderMap& order_map,
                                       size_t& dummy_counter) {
    std::vector<Edge> ordering_edges;
    auto plain_edge = [](const std::string& from, const std::string& to) {
        Edge e;
        e.from = from;
        e.to = to;
        e.directed = true;
        return e;
    };
    for (const Edge& edge : layout_edges) {
        auto fr = shifted_ranks.find(edge.from);
        auto tr = shifted_ranks.find(edge.to);
        if (fr == shifted_ranks.end() || tr == shifted_ranks.end()) continue;
        size_t from_rank = fr->second, to_rank = tr->second;
        if (to_rank <= from_rank) { ordering_edges.push_back(edge); continue; }
        size_t span = to_rank - from_rank;
        if (span <= 1) { ordering_edges.push_back(edge); continue; }
        std::string prev = edge.from;
        for (size_t step = 1; step < span; ++step) {
            size_t current_rank = from_rank + step;
            std::string id = "__dummy_" + std::to_string(dummy_counter) + "__";
            ++dummy_counter;
            size_t order_idx = order_map.size();
            order_map[id] = order_idx;
            if (current_rank < rank_nodes.size()) rank_nodes[current_rank].push_back(id);
            ordering_edges.push_back(plain_edge(prev, id));
            prev = id;
        }
        ordering_edges.push_back(plain_edge(prev, edge.to));
    }
    return ordering_edges;
}

void assign_positions_manual(const Graph& graph, const std::vector<std::string>& layout_node_ids,
                             std::map<std::string, NodeLayout>& nodes, const LayoutConfig& config,
                             const std::vector<Edge>& layout_edges, const Theme& theme,
                             const std::vector<std::optional<TextBlock>>& edge_labels) {
    (void)theme;
    std::vector<Edge> rank_edges = rank_edges_for_manual_layout(graph, layout_node_ids, layout_edges);
    RankMap ranks = compute_ranks_subset(layout_node_ids, rank_edges, graph.node_order);
    RelationshipAnalysis analysis = analyze(graph, layout_node_ids, layout_edges, edge_labels, ranks);

    // Class diagrams: nodes that are not part of an inheritance hierarchy (open-triangle edges)
    // are pulled below the hierarchy's first rank, towards the average rank of their neighbours.
    if (graph.kind == DiagramKind::Class) {
        std::unordered_set<std::string> hierarchy_nodes;
        for (const Edge& edge : layout_edges) {
            bool has_open_triangle = edge.arrow_start_kind == EdgeArrowhead::OpenTriangle || edge.arrow_end_kind == EdgeArrowhead::OpenTriangle;
            if (has_open_triangle) {
                hierarchy_nodes.insert(edge.from);
                hierarchy_nodes.insert(edge.to);
            }
        }
        if (!hierarchy_nodes.empty()) {
            size_t min_hierarchy_rank = kMax;
            for (const auto& id : hierarchy_nodes)
                if (auto it = ranks.find(id); it != ranks.end()) min_hierarchy_rank = std::min(min_hierarchy_rank, it->second);
            if (min_hierarchy_rank == kMax) min_hierarchy_rank = 0;
            std::vector<std::pair<std::string, size_t>> pending_updates;
            for (const auto& node_id : layout_node_ids) {
                if (hierarchy_nodes.count(node_id)) continue;
                float sum = 0.0f;
                size_t count = 0;
                for (const Edge& edge : layout_edges) {
                    if (edge.from == node_id) {
                        if (auto r = ranks.find(edge.to); r != ranks.end()) { sum += static_cast<float>(r->second); ++count; }
                    } else if (edge.to == node_id) {
                        if (auto r = ranks.find(edge.from); r != ranks.end()) { sum += static_cast<float>(r->second); ++count; }
                    }
                }
                if (count == 0) continue;
                float avg = std::round(sum / static_cast<float>(count));
                size_t avg_neighbor_rank = avg > 0.0f ? static_cast<size_t>(avg) : 0;
                size_t target_rank = std::max(avg_neighbor_rank, min_hierarchy_rank + 1);
                auto cur = ranks.find(node_id);
                size_t current_rank = cur == ranks.end() ? 0 : cur->second;
                if (target_rank > current_rank) pending_updates.push_back({node_id, target_rank});
            }
            for (auto& [node_id, rank] : pending_updates) ranks[node_id] = rank;
        }
    }

    size_t max_rank = 0;
    for (const auto& [id, r] : ranks) max_rank = std::max(max_rank, r);
    std::vector<std::vector<std::string>> rank_nodes(max_rank + 1);
    for (const auto& id : layout_node_ids) {
        auto it = ranks.find(id);
        size_t rank = it == ranks.end() ? 0 : it->second;
        if (rank < rank_nodes.size()) rank_nodes[rank].push_back(id);
    }

    // Flowcharts never add label dummy ranks (use_label_dummies is false for them), so the rank
    // shift of the Rust code is all zeros and shifted ranks equal ranks.
    OrderMap order_map = graph.node_order;
    size_t dummy_counter = 0;
    const RankMap& shifted_ranks = ranks;
    auto gap_budgets = flowchart_label_main_gap_budgets(graph, layout_edges, edge_labels, shifted_ranks, config);
    std::vector<Edge> ordering_edges = build_ordering_edges(layout_edges, shifted_ranks, rank_nodes, order_map, dummy_counter);

    for (auto& bucket : rank_nodes)
        std::stable_sort(bucket.begin(), bucket.end(), [&](const std::string& a, const std::string& b) {
            auto ia = order_map.find(a), ib = order_map.find(b);
            return (ia == order_map.end() ? kMax : ia->second) < (ib == order_map.end() ? kMax : ib->second);
        });
    order_rank_nodes(rank_nodes, ordering_edges, order_map, config.flowchart.order_passes);

    bool horizontal = is_horizontal(graph.direction);

    // Main axis: ranks stacked with rank_spacing between them (plus label gap budgets).
    float main_cursor = 0.0f;
    for (size_t rank_idx = 0; rank_idx < rank_nodes.size(); ++rank_idx) {
        float max_main = 0.0f;
        for (const auto& node_id : rank_nodes[rank_idx]) {
            auto it = nodes.find(node_id);
            if (it == nodes.end()) continue;
            NodeLayout& n = it->second;
            if (horizontal) { n.x = main_cursor; max_main = f32_max(max_main, n.width); }
            else { n.y = main_cursor; max_main = f32_max(max_main, n.height); }
        }
        if (max_main > 0.0f) {
            float budget = 0.0f;
            if (auto b = gap_budgets.find(rank_idx); b != gap_budgets.end()) budget = b->second;
            float gap = config.rank_spacing + budget;
            main_cursor += max_main + gap;
        }
    }

    // Cross axis: iterate "place each rank at the median of its neighbours" in both directions.
    std::unordered_map<std::string, std::vector<std::string>> incoming, outgoing;
    std::unordered_map<std::string, std::vector<std::pair<std::string, float>>> incoming_weighted, outgoing_weighted;
    for (const Edge& edge : ordering_edges) {
        incoming[edge.to].push_back(edge.from);
        outgoing[edge.from].push_back(edge.to);
        float weight = analysis.edge_weight_between(edge.from, edge.to);
        incoming_weighted[edge.to].push_back({edge.from, weight});
        outgoing_weighted[edge.from].push_back({edge.to, weight});
    }

    std::unordered_map<std::string, float> cross_pos;
    for (const auto& bucket : rank_nodes)
        for (size_t idx = 0; idx < bucket.size(); ++idx) {
            auto it = nodes.find(bucket[idx]);
            if (it == nodes.end()) continue;
            const NodeLayout& n = it->second;
            float center = horizontal ? n.y + n.height / 2.0f : n.x + n.width / 2.0f;
            cross_pos[bucket[idx]] = center + static_cast<float>(idx) * 0.01f;
        }

    auto place_rank = [&](size_t rank_idx, bool use_incoming) {
        const auto& bucket = rank_nodes[rank_idx];
        if (bucket.empty()) return;
        const auto& neighbors = use_incoming ? incoming : outgoing;
        const auto& weighted_neighbors = use_incoming ? incoming_weighted : outgoing_weighted;
        struct Entry {
            std::string id;
            float desired, half;
            size_t idx;
        };
        std::vector<Entry> entries;
        for (size_t idx = 0; idx < bucket.size(); ++idx) {
            const std::string& node_id = bucket[idx];
            auto nit = nodes.find(node_id);
            if (nit == nodes.end()) continue;
            const NodeLayout& node = nit->second;
            std::vector<float> neighbor_centers;
            std::vector<std::pair<float, float>> weighted_centers;
            if (auto it = neighbors.find(node_id); it != neighbors.end())
                for (const auto& nb : it->second)
                    if (auto c = cross_pos.find(nb); c != cross_pos.end()) neighbor_centers.push_back(c->second);
            if (auto it = weighted_neighbors.find(node_id); it != weighted_neighbors.end())
                for (const auto& [nb, w] : it->second)
                    if (auto c = cross_pos.find(nb); c != cross_pos.end()) weighted_centers.push_back({c->second, w});
            auto self_pos = [&]() {
                auto c = cross_pos.find(node_id);
                return c == cross_pos.end() ? 0.0f : c->second;
            };
            float desired;
            if (neighbor_centers.empty()) desired = self_pos();
            else desired = median_center(neighbor_centers).value_or(self_pos());
            if (auto cur = cross_pos.find(node_id); cur != cross_pos.end()) {
                if (!neighbor_centers.empty()) {
                    auto weighted = weighted_median_center(weighted_centers);
                    desired = bounded_edge_aware_desired_center(desired, weighted, cur->second, config.node_spacing);
                } else {
                    desired = cur->second;
                }
            }
            float visual_half = horizontal ? node.height / 2.0f : node.width / 2.0f;
            float extra = 0.0f;
            if (auto rel = analysis.nodes.find(node_id); rel != analysis.nodes.end())
                extra = rel->second.extra_cross_padding(config);
            float half = visual_half + extra;
            entries.push_back({node_id, desired, half, idx});
        }
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            if (a.desired < b.desired) return true;
            if (a.desired > b.desired) return false;
            return a.idx < b.idx;
        });
        if (entries.empty()) return;  // (Rust would divide by zero here; cannot happen for real buckets)
        float desired_sum = 0.0f;
        for (const auto& e : entries) desired_sum += e.desired;
        float desired_mean = desired_sum / static_cast<float>(entries.size());
        struct Assigned {
            std::string id;
            float center, half;
        };
        std::vector<Assigned> assigned;
        bool have_prev = false;
        float prev_center = 0.0f, prev_half = 0.0f;
        for (const auto& e : entries) {
            float center;
            if (have_prev) {
                float min_center = prev_center + prev_half + e.half + config.node_spacing;
                center = e.desired < min_center ? min_center : e.desired;
            } else {
                center = e.desired;
            }
            assigned.push_back({e.id, center, e.half});
            have_prev = true;
            prev_center = center;
            prev_half = e.half;
        }
        float actual_sum = 0.0f;
        for (const auto& a : assigned) actual_sum += a.center;
        float actual_mean = actual_sum / static_cast<float>(assigned.size());
        float delta = desired_mean - actual_mean;
        for (const auto& a : assigned) {
            float center = a.center + delta;
            auto it = nodes.find(a.id);
            if (it != nodes.end()) {
                if (horizontal) it->second.y = center - it->second.height / 2.0f;
                else it->second.x = center - it->second.width / 2.0f;
            }
            cross_pos[a.id] = center;
        }
    };

    size_t passes = std::max<size_t>(config.flowchart.order_passes, 1);
    for (size_t p = 0; p < passes; ++p) {
        for (size_t r = 0; r < rank_nodes.size(); ++r) place_rank(r, true);
        for (size_t r = rank_nodes.size(); r-- > 0;) place_rank(r, false);
    }
}

// ======================================================================================
// objectives.rs: relax_edge_span_constraints (only edges with labels act; no subgraph groups)
// ======================================================================================

float node_main_center(const NodeLayout& n, bool horizontal) {
    return horizontal ? n.x + n.width * 0.5f : n.y + n.height * 0.5f;
}
float node_main_half(const NodeLayout& n, bool horizontal) { return horizontal ? n.width * 0.5f : n.height * 0.5f; }

void relax_edge_span_constraints(const Graph& graph, const std::vector<Edge>& layout_edges,
                                 std::map<std::string, NodeLayout>& nodes, const Theme& theme,
                                 const LayoutConfig& config) {
    if (layout_edges.empty()) return;
    switch (graph.kind) {
    case DiagramKind::Class: case DiagramKind::Flowchart: case DiagramKind::State: case DiagramKind::Er:
    case DiagramKind::Requirement: break;
    default: return;
    }
    bool horizontal = is_horizontal(graph.direction);
    const auto& objective = config.flowchart.objective;
    size_t passes = std::max<size_t>(objective.edge_relax_passes, 1);
    float step_limit = f32_max(config.rank_spacing + config.node_spacing, EDGE_RELAX_STEP_MIN);
    auto nonblank = [](const std::optional<std::string>& s) { return s && !trim_view(*s).empty(); };
    SubgraphGroups flowchart_groups = collect_flowchart_top_level_groups(graph);

    for (size_t pass = 0; pass < passes; ++pass) {
        bool changed = false;
        for (const Edge& edge : layout_edges) {
            auto fit = nodes.find(edge.from);
            auto tit = nodes.find(edge.to);
            if (fit == nodes.end() || tit == nodes.end()) continue;
            const NodeLayout& from_node = fit->second;
            const NodeLayout& to_node = tit->second;
            if (from_node.hidden || to_node.hidden) continue;
            float from_main = node_main_center(from_node, horizontal);
            float to_main = node_main_center(to_node, horizontal);
            float from_half = node_main_half(from_node, horizontal);
            float to_half = node_main_half(to_node, horizontal);
            float main_delta = to_main - from_main;
            float current_main_gap = main_delta >= 0.0f ? (to_main - to_half) - (from_main + from_half)
                                                        : (from_main - from_half) - (to_main + to_half);
            bool has_center_label = nonblank(edge.label);
            bool has_start_label = nonblank(edge.start_label);
            bool has_end_label = nonblank(edge.end_label);
            bool has_endpoint_label = has_start_label || has_end_label;
            if (graph.kind == DiagramKind::Flowchart && edge.style == EdgeStyle::Dotted) continue;
            if (!has_center_label && !has_endpoint_label) continue;

            float required = f32_max(config.node_spacing * objective.edge_gap_floor_ratio, 8.0f);
            auto extent_of = [&](const std::string& text) {
                TextBlock b = measure_label(text, theme, config);
                return horizontal ? b.width : b.height;
            };
            if (has_center_label) {
                required += extent_of(*edge.label) * objective.edge_label_weight;
                required += theme.font_size * EDGE_LABEL_PAD_SCALE;
            }
            if (has_start_label) {
                required += extent_of(*edge.start_label) * objective.endpoint_label_weight;
                required += theme.font_size * ENDPOINT_LABEL_PAD_SCALE;
            }
            if (has_end_label) {
                required += extent_of(*edge.end_label) * objective.endpoint_label_weight;
                required += theme.font_size * ENDPOINT_LABEL_PAD_SCALE;
            }
            if (has_start_label && has_end_label) required += theme.font_size * DUAL_ENDPOINT_EXTRA_PAD_SCALE;
            float max_main_gap = (config.rank_spacing + config.node_spacing) * MAX_MAIN_GAP_FACTOR;
            required = f32_min(required, max_main_gap);

            if (current_main_gap + EDGE_RELAX_GAP_TOLERANCE < required) {
                float delta = f32_min(required - current_main_gap, step_limit);
                // shift_cross_subgraph_flowchart_group: an edge between two top-level subgraphs
                // moves the whole destination subgraph instead of one node.
                if (flowchart_groups.valid) {
                    auto fg = flowchart_groups.node_to_group.find(edge.from);
                    auto tg = flowchart_groups.node_to_group.find(edge.to);
                    if (fg != flowchart_groups.node_to_group.end() && tg != flowchart_groups.node_to_group.end() &&
                        fg->second != tg->second) {
                        size_t ahead_group = main_delta >= 0.0f ? tg->second : fg->second;
                        auto gn = flowchart_groups.group_nodes.find(ahead_group);
                        if (gn != flowchart_groups.group_nodes.end()) {
                            for (const auto& id : gn->second) {
                                auto nit = nodes.find(id);
                                if (nit == nodes.end()) continue;
                                if (horizontal) nit->second.x += delta;
                                else nit->second.y += delta;
                            }
                            changed = true;
                            continue;
                        }
                    }
                }
                const std::string& ahead_id = main_delta >= 0.0f ? edge.to : edge.from;
                auto ait = nodes.find(ahead_id);
                if (ait != nodes.end()) {
                    if (horizontal) ait->second.x += delta;
                    else ait->second.y += delta;
                    changed = true;
                }
            }
        }
        if (!changed) break;
    }
}

bool has_visible_node_overlap(const std::map<std::string, NodeLayout>& nodes) {
    std::vector<const NodeLayout*> visible;
    for (const auto& [id, n] : nodes)
        if (!n.hidden) visible.push_back(&n);
    if (visible.size() < 2) return false;
    std::stable_sort(visible.begin(), visible.end(), [](const NodeLayout* a, const NodeLayout* b) { return a->x < b->x; });
    for (size_t i = 0; i < visible.size(); ++i) {
        const NodeLayout* a = visible[i];
        for (size_t j = i + 1; j < visible.size(); ++j) {
            const NodeLayout* b = visible[j];
            if (b->x >= a->x + a->width) break;
            float overlap_x = f32_min(a->x + a->width, b->x + b->width) - f32_max(a->x, b->x);
            float overlap_y = f32_min(a->y + a->height, b->y + b->height) - f32_max(a->y, b->y);
            if (overlap_x > 0.0f && overlap_y > 0.0f) return true;
        }
    }
    return false;
}

// objectives.rs: compact_large_flowchart_whitespace. Dense wide flowcharts without subgraphs get
// the empty gaps between nodes of the same cross-axis band squeezed out.
void compact_large_flowchart_whitespace(const Graph& graph, std::map<std::string, NodeLayout>& nodes,
                                        const LayoutConfig& config) {
    if (graph.kind != DiagramKind::Flowchart || !graph.subgraphs.empty()) return;
    std::vector<std::string> visible_ids;
    for (const auto& [id, node] : nodes)
        if (!node.hidden && !node.anchor_subgraph) visible_ids.push_back(node.id);
    size_t node_count = visible_ids.size();
    if (node_count < 16) return;
    size_t edge_count = 0;
    for (const Edge& e : graph.edges)
        if (nodes.count(e.from) && nodes.count(e.to)) ++edge_count;
    if (edge_count < 24) return;
    float density = static_cast<float>(edge_count) / static_cast<float>(node_count);
    if (density < 1.2f) return;

    bool horizontal = is_horizontal(graph.direction);
    float min_main = std::numeric_limits<float>::max(), max_main = std::numeric_limits<float>::lowest();
    float min_cross = std::numeric_limits<float>::max(), max_cross = std::numeric_limits<float>::lowest();
    for (const auto& [id, node] : nodes) {
        if (node.hidden || node.anchor_subgraph) continue;
        float main_start = horizontal ? node.x : node.y;
        float main_end = horizontal ? node.x + node.width : node.y + node.height;
        float cross_start = horizontal ? node.y : node.x;
        float cross_end = horizontal ? node.y + node.height : node.x + node.width;
        min_main = f32_min(min_main, main_start);
        max_main = f32_max(max_main, main_end);
        min_cross = f32_min(min_cross, cross_start);
        max_cross = f32_max(max_cross, cross_end);
    }
    if (min_main == std::numeric_limits<float>::max() || min_cross == std::numeric_limits<float>::max()) return;
    float main_span = f32_max(max_main - min_main, 1.0f);
    float cross_span = f32_max(max_cross - min_cross, 1.0f);
    float aspect = main_span / cross_span;
    if (aspect <= f32_max(config.flowchart.objective.max_aspect_ratio, 1.0f)) return;

    float band_size = f32_max(config.node_spacing * 1.25f, 24.0f);
    float desired_gap = f32_max(config.node_spacing * 0.72f, 10.0f);
    struct Entry {
        std::string id;
        float start, end;
    };
    std::unordered_map<int, std::vector<Entry>> bands;
    for (const auto& id : visible_ids) {
        auto it = nodes.find(id);
        if (it == nodes.end()) continue;
        const NodeLayout& node = it->second;
        float cross_center = horizontal ? node.y + node.height * 0.5f : node.x + node.width * 0.5f;
        float main_start = horizontal ? node.x : node.y;
        float main_end = horizontal ? node.x + node.width : node.y + node.height;
        int band = f2i(std::round(cross_center / band_size));
        bands[band].push_back({id, main_start, main_end});
    }
    if (bands.empty()) return;

    std::unordered_map<std::string, float> main_deltas;
    for (auto& [band, entries] : bands) {
        if (entries.size() < 2) continue;
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.start < b.start; });
        float cumulative_shift = 0.0f;
        std::optional<float> prev_end;
        for (const Entry& e : entries) {
            float adjusted_start = e.start - cumulative_shift;
            if (prev_end) {
                float gap = adjusted_start - *prev_end;
                if (gap > desired_gap) cumulative_shift += gap - desired_gap;
            }
            if (cumulative_shift > 0.0f) main_deltas[e.id] = -cumulative_shift;
            prev_end = e.end - cumulative_shift;
        }
    }
    if (main_deltas.empty()) return;
    for (const auto& [id, delta] : main_deltas) {
        auto it = nodes.find(id);
        if (it == nodes.end()) continue;
        if (horizontal) it->second.x += delta;
        else it->second.y += delta;
    }
}

// objectives.rs: resolve_node_overlaps (rounds of pushing overlapping nodes apart on the cross axis).
void resolve_node_overlaps(const Graph& graph, std::map<std::string, NodeLayout>& nodes, const LayoutConfig& config) {
    constexpr int OVERLAP_RESOLVE_PASSES = 6;
    constexpr float OVERLAP_MIN_GAP_RATIO = 0.2f, OVERLAP_MIN_GAP_FLOOR = 4.0f, OVERLAP_CENTER_THRESHOLD = 0.5f;
    bool horizontal = is_horizontal(graph.direction);
    float min_gap = f32_max(config.node_spacing * OVERLAP_MIN_GAP_RATIO, OVERLAP_MIN_GAP_FLOOR);
    SubgraphGroups flowchart_groups = collect_flowchart_top_level_groups(graph);
    std::vector<std::string> ids;
    for (const auto& [id, node] : nodes)
        if (!node.hidden) ids.push_back(node.id);
    if (ids.size() < 2) return;
    auto order_of = [&](const std::string& id) {
        auto it = graph.node_order.find(id);
        return it == graph.node_order.end() ? kMax : it->second;
    };
    std::stable_sort(ids.begin(), ids.end(), [&](const std::string& a, const std::string& b) { return order_of(a) < order_of(b); });

    for (int pass = 0; pass < OVERLAP_RESOLVE_PASSES; ++pass) {
        bool moved = false;
        for (size_t i = 0; i < ids.size(); ++i) {
            for (size_t j = i + 1; j < ids.size(); ++j) {
                const std::string& id_a = ids[i];
                const std::string& id_b = ids[j];
                auto ait = nodes.find(id_a);
                auto bit = nodes.find(id_b);
                if (ait == nodes.end() || bit == nodes.end()) continue;
                float ax = ait->second.x, ay = ait->second.y, aw = ait->second.width, ah = ait->second.height;
                float bx = bit->second.x, by = bit->second.y, bw = bit->second.width, bh = bit->second.height;
                float overlap_x = f32_min(ax + aw, bx + bw) - f32_max(ax, bx);
                float overlap_y = f32_min(ay + ah, by + bh) - f32_max(ay, by);
                if (overlap_x <= 0.0f || overlap_y <= 0.0f) continue;
                float center_a = horizontal ? ay + ah / 2.0f : ax + aw / 2.0f;
                float center_b = horizontal ? by + bh / 2.0f : bx + bw / 2.0f;
                float sign = center_b >= center_a ? 1.0f : -1.0f;
                if (std::fabs(center_b - center_a) < OVERLAP_CENTER_THRESHOLD)
                    sign = order_of(id_b) >= order_of(id_a) ? 1.0f : -1.0f;
                float delta = horizontal ? overlap_y + min_gap : overlap_x + min_gap;
                // shift_overlap_flowchart_group: overlapping nodes of two top-level subgraphs
                // move the whole target group.
                if (flowchart_groups.valid) {
                    auto sg = flowchart_groups.node_to_group.find(id_a);
                    auto tg = flowchart_groups.node_to_group.find(id_b);
                    if (sg != flowchart_groups.node_to_group.end() && tg != flowchart_groups.node_to_group.end() &&
                        sg->second != tg->second) {
                        auto gn = flowchart_groups.group_nodes.find(tg->second);
                        if (gn != flowchart_groups.group_nodes.end()) {
                            for (const auto& id : gn->second) {
                                auto nit = nodes.find(id);
                                if (nit == nodes.end()) continue;
                                if (horizontal) nit->second.y += sign * delta;
                                else nit->second.x += sign * delta;
                            }
                            moved = true;
                            continue;
                        }
                    }
                }
                if (horizontal) bit->second.y += sign * delta;
                else bit->second.x += sign * delta;
                moved = true;
            }
        }
        if (!moved) break;
    }
}

// ======================================================================================
// finalize / bounds
// ======================================================================================

std::pair<float, float> bounds_with_edges(const std::map<std::string, NodeLayout>& nodes,
                                          const std::vector<SubgraphLayout>& subgraphs,
                                          const std::vector<EdgeLayout>& edges) {
    float max_x = 0.0f, max_y = 0.0f;
    for (const auto& [id, n] : nodes) {
        max_x = f32_max(max_x, n.x + n.width);
        max_y = f32_max(max_y, n.y + n.height);
    }
    for (const auto& sub : subgraphs) {
        bool invisible_region = trim_view(sub.label).empty() && sub.style.stroke == std::optional<std::string>("none") &&
                                sub.style.fill == std::optional<std::string>("none");
        if (invisible_region) continue;
        max_x = f32_max(max_x, sub.x + sub.width);
        max_y = f32_max(max_y, sub.y + sub.height);
    }
    for (const auto& e : edges)
        for (const auto& p : e.points) {
            max_x = f32_max(max_x, p.first);
            max_y = f32_max(max_y, p.second);
        }
    return {max_x, max_y};
}

void apply_direction_mirror(Direction direction, std::map<std::string, NodeLayout>& nodes, std::vector<EdgeLayout>& edges,
                            std::vector<SubgraphLayout>& subgraphs) {
    auto [max_x, max_y] = bounds_with_edges(nodes, subgraphs, {});
    if (direction == Direction::RightLeft) {
        for (auto& sub : subgraphs) sub.x = max_x - sub.x - sub.width;
        for (auto& [id, n] : nodes) n.x = max_x - n.x - n.width;
        for (auto& e : edges) {
            for (auto& p : e.points) p.first = max_x - p.first;
            if (e.label_anchor) e.label_anchor->first = max_x - e.label_anchor->first;
        }
    }
    if (direction == Direction::BottomTop) {
        for (auto& sub : subgraphs) sub.y = max_y - sub.y - sub.height;
        for (auto& [id, n] : nodes) n.y = max_y - n.y - n.height;
        for (auto& e : edges) {
            for (auto& p : e.points) p.second = max_y - p.second;
            if (e.label_anchor) e.label_anchor->second = max_y - e.label_anchor->second;
        }
    }
}

void normalize_layout(std::map<std::string, NodeLayout>& nodes, std::vector<EdgeLayout>& edges,
                      std::vector<SubgraphLayout>& subgraphs) {
    float min_x = std::numeric_limits<float>::max(), min_y = std::numeric_limits<float>::max();
    for (const auto& [id, n] : nodes) {
        min_x = f32_min(min_x, n.x);
        min_y = f32_min(min_y, n.y);
    }
    for (const auto& sub : subgraphs) {
        min_x = f32_min(min_x, sub.x);
        min_y = f32_min(min_y, sub.y);
    }
    for (const auto& e : edges)
        for (const auto& p : e.points) {
            min_x = f32_min(min_x, p.first);
            min_y = f32_min(min_y, p.second);
        }
    if (!std::isfinite(min_x) || !std::isfinite(min_y)) return;
    float shift_x = kLayoutBoundaryPad - min_x;
    float shift_y = kLayoutBoundaryPad - min_y;
    if (std::fabs(shift_x) < 1e-3f && std::fabs(shift_y) < 1e-3f) return;
    for (auto& [id, n] : nodes) { n.x += shift_x; n.y += shift_y; }
    for (auto& sub : subgraphs) { sub.x += shift_x; sub.y += shift_y; }
    for (auto& e : edges) {
        for (auto& p : e.points) { p.first += shift_x; p.second += shift_y; }
        if (e.label_anchor) { e.label_anchor->first += shift_x; e.label_anchor->second += shift_y; }
    }
}

void finalize_graph_label_bounds(Layout& layout, const LayoutConfig& config) {
    float min_x = 0.0f, min_y = 0.0f, max_x = layout.width, max_y = layout.height;
    auto [label_pad_x, label_pad_y] = edge_label_padding(layout.kind, config);
    auto include_rect = [&](float x, float y, float w, float h) {
        min_x = f32_min(min_x, x);
        min_y = f32_min(min_y, y);
        max_x = f32_max(max_x, x + w);
        max_y = f32_max(max_y, y + h);
    };
    for (const auto& [id, n] : layout.nodes) include_rect(n.x, n.y, n.width, n.height);
    for (const auto& sub : layout.subgraphs) include_rect(sub.x, sub.y, sub.width, sub.height);
    for (const auto& e : layout.edges) {
        for (const auto& p : e.points) {
            min_x = f32_min(min_x, p.first);
            min_y = f32_min(min_y, p.second);
            max_x = f32_max(max_x, p.first);
            max_y = f32_max(max_y, p.second);
        }
        auto label_rect = [&](const std::optional<TextBlock>& label, const std::optional<Point>& anchor) {
            if (!label || !anchor) return;
            include_rect(anchor->first - label->width / 2.0f - label_pad_x, anchor->second - label->height / 2.0f - label_pad_y,
                         label->width + label_pad_x * 2.0f, label->height + label_pad_y * 2.0f);
        };
        label_rect(e.label, e.label_anchor);
        label_rect(e.start_label, e.start_label_anchor);
        label_rect(e.end_label, e.end_label_anchor);
    }
    for (const auto& note : layout.state_notes) include_rect(note.x, note.y, note.width, note.height);
    float dx = min_x < 0.0f ? -min_x + kLayoutBoundaryPad : 0.0f;
    float dy = min_y < 0.0f ? -min_y + kLayoutBoundaryPad : 0.0f;
    if (dx != 0.0f || dy != 0.0f) {
        for (auto& note : layout.state_notes) { note.x += dx; note.y += dy; }
        for (auto& [id, n] : layout.nodes) { n.x += dx; n.y += dy; }
        for (auto& sub : layout.subgraphs) { sub.x += dx; sub.y += dy; }
        for (auto& e : layout.edges) {
            for (auto& p : e.points) { p.first += dx; p.second += dy; }
            for (auto* a : {&e.label_anchor, &e.start_label_anchor, &e.end_label_anchor})
                if (*a) { (*a)->first += dx; (*a)->second += dy; }
        }
    }
    layout.width = f32_max(max_x + dx + kLayoutBoundaryPad, layout.width + dx);
    layout.height = f32_max(max_y + dy + kLayoutBoundaryPad, layout.height + dy);
}

}  // namespace

// layout/mod.rs build_graph_node_layouts: measure_label -> shape_size -> resolve_node_style -> NodeLayout.
std::map<std::string, NodeLayout> build_graph_node_layouts(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    std::map<std::string, NodeLayout> nodes;
    for (const auto& [id, node] : graph.nodes) {
        TextBlock label = measure_label(node.label, theme, config);
        auto [width, height] = shape_size(node.shape, label, config, theme, graph.kind);
        NodeLayout nl;
        nl.id = node.id;
        nl.width = width;
        nl.height = height;
        nl.label = std::move(label);
        nl.shape = node.shape;
        nl.style = resolve_node_style(node.id, graph);
        if (auto link = graph.node_links.find(node.id); link != graph.node_links.end()) nl.link = link->second;
        nodes.emplace(id, std::move(nl));
    }
    return nodes;
}

// Exports of the layout/mod.rs helpers the per-kind layouts use.
void normalize_layout_nodes(std::map<std::string, NodeLayout>& nodes, std::vector<EdgeLayout>& edges, std::vector<SubgraphLayout>& subgraphs) {
    normalize_layout(nodes, edges, subgraphs);
}
std::pair<float, float> bounds_without_padding(const std::map<std::string, NodeLayout>& nodes, const std::vector<SubgraphLayout>& subgraphs,
                                               const std::vector<EdgeLayout>& edges) {
    return bounds_with_edges(nodes, subgraphs, edges);
}

// Public entry for the per-kind layouts whose Rust layout is DiagramData::Graph (radar, mindmap, ...).
void finalize_graph_bounds(Layout& layout, const LayoutConfig& config) { finalize_graph_label_bounds(layout, config); }

std::pair<float, float> edge_label_padding(DiagramKind kind, const LayoutConfig& config) {
    switch (kind) {
    case DiagramKind::Requirement: return {config.requirement.edge_label_padding_x, config.requirement.edge_label_padding_y};
    case DiagramKind::State: return {3.0f, 1.6f};
    case DiagramKind::Flowchart: return {4.5f, 2.2f};
    default: return {4.0f, 2.0f};
    }
}

std::pair<float, float> layout_bounds_with_edges(const std::map<std::string, NodeLayout>& nodes, const std::vector<SubgraphLayout>& subgraphs,
                                                 const std::vector<EdgeLayout>& edges) {
    return bounds_with_edges(nodes, subgraphs, edges);
}

// Exported for sequence.cpp.
NodeStyle resolve_node_style_of(const std::string& node_id, const Graph& graph) { return resolve_node_style(node_id, graph); }

// ======================================================================================
// compute_layout / compute_flowchart_layout
// ======================================================================================

namespace {

// State diagrams: a visible start / end marker that landed on top of a real state is nudged clear of it
// (along the main axis, by the minimum needed). Mirrors separate_state_pseudostate_markers().
void separate_state_pseudostate_markers(const Graph& graph, std::map<std::string, NodeLayout>& nodes, const LayoutConfig& config) {
    auto is_marker = [](const std::string& id) {
        return (starts_with(id, "__start_") || starts_with(id, "__end_")) && ends_with(id, "__");
    };
    struct Rect {
        float x, y, w, h;
    };
    std::vector<Rect> obstacles;
    for (const auto& [id, n] : nodes)
        if (!n.hidden && !is_marker(n.id)) obstacles.push_back({n.x, n.y, n.width, n.height});
    if (obstacles.empty()) return;
    bool horizontal = is_horizontal(graph.direction);
    float gap = f32_max(config.node_spacing * 0.4f, 8.0f);
    std::vector<std::string> marker_ids;
    for (const auto& [id, n] : nodes)
        if (is_marker(id)) marker_ids.push_back(id);
    for (const auto& id : marker_ids) {
        auto it = nodes.find(id);
        if (it == nodes.end() || it->second.hidden) continue;
        float mx = it->second.x, my = it->second.y, mw = it->second.width, mh = it->second.height;
        for (const Rect& o : obstacles) {
            bool overlaps = mx < o.x + o.w && o.x < mx + mw && my < o.y + o.h && o.y < my + mh;
            if (!overlaps) continue;
            if (horizontal) {
                float new_x = o.x + o.w + gap;
                if (new_x > mx) mx = new_x;
            } else {
                float new_y = o.y + o.h + gap;
                if (new_y > my) my = new_y;
            }
        }
        it->second.x = mx;
        it->second.y = my;
    }
}

// State diagrams: nodes that belong to no subgraph are pushed below any composite box they overlap.
void push_non_members_out_of_subgraphs(const Graph& graph, std::map<std::string, NodeLayout>& nodes, const Theme& theme,
                                       const LayoutConfig& config) {
    if (graph.subgraphs.empty()) return;
    std::set<std::string> node_subgraphs, subgraph_ids;
    for (const auto& sub : graph.subgraphs)
        for (const auto& id : sub.nodes) node_subgraphs.insert(id);
    for (const auto& sub : graph.subgraphs) {
        if (sub.id) subgraph_ids.insert(*sub.id);
        if (!sub.label.empty()) subgraph_ids.insert(sub.label);
    }
    float gap = config.node_spacing * 0.5f;
    struct B {
        float x1, y1, x2, y2;
    };
    std::vector<B> sub_bounds;
    for (const auto& sub : graph.subgraphs) {
        float min_x = std::numeric_limits<float>::max(), min_y = min_x;
        float max_x = std::numeric_limits<float>::lowest(), max_y = max_x;
        for (const auto& id : sub.nodes)
            if (auto it = nodes.find(id); it != nodes.end()) {
                const NodeLayout& n = it->second;
                min_x = f32_min(min_x, n.x);
                min_y = f32_min(min_y, n.y);
                max_x = f32_max(max_x, n.x + n.width);
                max_y = f32_max(max_y, n.y + n.height);
            }
        auto [pad_x, pad_y, top_pad] = subgraph_padding_from_label(graph, sub, theme, measure_label(sub.label, theme, config));
        if (min_x < std::numeric_limits<float>::max()) sub_bounds.push_back({min_x - pad_x, min_y - top_pad, max_x + pad_x, max_y + pad_y});
        else sub_bounds.push_back({0.0f, 0.0f, 0.0f, 0.0f});
    }
    std::vector<std::string> node_ids;
    for (const auto& [id, n] : nodes) node_ids.push_back(id);
    for (const auto& node_id : node_ids) {
        if (node_subgraphs.count(node_id) || subgraph_ids.count(node_id)) continue;
        auto it = nodes.find(node_id);
        if (it == nodes.end()) continue;
        float nx = it->second.x, ny = it->second.y, nw = it->second.width, nh = it->second.height;
        for (const B& b : sub_bounds) {
            if (nx + nw > b.x1 && nx < b.x2 && ny + nh > b.y1 && ny < b.y2) {
                it->second.y = b.y2 + gap;
                break;
            }
        }
    }
}

}  // namespace

Layout compute_layout(const Graph& graph_in, const Theme& theme, const LayoutConfig& config, LayoutStage stage) {
    if (graph_in.kind == DiagramKind::Sequence || graph_in.kind == DiagramKind::ZenUML) {
        // normalize_graph_for_layout: messages and the participant list may mention undeclared nodes
        Graph graph = graph_in;
        for (const Edge& e : graph_in.edges) {
            graph.ensure_node(e.from, std::nullopt, std::nullopt);
            graph.ensure_node(e.to, std::nullopt, std::nullopt);
        }
        for (const auto& id : graph_in.sequence_participants) graph.ensure_node(id, std::nullopt, std::nullopt);
        Layout layout = compute_sequence_layout(graph, theme, config);
        if (stage == LayoutStage::NodesOnly) return layout;
        resolve_all_label_positions(layout, theme, config);
        finalize_sequence_layout_bounds(layout);
        return layout;
    }
    if (auto other = compute_other_layout(graph_in, theme, config)) {
        if (other->kind == DiagramKind::Architecture) {
            // graph-type layouts also get the final label pass and bounds finalisation
            resolve_all_label_positions(*other, theme, config);
            finalize_graph_label_bounds(*other, config);
        }
        return std::move(*other);
    }
    switch (graph_in.kind) {
    case DiagramKind::Flowchart: case DiagramKind::Class: case DiagramKind::Er: case DiagramKind::State: case DiagramKind::Packet: case DiagramKind::Requirement: break;
    default: throw Unsupported("layout of this diagram kind");
    }

    // normalize_graph_for_layout: edges may mention nodes that were never declared.
    Graph graph = graph_in;
    for (const Edge& e : graph_in.edges) {
        graph.ensure_node(e.from, std::nullopt, std::nullopt);
        graph.ensure_node(e.to, std::nullopt, std::nullopt);
    }

    LayoutConfig eff = apply_initial_config_heuristics(graph, config);
    bool tiny_graph = is_tiny_graph_layout(graph);
    const bool is_state = graph.kind == DiagramKind::State;

    std::map<std::string, NodeLayout> nodes;
    LayoutConfig label_config = eff;
    if (graph.kind == DiagramKind::Class) label_config.label_line_height = label_config.class_label_line_height();
    std::vector<std::string> state_marker_ids;
    float state_height_total = 0.0f;
    size_t state_height_count = 0;
    for (const auto& [id, node] : graph.nodes) {
        TextBlock label = measure_label_with_font_size(node.label, theme.font_size, label_config, true);
        bool label_empty = label.lines.size() == 1 && trim_view(label.lines[0]).empty();
        auto [width, height] = shape_size(node.shape, label, eff, theme, graph.kind);
        if (is_state && label_empty && (node.shape == NodeShape::Circle || node.shape == NodeShape::DoubleCircle)) {
            float size = f32_max(theme.font_size * STATE_MARKER_FONT_SCALE, STATE_MARKER_MIN_SIZE);
            width = size;
            height = size;
            state_marker_ids.push_back(node.id);
        } else if (is_state) {
            state_height_total += height;
            ++state_height_count;
        }
        NodeStyle style = resolve_node_style(node.id, graph);
        if (is_state && node.shape == NodeShape::ForkJoin && label_empty) {
            // fork / join bars in state diagrams are solid markers
            if (!style.fill) style.fill = theme.line_color;
            if (!style.stroke) style.stroke = theme.line_color;
            if (!style.stroke_width) style.stroke_width = 1.0f;
        }
        NodeLayout nl;
        nl.id = node.id;
        nl.width = width;
        nl.height = height;
        nl.label = std::move(label);
        nl.shape = node.shape;
        nl.style = style;
        if (auto link = graph.node_links.find(node.id); link != graph.node_links.end()) nl.link = link->second;
        nodes.emplace(id, std::move(nl));
    }
    if (is_state && !state_marker_ids.empty()) {
        float avg_height = state_height_count > 0 ? state_height_total / static_cast<float>(state_height_count) : theme.font_size * STATE_DEFAULT_HEIGHT_SCALE;
        float lo = theme.font_size * STATE_MARKER_MIN_SCALE, hi = theme.font_size * STATE_MARKER_MAX_SCALE;
        float marker_size = clampf(avg_height / STATE_MARKER_DIV, lo, hi);
        for (const auto& id : state_marker_ids)
            if (auto it = nodes.find(id); it != nodes.end()) {
                it->second.width = marker_size;
                it->second.height = marker_size;
            }
    }
    apply_measured_spacing_heuristics(graph, theme, eff, nodes);
    const LayoutConfig& cfg = eff;

    // Subgraphs that are edge endpoints get an "anchor" node: hidden, sized like the subgraph box.
    std::set<std::string> anchor_ids = mark_subgraph_anchor_nodes_hidden(graph, nodes);
    AnchorInfoMap anchor_info = apply_subgraph_anchor_sizes(graph, nodes, theme, cfg);
    std::set<std::string> anchored_subgraph_nodes;
    for (const auto& [aid, info] : anchor_info)
        if (info.sub_idx < graph.subgraphs.size())
            for (const auto& n : graph.subgraphs[info.sub_idx].nodes) anchored_subgraph_nodes.insert(n);
    std::set<size_t> anchored_indices;
    for (const auto& [aid, info] : anchor_info) anchored_indices.insert(info.sub_idx);
    std::map<std::string, std::string> edge_redirects;
    for (size_t idx = 0; idx < graph.subgraphs.size(); ++idx) {
        const Subgraph& sub = graph.subgraphs[idx];
        auto anchor_id = subgraph_anchor_id(sub, nodes);
        if (!anchor_id) continue;
        if (anchored_indices.count(idx)) continue;
        if (auto child = pick_subgraph_anchor_child(sub, graph, anchor_ids))
            if (*child != *anchor_id) edge_redirects[*anchor_id] = *child;
    }
    std::vector<Edge> layout_edges = graph.edges;
    for (Edge& e : layout_edges) {
        if (auto it = edge_redirects.find(e.from); it != edge_redirects.end()) e.from = it->second;
        if (auto it = edge_redirects.find(e.to); it != edge_redirects.end()) e.to = it->second;
    }

    std::vector<std::string> layout_node_ids;
    for (const auto& [id, node] : graph.nodes) layout_node_ids.push_back(id);
    std::stable_sort(layout_node_ids.begin(), layout_node_ids.end(), [&](const std::string& a, const std::string& b) {
        size_t oa = graph.node_order.count(a) ? graph.node_order.at(a) : kMax;
        size_t ob = graph.node_order.count(b) ? graph.node_order.at(b) : kMax;
        if (oa != ob) return oa < ob;
        return a < b;
    });
    auto drop_anchored = [&]() {
        if (anchored_subgraph_nodes.empty()) return;
        layout_node_ids.erase(std::remove_if(layout_node_ids.begin(), layout_node_ids.end(),
                                             [&](const std::string& id) { return anchored_subgraph_nodes.count(id) > 0; }),
                              layout_node_ids.end());
    };
    drop_anchored();
    if (anchor_info.empty()) {
        anchor_info = apply_subgraph_anchor_sizes(graph, nodes, theme, cfg);
        anchored_subgraph_nodes.clear();
        for (const auto& [aid, info] : anchor_info)
            if (info.sub_idx < graph.subgraphs.size())
                for (const auto& n : graph.subgraphs[info.sub_idx].nodes) anchored_subgraph_nodes.insert(n);
        drop_anchored();
    }
    std::set<std::string> layout_set(layout_node_ids.begin(), layout_node_ids.end());

    // Pre-measure edge labels once (layout, routing and edge construction share them).
    auto measure_edge_field = [&](const std::optional<std::string>& field) -> std::optional<TextBlock> {
        if (!field) return std::nullopt;
        std::string label_text = graph.kind == DiagramKind::Requirement ? requirement_edge_label_text(*field, cfg) : *field;
        if (graph.kind == DiagramKind::Flowchart && label_text.find('\n') == std::string::npos &&
            label_text.find("<br") == std::string::npos &&
            char_count(label_text) >= FLOWCHART_EDGE_LABEL_WRAP_TRIGGER_CHARS) {
            LayoutConfig wrap_cfg = cfg;
            wrap_cfg.max_label_width_chars = std::min(wrap_cfg.max_label_width_chars, FLOWCHART_EDGE_LABEL_WRAP_MAX_CHARS);
            return measure_label_with_font_size(label_text, f32_max(theme.font_size, 16.0f), wrap_cfg, true);
        }
        return measure_label(label_text, theme, cfg);
    };
    std::vector<std::optional<TextBlock>> edge_route_labels;
    std::vector<std::optional<TextBlock>> edge_start_labels, edge_end_labels;
    for (const auto& e : graph.edges) edge_route_labels.push_back(measure_edge_field(e.label));
    for (const auto& e : graph.edges) edge_start_labels.push_back(measure_edge_field(e.start_label));
    for (const auto& e : graph.edges) edge_end_labels.push_back(measure_edge_field(e.end_label));

    // assign_positions_manual sees only the edges whose endpoints are both laid out.
    {
        std::vector<Edge> kept_edges;
        std::vector<std::optional<TextBlock>> kept_labels;
        for (size_t i = 0; i < layout_edges.size(); ++i)
            if (layout_set.count(layout_edges[i].from) && layout_set.count(layout_edges[i].to)) {
                kept_edges.push_back(layout_edges[i]);
                kept_labels.push_back(edge_route_labels[i]);
            }
        assign_positions_manual(graph, layout_node_ids, nodes, cfg, kept_edges, theme, kept_labels);
    }

    // Passes that follow node placement (compute_flowchart_layout). The aspect-ratio fold is a
    // no-op (no preferred aspect ratio in the app).
    apply_subgraph_node_layout_passes(graph, nodes, cfg, anchored_indices, anchor_info);
    apply_flowchart_node_layout_cleanup(graph, nodes, theme, cfg);  // incl. align_disconnected_components
    if (cfg.flowchart.objective.enabled) {                          // apply_visual_objectives
        compact_large_flowchart_whitespace(graph, nodes, cfg);
        relax_edge_span_constraints(graph, layout_edges, nodes, theme, cfg);
        // rebalance_top_level_subgraphs_aspect only acts for 120+ nodes (flowcharts).
        if (graph.kind == DiagramKind::Flowchart && graph.subgraphs.size() >= 2 && graph.nodes.size() >= 120)
            throw Unsupported("rebalance_top_level_subgraphs_aspect");
        bool overlap_pass_enabled = graph.kind == DiagramKind::Class || has_visible_node_overlap(nodes);
        if (overlap_pass_enabled) resolve_node_overlaps(graph, nodes, cfg);
    }
    apply_subgraph_direction_overrides(graph, nodes, cfg, anchored_indices);
    // Objective passes can shift whole top-level groups: re-apply the non-overlap constraints
    // before the subgraph boxes are materialised.
    enforce_top_level_subgraph_gap(graph, nodes, theme, cfg);
    separate_sibling_subgraphs(graph, nodes, theme, cfg);
    evict_non_member_nodes_from_subgraphs(graph, nodes, theme, cfg);

    if (is_state && !graph.subgraphs.empty()) push_non_members_out_of_subgraphs(graph, nodes, theme, cfg);
    if (is_state) separate_state_pseudostate_markers(graph, nodes, cfg);

    std::vector<SubgraphLayout> subgraphs = build_subgraph_layouts(graph, nodes, theme, cfg);
    apply_subgraph_anchors(graph, subgraphs, nodes);

    Layout layout;
    layout.kind = graph.kind;
    if (stage == LayoutStage::NodesOnly) {
        // The mirror for RL / BT uses node bounds only, so it can be applied without routing.
        std::vector<EdgeLayout> no_edges;
        if (graph.direction == Direction::RightLeft || graph.direction == Direction::BottomTop)
            apply_direction_mirror(graph.direction, nodes, no_edges, subgraphs);
        layout.nodes = std::move(nodes);
        layout.subgraphs = std::move(subgraphs);
        return layout;
    }

    std::vector<EdgeLayout> edges =
        build_routed_edges(graph, nodes, subgraphs, cfg, layout_node_ids.size(), edge_route_labels, edge_start_labels, edge_end_labels, tiny_graph);

    // finalize_graph_layout
    if (graph.direction == Direction::RightLeft || graph.direction == Direction::BottomTop)
        apply_direction_mirror(graph.direction, nodes, edges, subgraphs);
    normalize_layout(nodes, edges, subgraphs);
    if (is_state && !graph.state_notes.empty()) {
        float note_pad_x = theme.font_size * 0.75f, note_pad_y = theme.font_size * 0.5f;
        float note_gap = f32_max(theme.font_size * 0.9f, 10.0f);
        for (const StateNote& note : graph.state_notes) {
            auto tit = nodes.find(note.target);
            if (tit == nodes.end()) continue;
            const NodeLayout& target = tit->second;
            StateNoteLayout nl;
            nl.label = measure_label(note.label, theme, cfg);
            nl.width = nl.label.width + note_pad_x * 2.0f;
            nl.height = nl.label.height + note_pad_y * 2.0f;
            nl.y = target.y + target.height / 2.0f - nl.height / 2.0f;
            nl.x = note.position == StateNotePosition::LeftOf ? target.x - note_gap - nl.width : target.x + target.width + note_gap;
            nl.position = note.position;
            nl.target = note.target;
            layout.state_notes.push_back(std::move(nl));
        }
    }
    auto [max_x, max_y] = bounds_with_edges(nodes, subgraphs, edges);
    for (const auto& note : layout.state_notes) {
        max_x = f32_max(max_x, note.x + note.width);
        max_y = f32_max(max_y, note.y + note.height);
    }
    layout.nodes = std::move(nodes);
    layout.edges = std::move(edges);
    layout.subgraphs = std::move(subgraphs);
    layout.width = max_x + kLayoutBoundaryPad;
    layout.height = max_y + kLayoutBoundaryPad;

    // label placement + final bounds
    resolve_all_label_positions(layout, theme, cfg);
    finalize_graph_label_bounds(layout, cfg);
    return layout;
}

}  // namespace mermaid
