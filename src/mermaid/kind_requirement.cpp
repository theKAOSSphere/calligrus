// Mermaid port: requirement diagrams. Mirrors parser.rs parse_requirement_diagram and render.rs render_requirement.
// The layout is the flowchart layout (layout.cpp, kind Requirement).
#include <cctype>
#include <cmath>
#include <map>

#include "kinds.h"
#include "layout_internal.h"
#include "numfmt.h"
#include "parser_internal.h"
#include "strutil.h"
#include "svg_common.h"
#include "text.h"

namespace mermaid {

namespace {

float fmax_(float a, float b) { return a > b ? a : (a < b ? b : a); }

std::string upper_first_lower_rest(const std::string& s) {
    std::string lower = to_ascii_lower(s);
    if (lower.empty()) return "";
    lower[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(lower[0])));
    return lower;
}

std::string requirement_kind_label(const std::string& kind) {
    std::string lower = to_ascii_lower(trim(kind));
    if (lower == "requirement") return "Requirement";
    if (lower == "functionalrequirement") return "Functional Requirement";
    if (lower == "interfacerequirement") return "Interface Requirement";
    if (lower == "performancerequirement") return "Performance Requirement";
    if (lower == "physicalrequirement") return "Physical Requirement";
    if (lower == "designconstraint") return "Design Constraint";
    if (lower == "element") return "Element";
    return upper_first_lower_rest(lower);
}

std::string requirement_title_case(const std::string& value) {
    std::string t = trim(value);
    if (t.empty()) return "";
    return upper_first_lower_rest(t);
}

std::string normalize_requirement_attr(const std::string& line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return pp::strip_quotes(trim(line));
    std::string key = to_ascii_lower(trim(line.substr(0, colon)));
    std::string value = pp::strip_quotes(trim(line.substr(colon + 1)));
    std::string pretty_key;
    if (key == "id") pretty_key = "ID";
    else if (key == "text") pretty_key = "Text";
    else if (key == "risk") pretty_key = "Risk";
    else if (key == "verifymethod" || key == "verification") pretty_key = "Verification";
    else if (key == "docref") pretty_key = "Doc Ref";
    else pretty_key = requirement_kind_label(key);
    std::string pretty_value = (key == "risk" || key == "verifymethod" || key == "verification") ? requirement_title_case(value) : value;
    if (pretty_value.empty()) return pretty_key;
    return pretty_key + ": " + pretty_value;
}

std::optional<std::tuple<std::string, std::string, std::vector<std::string>>> parse_requirement_header(const std::string& header) {
    std::string trimmed = trim(header);
    size_t split_at = std::string::npos;
    for (size_t i = 0; i < trimmed.size(); ++i)
        if (is_space(trimmed[i])) {
            split_at = i;
            break;
        }
    if (split_at == std::string::npos) return std::nullopt;
    std::string kind = trim(trimmed.substr(0, split_at));
    std::string rest = trim(trimmed.substr(split_at));
    if (kind.empty() || rest.empty()) return std::nullopt;
    auto [id_raw, classes] = pp::split_inline_classes(rest);
    std::string id = pp::strip_quotes(id_raw);
    if (id.empty()) return std::nullopt;
    return std::make_tuple(kind, id, classes);
}

void push_requirement_node(Graph& graph, const std::string& kind, const std::string& id, const std::vector<std::string>& classes) {
    std::string kind_label = requirement_kind_label(kind);
    std::string label = kind_label.empty() ? id : "<<" + kind_label + ">>\n" + id;
    graph.ensure_node(id, label, NodeShape::Rectangle);
    pp::apply_node_classes(graph, id, classes);
}

std::string trim_char(std::string s, char c, bool front, bool back) {
    size_t a = 0, b = s.size();
    if (front)
        while (a < b && s[a] == c) ++a;
    if (back)
        while (b > a && s[b - 1] == c) --b;
    return s.substr(a, b - a);
}

std::optional<std::string> normalize_requirement_relation(const std::string& raw) {
    std::string r = trim(raw);
    r = trim_char(r, '-', true, true);
    r = trim(r);
    r = trim_char(r, '<', true, false);
    r = trim_char(r, '>', false, true);
    r = to_ascii_lower(trim(r));
    for (const char* ok : {"contains", "copies", "derives", "satisfies", "verifies", "refines", "traces"})
        if (r == ok) return r;
    return std::nullopt;
}

std::optional<std::tuple<std::string, std::string, std::string>> parse_requirement_relation_line(const std::string& line) {
    size_t p = line.find("->");
    if (p != std::string::npos) {
        std::string to = pp::strip_quotes(trim(line.substr(p + 2)));
        std::string lt = trim(line.substr(0, p));
        size_t dash = lt.find('-');
        if (dash == std::string::npos) return std::nullopt;
        std::string from = pp::strip_quotes(trim(lt.substr(0, dash)));
        auto rel = normalize_requirement_relation(lt.substr(dash + 1));
        if (!rel) return std::nullopt;
        if (from.empty() || to.empty()) return std::nullopt;
        return std::make_tuple(from, *rel, to);
    }
    p = line.find("<-");
    if (p != std::string::npos) {
        std::string to = pp::strip_quotes(trim(line.substr(0, p)));
        std::string rt = trim(line.substr(p + 2));
        size_t dash = rt.find('-');
        if (dash == std::string::npos) return std::nullopt;
        std::string from = pp::strip_quotes(trim(rt.substr(dash + 1)));
        auto rel = normalize_requirement_relation(rt.substr(0, dash));
        if (!rel) return std::nullopt;
        if (from.empty() || to.empty()) return std::nullopt;
        return std::make_tuple(from, *rel, to);
    }
    return std::nullopt;
}

}  // namespace

