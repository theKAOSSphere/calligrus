// Mermaid port: dispatch to the per-kind layouts and writers (see kinds.h).
#include "kinds.h"

namespace mermaid {

std::optional<Layout> compute_other_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    switch (graph.kind) {
    case DiagramKind::Pie: return compute_pie_layout(graph, theme, config);
    case DiagramKind::Gantt: return compute_gantt_layout(graph, theme, config);
    case DiagramKind::Journey: return compute_journey_layout(graph, theme, config);
    case DiagramKind::Timeline: return compute_timeline_layout(graph, theme, config);
    case DiagramKind::Quadrant: return compute_quadrant_layout(graph, theme, config);
    case DiagramKind::XYChart: return compute_xychart_layout(graph, theme, config);
    case DiagramKind::Radar: return compute_radar_layout(graph, theme, config);
    case DiagramKind::Kanban: return compute_kanban_layout(graph, theme, config);
    case DiagramKind::Treemap: return compute_treemap_layout(graph, theme, config);
    case DiagramKind::Sankey: return compute_sankey_layout(graph, theme, config);
    case DiagramKind::Block: return compute_block_layout(graph, theme, config);
    case DiagramKind::Architecture: return compute_architecture_layout(graph, theme, config);
    case DiagramKind::Mindmap: return compute_mindmap_layout(graph, theme, config);
    case DiagramKind::GitGraph: return compute_gitgraph_layout(graph, theme, config);
    case DiagramKind::C4: return compute_c4_layout(graph, theme, config);
    default: return std::nullopt;
    }
}

std::optional<std::string> render_other_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config) {
    switch (layout.kind) {
    case DiagramKind::Requirement: return render_requirement_svg(layout, theme, config);
    case DiagramKind::Pie: return render_pie_svg(layout, theme, config);
    case DiagramKind::Gantt: return render_gantt_svg(layout, theme, config);
    case DiagramKind::Journey: return render_journey_svg(layout, theme, config);
    case DiagramKind::Timeline: return render_timeline_svg(layout, theme, config);
    case DiagramKind::Quadrant: return render_quadrant_svg(layout, theme, config);
    case DiagramKind::XYChart: return render_xychart_svg(layout, theme, config);
    case DiagramKind::Radar: return render_radar_svg(layout, theme, config);
    case DiagramKind::Sankey: return render_sankey_svg(layout, theme, config);
    case DiagramKind::Architecture: return render_architecture_svg(layout, theme, config);
    case DiagramKind::GitGraph: return render_gitgraph_svg(layout, theme, config);
    case DiagramKind::C4: return render_c4_svg(layout, theme, config);
    default: return std::nullopt;
    }
}

}  // namespace mermaid
