// Mermaid port: parsers of the diagram types that use the flowchart layout (class, ER, state).
// Mirrors parser.rs: parse_class_diagram, parse_er_diagram, parse_state_diagram and their helpers
// (function names match the Rust ones so the two sources can be read side by side).
#include <deque>
#include <map>

#include "parser_internal.h"
#include "strutil.h"

namespace mermaid::pp {

namespace {

// Rust str::trim_start_matches(prefix): strips the prefix as often as it repeats.
std::string trim_start_matches(std::string s, const std::string& prefix) {
    while (!prefix.empty() && starts_with(s, prefix)) s.erase(0, prefix.size());
    return s;
}

// Rust `s[a..b]` (panics on bad ranges; the app reports that as an error).
std::string slice(const std::string& s, size_t a, size_t b) {
    if (a > b || b > s.size()) throw ParseError("invalid slice in parser");
    return s.substr(a, b - a);
}

struct EdgeMeta {
    bool directed = false, arrow_start = false, arrow_end = false;
    std::optional<EdgeArrowhead> arrow_start_kind, arrow_end_kind;
    std::optional<EdgeDecoration> start_decoration, end_decoration;
    EdgeStyle style = EdgeStyle::Solid;
};

bool contains(const std::string& s, char c) { return s.find(c) != std::string::npos; }
bool contains(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

// ======================================================================================
// class diagrams
// ======================================================================================

// split_trailing_quoted: `before "value"` -> (before, value).
std::optional<std::pair<std::string, std::string>> split_trailing_quoted(const std::string& input) {
    std::string trimmed(trim_end_view(input));
    if (trimmed.empty()) return std::nullopt;
    char quote = trimmed.back();
    if (quote != '"' && quote != '\'') return std::nullopt;
    for (size_t i = trimmed.size() - 1; i-- > 0;) {
        if (trimmed[i] == quote) return std::make_pair(trimmed.substr(0, i), trimmed.substr(i + 1, trimmed.size() - 1 - (i + 1)));
    }
    return std::nullopt;
}

// split_leading_quoted: `"value" rest` -> (value, rest).
std::optional<std::pair<std::string, std::string>> split_leading_quoted(const std::string& input) {
    std::string trimmed(trim_start_view(input));
    if (trimmed.empty()) return std::nullopt;
    char quote = trimmed[0];
    if (quote != '"' && quote != '\'') return std::nullopt;
    for (size_t i = 1; i < trimmed.size(); ++i)
        if (trimmed[i] == quote) return std::make_pair(trimmed.substr(1, i - 1), trimmed.substr(i + 1));
    return std::nullopt;
}

std::pair<std::string, std::optional<std::string>> split_multiplicity_left(const std::string& input) {
    std::string trimmed = trim(input);
    if (trimmed.empty()) return {"", std::nullopt};
    if (auto q = split_trailing_quoted(trimmed)) {
        std::string before = trim(q->first);
        if (!before.empty() && !q->second.empty()) return {before, q->second};
    }
    return {trimmed, std::nullopt};
}

std::pair<std::string, std::optional<std::string>> split_multiplicity_right(const std::string& input) {
    std::string trimmed = trim(input);
    if (trimmed.empty()) return {"", std::nullopt};
    if (auto q = split_leading_quoted(trimmed)) {
        std::string rest = trim(q->second);
        if (!rest.empty() && !q->first.empty()) return {rest, q->first};
    }
    return {trimmed, std::nullopt};
}

EdgeMeta edge_meta_from_class_token(const std::string& token) {
    EdgeMeta m;
    m.arrow_start = contains(token, '<');
    m.arrow_end = contains(token, '>');
    m.directed = m.arrow_start || m.arrow_end;
    m.style = contains(token, "..") ? EdgeStyle::Dotted : EdgeStyle::Solid;
    if (token.front() == '*') m.start_decoration = EdgeDecoration::DiamondFilled;
    if (token.back() == '*') m.end_decoration = EdgeDecoration::DiamondFilled;
    if (token.front() == 'o') m.start_decoration = EdgeDecoration::Diamond;
    if (token.back() == 'o') m.end_decoration = EdgeDecoration::Diamond;
    if (contains(token, '|')) {
        if (m.arrow_start) m.arrow_start_kind = EdgeArrowhead::OpenTriangle;
        if (m.arrow_end) m.arrow_end_kind = EdgeArrowhead::OpenTriangle;
    } else {
        if (m.arrow_start) m.arrow_start_kind = EdgeArrowhead::ClassDependency;
        if (m.arrow_end) m.arrow_end_kind = EdgeArrowhead::ClassDependency;
    }
    return m;
}

struct ClassRelation {
    std::string left, right;
    EdgeMeta meta;
    std::optional<std::string> label, start_label, end_label;
};

std::optional<ClassRelation> parse_class_relation_line(const std::string& line) {
    static const char* tokens[] = {"<|..", "..|>", "<|--", "--|>", "*--", "--*", "o--",
                                   "--o",  "<..",  "..>",  "<--",  "-->", "..",  "--"};
    for (const char* token : tokens) {
        size_t pos = line.find(token);
        if (pos == std::string::npos) continue;
        std::string tk = token;
        std::string left = trim(line.substr(0, pos));
        std::string right_part = trim(line.substr(pos + tk.size()));
        if (left.empty() || right_part.empty()) continue;
        auto [right0, label] = split_label(right_part);
        auto [left1, start_label] = split_multiplicity_left(left);
        auto [right1, end_label] = split_multiplicity_right(right0);
        return ClassRelation{left1, right1, edge_meta_from_class_token(tk), label, start_label, end_label};
    }
    return std::nullopt;
}

struct ClassDeclaration {
    std::string id;
    std::optional<std::string> label, body;
    bool open_body;
};

std::optional<ClassDeclaration> parse_class_declaration(const std::string& input) {
    std::string rest = trim(input);
    if (rest.empty()) return std::nullopt;
    std::optional<std::string> body;
    bool open_body = false;
    size_t open_idx = rest.find('{');
    if (open_idx != std::string::npos) {
        std::string header = trim(rest.substr(0, open_idx));
        std::string tail = trim(rest.substr(open_idx + 1));
        size_t close_idx = tail.find('}');
        if (close_idx != std::string::npos) {
            std::string body_str = trim(tail.substr(0, close_idx));
            if (!body_str.empty()) body = body_str;
        } else {
            open_body = true;
        }
        rest = header;
    }
    std::string lower = to_ascii_lower(rest);
    size_t as_idx = lower.find(" as ");
    if (as_idx != std::string::npos) {
        std::string label_part = trim(rest.substr(0, as_idx));
        std::string id_part = trim(rest.substr(as_idx + 4));
        if (!id_part.empty()) return ClassDeclaration{id_part, strip_quotes(label_part), body, open_body};
    }
    if (!rest.empty() && rest.front() == '"' && rest.back() == '"') {
        std::string label = strip_quotes(rest);
        return ClassDeclaration{label, label, body, open_body};
    }
    return ClassDeclaration{strip_quotes(rest), std::nullopt, body, open_body};
}

std::vector<std::string> split_class_body(const std::string& body) {
    std::vector<std::string> entries;
    size_t pos = 0;
    while (true) {
        size_t j = body.find(';', pos);
        std::string part = body.substr(pos, j == std::string::npos ? std::string::npos : j - pos);
        std::string trimmed = trim(part);
        if (!trimmed.empty())
            for (const std::string& line : lines_of(trimmed)) {
                std::string t = trim(line);
                if (!t.empty()) entries.push_back(t);
            }
        if (j == std::string::npos) break;
        pos = j + 1;
    }
    return entries;
}

std::string normalize_class_method_signature(const std::string& entry) {
    std::string trimmed = trim(entry);
    size_t close_idx = trimmed.find(')');
    if (close_idx == std::string::npos) return trimmed;
    std::string sig = trimmed.substr(0, close_idx + 1);
    std::string rest = trim(trimmed.substr(close_idx + 1));
    if (rest.empty()) return trimmed;
    if (rest.front() == ':') return sig + " " + rest;
    if (contains(trimmed, "):") || contains(trimmed, ") :")) return trimmed;
    return sig + " : " + rest;
}

std::optional<std::pair<std::string, std::string>> parse_class_member_line(const std::string& line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return std::nullopt;
    std::string id = trim(line.substr(0, colon)), member = trim(line.substr(colon + 1));
    if (id.empty() || member.empty()) return std::nullopt;
    if (contains(id, ' ')) return std::nullopt;
    return std::make_pair(id, member);
}

std::pair<std::string, std::optional<std::string>> normalize_class_id(const std::string& token) {
    std::string trimmed = trim(token);
    if (!trimmed.empty() && trimmed.front() == '"' && trimmed.back() == '"') {
        std::string label = strip_quotes(trimmed);
        return {label, label};
    }
    return {trimmed, std::nullopt};
}

bool is_class_stereotype(const std::string& entry) {
    std::string t = trim(entry);
    return starts_with(t, "<<") && ends_with(t, ">>") && t.size() > 4;
}

}  // namespace

Graph parse_class_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::Class;
    graph.direction = Direction::TopDown;
    std::vector<std::string> lines = preprocess_input(input);

