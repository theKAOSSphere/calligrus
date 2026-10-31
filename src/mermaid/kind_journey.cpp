// Mermaid port: user journeys. Mirrors parser.rs parse_journey_diagram, layout/journey.rs and render.rs render_journey.
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <set>

#include "kinds.h"
#include "numfmt.h"
#include "parser_internal.h"
#include "strutil.h"
#include "svg_common.h"
#include "text.h"

namespace mermaid {

namespace {
float fmax_(float a, float b) { return a > b ? a : (a < b ? b : a); }

std::vector<std::string> split_char(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (true) {
        size_t j = s.find(sep, pos);
        if (j == std::string::npos) {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, j - pos));
        pos = j + 1;
    }
    return out;
}

struct JourneyTaskLine {
    std::string label;
    std::optional<float> score;
    std::vector<std::string> actors;
};

std::optional<JourneyTaskLine> parse_journey_task_line(const std::string& line) {
    std::vector<std::string> parts;
    for (const std::string& p : split_char(line, ':')) parts.push_back(trim(p));
    if (parts.size() < 2) return std::nullopt;
    JourneyTaskLine t;
    t.label = parts[0];
    if (t.label.empty()) return std::nullopt;
    float v;
    if (parse_rust_f32(parts[1], v)) t.score = v;
    if (parts.size() >= 3)
        for (const std::string& a : split_char(parts[2], ',')) {
            std::string actor = trim(a);
            if (!actor.empty()) t.actors.push_back(actor);
        }
    return t;
}
}  // namespace

Graph parse_journey_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::Journey;
    graph.direction = Direction::LeftRight;
    std::optional<size_t> current_section;
    for (const std::string& raw_line : pp::preprocess_input(input)) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        std::string lower = to_ascii_lower(line);
        if (starts_with(lower, "journey")) continue;
        if (starts_with(lower, "title")) {
            std::string title = trim(line.substr(5));
            if (!title.empty()) graph.journey_title = title;
            continue;
        }
        if (starts_with(lower, "section")) {
            std::string label = trim(line.substr(7));
            Subgraph sg;
            sg.id = "section_" + std::to_string(graph.subgraphs.size());
            sg.label = label;
            graph.subgraphs.push_back(std::move(sg));
            current_section = graph.subgraphs.size() - 1;
            continue;
        }
        if (auto task = parse_journey_task_line(line)) {
            std::string node_id = "journey_" + std::to_string(graph.nodes.size());
            std::string node_label = task->label;
            if (!task->actors.empty()) {
                node_label += "\n";
                for (size_t i = 0; i < task->actors.size(); ++i) node_label += (i ? ", " : "") + task->actors[i];
            }
            graph.ensure_node(node_id, node_label, NodeShape::Rectangle);
            if (task->score) graph.nodes[node_id].value = task->score;
            if (current_section) graph.subgraphs[*current_section].nodes.push_back(node_id);
        }
    }
    return graph;
}

namespace {

std::pair<std::string, std::vector<std::string>> parse_journey_task_label(const std::string& label) {
    std::vector<std::string> lines = split_lines(label);
    if (lines.empty()) return {"", {}};
    std::string title = trim(lines[0]);
    std::vector<std::string> actors;
    for (size_t i = 1; i < lines.size(); ++i)
        for (const std::string& part : split_char(lines[i], ',')) {
            std::string actor = trim(part);
            if (!actor.empty()) actors.push_back(actor);
        }
    return {title, actors};
}

std::string journey_score_color(float score) {
    float clamped = score < 1.0f ? 1.0f : (score > 5.0f ? 5.0f : score);
    float t = (clamped - 1.0f) / 4.0f;
    const float s0 = 248.0f, s1 = 113.0f, s2 = 113.0f, e0 = 74.0f, e1 = 222.0f, e2 = 128.0f;
    int r = static_cast<int>(std::round(s0 + (e0 - s0) * t));
    int g = static_cast<int>(std::round(s1 + (e1 - s1) * t));
    int b = static_cast<int>(std::round(s2 + (e2 - s2) * t));
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", r, g, b);
    return buf;
}

}  // namespace

