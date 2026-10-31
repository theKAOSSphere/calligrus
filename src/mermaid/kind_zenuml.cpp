// Mermaid port: ZenUML. Mirrors parser.rs parse_zenuml_diagram. Layout and writer are the sequence diagram ones
// (sequence.cpp / svg_writer.cpp, layout kind ZenUML).
#include <algorithm>

#include "kinds.h"
#include "parser_internal.h"
#include "strutil.h"

namespace mermaid {

namespace {

struct ZenMessage {
    std::string from, to;
    std::optional<std::string> label;
    EdgeStyle style;
};

std::optional<ZenMessage> parse_zenuml_message_line(const std::string& line) {
    static const char* arrows[] = {"-->>", "->>", "-->", "->", "==>", "=>"};
    size_t idx = std::string::npos;
    std::string arrow;
    for (const char* a : arrows) {
        size_t p = line.find(a);
        if (p != std::string::npos) {
            idx = p;
            arrow = a;
            break;
        }
    }
    if (idx == std::string::npos) return std::nullopt;
    std::string left = trim(line.substr(0, idx));
    std::string rest = trim(line.substr(idx + arrow.size()));
    if (left.empty() || rest.empty()) return std::nullopt;
    std::string right;
    std::optional<std::string> label;
    size_t colon = rest.find(':');
    if (colon != std::string::npos) {
        std::string lbl = trim(rest.substr(colon + 1));
        if (!lbl.empty()) label = lbl;
        right = trim(rest.substr(0, colon));
    } else {
        right = rest;
    }
    if (right.empty()) return std::nullopt;
    EdgeStyle style = arrow.find("--") != std::string::npos ? EdgeStyle::Dotted : EdgeStyle::Solid;
    return ZenMessage{left, right, label, style};
}

}  // namespace

Graph parse_zenuml_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::ZenUML;
    std::vector<std::string> order;
    auto ensure = [&](const std::string& id) {
        // ensure_sequence_node with no labels: an unknown participant becomes an actor box
        if (graph.nodes.count(id)) graph.ensure_node(id, std::nullopt, std::nullopt);
        else graph.ensure_node(id, std::nullopt, NodeShape::ActorBox);
    };
    for (const std::string& raw_line : pp::preprocess_input(input)) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        std::string lower = to_ascii_lower(line);
        if (starts_with(lower, "zenuml") || starts_with(lower, "title")) continue;
        if (auto m = parse_zenuml_message_line(line)) {
            ensure(m->from);
            ensure(m->to);
            if (std::find(order.begin(), order.end(), m->from) == order.end()) order.push_back(m->from);
            if (std::find(order.begin(), order.end(), m->to) == order.end()) order.push_back(m->to);
            Edge e;
            e.from = m->from;
            e.to = m->to;
            e.label = m->label;
            e.directed = true;
            e.arrow_end = true;
            e.style = m->style;
            graph.edges.push_back(std::move(e));
        }
    }
    graph.sequence_participants = order;
    return graph;
}

}  // namespace mermaid