    std::map<std::string, std::vector<std::string>> members, stereotypes;
    std::map<std::string, std::string> labels;
    std::optional<std::string> current_class;

    auto add_member = [&](const std::string& id, const std::string& entry) {
        if (is_class_stereotype(entry)) stereotypes[id].push_back(entry);
        else members[id].push_back(entry);
    };

    for (const std::string& raw_line : lines) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        std::string lower = to_ascii_lower(line);
        if (starts_with(lower, "classdiagram")) {
            auto parts = split_whitespace(line);
            if (parts.size() > 1)
                if (auto dir = direction_from_token(parts[1])) graph.direction = *dir;
            continue;
        }
        if (auto direction = parse_direction_line(line)) {
            graph.direction = *direction;
            continue;
        }

        if (current_class) {
            const std::string active = *current_class;
            size_t end_idx = line.find('}');
            if (end_idx != std::string::npos) {
                std::string fragment = trim(line.substr(0, end_idx));
                if (!fragment.empty()) add_member(active, fragment);
                current_class.reset();
            } else if (is_class_stereotype(trim(line))) {
                stereotypes[active].push_back(trim(line));
            } else {
                members[active].push_back(line);
            }
            continue;
        }

        if (auto rel = parse_class_relation_line(line)) {
            auto [left_id, left_label] = normalize_class_id(rel->left);
            auto [right_id, right_label] = normalize_class_id(rel->right);
            if (left_label) labels[left_id] = *left_label;
            if (right_label) labels[right_id] = *right_label;
            auto label_of = [&](const std::string& id) -> std::optional<std::string> {
                auto it = labels.find(id);
                if (it == labels.end()) return std::nullopt;
                return it->second;
            };
            graph.ensure_node(left_id, label_of(left_id), NodeShape::Rectangle);
            graph.ensure_node(right_id, label_of(right_id), NodeShape::Rectangle);
            Edge e;
            e.from = left_id;
            e.to = right_id;
            e.label = rel->label;
            e.start_label = rel->start_label;
            e.end_label = rel->end_label;
            e.directed = rel->meta.directed;
            e.arrow_start = rel->meta.arrow_start;
            e.arrow_end = rel->meta.arrow_end;
            e.arrow_start_kind = rel->meta.arrow_start_kind;
            e.arrow_end_kind = rel->meta.arrow_end_kind;
            e.start_decoration = rel->meta.start_decoration;
            e.end_decoration = rel->meta.end_decoration;
            e.style = rel->meta.style;
            graph.edges.push_back(std::move(e));
            continue;
        }

