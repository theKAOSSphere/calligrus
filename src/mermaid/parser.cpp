// Mermaid port: flowchart parser (see parser.h). Function names match parser.rs.
#include "parser.h"

#include <charconv>
#include <cstring>
#include <regex>
#include <tuple>

#include "kinds.h"
#include "json5.h"
#include "parser_internal.h"
#include "strutil.h"

namespace mermaid {

namespace ir_detail {}

void Graph::ensure_node(const std::string& id, const std::optional<std::string>& label,
                        const std::optional<NodeShape>& shape) {
    auto it = nodes.find(id);
    if (it == nodes.end()) {
        Node n;
        n.id = id;
        n.label = id;
        n.shape = NodeShape::Rectangle;
        it = nodes.emplace(id, std::move(n)).first;
        size_t order = node_order.size();
        node_order.emplace(id, order);
    }
    if (label) it->second.label = *label;
    if (shape) it->second.shape = *shape;
}

namespace {

// ---- the regexes of parser.rs (named groups become numbered groups, noted per regex) ----

// ^(flowchart|graph)\s+(\w+)
const std::regex& header_re() {
    static const std::regex re(R"(^(flowchart|graph)\s+(\w+))");
    return re;
}
// ^subgraph\s+(.*)$
const std::regex& subgraph_re() {
    static const std::regex re(R"(^subgraph\s+(.*)$)");
    return re;
}
// groups: 1 left, 2 arrow, 3 label, 4 right
const std::regex& pipe_label_re() {
    static const std::regex re(
        R"(^(.+?)\s*(<[-.=ox]*[-=]+[-.=ox]*>|<[-.=ox]*[-=]+[-.=ox]*|[-.=ox]*[-=]+[-.=ox]*>|[-.=ox]*[-=]+[-.=ox]*)\|(.+?)\|\s*(.+)$)");
    return re;
}
// groups: 1 left, 2 start, 3 dash1, 4 label, 5 dash2, 6 end, 7 right
const std::regex& quoted_label_arrow_re() {
    static const std::regex re(
        R"re(^(.+?)\s*(<)?([-.=ox]*[-=]+[-.=ox]*)\s+"([^"]+)"\s+([-.=ox]*[-=]+[-.=ox]*)(>)?\s*(.+)$)re");
    return re;
}
const std::regex& label_arrow_re() {
    static const std::regex re(
        R"(^(.+?)\s*(<)?([-.=ox]*[-=]+[-.=ox]*)\s+([^<>=]+?)\s+([-.=ox]*[-=]+[-.=ox]*)(>)?\s*(.+)$)");
    return re;
}
const std::regex& compact_dotted_label_arrow_re() {
    static const std::regex re(
        R"(^(.+?)\s*(<)?([-=ox]*[-=]+[-=ox]*)\.([^<>=|].*?)\.([-.=ox]*[-=]+[-.=ox]*)(>)?\s*(.+)$)");
    return re;
}
// groups: 1 left, 2 arrow, 3 right
const std::regex& arrow_re() {
    static const std::regex re(
        R"(^(.+?)\s*(<[-.=ox]*[-=]+[-.=ox]*>|<[-.=ox]*[-=]+[-.=ox]*|[-.=ox]*[-=]+[-.=ox]*>|[-.=ox]*[-=]+[-.=ox]*)\s*(.+)$)");
    return re;
}
const std::regex& arrow_token_re() {
    static const std::regex re(
        R"(<[-.=ox]*[-=]+[-.=ox]*>|<[-.=ox]*[-=]+[-.=ox]*|[-.=ox]*[-=]+[-.=ox]*>|[-.=ox]*[-=]+[-.=ox]*)");
    return re;
}

// A Rust slice like s[a..b] panics when a > b (or out of range); the app catches the panic and
// reports an error, so we report a ParseError (the hook then falls back to Rust).
std::string slice(const std::string& s, size_t a, size_t b) {
    if (a > b || b > s.size()) throw ParseError("invalid slice in parser");
    return s.substr(a, b - a);
}

// ---- comment / line preprocessing ----

std::string strip_trailing_comment(const std::string& line) {
    char quote = 0;
    std::string out;
    for (size_t i = 0; i < line.size(); ++i) {
        char ch = line[i];
        if (quote) {
            if (ch == quote) quote = 0;
            out.push_back(ch);
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            out.push_back(ch);
            continue;
        }
        if (ch == '%' && i + 1 < line.size() && line[i + 1] == '%') break;
        out.push_back(ch);
    }
    return trim(out);
}

// parser.rs parse_init_directive(): a `%%{init: {...}}%%` line must hold valid JSON / JSON5 (the crate parses
// the config but render_with_options ignores it). Returns true when the line is an init directive.
bool parse_init_directive(const std::string& trimmed_line) {
    static const std::regex init_re(R"(^%%\{\s*init\s*:\s*(\{.*\})\s*\}%%)");
    std::smatch m;
    if (std::regex_search(trimmed_line, m, init_re)) {
        if (!json5_is_valid(m[1].str())) throw ParseError("invalid Mermaid init directive: could not parse JSON/JSON5 config");
        return true;
    }
    if (starts_with(trimmed_line, "%%{") && to_ascii_lower(trimmed_line).find("init") != std::string::npos)
        throw ParseError("invalid Mermaid init directive syntax");
    return false;
}

// Lines of the diagram without comments, blank lines and frontmatter. `%%{` directives are not
// supported (the Rust renderer parses them but render_with_options ignores the result; we
// refuse so a malformed directive's error behaviour is not silently different).
std::vector<std::string> preprocess_input(const std::string& input) {
    std::vector<std::string> lines;
    bool in_frontmatter = false;
    for (const std::string& raw : lines_of(input)) {
        std::string t = trim(raw);
        if (t.empty()) continue;
        if (t == "---") {
            in_frontmatter = !in_frontmatter;
            continue;
        }
        if (in_frontmatter) continue;
        if (parse_init_directive(t)) continue;
        if (starts_with(t, "%%")) continue;
        std::string without = strip_trailing_comment(t);
        if (without.empty()) continue;
        lines.push_back(without);
    }
    return lines;
}

// ---- masking and splitting helpers ----

// Blank out everything inside [...], (...), {...}, "..." and '...' (keeping byte length), so edge
// detection does not see dashes inside labels.
std::string mask_bracket_content(const std::string& line) {
    std::string result;
    result.reserve(line.size());
    int depth_square = 0, depth_paren = 0, depth_curly = 0;
    bool in_double = false, in_single = false;
    char prev = '\0';
    for (char ch : line) {
        bool in_bracket = depth_square > 0 || depth_paren > 0 || depth_curly > 0;
        bool in_quote = in_double || in_single;
        if (ch == '[' && !in_quote) {
            ++depth_square;
            result.push_back(ch);
        } else if (ch == ']' && !in_quote && depth_square > 0) {
            --depth_square;
            result.push_back(ch);
        } else if (ch == '(' && !in_quote && !in_bracket) {
            ++depth_paren;
            result.push_back(ch);
        } else if (ch == ')' && !in_quote && depth_paren > 0) {
            --depth_paren;
            result.push_back(ch);
        } else if (ch == '{' && !in_quote && !in_bracket) {
            ++depth_curly;
            result.push_back(ch);
        } else if (ch == '}' && !in_quote && depth_curly > 0) {
            --depth_curly;
            result.push_back(ch);
        } else if (ch == '"' && prev != '\\') {
            in_double = !in_double;
            result.push_back((in_bracket || in_quote) ? ' ' : ch);
        } else if (ch == '\'' && prev != '\\') {
            in_single = !in_single;
            result.push_back((in_bracket || in_quote) ? ' ' : ch);
        } else {
            result.push_back((in_bracket || in_quote) ? ' ' : ch);
        }
        prev = ch;
    }
    return result;
}

std::vector<std::string> split_statements(const std::string& line) {
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    char quote = 0;
    bool escaped = false;
    for (char ch : line) {
        if (escaped) {
            current.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            current.push_back(ch);
            escaped = true;
            continue;
        }
        if (quote) {
            if (ch == quote) quote = 0;
            current.push_back(ch);
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            current.push_back(ch);
            continue;
        }
        switch (ch) {
        case '[': case '(': case '{':
            ++depth;
            current.push_back(ch);
            break;
        case ']': case ')': case '}':
            if (depth > 0) --depth;
            current.push_back(ch);
            break;
        case ';':
            if (depth == 0) {
                std::string t = trim(current);
                if (!t.empty()) parts.push_back(t);
                current.clear();
            } else {
                current.push_back(ch);
            }
            break;
        default:
            current.push_back(ch);
        }
    }
    std::string t = trim(current);
    if (!t.empty()) parts.push_back(t);
    return parts;
}

// Split on '&' outside brackets/quotes: `A & B` -> A, B.
std::vector<std::string> split_on_ampersand(const std::string& input) {
    std::string masked = mask_bracket_content(input);
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i < masked.size(); ++i) {
        if (masked[i] == '&') {
            std::string part = trim(std::string_view(input).substr(start, i - start));
            if (!part.empty()) parts.push_back(part);
            start = i + 1;
        }
    }
    std::string last = trim(std::string_view(input).substr(start));
    if (!last.empty()) parts.push_back(last);
    return parts;
}

std::string strip_quotes(const std::string& input) {
    std::string t = trim(input);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') return t.substr(1, t.size() - 2);
    if (t.size() >= 2 && t.front() == '\'' && t.back() == '\'') return t.substr(1, t.size() - 2);
    return t;
}

// ---- node tokens: id, shape, label ----

using NodeTokenParts = std::tuple<std::string, std::optional<std::string>, std::optional<NodeShape>,
                                  std::vector<std::string>>;

// `A:::cls1:::cls2` -> ("A", [cls1, cls2])
std::pair<std::string, std::vector<std::string>> split_inline_classes(const std::string& token) {
    std::vector<std::string> pieces;
    size_t pos = 0;
    while (true) {
        size_t j = token.find(":::", pos);
        if (j == std::string::npos) {
            pieces.push_back(token.substr(pos));
            break;
        }
        pieces.push_back(token.substr(pos, j - pos));
        pos = j + 3;
    }
    std::string base = trim(pieces[0]);
    std::vector<std::string> classes;
    for (size_t i = 1; i < pieces.size(); ++i) {
        std::string c = trim(pieces[i]);
        if (!c.empty()) classes.push_back(c);
    }
    return {base, classes};
}

std::pair<std::string, NodeShape> parse_shape_from_brackets(const std::string& raw) {
    std::string t = trim(raw);
    auto inner = [&](size_t n) { return strip_quotes(slice(t, n, t.size() - n)); };
    if (starts_with(t, "[/") && ends_with(t, "/]")) return {inner(2), NodeShape::Parallelogram};
    if (starts_with(t, "[\\") && ends_with(t, "\\]")) return {inner(2), NodeShape::ParallelogramAlt};
    if (starts_with(t, "[/") && ends_with(t, "\\]")) return {inner(2), NodeShape::Trapezoid};
    if (starts_with(t, "[\\") && ends_with(t, "/]")) return {inner(2), NodeShape::TrapezoidAlt};
    if (starts_with(t, "[[") && ends_with(t, "]]")) return {inner(2), NodeShape::Subroutine};
    if (starts_with(t, "[(") && ends_with(t, ")]")) return {inner(2), NodeShape::Cylinder};
    if (starts_with(t, "[") && ends_with(t, "]")) {
        std::string in = slice(t, 1, t.size() - 1);
        if (starts_with(in, "(") && ends_with(in, ")"))
            return {strip_quotes(slice(in, 1, in.size() - 1)), NodeShape::Stadium};
        return {strip_quotes(in), NodeShape::Rectangle};
    }
    return {strip_quotes(t), NodeShape::Rectangle};
}

std::pair<std::string, NodeShape> parse_shape_from_parens(const std::string& raw) {
    std::string t = trim(raw);
    if (starts_with(t, "(((") && ends_with(t, ")))"))
        return {strip_quotes(slice(t, 3, t.size() - 3)), NodeShape::DoubleCircle};
    if (starts_with(t, "((") && ends_with(t, "))"))
        return {strip_quotes(slice(t, 2, t.size() - 2)), NodeShape::DoubleCircle};
    if (starts_with(t, "(") && ends_with(t, ")")) {
        std::string in = slice(t, 1, t.size() - 1);
        if (starts_with(in, "[") && ends_with(in, "]"))
            return {strip_quotes(slice(in, 1, in.size() - 1)), NodeShape::Stadium};
        return {strip_quotes(in), NodeShape::RoundRect};
    }
    return {strip_quotes(t), NodeShape::RoundRect};
}

std::pair<std::string, NodeShape> parse_shape_from_braces(const std::string& raw) {
    std::string t = trim(raw);
    if (starts_with(t, "{{") && ends_with(t, "}}"))
        return {strip_quotes(slice(t, 2, t.size() - 2)), NodeShape::Hexagon};
    if (starts_with(t, "{") && ends_with(t, "}"))
        return {strip_quotes(slice(t, 1, t.size() - 1)), NodeShape::Diamond};
    return {strip_quotes(t), NodeShape::Diamond};
}

struct IdLabel {
    std::string id, label;
    NodeShape shape;
};

// `A[text]`, `A(text)`, `A{text}` -> id, label, shape
std::optional<IdLabel> split_id_label(const std::string& token) {
    struct Form {
        char open;
        char close;
        std::pair<std::string, NodeShape> (*parse)(const std::string&);
    };
    static const Form forms[] = {{'[', ']', parse_shape_from_brackets},
                                 {'(', ')', parse_shape_from_parens},
                                 {'{', '}', parse_shape_from_braces}};
    for (const Form& f : forms) {
        size_t start = token.find(f.open);
        if (start != std::string::npos && !token.empty() && token.back() == f.close) {
            std::string id = trim(std::string_view(token).substr(0, start));
            if (!id.empty()) {
                auto [label, shape] = f.parse(token.substr(start));
                return IdLabel{id, label, shape};
            }
        }
    }
    return std::nullopt;
}

// `A>text]` (asymmetric shape)
std::optional<IdLabel> split_asymmetric_label(const std::string& token) {
    std::string t = trim(token);
    if (t.find('[') != std::string::npos) return std::nullopt;
    size_t pos = t.find('>');
    if (pos == std::string::npos) return std::nullopt;
    if (!ends_with(t, "]")) return std::nullopt;
    std::string id = trim(std::string_view(t).substr(0, pos));
    if (id.empty()) return std::nullopt;
    std::string label = trim(slice(t, pos + 1, t.size() - 1));
    if (label.empty()) return std::nullopt;
    return IdLabel{id, strip_quotes(label), NodeShape::Asymmetric};
}

NodeTokenParts parse_node_token(const std::string& token) {
    auto [base, classes] = split_inline_classes(token);
    std::string trimmed = trim(base);
    if (auto a = split_asymmetric_label(trimmed))
        return {a->id, a->label, a->shape, classes};
    if (auto b = split_id_label(trimmed))
        return {b->id, b->label, b->shape, classes};
    auto words = split_whitespace(trimmed);
    std::string id = words.empty() ? std::string() : words[0];
    return {id, std::nullopt, std::nullopt, classes};
}

std::optional<NodeTokenParts> parse_node_only(const std::string& line) {
    if (line.find("--") != std::string::npos) return std::nullopt;
    NodeTokenParts p = parse_node_token(line);
    if (std::get<0>(p).empty()) return std::nullopt;
    return p;
}

// ---- membership of nodes in subgraphs ----

void add_node_to_subgraph(Graph& g, size_t idx, const std::string& id) {
    if (idx >= g.subgraphs.size()) return;
    auto& nodes = g.subgraphs[idx].nodes;
    for (const auto& n : nodes)
        if (n == id) return;
    nodes.push_back(id);
}

void add_node_to_subgraphs(Graph& g, const std::vector<size_t>& stack, const std::string& id) {
    if (stack.empty()) return;
    // Mermaid-js parity: a node belongs to the first subgraph that claims it (see the Rust comment).
    bool claimed_elsewhere = false;
    for (size_t idx = 0; idx < g.subgraphs.size(); ++idx) {
        bool in_stack = false;
        for (size_t s : stack)
            if (s == idx) in_stack = true;
        if (in_stack) continue;
        for (const auto& n : g.subgraphs[idx].nodes)
            if (n == id) claimed_elsewhere = true;
    }
    if (claimed_elsewhere) return;
    for (size_t idx : stack) add_node_to_subgraph(g, idx, id);
}

void apply_node_classes(Graph& g, const std::string& id, const std::vector<std::string>& classes) {
    for (const auto& c : classes) {
        if (c.empty()) continue;
        g.node_classes[id].push_back(c);
    }
}

void apply_subgraph_classes(Graph& g, const std::string& id, const std::vector<std::string>& classes) {
    for (const auto& c : classes) {
        if (c.empty()) continue;
        g.subgraph_classes[id].push_back(c);
    }
}

// `subgraph` header text -> (id, label, classes)
std::tuple<std::optional<std::string>, std::string, std::vector<std::string>>
parse_subgraph_header(const std::string& input) {
    auto [base, classes] = split_inline_classes(input);
    std::string t = trim(base);
    if (t.empty()) return {std::nullopt, "Subgraph", classes};
    if (auto il = split_id_label(t)) return {il->id, il->label, classes};
    if (t.find('"') == std::string::npos && t.find('\'') == std::string::npos) {
        auto parts = split_whitespace(t);
        if (parts.size() == 1) return {parts[0], parts[0], classes};
    }
    return {std::nullopt, strip_quotes(t), classes};
}

// ---- edges ----

struct EdgeMeta {
    bool directed = false, arrow_start = false, arrow_end = false;
    std::optional<EdgeArrowhead> arrow_start_kind, arrow_end_kind;
    std::optional<EdgeDecoration> start_decoration, end_decoration;
    EdgeStyle style = EdgeStyle::Solid;
};

EdgeMeta parse_edge_meta(const std::string& arrow) {
    std::string t = trim(arrow);
    EdgeMeta m;
    if (!t.empty() && t.front() == 'o') {
        m.start_decoration = EdgeDecoration::Circle;
        t.erase(0, 1);
    } else if (!t.empty() && t.front() == 'x') {
        m.start_decoration = EdgeDecoration::Cross;
        t.erase(0, 1);
    }
    if (!t.empty() && t.back() == 'o') {
        m.end_decoration = EdgeDecoration::Circle;
        t.pop_back();
    } else if (!t.empty() && t.back() == 'x') {
        m.end_decoration = EdgeDecoration::Cross;
        t.pop_back();
    }
    m.arrow_start = !t.empty() && t.front() == '<';
    m.arrow_end = !t.empty() && t.back() == '>';
    if (t.find('=') != std::string::npos) m.style = EdgeStyle::Thick;
    else if (t.find('.') != std::string::npos) m.style = EdgeStyle::Dotted;
    else m.style = EdgeStyle::Solid;
    m.directed = m.arrow_start || m.arrow_end;
    return m;
}

struct ParsedEdge {
    std::string left;
    std::optional<std::string> label;
    std::string right;
    EdgeMeta meta;
};

// `o--` / `x--` style decoration glued to the right node: "ox" + whitespace + rest.
std::optional<std::pair<char, std::string>> extract_leading_decoration(const std::string& right) {
    if (right.empty()) return std::nullopt;
    char first = right[0];
    if (first != 'o' && first != 'x') return std::nullopt;
    std::string rest = right.substr(1);
    if (rest.empty()) return std::nullopt;
    if (is_space(rest[0])) return std::make_pair(first, std::string(trim_start_view(rest)));
    return std::nullopt;
}

std::string trim_matches(std::string s, char c) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] == c) ++a;
    while (b > a && s[b - 1] == c) --b;
    return s.substr(a, b - a);
}

