// Mermaid port: parser helpers shared between parser.cpp and the per-diagram parser files
// (parser_graphs.cpp ...). They are defined in parser.cpp; this header only declares them.
#pragma once

#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ir.h"

namespace mermaid::pp {

// parser.rs preprocess_input: non-blank lines without comments / frontmatter (each trimmed).
std::vector<std::string> preprocess_input(const std::string& input);
// parser.rs parse_init_directive: validates a `%%{init: ...}%%` line (throws ParseError); true when it is one.
bool parse_init_directive(const std::string& trimmed_line);
std::vector<std::string> split_statements(const std::string& line);
std::string strip_quotes(const std::string& input);
std::pair<std::string, std::vector<std::string>> split_inline_classes(const std::string& token);
std::optional<Direction> parse_direction_line(const std::string& line);
std::optional<Direction> direction_from_token(const std::string& token);
void parse_class_def(const std::string& line, Graph& g);
void parse_class_line(const std::string& line, Graph& g);
void parse_style_line(const std::string& line, Graph& g);
void apply_node_classes(Graph& g, const std::string& id, const std::vector<std::string>& classes);
void add_node_to_subgraphs(Graph& g, const std::vector<size_t>& stack, const std::string& id);
// parser.rs split_label: "target : label" -> (target, optional label).
std::pair<std::string, std::optional<std::string>> split_label(const std::string& input);

// parser.rs parse_node_token: (id, label, shape, inline classes) of "id[label]:::class".
std::tuple<std::string, std::optional<std::string>, std::optional<NodeShape>, std::vector<std::string>> parse_node_token(
    const std::string& token);
// parser.rs add_flowchart_edge: parse `A & B -->|label| C` and add the nodes and edges (false = not an edge line).
bool add_flowchart_edge(const std::string& line, Graph& g, const std::vector<size_t>& stack);
// parser.rs preprocess_input_keep_indent: like preprocess_input but lines keep their leading indentation
// (trailing blanks are cut). Defined in kind_util.cpp.
std::vector<std::string> preprocess_input_keep_indent(const std::string& input);
// parser.rs count_indent: spaces count 1, tabs 2, up to the first other character.
size_t count_indent(const std::string& line);

// ---- per-diagram parsers (each in its own file) ----
Graph parse_class_diagram(const std::string& input);
Graph parse_er_diagram(const std::string& input);
Graph parse_state_diagram(const std::string& input);

}  // namespace mermaid::pp