        if (starts_with(line, "class ")) {
            std::string rest = trim(trim_start_matches(line, "class "));
            if (auto decl = parse_class_declaration(rest)) {
                if (decl->label) labels[decl->id] = *decl->label;
                std::optional<std::string> label;
                if (auto it = labels.find(decl->id); it != labels.end()) label = it->second;
                graph.ensure_node(decl->id, label, NodeShape::Rectangle);
                if (decl->body)
                    for (const std::string& entry : split_class_body(*decl->body))
                        if (!entry.empty()) add_member(decl->id, entry);
                if (decl->open_body) current_class = decl->id;
                continue;
            }
        }

        if (auto mem = parse_class_member_line(line)) {
            add_member(mem->first, mem->second);
            continue;
        }
    }

    for (auto& [id, node] : graph.nodes) {
        std::string class_name = node.label;
        if (auto it = labels.find(id); it != labels.end()) class_name = it->second;
        std::vector<std::string> out;
        if (auto st = stereotypes.find(id); st != stereotypes.end()) out.insert(out.end(), st->second.begin(), st->second.end());
        out.push_back(class_name);
        if (auto it = members.find(id); it != members.end() && !it->second.empty()) {
            std::vector<std::string> attrs, methods;
            for (const std::string& entry : it->second) {
                std::string trimmed = trim(entry);
                if (contains(trimmed, '(') && contains(trimmed, ')')) methods.push_back(normalize_class_method_signature(trimmed));
                else attrs.push_back(trimmed);
            }
            if (!attrs.empty() || !methods.empty()) {
                out.push_back("---");
                if (!attrs.empty()) {
                    out.insert(out.end(), attrs.begin(), attrs.end());
                    if (!methods.empty()) {
                        out.push_back("---");
                        out.insert(out.end(), methods.begin(), methods.end());
                    }
                } else {
                    out.insert(out.end(), methods.begin(), methods.end());
                }
            }
        }
        std::string joined;
        for (size_t i = 0; i < out.size(); ++i) joined += (i ? "\n" : "") + out[i];
        node.label = joined;
    }
    return graph;
}

// ======================================================================================
// ER diagrams
// ======================================================================================

