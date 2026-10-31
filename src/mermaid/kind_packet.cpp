// Mermaid port: packet diagrams. Mirrors parser.rs parse_packet_diagram. Layout and writer are the flowchart ones
// (the fields become a chain of rectangle nodes).
#include "kinds.h"
#include "parser_internal.h"
#include "strutil.h"

namespace mermaid {

Graph parse_packet_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::Packet;
    graph.direction = Direction::LeftRight;
    std::optional<std::string> last_node;

    for (const std::string& raw_line : pp::preprocess_input(input)) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        std::string lower = to_ascii_lower(line);
        if (starts_with(lower, "packet") || starts_with(lower, "title")) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string range = trim(line.substr(0, colon));
        std::string label = pp::strip_quotes(trim(line.substr(colon + 1)));
        if (range.empty()) continue;
        std::string node_id = "packet_" + std::to_string(graph.nodes.size());
        std::string node_label = label.empty() ? range : range + "\n" + label;
        graph.ensure_node(node_id, node_label, NodeShape::Rectangle);
        if (last_node) {
            Edge e;
            e.from = *last_node;
            e.to = node_id;
            graph.edges.push_back(std::move(e));
        }
        last_node = node_id;
    }
    return graph;
}

}  // namespace mermaid
