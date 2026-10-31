// Mermaid port: architecture diagrams. Mirrors parser.rs parse_architecture_diagram, layout/architecture.rs and
// render.rs render_architecture (+ architecture_icon_svg).
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>

#include "kinds.h"
#include "layout_internal.h"
#include "numfmt.h"
#include "parser_internal.h"
#include "routing.h"
#include "strutil.h"
#include "svg_common.h"
#include "text.h"

namespace mermaid {

namespace {

float fmax_(float a, float b) { return a > b ? a : (a < b ? b : a); }
float fmin_(float a, float b) { return a < b ? a : (a > b ? b : a); }

// ---- parser ----

std::optional<ArchDir> arch_dir_from_char(char c) {
    switch (std::toupper(static_cast<unsigned char>(c))) {
    case 'L': return ArchDir::Left;
    case 'R': return ArchDir::Right;
    case 'T': return ArchDir::Top;
    case 'B': return ArchDir::Bottom;
    default: return std::nullopt;
    }
}
bool arch_is_horizontal(ArchDir d) { return d == ArchDir::Left || d == ArchDir::Right; }

std::string strip_arch_group_modifier(const std::string& token) {
    std::string t = trim(token);
    const std::string suffix = "{group}";
    if (ends_with(t, suffix)) t.resize(t.size() - suffix.size());
    return trim(t);
}

std::optional<ArchDir> parse_arch_port(const std::string& token) {
    std::string t = trim(token);
    if (t.empty()) return std::nullopt;
    // exactly one character (not one byte)
    if (char_count(t) != 1) return std::nullopt;
    return arch_dir_from_char(t[0]);
}

std::pair<std::string, std::optional<ArchDir>> split_arch_port_left(const std::string& token) {
    size_t c = token.find(':');
    if (c == std::string::npos) return {strip_arch_group_modifier(trim(token)), std::nullopt};
    return {strip_arch_group_modifier(trim(token.substr(0, c))), parse_arch_port(token.substr(c + 1))};
}

std::pair<std::string, std::optional<ArchDir>> split_arch_port_right(const std::string& token) {
    size_t c = token.find(':');
    if (c == std::string::npos) return {strip_arch_group_modifier(trim(token)), std::nullopt};
    return {strip_arch_group_modifier(trim(token.substr(c + 1))), parse_arch_port(token.substr(0, c))};
}

struct ArchNode {
    std::string kind, id, label;
    std::optional<std::string> parent, icon;
};

std::optional<ArchNode> parse_architecture_node(const std::string& line) {
    size_t sp = line.find(' ');
    if (sp == std::string::npos) return std::nullopt;
    std::string kind = to_ascii_lower(trim(line.substr(0, sp)));
    std::string rest = trim(line.substr(sp + 1));
    std::string node_part;
    std::optional<std::string> parent;
    size_t in_pos = rest.find(" in ");
    if (in_pos != std::string::npos) {
        node_part = trim(rest.substr(0, in_pos));
        parent = trim(rest.substr(in_pos + 4));
    } else {
        node_part = rest;
    }
    std::string label;
    size_t start = node_part.find('[');
    if (start != std::string::npos) {
        size_t end = node_part.rfind(']');
        if (end != std::string::npos && end >= start + 1) label = pp::strip_quotes(trim(node_part.substr(start + 1, end - start - 1)));
        else if (end != std::string::npos) return std::nullopt;  // (slice start > end panics in Rust)
    }
    std::string id_part = trim(node_part.substr(0, node_part.find('[')));
    std::optional<std::string> icon;
    size_t pstart = id_part.find('(');
    if (pstart != std::string::npos) {
        size_t pend = id_part.find(')');
        if (pend != std::string::npos) {
            if (pend < pstart + 1) return std::nullopt;
            icon = trim(id_part.substr(pstart + 1, pend - pstart - 1));
        }
    }
    std::string id = trim(id_part.substr(0, id_part.find('(')));
    if (id.empty()) return std::nullopt;
    if (label.empty()) label = id;
    return ArchNode{kind, id, label, parent, icon};
}

std::optional<std::pair<std::string, std::optional<std::string>>> parse_architecture_junction(const std::string& line) {
    if (line.size() < 8) return std::nullopt;
    std::string rest = trim(line.substr(8));
    if (rest.empty()) return std::nullopt;
    std::string id_part;
    std::optional<std::string> parent;
    size_t in_pos = rest.find(" in ");
    if (in_pos != std::string::npos) {
        id_part = trim(rest.substr(0, in_pos));
        parent = trim(rest.substr(in_pos + 4));
    } else {
        id_part = rest;
    }
    std::string id = pp::strip_quotes(id_part);
    if (id.empty()) return std::nullopt;
    return std::make_pair(id, parent);
}

struct ArchEdgeSpec {
    std::string from, to;
    std::optional<ArchDir> from_port, to_port;
    bool arrow_start, arrow_end;
};

std::optional<ArchEdgeSpec> parse_architecture_edge(const std::string& line) {
    struct A {
        const char* arrow;
        bool s, e;
    };
    static const A arrows[] = {{"<-->", true, true}, {"<--", true, false}, {"-->", false, true}, {"--", false, false}, {"->", false, true}};
    for (const A& a : arrows) {
        size_t idx = line.find(a.arrow);
        if (idx == std::string::npos) continue;
        std::string left = trim(line.substr(0, idx));
        std::string right = trim(line.substr(idx + std::strlen(a.arrow)));
        auto [from, from_port] = split_arch_port_left(left);
        auto [to, to_port] = split_arch_port_right(right);
        if (from.empty() || to.empty()) return std::nullopt;
        return ArchEdgeSpec{from, to, from_port, to_port, a.s, a.e};
    }
    return std::nullopt;
}

}  // namespace

Graph parse_architecture_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::Architecture;
    graph.direction = Direction::LeftRight;
    std::map<std::string, size_t> groups;
    for (const std::string& raw_line : pp::preprocess_input(input)) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        std::string lower = to_ascii_lower(line);
        if (starts_with(lower, "architecture")) continue;
        if (starts_with(lower, "group ") || starts_with(lower, "service ")) {
            if (auto n = parse_architecture_node(line)) {
                if (n->kind == "group") {
                    Subgraph sg;
                    sg.id = n->id;
                    sg.label = n->label;
                    sg.icon = n->icon;
                    graph.subgraphs.push_back(std::move(sg));
                    groups[n->id] = graph.subgraphs.size() - 1;
                } else {
                    graph.ensure_node(n->id, n->label, NodeShape::Rectangle);
                    if (n->icon) graph.nodes[n->id].icon = n->icon;
                    if (n->parent)
                        if (auto g = groups.find(*n->parent); g != groups.end()) graph.subgraphs[g->second].nodes.push_back(n->id);
                }
            }
            continue;
        }
        if (starts_with(lower, "junction ")) {
            if (auto j = parse_architecture_junction(line)) {
                graph.ensure_node(j->first, std::string(), NodeShape::Circle);
                graph.nodes[j->first].icon = "junction";
                if (j->second)
                    if (auto g = groups.find(*j->second); g != groups.end()) graph.subgraphs[g->second].nodes.push_back(j->first);
            }
            continue;
        }
        if (auto spec = parse_architecture_edge(line)) {
            graph.ensure_node(spec->from, std::nullopt, std::nullopt);
            graph.ensure_node(spec->to, std::nullopt, std::nullopt);
            size_t edge_index = graph.edges.size();
            if (spec->from_port || spec->to_port) graph.arch_edge_ports[edge_index] = {spec->from_port, spec->to_port};
            Edge e;
            e.from = spec->from;
            e.to = spec->to;
            e.directed = spec->arrow_start || spec->arrow_end;
            e.arrow_start = spec->arrow_start;
            e.arrow_end = spec->arrow_end;
            graph.edges.push_back(std::move(e));
        }
    }
    return graph;
}

