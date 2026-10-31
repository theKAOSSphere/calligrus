// Mermaid port: block diagrams. Mirrors parser.rs parse_block_diagram and layout/block.rs. The writer is the generic
// graph writer (svg_writer.cpp): blocks are plain nodes laid out on a grid, edges are straight centre-to-centre lines.
#include <cstdint>
#include <algorithm>
#include <map>
#include <unordered_map>

#include "kind_util.h"
#include "kinds.h"
#include "parser_internal.h"
#include "strutil.h"

namespace mermaid {

namespace {

// Rust `str::parse::<usize>()`: digits only (an optional leading '+').
bool parse_usize(const std::string& s, size_t& out) {
    size_t i = (!s.empty() && s[0] == '+') ? 1 : 0;
    if (i >= s.size()) return false;
    size_t value = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        size_t next = value * 10 + static_cast<size_t>(s[i] - '0');
        if (next / 10 != value && value != 0) return false;  // overflow
        value = next;
    }
    out = value;
    return true;
}

// `id:3` -> (id, 3) when the suffix after the last ':' is a positive integer.
std::string split_span(const std::string& token, size_t& span) {
    size_t colon = token.rfind(':');
    if (colon != std::string::npos) {
        size_t parsed;
        if (parse_usize(token.substr(colon + 1), parsed) && parsed > 0) {
            span = parsed;
            return token.substr(0, colon);
        }
    }
    return token;
}

void add_block_node(Graph& graph, BlockDiagram& block, const std::string& token, size_t span) {
    auto [id, label, shape, classes] = pp::parse_node_token(token);
    if (id.empty()) return;
    graph.ensure_node(id, label, shape);
    pp::apply_node_classes(graph, id, classes);
    block.nodes.push_back({id, span, false});
}

}  // namespace

Graph parse_block_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::Block;
    graph.direction = Direction::LeftRight;
    BlockDiagram block;
    // Depth of nested `block:id ... end` groups currently being skipped. The group node itself represents the
    // section; its inner nodes are not laid out individually.
    size_t group_depth = 0;

    for (const std::string& raw_line : pp::preprocess_input(input)) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        std::string lower = to_ascii_lower(line);
        if (group_depth > 0) {
            // inside a named group: only track nesting until the matching `end`
            if (starts_with(lower, "block:")) ++group_depth;
            else if (lower == "end" || starts_with(lower, "end ") || starts_with(lower, "end\t")) --group_depth;
            continue;
        }
        if (starts_with(line, "block:")) {
            // named nested group: `block:id["Label"]` (optionally with a `:span` suffix)
            size_t span = 1;
            std::string token = split_span(trim(line.substr(6)), span);
            auto [id, label, shape, classes] = pp::parse_node_token(token);
            if (!id.empty()) {
                graph.ensure_node(id, label, shape);
                pp::apply_node_classes(graph, id, classes);
                block.nodes.push_back({id, span, false});
            }
            group_depth = 1;
            continue;
        }
        if (starts_with(lower, "block")) continue;  // diagram header (`block-beta` / `block`)
        if (starts_with(lower, "classdef ") || starts_with(lower, "class ") || starts_with(lower, "style ") || starts_with(lower, "linkstyle "))
            continue;
        if (starts_with(lower, "columns")) {
            auto parts = split_whitespace(line);
            size_t cols;
            if (parts.size() >= 2 && parse_usize(parts[1], cols) && cols > 0) block.columns = cols;
            continue;
        }
        if (lower == "end") continue;
        if (pp::add_flowchart_edge(line, graph, {})) continue;

        for (const std::string& raw : split_whitespace(line)) {
            std::string token = trim(raw);
            if (token.empty()) continue;
            size_t span = 1;
            token = split_span(token, span);
            if (to_ascii_lower(token) == "space") {
                block.nodes.push_back({"__space", span, true});
                continue;
            }
            add_block_node(graph, block, token, span);
        }
    }
    // Keep block metadata even when the DSL only contains edge lines: layout infers a grid from the topology then.
    graph.block = std::move(block);
    return graph;
}