Graph parse_requirement_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::Requirement;
    graph.direction = Direction::TopDown;
    std::map<std::string, std::vector<std::string>> attributes;
    std::optional<std::string> current_id;
    for (const std::string& raw_line : pp::preprocess_input(input)) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        if (starts_with(to_ascii_lower(line), "requirementdiagram")) continue;
        if (auto d = pp::parse_direction_line(line)) {
            graph.direction = *d;
            continue;
        }
        if (starts_with(line, "classDef")) { pp::parse_class_def(line, graph); continue; }
        if (starts_with(line, "class ")) { pp::parse_class_line(line, graph); continue; }
        if (starts_with(line, "style ")) { pp::parse_style_line(line, graph); continue; }
        if (current_id) {
            const std::string active = *current_id;
            size_t end_idx = line.find('}');
            if (end_idx != std::string::npos) {
                std::string fragment = trim(line.substr(0, end_idx));
                if (!fragment.empty()) attributes[active].push_back(fragment);
                current_id.reset();
            } else {
                attributes[active].push_back(line);
            }
            continue;
        }
        if (auto rel = parse_requirement_relation_line(line)) {
            auto& [from, r, to] = *rel;
            graph.ensure_node(from, std::nullopt, NodeShape::Rectangle);
            graph.ensure_node(to, std::nullopt, NodeShape::Rectangle);
            bool is_contains = r == "contains";
            Edge e;
            e.from = from;
            e.to = to;
            e.label = r;
            e.directed = true;
            e.arrow_start = is_contains;
            e.arrow_end = !is_contains;
            graph.edges.push_back(std::move(e));
            continue;
        }
        size_t open_idx = line.find('{');
        if (open_idx != std::string::npos) {
            std::string header = trim(line.substr(0, open_idx));
            if (auto h = parse_requirement_header(header)) {
                auto& [kind, id, classes] = *h;
                push_requirement_node(graph, kind, id, classes);
                current_id = id;
                std::string tail = trim(line.substr(open_idx + 1));
                size_t close_idx = tail.find('}');
                if (close_idx != std::string::npos) {
                    std::string fragment = trim(tail.substr(0, close_idx));
                    if (!fragment.empty()) attributes[id].push_back(fragment);
                    current_id.reset();
                } else if (!tail.empty()) {
                    attributes[id].push_back(tail);
                }
            }
            continue;
        }
        if (auto h = parse_requirement_header(line)) {
            auto& [kind, id, classes] = *h;
            push_requirement_node(graph, kind, id, classes);
        }
    }
    for (auto& [id, node] : graph.nodes) {
        auto it = attributes.find(id);
        if (it == attributes.end() || it->second.empty()) continue;
        std::string label = node.label;
        for (const auto& attr : it->second) label += "\n" + normalize_requirement_attr(attr);
        node.label = label;
    }
    return graph;
}

