// Mermaid port of layout/flowchart/path_cleanup.rs: post-processing of routed edge paths
// (crossing reduction, overlap removal, detours around nodes, endpoint repairs).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "config.h"
#include "ir.h"
#include "routing.h"

namespace mermaid {

using Paths = std::vector<std::vector<Point>>;

void reduce_orthogonal_path_crossings(const Graph& graph, const std::map<std::string, NodeLayout>& nodes, Paths& routed,
                                      const LayoutConfig& config);
void deoverlap_flowchart_paths(const Graph& graph, const std::map<std::string, NodeLayout>& nodes, Paths& routed,
                               const LayoutConfig& config);
void simplify_flowchart_detour_rectangles(const Graph& graph, const std::map<std::string, NodeLayout>& nodes, Paths& routed);
void simplify_flowchart_axis_oscillations(Paths& routed);
void detour_flowchart_paths_around_non_endpoint_nodes(const Graph& graph, const std::map<std::string, NodeLayout>& nodes,
                                                      Paths& routed, const LayoutConfig& config);
void detour_flowchart_paths_around_foreign_subgraphs(const Graph& graph, const std::map<std::string, NodeLayout>& nodes,
                                                     const std::vector<SubgraphLayout>& subgraphs, Paths& routed,
                                                     const LayoutConfig& config);
void repair_flowchart_endpoint_reentries(const Graph& graph, const std::map<std::string, NodeLayout>& nodes, Paths& routed,
                                         const LayoutConfig& config);

bool flowchart_path_hits_non_endpoint_nodes(const std::vector<Point>& path, const std::string& from_id,
                                            const std::string& to_id, const std::map<std::string, NodeLayout>& nodes);
size_t flowchart_endpoint_reentry_count(const std::vector<Point>& points, const Edge& edge,
                                        const std::map<std::string, NodeLayout>& nodes);
size_t flowchart_endpoint_direction_violation_count(const std::vector<Point>& points, const Edge& edge,
                                                    const std::map<std::string, NodeLayout>& nodes);

}  // namespace mermaid
