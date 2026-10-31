// Mermaid port: timelines. Mirrors parser.rs parse_timeline_diagram, layout/timeline.rs and render.rs render_timeline.
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
float clamp_(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Direction::from_timeline_token: only TD and LR.
std::optional<Direction> timeline_token(const std::string& token) {
    std::string u = to_ascii_upper(token);
    if (u == "TD") return Direction::TopDown;
    if (u == "LR") return Direction::LeftRight;
    return std::nullopt;
}

std::optional<Direction> parse_timeline_header_direction(const std::string& line) {
    auto parts = split_whitespace(line);
    if (parts.empty() || to_ascii_lower(parts[0]) != "timeline") return std::nullopt;
    if (parts.size() < 2) return std::nullopt;
    return timeline_token(parts[1]);
}
}  // namespace

Graph parse_timeline_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::Timeline;
    graph.direction = Direction::LeftRight;
    std::optional<std::string> current_section, pending_time;
    std::vector<std::string> pending_events;

    auto flush_pending = [&]() {
        if (pending_time) {
            graph.timeline.events.push_back({*pending_time, pending_events, current_section});
            pending_time.reset();
        }
        pending_events.clear();
    };

    for (const std::string& raw_line : pp::preprocess_input(input)) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        std::string lower = to_ascii_lower(line);
        if (starts_with(lower, "timeline")) {
            if (auto direction = parse_timeline_header_direction(line)) {
                graph.direction = *direction;
                graph.timeline.direction = *direction;
            }
            continue;
        }
        if (starts_with(lower, "title")) {
            std::string rest = trim(line.substr(5));
            if (!rest.empty()) graph.timeline.title = pp::strip_quotes(rest);
            continue;
        }
        if (starts_with(lower, "section")) {
            flush_pending();
            std::string label = trim(line.substr(7));
            graph.timeline.sections.push_back(label);
            current_section = label;
            continue;
        }
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string time_part = trim(line.substr(0, colon));
            std::string events_part = trim(line.substr(colon + 1));
            if (!time_part.empty()) {
                flush_pending();
                pending_time = time_part;
                size_t pos = 0;
                while (true) {
                    size_t j = events_part.find(':', pos);
                    std::string event = trim(events_part.substr(pos, j == std::string::npos ? std::string::npos : j - pos));
                    if (!event.empty()) pending_events.push_back(event);
                    if (j == std::string::npos) break;
                    pos = j + 1;
                }
            }
        }
    }
    flush_pending();
    return graph;
}

