// Mermaid port: xy charts. Mirrors parser.rs parse_xy_chart_diagram, layout/xychart.rs and render.rs render_xychart.
#include <algorithm>
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
float fmin_(float a, float b) { return a < b ? a : (a > b ? b : a); }

std::string trim_brackets(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == '[' || s[a] == ']')) ++a;
    while (b > a && (s[b - 1] == '[' || s[b - 1] == ']')) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> parse_xy_axis_categories(const std::string& rest) {
    std::string trimmed = trim(rest);
    if (trimmed.empty()) return {};
    std::string categories = trimmed;
    size_t open = trimmed.find('[');
    if (open != std::string::npos) {
        size_t close = trimmed.rfind(']');
        if (close != std::string::npos && close > open) categories = trimmed.substr(open + 1, close - open - 1);
    }
    std::vector<std::string> out;
    for (const std::string& value : split_args(categories)) {
        std::string cleaned = trim(trim_brackets(trim(value)));
        std::string v = pp::strip_quotes(cleaned);
        if (!v.empty()) out.push_back(v);
    }
    return out;
}

std::optional<XYSeries> parse_xy_series_line_v2(const std::string& line) {
    std::string lower = to_ascii_lower(line);
    XYSeriesKind kind;
    std::string rest;
    if (starts_with(lower, "bar")) { kind = XYSeriesKind::Bar; rest = trim(line.substr(3)); }
    else if (starts_with(lower, "line")) { kind = XYSeriesKind::Line; rest = trim(line.substr(4)); }
    else return std::nullopt;
    std::optional<std::string> label;
    std::string values_str = rest;
    size_t bracket = rest.find('[');
    if (bracket != std::string::npos) {
        std::string label_part = trim(rest.substr(0, bracket));
        if (!label_part.empty()) label = pp::strip_quotes(label_part);
        values_str = rest.substr(bracket);
    }
    std::string inner = trim_brackets(values_str);
    std::vector<float> values;
    size_t pos = 0;
    while (true) {
        size_t j = inner.find(',', pos);
        std::string piece = trim(inner.substr(pos, j == std::string::npos ? std::string::npos : j - pos));
        float v;
        if (parse_rust_f32(piece, v)) values.push_back(v);
        if (j == std::string::npos) break;
        pos = j + 1;
    }
    if (values.empty()) return std::nullopt;
    return XYSeries{kind, label, values};
}

float finite_chart_value(float v) { return std::isfinite(v) ? v : 0.0f; }
}  // namespace

Graph parse_xychart_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::XYChart;
    graph.direction = Direction::LeftRight;
    XYChartData& xy = graph.xychart;
    for (const std::string& raw_line : pp::preprocess_input(input)) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        std::string lower = to_ascii_lower(line);
        if (starts_with(lower, "xychart")) continue;
        if (starts_with(lower, "title")) {
            std::string rest = trim(line.substr(5));
            if (!rest.empty()) xy.title = pp::strip_quotes(rest);
            continue;
        }
        if (starts_with(lower, "x-axis")) {
            std::string rest = trim(line.substr(6));
            size_t bracket = rest.find('[');
            if (bracket != std::string::npos) {
                std::string label_part = trim(rest.substr(0, bracket));
                if (!label_part.empty()) xy.x_axis_label = pp::strip_quotes(label_part);
                xy.x_axis_categories = parse_xy_axis_categories(rest.substr(bracket));
            } else {
                xy.x_axis_categories = parse_xy_axis_categories(rest);
            }
            continue;
        }
        if (starts_with(lower, "y-axis")) {
            std::string rest = trim(line.substr(6));
            if (!rest.empty()) {
                std::string rest_lower = to_ascii_lower(rest);
                size_t arrow = rest_lower.find("-->");
                if (arrow != std::string::npos) {
                    std::string before_arrow = trim(rest.substr(0, arrow));
                    std::string after_arrow = trim(rest.substr(arrow + 3));
                    auto words = split_whitespace(before_arrow);
                    std::string min_str = words.empty() ? "0" : words.back();
                    float v;
                    if (parse_rust_f32(min_str, v)) xy.y_axis_min = v;
                    if (parse_rust_f32(after_arrow, v)) xy.y_axis_max = v;
                    std::string label_part = before_arrow;  // trim_end_matches(min_str)
                    while (!min_str.empty() && ends_with(label_part, min_str)) label_part.resize(label_part.size() - min_str.size());
                    label_part = trim(label_part);
                    if (!label_part.empty()) xy.y_axis_label = pp::strip_quotes(label_part);
                } else {
                    xy.y_axis_label = pp::strip_quotes(rest);
                }
            }
            continue;
        }
        if (auto series = parse_xy_series_line_v2(line)) xy.series.push_back(std::move(*series));
    }
    return graph;
}