namespace {

bool is_er_card_char(char ch) { return ch == '|' || ch == 'o' || ch == '{' || ch == '}'; }

std::pair<std::string, std::optional<std::string>> split_er_cardinality_left(const std::string& input) {
    std::string trimmed = trim(input);
    if (trimmed.empty()) return {"", std::nullopt};
    size_t len = trimmed.size();
    if (len >= 2 && is_er_card_char(trimmed[len - 2]) && is_er_card_char(trimmed[len - 1]))
        return {trim(trimmed.substr(0, len - 2)), trimmed.substr(len - 2)};
    if (is_er_card_char(trimmed[len - 1])) return {trim(trimmed.substr(0, len - 1)), trimmed.substr(len - 1)};
    return {trimmed, std::nullopt};
}

std::pair<std::string, std::optional<std::string>> split_er_cardinality_right(const std::string& input) {
    std::string trimmed = trim(input);
    if (trimmed.empty()) return {"", std::nullopt};
    if (trimmed.size() >= 2 && is_er_card_char(trimmed[0]) && is_er_card_char(trimmed[1]))
        return {trim(trimmed.substr(2)), trimmed.substr(0, 2)};
    if (is_er_card_char(trimmed[0])) return {trim(trimmed.substr(1)), trimmed.substr(0, 1)};
    return {trimmed, std::nullopt};
}

std::pair<std::string, std::optional<EdgeDecoration>> normalize_er_cardinality(const std::string& token) {
    std::string t = trim(token);
    if (t == "||" || t == "|") return {"1", EdgeDecoration::CrowsFootOne};
    if (t == "o|" || t == "|o" || t == "o") return {"0..1", EdgeDecoration::CrowsFootZeroOne};
    if (t == "|{" || t == "}|") return {"1..*", EdgeDecoration::CrowsFootMany};
    if (t == "o{" || t == "}o" || t == "}" || t == "{") return {"0..*", EdgeDecoration::CrowsFootZeroMany};
    return {t, std::nullopt};
}

struct ErRelation {
    std::string left, right;
    std::optional<std::string> label;
    std::optional<EdgeDecoration> left_decoration, right_decoration;
    EdgeStyle style;
};

std::optional<ErRelation> parse_er_relation_line(const std::string& line) {
    std::string relation_part;
    std::optional<std::string> label;
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        std::string l = trim(line.substr(colon + 1));
        if (!l.empty()) label = l;
        relation_part = trim(line.substr(0, colon));
    } else {
        relation_part = trim(line);
    }
    size_t sep;
    EdgeStyle style;
    if (size_t i = relation_part.find("--"); i != std::string::npos) { sep = i; style = EdgeStyle::Solid; }
    else if (size_t k = relation_part.find(".."); k != std::string::npos) { sep = k; style = EdgeStyle::Dotted; }
    else return std::nullopt;
    std::string left_part = trim(relation_part.substr(0, sep));
    std::string right_part = trim(relation_part.substr(sep + 2));
    if (left_part.empty() || right_part.empty()) return std::nullopt;
    auto [left_entity, left_card] = split_er_cardinality_left(left_part);
    auto [right_entity, right_card] = split_er_cardinality_right(right_part);
    if (left_entity.empty() || right_entity.empty()) return std::nullopt;
    std::string left_id = strip_quotes(trim(left_entity));
    std::string right_id = strip_quotes(trim(right_entity));
    if (left_id.empty() || right_id.empty()) return std::nullopt;
    ErRelation r;
    r.left = left_id;
    r.right = right_id;
    r.label = label;
    r.style = style;
    if (left_card) r.left_decoration = normalize_er_cardinality(*left_card).second;
    if (right_card) r.right_decoration = normalize_er_cardinality(*right_card).second;
    return r;
}

}  // namespace

Graph parse_er_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::Er;
    graph.direction = Direction::TopDown;
    std::vector<std::string> lines = preprocess_input(input);

    std::map<std::string, std::vector<std::string>> members;
    std::optional<std::string> current_entity;

    for (const std::string& raw_line : lines) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        if (starts_with(to_ascii_lower(line), "erdiagram")) continue;
        if (auto direction = parse_direction_line(line)) {
            graph.direction = *direction;
            continue;
        }

        if (current_entity) {
            const std::string active = *current_entity;
            size_t end_idx = line.find('}');
            if (end_idx != std::string::npos) {
                std::string fragment = trim(line.substr(0, end_idx));
                if (!fragment.empty()) members[active].push_back(fragment);
                current_entity.reset();
            } else {
                members[active].push_back(line);
            }
            continue;
        }

        if (auto rel = parse_er_relation_line(line)) {
            graph.ensure_node(rel->left, std::nullopt, NodeShape::RoundRect);
            graph.ensure_node(rel->right, std::nullopt, NodeShape::RoundRect);
            // No start/end labels for ER diagrams: the crow's foot symbols convey the cardinality.
            Edge e;
            e.from = rel->left;
            e.to = rel->right;
            e.label = rel->label;
            e.start_decoration = rel->left_decoration;
            e.end_decoration = rel->right_decoration;
            e.style = rel->style;
            graph.edges.push_back(std::move(e));
            continue;
        }

        size_t open_idx = line.find('{');
        if (open_idx != std::string::npos) {
            std::string name = strip_quotes(trim(line.substr(0, open_idx)));
            if (!name.empty()) {
                graph.ensure_node(name, std::nullopt, NodeShape::RoundRect);
                current_entity = name;
                std::string tail = trim(line.substr(open_idx + 1));
                size_t close_idx = tail.find('}');
                if (close_idx != std::string::npos) {
                    std::string fragment = trim(tail.substr(0, close_idx));
                    if (!fragment.empty()) members[name].push_back(fragment);
                    current_entity.reset();
                } else if (!tail.empty()) {
                    members[name].push_back(tail);
                }
            }
            continue;
        }

        std::string entity = strip_quotes(line);
        if (!entity.empty()) graph.ensure_node(entity, std::nullopt, NodeShape::RoundRect);
    }

    for (auto& [id, node] : graph.nodes) {
        std::string label = node.label;
        if (auto it = members.find(id); it != members.end() && !it->second.empty()) {
            label += "\n---";
            for (const std::string& a : it->second) label += "\n" + a;
        }
        node.label = label;
    }
    return graph;
}