// ======================================================================================
// layout
// ======================================================================================

namespace {

constexpr float MARGIN = 24.0f, SERVICE_SIZE = 64.0f, JUNCTION_SIZE = 18.0f;
constexpr float CELL_W = SERVICE_SIZE + 80.0f, CELL_H = SERVICE_SIZE + 112.0f;
constexpr float GROUP_PAD_X = 28.0f, GROUP_PAD_TOP = 32.0f, GROUP_PAD_BOTTOM = 44.0f, GROUP_GAP_Y = 48.0f, LABEL_SPACE = 24.0f;
const char* const GROUP_STROKE = "hsl(240, 60%, 86.2745098039%)";
const char* const ICON_FILL = "#087ebf";

using Cell = std::pair<int, int>;

Cell shift_position(Cell pos, ArchDir source, ArchDir target) {
    auto [x, y] = pos;
    bool sh = arch_is_horizontal(source), th = arch_is_horizontal(target);
    if (sh && !th) return {x + (source == ArchDir::Left ? -1 : 1), y + (target == ArchDir::Top ? 1 : -1)};
    if (sh && th) return {x + (source == ArchDir::Left ? -1 : 1), y};
    if (!sh && th) return {x + (target == ArchDir::Left ? 1 : -1), y + (source == ArchDir::Top ? 1 : -1)};
    return {x, y + (source == ArchDir::Top ? 1 : -1)};
}

EdgeSide arch_side(ArchDir d) {
    switch (d) {
    case ArchDir::Left: return EdgeSide::Left;
    case ArchDir::Right: return EdgeSide::Right;
    case ArchDir::Top: return EdgeSide::Top;
    default: return EdgeSide::Bottom;
    }
}

using Adjacency = std::map<std::string, std::vector<std::pair<std::pair<ArchDir, ArchDir>, std::string>>>;

void insert_adjacency(Adjacency& adjacency, const std::string& id, std::pair<ArchDir, ArchDir> pair, const std::string& neighbor) {
    auto& entries = adjacency[id];
    for (auto& e : entries)
        if (e.first == pair) {
            e.second = neighbor;
            return;
        }
    entries.push_back({pair, neighbor});
}

Cell find_free_cell(const std::set<Cell>& occupied, Cell desired) {
    if (!occupied.count(desired)) return desired;
    for (int radius = 1; radius < 64; ++radius)
        for (int dy = -radius; dy <= radius; ++dy) {
            int rem = radius - std::abs(dy);
            for (int dx : {-rem, rem}) {
                Cell candidate{desired.first + dx, desired.second + dy};
                if (!occupied.count(candidate)) return candidate;
                if (rem == 0) break;
            }
        }
    return desired;
}

std::vector<Point> route_arch_edge(Point start, Point end, EdgeSide start_side, EdgeSide end_side, const std::map<std::string, NodeLayout>& nodes,
                                   const std::string& from_id, const std::string& to_id) {
    const float EPS = 1e-3f, DETOUR_GAP = 16.0f;
    bool sh = start_side == EdgeSide::Left || start_side == EdgeSide::Right;
    bool eh = end_side == EdgeSide::Left || end_side == EdgeSide::Right;
    std::vector<Point> points{start};
    if (sh && eh) {
        if (std::fabs(start.second - end.second) > EPS) {
            float mid_x = (start.first + end.first) / 2.0f;
            points.push_back({mid_x, start.second});
            points.push_back({mid_x, end.second});
        } else {
            float y = start.second;
            float seg_min_x = fmin_(start.first, end.first), seg_max_x = fmax_(start.first, end.first);
            float block_top = std::numeric_limits<float>::max(), block_bottom = std::numeric_limits<float>::lowest();
            bool has_blocker = false;
            for (const auto& [id, node] : nodes) {
                if (node.id == from_id || node.id == to_id) continue;
                if (y > node.y && y < node.y + node.height && seg_max_x > node.x && seg_min_x < node.x + node.width) {
                    has_blocker = true;
                    block_top = fmin_(block_top, node.y);
                    block_bottom = fmax_(block_bottom, node.y + node.height);
                }
            }
            if (has_blocker) {
                float above = block_top - DETOUR_GAP, below = block_bottom + DETOUR_GAP;
                float detour_y = std::fabs(y - above) <= std::fabs(below - y) ? above : below;
                points.push_back({start.first, detour_y});
                points.push_back({end.first, detour_y});
            }
        }
    } else if (!sh && !eh) {
        if (std::fabs(start.first - end.first) > EPS) {
            float mid_y = (start.second + end.second) / 2.0f;
            points.push_back({start.first, mid_y});
            points.push_back({end.first, mid_y});
        } else {
            float x = start.first;
            float seg_min_y = fmin_(start.second, end.second), seg_max_y = fmax_(start.second, end.second);
            float block_left = std::numeric_limits<float>::max(), block_right = std::numeric_limits<float>::lowest();
            bool has_blocker = false;
            for (const auto& [id, node] : nodes) {
                if (node.id == from_id || node.id == to_id) continue;
                if (x > node.x && x < node.x + node.width && seg_max_y > node.y && seg_min_y < node.y + node.height) {
                    has_blocker = true;
                    block_left = fmin_(block_left, node.x);
                    block_right = fmax_(block_right, node.x + node.width);
                }
            }
            if (has_blocker) {
                float left = block_left - DETOUR_GAP, right = block_right + DETOUR_GAP;
                float detour_x = std::fabs(x - left) <= std::fabs(right - x) ? left : right;
                points.push_back({detour_x, start.second});
                points.push_back({detour_x, end.second});
            }
        }
    } else if (sh && !eh) {
        points.push_back({end.first, start.second});
    } else {
        points.push_back({start.first, end.second});
    }
    points.push_back(end);
    return points;
}

}  // namespace

