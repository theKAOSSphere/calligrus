// Mermaid port: treemaps. Mirrors parser.rs parse_treemap_diagram and layout/treemap.rs. The writer is the generic
// graph writer (svg_writer.cpp), which has Treemap branches: big boxes first, label at the top-left corner.
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "kind_util.h"
#include "kinds.h"
#include "parser_internal.h"
#include "strutil.h"

namespace mermaid {

namespace {

// config.treemap defaults (config.rs TreemapConfig::default)
constexpr float kTreemapWidth = 720.0f, kTreemapHeight = 480.0f;
constexpr float kTreemapPadding = 8.0f, kTreemapGap = 3.0f;
constexpr float kLabelPaddingX = 6.0f, kLabelPaddingY = 4.0f, kMinLabelArea = 200.0f;

// parse_treemap_item: `"Label": 12` -> (label, value text).
std::pair<std::string, std::optional<std::string>> parse_treemap_item(const std::string& line) {
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        std::string left = trim(line.substr(0, colon));
        std::string label = pp::strip_quotes(left);
        std::string value = trim(line.substr(colon + 1));
        return {label.empty() ? left : label, value.empty() ? std::nullopt : std::optional<std::string>(value)};
    }
    return {pp::strip_quotes(trim(line)), std::nullopt};
}

struct Rect {
    float x, y, w, h;
    Rect inset(float padding) const {
        float pad = rmax(padding, 0.0f);
        return {x + pad, y + pad, rmax(w - pad * 2.0f, 0.0f), rmax(h - pad * 2.0f, 0.0f)};
    }
};

struct TreemapCtx {
    const Graph& graph;
    const Theme& theme;
    const LayoutConfig& config;
    std::unordered_map<std::string, std::vector<std::string>> children;
    std::unordered_map<std::string, float> weight_cache;
    std::map<std::string, NodeLayout>& nodes_out;
};

float treemap_weight(const std::string& id, TreemapCtx& ctx) {
    if (auto it = ctx.weight_cache.find(id); it != ctx.weight_cache.end()) return it->second;
    float weight = 0.0f;
    if (auto node = ctx.graph.nodes.find(id); node != ctx.graph.nodes.end() && node->second.value && std::isfinite(*node->second.value))
        weight = *node->second.value;
    if (weight <= 0.0f) {
        if (auto kids = ctx.children.find(id); kids != ctx.children.end()) {
            weight = 0.0f;
            for (const std::string& child : kids->second) weight += treemap_weight(child, ctx);
        }
    }
    if (weight <= 0.0f) weight = 1.0f;
    ctx.weight_cache[id] = weight;
    return weight;
}

std::string treemap_depth_color(size_t depth, const Theme& theme) {
    switch (depth % 3) {
    case 0: return theme.primary_color;
    case 1: return theme.secondary_color;
    default: return theme.tertiary_color;
    }
}

void layout_treemap_nodes(const std::vector<std::string>& ids, Rect rect, size_t depth, TreemapCtx& ctx) {
    if (ids.empty() || rect.w <= 0.0f || rect.h <= 0.0f) return;
    float total_weight = 0.0f;
    for (const std::string& id : ids) total_weight += treemap_weight(id, ctx);
    if (total_weight <= 0.0f) return;
    float gap = rmax(kTreemapGap, 0.0f);
    bool horizontal = depth % 2 == 0;
    size_t count = ids.size();
    float gaps = gap * static_cast<float>(count - 1);
    float available = horizontal ? rmax(rect.w - gaps, 0.0f) : rmax(rect.h - gaps, 0.0f);

    float offset = 0.0f;
    for (const std::string& id : ids) {
        float weight = treemap_weight(id, ctx);
        float ratio = rmax(weight / total_weight, 0.0f);
        float span = available * ratio;
        Rect node_rect;
        if (horizontal) {
            float x = rect.x + offset;
            offset += span + gap;
            node_rect = {x, rect.y, span, rect.h};
        } else {
            float y = rect.y + offset;
            offset += span + gap;
            node_rect = {rect.x, y, rect.w, span};
        }

        float child_header_reserve = 0.0f;
        if (auto node_it = ctx.graph.nodes.find(id); node_it != ctx.graph.nodes.end()) {
            const Node& node = node_it->second;
            NodeStyle style = resolve_node_style_of(id, ctx.graph);
            if (!style.fill) style.fill = treemap_depth_color(depth, ctx.theme);
            if (!style.stroke) style.stroke = ctx.theme.primary_border_color;
            if (!style.stroke_width) style.stroke_width = 1.0f;
            if (!style.text_color) style.text_color = ctx.theme.primary_text_color;

            TextBlock label = measure_label(node.label, ctx.theme, ctx.config);
            float pad_x = kLabelPaddingX, pad_y = kLabelPaddingY;
            bool fits = label.width <= rmax(node_rect.w - pad_x * 2.0f, 0.0f) && label.height <= rmax(node_rect.h - pad_y * 2.0f, 0.0f);
            float area = node_rect.w * node_rect.h;
            if (fits && area >= kMinLabelArea) {
                child_header_reserve = rmax(label.height + pad_y * 2.0f, 0.0f);
            } else {
                label = TextBlock{{""}, 0.0f, 0.0f};
            }

            NodeLayout nl;
            nl.id = node.id;
            nl.x = node_rect.x;
            nl.y = node_rect.y;
            nl.width = node_rect.w;
            nl.height = node_rect.h;
            nl.label = std::move(label);
            nl.shape = NodeShape::Rectangle;
            nl.style = std::move(style);
            if (auto link = ctx.graph.node_links.find(id); link != ctx.graph.node_links.end()) nl.link = link->second;
            ctx.nodes_out.insert_or_assign(id, std::move(nl));
        }

        if (auto kids = ctx.children.find(id); kids != ctx.children.end()) {
            Rect child_rect = node_rect.inset(kTreemapPadding);
            if (child_header_reserve > 0.0f) {
                float reserve = rmin(child_header_reserve, child_rect.h * 0.35f);
                child_rect.y += reserve;
                child_rect.h = rmax(child_rect.h - reserve, 0.0f);
            }
            if (child_rect.w > 1.0f && child_rect.h > 1.0f) layout_treemap_nodes(kids->second, child_rect, depth + 1, ctx);
        }
    }
}

}  // namespace