// ======================================================================================
// state diagrams
// ======================================================================================

namespace {

std::pair<std::string, std::vector<std::string>> parse_state_id_with_classes(const std::string& input) {
    auto [base, classes] = split_inline_classes(input);
    return {strip_quotes(trim(base)), classes};
}

std::optional<std::tuple<std::string, std::string, std::vector<std::string>>> parse_state_alias_line(const std::string& line) {
    std::string trimmed = trim(line);
    if (!starts_with(trimmed, "state ")) return std::nullopt;
    if (contains(trimmed, '{')) return std::nullopt;
    std::string rest = trim(trim_start_matches(trimmed, "state "));
    if (rest.empty() || rest[0] != '"') return std::nullopt;
    size_t q = rest.find('"', 1);
    if (q == std::string::npos) return std::nullopt;
    std::string label = rest.substr(1, q - 1);
    std::string remaining = trim(rest.substr(q + 1));
    if (!starts_with(to_ascii_lower(remaining), "as ")) return std::nullopt;
    std::string id_raw = trim(remaining.substr(3));
    auto [id, classes] = parse_state_id_with_classes(id_raw);
    if (id.empty()) return std::nullopt;
    return std::make_tuple(id, label, classes);
}

// (cleaned line, shape override, label override)
std::tuple<std::string, std::optional<NodeShape>, std::optional<std::string>> parse_state_stereotype(const std::string& line) {
    std::string trimmed = trim(line);
    if (!starts_with(trimmed, "state ")) return {trimmed, std::nullopt, std::nullopt};
    size_t start = trimmed.find("<<");
    if (start == std::string::npos) return {trimmed, std::nullopt, std::nullopt};
    size_t end_rel = trimmed.find(">>", start + 2);
    if (end_rel == std::string::npos) return {trimmed, std::nullopt, std::nullopt};
    size_t end = end_rel - (start + 2);
    std::string stereo = to_ascii_lower(trim(trimmed.substr(start + 2, end)));
    std::string before = std::string(trim_end_view(trimmed.substr(0, start)));
    std::string after = std::string(trim_start_view(trimmed.substr(start + 2 + end + 2)));
    std::string cleaned;
    if (after.empty()) cleaned = before;
    else if (before.empty()) cleaned = after;
    else cleaned = before + " " + after;
    std::optional<NodeShape> shape;
    std::optional<std::string> label_override;
    if (stereo == "choice") shape = NodeShape::Diamond;
    else if (stereo == "fork" || stereo == "join") { shape = NodeShape::ForkJoin; label_override = std::string(); }
    return {cleaned, shape, label_override};
}

std::optional<std::tuple<std::string, std::string, std::vector<std::string>>> parse_state_description_line(const std::string& line) {
    std::string trimmed = trim(line);
    if (trimmed.empty()) return std::nullopt;
    if (starts_with(to_ascii_lower(trimmed), "note ")) return std::nullopt;
    std::string rest = starts_with(trimmed, "state ") ? trim(trimmed.substr(6)) : trimmed;
    if (contains(to_ascii_lower(rest), " as ")) return std::nullopt;
    std::optional<size_t> sep;
    size_t idx = 0;
    while (idx < rest.size()) {
        if (rest[idx] == ':') {
            if (idx + 2 < rest.size() && rest[idx + 1] == ':' && rest[idx + 2] == ':') {
                idx += 3;
                continue;
            }
            sep = idx;
            break;
        }
        ++idx;
    }
    if (!sep) return std::nullopt;
    std::string id_part = rest.substr(0, *sep);
    std::string desc_part = rest.substr(*sep + 1);
    auto [id, classes] = parse_state_id_with_classes(trim(id_part));
    std::string desc = strip_quotes(trim(desc_part));
    if (id.empty() || desc.empty()) return std::nullopt;
    return std::make_tuple(id, desc, classes);
}

std::string state_display_label(const std::string& id, const std::map<std::string, std::string>& labels,
                                const std::map<std::string, std::vector<std::string>>& descriptions) {
    auto lit = labels.find(id);
    std::string title = lit != labels.end() ? lit->second : id;
    auto dit = descriptions.find(id);
    if (dit == descriptions.end() || dit->second.empty()) return title;
    std::string label = title + "\n---";
    for (const std::string& d : dit->second) label += "\n" + d;
    return label;
}

std::optional<std::string> state_display_label_option(const std::string& id, const std::map<std::string, std::string>& labels,
                                                      const std::map<std::string, std::vector<std::string>>& descriptions) {
    if (labels.count(id) || descriptions.count(id)) return state_display_label(id, labels, descriptions);
    return std::nullopt;
}

struct StateNoteParsed {
    StateNotePosition position;
    std::string target, label;
};

std::optional<StateNoteParsed> parse_state_note(const std::string& line) {
    std::string trimmed = trim(line);
    if (!starts_with(to_ascii_lower(trimmed), "note ")) return std::nullopt;
    std::string rest = trim(trimmed.substr(4));
    std::string lower_rest = to_ascii_lower(rest);
    StateNotePosition position;
    std::string targets_part;
    if (starts_with(lower_rest, "right of ")) { position = StateNotePosition::RightOf; targets_part = trim(rest.substr(9)); }
    else if (starts_with(lower_rest, "left of ")) { position = StateNotePosition::LeftOf; targets_part = trim(rest.substr(8)); }
    else return std::nullopt;
    size_t colon = targets_part.find(':');
    if (colon == std::string::npos) return std::nullopt;
    std::string target = trim(targets_part.substr(0, colon)), label = trim(targets_part.substr(colon + 1));
    if (target.empty() || label.empty()) return std::nullopt;
    return StateNoteParsed{position, target, label};
}

EdgeMeta edge_meta_from_state_token(const std::string& token) {
    EdgeMeta m;
    m.arrow_start = contains(token, '<');
    m.arrow_end = contains(token, '>');
    m.directed = m.arrow_start || m.arrow_end;
    m.style = contains(token, "..") ? EdgeStyle::Dotted : EdgeStyle::Solid;
    return m;
}

struct StateTransition {
    std::string left;
    EdgeMeta meta;
    std::string right;
    std::optional<std::string> label;
};

std::optional<StateTransition> parse_state_transition(const std::string& line) {
    static const char* tokens[] = {"<-->", "<--", "-->", "<->", "->", "<-", "..>", "<.."};
    for (const char* token : tokens) {
        size_t pos = line.find(token);
        if (pos == std::string::npos) continue;
        std::string tk = token;
        std::string left = trim(line.substr(0, pos));
        std::string right_part = trim(line.substr(pos + tk.size()));
        if (left.empty() || right_part.empty()) continue;
        auto [right, label] = split_label(right_part);
        return StateTransition{left, edge_meta_from_state_token(tk), right, label};
    }
    return std::nullopt;
}

// (id, shape, label override)
std::tuple<std::string, NodeShape, std::optional<std::string>> normalize_state_token(
    const std::string& token, bool is_start, std::map<std::string, std::string>& start_states,
    std::map<std::string, std::string>& end_states, const std::string& scope) {
    std::string trimmed = trim(token);
    if (trimmed == "[*]" || trimmed == "*") {
        std::string id;
        NodeShape shape;
        if (is_start) {
            // Start states are shared per scope (fan-out becomes a fork bar).
            auto it = start_states.find(scope);
            if (it == start_states.end()) it = start_states.emplace(scope, "__start_" + scope + "__").first;
            id = it->second;
            shape = NodeShape::Circle;
        } else {
            auto it = end_states.find(scope);
            if (it == end_states.end()) it = end_states.emplace(scope, "__end_" + scope + "__").first;
            id = it->second;
            shape = NodeShape::DoubleCircle;
        }
        return {id, shape, std::string()};
    }
    return {strip_quotes(trimmed), NodeShape::RoundRect, std::nullopt};
}

std::optional<std::pair<std::string, std::vector<std::string>>> parse_state_simple(const std::string& line) {
    std::string trimmed = trim(line);
    if (!starts_with(trimmed, "state ")) return std::nullopt;
    if (contains(trimmed, '{')) return std::nullopt;
    std::string rest = trim(trim_start_matches(trimmed, "state "));
    if (contains(to_ascii_lower(rest), " as ")) return std::nullopt;
    size_t idx = rest.find('{');
    if (idx != std::string::npos) rest = trim(rest.substr(0, idx));
    if (rest.empty()) return std::nullopt;
    auto [id, classes] = parse_state_id_with_classes(rest);
    if (id.empty()) return std::nullopt;
    return std::make_pair(id, classes);
}

// (id, label, tail after '{')
std::optional<std::tuple<std::optional<std::string>, std::string, std::string>> parse_state_container_header(const std::string& line) {
    std::string trimmed = trim(line);
    if (!starts_with(trimmed, "state ")) return std::nullopt;
    size_t brace_idx = trimmed.find('{');
    if (brace_idx == std::string::npos) return std::nullopt;
    std::string head = trim(trimmed.substr(0, brace_idx));
    std::string tail = trim(trimmed.substr(brace_idx + 1));
    std::string rest = trim(trim_start_matches(head, "state "));
    if (rest.empty()) return std::nullopt;
    if (rest[0] == '"') {
        size_t q = rest.find('"', 1);
        if (q == std::string::npos) return std::nullopt;
        std::string label = rest.substr(1, q - 1);
        std::string remaining = trim(rest.substr(q + 1));
        if (starts_with(to_ascii_lower(remaining), "as ")) {
            std::string id = trim(remaining.substr(3));
            if (id.empty()) return std::nullopt;
            return std::make_tuple(std::optional<std::string>(id), label, tail);
        }
        return std::make_tuple(std::optional<std::string>(), label, tail);
    }
    std::string lower = to_ascii_lower(rest);
    size_t as_idx = lower.find(" as ");
    if (as_idx != std::string::npos) {
        std::string id_part = trim(rest.substr(0, as_idx));
        std::string label_part = trim(rest.substr(as_idx + 4));
        if (id_part.empty() || label_part.empty()) return std::nullopt;
        return std::make_tuple(std::optional<std::string>(strip_quotes(id_part)), strip_quotes(label_part), tail);
    }
    std::string id = strip_quotes(rest);
    return std::make_tuple(std::optional<std::string>(id), id, tail);
}

}  // namespace