Layout compute_architecture_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    std::map<std::string, NodeLayout> nodes;
    for (const auto& [id, node] : graph.nodes) {
        bool is_junction = (node.icon && *node.icon == "junction") || (node.shape == NodeShape::Circle && trim_view(node.label).empty());
        TextBlock label = measure_label(node.label, theme, config);
        NodeStyle style = resolve_node_style_of(node.id, graph);
        if (!style.fill) style.fill = is_junction ? theme.line_color : std::string(ICON_FILL);
        if (!style.stroke) style.stroke = "none";
        if (!style.stroke_width) style.stroke_width = 0.0f;
        float size = is_junction ? JUNCTION_SIZE : SERVICE_SIZE;
        NodeLayout nl;
        nl.id = node.id;
        nl.width = size;
        nl.height = size;
        nl.label = std::move(label);
        nl.style = style;
        if (auto link = graph.node_links.find(node.id); link != graph.node_links.end()) nl.link = link->second;
        nl.shape = is_junction ? NodeShape::Circle : NodeShape::Rectangle;
        nl.icon = node.icon;
        nodes.emplace(node.id, std::move(nl));
    }

    std::vector<std::string> order;
    for (const auto& [id, n] : nodes) order.push_back(id);
    std::stable_sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) {
        size_t oa = graph.node_order.count(a) ? graph.node_order.at(a) : SIZE_MAX;
        size_t ob = graph.node_order.count(b) ? graph.node_order.at(b) : SIZE_MAX;
        if (oa != ob) return oa < ob;
        return a < b;
    });

    Adjacency adjacency;
    for (size_t idx = 0; idx < graph.edges.size(); ++idx) {
        const Edge& edge = graph.edges[idx];
        auto pit = graph.arch_edge_ports.find(idx);
        if (pit == graph.arch_edge_ports.end() || !pit->second.first || !pit->second.second) continue;
        ArchDir from_port = *pit->second.first, to_port = *pit->second.second;
        if (!nodes.count(edge.from) || !nodes.count(edge.to)) continue;
        if (from_port == to_port) continue;
        insert_adjacency(adjacency, edge.from, {from_port, to_port}, edge.to);
        insert_adjacency(adjacency, edge.to, {to_port, from_port}, edge.from);
    }

    std::set<std::string> visited;
    std::vector<std::vector<std::pair<std::string, Cell>>> raw_components;
    for (const std::string& seed : order) {
        if (visited.count(seed)) continue;
        std::map<std::string, Cell> placed;
        std::set<Cell> occupied;
        std::vector<std::pair<std::string, Cell>> component;
        placed[seed] = {0, 0};
        occupied.insert({0, 0});
        visited.insert(seed);
        component.push_back({seed, {0, 0}});
        std::deque<std::string> queue{seed};
        while (!queue.empty()) {
            std::string id = queue.front();
            queue.pop_front();
            Cell pos = placed[id];
            auto nit = adjacency.find(id);
            if (nit == adjacency.end()) continue;
            for (const auto& [dirs, neighbor] : nit->second) {
                if (visited.count(neighbor)) continue;
                Cell desired = shift_position(pos, dirs.first, dirs.second);
                Cell cell = find_free_cell(occupied, desired);
                placed[neighbor] = cell;
                occupied.insert(cell);
                visited.insert(neighbor);
                component.push_back({neighbor, cell});
                queue.push_back(neighbor);
            }
        }
        raw_components.push_back(std::move(component));
    }

    std::map<std::string, size_t> node_group;
    for (size_t idx = 0; idx < graph.subgraphs.size(); ++idx)
        for (const auto& id : graph.subgraphs[idx].nodes) node_group[id] = idx;

    struct Canvas {
        std::vector<std::pair<std::string, Cell>> cells;
        std::set<Cell> occupied;
        std::set<size_t> groups;
    };
    std::vector<Canvas> canvases;
    for (auto& component : raw_components) {
        std::set<size_t> comp_groups;
        for (const auto& [id, cell] : component)
            if (auto g = node_group.find(id); g != node_group.end()) comp_groups.insert(g->second);
        std::optional<size_t> target;
        for (size_t i = 0; i < canvases.size(); ++i) {
            bool disjoint = true;
            for (size_t g : canvases[i].groups)
                if (comp_groups.count(g)) disjoint = false;
            if (!disjoint) { target = i; break; }
        }
        if (target) {
            Canvas& canvas = canvases[*target];
            std::vector<Cell> shared;
            for (const auto& [id, cell] : canvas.cells)
                if (auto g = node_group.find(id); g != node_group.end() && comp_groups.count(g->second)) shared.push_back(cell);
            int anchor_x = 0, anchor_y = 0;
            if (!shared.empty()) {
                anchor_x = std::min_element(shared.begin(), shared.end(), [](Cell a, Cell b) { return a.first < b.first; })->first;
                anchor_y = std::min_element(shared.begin(), shared.end(), [](Cell a, Cell b) { return a.second < b.second; })->second;
            }
            int comp_min_x = 0, comp_max_y = 0;
            if (!component.empty()) {
                comp_min_x = std::min_element(component.begin(), component.end(), [](const auto& a, const auto& b) { return a.second.first < b.second.first; })->second.first;
                comp_max_y = std::max_element(component.begin(), component.end(), [](const auto& a, const auto& b) { return a.second.second < b.second.second; })->second.second;
            }
            Cell offset{anchor_x - comp_min_x, anchor_y - 1 - comp_max_y};
            for (auto& [id, cell] : component) {
                Cell desired{cell.first + offset.first, cell.second + offset.second};
                Cell placed = find_free_cell(canvas.occupied, desired);
                canvas.occupied.insert(placed);
                canvas.cells.push_back({id, placed});
            }
            canvas.groups.insert(comp_groups.begin(), comp_groups.end());
        } else {
            std::set<Cell> occupied;
            for (const auto& [id, cell] : component) occupied.insert(cell);
            canvases.push_back({component, occupied, comp_groups});
        }
    }

    float origin_x = MARGIN + GROUP_PAD_X;
    float current_top = MARGIN;
    for (const Canvas& canvas : canvases) {
        const auto& component = canvas.cells;
        int min_x = 0, max_y = 0;
        if (!component.empty()) {
            min_x = std::min_element(component.begin(), component.end(), [](const auto& a, const auto& b) { return a.second.first < b.second.first; })->second.first;
            max_y = std::max_element(component.begin(), component.end(), [](const auto& a, const auto& b) { return a.second.second < b.second.second; })->second.second;
        }
        float base_y = current_top + GROUP_PAD_TOP;
        float comp_bottom = base_y;
        for (const auto& [id, cell] : component) {
            float col = static_cast<float>(cell.first - min_x), row = static_cast<float>(max_y - cell.second);
            if (auto it = nodes.find(id); it != nodes.end()) {
                NodeLayout& node = it->second;
                node.x = origin_x + col * CELL_W + (SERVICE_SIZE - node.width) / 2.0f;
                node.y = base_y + row * CELL_H + (SERVICE_SIZE - node.height) / 2.0f;
                comp_bottom = fmax_(comp_bottom, node.y + node.height + LABEL_SPACE);
            }
        }
        current_top = comp_bottom + GROUP_PAD_BOTTOM + GROUP_GAP_Y;
    }

    std::vector<SubgraphLayout> subgraphs;
    for (const Subgraph& sub : graph.subgraphs) {
        std::vector<const NodeLayout*> members;
        for (const auto& id : sub.nodes)
            if (auto it = nodes.find(id); it != nodes.end()) members.push_back(&it->second);
        if (members.empty()) continue;
        float min_x = std::numeric_limits<float>::max(), min_y = min_x;
        float max_x = std::numeric_limits<float>::lowest(), max_y = max_x;
        for (const NodeLayout* n : members) {
            min_x = fmin_(min_x, n->x);
            min_y = fmin_(min_y, n->y);
            max_x = fmax_(max_x, n->x + n->width);
            max_y = fmax_(max_y, n->y + n->height + LABEL_SPACE);
        }
        SubgraphLayout sl;
        sl.label = sub.label;
        sl.label_block = measure_label(sub.label, theme, config);
        NodeStyle style = resolve_subgraph_style(sub, graph);
        style.fill = "none";
        style.stroke = GROUP_STROKE;
        style.stroke_width = 2.0f;
        style.stroke_dasharray = "8";
        if (!style.text_color) style.text_color = theme.primary_text_color;
        for (const auto& id : sub.nodes)
            if (nodes.count(id)) sl.nodes.push_back(id);
        sl.x = min_x - GROUP_PAD_X;
        sl.y = min_y - GROUP_PAD_TOP;
        sl.width = max_x - min_x + GROUP_PAD_X * 2.0f;
        sl.height = max_y - min_y + GROUP_PAD_TOP + GROUP_PAD_BOTTOM;
        sl.style = style;
        sl.icon = sub.icon;
        subgraphs.push_back(std::move(sl));
    }

    std::vector<EdgeLayout> edges;
    for (size_t idx = 0; idx < graph.edges.size(); ++idx) {
        const Edge& edge = graph.edges[idx];
        auto fit = nodes.find(edge.from);
        auto tit = nodes.find(edge.to);
        if (fit == nodes.end() || tit == nodes.end()) continue;
        const NodeLayout& from = fit->second;
        const NodeLayout& to = tit->second;
        std::optional<ArchDir> from_port, to_port;
        if (auto p = graph.arch_edge_ports.find(idx); p != graph.arch_edge_ports.end()) {
            from_port = p->second.first;
            to_port = p->second.second;
        }
        auto [fallback_start, fallback_end, backward] = edge_sides(from, to, graph.direction);
        (void)backward;
        EdgeSide start_side = from_port ? arch_side(*from_port) : fallback_start;
        EdgeSide end_side = to_port ? arch_side(*to_port) : fallback_end;
        Point start = anchor_point_for_node(from, start_side, 0.0f);
        Point end = anchor_point_for_node(to, end_side, 0.0f);
        std::vector<Point> points = route_arch_edge(start, end, start_side, end_side, nodes, edge.from, edge.to);
        EdgeStyleOverride override_style = resolve_edge_style_of(idx, graph);
        if (!override_style.stroke) override_style.stroke = theme.line_color;
        override_style.stroke_width = override_style.stroke_width ? *override_style.stroke_width : 3.0f;
        EdgeLayout el;
        el.from = edge.from;
        el.to = edge.to;
        el.points = compress_path(points);
        el.directed = edge.directed;
        el.arrow_start = edge.arrow_start;
        el.arrow_end = edge.arrow_end;
        el.style = edge.style;
        el.override_style = override_style;
        edges.push_back(std::move(el));
    }

    auto [max_x, max_y] = layout_bounds_with_edges(nodes, subgraphs, edges);
    Layout layout;
    layout.kind = graph.kind;
    layout.nodes = std::move(nodes);
    layout.edges = std::move(edges);
    layout.subgraphs = std::move(subgraphs);
    layout.width = fmax_(max_x + MARGIN, 200.0f);
    layout.height = fmax_(max_y + MARGIN, 200.0f);
    return layout;
}