// `masked` positions index into the original `line` (mask keeps byte length).
std::string extract(const std::string& line, const std::smatch& m, size_t group) {
    return line.substr(static_cast<size_t>(m.position(group)), static_cast<size_t>(m.length(group)));
}

std::optional<ParsedEdge> parse_edge_line(const std::string& line) {
    std::string masked = mask_bracket_content(line);
    std::smatch m;

    if (std::regex_search(masked, m, pipe_label_re())) {
        std::string left = trim(extract(line, m, 1));
        std::string right = trim(extract(line, m, 4));
        std::string label = trim(extract(line, m, 3));
        if (!label.empty() && !left.empty() && !right.empty()) {
            std::string arrow = trim(extract(line, m, 2));
            return ParsedEdge{left, label, right, parse_edge_meta(arrow)};
        }
    }

    // Quoted label syntax matches the original line: masking blanks quoted text.
    if (std::regex_search(line, m, quoted_label_arrow_re())) {
        std::string left = trim(m[1].str());
        std::string right = trim(m[7].str());
        std::string label = trim(m[4].str());
        if (!label.empty() && !left.empty() && !right.empty()) {
            std::string arrow = m[2].str() + m[3].str() + m[5].str() + m[6].str();
            return ParsedEdge{left, label, right, parse_edge_meta(arrow)};
        }
    }

    if (std::regex_search(masked, m, label_arrow_re())) {
        std::string left = trim(extract(line, m, 1));
        std::string right = trim(extract(line, m, 7));
        std::string raw = trim(extract(line, m, 4));
        std::string label = trim(trim_matches(raw, '|'));
        if (!label.empty() && !left.empty() && !right.empty()) {
            std::string arrow = m[2].str() + m[3].str() + m[5].str() + m[6].str();
            return ParsedEdge{left, label, right, parse_edge_meta(arrow)};
        }
    }

    if (std::regex_search(masked, m, compact_dotted_label_arrow_re())) {
        std::string left = trim(extract(line, m, 1));
        std::string right = trim(extract(line, m, 7));
        std::string label = trim_matches(trim(extract(line, m, 4)), '.');
        if (!label.empty() && !left.empty() && !right.empty()) {
            std::string arrow = m[2].str() + m[3].str() + "." + m[5].str() + m[6].str();
            return ParsedEdge{left, label, right, parse_edge_meta(arrow)};
        }
    }

    if (!std::regex_search(masked, m, arrow_re())) return std::nullopt;
    std::string left = trim(extract(line, m, 1));
    std::string arrow = trim(m[2].str());
    std::string right = trim(extract(line, m, 3));
    if (auto dec = extract_leading_decoration(right)) {
        arrow.push_back(dec->first);
        right = dec->second;
    }
    if (left.empty() || right.empty() || arrow.empty()) return std::nullopt;

    std::optional<std::string> label;
    std::string right_token = right;
    if (starts_with(right, "|")) {
        std::string stripped = right.substr(1);
        size_t end = stripped.find('|');
        if (end != std::string::npos) {
            label = trim(stripped.substr(0, end));
            right_token = trim(stripped.substr(end + 1));
        }
    }
    if (right_token.empty()) return std::nullopt;
    return ParsedEdge{left, label, right_token, parse_edge_meta(arrow)};
}