Layout compute_timeline_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    const TimelineData& data = graph.timeline;
    Direction direction = data.direction ? *data.direction : Direction::LeftRight;  // config.timeline.direction = "LR"
    float font_size = theme.font_size;
    float event_font_size = theme.font_size * 0.9f;
    float padding = 30.0f, min_event_width = 120.0f;
    bool vertical = direction == Direction::TopDown;
    float max_event_width = vertical ? 360.0f : min_event_width;
    float min_event_height = vertical ? 0.0f : 80.0f;
    float empty_event_height = 80.0f, event_spacing = 40.0f;
    float event_text_width = fmax_(max_event_width - 16.0f, 1.0f);
    float event_bottom_padding = 8.0f;
    float title_height = data.title ? 40.0f : 0.0f;

    std::optional<TextBlock> title;
    if (data.title) title = measure_label(*data.title, theme, config);

    struct Measured {
        TextBlock time;
        std::vector<TextBlock> events;
        float width, height;
    };
    float max_event_height = min_event_height;
    std::vector<Measured> measured_events;
    for (const auto& event : data.events) {
        TextBlock time_block = measure_label_with_font_size(event.time, font_size, config, false);
        std::vector<TextBlock> event_blocks;
        for (const auto& e : event.events) event_blocks.push_back(measure_label_with_max_width_px(e, event_font_size, event_text_width, config, true));
        float time_extra = static_cast<float>(time_block.lines.size() > 0 ? time_block.lines.size() - 1 : 0) * font_size * config.label_line_height;
        size_t description_line_count = 0;
        for (const auto& b : event_blocks) description_line_count += b.lines.size();
        float event_width;
        if (vertical) {
            float time_width = 0.0f;
            for (const auto& line : time_block.lines) time_width = fmax_(time_width, measure_text_width(line, font_size));
            float content_width = time_width;
            for (const auto& b : event_blocks)
                for (const auto& line : b.lines) content_width = fmax_(content_width, measure_text_width(line, event_font_size));
            event_width = clamp_(content_width + 16.0f, min_event_width, max_event_width);
        } else {
            event_width = min_event_width;
        }
        float event_height;
        if (description_line_count == 0) {
            event_height = 20.0f + time_extra + font_size * 0.25f + event_bottom_padding;
        } else {
            float event_line_height = event_font_size * config.label_line_height;
            event_height = 40.0f + time_extra + static_cast<float>(description_line_count - 1) * event_line_height + event_font_size * 0.25f + event_bottom_padding;
        }
        event_height = fmax_(event_height, min_event_height);
        max_event_height = fmax_(max_event_height, event_height);
        measured_events.push_back({std::move(time_block), std::move(event_blocks), event_width, event_height});
    }

    TimelineLayout tl;
    float width, height, line_y, line_start_x, line_end_x, line_start_y, line_end_y;
    if (vertical) {
        float content_top = padding + title_height + 30.0f;
        float line_x = padding + 20.0f;
        float event_x = line_x + 40.0f;
        float content_height;
        if (measured_events.empty()) {
            content_height = empty_event_height;
        } else {
            float sum = 0.0f;
            for (const auto& e : measured_events) sum += e.height;
            content_height = sum + static_cast<float>(measured_events.size() - 1) * event_spacing;
        }
        float content_width = min_event_width;
        for (const auto& e : measured_events) content_width = fmax_(content_width, e.width);
        width = event_x + content_width + padding;
        height = content_top + content_height + padding;
        float y = content_top;
        for (auto& m : measured_events) {
            float circle_y = y + m.height / 2.0f;
            tl.events.push_back({std::move(m.time), std::move(m.events), event_x, y, m.width, m.height, circle_y});
            y += m.height + event_spacing;
        }
        line_y = content_top;
        line_start_x = line_x;
        line_end_x = line_x;
        line_start_y = content_top;
        line_end_y = content_top + content_height;
    } else {
        line_y = padding + title_height + 60.0f;
        size_t num_events = std::max<size_t>(measured_events.size(), 1);
        float total_events_width = static_cast<float>(num_events) * min_event_width + static_cast<float>(num_events - 1) * event_spacing;
        width = padding * 2.0f + total_events_width;
        height = padding * 2.0f + title_height + max_event_height + 100.0f;
        for (size_t i = 0; i < measured_events.size(); ++i) {
            auto& m = measured_events[i];
            float x = padding + static_cast<float>(i) * (min_event_width + event_spacing);
            float y = line_y + 30.0f;
            tl.events.push_back({std::move(m.time), std::move(m.events), x, y, m.width, m.height, line_y});
        }
        line_start_x = padding;
        line_end_x = width - padding;
        line_start_y = line_y;
        line_end_y = line_y;
    }

    for (size_t i = 0; i < data.sections.size(); ++i)
        tl.sections.push_back({measure_label(data.sections[i], theme, config), padding + static_cast<float>(i) * 200.0f, padding, 180.0f, 30.0f});

    tl.title = title;
    tl.title_y = padding + font_size;
    tl.direction = direction;
    tl.line_y = line_y;
    tl.line_start_x = line_start_x;
    tl.line_end_x = line_end_x;
    tl.line_start_y = line_start_y;
    tl.line_end_y = line_end_y;
    tl.width = width;
    tl.height = height;

    Layout layout;
    layout.kind = graph.kind;
    layout.width = width;
    layout.height = height;
    layout.timeline = std::move(tl);
    NodeLayout metrics;
    metrics.id = "__timeline_metrics_content";
    metrics.x = padding;
    metrics.y = padding;
    metrics.width = fmax_(width - padding * 2.0f, 1.0f);
    metrics.height = fmax_(height - padding * 2.0f, 1.0f);
    metrics.label.lines = {""};
    layout.nodes.emplace(metrics.id, std::move(metrics));
    return layout;
}