// requirement_edge_label_text(): the relation word, wrapped in << >> unless the config says otherwise.
std::string requirement_edge_label_text(const std::string& label, const LayoutConfig& config) {
    std::string t = trim(label);
    t = trim_char(t, '<', true, false);
    t = trim_char(t, '>', false, true);
    t = trim(t);
    if (t.empty()) return "";
    return config.requirement.edge_label_brackets ? "<<" + t + ">>" : t;
}

std::string render_requirement_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config) {
    using svg::escape_xml;
    using svg::F;
    using svg::F2;
    const RequirementConfig& req = config.requirement;
    float pad_x = req.render_padding_x, pad_y = req.render_padding_y;
    float width = fmax_(layout.width + pad_x * 2.0f, 1.0f), height = fmax_(layout.height + pad_y * 2.0f, 1.0f);
    std::vector<std::string> edge_colors;
    for (const auto& e : layout.edges)
        if (e.override_style.stroke) edge_colors.push_back(*e.override_style.stroke);
    std::string s = svg::document_start(width, height, 0.0f, 0.0f, width, height, false, false, theme, true, &edge_colors);

    std::string font_family = svg::normalize_font_family(theme.font_family);
    float measure_font_size = fmax_(theme.font_size, 16.0f);
    float line_height = measure_font_size * config.label_line_height;
    auto render_line = [&](float x, float y, const std::string& text, const std::string& color, bool bold) {
        std::string weight = bold ? " font-weight=\"bold\"" : "";
        return "<text x=\"" + F2(x) + "\" y=\"" + F2(y) + "\" text-anchor=\"start\" font-family=\"" + font_family + "\" font-size=\"" + F(theme.font_size) +
               "\" fill=\"" + color + "\"" + weight + ">" + escape_xml(text) + "</text>";
    };

    std::string edge_stroke = escape_xml(req.edge_stroke);
    s += "<defs>";
    s += "<marker id=\"req-contains-start\" refX=\"0\" refY=\"10\" markerWidth=\"20\" markerHeight=\"20\" orient=\"auto\"><g><circle cx=\"10\" cy=\"10\" r=\"9\" fill=\"none\" stroke=\"" +
         edge_stroke + "\" stroke-width=\"1\"/><line x1=\"1\" x2=\"19\" y1=\"10\" y2=\"10\" stroke=\"" + edge_stroke +
         "\"/><line y1=\"1\" y2=\"19\" x1=\"10\" x2=\"10\" stroke=\"" + edge_stroke + "\"/></g></marker>";
    s += "<marker id=\"req-arrow-end\" refX=\"20\" refY=\"10\" markerWidth=\"20\" markerHeight=\"20\" orient=\"auto\"><path d=\"M0,0 L20,10 M20,10 L0,20\" fill=\"none\" stroke=\"" +
         edge_stroke + "\" stroke-width=\"1\"/></marker>";
    s += "</defs>";

    bool has_padding = std::fabs(pad_x) > 1.1920929e-7f || std::fabs(pad_y) > 1.1920929e-7f;
    if (has_padding) s += "<g transform=\"translate(" + F2(pad_x) + "," + F2(pad_y) + ")\">";

    for (size_t edge_idx = 0; edge_idx < layout.edges.size(); ++edge_idx) {
        const EdgeLayout& edge = layout.edges[edge_idx];
        std::string edge_id = "edge-" + std::to_string(edge_idx);
        std::string stroke = edge.override_style.stroke ? *edge.override_style.stroke : req.edge_stroke;
        float stroke_width = edge.override_style.stroke_width ? *edge.override_style.stroke_width : req.edge_stroke_width;
        std::string dash = edge.override_style.dasharray ? " stroke-dasharray=\"" + *edge.override_style.dasharray + "\"" : "";
        std::string marker_start = edge.arrow_start ? " marker-start=\"url(#req-contains-start)\"" : "";
        std::string marker_end = edge.arrow_end ? " marker-end=\"url(#req-arrow-end)\"" : "";
        s += "<path id=\"" + edge_id + "\" data-edge-id=\"" + edge_id + "\" d=\"" + svg::points_to_path(edge.points) + "\" fill=\"none\" stroke=\"" + stroke +
             "\" stroke-width=\"" + F(stroke_width) + "\"" + dash + marker_start + marker_end + " stroke-linecap=\"round\" stroke-linejoin=\"round\"/>";
        if (edge.label && edge.label_anchor) {
            auto [x, y] = *edge.label_anchor;
            auto [lpx, lpy] = edge_label_padding(layout.kind, config);
            auto rect = svg::label_rect_from_center(x, y, edge.label->width, edge.label->height, lpx, lpy);
            if (req.edge_label_background != "none") {
                bool visible = svg::label_background_visible(layout.kind, edge.points, rect);
                float fill_opacity = visible ? 0.5f : 0.0f;
                s += "<rect data-edge-id=\"" + edge_id + "\" data-label-kind=\"center\" x=\"" + F2(rect[0]) + "\" y=\"" + F2(rect[1]) + "\" width=\"" +
                     F2(rect[2]) + "\" height=\"" + F2(rect[3]) + "\" rx=\"2\" ry=\"2\" fill=\"" + req.edge_label_background + "\" fill-opacity=\"" +
                     F2(fill_opacity) + "\" stroke=\"none\"/>";
            }
            std::string label_color = edge.override_style.label_color ? *edge.override_style.label_color : req.edge_label_color;
            s += "<g class=\"edgeLabel\" data-edge-id=\"" + edge_id + "\" data-label-kind=\"center\">";
            s += svg::text_block(x, y, *edge.label, theme, config, theme.font_size, "middle", label_color, false);
            s += "</g>";
        }
    }

    for (const auto& [id, node] : layout.nodes) {
        if (node.hidden || node.anchor_subgraph) continue;
        std::string fill = node.style.fill ? *node.style.fill : req.fill;
        std::string base_stroke = node.style.stroke ? *node.style.stroke : req.box_stroke;
        float base_stroke_width = node.style.stroke_width ? *node.style.stroke_width : req.box_stroke_width;
        std::string label_color = node.style.text_color ? *node.style.text_color : req.label_color;
        s += "<rect x=\"" + F2(node.x) + "\" y=\"" + F2(node.y) + "\" width=\"" + F2(node.width) + "\" height=\"" + F2(node.height) + "\" fill=\"" + fill +
             "\" stroke=\"" + base_stroke + "\" stroke-width=\"" + F(base_stroke_width) + "\"/>";
        if (req.stroke != "none" && req.stroke_width > 0.0f)
            s += "<rect x=\"" + F2(node.x) + "\" y=\"" + F2(node.y) + "\" width=\"" + F2(node.width) + "\" height=\"" + F2(node.height) +
                 "\" fill=\"none\" stroke=\"" + req.stroke + "\" stroke-width=\"" + F(req.stroke_width) + "\"/>";
        const auto& lines = node.label.lines;
        size_t header_count = std::min<size_t>(lines.size(), 2);
        float header_x = node.x + req.label_padding_x, header_y = node.y + req.label_padding_y;
        if (header_count >= 1) s += render_line(header_x, header_y, lines[0], label_color, false);
        if (header_count >= 2) {
            float min_header_gap = theme.font_size * 1.25f;
            float id_y = header_y + fmax_(req.header_line_gap, min_header_gap);
            s += render_line(header_x, id_y, lines[1], label_color, true);
        }
        if (lines.size() > 2) {
            float divider_y = node.y + req.header_band_height;
            s += "<line x1=\"" + F2(node.x) + "\" y1=\"" + F2(divider_y) + "\" x2=\"" + F2(node.x + node.width) + "\" y2=\"" + F2(divider_y) + "\" stroke=\"" +
                 req.divider_color + "\" stroke-width=\"" + F(req.divider_width) + "\"/>";
            float body_y = divider_y + req.label_padding_y;
            for (size_t i = 2; i < lines.size(); ++i) {
                s += render_line(header_x, body_y, lines[i], label_color, false);
                body_y += line_height;
            }
        }
    }
    if (has_padding) s += "</g>";
    s += "</svg>";
    return s;
}

}  // namespace mermaid