// ======================================================================================
// writer
// ======================================================================================

namespace {

std::string architecture_icon_svg(const std::optional<std::string>& icon_type, float w, float h, const std::string& fill) {
    using svg::F2;
    auto d1 = [](float v) { return fmt_fixed(v, 1); };
    float cx = w / 2.0f, cy = h / 2.0f;
    float r = fmin_(w, h) * 0.35f;
    float sw = fmax_(w * 0.02f, 1.5f);
    std::string style = "fill=\"none\" stroke=\"" + fill + "\" stroke-width=\"" + d1(sw) + "\" stroke-linecap=\"round\" stroke-linejoin=\"round\"";
    std::string icon = icon_type ? to_ascii_lower(*icon_type) : "";
    auto has = [&](const char* t) { return icon.find(t) != std::string::npos; };
    auto globe = [&](bool vertical) {
        float rx = r * 0.5f;
        std::string s = "<circle cx=\"" + d1(cx) + "\" cy=\"" + d1(cy) + "\" r=\"" + d1(r) + "\" " + style + "/>" + "<ellipse cx=\"" + d1(cx) + "\" cy=\"" +
                        d1(cy) + "\" rx=\"" + d1(rx) + "\" ry=\"" + d1(r) + "\" " + style + "/>" + "<line x1=\"" + d1(cx - r) + "\" y1=\"" + d1(cy) + "\" x2=\"" +
                        d1(cx + r) + "\" y2=\"" + d1(cy) + "\" " + style + "/>";
        if (vertical)
            s += "<line x1=\"" + d1(cx) + "\" y1=\"" + d1(cy - r) + "\" x2=\"" + d1(cx) + "\" y2=\"" + d1(cy + r) + "\" " + style + "/>";
        return s;
    };
    auto server = [&]() {
        float bx = cx - r, by = cy - r, bw = r * 2.0f, bh = r * 2.0f;
        int rows = 3;
        float row_h = bh / static_cast<float>(rows);
        std::string s;
        for (int i = 0; i < rows; ++i) {
            float ry = by + static_cast<float>(i) * row_h;
            s += "<rect x=\"" + d1(bx) + "\" y=\"" + d1(ry) + "\" width=\"" + d1(bw) + "\" height=\"" + d1(row_h) + "\" rx=\"2\" " + style + "/>";
            float dot_x = bx + bw - row_h * 0.35f, dot_y = ry + row_h * 0.5f, dot_r = row_h * 0.12f;
            s += "<circle cx=\"" + d1(dot_x) + "\" cy=\"" + d1(dot_y) + "\" r=\"" + d1(dot_r) + "\" fill=\"" + fill + "\" stroke=\"none\"/>";
        }
        return s;
    };
    auto database = [&](bool with_lines) {
        float bx = cx - r, bw = r * 2.0f, ell_ry = r * 0.3f;
        float body_top = cy - r + ell_ry, body_bot = cy + r - ell_ry, body_h = body_bot - body_top;
        std::string s = "<rect x=\"" + d1(bx) + "\" y=\"" + d1(body_top) + "\" width=\"" + d1(bw) + "\" height=\"" + d1(body_h) + "\" " + style + "/>" +
                        "<ellipse cx=\"" + d1(cx) + "\" cy=\"" + d1(body_top) + "\" rx=\"" + d1(r) + "\" ry=\"" + d1(ell_ry) + "\" " + style + "/>" +
                        "<ellipse cx=\"" + d1(cx) + "\" cy=\"" + d1(body_bot) + "\" rx=\"" + d1(r) + "\" ry=\"" + d1(ell_ry) + "\" " + style + "/>";
        if (with_lines) {
            float x1 = bx, x2 = bx + bw;
            s += "<line x1=\"" + d1(x1) + "\" y1=\"" + d1(body_top) + "\" x2=\"" + d1(x1) + "\" y2=\"" + d1(body_bot) + "\" " + style + "/>";
            s += "<line x1=\"" + d1(x2) + "\" y1=\"" + d1(body_top) + "\" x2=\"" + d1(x2) + "\" y2=\"" + d1(body_bot) + "\" " + style + "/>";
        }
        return s;
    };
    auto cloud = [&]() {
        float s = r * 0.7f;
        float x1 = cx - s, x2 = cx + s, y_mid = cy, y_top = cy - s * 0.8f, y_bot = cy + s * 0.6f;
        return "<path d=\"M " + d1(x1) + " " + d1(y_mid) + " Q " + d1(x1) + " " + d1(y_top) + " " + d1(cx) + " " + d1(y_top) + " Q " + d1(x2) + " " + d1(y_top) +
               " " + d1(x2) + " " + d1(y_mid) + " Q " + d1(x2) + " " + d1(y_bot) + " " + d1(cx) + " " + d1(y_bot) + " Q " + d1(x1) + " " + d1(y_bot) + " " +
               d1(x1) + " " + d1(y_mid) + " Z\" " + style + "/>";
    };
    if (icon == "internet" || icon == "globe") return globe(true);
    if (has("internet") || has("gateway") || has("api-gateway")) return globe(false);
    if (icon == "server") return server();
    if (has("server") || has("ec2") || has("compute")) return server();
    if (icon == "database" || icon == "disk") return database(true);
    if (has("database") || has("aurora") || has("rds") || has("dynamodb") || has("db") || has("disk") || has("storage") || has("s3") || has("glacier")) return database(false);
    if (icon == "cloud") return cloud();
    if (has("cloud")) return cloud();
    if (has("lambda")) {
        float fs = w * 0.72f;
        return "<text x=\"" + d1(cx) + "\" y=\"" + d1(cy) + "\" text-anchor=\"middle\" dominant-baseline=\"central\" fill=\"" + fill + "\" font-size=\"" +
               fmt_fixed(fs, 0) + "\" font-family=\"serif\">\xCE\xBB</text>";
    }
    float s = r * 0.9f, x = cx - s, y = cy - s, inner = s * 0.35f;
    float w2 = s * 2.0f;
    return "<rect x=\"" + d1(x) + "\" y=\"" + d1(y) + "\" width=\"" + d1(w2) + "\" height=\"" + d1(w2) + "\" rx=\"4\" " + style + "/>" + "<line x1=\"" + d1(cx - inner) +
           "\" y1=\"" + d1(cy) + "\" x2=\"" + d1(cx + inner) + "\" y2=\"" + d1(cy) + "\" " + style + "/>" + "<line x1=\"" + d1(cx) + "\" y1=\"" + d1(cy - inner) +
           "\" x2=\"" + d1(cx) + "\" y2=\"" + d1(cy + inner) + "\" " + style + "/>";
}

}  // namespace

