// Mermaid port: quadrant charts. Mirrors parser.rs parse_quadrant_diagram, layout/quadrant.rs and render.rs render_quadrant.
#include <cmath>

#include "kinds.h"
#include "numfmt.h"
#include "parser_internal.h"
#include "strutil.h"
#include "svg_common.h"
#include "text.h"

namespace mermaid {

namespace {
float fmax_(float a, float b) { return a > b ? a : (a < b ? b : a); }

// `a --> b` -> (trimmed a, trimmed b)
std::optional<std::pair<std::string, std::string>> split_arrow(const std::string& rest) {
    size_t p = rest.find("-->");
    if (p == std::string::npos) return std::nullopt;
    return std::make_pair(trim(rest.substr(0, p)), trim(rest.substr(p + 3)));
}

std::optional<std::tuple<std::string, float, float>> parse_quadrant_point_coords(const std::string& line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return std::nullopt;
    std::string label = trim(line.substr(0, colon));
    if (label.empty()) return std::nullopt;
    std::string coords = trim(line.substr(colon + 1));
    auto is_br = [](char c) { return c == '[' || c == ']' || c == '(' || c == ')'; };
    size_t a = 0, b = coords.size();
    while (a < b && is_br(coords[a])) ++a;
    while (b > a && is_br(coords[b - 1])) --b;
    coords = coords.substr(a, b - a);
    size_t comma = coords.find(',');
    std::string xs = trim(coords.substr(0, comma));
    if (comma == std::string::npos) return std::nullopt;  // second coordinate missing
    std::string rest = coords.substr(comma + 1);
    size_t comma2 = rest.find(',');
    std::string ys = trim(rest.substr(0, comma2));
    float x, y;
    if (!parse_rust_f32(xs, x) || !parse_rust_f32(ys, y)) return std::nullopt;
    return std::make_tuple(label, x, y);
}

float finite_unit_interval(float v) { return std::isfinite(v) ? (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)) : 0.0f; }
}  // namespace

Graph parse_quadrant_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::Quadrant;
    graph.direction = Direction::LeftRight;
    for (const std::string& raw_line : pp::preprocess_input(input)) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        std::string lower = to_ascii_lower(line);
        if (starts_with(lower, "quadrantchart")) continue;
        if (starts_with(lower, "title")) {
            std::string title = trim(line.substr(5));
            if (!title.empty()) graph.quadrant.title = title;
            continue;
        }
        if (starts_with(lower, "x-axis")) {
            if (auto lr = split_arrow(trim(line.substr(6)))) {
                graph.quadrant.x_axis_left = lr->first;
                graph.quadrant.x_axis_right = lr->second;
            }
            continue;
        }
        if (starts_with(lower, "y-axis")) {
            if (auto bt = split_arrow(trim(line.substr(6)))) {
                graph.quadrant.y_axis_bottom = bt->first;
                graph.quadrant.y_axis_top = bt->second;
            }
            continue;
        }
        if (starts_with(lower, "quadrant-")) {
            if (line.size() >= 10) {
                std::string label = trim(line.substr(10));
                if (starts_with(lower, "quadrant-1")) graph.quadrant.quadrant_labels[0] = label;
                else if (starts_with(lower, "quadrant-2")) graph.quadrant.quadrant_labels[1] = label;
                else if (starts_with(lower, "quadrant-3")) graph.quadrant.quadrant_labels[2] = label;
                else if (starts_with(lower, "quadrant-4")) graph.quadrant.quadrant_labels[3] = label;
            }
            continue;
        }
        if (auto p = parse_quadrant_point_coords(line)) graph.quadrant.points.push_back({std::get<0>(*p), std::get<1>(*p), std::get<2>(*p)});
    }
    return graph;
}

Layout compute_quadrant_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    const QuadrantData& q = graph.quadrant;
    float padding = theme.font_size * 1.6f;
    float grid_size = 360.0f;
    auto measure = [&](const std::optional<std::string>& t) -> std::optional<TextBlock> {
        if (!t) return std::nullopt;
        return measure_label(*t, theme, config);
    };
    std::optional<TextBlock> title = measure(q.title);
    float title_height = title ? title->height + padding : 0.0f;
    std::optional<TextBlock> x_left = measure(q.x_axis_left), x_right = measure(q.x_axis_right), y_bottom = measure(q.y_axis_bottom),
                             y_top = measure(q.y_axis_top);
    QuadrantLayout ql;
    for (int i = 0; i < 4; ++i) ql.quadrant_labels[i] = measure(q.quadrant_labels[i]);

    float y_axis_label_width = fmax_(y_bottom ? y_bottom->width : 0.0f, y_top ? y_top->width : 0.0f);
    float y_axis_width = y_axis_label_width > 0.0f ? y_axis_label_width + padding : padding;
    float x_axis_height = x_left ? x_left->height + padding : padding;
    float grid_x = y_axis_width + padding;
    float grid_y = title_height + padding;

    static const char* palette[] = {"#6366f1", "#f59e0b", "#10b981", "#ef4444", "#8b5cf6", "#06b6d4"};
    for (size_t i = 0; i < q.points.size(); ++i) {
        float x = finite_unit_interval(q.points[i].x), y = finite_unit_interval(q.points[i].y);
        QuadrantPointLayout p;
        p.label = measure_label(q.points[i].label, theme, config);
        p.x = grid_x + x * grid_size;
        p.y = grid_y + (1.0f - y) * grid_size;
        p.color = palette[i % 6];
        ql.points.push_back(std::move(p));
    }
    float width = grid_x + grid_size + padding * 2.0f;
    float height = grid_y + grid_size + x_axis_height + padding;
    ql.title = title;
    ql.title_y = title_height / 2.0f;
    ql.x_axis_left = x_left;
    ql.x_axis_right = x_right;
    ql.y_axis_bottom = y_bottom;
    ql.y_axis_top = y_top;
    ql.grid_x = grid_x;
    ql.grid_y = grid_y;
    ql.grid_width = grid_size;
    ql.grid_height = grid_size;
    Layout layout;
    layout.kind = graph.kind;
    layout.width = width;
    layout.height = height;
    layout.quadrant = std::move(ql);
    return layout;
}