Layout compute_xychart_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    const XYChartData& data = graph.xychart;
    float font_size = theme.font_size;
    float padding = 40.0f, y_axis_width = 60.0f, x_axis_height = 40.0f;
    float title_height = data.title ? 30.0f : 0.0f;
    float plot_width = 400.0f, plot_height = 250.0f;
    float width = padding * 2.0f + y_axis_width + plot_width;
    float height = padding * 2.0f + title_height + plot_height + x_axis_height;
    float plot_x = padding + y_axis_width, plot_y = padding + title_height;

    std::vector<float> all_values;
    for (const auto& s : data.series)
        for (float v : s.values) all_values.push_back(finite_chart_value(v));
    float min_val, max_val;
    if (data.y_axis_min && std::isfinite(*data.y_axis_min)) min_val = *data.y_axis_min;
    else {
        float m = 0.0f;
        for (float v : all_values) m = fmin_(m, v);
        min_val = fmin_(m, 0.0f);
    }
    if (data.y_axis_max && std::isfinite(*data.y_axis_max)) max_val = *data.y_axis_max;
    else {
        float m = 0.0f;
        for (float v : all_values) m = fmax_(m, v);
        max_val = m;
    }
    float range = fmax_(max_val - min_val, 1.0f);

    size_t max_series_len = 0;
    for (const auto& s : data.series) max_series_len = std::max(max_series_len, s.values.size());
    size_t num_categories = std::max<size_t>(std::max(data.x_axis_categories.size(), max_series_len), 1);
    float bar_group_width = plot_width / static_cast<float>(num_categories);
    float bar_padding = bar_group_width * 0.1f;
    size_t bar_count = 0;
    for (const auto& s : data.series)
        if (s.kind == XYSeriesKind::Bar) ++bar_count;
    bar_count = std::max<size_t>(bar_count, 1);
    float bar_width = (bar_group_width - bar_padding * 2.0f) / static_cast<float>(bar_count);

    static const char* colors[] = {"#4e79a7", "#f28e2c", "#e15759", "#76b7b2", "#59a14f", "#edc949", "#af7aa1", "#ff9da7"};
    XYChartLayout xl;
    size_t bar_series_idx = 0;
    for (size_t series_idx = 0; series_idx < data.series.size(); ++series_idx) {
        const XYSeries& series = data.series[series_idx];
        std::string color = colors[series_idx % 8];
        if (series.kind == XYSeriesKind::Bar) {
            for (size_t i = 0; i < series.values.size(); ++i) {
                float value = finite_chart_value(series.values[i]);
                float bar_height = ((value - min_val) / range) * plot_height;
                float x = plot_x + static_cast<float>(i) * bar_group_width + bar_padding + static_cast<float>(bar_series_idx) * bar_width;
                float y = plot_y + plot_height - bar_height;
                xl.bars.push_back({x, y, bar_width, bar_height, value, color});
            }
            ++bar_series_idx;
        } else {
            XYChartLineLayout line;
            for (size_t i = 0; i < series.values.size(); ++i) {
                float value = finite_chart_value(series.values[i]);
                float x = plot_x + static_cast<float>(i) * bar_group_width + bar_group_width / 2.0f;
                float y = plot_y + plot_height - ((value - min_val) / range) * plot_height;
                line.points.push_back({x, y});
            }
            line.color = color;
            xl.lines.push_back(std::move(line));
        }
    }
    for (size_t i = 0; i < data.x_axis_categories.size(); ++i)
        xl.x_axis_categories.push_back({data.x_axis_categories[i], plot_x + static_cast<float>(i) * bar_group_width + bar_group_width / 2.0f});
    const int num_ticks = 5;
    for (int i = 0; i <= num_ticks; ++i) {
        float value = min_val + (static_cast<float>(i) / static_cast<float>(num_ticks)) * range;
        float y = plot_y + plot_height - (static_cast<float>(i) / static_cast<float>(num_ticks)) * plot_height;
        xl.y_axis_ticks.push_back({fmt_fixed(value, 0), y});
    }
    if (data.title) xl.title = measure_label(*data.title, theme, config);
    if (data.x_axis_label) xl.x_axis_label = measure_label(*data.x_axis_label, theme, config);
    if (data.y_axis_label) xl.y_axis_label = measure_label(*data.y_axis_label, theme, config);
    xl.title_y = padding + font_size;
    xl.x_axis_label_y = plot_y + plot_height + x_axis_height - 10.0f;
    xl.y_axis_label_x = padding;
    xl.plot_x = plot_x;
    xl.plot_y = plot_y;
    xl.plot_width = plot_width;
    xl.plot_height = plot_height;
    xl.width = width;
    xl.height = height;
    Layout layout;
    layout.kind = graph.kind;
    layout.width = width;
    layout.height = height;
    layout.xychart = std::move(xl);
    return layout;
}