bool looks_like_flowchart_edge_syntax(const std::string& line) {
    std::string masked = mask_bracket_content(line);
    return std::regex_search(masked, arrow_token_re());
}

bool has_missing_flowchart_edge_endpoint(const std::string& line) {
    std::string masked = mask_bracket_content(line);
    std::string t = trim(masked);
    for (auto it = std::sregex_iterator(t.begin(), t.end(), arrow_token_re());
         it != std::sregex_iterator(); ++it) {
        size_t s = static_cast<size_t>(it->position(0));
        size_t e = s + static_cast<size_t>(it->length(0));
        if (trim_view(std::string_view(t).substr(0, s)).empty() ||
            trim_view(std::string_view(t).substr(e)).empty())
            return true;
    }
    return false;
}

// `A --> B --> C` -> ["A --> B", "B --> C"]; nullopt if not a chain (or a labelled single edge).
std::optional<std::vector<std::string>> split_edge_chain(const std::string& line) {
    std::string masked = mask_bracket_content(line);
    if (std::regex_search(masked, pipe_label_re()) || std::regex_search(line, quoted_label_arrow_re()) ||
        std::regex_search(masked, label_arrow_re()) ||
        std::regex_search(masked, compact_dotted_label_arrow_re()))
        return std::nullopt;

    std::vector<std::pair<size_t, size_t>> matches;
    for (auto it = std::sregex_iterator(masked.begin(), masked.end(), arrow_token_re());
         it != std::sregex_iterator(); ++it)
        matches.emplace_back(static_cast<size_t>(it->position(0)),
                             static_cast<size_t>(it->position(0) + it->length(0)));
    if (matches.size() < 2) return std::nullopt;

    std::vector<std::string> nodes, arrows;
    size_t last = 0;
    for (auto [s, e] : matches) {
        nodes.push_back(trim(std::string_view(line).substr(last, s - last)));
        arrows.push_back(trim(std::string_view(line).substr(s, e - s)));
        last = e;
    }
    nodes.push_back(trim(std::string_view(line).substr(last)));

    // Attach leading pipe labels to the preceding arrow and strip them from the node token.
    for (size_t i = 1; i < nodes.size(); ++i) {
        std::string_view tr = trim_start_view(nodes[i]);
        if (starts_with(tr, "|")) {
            std::string_view stripped = tr.substr(1);
            size_t end_idx = stripped.find('|');
            if (end_idx != std::string_view::npos) {
                size_t label_len = end_idx + 2;
                std::string label(tr.substr(0, label_len));
                std::string rest(trim_start_view(tr.substr(label_len)));
                arrows[i - 1] += label;
                nodes[i] = rest;
            }
        }
    }
    for (const auto& n : nodes)
        if (n.empty()) return std::nullopt;

    std::vector<std::string> statements;
    for (size_t i = 0; i < arrows.size(); ++i)
        statements.push_back(nodes[i] + " " + arrows[i] + " " + nodes[i + 1]);
    return statements;
}