Layout compute_journey_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    auto ordered_ids = [&](const std::vector<std::string>& ids) {
        std::vector<std::string> v = ids;
        std::stable_sort(v.begin(), v.end(), [&](const std::string& a, const std::string& b) {
            size_t oa = graph.node_order.count(a) ? graph.node_order.at(a) : SIZE_MAX;
            size_t ob = graph.node_order.count(b) ? graph.node_order.at(b) : SIZE_MAX;
            return oa < ob;
        });
        return v;
    };
    std::vector<std::pair<std::string, std::vector<std::string>>> section_defs;
    std::set<std::string> assigned;
    if (graph.subgraphs.empty()) {
        std::vector<std::string> keys;
        for (const auto& [id, n] : graph.nodes) keys.push_back(id);
        section_defs.push_back({"", ordered_ids(keys)});
    } else {
        for (const auto& sub : graph.subgraphs) {
            std::vector<std::string> nodes;
            for (const auto& id : sub.nodes)
                if (graph.nodes.count(id)) {
                    nodes.push_back(id);
                    assigned.insert(id);
                }
            section_defs.push_back({sub.label, nodes});
        }
        std::vector<std::string> extras;
        for (const auto& [id, n] : graph.nodes)
            if (!assigned.count(id)) extras.push_back(id);
        if (!extras.empty()) section_defs.push_back({"Other", ordered_ids(extras)});
    }

    struct TaskData {
        std::string id;
        TextBlock label;
        std::optional<float> score;
        std::vector<std::string> actors;
        size_t section_idx, order_idx;
    };
    std::vector<TaskData> tasks_data;
    std::vector<std::pair<size_t, size_t>> section_ranges;
    size_t order_idx = 0;
    for (size_t section_idx = 0; section_idx < section_defs.size(); ++section_idx) {
        size_t start_idx = order_idx;
        for (const auto& node_id : section_defs[section_idx].second) {
            auto nit = graph.nodes.find(node_id);
            if (nit == graph.nodes.end()) continue;
            const Node& node = nit->second;
            auto [title, actors] = parse_journey_task_label(node.label);
            std::string title_text = title.empty() ? node.label : title;
            tasks_data.push_back({node_id, measure_label(title_text, theme, config), node.value, actors, section_idx, order_idx});
            ++order_idx;
        }
        size_t end_idx = order_idx > 0 ? order_idx - 1 : 0;
        section_ranges.push_back({start_idx, end_idx});
    }

    std::vector<std::string> actor_order;
    std::set<std::string> actor_set;
    for (const auto& task : tasks_data)
        for (const auto& actor : task.actors)
            if (actor_set.insert(actor).second) actor_order.push_back(actor);

    float max_label_w = theme.font_size * 4.0f;
    float max_label_h = theme.font_size * config.label_line_height;
    for (const auto& task : tasks_data) {
        max_label_w = fmax_(max_label_w, task.label.width);
        max_label_h = fmax_(max_label_h, task.label.height);
    }

    float margin_x = theme.font_size * 2.0f, margin_y = theme.font_size * 2.0f;
    float task_gap_x = theme.font_size * 1.6f, section_gap_y = theme.font_size * 1.8f;
    float header_height = theme.font_size * 1.6f, card_gap_y = theme.font_size * 0.6f;
    float score_radius = fmax_(theme.font_size * 0.55f, 6.0f);
    float actor_radius = fmax_(theme.font_size * 0.35f, 4.0f);
    float actor_gap = theme.font_size * 0.5f;
    float task_pad_x = theme.font_size * 0.9f, task_pad_y = theme.font_size * 0.6f;
    float task_width = fmax_(max_label_w + task_pad_x * 2.0f, theme.font_size * 6.0f);
    float task_height = fmax_(max_label_h + task_pad_y * 2.0f, theme.font_size * 2.4f);

    std::optional<TextBlock> title_block;
    if (graph.journey_title) title_block = measure_label(*graph.journey_title, theme, config);
    float cursor_y = margin_y;
    float title_y = 0.0f;
    if (title_block) {
        title_y = cursor_y + title_block->height / 2.0f;
        cursor_y += title_block->height + theme.font_size * 0.6f;
    }

    JourneyLayout jl;
    float actor_label_y = 0.0f;
    if (!actor_order.empty()) {
        float x = margin_x;
        float legend_y = cursor_y + actor_radius;
        actor_label_y = legend_y + theme.font_size * 0.35f;
        for (size_t idx = 0; idx < actor_order.size(); ++idx) {
            TextBlock label = measure_label(actor_order[idx], theme, config);
            std::string color = theme.git_colors[idx % theme.git_colors.size()];
            jl.actors.push_back({actor_order[idx], color, x + actor_radius, legend_y, actor_radius});
            x += actor_radius * 2.0f + actor_gap + label.width + theme.font_size * 0.8f;
        }
        cursor_y += actor_radius * 2.0f + theme.font_size * 0.8f;
    }

    float content_y = cursor_y;
    bool has_actor_rows = false;
    for (const auto& t : tasks_data)
        if (!t.actors.empty()) has_actor_rows = true;
    float actor_row_height = has_actor_rows ? actor_radius * 2.0f : 0.0f;
    float actor_row_gap = has_actor_rows ? theme.font_size * 0.4f : 0.0f;
    float row_height = header_height + score_radius * 2.0f + card_gap_y + task_height + actor_row_gap + actor_row_height + theme.font_size * 0.6f;

    float content_x = margin_x;
    size_t total_tasks = tasks_data.size();
    float task_area_width = total_tasks > 0 ? static_cast<float>(total_tasks) * task_width + static_cast<float>(total_tasks - 1) * task_gap_x : 0.0f;

    for (const auto& task : tasks_data) {
        float row_top = content_y + static_cast<float>(task.section_idx) * (row_height + section_gap_y);
        float card_y = row_top + header_height + score_radius * 2.0f + card_gap_y;
        float score_y = row_top + header_height + score_radius;
        std::optional<float> actor_y;
        if (has_actor_rows) actor_y = card_y + task_height + actor_row_gap + actor_radius;
        float x = content_x + static_cast<float>(task.order_idx) * (task_width + task_gap_x);
        std::string score_color = task.score ? journey_score_color(*task.score) : theme.secondary_color;
        JourneyTaskLayout t;
        t.id = task.id;
        t.label = task.label;
        t.x = x;
        t.y = card_y;
        t.width = task_width;
        t.height = task_height;
        t.score = task.score;
        t.score_color = score_color;
        t.score_y = score_y;
        t.actors = task.actors;
        t.actor_y = actor_y;
        t.section_idx = task.section_idx;
        jl.tasks.push_back(std::move(t));
    }

    float section_pad_x = theme.font_size * 0.6f;
    for (size_t section_idx = 0; section_idx < section_defs.size(); ++section_idx) {
        auto [start_idx, end_idx] = section_ranges[section_idx];
        if (start_idx > end_idx || total_tasks == 0) continue;
        float row_top = content_y + static_cast<float>(section_idx) * (row_height + section_gap_y);
        float x = content_x + static_cast<float>(start_idx) * (task_width + task_gap_x) - section_pad_x;
        size_t span = end_idx - start_idx + 1;
        float width = static_cast<float>(span) * task_width + static_cast<float>(span - 1) * task_gap_x + section_pad_x * 2.0f;
        JourneySectionLayout s;
        s.label = measure_label(section_defs[section_idx].first, theme, config);
        s.x = x;
        s.y = row_top;
        s.width = width;
        s.height = header_height;
        s.color = theme.git_colors[section_idx % theme.git_colors.size()];
        jl.sections.push_back(std::move(s));
    }

    std::optional<std::array<float, 3>> baseline;
    if (total_tasks > 0) {
        size_t rows = section_defs.size();
        float total_rows_height = rows > 0 ? static_cast<float>(rows) * row_height + static_cast<float>(rows - 1) * section_gap_y : 0.0f;
        float y = content_y + total_rows_height + theme.font_size * 0.5f;
        baseline = std::array<float, 3>{content_x, y, content_x + task_area_width};
    }
    float width = fmax_(content_x + task_area_width + margin_x, 1.0f);
    float height = fmax_(baseline ? (*baseline)[1] + theme.font_size * 1.6f : content_y + theme.font_size * 4.0f, 1.0f);

    jl.title = title_block;
    jl.title_y = title_y;
    jl.actor_label_y = actor_label_y;
    jl.baseline = baseline;
    jl.score_radius = score_radius;
    jl.actor_radius = actor_radius;
    jl.actor_gap = actor_gap;
    jl.card_gap_y = card_gap_y;
    jl.width = width;
    jl.height = height;

    Layout layout;
    layout.kind = graph.kind;
    layout.width = width;
    layout.height = height;
    layout.journey = std::move(jl);
    // The crate also leaves a hidden "metrics" node covering the content (it shows up in layout dumps).
    NodeLayout metrics;
    metrics.id = "__journey_metrics_content";
    metrics.x = margin_x;
    metrics.y = margin_y;
    metrics.width = fmax_(width - margin_x * 2.0f, 1.0f);
    metrics.height = fmax_(height - margin_y * 2.0f, 1.0f);
    metrics.label.lines = {""};
    layout.nodes.emplace(metrics.id, std::move(metrics));
    return layout;
}

