// Mermaid port: pie charts. Mirrors parser.rs parse_pie_diagram, layout/pie.rs and render.rs render_pie.
#include <algorithm>
#include <cmath>
#include <map>

#include "kinds.h"
#include "numfmt.h"
#include "parser_internal.h"
#include "strutil.h"
#include "svg_common.h"
#include "text.h"

namespace mermaid {

namespace {

constexpr float kPi = 3.14159265358979323846f;  // std::f32::consts::PI
float fmax_(float a, float b) { return a > b ? a : (a < b ? b : a); }

// ---- layout/pie.rs helpers shared by layout and writer ----
float pie_outside_label_bump(float font_size, float radius) { return fmax_(font_size * 1.6f, radius * 0.18f); }
float pie_outside_label_pad_x(float font_size) { return fmax_(font_size * 0.35f, 4.0f); }
bool pie_label_is_outside(float arc_len, float percent_width, float span) { return arc_len < percent_width * 1.35f || span < 0.4f; }
float pie_outside_label_extent(float radius, float font_size, float label_width) {
    return radius + pie_outside_label_bump(font_size, radius) + label_width + pie_outside_label_pad_x(font_size);
}
float sanitize_pie_value(float v) { return std::isfinite(v) ? fmax_(v, 0.0f) : 0.0f; }

std::string format_pie_value(float value) {
    float rounded = std::round(value * 100.0f) / 100.0f;
    if (std::fabs(rounded - std::round(rounded)) < 0.001f) return fmt_fixed(rounded, 0);
    return fmt_fixed(rounded, 2);
}

std::optional<std::pair<std::string, float>> parse_pie_slice_line(const std::string& line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return std::nullopt;
    std::string label = pp::strip_quotes(trim(line.substr(0, colon)));
    if (label.empty()) return std::nullopt;
    std::string value_str = trim(line.substr(colon + 1));
    if (value_str.empty()) return std::nullopt;
    float value;
    if (!parse_rust_f32(value_str, value)) return std::nullopt;
    return std::make_pair(label, value);
}

}  // namespace

Graph parse_pie_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::Pie;
    for (const std::string& raw_line : pp::preprocess_input(input)) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        std::string lower = to_ascii_lower(line);
        if (starts_with(lower, "pie")) {
            if (lower.find("showdata") != std::string::npos) graph.pie_show_data = true;
            size_t title_pos = lower.find("title");
            if (title_pos != std::string::npos) {
                std::string title = trim(line.substr(title_pos + 5));
                if (!title.empty()) graph.pie_title = title;
            }
            continue;
        }
        if (starts_with(lower, "showdata")) {
            graph.pie_show_data = true;
            continue;
        }
        if (starts_with(lower, "title")) {
            std::string title = trim(line.substr(5));
            if (!title.empty()) graph.pie_title = title;
            continue;
        }
        if (auto slice = parse_pie_slice_line(line)) graph.pie_slices.push_back({slice->first, slice->second});
    }
    return graph;
}

