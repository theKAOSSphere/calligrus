// Mermaid port of layout/ranking.rs: layering (ranks) and crossing-reducing order of nodes.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ir.h"

namespace mermaid {

using RankMap = std::unordered_map<std::string, size_t>;
using OrderMap = std::unordered_map<std::string, size_t>;

// Edges used for ranking: dotted edges are dropped when the solid ones cover enough nodes.
std::vector<Edge> rank_edges_for_manual_layout(const Graph& graph, const std::vector<std::string>& layout_node_ids,
                                               const std::vector<Edge>& layout_edges);

// Rank (layer index) of every node: strongly connected components are collapsed, then nodes
// are layered along a stable topological order.
RankMap compute_ranks_subset(const std::vector<std::string>& node_ids, const std::vector<Edge>& edges,
                             const OrderMap& node_order);

// Median heuristic + transpose sweeps to reduce edge crossings; reorders each bucket in place.
void order_rank_nodes(std::vector<std::vector<std::string>>& rank_nodes, const std::vector<Edge>& edges,
                      const OrderMap& node_order, size_t passes);

// Component index of every node (nodes in one strongly connected component share an index).
std::unordered_map<std::string, size_t> scc_component_ids(const std::vector<std::string>& node_ids,
                                                          const std::vector<Edge>& edges);

}  // namespace mermaid