std::string render_journey_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config) {
    using svg::F2;
    const JourneyLayout& jl = *layout.journey;
    float width = fmax_(layout.width, 1.0f), height = fmax_(layout.height, 1.0f);
    std::string s = svg::document_start(width, height, 0.0f, 0.0f, width, height, false, false, theme);
    auto text = [&](float x, float y, const TextBlock& label, const std::string& color) {
        return svg::text_block(x, y, label, theme, config, theme.font_size, "middle", color, false);
    };
    if (jl.title) s += text(jl.width / 2.0f, jl.title_y, *jl.title, theme.primary_text_color);

    std::map<std::string, std::string> actor_colors;
    for (const auto& actor : jl.actors) {
        actor_colors[actor.name] = actor.color;
        s += "<circle cx=\"" + F2(actor.x) + "\" cy=\"" + F2(actor.y) + "\" r=\"" + F2(actor.radius) + "\" fill=\"" + actor.color + "\" stroke=\"" +
             theme.line_color + "\" stroke-width=\"1\"/>";
        s += svg::text_line(actor.x + actor.radius + jl.actor_gap, jl.actor_label_y, actor.name, theme, theme.font_size, theme.primary_text_color, "start");
    }
    for (const auto& section : jl.sections) {
        s += "<rect x=\"" + F2(section.x) + "\" y=\"" + F2(section.y) + "\" width=\"" + F2(section.width) + "\" height=\"" + F2(section.height) +
             "\" rx=\"8\" ry=\"8\" fill=\"" + section.color + "\" fill-opacity=\"0.18\" stroke=\"" + theme.cluster_border + "\" stroke-width=\"1\"/>";
        bool all_blank = true;
        for (const auto& l : section.label.lines)
            if (!trim_view(l).empty()) all_blank = false;
        if (!section.label.lines.empty() && !all_blank)
            s += text(section.x + section.width / 2.0f, section.y + section.height / 2.0f, section.label, theme.primary_text_color);
    }
    for (const auto& task : jl.tasks) {
        s += "<rect x=\"" + F2(task.x) + "\" y=\"" + F2(task.y) + "\" width=\"" + F2(task.width) + "\" height=\"" + F2(task.height) +
             "\" rx=\"10\" ry=\"10\" fill=\"" + theme.primary_color + "\" stroke=\"" + theme.primary_border_color + "\" stroke-width=\"1.2\"/>";
        s += text(task.x + task.width / 2.0f, task.y + task.height / 2.0f, task.label, theme.primary_text_color);
        if (task.score) {
            float score_x = task.x + jl.score_radius + theme.font_size * 0.2f;
            s += "<circle cx=\"" + F2(score_x) + "\" cy=\"" + F2(task.score_y) + "\" r=\"" + F2(jl.score_radius) + "\" fill=\"" + task.score_color +
                 "\" stroke=\"" + theme.line_color + "\" stroke-width=\"1\"/>";
            s += svg::text_line(score_x, task.score_y + theme.font_size * 0.35f, fmt_fixed(*task.score, 0), theme, theme.font_size, theme.primary_text_color, "middle");
        }
        if (task.actor_y) {
            size_t count = task.actors.size();
            if (count > 0) {
                float total_width = static_cast<float>(count) * jl.actor_radius * 2.0f + static_cast<float>(count - 1) * jl.actor_gap;
                float start_x = task.x + task.width / 2.0f - total_width / 2.0f;
                for (size_t idx = 0; idx < count; ++idx) {
                    auto it = actor_colors.find(task.actors[idx]);
                    const std::string& color = it != actor_colors.end() ? it->second : theme.secondary_color;
                    float cx = start_x + static_cast<float>(idx) * (jl.actor_radius * 2.0f + jl.actor_gap) + jl.actor_radius;
                    s += "<circle cx=\"" + F2(cx) + "\" cy=\"" + F2(*task.actor_y) + "\" r=\"" + F2(jl.actor_radius) + "\" fill=\"" + color +
                         "\" stroke=\"" + theme.line_color + "\" stroke-width=\"1\"/>";
                }
            }
        }
    }
    if (jl.baseline) {
        float x1 = (*jl.baseline)[0], y = (*jl.baseline)[1], x2 = (*jl.baseline)[2];
        s += "<line x1=\"" + F2(x1) + "\" y1=\"" + F2(y) + "\" x2=\"" + F2(x2) + "\" y2=\"" + F2(y) + "\" stroke=\"" + theme.line_color + "\" stroke-width=\"2\"/>";
        float arrow = 8.0f;
        float ax = x2 - arrow, ay1 = y - arrow * 0.6f, ay2 = y + arrow * 0.6f;
        s += "<polygon points=\"" + F2(x2) + "," + F2(y) + " " + F2(ax) + "," + F2(ay1) + " " + F2(ax) + "," + F2(ay2) + "\" fill=\"" + theme.line_color + "\"/>";
    }
    s += "</svg>";
    return s;
}

}  // namespace mermaid