namespace {

// infer_block_grid: rows = longest-path rank of each node, in declaration order.
std::pair<std::vector<BlockNode>, size_t> infer_block_grid(const Graph& graph) {
    auto order_of = [&](const std::string& id) {
        auto it = graph.node_order.find(id);
        return it == graph.node_order.end() ? SIZE_MAX : it->second;
    };
    auto by_order = [&](const std::string& a, const std::string& b) {
        size_t ao = order_of(a), bo = order_of(b);
        if (ao != bo) return ao < bo;
        return a < b;
    };
    std::vector<std::string> ids;
    for (const auto& [id, node] : graph.nodes) ids.push_back(id);
    std::stable_sort(ids.begin(), ids.end(), by_order);
    if (ids.empty()) return {{}, 1};

    std::unordered_map<std::string, size_t> indegree;
    for (const auto& id : ids) indegree[id] = 0;
    std::unordered_map<std::string, std::vector<std::string>> outgoing;
    for (const Edge& edge : graph.edges) {
        if (edge.from == edge.to) continue;
        if (!indegree.count(edge.from) || !indegree.count(edge.to)) continue;
        outgoing[edge.from].push_back(edge.to);
        indegree[edge.to] += 1;
    }
    for (auto& [id, children] : outgoing) {
        std::stable_sort(children.begin(), children.end(), by_order);
        children.erase(std::unique(children.begin(), children.end()), children.end());  // dedup
    }

    std::vector<std::string> queue;
    for (const auto& id : ids)
        if (indegree[id] == 0) queue.push_back(id);
    std::unordered_map<std::string, size_t> rank;
    for (size_t head = 0; head < queue.size(); ++head) {
        std::string id = queue[head];
        size_t base_rank = rank.count(id) ? rank[id] : 0;
        auto it = outgoing.find(id);
        if (it == outgoing.end()) continue;
        for (const std::string& child : it->second) {
            auto r = rank.find(child);
            if (r == rank.end()) rank[child] = base_rank + 1;
            else r->second = std::max(r->second, base_rank + 1);
            size_t& value = indegree[child];
            value = value > 0 ? value - 1 : 0;
            if (value == 0) queue.push_back(child);
        }
    }

    if (rank.size() < ids.size()) {
        // nodes on a cycle: one below their deepest ranked parent
        for (const auto& id : ids) {
            if (rank.count(id)) continue;
            std::optional<size_t> inferred_rank;
            for (const Edge& edge : graph.edges) {
                if (edge.to != id) continue;
                auto parent = rank.find(edge.from);
                if (parent != rank.end())
                    inferred_rank = inferred_rank ? std::max(*inferred_rank, parent->second + 1) : parent->second + 1;
            }
            rank[id] = inferred_rank.value_or(0);
        }
    }

    std::map<size_t, std::vector<std::string>> rows;
    for (const auto& id : ids) rows[rank.count(id) ? rank[id] : 0].push_back(id);
    for (auto& [row, row_ids] : rows) std::stable_sort(row_ids.begin(), row_ids.end(), by_order);

    size_t columns = 1;
    for (const auto& [row, row_ids] : rows) columns = std::max(columns, row_ids.size());
    std::vector<BlockNode> block_nodes;
    for (const auto& [row, row_ids] : rows) {
        for (const auto& id : row_ids) block_nodes.push_back({id, 1, false});
        for (size_t missing = columns - row_ids.size(); missing > 0; --missing) block_nodes.push_back({"__space", 1, true});
    }
    return {block_nodes, columns};
}

}  // namespace

