// Mermaid port: the diagram kinds that have their own parser / layout / writer (everything except the
// flowchart-layout family and sequence diagrams). One source file per kind (kind_<name>.cpp); the
// dispatchers below are defined in kinds.cpp.
#pragma once

#include <optional>
#include <string>

#include "config.h"
#include "ir.h"
#include "layout_types.h"

namespace mermaid {

// Parsers (each takes the whole source; see parser.rs parse_<kind>_diagram).
Graph parse_pie_diagram(const std::string& input);
Graph parse_gantt_diagram(const std::string& input);
Graph parse_journey_diagram(const std::string& input);
Graph parse_timeline_diagram(const std::string& input);

// compute_layout() for these kinds. Returns nullopt when `graph.kind` is not one of them.
std::optional<Layout> compute_other_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);
// render_svg() for these kinds (the complete document). nullopt when not one of them.
std::optional<std::string> render_other_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config);

Graph parse_quadrant_diagram(const std::string& input);

Graph parse_xychart_diagram(const std::string& input);

Graph parse_radar_diagram(const std::string& input);

Graph parse_kanban_diagram(const std::string& input);

Graph parse_packet_diagram(const std::string& input);

Graph parse_treemap_diagram(const std::string& input);

Graph parse_requirement_diagram(const std::string& input);
// requirement_edge_label_text(): <<relation>> text of a requirement edge label.
std::string requirement_edge_label_text(const std::string& label, const LayoutConfig& config);
std::string render_requirement_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config);

Graph parse_sankey_diagram(const std::string& input);

Graph parse_zenuml_diagram(const std::string& input);

Graph parse_block_diagram(const std::string& input);

Graph parse_architecture_diagram(const std::string& input);

Graph parse_mindmap_diagram(const std::string& input);

Graph parse_gitgraph_diagram(const std::string& input);

Graph parse_c4_diagram(const std::string& input);

// per-kind layout / writer entry points
Layout compute_pie_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);
std::string render_pie_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config);
Layout compute_gantt_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);
std::string render_gantt_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config);
Layout compute_journey_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);
std::string render_journey_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config);
Layout compute_timeline_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);
std::string render_timeline_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config);

Layout compute_quadrant_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);
std::string render_quadrant_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config);

Layout compute_xychart_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);
std::string render_xychart_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config);

Layout compute_radar_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);
std::string render_radar_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config);

Layout compute_kanban_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);

Layout compute_treemap_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);

Layout compute_sankey_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);
std::string render_sankey_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config);

Layout compute_block_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);

Layout compute_architecture_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);
std::string render_architecture_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config);

Layout compute_mindmap_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);

Layout compute_gitgraph_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);
std::string render_gitgraph_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config);

Layout compute_c4_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config);
std::string render_c4_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config);

}  // namespace mermaid