Graph parse_state_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::State;
    std::vector<std::string> lines = preprocess_input(input);

    std::map<std::string, std::string> labels;
    std::map<std::string, std::vector<std::string>> descriptions;
    std::map<std::string, std::string> start_states, end_states;
    std::vector<size_t> subgraph_stack;
    size_t region_counter = 0;

    struct CompositeContext {
        size_t subgraph_idx;
        std::vector<std::vector<std::string>> regions;
        size_t current_region;
        bool has_separator;
    };
    std::vector<CompositeContext> composite_stack;
    std::deque<std::string> pending(lines.begin(), lines.end());

    auto record_region_node = [&](const std::string& node_id) {
        for (CompositeContext& ctx : composite_stack) {
            bool found = false;
            for (const auto& region : ctx.regions)
                for (const auto& id : region)
                    if (id == node_id) found = true;
            if (found) continue;
            ctx.regions[ctx.current_region].push_back(node_id);
        }
    };

    auto finalize_regions = [&](CompositeContext& ctx) {
        if (!ctx.has_separator) return;
        std::vector<std::vector<std::string>> regions;
        for (auto& r : ctx.regions)
            if (!r.empty()) regions.push_back(std::move(r));
        if (regions.size() <= 1) return;
        for (auto& region_nodes : regions) {
            std::string id = "__region_" + std::to_string(region_counter++) + "__";
            Subgraph sg;
            sg.id = id;
            sg.label = "";
            sg.nodes = std::move(region_nodes);
            graph.subgraphs.push_back(std::move(sg));
            NodeStyle st;
            st.fill = "none";
            st.stroke = "none";
            st.stroke_width = 0.0f;
            graph.subgraph_styles[id] = st;
        }
    };

    auto node_exists = [&](const std::string& id) { return graph.nodes.count(id) > 0; };

    while (!pending.empty()) {
        std::string raw_line = pending.front();
        pending.pop_front();
        for (const std::string& raw_statement : split_statements(raw_line)) {
            std::string raw = trim(raw_statement);
            if (raw.empty()) continue;
            auto [line0, state_shape, label_override] = parse_state_stereotype(raw);
            std::string line = trim(line0);
            if (line.empty()) continue;
            if (starts_with(to_ascii_lower(line), "statediagram")) continue;

            if (auto direction = parse_direction_line(line)) {
                graph.direction = *direction;
                continue;
            }
            if (starts_with(line, "classDef")) { parse_class_def(line, graph); continue; }
            if (starts_with(line, "class ")) { parse_class_line(line, graph); continue; }
            if (starts_with(line, "style ")) { parse_style_line(line, graph); continue; }

            if (line == "}") {
                if (!composite_stack.empty()) {
                    CompositeContext ctx = std::move(composite_stack.back());
                    composite_stack.pop_back();
                    if (!subgraph_stack.empty()) {
                        size_t idx = subgraph_stack.back();
                        subgraph_stack.pop_back();
                        if (idx != ctx.subgraph_idx) subgraph_stack.push_back(idx);
                    }
                    finalize_regions(ctx);
                }
                continue;
            }

            if (line == "--") {
                if (!composite_stack.empty()) {
                    CompositeContext& ctx = composite_stack.back();
                    ctx.has_separator = true;
                    ctx.regions.emplace_back();
                    ctx.current_region = ctx.regions.size() - 1;
                }
                continue;
            }

            if (auto header = parse_state_container_header(line)) {
                auto& [id, label, tail] = *header;
                if (id) labels[*id] = label;
                Subgraph sg;
                sg.id = id;
                sg.label = label;
                graph.subgraphs.push_back(std::move(sg));
                subgraph_stack.push_back(graph.subgraphs.size() - 1);
                composite_stack.push_back({graph.subgraphs.size() - 1, {std::vector<std::string>()}, 0, false});
                if (!tail.empty()) {
                    size_t close_idx = tail.find('}');
                    if (close_idx != std::string::npos) {
                        std::string body = trim(tail.substr(0, close_idx));
                        std::string after = trim(tail.substr(close_idx + 1));
                        if (!after.empty()) pending.push_front(after);
                        pending.push_front("}");
                        if (!body.empty()) pending.push_front(body);
                    } else {
                        pending.push_front(tail);
                    }
                }
                continue;
            }

            if (auto alias = parse_state_alias_line(line)) {
                auto& [id, label0, classes] = *alias;
                std::string label = label_override ? *label_override : label0;
                labels[id] = label;
                graph.ensure_node(id, state_display_label(id, labels, descriptions), state_shape ? state_shape : std::optional<NodeShape>(NodeShape::RoundRect));
                apply_node_classes(graph, id, classes);
                add_node_to_subgraphs(graph, subgraph_stack, id);
                record_region_node(id);
                continue;
            }

            if (auto tr = parse_state_transition(line)) {
                std::string scope = "root";
                if (!subgraph_stack.empty() && subgraph_stack.back() < graph.subgraphs.size())
                    if (const auto& sid = graph.subgraphs[subgraph_stack.back()].id) scope = *sid;
                auto [left_token, left_classes] = split_inline_classes(tr->left);
                auto [right_token, right_classes] = split_inline_classes(tr->right);
                auto [left_id, left_shape0, left_label_override] = normalize_state_token(left_token, true, start_states, end_states, scope);
                auto [right_id, right_shape0, right_label_override] = normalize_state_token(right_token, false, start_states, end_states, scope);
                std::optional<std::string> left_label = left_label_override ? left_label_override : state_display_label_option(left_id, labels, descriptions);
                std::optional<std::string> right_label = right_label_override ? right_label_override : state_display_label_option(right_id, labels, descriptions);
                std::optional<NodeShape> left_shape = (left_shape0 == NodeShape::RoundRect && node_exists(left_id)) ? std::nullopt : std::optional<NodeShape>(left_shape0);
                std::optional<NodeShape> right_shape = (right_shape0 == NodeShape::RoundRect && node_exists(right_id)) ? std::nullopt : std::optional<NodeShape>(right_shape0);
                graph.ensure_node(left_id, left_label, left_shape);
                graph.ensure_node(right_id, right_label, right_shape);
                apply_node_classes(graph, left_id, left_classes);
                apply_node_classes(graph, right_id, right_classes);
                add_node_to_subgraphs(graph, subgraph_stack, left_id);
                add_node_to_subgraphs(graph, subgraph_stack, right_id);
                record_region_node(left_id);
                record_region_node(right_id);
                Edge e;
                e.from = left_id;
                e.to = right_id;
                e.label = tr->label;
                e.directed = tr->meta.directed;
                e.arrow_start = tr->meta.arrow_start;
                e.arrow_end = tr->meta.arrow_end;
                e.arrow_start_kind = tr->meta.arrow_start_kind;
                e.arrow_end_kind = tr->meta.arrow_end_kind;
                e.start_decoration = tr->meta.start_decoration;
                e.end_decoration = tr->meta.end_decoration;
                e.style = tr->meta.style;
                graph.edges.push_back(std::move(e));
                continue;
            }

            if (auto desc = parse_state_description_line(line)) {
                auto& [id, label0, classes] = *desc;
                std::string label = label_override ? *label_override : label0;
                descriptions[id].push_back(label);
                graph.ensure_node(id, state_display_label(id, labels, descriptions), state_shape ? state_shape : std::optional<NodeShape>(NodeShape::RoundRect));
                apply_node_classes(graph, id, classes);
                add_node_to_subgraphs(graph, subgraph_stack, id);
                record_region_node(id);
                continue;
            }

            if (auto note = parse_state_note(line)) {
                auto [target, classes] = parse_state_id_with_classes(note->target);
                if (target.empty()) continue;
                std::optional<NodeShape> shape = node_exists(target) ? std::nullopt : std::optional<NodeShape>(NodeShape::RoundRect);
                graph.ensure_node(target, state_display_label_option(target, labels, descriptions), shape);
                apply_node_classes(graph, target, classes);
                graph.state_notes.push_back({note->position, target, note->label});
                add_node_to_subgraphs(graph, subgraph_stack, target);
                record_region_node(target);
                continue;
            }

            if (auto simple = parse_state_simple(line)) {
                auto& [id, classes] = *simple;
                if (label_override) labels[id] = *label_override;
                graph.ensure_node(id, state_display_label_option(id, labels, descriptions), state_shape ? state_shape : std::optional<NodeShape>(NodeShape::RoundRect));
                apply_node_classes(graph, id, classes);
                add_node_to_subgraphs(graph, subgraph_stack, id);
                record_region_node(id);
                continue;
            }
        }
    }

    // Convert scoped [*] fan-out / fan-in nodes into fork / join bars.
    std::map<std::string, size_t> outgoing_counts, incoming_counts;
    for (const Edge& e : graph.edges) {
        ++outgoing_counts[e.from];
        ++incoming_counts[e.to];
    }
    std::vector<std::string> bars;
    for (const auto& [scope, id] : start_states)
        if (scope != "root" && outgoing_counts[id] > 1) bars.push_back(id);
    for (const auto& [scope, id] : end_states)
        if (scope != "root" && incoming_counts[id] > 1) bars.push_back(id);
    for (const std::string& id : bars)
        if (auto it = graph.nodes.find(id); it != graph.nodes.end()) {
            it->second.shape = NodeShape::ForkJoin;
            it->second.label.clear();
        }
    return graph;
}

}  // namespace mermaid::pp
