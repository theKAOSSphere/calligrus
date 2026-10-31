// Mermaid port: radar charts. Mirrors parser.rs parse_radar_diagram, layout/radar.rs and render.rs render_radar.
// A radar curve is a graph node whose label holds the series ("name\naxis: value\n..."); the writer parses it back.
#include <cstdint>
#include <algorithm>
#include <cmath>

#include "kind_util.h"
#include "kinds.h"
#include "numfmt.h"
#include "parser_internal.h"
#include "strutil.h"
#include "svg_common.h"
#include "text.h"

namespace mermaid {

namespace {

constexpr float kPi = 3.14159265358979323846f;  // std::f32::consts::PI

// parse_radar_curve: `curve name{1, 2, 3}` -> (name, values).
std::optional<std::pair<std::string, std::vector<std::string>>> parse_radar_curve(const std::string& line) {
    std::string rest = trim(line.size() > 5 ? line.substr(5) : std::string());
    size_t open = rest.find('{');
    if (open == std::string::npos) return std::nullopt;
    std::string name = pp::strip_quotes(trim(rest.substr(0, open)));
    std::string values_part = rest.substr(open + 1);
    size_t close = values_part.find('}');
    if (close == std::string::npos) return std::nullopt;
    std::vector<std::string> values;
    for (const std::string& value : split_args(values_part.substr(0, close))) {
        std::string v = trim(value);
        if (!v.empty()) values.push_back(v);
    }
    if (name.empty()) return std::nullopt;
    return std::make_pair(name, values);
}

}  // namespace

Graph parse_radar_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::Radar;
    graph.direction = Direction::LeftRight;
    std::vector<std::string> axes;

    for (const std::string& raw_line : pp::preprocess_input(input)) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        std::string lower = to_ascii_lower(line);
        if (starts_with(lower, "radar") || starts_with(lower, "title")) continue;
        if (starts_with(lower, "axis")) {
            std::string rest = trim(line.size() > 4 ? line.substr(4) : std::string());
            axes.clear();
            for (const std::string& value : split_args(rest)) {
                std::string v = pp::strip_quotes(trim(value));
                if (!v.empty()) axes.push_back(v);
            }
            continue;
        }
        if (starts_with(lower, "curve")) {
            auto curve = parse_radar_curve(line);
            if (!curve) continue;
            std::string node_id = "radar_" + std::to_string(graph.nodes.size());
            std::string label = curve->first;
            for (size_t idx = 0; idx < curve->second.size(); ++idx) {
                label += "\n";
                if (idx < axes.size()) label += axes[idx] + ": " + curve->second[idx];
                else label += curve->second[idx];
            }
            graph.ensure_node(node_id, label, NodeShape::Circle);
        }
    }
    return graph;
}

Layout compute_radar_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    constexpr float kWidth = 680.0f, kHeight = 680.0f;
    constexpr float kCenterX = kWidth / 2.0f, kCenterY = kHeight / 2.0f;
    constexpr float kMaxRadius = 290.0f;
    constexpr float kLegendBoxSize = 11.0f, kLegendGap = 3.0f;

    float legend_offset = kMaxRadius * 0.875f;
    float legend_base_x = kCenterX + legend_offset;
    float legend_base_y = kCenterY - legend_offset;
    float legend_row_height = theme.font_size + 6.0f;

    // nodes in declaration order (ties by id)
    std::vector<std::string> node_ids;
    for (const auto& [id, node] : graph.nodes) node_ids.push_back(id);
    auto order_of = [&](const std::string& id) {
        auto it = graph.node_order.find(id);
        return it == graph.node_order.end() ? SIZE_MAX : it->second;
    };
    std::stable_sort(node_ids.begin(), node_ids.end(), [&](const std::string& a, const std::string& b) {
        size_t oa = order_of(a), ob = order_of(b);
        if (oa != ob) return oa < ob;
        return a < b;
    });

    Layout layout;
    for (size_t idx = 0; idx < node_ids.size(); ++idx) {
        const Node& node = graph.nodes.at(node_ids[idx]);
        TextBlock label = measure_label(node.label, theme, config);
        float width = kLegendBoxSize + kLegendGap + label.width;
        float height = rmax(label.height, kLegendBoxSize);
        NodeStyle style = resolve_node_style_of(node.id, graph);
        if (!style.stroke) style.stroke = "none";
        if (!style.stroke_width) style.stroke_width = 0.0f;
        NodeLayout nl = build_node_layout(node, std::move(label), width, height, std::move(style), graph);
        nl.x = legend_base_x;
        nl.y = legend_base_y + static_cast<float>(idx) * legend_row_height;
        layout.nodes.emplace(node.id, std::move(nl));
    }
    layout.kind = graph.kind;
    layout.width = kWidth;
    layout.height = kHeight;
    // compute_layout's last step for DiagramData::Graph (radar has no edges, so only the bounds pass matters)
    finalize_graph_bounds(layout, config);
    return layout;
}

