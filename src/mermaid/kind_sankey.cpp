// Mermaid port: sankey diagrams. Mirrors parser.rs parse_sankey_diagram, layout/sankey.rs and render.rs render_sankey.
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <unordered_map>

#include "kind_util.h"
#include "kinds.h"
#include "numfmt.h"
#include "parser_internal.h"
#include "strutil.h"
#include "svg_common.h"

namespace mermaid {

Graph parse_sankey_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::Sankey;
    graph.direction = Direction::LeftRight;

    for (const std::string& raw_line : pp::preprocess_input(input)) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        if (starts_with(to_ascii_lower(line), "sankey")) continue;
        std::vector<std::string> parts = split_args(line);
        if (parts.size() < 3) continue;
        std::string from = pp::strip_quotes(trim(parts[0]));
        std::string to = pp::strip_quotes(trim(parts[1]));
        std::string value = trim(parts[2]);
        if (from.empty() || to.empty()) continue;
        graph.ensure_node(from, std::nullopt, NodeShape::Rectangle);
        graph.ensure_node(to, std::nullopt, NodeShape::Rectangle);
        Edge e;
        e.from = from;
        e.to = to;
        if (!value.empty()) e.label = value;
        e.directed = true;
        e.arrow_end = true;
        graph.edges.push_back(std::move(e));
    }
    return graph;
}

namespace {

// Rust f32::clamp
float clamp_f(float v, float lo, float hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

// compute_link_tops: where each outgoing link starts on its source node (stacked in `outbound_order`).
void compute_link_tops(const std::vector<float>& node_positions, const std::vector<std::vector<size_t>>& outbound_order,
                       const std::vector<float>& edge_thickness, std::vector<float>& link_top, std::vector<float>& outbound_offset,
                       std::vector<float>& acc) {
    std::fill(link_top.begin(), link_top.end(), 0.0f);
    std::fill(outbound_offset.begin(), outbound_offset.end(), 0.0f);
    std::fill(acc.begin(), acc.end(), 0.0f);
    for (size_t source_idx = 0; source_idx < outbound_order.size(); ++source_idx) {
        for (size_t edge_idx : outbound_order[source_idx]) {
            float offset = acc[source_idx];
            outbound_offset[edge_idx] = offset;
            link_top[edge_idx] = node_positions[source_idx] + offset;
            acc[source_idx] += edge_thickness[edge_idx];
        }
    }
}

}  // namespace

Layout compute_sankey_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    constexpr float kMinWidth = 560.0f, kMaxWidth = 640.0f, kHeight = 360.0f, kNodeWidth = 10.0f;
    static const char* const kPalette[10] = {"#4e79a7", "#f28e2c", "#e15759", "#76b7b2", "#59a14f",
                                              "#edc949", "#af7aa1", "#ff9da7", "#9c755f", "#bab0ab"};

    auto order_of = [&](const std::string& id) {
        auto it = graph.node_order.find(id);
        return it == graph.node_order.end() ? SIZE_MAX : it->second;
    };
    std::vector<std::string> node_ids;
    for (const auto& [id, node] : graph.nodes) node_ids.push_back(id);
    std::stable_sort(node_ids.begin(), node_ids.end(), [&](const std::string& a, const std::string& b) {
        size_t oa = order_of(a), ob = order_of(b);
        if (oa != ob) return oa < ob;
        return a < b;
    });

    size_t node_count = node_ids.size();
    std::unordered_map<std::string, size_t> id_to_idx;
    for (size_t idx = 0; idx < node_count; ++idx) id_to_idx[node_ids[idx]] = idx;
    std::vector<size_t> node_order_idx;
    for (const auto& id : node_ids) node_order_idx.push_back(order_of(id));

    struct EdgeData {
        size_t from_idx, to_idx;
        float value;
    };
    std::vector<EdgeData> edges_data;
    std::vector<std::vector<size_t>> incoming(node_count), outgoing(node_count);
    std::vector<size_t> indegree(node_count, 0);
    std::vector<float> in_total(node_count, 0.0f), out_total(node_count, 0.0f);