bool add_flowchart_edge(const std::string& line, Graph& g, const std::vector<size_t>& stack) {
    auto parsed = parse_edge_line(line);
    if (!parsed) return false;

    std::vector<std::string> source_ids, target_ids;
    for (const auto& source : split_on_ampersand(parsed->left)) {
        auto [id, label, shape, classes] = parse_node_token(source);
        g.ensure_node(id, label, shape);
        apply_node_classes(g, id, classes);
        add_node_to_subgraphs(g, stack, id);
        source_ids.push_back(id);
    }
    for (const auto& target : split_on_ampersand(parsed->right)) {
        auto [id, label, shape, classes] = parse_node_token(target);
        g.ensure_node(id, label, shape);
        apply_node_classes(g, id, classes);
        add_node_to_subgraphs(g, stack, id);
        target_ids.push_back(id);
    }
    for (const auto& l : source_ids) {
        for (const auto& r : target_ids) {
            Edge e;
            e.from = l;
            e.to = r;
            e.label = parsed->label;
            e.directed = parsed->meta.directed;
            e.arrow_start = parsed->meta.arrow_start;
            e.arrow_end = parsed->meta.arrow_end;
            e.arrow_start_kind = parsed->meta.arrow_start_kind;
            e.arrow_end_kind = parsed->meta.arrow_end_kind;
            e.start_decoration = parsed->meta.start_decoration;
            e.end_decoration = parsed->meta.end_decoration;
            e.style = parsed->meta.style;
            g.edges.push_back(std::move(e));
        }
    }
    return true;
}

// ---- direction, classDef, class, style, linkStyle, click ----

std::optional<Direction> direction_from_token(const std::string& token) {
    std::string u = to_ascii_upper(token);
    if (u == "TD" || u == "TB") return Direction::TopDown;
    if (u == "BT") return Direction::BottomTop;
    if (u == "LR") return Direction::LeftRight;
    if (u == "RL") return Direction::RightLeft;
    return std::nullopt;
}

std::optional<Direction> parse_direction_line(const std::string& line) {
    auto parts = split_whitespace(line);
    if (parts.size() == 2 && parts[0] == "direction") return direction_from_token(parts[1]);
    return std::nullopt;
}

// Rust "x".parse::<f32>() after trim_end_matches("px"): whole string must be a number.
std::optional<float> parse_f32_px(std::string value) {
    while (ends_with(value, "px")) value.resize(value.size() - 2);
    if (value.empty()) return std::nullopt;
    const char* b = value.data();
    const char* e = b + value.size();
    if (*b == '+') ++b;
    if (b == e) return std::nullopt;
    float f = 0;
    auto res = std::from_chars(b, e, f);
    if (res.ec != std::errc() || res.ptr != e) return std::nullopt;
    return f;
}

// `part.splitn(2, ':')` -> key, value (both trimmed)
std::pair<std::string, std::string> split_kv(const std::string& part) {
    size_t c = part.find(':');
    if (c == std::string::npos) return {trim(part), ""};
    return {trim(std::string_view(part).substr(0, c)), trim(std::string_view(part).substr(c + 1))};
}

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

NodeStyle parse_node_style(const std::string& input) {
    NodeStyle style;
    for (const auto& part : split_char(input, ',')) {
        auto [key, value] = split_kv(part);
        if (key.empty() || value.empty()) continue;
        if (key == "fill") style.fill = value;
        else if (key == "stroke") style.stroke = value;
        else if (key == "stroke-width") style.stroke_width = parse_f32_px(value);
        else if (key == "stroke-dasharray") style.stroke_dasharray = value;
        else if (key == "color") style.text_color = value;
    }
    return style;
}

EdgeStyleOverride parse_edge_style(const std::string& input) {
    EdgeStyleOverride style;
    for (const auto& part : split_char(input, ',')) {
        auto [key, value] = split_kv(part);
        if (key.empty() || value.empty()) continue;
        if (key == "stroke") style.stroke = value;
        else if (key == "stroke-width") style.stroke_width = parse_f32_px(value);
        else if (key == "stroke-dasharray") style.dasharray = value;
        else if (key == "color") style.label_color = value;
    }
    return style;
}

// `s.splitn(n, pred)` with a single-char separator test.
template <class Pred>
std::vector<std::string> splitn(const std::string& s, size_t n, Pred is_sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (out.size() + 1 < n) {
        size_t j = pos;
        while (j < s.size() && !is_sep(s[j])) ++j;
        if (j >= s.size()) break;
        out.push_back(s.substr(pos, j - pos));
        pos = j + 1;
    }
    out.push_back(s.substr(pos));
    return out;
}

void parse_class_def(const std::string& line, Graph& g) {
    std::string t = trim(line);
    auto parts = splitn(t, 3, [](char c) { return is_space(c); });
    std::string class_name = parts.size() > 1 ? trim(parts[1]) : "";
    std::string rest = parts.size() > 2 ? trim(parts[2]) : "";
    if (class_name.empty() || rest.empty()) return;
    g.class_defs[class_name] = parse_node_style(rest);
}

void parse_class_line(const std::string& line, Graph& g) {
    auto parts = split_whitespace(line);
    if (parts.size() < 3) return;
    std::vector<std::string> class_names;
    for (const auto& n : split_char(parts.back(), ',')) {
        std::string t = trim(n);
        if (!t.empty()) class_names.push_back(t);
    }
    if (class_names.empty()) return;
    std::string nodes_raw;
    for (size_t i = 1; i + 1 < parts.size(); ++i) {
        if (i > 1) nodes_raw += " ";
        nodes_raw += parts[i];
    }
    for (const auto& node_id : split_char(nodes_raw, ',')) {
        std::string id = trim(node_id);
        if (id.empty()) continue;
        for (const auto& cn : class_names) {
            g.node_classes[id].push_back(cn);
            g.subgraph_classes[id].push_back(cn);
        }
    }
}

void parse_style_line(const std::string& line, Graph& g) {
    auto parts = splitn(line, 3, [](char c) { return c == ' '; });
    std::string node_id = parts.size() > 1 ? trim(parts[1]) : "";
    std::string rest = parts.size() > 2 ? trim(parts[2]) : "";
    if (node_id.empty() || rest.empty()) return;
    NodeStyle style = parse_node_style(rest);
    for (const auto& raw : split_char(node_id, ',')) {
        std::string id = trim(raw);
        if (id.empty()) continue;
        g.node_styles[id] = style;
        g.subgraph_styles[id] = style;
    }
}