std::string render_architecture_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config) {
    (void)config;
    using svg::escape_xml;
    using svg::F;
    using svg::F2;
    using svg::F3;
    float width = fmax_(layout.width, 1.0f), height = fmax_(layout.height, 1.0f);
    std::vector<std::string> edge_colors;
    for (const auto& e : layout.edges)
        if (e.override_style.stroke) edge_colors.push_back(*e.override_style.stroke);
    std::string s = svg::document_start(width, height, 0.0f, 0.0f, width, height, false, false, theme, true, &edge_colors);

    std::vector<std::string> colors{theme.line_color};
    for (const auto& c : edge_colors)
        if (std::find(colors.begin(), colors.end(), c) == colors.end()) colors.push_back(c);
    auto color_id = [&](const std::string& c) {
        for (size_t i = 0; i < colors.size(); ++i)
            if (colors[i] == c) return i;
        return size_t(0);
    };
    const std::string ICON_TEXT_FILL = "#ffffff";
    const float GROUP_ICON_SIZE = 30.0f, GROUP_ICON_OFFSET = 1.0f;
    std::string fam = svg::normalize_font_family(theme.font_family);

    s += "<g class=\"architecture-edges\">";
    for (const auto& edge : layout.edges) {
        if (edge.points.size() < 2) continue;
        const std::string& stroke = edge.override_style.stroke ? *edge.override_style.stroke : theme.line_color;
        float stroke_width = edge.override_style.stroke_width ? *edge.override_style.stroke_width : 3.0f;
        size_t marker_idx = color_id(stroke);
        std::string dash_attr = edge.override_style.dasharray ? " stroke-dasharray=\"" + *edge.override_style.dasharray + "\"" : "";
        std::string marker_start = edge.arrow_start ? " marker-start=\"url(#arrow-start-" + std::to_string(marker_idx) + ")\"" : "";
        std::string marker_end = edge.arrow_end ? " marker-end=\"url(#arrow-" + std::to_string(marker_idx) + ")\"" : "";
        s += "<path d=\"" + svg::points_to_path(edge.points) + "\" fill=\"none\" stroke=\"" + escape_xml(stroke) + "\" stroke-width=\"" + F(stroke_width) + "\"" +
             marker_start + marker_end + dash_attr + " />";
    }
    s += "</g>";

    s += "<g class=\"architecture-services\">";
    for (const auto& [id, node] : layout.nodes) {
        if (node.hidden) continue;
        bool all_blank = true;
        for (const auto& l : node.label.lines)
            if (!trim_view(l).empty()) all_blank = false;
        bool is_junction = (node.icon && *node.icon == "junction") || (node.shape == NodeShape::Circle && all_blank);
        std::string icon_fill = node.style.fill ? *node.style.fill : "#087ebf";
        std::string label_text = node.id;
        for (const auto& l : node.label.lines)
            if (!trim_view(l).empty()) { label_text = l; break; }
        float label_y = node.height + theme.font_size + 8.0f;
        s += "<g id=\"service-" + escape_xml(node.id) + "\" class=\"architecture-service\" transform=\"translate(" + F3(node.x) + "," + F3(node.y) + ")\">";
        if (is_junction) {
            s += "<circle cx=\"" + F3(node.width / 2.0f) + "\" cy=\"" + F3(node.height / 2.0f) + "\" r=\"" + F3(fmin_(node.width, node.height) / 2.0f) + "\" fill=\"" +
                 escape_xml(icon_fill) + "\" stroke=\"none\" />";
            s += "</g>";
            continue;
        }
        s += "<rect width=\"" + F(node.width) + "\" height=\"" + F(node.height) + "\" fill=\"" + escape_xml(icon_fill) + "\" stroke=\"none\" />";
        s += architecture_icon_svg(node.icon, node.width, node.height, ICON_TEXT_FILL);
        s += "<text x=\"" + F3(node.width / 2.0f) + "\" y=\"" + F3(label_y) + "\" text-anchor=\"middle\" font-family=\"" + fam + "\" font-size=\"" + F(theme.font_size) +
             "\" fill=\"" + escape_xml(theme.primary_text_color) + "\">" + escape_xml(label_text) + "</text>";
        s += "</g>";
    }
    s += "</g>";

    s += "<g class=\"architecture-groups\">";
    for (const auto& sg : layout.subgraphs) {
        std::string stroke = sg.style.stroke ? *sg.style.stroke : "hsl(240, 60%, 86.2745098039%)";
        float stroke_width = sg.style.stroke_width ? *sg.style.stroke_width : 2.0f;
        std::string dash_attr = sg.style.stroke_dasharray ? " stroke-dasharray=\"" + *sg.style.stroke_dasharray + "\"" : "";
        // sanitize_group_suffix: per character (not per byte), trimmed of '-', "group" when empty
        std::string out;
        {
            size_t i = 0;
            while (i < sg.label.size()) {
                unsigned char c = static_cast<unsigned char>(sg.label[i]);
                size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
                if (len == 1 && std::isalnum(c)) out += static_cast<char>(std::tolower(c));
                else if (len == 1 && (c == '_' || c == '-')) out += static_cast<char>(c);
                else out += '-';
                i += len;
            }
        }
        size_t a = 0, b = out.size();
        while (a < b && out[a] == '-') ++a;
        while (b > a && out[b - 1] == '-') --b;
        std::string group_id = a == b ? "group" : out.substr(a, b - a);
        s += "<rect id=\"group-" + escape_xml(group_id) + "\" x=\"" + F3(sg.x) + "\" y=\"" + F3(sg.y) + "\" width=\"" + F3(sg.width) + "\" height=\"" + F3(sg.height) +
             "\" fill=\"none\" stroke=\"" + escape_xml(stroke) + "\" stroke-width=\"" + F(stroke_width) + "\"" + dash_attr + " />";
        s += "<g transform=\"translate(" + F3(sg.x + GROUP_ICON_OFFSET) + "," + F3(sg.y + GROUP_ICON_OFFSET) + ")\">";
        s += "<rect width=\"" + F(GROUP_ICON_SIZE) + "\" height=\"" + F(GROUP_ICON_SIZE) + "\" fill=\"#087ebf\" stroke=\"none\" />";
        s += architecture_icon_svg(sg.icon, GROUP_ICON_SIZE, GROUP_ICON_SIZE, ICON_TEXT_FILL);
        s += "</g>";
        float label_x = sg.x + GROUP_ICON_SIZE + 4.0f, label_y = sg.y + GROUP_ICON_SIZE * 0.7f;
        std::string first_line = sg.label;
        {
            auto ls = lines_of(sg.label);
            bool found = false;
            for (const auto& l : ls)
                if (!trim_view(l).empty()) { first_line = l; found = true; break; }
            (void)found;
        }
        s += "<text x=\"" + F3(label_x) + "\" y=\"" + F3(label_y) + "\" text-anchor=\"start\" font-family=\"" + fam + "\" font-size=\"" + F(theme.font_size) +
             "\" fill=\"" + escape_xml(theme.primary_text_color) + "\">" + escape_xml(first_line) + "</text>";
    }
    s += "</g></svg>";
    return s;
}

}  // namespace mermaid