Layout compute_block_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    std::map<std::string, NodeLayout> nodes = build_graph_node_layouts(graph, theme, config);

    float node_gap = rmax(theme.font_size * 0.4f, 4.0f);
    float column_gap = rmax(theme.font_size * 0.45f, 6.0f);
    float origin_x = 6.0f, origin_y = 6.0f;

    std::vector<EdgeLayout> edges;
    Layout layout;
    layout.kind = graph.kind;

    if (!graph.block) {
        std::vector<SubgraphLayout> subgraphs = build_subgraph_layouts(graph, nodes, theme, config);
        normalize_layout_nodes(nodes, edges, subgraphs);
        auto [max_x, max_y] = bounds_without_padding(nodes, subgraphs);
        layout.nodes = std::move(nodes);
        layout.subgraphs = std::move(subgraphs);
        layout.width = max_x + 6.0f;
        layout.height = max_y + 6.0f;
        finalize_graph_bounds(layout, config);
        return layout;
    }
    const BlockDiagram& block = *graph.block;

    std::vector<BlockNode> placement_nodes;
    size_t inferred_columns = 0;
    if (block.nodes.empty()) {
        auto inferred = infer_block_grid(graph);
        placement_nodes = std::move(inferred.first);
        inferred_columns = inferred.second;
    } else {
        placement_nodes = block.nodes;
    }
    size_t columns;
    if (block.columns && *block.columns > 0) columns = *block.columns;
    else if (placement_nodes.empty()) columns = 1;
    else if (inferred_columns > 0) columns = inferred_columns;
    else columns = std::max<size_t>(placement_nodes.size(), 1);

    std::vector<float> column_widths(columns, 0.0f), column_x(columns, 0.0f), row_y;
    size_t row = 0, col = 0;
    std::vector<float> row_heights{0.0f};

    for (const BlockNode& node : placement_nodes) {
        if (col >= columns) {
            col = 0;
            ++row;
            row_heights.push_back(0.0f);
        }
        size_t span = std::min(std::max<size_t>(node.span, 1), columns);
        if (col + span > columns) {
            col = 0;
            ++row;
            row_heights.push_back(0.0f);
        }
        if (!node.is_space) {
            auto it = nodes.find(node.id);
            if (it != nodes.end()) {
                float per_col = it->second.width / static_cast<float>(span);
                for (size_t i = 0; i < span; ++i) {
                    size_t idx = col + i;
                    if (idx < columns) column_widths[idx] = rmax(column_widths[idx], per_col);
                }
                row_heights[row] = rmax(row_heights[row], it->second.height);
            }
        }
        col += span;
    }

    column_x[0] = origin_x;
    for (size_t i = 1; i < columns; ++i) column_x[i] = column_x[i - 1] + column_widths[i - 1] + column_gap;

    float y_cursor = origin_y;
    for (float h : row_heights) {
        row_y.push_back(y_cursor);
        y_cursor += h + node_gap;
    }

    row = 0;
    col = 0;
    for (const BlockNode& node : placement_nodes) {
        if (col >= columns) {
            col = 0;
            ++row;
        }
        size_t span = std::min(std::max<size_t>(node.span, 1), columns);
        if (col + span > columns) {
            col = 0;
            ++row;
        }
        if (!node.is_space) {
            auto it = nodes.find(node.id);
            if (it != nodes.end()) {
                NodeLayout& nl = it->second;
                float start_x = column_x[col];
                float span_width = 0.0f;
                for (size_t i = 0; i < span; ++i) {
                    size_t idx = col + i;
                    if (idx < columns) {
                        span_width += column_widths[idx];
                        if (i + 1 < span) span_width += column_gap;
                    }
                }
                nl.x = start_x + (span_width - nl.width) / 2.0f;
                nl.y = row_y[row] + (row_heights[row] - nl.height) / 2.0f;
            }
        }
        col += span;
    }

    for (const Edge& edge : graph.edges) {
        auto from_it = nodes.find(edge.from);
        auto to_it = nodes.find(edge.to);
        if (from_it == nodes.end() || to_it == nodes.end()) continue;
        const NodeLayout& from_layout = from_it->second;
        const NodeLayout& to_layout = to_it->second;
        Point from_center{from_layout.x + from_layout.width / 2.0f, from_layout.y + from_layout.height / 2.0f};
        Point to_center{to_layout.x + to_layout.width / 2.0f, to_layout.y + to_layout.height / 2.0f};
        EdgeLayout el;
        el.from = edge.from;
        el.to = edge.to;
        if (edge.label) el.label = measure_label(*edge.label, theme, config);
        if (edge.start_label) el.start_label = measure_label(*edge.start_label, theme, config);
        if (edge.end_label) el.end_label = measure_label(*edge.end_label, theme, config);
        el.points = {from_center, to_center};
        el.directed = edge.directed;
        el.arrow_start = edge.arrow_start;
        el.arrow_end = edge.arrow_end;
        el.arrow_start_kind = edge.arrow_start_kind;
        el.arrow_end_kind = edge.arrow_end_kind;
        el.start_decoration = edge.start_decoration;
        el.end_decoration = edge.end_decoration;
        el.style = edge.style;
        // (the style index is the number of edges laid out so far, as in the Rust code)
        el.override_style = resolve_edge_style_of(edges.size(), graph);
        if (edge.style == EdgeStyle::Dotted && !el.override_style.dasharray) el.override_style.dasharray = "3 3";
        edges.push_back(std::move(el));
    }

    std::vector<SubgraphLayout> subgraphs = build_subgraph_layouts(graph, nodes, theme, config);
    normalize_layout_nodes(nodes, edges, subgraphs);
    auto [max_x, max_y] = bounds_without_padding(nodes, subgraphs, edges);

    layout.nodes = std::move(nodes);
    layout.edges = std::move(edges);
    layout.subgraphs = std::move(subgraphs);
    layout.width = max_x + 6.0f;
    layout.height = max_y + 6.0f;
    // compute_layout's final passes for graph-data layouts
    resolve_all_label_positions(layout, theme, config);
    finalize_graph_bounds(layout, config);
    return layout;
}

}  // namespace mermaid