Layout compute_pie_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    const PieConfig& pie_cfg = config.pie;
    std::optional<TextBlock> title_block;
    if (graph.pie_title) title_block = measure_label_with_font_size(*graph.pie_title, theme.pie_title_text_size, config, false);

    const auto& palette = theme.pie_colors;
    float total = 0.0f;
    for (const auto& s : graph.pie_slices) total += sanitize_pie_value(s.value);
    float fallback_total = static_cast<float>(std::max<size_t>(graph.pie_slices.size(), 1));
    if (!(total > 0.0f)) total = fallback_total;

    struct Datum {
        size_t index;
        std::string label;
        float value;
    };
    std::vector<Datum> filtered;
    for (size_t idx = 0; idx < graph.pie_slices.size(); ++idx) {
        float value = sanitize_pie_value(graph.pie_slices[idx].value);
        float percent = total > 0.0f ? value / total * 100.0f : 0.0f;
        if (percent >= pie_cfg.min_percent) filtered.push_back({idx, graph.pie_slices[idx].label, value});
    }
    std::stable_sort(filtered.begin(), filtered.end(), [](const Datum& a, const Datum& b) {
        if (a.value != b.value) return a.value > b.value;
        return a.index < b.index;
    });

    std::map<std::string, std::string> color_map;
    size_t color_index = 0;
    auto resolve_color = [&](const std::string& label) -> std::string {
        if (auto it = color_map.find(label); it != color_map.end()) return it->second;
        std::string color = palette[color_index % palette.size()];
        ++color_index;
        color_map[label] = color;
        return color;
    };

    PieData pie;
    float angle = 0.0f;
    for (const Datum& d : filtered) {
        float span = total > 0.0f ? d.value / total * kPi * 2.0f : kPi * 2.0f / fallback_total;
        PieSliceLayout sl;
        sl.label = measure_label_with_font_size(d.label, theme.pie_section_text_size, config, false);
        sl.color = resolve_color(d.label);
        sl.value = d.value;
        sl.start_angle = angle;
        sl.end_angle = angle + span;
        pie.slices.push_back(std::move(sl));
        angle += span;
    }

    float legend_width = 0.0f;
    std::vector<std::pair<TextBlock, std::string>> legend_items;
    for (const auto& slice : graph.pie_slices) {
        std::string value_text = format_pie_value(sanitize_pie_value(slice.value));
        std::string label_text = graph.pie_show_data ? slice.label + " [" + value_text + "]" : slice.label;
        TextBlock label = measure_label_with_font_size(label_text, theme.pie_legend_text_size, config, false);
        legend_width = fmax_(legend_width, label.width);
        legend_items.push_back({std::move(label), resolve_color(slice.label)});
    }

    float legend_text_height = theme.pie_legend_text_size * 1.25f;
    float legend_item_height = fmax_(pie_cfg.legend_rect_size + pie_cfg.legend_spacing, legend_text_height);
    float legend_offset = legend_item_height * static_cast<float>(legend_items.size()) / 2.0f;

    float height = fmax_(pie_cfg.height, 1.0f);
    float pie_width = height;
    float radius = fmax_(std::fmin(pie_width, height) / 2.0f - pie_cfg.margin, 1.0f);
    float center_x = pie_width / 2.0f;
    float center_y = height / 2.0f;
    bool suppress_outside_labels = graph.pie_slices.size() >= 4;
    float right_outside_extent = 0.0f, left_outside_extent = 0.0f;
    if (!suppress_outside_labels) {
        for (const auto& slice : pie.slices) {
            float span = std::fabs(slice.end_angle - slice.start_angle);
            if (span <= 0.0001f || total <= 0.0f) continue;
            std::string percent_text = fmt_fixed(slice.value / total * 100.0f, 0) + "%";
            float percent_width = measure_text_width(percent_text, theme.pie_section_text_size);
            float arc_len = radius * span;
            if (!pie_label_is_outside(arc_len, percent_width, span)) continue;
            float extent = pie_outside_label_extent(radius, theme.pie_section_text_size, slice.label.width);
            float mid_angle = (slice.start_angle + slice.end_angle) / 2.0f;
            if (std::cos(mid_angle) >= 0.0f) right_outside_extent = fmax_(right_outside_extent, extent);
            else left_outside_extent = fmax_(left_outside_extent, extent);
        }
    }

    float title_half_width = title_block ? title_block->width / 2.0f : 0.0f;
    float left_needed = fmax_(left_outside_extent + pie_cfg.margin * 0.35f, title_half_width + pie_cfg.margin * 0.25f);
    float left_shift = fmax_(left_needed - center_x, 0.0f);
    center_x += left_shift;

    float legend_x = center_x + fmax_(radius + pie_cfg.margin * 0.6f, right_outside_extent + pie_cfg.margin * 0.35f);
    for (size_t idx = 0; idx < legend_items.size(); ++idx) {
        float vertical = static_cast<float>(idx) * legend_item_height - legend_offset;
        PieLegendItem item;
        item.x = legend_x;
        item.y = center_y + vertical;
        item.label = std::move(legend_items[idx].first);
        item.color = std::move(legend_items[idx].second);
        item.marker_size = pie_cfg.legend_rect_size;
        item.value = sanitize_pie_value(graph.pie_slices[idx].value);
        pie.legend.push_back(std::move(item));
    }

    float width = fmax_(legend_x + pie_cfg.legend_rect_size + pie_cfg.legend_spacing + legend_width + pie_cfg.margin * 0.4f,
                        center_x + title_half_width + pie_cfg.margin * 0.25f);
    if (title_block) pie.title = PieTitleLayout{center_x, center_y - (height - 50.0f) / 2.0f, *title_block};
    pie.center = {center_x, center_y};
    pie.radius = radius;

    Layout layout;
    layout.kind = graph.kind;
    layout.width = fmax_(width, 200.0f);
    layout.height = fmax_(height, 1.0f);
    layout.pie = std::move(pie);
    return layout;
}