    for (const Edge& edge : graph.edges) {
        auto from = id_to_idx.find(edge.from);
        auto to = id_to_idx.find(edge.to);
        if (from == id_to_idx.end() || to == id_to_idx.end()) continue;
        size_t from_idx = from->second, to_idx = to->second;
        float raw_value = 1.0f;
        if (edge.label) {
            float parsed;
            if (parse_rust_f32(*edge.label, parsed)) raw_value = parsed;
        }
        float value = std::isfinite(raw_value) ? rmax(raw_value, 0.0f) : 0.0f;
        size_t edge_idx = edges_data.size();
        edges_data.push_back({from_idx, to_idx, value});
        outgoing[from_idx].push_back(edge_idx);
        incoming[to_idx].push_back(edge_idx);
        indegree[to_idx] += 1;
        out_total[from_idx] += value;
        in_total[to_idx] += value;
    }

    // ranks: longest path from the sources (only when the graph is acyclic)
    std::vector<size_t> ranks(node_count, 0);
    std::vector<size_t> indegree_work = indegree;
    std::deque<size_t> queue;
    for (size_t idx = 0; idx < node_count; ++idx)
        if (indegree_work[idx] == 0) queue.push_back(idx);
    std::vector<size_t> topo;
    while (!queue.empty()) {
        size_t node_idx = queue.front();
        queue.pop_front();
        topo.push_back(node_idx);
        for (size_t edge_idx : outgoing[node_idx]) {
            size_t to_idx = edges_data[edge_idx].to_idx;
            if (indegree_work[to_idx] > 0) {
                indegree_work[to_idx] -= 1;
                if (indegree_work[to_idx] == 0) queue.push_back(to_idx);
            }
        }
    }
    if (topo.size() == node_count) {
        for (size_t node_idx : topo)
            for (size_t edge_idx : outgoing[node_idx]) {
                size_t to_idx = edges_data[edge_idx].to_idx;
                ranks[to_idx] = std::max(ranks[to_idx], ranks[node_idx] + 1);
            }
    }

    size_t max_rank = 0;
    for (size_t r : ranks) max_rank = std::max(max_rank, r);
    size_t num_ranks = max_rank + 1;
    float sankey_width = clamp_f(kMinWidth + static_cast<float>(num_ranks > 2 ? num_ranks - 2 : 0) * 25.0f, kMinWidth, kMaxWidth);
    float gap_x = num_ranks > 1 ? rmax((sankey_width - kNodeWidth * static_cast<float>(num_ranks)) / static_cast<float>(num_ranks - 1), 0.0f) : 0.0f;

    std::vector<float> totals(node_count, 0.0f);
    for (size_t idx = 0; idx < node_count; ++idx) {
        float total = rmax(in_total[idx], out_total[idx]);
        totals[idx] = total > 0.0f ? total : 1.0f;
    }
    float max_total = 0.0f;
    for (float t : totals) max_total = rmax(max_total, t);
    max_total = rmax(max_total, 1.0f);
    float scale = kHeight / max_total;

    std::vector<float> node_x(node_count, 0.0f), node_y(node_count, 0.0f), node_h(node_count, 0.0f);
    for (size_t idx = 0; idx < node_count; ++idx) {
        node_x[idx] = static_cast<float>(ranks[idx]) * (kNodeWidth + gap_x);
        node_h[idx] = totals[idx] * scale;
    }

    std::vector<std::vector<size_t>> rank_nodes(num_ranks);
    for (size_t idx = 0; idx < node_count; ++idx) rank_nodes[ranks[idx]].push_back(idx);
    for (auto& nodes_in_rank : rank_nodes)
        std::stable_sort(nodes_in_rank.begin(), nodes_in_rank.end(), [&](size_t a, size_t b) {
            if (node_order_idx[a] != node_order_idx[b]) return node_order_idx[a] < node_order_idx[b];
            return node_ids[a] < node_ids[b];
        });

    // outgoing links of a node: the ones going furthest right first, then by target order
    std::vector<std::vector<size_t>> outbound_order = outgoing;
    for (auto& edges : outbound_order)
        std::stable_sort(edges.begin(), edges.end(), [&](size_t a, size_t b) {
            size_t target_a = edges_data[a].to_idx, target_b = edges_data[b].to_idx;
            if (ranks[target_b] != ranks[target_a]) return ranks[target_b] < ranks[target_a];
            if (node_order_idx[target_a] != node_order_idx[target_b]) return node_order_idx[target_a] < node_order_idx[target_b];
            return node_ids[target_a] < node_ids[target_b];
        });