std::string render_xychart_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config) {
    using svg::escape_xml;
    using svg::F2;
    using svg::F;
    const XYChartLayout& xl = *layout.xychart;
    float width = fmax_(layout.width, 1.0f), height = fmax_(layout.height, 1.0f);
    std::string s = svg::document_start(width, height, 0.0f, 0.0f, width, height, false, false, theme);
    s += "<rect x=\"0\" y=\"0\" width=\"" + F2(xl.width) + "\" height=\"" + F2(xl.height) + "\" fill=\"" + theme.background + "\"/>";
    if (xl.title) s += svg::text_block(xl.width / 2.0f, xl.title_y, *xl.title, theme, config, theme.font_size, "middle", theme.primary_text_color, false);
    s += "<rect x=\"" + F2(xl.plot_x) + "\" y=\"" + F2(xl.plot_y) + "\" width=\"" + F2(xl.plot_width) + "\" height=\"" + F2(xl.plot_height) +
         "\" fill=\"none\" stroke=\"" + theme.line_color + "\" stroke-width=\"1\"/>";
    auto fam = svg::normalize_font_family(theme.font_family);
    for (const auto& [label, y] : xl.y_axis_ticks) {
        s += "<line x1=\"" + F2(xl.plot_x) + "\" y1=\"" + F2(y) + "\" x2=\"" + F2(xl.plot_x + xl.plot_width) + "\" y2=\"" + F2(y) +
             "\" stroke=\"#ccc\" stroke-width=\"1\" stroke-dasharray=\"2,2\"/>";
        s += "<text x=\"" + F2(xl.plot_x - 5.0f) + "\" y=\"" + F2(y + theme.font_size / 3.0f) + "\" text-anchor=\"end\" font-family=\"" + fam +
             "\" font-size=\"" + fmt_fixed(theme.font_size * 0.8f, 1) + "\" fill=\"" + theme.primary_text_color + "\">" + escape_xml(label) + "</text>";
    }
    for (const auto& [label, x] : xl.x_axis_categories)
        s += "<text x=\"" + F2(x) + "\" y=\"" + F2(xl.plot_y + xl.plot_height + 20.0f) + "\" text-anchor=\"middle\" font-family=\"" + fam +
             "\" font-size=\"" + fmt_fixed(theme.font_size * 0.9f, 1) + "\" fill=\"" + theme.primary_text_color + "\">" + escape_xml(label) + "</text>";
    if (xl.y_axis_label) {
        std::string joined;
        for (size_t i = 0; i < xl.y_axis_label->lines.size(); ++i) joined += (i ? " " : "") + xl.y_axis_label->lines[i];
        float cy = xl.plot_y + xl.plot_height / 2.0f;
        s += "<text x=\"" + F2(xl.y_axis_label_x) + "\" y=\"" + F2(cy) + "\" text-anchor=\"middle\" font-family=\"" + fam + "\" font-size=\"" +
             fmt_fixed(theme.font_size, 1) + "\" fill=\"" + theme.primary_text_color + "\" transform=\"rotate(-90, " + F2(xl.y_axis_label_x) + ", " + F2(cy) +
             ")\">" + escape_xml(joined) + "</text>";
    }
    for (const auto& bar : xl.bars)
        s += "<rect x=\"" + F2(bar.x) + "\" y=\"" + F2(bar.y) + "\" width=\"" + F2(bar.width) + "\" height=\"" + F2(bar.height) + "\" fill=\"" +
             escape_xml(bar.color) + "\" stroke=\"none\"/>";
    for (const auto& line : xl.lines) {
        if (line.points.size() < 2) continue;
        std::string path;
        for (size_t i = 0; i < line.points.size(); ++i)
            path += (i == 0 ? "M " : " L ") + F2(line.points[i].first) + "," + F2(line.points[i].second);
        s += "<path d=\"" + path + "\" fill=\"none\" stroke=\"" + escape_xml(line.color) + "\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>";
        for (const auto& [x, y] : line.points)
            s += "<circle cx=\"" + F2(x) + "\" cy=\"" + F2(y) + "\" r=\"4\" fill=\"" + escape_xml(line.color) + "\" stroke=\"white\" stroke-width=\"1\"/>";
    }
    s += "</svg>";
    return s;
}

}  // namespace mermaid