namespace {

std::string pie_slice_path(float cx, float cy, float radius, float start_angle, float end_angle) {
    using svg::F2;
    float sx = cx + radius * std::cos(start_angle), sy = cy + radius * std::sin(start_angle);
    float ex = cx + radius * std::cos(end_angle), ey = cy + radius * std::sin(end_angle);
    int large_arc = std::fabs(end_angle - start_angle) > kPi ? 1 : 0;
    return "M " + F2(cx) + " " + F2(cy) + " L " + F2(sx) + " " + F2(sy) + " A " + F2(radius) + " " + F2(radius) + " 0 " +
           std::to_string(large_arc) + " 1 " + F2(ex) + " " + F2(ey) + " Z";
}

std::string render_pie_body(const PieData& pie, const Theme& theme, const LayoutConfig& config) {
    using svg::F;
    using svg::F2;
    using svg::F3;
    using svg::escape_xml;
    std::string out;
    auto [cx, cy] = pie.center;
    float radius = pie.radius;
    if (radius <= 0.0f) return out;
    const PieConfig& pie_cfg = config.pie;
    float total = 0.0f;
    for (const auto& s : pie.legend) total += fmax_(s.value, 0.0f);
    if (total <= 0.0f)
        for (const auto& s : pie.slices) total += fmax_(s.value, 0.0f);

    const std::string& slice_stroke = theme.background;
    float slice_stroke_width = fmax_(theme.pie_stroke_width, 1.2f);
    for (const auto& slice : pie.slices) {
        float span = std::fabs(slice.end_angle - slice.start_angle);
        if (span <= 0.0001f) continue;
        if (span >= kPi * 2.0f - 0.001f) {
            out += "<circle cx=\"" + F2(cx) + "\" cy=\"" + F2(cy) + "\" r=\"" + F2(radius) + "\" fill=\"" + escape_xml(slice.color) +
                   "\" stroke=\"" + escape_xml(slice_stroke) + "\" stroke-width=\"" + F3(slice_stroke_width) + "\" opacity=\"" +
                   F3(theme.pie_opacity) + "\"/>";
            continue;
        }
        std::string path = pie_slice_path(cx, cy, radius, slice.start_angle, slice.end_angle);
        out += "<path d=\"" + escape_xml(path) + "\" fill=\"" + escape_xml(slice.color) + "\" stroke=\"" + escape_xml(slice_stroke) +
               "\" stroke-width=\"" + F3(slice_stroke_width) + "\" opacity=\"" + F3(theme.pie_opacity) + "\"/>";
    }
    if (theme.pie_outer_stroke_width > 0.0f) {
        float outer_radius = radius + theme.pie_outer_stroke_width / 2.0f;
        out += "<circle cx=\"" + F2(cx) + "\" cy=\"" + F2(cy) + "\" r=\"" + F2(outer_radius) + "\" fill=\"none\" stroke=\"" +
               escape_xml(theme.pie_outer_stroke_color) + "\" stroke-width=\"" + F3(theme.pie_outer_stroke_width) + "\"/>";
    }

    struct Label {
        std::string text;
        float font_size;
        bool outside;
        int side;
        float x, y, edge_x, edge_y;
        std::string line_color;
    };
    std::vector<Label> labels;
    bool suppress_outside_labels = pie.legend.size() >= 4;
    for (const auto& slice : pie.slices) {
        float span = std::fabs(slice.end_angle - slice.start_angle);
        if (span <= 0.0001f || total <= 0.0f) continue;
        float percent = slice.value / total * 100.0f;
        if (percent < pie_cfg.min_percent) continue;
        std::string percent_text = fmt_fixed(percent, 0) + "%";
        float mid_angle = (slice.start_angle + slice.end_angle) / 2.0f;
        float font_size = theme.pie_section_text_size;
        float arc_len = radius * span;
        float percent_width = measure_text_width(percent_text, font_size);
        bool outside = !suppress_outside_labels && pie_label_is_outside(arc_len, percent_width, span);
        std::string label_text;
        if (outside) {
            for (size_t i = 0; i < slice.label.lines.size(); ++i) label_text += (i ? " " : "") + slice.label.lines[i];
        } else {
            label_text = percent_text;
        }
        float edge_x = cx + radius * std::cos(mid_angle), edge_y = cy + radius * std::sin(mid_angle);
        float bump = pie_outside_label_bump(font_size, radius);
        float label_x, label_y;
        if (outside) {
            label_x = cx + (radius + bump) * std::cos(mid_angle);
            label_y = cy + (radius + bump) * std::sin(mid_angle);
        } else {
            float label_radius = radius * pie_cfg.text_position;
            label_x = cx + label_radius * std::cos(mid_angle);
            label_y = cy + label_radius * std::sin(mid_angle);
        }
        labels.push_back({label_text, font_size, outside, std::cos(mid_angle) >= 0.0f ? 1 : -1, label_x, label_y, edge_x, edge_y, slice.color});
    }

    float min_y = cy - radius * 1.1f, max_y = cy + radius * 1.1f;
    float min_gap = theme.pie_section_text_size * 1.2f;
    std::vector<size_t> left, right;
    for (size_t i = 0; i < labels.size(); ++i)
        if (labels[i].outside) (labels[i].side >= 0 ? right : left).push_back(i);
    auto distribute = [&](std::vector<size_t>& indices) {
        std::stable_sort(indices.begin(), indices.end(), [&](size_t a, size_t b) { return labels[a].y < labels[b].y; });
        float prev = min_y - min_gap;
        for (size_t idx : indices) {
            float y = fmax_(labels[idx].y, prev + min_gap);
            labels[idx].y = y;
            prev = y;
        }
        if (!indices.empty()) {
            float overflow = labels[indices.back()].y - max_y;
            if (overflow > 0.0f)
                for (size_t idx : indices) labels[idx].y -= overflow;
        }
        if (!indices.empty()) {
            float underflow = min_y - labels[indices.front()].y;
            if (underflow > 0.0f)
                for (size_t idx : indices) labels[idx].y += underflow;
        }
    };
    distribute(left);
    distribute(right);

    for (const Label& label : labels) {
        std::string anchor = "middle";
        float label_x = label.x;
        if (label.outside) {
            float bump = pie_outside_label_bump(label.font_size, radius);
            if (label.side >= 0) {
                label_x = cx + radius + bump;
                anchor = "start";
            } else {
                label_x = cx - radius - bump;
                anchor = "end";
            }
            float elbow_x = label.side >= 0 ? label_x - 6.0f : label_x + 6.0f;
            out += "<path d=\"M " + F2(label.edge_x) + "," + F2(label.edge_y) + " L " + F2(elbow_x) + "," + F2(label.y) + " L " + F2(label_x) +
                   "," + F2(label.y) + "\" fill=\"none\" stroke=\"" + escape_xml(label.line_color) + "\" stroke-width=\"1\"/>";
            float label_width = measure_text_width(label.text, label.font_size);
            float pad_x = pie_outside_label_pad_x(label.font_size);
            float pad_y = fmax_(label.font_size * 0.25f, 2.5f);
            float rect_w = label_width + pad_x * 2.0f, rect_h = label.font_size + pad_y * 2.0f;
            float rect_x = label.side >= 0 ? label_x - pad_x : label_x - rect_w + pad_x;
            float rect_y = label.y - rect_h / 2.0f;
            const std::string& bg = theme.edge_label_background == "none" ? theme.background : theme.edge_label_background;
            out += "<rect x=\"" + F2(rect_x) + "\" y=\"" + F2(rect_y) + "\" width=\"" + F2(rect_w) + "\" height=\"" + F2(rect_h) +
                   "\" rx=\"2\" ry=\"2\" fill=\"" + escape_xml(bg) + "\" stroke=\"none\"/>";
        }
        out += "<text x=\"" + F2(label_x) + "\" y=\"" + F2(label.y) + "\" text-anchor=\"" + anchor + "\" dominant-baseline=\"middle\" font-family=\"" +
               svg::normalize_font_family(theme.font_family) + "\" font-size=\"" + F(label.font_size) + "\" fill=\"" +
               escape_xml(theme.pie_section_text_color) + "\">" + label.text + "</text>";
    }

    for (const auto& item : pie.legend) {
        float rect_x = item.x, rect_y = item.y;
        out += "<rect x=\"" + F2(rect_x) + "\" y=\"" + F2(rect_y) + "\" width=\"" + F2(item.marker_size) + "\" height=\"" + F2(item.marker_size) +
               "\" fill=\"" + escape_xml(item.color) + "\" stroke=\"" + escape_xml(item.color) + "\" stroke-width=\"" + F3(theme.pie_stroke_width) + "\"/>";
        float label_x = rect_x + item.marker_size + pie_cfg.legend_spacing;
        float label_y = rect_y + item.marker_size / 2.0f;
        out += svg::text_block(label_x, label_y, item.label, theme, config, theme.pie_legend_text_size, "start", theme.pie_legend_text_color, true);
    }
    if (pie.title)
        out += svg::text_block(pie.title->x, pie.title->y, pie.title->text, theme, config, theme.pie_title_text_size, "middle",
                               theme.pie_title_text_color, true);
    return out;
}

}  // namespace

std::string render_pie_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config) {
    float width = fmax_(layout.width, 1.0f), height = fmax_(layout.height, 1.0f);
    std::string s = svg::document_start(width, height, 0.0f, 0.0f, width, height, true, false, theme);
    s += render_pie_body(*layout.pie, theme, config);
    s += "</svg>";
    return s;
}

}  // namespace mermaid