void parse_link_style_line(const std::string& line, Graph& g) {
    auto tokens = split_whitespace(trim(line));
    if (tokens.size() < 3) return;
    size_t style_idx = 0;
    bool found = false;
    for (size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i].find(':') != std::string::npos) {
            style_idx = i;
            found = true;
            break;
        }
    }
    if (!found) return;
    std::string style_str;
    for (size_t i = style_idx; i < tokens.size(); ++i) {
        if (i > style_idx) style_str += " ";
        style_str += tokens[i];
    }
    if (style_str.empty()) return;
    EdgeStyleOverride style = parse_edge_style(style_str);
    if (style_idx == 2 && tokens[1] == "default") {
        g.edge_style_default = style;
        return;
    }
    for (size_t i = 1; i < style_idx; ++i) {
        for (const auto& raw : split_char(tokens[i], ',')) {
            std::string tok = trim(raw);
            if (tok.empty()) continue;
            // Rust: token.parse::<usize>() (digits only).
            bool digits = true;
            for (char c : tok)
                if (c < '0' || c > '9') digits = false;
            if (!digits && !(tok[0] == '+' && tok.size() > 1)) continue;
            try {
                g.edge_styles[static_cast<size_t>(std::stoull(tok))] = style;
            } catch (...) {
            }
        }
    }
}