std::string render_quadrant_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config) {
    using svg::F;
    using svg::F2;
    const QuadrantLayout& ql = *layout.quadrant;
    float width = fmax_(layout.width, 1.0f), height = fmax_(layout.height, 1.0f);
    std::string s = svg::document_start(width, height, 0.0f, 0.0f, width, height, false, false, theme);
    float gx = ql.grid_x, gy = ql.grid_y, w = ql.grid_width, h = ql.grid_height, half_w = w / 2.0f, half_h = h / 2.0f;
    static const char* q_colors[] = {"#ECECFF", "#f1f1ff", "#f6f6ff", "#fbfbff"};
    auto rect = [&](float x, float y, float rw, float rh, const char* color) {
        s += "<rect x=\"" + F2(x) + "\" y=\"" + F2(y) + "\" width=\"" + F2(rw) + "\" height=\"" + F2(rh) + "\" fill=\"" + color + "\"/>";
    };
    rect(gx + half_w, gy, half_w, half_h, q_colors[0]);
    rect(gx, gy, half_w, half_h, q_colors[1]);
    rect(gx, gy + half_h, half_w, half_h, q_colors[2]);
    rect(gx + half_w, gy + half_h, half_w, half_h, q_colors[3]);
    s += "<rect x=\"" + F2(gx) + "\" y=\"" + F2(gy) + "\" width=\"" + F2(w) + "\" height=\"" + F2(h) + "\" fill=\"none\" stroke=\"#c7c7f1\" stroke-width=\"2\"/>";
    s += "<line x1=\"" + F2(gx + half_w) + "\" y1=\"" + F2(gy) + "\" x2=\"" + F2(gx + half_w) + "\" y2=\"" + F2(gy + h) + "\" stroke=\"#c7c7f1\" stroke-width=\"1\"/>";
    s += "<line x1=\"" + F2(gx) + "\" y1=\"" + F2(gy + half_h) + "\" x2=\"" + F2(gx + w) + "\" y2=\"" + F2(gy + half_h) + "\" stroke=\"#c7c7f1\" stroke-width=\"1\"/>";
    auto text = [&](float x, float y, const TextBlock& label, const std::string& color) {
        s += svg::text_block(x, y, label, theme, config, theme.font_size, "middle", color, false);
    };
    if (ql.title) text(gx + half_w, ql.title_y, *ql.title, theme.primary_text_color);
    float label_positions[4][2] = {{gx + half_w + half_w / 2.0f, gy + 15.0f},
                                   {gx + half_w / 2.0f, gy + 15.0f},
                                   {gx + half_w / 2.0f, gy + half_h + 15.0f},
                                   {gx + half_w + half_w / 2.0f, gy + half_h + 15.0f}};
    for (int i = 0; i < 4; ++i)
        if (ql.quadrant_labels[i]) text(label_positions[i][0], label_positions[i][1], *ql.quadrant_labels[i], "#131300");
    if (ql.x_axis_left) text(gx + half_w / 2.0f, gy + h + 20.0f, *ql.x_axis_left, "#131300");
    if (ql.x_axis_right) text(gx + half_w + half_w / 2.0f, gy + h + 20.0f, *ql.x_axis_right, "#131300");
    auto vertical_axis = [&](const TextBlock& label, float axis_y) {
        float axis_x = gx - theme.font_size * 2.2f;
        s += "<text x=\"" + F2(axis_x) + "\" y=\"" + F2(axis_y) + "\" text-anchor=\"end\" dominant-baseline=\"middle\" font-family=\"" +
             svg::normalize_font_family(theme.font_family) + "\" font-size=\"" + F(theme.font_size) + "\" fill=\"#131300\"><tspan>" +
             (label.lines.empty() ? std::string() : label.lines[0]) + "</tspan></text>";
    };
    if (ql.y_axis_bottom) vertical_axis(*ql.y_axis_bottom, gy + half_h + half_h / 2.0f);
    if (ql.y_axis_top) vertical_axis(*ql.y_axis_top, gy + half_h / 2.0f);
    for (const auto& point : ql.points) {
        s += "<circle cx=\"" + F2(point.x) + "\" cy=\"" + F2(point.y) + "\" r=\"5\" fill=\"" + point.color + "\" stroke=\"" + point.color + "\" stroke-width=\"1\"/>";
        text(point.x, point.y + 15.0f, point.label, "#131300");
    }
    s += "</svg>";
    return s;
}

}  // namespace mermaid