std::string render_timeline_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config) {
    using svg::F2;
    const TimelineLayout& tl = *layout.timeline;
    float width = fmax_(layout.width, 1.0f), height = fmax_(layout.height, 1.0f);
    std::string s = svg::document_start(width, height, 0.0f, 0.0f, width, height, false, false, theme);
    // the crate paints its own background rect as well
    s += "<rect x=\"0\" y=\"0\" width=\"" + F2(tl.width) + "\" height=\"" + F2(tl.height) + "\" fill=\"" + theme.background + "\"/>";
    if (tl.title) s += svg::text_block(tl.width / 2.0f, tl.title_y, *tl.title, theme, config, theme.font_size, "middle", theme.primary_text_color, false);
    s += "<line x1=\"" + F2(tl.line_start_x) + "\" y1=\"" + F2(tl.line_start_y) + "\" x2=\"" + F2(tl.line_end_x) + "\" y2=\"" + F2(tl.line_end_y) +
         "\" stroke=\"" + theme.primary_border_color + "\" stroke-width=\"3\"/>";
    static const char* colors[] = {"#ECECFF", "#FFE6CC", "#D5E8D4", "#F8CECC", "#FFF2CC", "#E1D5E7"};
    for (size_t i = 0; i < tl.events.size(); ++i) {
        const auto& event = tl.events[i];
        const char* color = colors[i % 6];
        bool vertical = tl.direction == Direction::TopDown;
        float center_x = event.x + event.width / 2.0f;
        float circle_x = vertical ? tl.line_start_x : center_x;
        float text_x = vertical ? event.x + 8.0f : center_x;
        const char* text_anchor = vertical ? "start" : "middle";
        s += "<circle cx=\"" + F2(circle_x) + "\" cy=\"" + F2(event.circle_y) + "\" r=\"8\" fill=\"" + theme.primary_color + "\" stroke=\"" +
             theme.primary_border_color + "\" stroke-width=\"2\"/>";
        if (vertical)
            s += "<line x1=\"" + F2(circle_x + 8.0f) + "\" y1=\"" + F2(event.circle_y) + "\" x2=\"" + F2(event.x) + "\" y2=\"" + F2(event.circle_y) +
                 "\" stroke=\"" + theme.primary_border_color + "\" stroke-width=\"2\" stroke-dasharray=\"4,2\"/>";
        else
            s += "<line x1=\"" + F2(circle_x) + "\" y1=\"" + F2(event.circle_y + 8.0f) + "\" x2=\"" + F2(circle_x) + "\" y2=\"" + F2(event.y) +
                 "\" stroke=\"" + theme.primary_border_color + "\" stroke-width=\"2\" stroke-dasharray=\"4,2\"/>";
        s += "<rect x=\"" + F2(event.x) + "\" y=\"" + F2(event.y) + "\" width=\"" + F2(event.width) + "\" height=\"" + F2(event.height) +
             "\" rx=\"5\" ry=\"5\" fill=\"" + color + "\" stroke=\"" + theme.primary_border_color + "\" stroke-width=\"1\"/>";
        s += svg::text_block(text_x, event.y + 20.0f, event.time, theme, config, theme.font_size, text_anchor, theme.primary_text_color, true, std::string("bold"));
        float time_extra = static_cast<float>(event.time.lines.size() > 0 ? event.time.lines.size() - 1 : 0) * theme.font_size * config.label_line_height;
        float text_y = event.y + 40.0f + time_extra;
        float event_font_size = theme.font_size * 0.9f;
        float event_line_height = event_font_size * config.label_line_height;
        for (const auto& evt : event.events) {
            s += svg::text_block(text_x, text_y, evt, theme, config, event_font_size, text_anchor, theme.primary_text_color, true);
            text_y += static_cast<float>(evt.lines.size()) * event_line_height;
        }
    }
    s += "</svg>";
    return s;
}

}  // namespace mermaid