    std::vector<float> edge_thickness;
    for (const auto& e : edges_data) edge_thickness.push_back(e.value * scale);
    std::vector<float> link_top(edges_data.size(), 0.0f), outbound_offset(edges_data.size(), 0.0f), acc(node_count, 0.0f);

    for (size_t rank = 1; rank <= max_rank; ++rank) {
        compute_link_tops(node_y, outbound_order, edge_thickness, link_top, outbound_offset, acc);
        for (size_t node_idx : rank_nodes[rank]) {
            float min_top = std::numeric_limits<float>::infinity();
            for (size_t edge_idx : incoming[node_idx]) {
                size_t from_idx = edges_data[edge_idx].from_idx;
                if (ranks[from_idx] >= rank) continue;
                min_top = rmin(min_top, link_top[edge_idx]);
            }
            if (!std::isfinite(min_top)) continue;
            float max_y = rmax(kHeight - node_h[node_idx], 0.0f);
            node_y[node_idx] = clamp_f(min_top, 0.0f, max_y);
        }
    }
    compute_link_tops(node_y, outbound_order, edge_thickness, link_top, outbound_offset, acc);

    std::vector<std::pair<std::string, NodeStyle>> node_colors;
    for (size_t idx = 0; idx < node_count; ++idx) {
        std::string default_color = kPalette[idx % 10];
        NodeStyle style = resolve_node_style_of(node_ids[idx], graph);
        std::string color = style.fill ? *style.fill : default_color;
        if (!style.fill) style.fill = color;
        if (!style.stroke) style.stroke = "none";
        if (!style.stroke_width) style.stroke_width = 0.0f;
        node_colors.emplace_back(color, style);
    }

    Layout layout;
    layout.kind = graph.kind;
    SankeyData sankey;
    for (size_t idx = 0; idx < node_count; ++idx) {
        const std::string& id = node_ids[idx];
        std::string label = graph.nodes.at(id).label;
        TextBlock label_block = measure_label(label, theme, config);
        NodeLayout nl;
        nl.id = id;
        nl.x = node_x[idx];
        nl.y = node_y[idx];
        nl.width = kNodeWidth;
        nl.height = node_h[idx];
        nl.label = std::move(label_block);
        nl.shape = NodeShape::Rectangle;
        nl.style = node_colors[idx].second;
        if (auto link = graph.node_links.find(id); link != graph.node_links.end()) nl.link = link->second;
        layout.nodes.insert_or_assign(id, std::move(nl));
        sankey.nodes.push_back({id, label, totals[idx], ranks[idx], node_x[idx], node_y[idx], kNodeWidth, node_h[idx], node_colors[idx].first});
    }

    for (size_t edge_idx = 0; edge_idx < edges_data.size(); ++edge_idx) {
        const EdgeData& edge = edges_data[edge_idx];
        const std::string& from_id = node_ids[edge.from_idx];
        const std::string& to_id = node_ids[edge.to_idx];
        float thickness = edge_thickness[edge_idx];
        if (thickness <= 0.0f) continue;
        float start_x = node_x[edge.from_idx] + kNodeWidth;
        float end_x = node_x[edge.to_idx];
        float start_y = node_y[edge.from_idx] + outbound_offset[edge_idx] + thickness / 2.0f;
        float inbound_offset = rmax(link_top[edge_idx] - node_y[edge.to_idx], 0.0f);
        float end_y = node_y[edge.to_idx] + inbound_offset + thickness / 2.0f;
        const std::string& color_start = node_colors[edge.from_idx].first;
        const std::string& color_end = node_colors[edge.to_idx].first;

        EdgeLayout el;
        el.from = from_id;
        el.to = to_id;
        el.points = {{start_x, start_y}, {end_x, end_y}};
        el.override_style.stroke = color_start;
        el.override_style.stroke_width = thickness;
        layout.edges.push_back(std::move(el));
        sankey.links.push_back({from_id, to_id, edge.value, thickness, {start_x, start_y}, {end_x, end_y}, color_start, color_end,
                                "sankey-grad-" + std::to_string(edge_idx)});
    }

