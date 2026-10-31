// Mermaid port: declarations shared between the layout source files (not a public API).
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "ir.h"
#include "layout_types.h"

namespace mermaid {

constexpr float kLayoutBoundaryPad = 8.0f;  // LAYOUT_BOUNDARY_PAD in layout/mod.rs

// Edge routing (flowchart/edge_pipeline.rs + routing.rs): returns one EdgeLayout per graph edge.
// `edge_route_labels[i]` is the measured centre label of edge i (nullopt if none).
std::vector<EdgeLayout> build_routed_edges(const Graph& graph, const std::map<std::string, NodeLayout>& nodes,
                                           const std::vector<SubgraphLayout>& subgraphs,
                                           const LayoutConfig& config, size_t layout_node_count,
                                           const std::vector<std::optional<TextBlock>>& edge_route_labels,
                                           const std::vector<std::optional<TextBlock>>& edge_start_labels,
                                           const std::vector<std::optional<TextBlock>>& edge_end_labels, bool tiny_graph);

// ---- subgraphs.cpp (layout/subgraphs.rs + flowchart/subgraph_spacing.rs) ----

void merge_node_style(NodeStyle& target, const NodeStyle& source);  // layout.cpp
bool is_region_subgraph(const Subgraph& sub);
std::vector<size_t> top_level_subgraph_indices(const Graph& graph);
std::optional<std::string> subgraph_anchor_id(const Subgraph& sub, const std::map<std::string, NodeLayout>& nodes);
std::tuple<float, float, float> subgraph_padding_from_label(const Graph& graph, const Subgraph& sub, const Theme& theme,
                                                            const TextBlock& label_block);
std::vector<SubgraphLayout> build_subgraph_layouts(const Graph& graph, const std::map<std::string, NodeLayout>& nodes,
                                                   const Theme& theme, const LayoutConfig& config);
// Index lists of the subgraphs each node belongs to (nodes in none are absent).
std::map<std::string, std::vector<size_t>> node_subgraph_memberships(const Graph& graph);
// `from_subgraphs != to_subgraphs` of the Rust code (an absent entry differs from a present one).
bool memberships_differ(const std::map<std::string, std::vector<size_t>>& m, const std::string& a, const std::string& b);
void apply_flowchart_node_layout_cleanup(const Graph& graph, std::map<std::string, NodeLayout>& nodes,
                                         const Theme& theme, const LayoutConfig& config);
void enforce_top_level_subgraph_gap(const Graph& graph, std::map<std::string, NodeLayout>& nodes, const Theme& theme,
                                    const LayoutConfig& config);
void separate_sibling_subgraphs(const Graph& graph, std::map<std::string, NodeLayout>& nodes, const Theme& theme,
                                const LayoutConfig& config);
void apply_subgraph_direction_overrides(const Graph& graph, std::map<std::string, NodeLayout>& nodes, const LayoutConfig& config,
                                        const std::set<size_t>& skip_indices = {});

// ---- subgraph anchors (a subgraph used as an edge endpoint; subgraphs.cpp) ----
struct SubgraphAnchorInfo {
    size_t sub_idx = 0;
    float padding_x = 0, top_padding = 0;
};
using AnchorInfoMap = std::map<std::string, SubgraphAnchorInfo>;
std::set<std::string> mark_subgraph_anchor_nodes_hidden(const Graph& graph, std::map<std::string, NodeLayout>& nodes);
std::optional<std::string> pick_subgraph_anchor_child(const Subgraph& sub, const Graph& graph, const std::set<std::string>& anchor_ids);
AnchorInfoMap apply_subgraph_anchor_sizes(const Graph& graph, std::map<std::string, NodeLayout>& nodes, const Theme& theme,
                                          const LayoutConfig& config);
void apply_subgraph_node_layout_passes(const Graph& graph, std::map<std::string, NodeLayout>& nodes, const LayoutConfig& config,
                                       const std::set<size_t>& anchored_indices, const AnchorInfoMap& anchor_info);
void apply_subgraph_anchors(const Graph& graph, const std::vector<SubgraphLayout>& subgraphs, std::map<std::string, NodeLayout>& nodes);
NodeLayout anchor_layout_for_edge_towards(const NodeLayout& anchor, const SubgraphLayout& subgraph, const NodeLayout& remote,
                                          Direction direction, bool is_from);
std::optional<std::pair<NodeLayout, NodeLayout>> effective_edge_endpoint_layouts(const Graph& graph,
                                                                                 const std::map<std::string, NodeLayout>& nodes,
                                                                                 const std::vector<SubgraphLayout>& subgraphs, const Edge& edge);
void evict_non_member_nodes_from_subgraphs(const Graph& graph, std::map<std::string, NodeLayout>& nodes,
                                           const Theme& theme, const LayoutConfig& config);

// objectives.rs FlowchartTopLevelGroups: top-level subgraphs that move as one block.
struct SubgraphGroups {
    bool valid = false;
    std::unordered_map<std::string, size_t> node_to_group;
    std::unordered_map<size_t, std::vector<std::string>> group_nodes;
};
SubgraphGroups collect_flowchart_top_level_groups(const Graph& graph);

// ---- sequence.cpp (layout/sequence.rs) ----
EdgeStyleOverride resolve_edge_style_of(size_t idx, const Graph& graph);  // edge_pipeline.cpp
NodeStyle resolve_node_style_of(const std::string& node_id, const Graph& graph);  // layout.cpp
NodeStyle resolve_subgraph_style(const Subgraph& sub, const Graph& graph);        // subgraphs.cpp
std::pair<float, float> layout_bounds_with_edges(const std::map<std::string, NodeLayout>& nodes, const std::vector<SubgraphLayout>& subgraphs,
                                                 const std::vector<EdgeLayout>& edges);  // layout.cpp
Layout compute_sequence_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);
void resolve_sequence_label_positions(Layout& layout, const Theme& theme);
void finalize_sequence_layout_bounds(Layout& layout);

// label_placement.rs entry point (edge label anchors).
void resolve_all_label_positions(Layout& layout, const Theme& theme, const LayoutConfig& config);

// layout/mod.rs build_graph_node_layouts / normalize_layout / bounds_without_padding (layout.cpp).
std::map<std::string, NodeLayout> build_graph_node_layouts(const Graph& graph, const Theme& theme, const LayoutConfig& config);
void normalize_layout_nodes(std::map<std::string, NodeLayout>& nodes, std::vector<EdgeLayout>& edges, std::vector<SubgraphLayout>& subgraphs);
// (with edges given this is layout/mod.rs bounds_with_edges)
std::pair<float, float> bounds_without_padding(const std::map<std::string, NodeLayout>& nodes, const std::vector<SubgraphLayout>& subgraphs,
                                               const std::vector<EdgeLayout>& edges = {});

// layout/mod.rs finalize_graph_label_bounds (grow / shift the layout to contain nodes, edges and labels).
void finalize_graph_bounds(Layout& layout, const LayoutConfig& config);

// label_placement::edge_label_padding (centre label padding per diagram kind).
std::pair<float, float> edge_label_padding(DiagramKind kind, const LayoutConfig& config);
// (same value for flowcharts)
inline std::pair<float, float> edge_label_padding_flowchart() { return {4.5f, 2.2f}; }

}  // namespace mermaid