namespace {

// render_radar helpers
size_t radar_index(const std::string& id) {
    size_t us = id.rfind('_');
    std::string part = us == std::string::npos ? id : id.substr(us + 1);
    if (part.empty()) return SIZE_MAX;
    for (char c : part)
        if (c < '0' || c > '9') return SIZE_MAX;
    return static_cast<size_t>(std::strtoull(part.c_str(), nullptr, 10));
}

using RadarSeries = std::pair<std::string, std::vector<std::pair<std::string, float>>>;

std::optional<RadarSeries> parse_radar_series(const NodeLayout& node) {
    std::vector<std::string> lines;
    for (const std::string& l : node.label.lines) {
        std::string t = trim(l);
        if (!t.empty()) lines.push_back(t);
    }
    if (lines.empty()) return std::nullopt;
    RadarSeries series;
    series.first = lines[0];
    for (size_t i = 1; i < lines.size(); ++i) {
        size_t colon = lines[i].find(':');
        if (colon == std::string::npos) continue;
        std::string axis = trim(lines[i].substr(0, colon));
        std::string value_str = trim(lines[i].substr(colon + 1));
        if (axis.empty() || value_str.empty()) continue;
        float value;
        if (!parse_rust_f32(value_str, value)) continue;
        series.second.emplace_back(axis, rmax(value, 0.0f));
    }
    if (series.second.empty()) return std::nullopt;
    return series;
}

}  // namespace