    layout.width = sankey_width;
    layout.height = kHeight;
    sankey.width = sankey_width;
    sankey.height = kHeight;
    sankey.node_width = kNodeWidth;
    layout.sankey = std::move(sankey);
    // compute_layout's label pass finds no edge labels here, so nothing else changes.
    return layout;
}

namespace {

// render.rs format_sankey_value: 12, 12.5 or 12.25 (at most two decimals).
std::string format_sankey_value(float value) {
    float rounded_2 = std::round(value * 100.0f) / 100.0f;
    if (std::fabs(rounded_2 - std::round(rounded_2)) < 0.001f) return fmt_fixed(rounded_2, 0);
    float rounded_1 = std::round(value * 10.0f) / 10.0f;
    if (std::fabs(rounded_1 - rounded_2) < 0.001f) return fmt_fixed(rounded_1, 1);
    return fmt_fixed(rounded_2, 2);
}

}  // namespace

std::string render_sankey_svg(const Layout& layout, const Theme& theme, const LayoutConfig&) {
    using svg::escape_xml;
    using svg::F;
    using svg::F2;
    using svg::F3;
    const SankeyData& sk = *layout.sankey;
    float width = rmax(layout.width, 1.0f), height = rmax(layout.height, 1.0f);
    bool has_links = false;
    for (const auto& [id, n] : layout.nodes)
        if (n.link) has_links = true;
    std::string out = svg::document_start(width, height, 0.0f, 0.0f, width, height, false, has_links, theme, false);
    // one marker pair per distinct edge colour (the writer draws no arrows, but the crate emits the defs)
    std::vector<std::string> colors{theme.line_color};
    for (const auto& e : layout.edges)
        if (e.override_style.stroke && std::find(colors.begin(), colors.end(), *e.override_style.stroke) == colors.end())
            colors.push_back(*e.override_style.stroke);
    out += svg::marker_defs(colors);

    size_t max_rank = 0;
    for (const auto& n : sk.nodes) max_rank = std::max(max_rank, n.rank);
    const float label_font_size = 14.0f;

    out += "<g class=\"nodes\">";
    for (size_t idx = 0; idx < sk.nodes.size(); ++idx) {
        const auto& node = sk.nodes[idx];
        out += "<g class=\"node\" id=\"node-" + std::to_string(idx + 1) + "\" transform=\"translate(" + F3(node.x) + "," + F3(node.y) + ")\" x=\"" +
               F3(node.x) + "\" y=\"" + F3(node.y) + "\">";
        out += "<rect height=\"" + F(node.height) + "\" width=\"" + F(sk.node_width) + "\" fill=\"" + escape_xml(node.color) + "\"/>";
        out += "</g>";
    }
    out += "</g>";

    // label positions: centred on the node, then pushed apart within each rank
    std::vector<float> label_y;
    for (const auto& node : sk.nodes) label_y.push_back(node.y + node.height / 2.0f);
    float label_line_height = label_font_size * 1.2f;
    std::vector<float> label_half_heights;
    for (const auto& node : sk.nodes) {
        size_t text_lines = 0;
        for (const std::string& line : lines_of(node.label))
            if (!trim_view(line).empty()) ++text_lines;
        float count = static_cast<float>(std::max<size_t>(text_lines, 1));
        // Node labels always render one additional value line below the title.
        label_half_heights.push_back((count + 1.0f) * label_line_height * 0.5f);
    }
    std::vector<float> rank_min_x(max_rank + 1, std::numeric_limits<float>::infinity());
    for (const auto& node : sk.nodes) rank_min_x[node.rank] = rmin(rank_min_x[node.rank], node.x);
    for (float& x : rank_min_x)
        if (!std::isfinite(x)) x = 0.0f;
    float edge_margin = label_font_size * 0.3f;
    float preferred_gap = label_font_size * 0.25f;
    for (size_t rank = 0; rank <= max_rank; ++rank) {
        std::vector<size_t> indices;
        for (size_t idx = 0; idx < sk.nodes.size(); ++idx)
            if (sk.nodes[idx].rank == rank) indices.push_back(idx);
        if (indices.size() < 2) continue;
        std::stable_sort(indices.begin(), indices.end(), [&](size_t a, size_t b) { return label_y[a] < label_y[b]; });
        size_t first_idx = indices.front(), last_idx = indices.back();
        float top = edge_margin + label_half_heights[first_idx];
        float bottom = rmax(layout.height - edge_margin - label_half_heights[last_idx], top);

        float half_pair_span = 0.0f;
        for (size_t i = 0; i + 1 < indices.size(); ++i) half_pair_span += label_half_heights[indices[i]] + label_half_heights[indices[i + 1]];
        float available_span = rmax(bottom - top, 0.0f);
        float max_gap = (available_span - half_pair_span) / static_cast<float>(indices.size() - 1);
        float gap = rmin(preferred_gap, rmax(max_gap, 0.0f));
        float required_span = half_pair_span + gap * static_cast<float>(indices.size() - 1);
        float first_max = rmax(bottom - required_span, top);

        label_y[first_idx] = clamp_f(label_y[first_idx], top, first_max);
        for (size_t i = 0; i + 1 < indices.size(); ++i) {
            size_t prev_idx = indices[i], cur_idx = indices[i + 1];
            float min_gap = label_half_heights[prev_idx] + label_half_heights[cur_idx] + gap;
            label_y[cur_idx] = label_y[prev_idx] + min_gap;
        }
    }

    out += "<g class=\"node-labels\" font-size=\"" + F(label_font_size) + "\" fill=\"" + theme.primary_text_color + "\">";
    for (size_t idx = 0; idx < sk.nodes.size(); ++idx) {
        const auto& node = sk.nodes[idx];
        bool align_left_of_node = node.rank > 0;
        const char* text_anchor = align_left_of_node ? "end" : "start";
        float x;
        if (align_left_of_node) {
            float inward_offset = 6.0f;
            if (node.rank < max_rank) {
                float prev_x = rank_min_x[node.rank > 0 ? node.rank - 1 : 0];
                float rank_gap = rmax(node.x - prev_x, 0.0f);
                inward_offset = clamp_f(rank_gap * 0.2f, 6.0f, label_font_size * 3.0f);
            }
            x = node.x - inward_offset;
        } else {
            x = node.x + sk.node_width + 6.0f;
        }
        float y = label_y[idx];
        std::string label = escape_xml(node.label);
        std::string value = format_sankey_value(node.total);
        float first_y = y - label_font_size * 0.4f;
        out += "<text x=\"" + F2(x) + "\" y=\"" + F2(first_y) + "\" dy=\"0em\" text-anchor=\"" + text_anchor + "\" font-size=\"" + fmt_fixed(label_font_size, 1) +
               "\"><tspan x=\"" + F2(x) + "\" dy=\"0em\">" + label + "</tspan><tspan x=\"" + F2(x) + "\" dy=\"1.15em\">" + value + "</tspan></text>";
    }
    out += "</g>";

    out += "<g class=\"links\" fill=\"none\" stroke-opacity=\"0.5\">";
    for (const auto& link : sk.links) {
        float mid_x = (link.start.first + link.end.first) / 2.0f;
        std::string gradient_id = escape_xml(link.gradient_id);
        out += "<g class=\"link\" style=\"mix-blend-mode: multiply;\">";
        out += "<linearGradient id=\"" + gradient_id + "\" gradientUnits=\"userSpaceOnUse\" x1=\"" + F(link.start.first) + "\" x2=\"" + F(link.end.first) + "\">";
        out += "<stop offset=\"0%\" stop-color=\"" + escape_xml(link.color_start) + "\"/>";
        out += "<stop offset=\"100%\" stop-color=\"" + escape_xml(link.color_end) + "\"/>";
        out += "</linearGradient>";
        out += "<path d=\"M" + F3(link.start.first) + "," + F3(link.start.second) + "C" + F3(mid_x) + "," + F3(link.start.second) + "," + F3(mid_x) + "," +
               F3(link.end.second) + "," + F3(link.end.first) + "," + F3(link.end.second) + "\" stroke=\"url(#" + gradient_id + ")\" stroke-width=\"" +
               F(link.thickness) + "\"/>";
        out += "</g>";
    }
    out += "</g>";
    out += "</svg>";
    return out;
}

}  // namespace mermaid