std::vector<std::string> tokenize_quoted(const std::string& input) {
    std::vector<std::string> tokens;
    std::string current;
    char quote = 0;
    bool escaped = false;
    for (char ch : input) {
        if (escaped) {
            current.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (quote) {
            if (ch == quote) quote = 0;
            else current.push_back(ch);
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            continue;
        }
        if (is_space(ch)) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

std::optional<std::pair<std::string, NodeLink>> parse_click_line(const std::string& line) {
    std::string t = trim(line);
    std::string lower = to_ascii_lower(t);
    size_t keyword_len;
    if (starts_with(lower, "click ")) keyword_len = 5;
    else if (starts_with(lower, "link ")) keyword_len = 4;
    else return std::nullopt;
    std::string rest = trim(t.substr(keyword_len));
    auto tokens = tokenize_quoted(rest);
    if (tokens.size() < 2) return std::nullopt;
    std::string id = tokens[0];
    size_t idx = 1;
    if (to_ascii_lower(tokens[idx]) == "call") return std::nullopt;
    if (to_ascii_lower(tokens[idx]) == "href") ++idx;
    if (idx >= tokens.size()) return std::nullopt;
    NodeLink link;
    link.url = tokens[idx];
    ++idx;
    if (idx < tokens.size()) {
        if (starts_with(tokens[idx], "_")) link.target = tokens[idx];
        else link.title = tokens[idx];
        ++idx;
    }
    if (!link.target && idx < tokens.size() && starts_with(tokens[idx], "_")) link.target = tokens[idx];
    return std::make_pair(id, link);
}

// ---- diagram kind detection ----

bool starts_with_diagram_header(const std::string& line, const std::string& keyword) {
    if (!starts_with(line, keyword)) return false;
    if (line.size() == keyword.size()) return true;
    unsigned char c = static_cast<unsigned char>(line[keyword.size()]);
    return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}

std::optional<DiagramKind> detect_diagram_kind(const std::string& input) {
    bool in_frontmatter = false;
    for (const std::string& raw : lines_of(input)) {
        std::string t = trim(raw);
        if (t.empty()) continue;
        if (t == "---") {
            in_frontmatter = !in_frontmatter;
            continue;
        }
        if (in_frontmatter) continue;
        if (starts_with(t, "%%")) continue;
        std::string without = strip_trailing_comment(t);
        if (without.empty()) continue;
        std::string lower = to_ascii_lower(without);
        struct K {
            const char* kw;
            DiagramKind kind;
        };
        static const K table[] = {
            {"sequencediagram", DiagramKind::Sequence}, {"classdiagram", DiagramKind::Class},
            {"statediagram", DiagramKind::State},       {"erdiagram", DiagramKind::Er},
            {"pie", DiagramKind::Pie},                  {"mindmap", DiagramKind::Mindmap},
            {"journey", DiagramKind::Journey},          {"timeline", DiagramKind::Timeline},
            {"gantt", DiagramKind::Gantt},              {"requirementdiagram", DiagramKind::Requirement},
            {"gitgraph", DiagramKind::GitGraph},        {"c4", DiagramKind::C4},
            {"c4context", DiagramKind::C4},             {"c4container", DiagramKind::C4},
            {"c4component", DiagramKind::C4},           {"c4dynamic", DiagramKind::C4},
            {"c4deployment", DiagramKind::C4},          {"sankey", DiagramKind::Sankey},
            {"quadrantchart", DiagramKind::Quadrant},   {"zenuml", DiagramKind::ZenUML},
            {"block", DiagramKind::Block},              {"packet", DiagramKind::Packet},
            {"kanban", DiagramKind::Kanban},            {"architecture", DiagramKind::Architecture},
            {"radar", DiagramKind::Radar},              {"treemap", DiagramKind::Treemap},
            {"xychart", DiagramKind::XYChart},          {"flowchart", DiagramKind::Flowchart},
            {"graph", DiagramKind::Flowchart},
        };
        for (const K& k : table)
            if (starts_with_diagram_header(lower, k.kw)) return k.kind;
        if (looks_like_flowchart_edge_syntax(without)) return DiagramKind::Flowchart;
        return std::nullopt;
    }
    return std::nullopt;
}

// ---- validator.rs (the checks that apply to flowcharts) ----

bool is_subgraph_open(const std::string& t) {
    std::string l = to_ascii_lower(t);
    return l == "subgraph" || starts_with(l, "subgraph ") || starts_with(l, "subgraph\t");
}
bool is_subgraph_close(const std::string& t) {
    std::string l = to_ascii_lower(t);
    return l == "end" || starts_with(l, "end ") || starts_with(l, "end\t") || starts_with(l, "end%%");
}
bool starts_with_arrow(const std::string& t) {
    if (t.empty()) return false;
    char c0 = t[0];
    if (!(c0 == '-' || c0 == '=' || c0 == '~' || c0 == '.' || c0 == '<')) return false;
    bool run_ok = false;
    for (char c : t) {
        if (!(c == '-' || c == '=' || c == '~' || c == '.' || c == '<' || c == '>' || c == 'o' || c == 'x')) break;
        if (c == '>' || c == '<' || c == '-' || c == '=') run_ok = true;
    }
    bool head = false;
    for (size_t i = 0; i < t.size() && i < 16; ++i)
        if (t[i] == '>' || t[i] == '<') head = true;
    return run_ok && head;
}

void validate(const std::string& input) {
    auto lines = lines_of(input);
    for (const auto& raw : lines) {
        std::string t(trim_start_view(raw));
        // validator.rs check_init_directive: fence, ':' and (for "init") a non-empty valid JSON5 body
        if (!starts_with(t, "%%{")) continue;
        std::string inside(trim_end_view(std::string_view(t).substr(3)));
        if (!ends_with(inside, "}%%")) throw ParseError("invalid directive: missing closing '}%%' fence");
        inside.resize(inside.size() - 3);
        size_t colon = inside.find(':');
        if (colon == std::string::npos) throw ParseError("invalid directive: missing ':' between directive name and body");
        if (trim(inside.substr(0, colon)) != "init") continue;
        std::string body = trim(inside.substr(colon + 1));
        if (body.empty()) throw ParseError("invalid directive: empty body");
        if (!json5_is_valid(body)) throw ParseError("invalid directive: JSON parse error");
    }
    // opener / `end` balance (validator.rs check_subgraph_balance): which keywords open a block depends
    // on the diagram header
    enum class Balance { Flowchart, Sequence, Block, Other };
    Balance kind = Balance::Other;
    {
        bool in_front = false, seen_any = false;
        for (const auto& raw : lines) {
            std::string t = trim(raw);
            if (t.empty() || starts_with(t, "%%")) continue;
            if (t == "---") {
                if (!seen_any) { in_front = true; seen_any = true; continue; }
                if (in_front) { in_front = false; continue; }
            }
            seen_any = true;
            if (in_front) continue;
            std::string lower = to_ascii_lower(t);
            if (starts_with(lower, "flowchart") || starts_with(lower, "graph")) kind = Balance::Flowchart;
            else if (starts_with(lower, "sequencediagram")) kind = Balance::Sequence;
            else if (starts_with(lower, "block-beta") || lower == "block" || starts_with(lower, "block ")) kind = Balance::Block;
            break;
        }
    }
    if (kind != Balance::Other) {
        auto opens = [&](const std::string& t) {
            std::string l = to_ascii_lower(t);
            switch (kind) {
                case Balance::Flowchart: return is_subgraph_open(t);
                case Balance::Sequence:
                    for (const char* kw : {"alt", "opt", "loop", "par", "rect", "critical", "break", "box"}) {
                        std::string k = kw;
                        if (l == k || starts_with(l, k + " ") || starts_with(l, k + "\t")) return true;
                    }
                    return false;
                case Balance::Block: return starts_with(l, "block:");
                default: return false;
            }
        };
        int open = 0;
        for (const auto& raw : lines) {
            std::string t = trim(raw);
            if (t.empty() || starts_with(t, "%%")) continue;
            if (opens(t)) ++open;
            else if (is_subgraph_close(t)) {
                if (open == 0) throw ParseError("unexpected end");
                --open;
            }
        }
        if (open != 0) throw ParseError("unclosed block");
    }
    for (const auto& raw : lines) {
        std::string t(trim_start_view(raw));
        if (t.empty() || starts_with(t, "%%")) continue;
        if (starts_with_arrow(t)) throw ParseError("line starts with an arrow");
    }
    for (const auto& raw : lines) {
        std::string t(trim_start_view(raw));
        if (!starts_with(t, "click ") && !starts_with(t, "click\t")) continue;
        size_t q = 0;
        for (char c : t)
            if (c == '"') ++q;
        if (q % 2 == 1) throw ParseError("unbalanced quote in click");
    }
    // sequence diagrams: arrows may only name declared participants (when any are declared)
    {
        std::string first;
        for (const auto& raw : lines) {
            std::string t = trim(raw);
            if (t.empty() || starts_with(t, "%%")) continue;
            first = to_ascii_lower(t);
            break;
        }
        if (starts_with(first, "sequencediagram")) {
            std::vector<std::string> declared;
            for (const auto& raw : lines) {
                std::string t = trim(raw);
                std::string rest;
                if (starts_with(t, "participant ")) rest = t.substr(12);
                else if (starts_with(t, "actor ")) rest = t.substr(6);
                else continue;
                rest = trim(rest);
                size_t as = rest.find(" as ");
                if (as != std::string::npos) {
                    std::string name = trim(rest.substr(0, as)), alias = trim(rest.substr(as + 4));
                    if (!name.empty()) declared.push_back(name);
                    if (!alias.empty()) declared.push_back(alias);
                } else if (!rest.empty()) {
                    declared.push_back(rest);
                }
            }
            if (!declared.empty()) {
                for (const auto& raw : lines) {
                    std::string t = trim(raw);
                    if (t.empty() || starts_with(t, "%%")) continue;
                    size_t colon = t.find(':');
                    std::string before = colon == std::string::npos ? t : t.substr(0, colon);
                    for (const char* pat : {"-->>", "--x", "--)", "-->", "->>", "->", "-x", "-)"}) {
                        size_t at = before.find(pat);
                        if (at == std::string::npos) continue;
                        for (const std::string& name : {trim(before.substr(0, at)), trim(before.substr(at + std::strlen(pat)))}) {
                            if (name.empty()) continue;
                            if (std::find(declared.begin(), declared.end(), name) == declared.end())
                                throw ParseError("unknown participant " + name);
                        }
                        break;
                    }
                }
            }
        }
    }
}

// ---- sequence diagrams (parser.rs: parse_sequence_diagram and helpers) ----

std::string lstrip_char(std::string s, char c) {
    size_t a = 0;
    while (a < s.size() && s[a] == c) ++a;
    return s.substr(a);
}

// Participant declaration: (id, display label, shape) or nullopt.
std::optional<std::tuple<std::string, std::optional<std::string>, NodeShape>> parse_sequence_participant(const std::string& line) {
    std::string lowered = to_ascii_lower(line);
    struct Kw {
        const char* word;
        NodeShape shape;
    };
    static const Kw keywords[] = {{"participant ", NodeShape::ActorBox}, {"actor ", NodeShape::ActorBox},
                                  {"boundary ", NodeShape::ActorBox},    {"control ", NodeShape::ActorBox},
                                  {"entity ", NodeShape::ActorBox},      {"database ", NodeShape::Cylinder}};
    std::optional<std::string> rest;
    NodeShape shape = NodeShape::ActorBox;
    for (const Kw& k : keywords) {
        if (starts_with(lowered, k.word)) {
            rest = trim(line.substr(std::strlen(k.word)));
            shape = k.shape;
            break;
        }
    }
    if (!rest || rest->empty()) return std::nullopt;

    std::string lower_rest = to_ascii_lower(*rest);
    size_t as_idx = lower_rest.find(" as ");
    if (as_idx != std::string::npos) {
        std::string label_part = trim(rest->substr(0, as_idx));
        std::string id_part = trim(rest->substr(as_idx + 4));
        if (id_part.empty()) return std::nullopt;
        return std::make_tuple(strip_quotes(label_part), std::optional<std::string>(strip_quotes(id_part)), shape);
    }
    if (rest->front() == '"' && rest->back() == '"') {
        std::string label = strip_quotes(*rest);
        return std::make_tuple(label, std::optional<std::string>(label), shape);
    }
    return std::make_tuple(strip_quotes(*rest), std::optional<std::string>(), shape);
}

bool is_color_token(const std::string& token) {
    std::string lower = to_ascii_lower(trim(token));
    return lower == "transparent" || starts_with(lower, "#") || starts_with(lower, "rgb(") || starts_with(lower, "rgba(") ||
           starts_with(lower, "hsl(") || starts_with(lower, "hsla(");
}

std::string join_tokens(const std::vector<std::string>& t, size_t from) {
    std::string out;
    for (size_t i = from; i < t.size(); ++i) {
        if (i > from) out += " ";
        out += t[i];
    }
    return out;
}

// `box [colour] [label]`: (colour, label) or nullopt.
std::optional<std::pair<std::optional<std::string>, std::optional<std::string>>> parse_sequence_box_line(const std::string& line) {
    std::string trimmed = trim(line);
    std::string lower = to_ascii_lower(trimmed);
    if (!starts_with(lower, "box")) return std::nullopt;
    std::string rest = trim(trimmed.substr(3));
    using Result = std::pair<std::optional<std::string>, std::optional<std::string>>;
    if (rest.empty()) return Result{std::nullopt, std::nullopt};
    std::vector<std::string> tokens = tokenize_quoted(rest);
    if (tokens.empty()) return Result{std::nullopt, std::nullopt};
    std::string first = tokens[0];
    auto non_blank = [](const std::string& s) { return !trim_view(s).empty(); };
    if (to_ascii_lower(first) == "transparent") {
        std::string label = join_tokens(tokens, 1);
        return Result{std::nullopt, non_blank(label) ? std::optional<std::string>(label) : std::nullopt};
    }
    std::optional<std::string> color, label;
    if (tokens.size() > 1) color = first;
    else if (is_color_token(first)) color = first;
    if (tokens.size() > 1) label = join_tokens(tokens, 1);
    else if (!color) label = first;
    if (label && !non_blank(*label)) label.reset();
    if (color && to_ascii_lower(*color) == "transparent") color.reset();
    return Result{color, label};
}

void ensure_sequence_node(Graph& g, const std::map<std::string, std::string>& labels, const std::string& id,
                          std::optional<NodeShape> shape) {
    std::optional<std::string> label;
    if (auto it = labels.find(id); it != labels.end()) label = it->second;
    if (shape) {
        g.ensure_node(id, label, shape);
        return;
    }
    if (g.nodes.count(id)) g.ensure_node(id, label, std::nullopt);
    else g.ensure_node(id, label, NodeShape::ActorBox);
}

std::pair<std::string, std::optional<std::string>> split_label(const std::string& input) {
    size_t colon = input.find(':');
    if (colon != std::string::npos) {
        std::string label = trim(input.substr(colon + 1));
        std::string target = trim(input.substr(0, colon));
        if (!label.empty()) return {target, label};
        return {target, std::nullopt};
    }
    return {trim(input), std::nullopt};
}

struct SequenceMessage {
    std::string from, to;
    std::optional<std::string> label;
    EdgeStyle style;
    std::optional<SequenceActivationKind> activation;
};

std::optional<SequenceMessage> parse_sequence_message(const std::string& line) {
    static const char* tokens[] = {"-->>+", "->>+", "-->+", "->+", "-->>-", "->>-", "-->-", "->-", "<--+", "<-+",
                                   "<--",   "<-",   "-->>", "->>", "-->",   "->"};
    for (const char* tk : tokens) {
        std::string token = tk;
        size_t pos = line.find(token);
        if (pos == std::string::npos) continue;
        std::string left = trim(line.substr(0, pos));
        std::string right_part = trim(line.substr(pos + token.size()));
        if (left.empty() || right_part.empty()) continue;
        auto [right, label] = split_label(right_part);
        std::string from = left, to = right;
        if (token[0] == '<') std::swap(from, to);
        // token.trim_start_matches('<').trim_end_matches(['+', '-'])
        std::string trimmed = lstrip_char(token, '<');
        while (!trimmed.empty() && (trimmed.back() == '+' || trimmed.back() == '-')) trimmed.pop_back();
        EdgeStyle style = starts_with(trimmed, "--") ? EdgeStyle::Dotted : EdgeStyle::Solid;
        std::optional<SequenceActivationKind> activation;
        if (token.back() == '+') activation = SequenceActivationKind::Activate;
        else if (token.back() == '-') activation = SequenceActivationKind::Deactivate;
        return SequenceMessage{from, to, label, style, activation};
    }
    return std::nullopt;
}

struct ParsedNote {
    SequenceNotePosition position;
    std::vector<std::string> participants;
    std::string label;
};

std::optional<ParsedNote> parse_sequence_note(const std::string& line) {
    std::string trimmed = trim(line);
    std::string lower = to_ascii_lower(trimmed);
    if (!starts_with(lower, "note ")) return std::nullopt;
    std::string rest = trim(trimmed.substr(4));
    std::string lower_rest = to_ascii_lower(rest);
    SequenceNotePosition position;
    std::string targets_part;
    if (starts_with(lower_rest, "left of ")) { position = SequenceNotePosition::LeftOf; targets_part = trim(rest.substr(8)); }
    else if (starts_with(lower_rest, "right of ")) { position = SequenceNotePosition::RightOf; targets_part = trim(rest.substr(9)); }
    else if (starts_with(lower_rest, "over ")) { position = SequenceNotePosition::Over; targets_part = trim(rest.substr(5)); }
    else return std::nullopt;

    size_t colon = targets_part.find(':');
    if (colon == std::string::npos) return std::nullopt;
    std::string targets = targets_part.substr(0, colon);
    std::string label = trim(targets_part.substr(colon + 1));
    if (label.empty()) return std::nullopt;
    std::vector<std::string> participants;
    for (const std::string& part : split_char(targets, ',')) {
        std::string p = strip_quotes(trim(part));
        if (!p.empty()) participants.push_back(p);
    }
    if (participants.empty()) return std::nullopt;
    return ParsedNote{position, participants, label};
}

bool contains_str(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

// Label after a keyword of `offset` bytes ("alt Some text"): None when empty.
std::optional<std::string> frame_label(const std::string& line, size_t offset) {
    if (offset > line.size()) return std::nullopt;
    std::string label = trim(line.substr(offset));
    if (label.empty()) return std::nullopt;
    return strip_quotes(label);
}

Graph parse_sequence_diagram(const std::string& input) {
    Graph g;
    g.kind = DiagramKind::Sequence;
    g.direction = Direction::LeftRight;
    std::vector<std::string> lines = preprocess_input(input);

    std::map<std::string, std::string> labels;
    std::vector<std::string> order;
    std::vector<SequenceFrame> open_frames, frames;
    std::vector<SequenceBox> open_boxes;
    auto remember = [&](const std::string& id) {
        if (!contains_str(order, id)) order.push_back(id);
    };

    for (const std::string& raw_line : lines) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        std::string lower = to_ascii_lower(line);
        if (starts_with(lower, "sequencediagram")) continue;
        if (auto part = parse_sequence_participant(line)) {
            auto& [id, label, shape] = *part;
            remember(id);
            if (label) labels[id] = *label;
            ensure_sequence_node(g, labels, id, shape);
            if (!open_boxes.empty() && !contains_str(open_boxes.back().participants, id)) open_boxes.back().participants.push_back(id);
            continue;
        }
        if (auto box = parse_sequence_box_line(line)) {
            SequenceBox b;
            b.color = box->first;
            b.label = box->second;
            open_boxes.push_back(std::move(b));
            continue;
        }
        auto is_kw = [&](const char* kw) {
            std::string k = kw;
            return lower == k || starts_with(lower, k + " ");
        };
        if (is_kw("alt") || is_kw("opt") || is_kw("loop") || is_kw("par") || is_kw("rect") || is_kw("critical") || is_kw("break")) {
            SequenceFrameKind kind;
            size_t offset;
            if (starts_with(lower, "opt")) { kind = SequenceFrameKind::Opt; offset = 3; }
            else if (starts_with(lower, "loop")) { kind = SequenceFrameKind::Loop; offset = 4; }
            else if (starts_with(lower, "par")) { kind = SequenceFrameKind::Par; offset = 3; }
            else if (starts_with(lower, "rect")) { kind = SequenceFrameKind::Rect; offset = 4; }
            else if (starts_with(lower, "critical")) { kind = SequenceFrameKind::Critical; offset = 8; }
            else if (starts_with(lower, "break")) { kind = SequenceFrameKind::Break; offset = 5; }
            else { kind = SequenceFrameKind::Alt; offset = 3; }
            size_t start_idx = g.edges.size();
            SequenceFrame f;
            f.kind = kind;
            f.sections.push_back({frame_label(line, offset), start_idx, start_idx});
            f.start_idx = f.end_idx = start_idx;
            open_frames.push_back(std::move(f));
            continue;
        }
        // else / and / option: start a new section of the open frame
        auto new_section = [&](size_t offset, std::optional<SequenceFrameKind> only_kind) {
            if (open_frames.empty()) return;
            SequenceFrame& frame = open_frames.back();
            if (only_kind && frame.kind != *only_kind) return;
            size_t split_idx = g.edges.size();
            if (!frame.sections.empty()) frame.sections.back().end_idx = split_idx;
            frame.sections.push_back({frame_label(line, offset), split_idx, split_idx});
        };
        if (is_kw("else")) { new_section(4, std::nullopt); continue; }
        if (is_kw("and")) { new_section(3, SequenceFrameKind::Par); continue; }
        if (is_kw("option")) { new_section(6, SequenceFrameKind::Critical); continue; }
        if (lower == "end") {
            if (!open_frames.empty()) {
                SequenceFrame frame = std::move(open_frames.back());
                open_frames.pop_back();
                size_t end_idx = g.edges.size();
                if (!frame.sections.empty()) frame.sections.back().end_idx = end_idx;
                frame.end_idx = end_idx;
                frames.push_back(std::move(frame));
            } else if (!open_boxes.empty()) {
                g.sequence_boxes.push_back(std::move(open_boxes.back()));
                open_boxes.pop_back();
            }
            continue;
        }
        if (auto note = parse_sequence_note(line)) {
            for (const auto& id : note->participants) {
                remember(id);
                ensure_sequence_node(g, labels, id, std::nullopt);
            }
            g.sequence_notes.push_back({note->position, note->participants, note->label, g.edges.size()});
            continue;
        }
        auto activation_line = [&](const char* kw, SequenceActivationKind kind) {
            std::string k = kw;
            if (!starts_with(lower, k)) return false;
            std::string id = trim(line.substr(k.size()));
            if (!id.empty()) {
                id = strip_quotes(id);
                remember(id);
                ensure_sequence_node(g, labels, id, std::nullopt);
                g.sequence_activations.push_back({id, g.edges.size(), kind});
            }
            return true;
        };
        if (activation_line("activate ", SequenceActivationKind::Activate)) continue;
        if (activation_line("deactivate ", SequenceActivationKind::Deactivate)) continue;
        if (starts_with(lower, "autonumber")) {
            std::vector<std::string> parts = split_whitespace(line);
            if (parts.size() >= 2) {
                std::string token = to_ascii_lower(parts[1]);
                char* end = nullptr;
                unsigned long long v = std::strtoull(parts[1].c_str(), &end, 10);
                if (token == "off" || token == "stop" || token == "disable") g.sequence_autonumber.reset();
                else if (!parts[1].empty() && *end == '\0' && parts[1][0] != '-')
                    g.sequence_autonumber = static_cast<size_t>(v);
                else g.sequence_autonumber = 1;
            } else {
                g.sequence_autonumber = 1;
            }
            continue;
        }
        if (auto msg = parse_sequence_message(line)) {
            remember(msg->from);
            remember(msg->to);
            ensure_sequence_node(g, labels, msg->from, std::nullopt);
            ensure_sequence_node(g, labels, msg->to, std::nullopt);
            Edge e;
            e.from = msg->from;
            e.to = msg->to;
            e.label = msg->label;
            e.directed = true;
            e.arrow_end = true;
            e.style = msg->style;
            g.edges.push_back(std::move(e));
            if (msg->activation) g.sequence_activations.push_back({g.edges.back().to, g.edges.size() - 1, *msg->activation});
        }
    }
    while (!open_frames.empty()) {
        SequenceFrame frame = std::move(open_frames.back());
        open_frames.pop_back();
        size_t end_idx = g.edges.size();
        if (!frame.sections.empty()) frame.sections.back().end_idx = end_idx;
        frame.end_idx = end_idx;
        frames.push_back(std::move(frame));
    }
    while (!open_boxes.empty()) {
        g.sequence_boxes.push_back(std::move(open_boxes.back()));
        open_boxes.pop_back();
    }
    g.sequence_participants = order;
    g.sequence_frames = frames;
    return g;
}

Graph parse_flowchart(const std::string& input) {
    Graph g;
    g.kind = DiagramKind::Flowchart;
    std::vector<size_t> subgraph_stack;

    for (const std::string& raw_line : preprocess_input(input)) {
        for (const std::string& line : split_statements(raw_line)) {
            if (line.empty()) continue;

            std::smatch m;
            if (std::regex_search(line, m, header_re())) {
                if (auto dir = direction_from_token(m[2].str())) g.direction = *dir;
                continue;
            }
            if (line == "end") {
                if (!subgraph_stack.empty()) subgraph_stack.pop_back();
                continue;
            }
            if (std::regex_search(line, m, subgraph_re())) {
                auto [id, label, classes] = parse_subgraph_header(m[1].str());
                Subgraph sg;
                sg.id = id;
                sg.label = label;
                g.subgraphs.push_back(std::move(sg));
                subgraph_stack.push_back(g.subgraphs.size() - 1);
                if (id) apply_subgraph_classes(g, *id, classes);
                continue;
            }
            if (auto direction = parse_direction_line(line)) {
                if (!subgraph_stack.empty()) g.subgraphs[subgraph_stack.back()].direction = direction;
                else g.direction = *direction;
                continue;
            }
            if (starts_with(line, "classDef")) { parse_class_def(line, g); continue; }
            if (starts_with(line, "class ")) { parse_class_line(line, g); continue; }
            if (starts_with(line, "style ")) { parse_style_line(line, g); continue; }
            if (starts_with(line, "linkStyle")) { parse_link_style_line(line, g); continue; }
            if (auto click = parse_click_line(line)) {
                g.node_links[click->first] = click->second;
                continue;
            }
            if (starts_with(line, "accTitle") || starts_with(line, "accDescr") || starts_with(line, "title "))
                continue;
            if (has_missing_flowchart_edge_endpoint(line))
                throw ParseError("invalid flowchart edge syntax: " + line);

            if (auto chain = split_edge_chain(line)) {
                bool added = false;
                for (const auto& edge_line : *chain) {
                    if (add_flowchart_edge(edge_line, g, subgraph_stack)) added = true;
                    else if (looks_like_flowchart_edge_syntax(edge_line))
                        throw ParseError("invalid flowchart edge syntax: " + edge_line);
                }
                if (added) continue;
            }
            if (add_flowchart_edge(line, g, subgraph_stack)) continue;
            if (looks_like_flowchart_edge_syntax(line))
                throw ParseError("invalid flowchart edge syntax: " + line);

            if (auto node = parse_node_only(line)) {
                auto& [id, label, shape, classes] = *node;
                g.ensure_node(id, label, shape);
                apply_node_classes(g, id, classes);
                add_node_to_subgraphs(g, subgraph_stack, id);
            }
        }
    }
    return g;
}

}  // namespace

// Thin exports of the helpers above for the per-diagram parser files (parser_internal.h).
namespace pp {
std::vector<std::string> preprocess_input(const std::string& input) { return mermaid::preprocess_input(input); }
bool parse_init_directive(const std::string& trimmed_line) { return mermaid::parse_init_directive(trimmed_line); }
std::vector<std::string> split_statements(const std::string& line) { return mermaid::split_statements(line); }
std::string strip_quotes(const std::string& input) { return mermaid::strip_quotes(input); }
std::pair<std::string, std::vector<std::string>> split_inline_classes(const std::string& token) {
    return mermaid::split_inline_classes(token);
}
std::optional<Direction> parse_direction_line(const std::string& line) { return mermaid::parse_direction_line(line); }
std::optional<Direction> direction_from_token(const std::string& token) { return mermaid::direction_from_token(token); }
void parse_class_def(const std::string& line, Graph& g) { mermaid::parse_class_def(line, g); }
void parse_class_line(const std::string& line, Graph& g) { mermaid::parse_class_line(line, g); }
void parse_style_line(const std::string& line, Graph& g) { mermaid::parse_style_line(line, g); }
void apply_node_classes(Graph& g, const std::string& id, const std::vector<std::string>& classes) {
    mermaid::apply_node_classes(g, id, classes);
}
void add_node_to_subgraphs(Graph& g, const std::vector<size_t>& stack, const std::string& id) {
    mermaid::add_node_to_subgraphs(g, stack, id);
}
std::pair<std::string, std::optional<std::string>> split_label(const std::string& input) { return mermaid::split_label(input); }
bool add_flowchart_edge(const std::string& line, Graph& g, const std::vector<size_t>& stack) {
    return mermaid::add_flowchart_edge(line, g, stack);
}
std::tuple<std::string, std::optional<std::string>, std::optional<NodeShape>, std::vector<std::string>> parse_node_token(
    const std::string& token) {
    return mermaid::parse_node_token(token);
}
}  // namespace pp

// parser.rs validate_init_directives(): every non-frontmatter line is checked as a possible init directive.
void validate_init_directives(const std::string& input) {
    bool in_frontmatter = false;
    for (const std::string& raw : lines_of(input)) {
        std::string t = trim(raw);
        if (t.empty()) continue;
        if (t == "---") {
            in_frontmatter = !in_frontmatter;
            continue;
        }
        if (in_frontmatter) continue;
        parse_init_directive(t);
    }
}

Graph parse_mermaid(const std::string& input) {
    validate(input);
    validate_init_directives(input);
    auto kind = detect_diagram_kind(input);
    if (!kind) throw ParseError("unknown or missing Mermaid diagram header");
    switch (*kind) {
    case DiagramKind::Sequence: return parse_sequence_diagram(input);
    case DiagramKind::Flowchart: return parse_flowchart(input);
    case DiagramKind::Class: return pp::parse_class_diagram(input);
    case DiagramKind::Er: return pp::parse_er_diagram(input);
    case DiagramKind::State: return pp::parse_state_diagram(input);
    case DiagramKind::ZenUML: return parse_zenuml_diagram(input);
    case DiagramKind::Requirement: return parse_requirement_diagram(input);
    case DiagramKind::Pie: return parse_pie_diagram(input);
    case DiagramKind::Gantt: return parse_gantt_diagram(input);
    case DiagramKind::Journey: return parse_journey_diagram(input);
    case DiagramKind::Timeline: return parse_timeline_diagram(input);
    case DiagramKind::Quadrant: return parse_quadrant_diagram(input);
    case DiagramKind::XYChart: return parse_xychart_diagram(input);
    case DiagramKind::Radar: return parse_radar_diagram(input);
    case DiagramKind::Kanban: return parse_kanban_diagram(input);
    case DiagramKind::Packet: return parse_packet_diagram(input);
    case DiagramKind::Treemap: return parse_treemap_diagram(input);
    case DiagramKind::Sankey: return parse_sankey_diagram(input);
    case DiagramKind::Block: return parse_block_diagram(input);
    case DiagramKind::Architecture: return parse_architecture_diagram(input);
    case DiagramKind::Mindmap: return parse_mindmap_diagram(input);
    case DiagramKind::GitGraph: return parse_gitgraph_diagram(input);
    case DiagramKind::C4: return parse_c4_diagram(input);
    default: throw Unsupported("diagram type not ported yet");
    }
}

}  // namespace mermaid