Graph parse_treemap_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::Treemap;
    graph.direction = Direction::LeftRight;
    std::vector<std::string> stack;
    std::optional<size_t> base_indent;

    for (const std::string& raw_line : pp::preprocess_input_keep_indent(input)) {
        std::string trimmed = trim(raw_line);
        if (trimmed.empty()) continue;
        if (starts_with(to_ascii_lower(trimmed), "treemap")) continue;
        size_t indent = pp::count_indent(raw_line);
        if (!base_indent) base_indent = indent;
        size_t rel_indent = indent > *base_indent ? indent - *base_indent : 0;
        size_t level = rel_indent / 2;
        if (level > stack.size()) level = stack.size();

        auto [label, value] = parse_treemap_item(trimmed);
        std::optional<float> numeric_value;
        if (value) {
            float parsed;
            if (parse_rust_f32(trim(*value), parsed)) numeric_value = parsed;
        }
        std::string node_id = "treemap_" + std::to_string(graph.nodes.size());
        std::string node_label = value ? label + "\n" + *value : label;
        graph.ensure_node(node_id, node_label, NodeShape::Rectangle);
        if (numeric_value) graph.nodes[node_id].value = numeric_value;

        if (level > 0) {
            if (stack.size() > level) stack.resize(level);
            if (!stack.empty()) {
                Edge e;
                e.from = stack.back();
                e.to = node_id;
                graph.edges.push_back(std::move(e));
            }
        } else {
            stack.clear();
        }
        stack.push_back(node_id);
    }
    return graph;
}

Layout compute_treemap_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    Layout layout;
    layout.kind = graph.kind;
    float width = rmax(kTreemapWidth, 1.0f), height = rmax(kTreemapHeight, 1.0f);
    Rect root_rect{0.0f, 0.0f, width, height};

    TreemapCtx ctx{graph, theme, config, {}, {}, layout.nodes};
    std::unordered_map<std::string, std::string> parents;
    for (const Edge& edge : graph.edges) {
        ctx.children[edge.from].push_back(edge.to);
        parents[edge.to] = edge.from;
    }
    auto order_of = [&](const std::string& id) {
        auto it = graph.node_order.find(id);
        return it == graph.node_order.end() ? SIZE_MAX : it->second;
    };
    for (auto& [id, list] : ctx.children)
        std::stable_sort(list.begin(), list.end(), [&](const std::string& a, const std::string& b) { return order_of(a) < order_of(b); });

    std::vector<std::string> roots;
    for (const auto& [id, node] : graph.nodes)
        if (!parents.count(id)) roots.push_back(id);
    std::stable_sort(roots.begin(), roots.end(), [&](const std::string& a, const std::string& b) { return order_of(a) < order_of(b); });

    if (!roots.empty()) layout_treemap_nodes(roots, root_rect, 0, ctx);

    layout.width = width;
    layout.height = height;
    finalize_graph_bounds(layout, config);
    return layout;
}

}  // namespace mermaid