std::string render_radar_svg(const Layout& layout, const Theme& theme, const LayoutConfig&) {
    using svg::F;
    using svg::F3;
    constexpr float kWidth = 700.0f, kHeight = 700.0f;
    constexpr float kCenterX = kWidth / 2.0f, kCenterY = kHeight / 2.0f;
    constexpr float kMaxRadius = 300.0f;
    constexpr int kGridSteps = 5;
    constexpr float kAxisLabelOffset = 15.0f;
    constexpr float kLegendBoxSize = 12.0f, kLegendGap = 4.0f;
    const std::string grid_color = "#DEDEDE", axis_color = "#333333";
    static const int hues[12] = {240, 60, 80, 270, 300, 330, 0, 30, 90, 150, 180, 210};
    const std::string lightness = "76.2745098039%";

    float width = rmax(layout.width, 1.0f), height = rmax(layout.height, 1.0f);
    std::string out = svg::document_start(width, height, 0.0f, 0.0f, width, height, false, false, theme);

    std::vector<const NodeLayout*> nodes;
    for (const auto& [id, n] : layout.nodes)
        if (!n.hidden) nodes.push_back(&n);
    std::stable_sort(nodes.begin(), nodes.end(), [](const NodeLayout* a, const NodeLayout* b) { return radar_index(a->id) < radar_index(b->id); });

    std::vector<RadarSeries> raw_series;
    for (const NodeLayout* node : nodes)
        if (auto series = parse_radar_series(*node)) raw_series.push_back(std::move(*series));
    if (raw_series.empty()) return out + "</svg>";

    std::vector<std::string> axes;
    for (const auto& pair : raw_series[0].second) axes.push_back(pair.first);
    size_t axis_count = axes.size();

    std::vector<std::pair<std::string, std::vector<float>>> series_values;
    float max_value = 0.0f;
    for (const auto& [name, pairs] : raw_series) {
        std::vector<float> values;
        for (const std::string& axis : axes) {
            float value = 0.0f;
            for (const auto& [a, v] : pairs)
                if (a == axis) { value = v; break; }
            max_value = rmax(max_value, value);
            values.push_back(value);
        }
        series_values.emplace_back(name, std::move(values));
    }
    if (max_value <= 0.0f) max_value = 1.0f;
    float scale = kMaxRadius / max_value;
    float angle_step = 2.0f * kPi / static_cast<float>(axis_count);
    float start_angle = -kPi / 2.0f;
    std::string font = svg::normalize_font_family(theme.font_family);

    out += "<g transform=\"translate(" + F3(kCenterX) + ", " + F3(kCenterY) + ")\">";
    for (int step = 1; step <= kGridSteps; ++step) {
        float r = kMaxRadius * static_cast<float>(step) / static_cast<float>(kGridSteps);
        out += "<circle r=\"" + F3(r) + "\" fill=\"" + grid_color + "\" fill-opacity=\"0.3\" stroke=\"" + grid_color + "\" stroke-width=\"1\" />";
    }
    for (size_t idx = 0; idx < axes.size(); ++idx) {
        float angle = start_angle + angle_step * static_cast<float>(idx);
        float x = kMaxRadius * std::cos(angle);
        float y = kMaxRadius * std::sin(angle);
        out += "<line x1=\"0\" y1=\"0\" x2=\"" + F3(x) + "\" y2=\"" + F3(y) + "\" stroke=\"" + axis_color + "\" stroke-width=\"2\" />";
        float label_r = kMaxRadius + kAxisLabelOffset;
        float lx = label_r * std::cos(angle);
        float ly = label_r * std::sin(angle);
        const char* anchor;
        if (std::cos(angle) > 0.35f) { lx -= 6.0f; anchor = "end"; }
        else if (std::cos(angle) < -0.35f) { lx += 6.0f; anchor = "start"; }
        else anchor = "middle";
        out += "<text x=\"" + F3(lx) + "\" y=\"" + F3(ly) + "\" text-anchor=\"" + anchor + "\" dominant-baseline=\"middle\" font-family=\"" + font +
               "\" font-size=\"12\" fill=\"" + axis_color + "\">" + svg::escape_xml(axes[idx]) + "</text>";
    }
    for (size_t series_idx = 0; series_idx < series_values.size(); ++series_idx) {
        const auto& [name, values] = series_values[series_idx];
        int hue = hues[series_idx % 12];
        std::string color = "hsl(" + std::to_string(hue) + ", 100%, " + lightness + ")";
        std::vector<Point> points;
        for (size_t idx = 0; idx < values.size(); ++idx) {
            float angle = start_angle + angle_step * static_cast<float>(idx);
            float r = values[idx] * scale;
            points.emplace_back(r * std::cos(angle), r * std::sin(angle));
        }
        if (points.empty()) continue;
        std::string d = "M" + F3(points[0].first) + "," + F3(points[0].second);
        for (size_t i = 1; i < points.size(); ++i) d += " L" + F3(points[i].first) + "," + F3(points[i].second);
        d += " Z";
        out += "<path d=\"" + d + "\" fill=\"" + svg::escape_xml(color) + "\" fill-opacity=\"0.5\" stroke=\"" + svg::escape_xml(color) + "\" stroke-width=\"2\" />";

        float legend_offset = kMaxRadius * 0.8f;
        float legend_x = legend_offset;
        float legend_y = -legend_offset + static_cast<float>(series_idx) * (theme.font_size + 6.0f);
        out += "<rect x=\"" + F3(legend_x) + "\" y=\"" + F3(legend_y) + "\" width=\"" + F(kLegendBoxSize) + "\" height=\"" + F(kLegendBoxSize) +
               "\" fill=\"" + svg::escape_xml(color) + "\" fill-opacity=\"0.5\" stroke=\"" + svg::escape_xml(color) + "\" />";
        out += "<text x=\"" + F3(legend_x + kLegendBoxSize + kLegendGap) + "\" y=\"" + F3(legend_y) +
               "\" text-anchor=\"start\" dominant-baseline=\"hanging\" font-family=\"" + font + "\" font-size=\"12\" fill=\"" + axis_color + "\">" +
               svg::escape_xml(name) + "</text>";
    }
    out += "<text x=\"0\" y=\"" + F3(-(kMaxRadius + 50.0f)) + "\" text-anchor=\"middle\" dominant-baseline=\"hanging\" font-family=\"" + font +
           "\" font-size=\"" + F(theme.font_size) + "\" fill=\"" + axis_color + "\"></text>";
    out += "</g></svg>";
    return out;
}

}  // namespace mermaid
