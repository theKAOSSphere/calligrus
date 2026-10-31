// Mermaid port: kanban boards. Mirrors parser.rs parse_kanban_diagram and layout/kanban.rs. The writer is the generic
// graph writer (svg_writer.cpp): columns are subgraphs, cards are rectangle nodes.
#include <algorithm>
#include <set>

#include "kind_util.h"
#include "kinds.h"
#include "parser_internal.h"
#include "strutil.h"

namespace mermaid {

Graph parse_kanban_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::Kanban;
    graph.direction = Direction::LeftRight;
    std::optional<size_t> current_section;
    std::optional<size_t> base_indent;

    for (const std::string& raw_line : pp::preprocess_input_keep_indent(input)) {
        std::string trimmed = trim(raw_line);
        if (trimmed.empty()) continue;
        if (starts_with(to_ascii_lower(trimmed), "kanban")) continue;
        size_t indent = pp::count_indent(raw_line);
        if (!base_indent) base_indent = indent;
        if (indent <= *base_indent) {
            // a column header
            auto [id, label, shape, classes] = pp::parse_node_token(trimmed);
            Subgraph sub;
            sub.label = label ? *label : id;
            sub.id = id;
            graph.subgraphs.push_back(std::move(sub));
            current_section = graph.subgraphs.size() - 1;
            continue;
        }

        // a card, optionally followed by metadata: `task[Label]@{ assigned: x }`
        std::string task_part = trimmed;
        std::optional<std::string> meta;
        size_t at = trimmed.find("@{");
        if (at != std::string::npos) {
            std::string right = trimmed.substr(at + 2);
            while (!right.empty() && right.back() == '}') right.pop_back();  // trim_end_matches('}')
            meta = trim(right);
            task_part = trim(trimmed.substr(0, at));
        }
        auto [id_in, label, shape, classes] = pp::parse_node_token(task_part);
        std::string id = id_in;
        if (graph.nodes.count(id)) id = id + "_" + std::to_string(graph.nodes.size());
        std::string node_label = label ? *label : id;
        if (meta && !meta->empty()) node_label += "\n" + *meta;
        graph.ensure_node(id, node_label, NodeShape::Rectangle);
        if (current_section) graph.subgraphs[*current_section].nodes.push_back(id);
    }
    return graph;
}

Layout compute_kanban_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    // With edges the crate lays the board out like a flowchart; that path is not ported.
    if (!graph.edges.empty()) throw Unsupported("kanban with edges");

    std::map<std::string, NodeLayout> nodes = build_graph_node_layouts(graph, theme, config);

    float node_gap = rmax(theme.font_size * 0.45f, 4.0f);
    float column_gap = rmax(theme.font_size * 0.3f, 3.0f);
    float origin_x = 6.0f, origin_y = 6.0f;
    float column_x = origin_x;
    std::set<std::string> assigned;

    for (const Subgraph& sub : graph.subgraphs) {
        std::vector<std::string> column_nodes;
        for (const std::string& id : sub.nodes)
            if (nodes.count(id)) column_nodes.push_back(id);
        if (column_nodes.empty()) continue;
        assigned.insert(column_nodes.begin(), column_nodes.end());

        bool label_empty = trim_view(sub.label).empty();
        TextBlock label_block = measure_label(sub.label, theme, config);
        if (label_empty) {
            label_block.width = 0.0f;
            label_block.height = 0.0f;
        }
        auto [pad_x, pad_y, top_padding] = subgraph_padding_from_label(graph, sub, theme, label_block);
        (void)pad_y;

        float max_node_width = 0.0f;
        for (const std::string& id : column_nodes) max_node_width = rmax(max_node_width, nodes.at(id).width);
        float inner_width = rmax(max_node_width, label_block.width);
        float column_width = inner_width + pad_x * 2.0f;

        float y_cursor = origin_y + top_padding;
        size_t last_idx = column_nodes.size() - 1;
        for (size_t idx = 0; idx < column_nodes.size(); ++idx) {
            NodeLayout& node = nodes.at(column_nodes[idx]);
            node.x = column_x + pad_x + (inner_width - node.width) / 2.0f;
            node.y = y_cursor;
            y_cursor += node.height;
            if (idx < last_idx) y_cursor += node_gap;
        }
        column_x += column_width + column_gap;
    }

    // cards that belong to no column go in a row after the columns
    float free_x = column_x;
    for (auto& [id, node] : nodes) {
        if (assigned.count(node.id)) continue;
        node.x = free_x;
        node.y = origin_y;
        free_x += node.width + column_gap;
    }

    std::vector<EdgeLayout> edges;
    std::vector<SubgraphLayout> subgraphs = build_subgraph_layouts(graph, nodes, theme, config);
    normalize_layout_nodes(nodes, edges, subgraphs);
    auto [max_x, max_y] = bounds_without_padding(nodes, subgraphs);

    Layout layout;
    layout.kind = graph.kind;
    layout.nodes = std::move(nodes);
    layout.edges = std::move(edges);
    layout.subgraphs = std::move(subgraphs);
    layout.width = max_x + 6.0f;
    layout.height = max_y + 6.0f;
    // compute_layout's final pass for graph-data layouts (no edge labels here)
    finalize_graph_bounds(layout, config);
    return layout;
}

}  // namespace mermaid
