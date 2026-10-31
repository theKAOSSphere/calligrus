// Mermaid port: subgraph handling for flowcharts. Mirrors layout/subgraphs.rs (tree, padding, box
// layouts, direction overrides) and layout/flowchart/subgraph_spacing.rs (keeping sibling boxes
// apart, evicting free nodes, aligning disconnected groups and components).
//
// Not ported (throws Unsupported in the caller): subgraphs that are edge endpoints ("anchor"
// nodes) and nested directions that differ from the graph direction.
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "layout_internal.h"
#include "ranking.h"
#include "routing.h"
#include "strutil.h"
#include "text.h"

namespace mermaid {

namespace {

constexpr float FLOWCHART_PAD_MAIN = 40.0f;
constexpr float FLOWCHART_PAD_CROSS = 30.0f;
constexpr float SUBGRAPH_LABEL_GAP_FLOWCHART = 6.0f;
constexpr float SUBGRAPH_DESIRED_GAP_RATIO = 1.6f;
constexpr float MIN_NODE_SPACING_FLOOR = 16.0f;

constexpr float F32_MAX = std::numeric_limits<float>::max();
constexpr float F32_MIN = std::numeric_limits<float>::lowest();  // Rust f32::MIN

// f32::max / f32::min / clamp helpers (NaN ignored like Rust's).
float fmax_(float a, float b) { return std::fmax(a, b); }
float fmin_(float a, float b) { return std::fmin(a, b); }
int cmp_f(float a, float b) { return a < b ? -1 : (a > b ? 1 : 0); }

bool horizontal_dir(Direction d) { return d == Direction::LeftRight || d == Direction::RightLeft; }

// Bounding box of the member nodes that exist in `nodes`. min_x == F32_MAX means "none found".
struct NodeBox {
    float min_x = F32_MAX, min_y = F32_MAX, max_x = F32_MIN, max_y = F32_MIN;
    bool empty() const { return min_x == F32_MAX; }
};
NodeBox member_box(const std::vector<std::string>& ids, const std::map<std::string, NodeLayout>& nodes) {
    NodeBox b;
    for (const auto& id : ids) {
        auto it = nodes.find(id);
        if (it == nodes.end()) continue;
        const NodeLayout& n = it->second;
        b.min_x = fmin_(b.min_x, n.x);
        b.min_y = fmin_(b.min_y, n.y);
        b.max_x = fmax_(b.max_x, n.x + n.width);
        b.max_y = fmax_(b.max_y, n.y + n.height);
    }
    return b;
}

// Subgraph label block with width/height zeroed for an empty label (the Rust code does this
// in most, but not all, callers: `zero_if_empty` says which).
TextBlock measured_sub_label(const Subgraph& sub, const Theme& theme, const LayoutConfig& config, bool zero_if_empty) {
    TextBlock b = measure_label(sub.label, theme, config);
    if (zero_if_empty && trim_view(sub.label).empty()) {
        b.width = 0.0f;
        b.height = 0.0f;
    }
    return b;
}

void shift_member_nodes(const Subgraph& sub, std::map<std::string, NodeLayout>& nodes, bool horizontal_axis_is_x,
                        float delta) {
    for (const auto& id : sub.nodes) {
        auto it = nodes.find(id);
        if (it == nodes.end()) continue;
        if (horizontal_axis_is_x) it->second.x += delta;
        else it->second.y += delta;
    }
}

// Subgraph tree (nesting by member-set inclusion).
struct SubgraphTree {
    std::vector<std::optional<size_t>> parent;
    std::vector<std::vector<size_t>> children;
    std::vector<size_t> top_level;

    static SubgraphTree build(const Graph& graph) {
        size_t n = graph.subgraphs.size();
        std::vector<std::set<std::string>> sets;
        for (const auto& sub : graph.subgraphs) sets.emplace_back(sub.nodes.begin(), sub.nodes.end());

        std::vector<size_t> by_size(n);
        for (size_t i = 0; i < n; ++i) by_size[i] = i;
        std::stable_sort(by_size.begin(), by_size.end(), [&](size_t a, size_t b) { return sets[a].size() < sets[b].size(); });

        SubgraphTree t;
        t.parent.assign(n, std::nullopt);
        t.children.assign(n, {});
        for (size_t pos = 0; pos < by_size.size(); ++pos) {
            size_t i = by_size[pos];
            for (size_t q = pos + 1; q < by_size.size(); ++q) {
                size_t j = by_size[q];
                if (sets[j].size() > sets[i].size() &&
                    std::includes(sets[j].begin(), sets[j].end(), sets[i].begin(), sets[i].end())) {
                    t.parent[i] = j;
                    t.children[j].push_back(i);
                    break;
                }
            }
        }
        for (size_t i = 0; i < n; ++i)
            if (!t.parent[i]) t.top_level.push_back(i);
        return t;
    }

    bool is_ancestor(size_t ancestor, size_t descendant) const {
        size_t cur = descendant;
        for (;;) {
            if (!parent[cur]) return false;
            if (*parent[cur] == ancestor) return true;
            cur = *parent[cur];
        }
    }
    bool are_siblings(size_t a, size_t b) const { return a != b && !is_ancestor(a, b) && !is_ancestor(b, a); }
};

}  // namespace

bool is_region_subgraph(const Subgraph& sub) {
    return trim_view(sub.label).empty() && sub.id && starts_with(*sub.id, "__region_");
}

std::vector<size_t> top_level_subgraph_indices(const Graph& graph) { return SubgraphTree::build(graph).top_level; }

std::optional<std::string> subgraph_anchor_id(const Subgraph& sub, const std::map<std::string, NodeLayout>& nodes) {
    auto is_member = [&](const std::string& id) {
        return std::find(sub.nodes.begin(), sub.nodes.end(), id) != sub.nodes.end();
    };
    if (sub.id && nodes.count(*sub.id) && !is_member(*sub.id)) return *sub.id;
    if (nodes.count(sub.label) && !is_member(sub.label)) return sub.label;
    return std::nullopt;
}

// subgraph_padding_from_label: (pad_x, pad_y, top_padding) for a subgraph box (per diagram kind).
std::tuple<float, float, float> subgraph_padding_from_label(const Graph& graph, const Subgraph& sub, const Theme& theme,
                                                            const TextBlock& label_block) {
    if (is_region_subgraph(sub)) return {0.0f, 0.0f, 0.0f};
    const bool flowchart = graph.kind == DiagramKind::Flowchart;

    bool label_empty = trim_view(sub.label).empty();
    float label_height = label_empty ? 0.0f : label_block.height;
    size_t internal_edge_count = 0;
    if (flowchart) {
        std::set<std::string> node_set(sub.nodes.begin(), sub.nodes.end());
        for (const Edge& e : graph.edges)
            if (node_set.count(e.from) && node_set.count(e.to)) ++internal_edge_count;
    }
    bool has_internal_cycle = flowchart && internal_edge_count >= std::max<size_t>(sub.nodes.size(), 1);

    float pad_x, pad_y;
    if (flowchart) {
        if (horizontal_dir(graph.direction)) { pad_x = FLOWCHART_PAD_MAIN; pad_y = FLOWCHART_PAD_CROSS; }
        else { pad_x = FLOWCHART_PAD_CROSS; pad_y = FLOWCHART_PAD_MAIN; }
    } else if (graph.kind == DiagramKind::Kanban) {
        pad_x = 8.0f; pad_y = 8.0f;
    } else {
        float base_padding = graph.kind == DiagramKind::State ? 16.0f : 24.0f;
        pad_x = base_padding; pad_y = base_padding;
    }

    bool has_long_edge_label = false;
    for (const Edge& e : graph.edges)
        if (e.label && char_count(*e.label) > 24) has_long_edge_label = true;
    if (flowchart && sub.nodes.size() <= 3 &&
        ((horizontal_dir(graph.direction) && graph.edges.size() <= 20) ||
         (!horizontal_dir(graph.direction) && graph.edges.size() <= 13)) &&
        !has_long_edge_label) {
        float reduction = has_internal_cycle ? 1.0f : (label_empty ? 0.7f : 0.9f);
        pad_x *= reduction;
        pad_y *= reduction;
    }
    if (flowchart && has_internal_cycle) pad_y += fmax_(theme.font_size * 0.75f, 10.0f);

    float top_padding;
    if (label_empty) {
        top_padding = pad_y;
    } else if (flowchart) {
        float title_clearance = fmax_(theme.font_size * 0.75f, 10.0f);
        top_padding = fmax_(pad_y, label_height + SUBGRAPH_LABEL_GAP_FLOWCHART + title_clearance);
    } else if (graph.kind == DiagramKind::Kanban) {
        top_padding = fmax_(pad_y, label_height + 4.0f);
    } else if (graph.kind == DiagramKind::State) {
        top_padding = fmax_(label_height + theme.font_size * 0.75f, theme.font_size * 1.4f);
    } else {
        top_padding = pad_y + label_height + 8.0f;
    }
    return {pad_x, pad_y, top_padding};
}

NodeStyle resolve_subgraph_style(const Subgraph& sub, const Graph& graph) {
    NodeStyle style;
    if (!sub.id) return style;
    if (auto it = graph.subgraph_classes.find(*sub.id); it != graph.subgraph_classes.end())
        for (const auto& cn : it->second)
            if (auto def = graph.class_defs.find(cn); def != graph.class_defs.end()) merge_node_style(style, def->second);
    if (auto it = graph.subgraph_styles.find(*sub.id); it != graph.subgraph_styles.end())
        merge_node_style(style, it->second);
    return style;
}

std::vector<SubgraphLayout> build_subgraph_layouts(const Graph& graph, const std::map<std::string, NodeLayout>& nodes,
                                                   const Theme& theme, const LayoutConfig& config) {
    std::vector<SubgraphLayout> subgraphs;
    std::vector<std::optional<size_t>> graph_to_local;  // graph.subgraphs index -> local index
    for (const Subgraph& sub : graph.subgraphs) {
        NodeBox box = member_box(sub.nodes, nodes);
        if (box.empty()) {
            graph_to_local.push_back(std::nullopt);
            continue;
        }
        NodeStyle style = resolve_subgraph_style(sub, graph);
        bool label_empty = trim_view(sub.label).empty();
        TextBlock label_block = measured_sub_label(sub, theme, config, true);
        auto [padding_x, padding_y, top_padding] = subgraph_padding_from_label(graph, sub, theme, label_block);

        float node_width = box.max_x - box.min_x;
        float base_width = node_width + padding_x * 2.0f;
        float min_label_width = label_empty ? base_width : label_block.width + padding_x * 2.0f;
        float width = fmax_(base_width, min_label_width);
        float extra_width = width - base_width;

        graph_to_local.push_back(subgraphs.size());
        SubgraphLayout sl;
        sl.label = sub.label;
        sl.label_block = label_block;
        sl.nodes = sub.nodes;
        sl.x = box.min_x - padding_x - extra_width / 2.0f;
        sl.y = box.min_y - top_padding;
        sl.width = width;
        sl.height = (box.max_y - box.min_y) + padding_y + top_padding;
        sl.style = style;
        subgraphs.push_back(std::move(sl));
    }

    if (subgraphs.size() > 1) {
        SubgraphTree tree = SubgraphTree::build(graph);
        size_t n = graph.subgraphs.size();
        std::vector<std::vector<size_t>> all_descendants(n);
        std::vector<size_t> order;
        // Post-order walk of the tree: children are processed before their parents.
        std::vector<std::pair<size_t, bool>> stack;
        for (size_t k = tree.top_level.size(); k-- > 0;) stack.push_back({tree.top_level[k], false});
        while (!stack.empty()) {
            auto [idx, visited] = stack.back();
            stack.pop_back();
            if (visited) {
                order.push_back(idx);
                continue;
            }
            stack.push_back({idx, true});
            for (size_t k = tree.children[idx].size(); k-- > 0;) stack.push_back({tree.children[idx][k], false});
        }
        for (size_t idx : order) {
            std::vector<size_t> descs;
            for (size_t child : tree.children[idx]) {
                descs.push_back(child);
                for (size_t d : all_descendants[child]) descs.push_back(d);
            }
            all_descendants[idx] = std::move(descs);
        }
        // A parent box grows to contain its nested children (with a 12px margin).
        for (size_t i : order) {
            if (!graph_to_local[i]) continue;
            size_t local_i = *graph_to_local[i];
            for (size_t j : all_descendants[i]) {
                if (is_region_subgraph(graph.subgraphs[j])) continue;
                if (!graph_to_local[j]) continue;
                size_t local_j = *graph_to_local[j];
                float pad = graph.kind == DiagramKind::State ? fmax_(theme.font_size * 1.8f, 24.0f) : 12.0f;
                float child_x = subgraphs[local_j].x, child_y = subgraphs[local_j].y;
                float child_w = subgraphs[local_j].width, child_h = subgraphs[local_j].height;
                SubgraphLayout& parent = subgraphs[local_i];
                float min_x = fmin_(parent.x, child_x - pad);
                float min_y = fmin_(parent.y, child_y - pad);
                float max_x = fmax_(parent.x + parent.width, child_x + child_w + pad);
                float max_y = fmax_(parent.y + parent.height, child_y + child_h + pad);
                parent.x = min_x;
                parent.y = min_y;
                parent.width = max_x - min_x;
                parent.height = max_y - min_y;
            }
        }
    }

    // Largest boxes first (drawn first, so nested boxes end up on top).
    std::stable_sort(subgraphs.begin(), subgraphs.end(), [](const SubgraphLayout& a, const SubgraphLayout& b) {
        return cmp_f(b.width * b.height, a.width * a.height) < 0;
    });
    return subgraphs;
}

std::map<std::string, std::vector<size_t>> node_subgraph_memberships(const Graph& graph) {
    std::map<std::string, std::vector<size_t>> m;
    for (size_t idx = 0; idx < graph.subgraphs.size(); ++idx)
        for (const auto& id : graph.subgraphs[idx].nodes) m[id].push_back(idx);
    for (auto& [id, v] : m) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    return m;
}

bool memberships_differ(const std::map<std::string, std::vector<size_t>>& m, const std::string& a, const std::string& b) {
    auto ia = m.find(a), ib = m.find(b);
    bool has_a = ia != m.end(), has_b = ib != m.end();
    if (has_a != has_b) return true;  // Option<&Vec>: None != Some
    if (!has_a) return false;
    return ia->second != ib->second;
}


// ======================================================================================
// subgraph_spacing.rs
// ======================================================================================


// ---- subgraphs with their own `direction` ----

// layout/mod.rs assign_positions: lay `node_ids` out rank by rank starting at (origin_x, origin_y).
static void assign_positions(const std::vector<std::string>& node_ids, const RankMap& ranks, Direction direction,
                             const LayoutConfig& config, std::map<std::string, NodeLayout>& nodes, float origin_x, float origin_y) {
    std::unordered_map<std::string, size_t> node_order;
    for (size_t i = 0; i < node_ids.size(); ++i) node_order[node_ids[i]] = i;  // (a later duplicate wins, like collect())
    size_t max_rank = 0;
    for (const auto& [id, r] : ranks) max_rank = std::max(max_rank, r);
    std::vector<std::vector<std::string>> rank_nodes(max_rank + 1);
    for (const auto& id : node_ids) {
        auto it = ranks.find(id);
        size_t rank = it == ranks.end() ? 0 : it->second;
        if (rank < rank_nodes.size()) rank_nodes[rank].push_back(id);
    }
    for (auto& bucket : rank_nodes)
        std::stable_sort(bucket.begin(), bucket.end(), [&](const std::string& a, const std::string& b) {
            auto ia = node_order.find(a), ib = node_order.find(b);
            size_t oa = ia == node_order.end() ? std::numeric_limits<size_t>::max() : ia->second;
            size_t ob = ib == node_order.end() ? std::numeric_limits<size_t>::max() : ib->second;
            return oa < ob;
        });
    float main_cursor = 0.0f;
    bool horizontal = horizontal_dir(direction);
    for (const auto& bucket : rank_nodes) {
        float cross_cursor = 0.0f;
        float max_main = 0.0f;
        for (const auto& id : bucket) {
            auto it = nodes.find(id);
            if (it == nodes.end()) continue;
            NodeLayout& node = it->second;
            if (horizontal) {
                node.x = origin_x + main_cursor;
                node.y = origin_y + cross_cursor;
                cross_cursor += node.height + config.node_spacing;
                max_main = fmax_(max_main, node.width);
            } else {
                node.x = origin_x + cross_cursor;
                node.y = origin_y + main_cursor;
                cross_cursor += node.width + config.node_spacing;
                max_main = fmax_(max_main, node.height);
            }
        }
        main_cursor += max_main + config.rank_spacing;
    }
}

static void mirror_subgraph_nodes(const std::vector<std::string>& node_ids, std::map<std::string, NodeLayout>& nodes, Direction direction) {
    NodeBox b = member_box(node_ids, nodes);
    if (b.empty()) return;
    if (direction == Direction::RightLeft)
        for (const auto& id : node_ids)
            if (auto it = nodes.find(id); it != nodes.end()) it->second.x = b.min_x + (b.max_x - (it->second.x + it->second.width));
    if (direction == Direction::BottomTop)
        for (const auto& id : node_ids)
            if (auto it = nodes.find(id); it != nodes.end()) it->second.y = b.min_y + (b.max_y - (it->second.y + it->second.height));
}

// apply_subgraph_direction_overrides: a subgraph with `direction LR` inside a TD graph is laid out again on its own.
void apply_subgraph_direction_overrides(const Graph& graph, std::map<std::string, NodeLayout>& nodes, const LayoutConfig& config,
                                        const std::set<size_t>& skip_indices) {
    for (size_t sub_idx = 0; sub_idx < graph.subgraphs.size(); ++sub_idx) {
        const Subgraph& sub = graph.subgraphs[sub_idx];
        if (skip_indices.count(sub_idx)) continue;
        if (is_region_subgraph(sub)) continue;
        Direction direction = sub.direction ? *sub.direction : graph.direction;  // subgraph_layout_direction
        if (sub.nodes.empty() || direction == graph.direction) continue;
        NodeBox b = member_box(sub.nodes, nodes);
        if (b.empty()) continue;
        float min_x = b.min_x, min_y = b.min_y;

        std::map<std::string, NodeLayout> temp_nodes;
        for (const auto& id : sub.nodes)
            if (auto it = nodes.find(id); it != nodes.end()) {
                NodeLayout clone = it->second;
                clone.x = 0.0f;
                clone.y = 0.0f;
                temp_nodes.emplace(id, std::move(clone));
            }
        RankMap ranks = compute_ranks_subset(sub.nodes, graph.edges, graph.node_order);
        assign_positions(sub.nodes, ranks, direction, config, temp_nodes, 0.0f, 0.0f);
        float temp_min_x = F32_MAX, temp_min_y = F32_MAX;
        for (const auto& id : sub.nodes)
            if (auto it = temp_nodes.find(id); it != temp_nodes.end()) {
                temp_min_x = fmin_(temp_min_x, it->second.x);
                temp_min_y = fmin_(temp_min_y, it->second.y);
            }
        if (temp_min_x == F32_MAX) continue;
        for (const auto& id : sub.nodes) {
            auto target = nodes.find(id);
            auto source = temp_nodes.find(id);
            if (target == nodes.end() || source == temp_nodes.end()) continue;
            target->second.x = source->second.x - temp_min_x + min_x;
            target->second.y = source->second.y - temp_min_y + min_y;
        }
        if (direction == Direction::RightLeft || direction == Direction::BottomTop) mirror_subgraph_nodes(sub.nodes, nodes, direction);
    }
}

// align_single_entry_top_level_subgraphs: a subgraph whose inner direction is perpendicular to the
// outer one and that has exactly one incoming edge is shifted so the entry node lines up with its feeder.
static void align_single_entry_top_level_subgraphs(const Graph& graph, std::map<std::string, NodeLayout>& nodes,
                                                   const LayoutConfig& config) {
    if (graph.subgraphs.empty()) return;
    bool outer_horizontal = horizontal_dir(graph.direction);
    float shift_limit = fmax_(config.node_spacing * 0.75f, 10.0f);
    for (size_t idx : top_level_subgraph_indices(graph)) {
        const Subgraph& sub = graph.subgraphs[idx];
        if (sub.nodes.empty() || is_region_subgraph(sub)) continue;
        Direction inner_direction = sub.direction ? *sub.direction : graph.direction;
        bool inner_horizontal = horizontal_dir(inner_direction);
        if (inner_horizontal == outer_horizontal) continue;
        std::set<std::string> node_set(sub.nodes.begin(), sub.nodes.end());
        std::vector<const Edge*> incoming, outgoing;
        for (const Edge& e : graph.edges) {
            if (!node_set.count(e.from) && node_set.count(e.to)) incoming.push_back(&e);
            if (node_set.count(e.from) && !node_set.count(e.to)) outgoing.push_back(&e);
        }
        if (incoming.size() != 1 || !outgoing.empty()) continue;
        const Edge& feeder = *incoming[0];
        auto sit = nodes.find(feeder.from), tit = nodes.find(feeder.to);
        if (sit == nodes.end() || tit == nodes.end()) continue;
        const NodeLayout& source = sit->second;
        const NodeLayout& target = tit->second;
        float source_center = outer_horizontal ? source.y + source.height * 0.5f : source.x + source.width * 0.5f;
        float target_center = outer_horizontal ? target.y + target.height * 0.5f : target.x + target.width * 0.5f;
        float delta = source_center - target_center;
        if (std::fabs(delta) < 0.5f || std::fabs(delta) > shift_limit) continue;
        float extreme = F32_MAX;
        for (const auto& id : sub.nodes)
            if (auto it = nodes.find(id); it != nodes.end()) extreme = fmin_(extreme, inner_horizontal ? it->second.x : it->second.y);
        bool is_edge_member = inner_horizontal ? std::fabs(target.x - extreme) <= 1.0f : std::fabs(target.y - extreme) <= 1.0f;
        if (!is_edge_member) continue;
        shift_member_nodes(sub, nodes, !outer_horizontal, delta);
    }
}

// A chain of >= 3 nodes inside one subgraph is packed tightly along the main axis.
static void compress_linear_subgraphs(const Graph& graph, std::map<std::string, NodeLayout>& nodes,
                                      const LayoutConfig& config) {
    if (graph.subgraphs.empty()) return;
    float gap = config.flowchart.auto_spacing.min_spacing;
    bool horizontal = horizontal_dir(graph.direction);

    for (const Subgraph& sub : graph.subgraphs) {
        if (sub.nodes.size() < 3) continue;
        std::set<std::string> sub_set(sub.nodes.begin(), sub.nodes.end());
        std::unordered_map<std::string, size_t> in_deg, out_deg;
        std::unordered_map<std::string, std::string> next_map;
        size_t edges_in_sub = 0;
        for (const auto& id : sub.nodes) { in_deg[id] = 0; out_deg[id] = 0; }
        for (const Edge& e : graph.edges) {
            if (!sub_set.count(e.from) || !sub_set.count(e.to)) continue;
            ++edges_in_sub;
            size_t& out = out_deg[e.from];
            ++out;
            if (out == 1) next_map[e.from] = e.to;
            else next_map.erase(e.from);
            ++in_deg[e.to];
        }
        if (edges_in_sub + 1 != sub.nodes.size()) continue;
        bool bad = false;
        for (const auto& [k, d] : in_deg) if (d > 1) bad = true;
        for (const auto& [k, d] : out_deg) if (d > 1) bad = true;
        if (bad) continue;

        std::vector<const std::string*> starts;
        for (const auto& id : sub.nodes)
            if (in_deg[id] == 0) starts.push_back(&id);
        if (starts.size() != 1) continue;

        std::vector<std::string> order;
        std::unordered_set<std::string> visited;
        std::string current = *starts[0];
        while (visited.insert(current).second) {
            order.push_back(current);
            auto it = next_map.find(current);
            if (it != next_map.end()) current = it->second;
            else break;
        }
        if (order.size() != sub.nodes.size()) continue;

        float min_main = F32_MAX;
        for (const auto& id : order) {
            auto it = nodes.find(id);
            if (it == nodes.end()) continue;
            min_main = fmin_(min_main, horizontal ? it->second.x : it->second.y);
        }
        float cursor = min_main;
        for (const auto& id : order) {
            auto it = nodes.find(id);
            if (it == nodes.end()) continue;
            if (horizontal) { it->second.x = cursor; cursor += it->second.width + gap; }
            else { it->second.y = cursor; cursor += it->second.height + gap; }
        }
    }
}

void enforce_top_level_subgraph_gap(const Graph& graph, std::map<std::string, NodeLayout>& nodes, const Theme& theme,
                                    const LayoutConfig& config) {
    if (graph.kind != DiagramKind::Flowchart || graph.subgraphs.size() < 2) return;
    std::vector<size_t> top_level = top_level_subgraph_indices(graph);
    if (top_level.size() < 2) return;

    std::set<std::string> seen;
    for (size_t idx : top_level)
        for (const auto& id : graph.subgraphs[idx].nodes)
            if (!seen.insert(id).second) return;

    std::unordered_map<std::string, size_t> node_to_top;
    for (size_t idx : top_level)
        for (const auto& id : graph.subgraphs[idx].nodes) node_to_top[id] = idx;
    bool has_cross_sg_edge = false;
    for (const Edge& e : graph.edges) {
        auto a = node_to_top.find(e.from), b = node_to_top.find(e.to);
        if (a != node_to_top.end() && b != node_to_top.end() && a->second != b->second) has_cross_sg_edge = true;
    }
    if (!has_cross_sg_edge) return;

    struct Bounds {
        size_t idx;
        float min_x, min_y, max_x, max_y, pad_main;
    };
    bool horizontal = horizontal_dir(graph.direction);
    std::vector<Bounds> bounds;
    for (size_t idx : top_level) {
        const Subgraph& sub = graph.subgraphs[idx];
        if (is_region_subgraph(sub) || sub.nodes.empty()) continue;
        NodeBox box = member_box(sub.nodes, nodes);
        if (box.empty()) continue;
        TextBlock label_block = measured_sub_label(sub, theme, config, true);
        auto [pad_x, pad_y, top_padding] = subgraph_padding_from_label(graph, sub, theme, label_block);
        bounds.push_back({idx, box.min_x - pad_x, box.min_y - top_padding, box.max_x + pad_x, box.max_y + pad_y,
                          horizontal ? pad_x : pad_y});
    }
    if (bounds.size() < 2) return;

    std::stable_sort(bounds.begin(), bounds.end(), [&](const Bounds& a, const Bounds& b) {
        int c = cmp_f(horizontal ? a.min_x : a.min_y, horizontal ? b.min_x : b.min_y);
        if (c != 0) return c < 0;
        return a.idx < b.idx;
    });

    float pad_main = 0.0f;
    for (const auto& b : bounds) pad_main = fmax_(pad_main, b.pad_main);
    float desired_gap = fmax_(config.node_spacing * SUBGRAPH_DESIRED_GAP_RATIO, pad_main * 2.0f);

    std::optional<float> prev_max_main;
    for (Bounds& bound : bounds) {
        float min_main = horizontal ? bound.min_x : bound.min_y;
        float max_main = horizontal ? bound.max_x : bound.max_y;
        float delta = 0.0f;
        if (prev_max_main) {
            float required_min = *prev_max_main + desired_gap;
            if (min_main < required_min) delta = required_min - min_main;
        }
        if (delta > 0.0f) {
            shift_member_nodes(graph.subgraphs[bound.idx], nodes, horizontal, delta);
            if (horizontal) { bound.min_x += delta; bound.max_x += delta; }
            else { bound.min_y += delta; bound.max_y += delta; }
            max_main += delta;
        }
        prev_max_main = max_main;
    }
}

void separate_sibling_subgraphs(const Graph& graph, std::map<std::string, NodeLayout>& nodes, const Theme& theme,
                                const LayoutConfig& config) {
    if (graph.kind != DiagramKind::Flowchart || graph.subgraphs.size() < 2) return;
    SubgraphTree tree = SubgraphTree::build(graph);
    std::vector<std::vector<size_t>> sibling_groups;
    std::set<size_t> assigned;
    for (size_t i = 0; i < graph.subgraphs.size(); ++i) {
        if (assigned.count(i)) continue;
        std::vector<size_t> group{i};
        assigned.insert(i);
        for (size_t j = i + 1; j < graph.subgraphs.size(); ++j) {
            if (assigned.count(j)) continue;
            bool is_sibling = true;
            for (size_t k : group)
                if (!tree.are_siblings(j, k)) { is_sibling = false; break; }
            if (is_sibling) { group.push_back(j); assigned.insert(j); }
        }
        if (group.size() > 1) sibling_groups.push_back(std::move(group));
    }

    bool horizontal = horizontal_dir(graph.direction);
    struct B {
        size_t idx;
        float min_x, min_y, max_x, max_y;
    };
    for (const auto& group : sibling_groups) {
        std::vector<B> bounds;
        for (size_t idx : group) {
            const Subgraph& sub = graph.subgraphs[idx];
            NodeBox box = member_box(sub.nodes, nodes);
            if (box.empty()) continue;
            TextBlock label_block = measured_sub_label(sub, theme, config, false);  // not zeroed here
            auto [pad_x, pad_y, top_padding] = subgraph_padding_from_label(graph, sub, theme, label_block);
            bounds.push_back({idx, box.min_x - pad_x, box.min_y - top_padding, box.max_x + pad_x, box.max_y + pad_y});
        }
        if (bounds.size() < 2) continue;
        // (Rust sorts on tuple field .2 = min_y when horizontal, .1 = min_x otherwise.)
        if (horizontal)
            std::stable_sort(bounds.begin(), bounds.end(), [](const B& a, const B& b) { return cmp_f(a.min_y, b.min_y) < 0; });
        else
            std::stable_sort(bounds.begin(), bounds.end(), [](const B& a, const B& b) { return cmp_f(a.min_x, b.min_x) < 0; });

        float gap = fmax_(config.node_spacing, 8.0f);
        auto overlaps = [](float a_min, float a_max, float b_min, float b_max) { return a_min < b_max && b_min < a_max; };

        std::vector<B> placed;
        for (const B& b : bounds) {
            float shift = 0.0f;
            for (const B& p : placed) {
                bool other_axis_overlaps = horizontal ? overlaps(b.min_x, b.max_x, p.min_x, p.max_x)
                                                      : overlaps(b.min_y, b.max_y, p.min_y, p.max_y);
                if (!other_axis_overlaps) continue;
                float shifted_min = horizontal ? b.min_y + shift : b.min_x + shift;
                float shifted_max = horizontal ? b.max_y + shift : b.max_x + shift;
                float placed_min = horizontal ? p.min_y : p.min_x;
                float placed_max = horizontal ? p.max_y : p.max_x;
                if (overlaps(shifted_min, shifted_max, placed_min, placed_max)) {
                    float needed = placed_max + gap - shifted_min;
                    if (needed > shift) shift = needed;
                }
            }
            if (shift > 0.0f) shift_member_nodes(graph.subgraphs[b.idx], nodes, !horizontal, shift);
            placed.push_back(horizontal ? B{b.idx, b.min_x, b.min_y + shift, b.max_x, b.max_y + shift}
                                        : B{b.idx, b.min_x + shift, b.min_y, b.max_x + shift, b.max_y});
        }
    }
}

// align_single_entry_top_level_subgraphs: only acts when a subgraph's inner direction is
// perpendicular to the outer one; the port rejects explicit subgraph directions, so it is a no-op.
// align_disconnected_top_level_subgraphs: top-level groups with no edges between them share a start line.
static void align_disconnected_top_level_subgraphs(const Graph& graph, std::map<std::string, NodeLayout>& nodes) {
    if (graph.subgraphs.size() < 2) return;
    std::vector<size_t> top_level = top_level_subgraph_indices(graph);
    if (top_level.size() < 2) return;

    std::set<std::string> seen;
    size_t union_count = 0;
    for (size_t idx : top_level)
        for (const auto& id : graph.subgraphs[idx].nodes) {
            if (!seen.insert(id).second) return;
            ++union_count;
        }
    if (union_count != graph.nodes.size()) return;

    std::unordered_map<std::string, size_t> node_to_top;
    for (size_t idx : top_level)
        for (const auto& id : graph.subgraphs[idx].nodes) node_to_top[id] = idx;
    for (const Edge& e : graph.edges) {
        auto a = node_to_top.find(e.from), b = node_to_top.find(e.to);
        if (a != node_to_top.end() && b != node_to_top.end() && a->second != b->second) return;
    }

    struct B {
        size_t idx;
        float min_x, min_y, max_x, max_y;
    };
    std::vector<B> bounds;
    for (size_t idx : top_level) {
        const Subgraph& sub = graph.subgraphs[idx];
        if (sub.nodes.empty()) continue;
        NodeBox box = member_box(sub.nodes, nodes);
        if (box.empty()) continue;
        bounds.push_back({idx, box.min_x, box.min_y, box.max_x, box.max_y});
    }
    if (bounds.size() < 2) return;

    bool horizontal = horizontal_dir(graph.direction);
    std::stable_sort(bounds.begin(), bounds.end(), [&](const B& a, const B& b) {
        int c = cmp_f(horizontal ? a.min_x : a.min_y, horizontal ? b.min_x : b.min_y);
        if (c != 0) return c < 0;
        return a.idx < b.idx;
    });
    std::optional<float> prev_max;
    for (const B& b : bounds) {
        float min_main = horizontal ? b.min_x : b.min_y;
        float max_main = horizontal ? b.max_x : b.max_y;
        if (prev_max && min_main < *prev_max) return;
        prev_max = max_main;
    }
    float target_cross = F32_MAX;
    for (const B& b : bounds) target_cross = fmin_(target_cross, horizontal ? b.min_y : b.min_x);
    for (const B& b : bounds) {
        float current_cross = horizontal ? b.min_y : b.min_x;
        float delta = target_cross - current_cross;
        if (std::fabs(delta) < 0.5f) continue;
        shift_member_nodes(graph.subgraphs[b.idx], nodes, !horizontal, delta);
    }
}

// Push nodes that belong to no subgraph out of subgraph boxes they landed in.
void evict_non_member_nodes_from_subgraphs(const Graph& graph, std::map<std::string, NodeLayout>& nodes,
                                           const Theme& theme, const LayoutConfig& config) {
    if (graph.kind != DiagramKind::Flowchart || graph.subgraphs.empty()) return;
    bool horizontal = horizontal_dir(graph.direction);
    float clearance = fmax_(config.node_spacing * 0.5f, MIN_NODE_SPACING_FLOOR);

    auto compute_rects = [&](const std::map<std::string, NodeLayout>& ns) {
        std::vector<std::array<float, 4>> rects;
        for (const auto& sub : build_subgraph_layouts(graph, ns, theme, config))
            if (sub.width > 0.0f && sub.height > 0.0f) rects.push_back({sub.x, sub.y, sub.x + sub.width, sub.y + sub.height});
        return rects;
    };

    std::unordered_set<std::string> all_members;
    for (const auto& sub : graph.subgraphs)
        for (const auto& id : sub.nodes) all_members.insert(id);
    std::vector<std::string> free_ids;
    for (const auto& [id, node] : nodes)
        if (!node.hidden && !node.anchor_subgraph && !all_members.count(node.id)) free_ids.push_back(node.id);
    if (free_ids.empty()) return;

    for (int iter = 0; iter < 4; ++iter) {
        auto rects = compute_rects(nodes);
        bool moved = false;
        for (const auto& id : free_ids) {
            auto nit = nodes.find(id);
            if (nit == nodes.end()) continue;
            float nx1 = nit->second.x, ny1 = nit->second.y;
            float nx2 = nit->second.x + nit->second.width, ny2 = nit->second.y + nit->second.height;
            auto overlaps_any = [&](float x1, float y1, float x2, float y2) {
                size_t n = 0;
                for (const auto& r : rects)
                    if (fmin_(x2, r[2]) - fmax_(x1, r[0]) > 0.0f && fmin_(y2, r[3]) - fmax_(y1, r[1]) > 0.0f) ++n;
                return n;
            };
            if (overlaps_any(nx1, ny1, nx2, ny2) == 0) continue;
            // Candidate pushes: escape each overlapped rect on all four sides; keep the one that
            // clears the most boxes with the least displacement.
            bool have_best = false;
            size_t best_remaining = 0;
            float best_score = 0.0f, best_dx = 0.0f, best_dy = 0.0f;
            for (const auto& r : rects) {
                float overlap_x = fmin_(nx2, r[2]) - fmax_(nx1, r[0]);
                float overlap_y = fmin_(ny2, r[3]) - fmax_(ny1, r[1]);
                if (overlap_x <= 0.0f || overlap_y <= 0.0f) continue;
                float cands[4][2] = {{-(nx2 - r[0] + clearance), 0.0f},
                                     {r[2] - nx1 + clearance, 0.0f},
                                     {0.0f, -(ny2 - r[1] + clearance)},
                                     {0.0f, r[3] - ny1 + clearance}};
                for (auto& c : cands) {
                    float dx = c[0], dy = c[1];
                    size_t remaining = overlaps_any(nx1 + dx, ny1 + dy, nx2 + dx, ny2 + dy);
                    float displacement = std::fabs(dx) + std::fabs(dy);
                    bool main_axis_move = horizontal ? dx != 0.0f : dy != 0.0f;
                    float score = displacement * (main_axis_move ? 1.25f : 1.0f);
                    bool better = !have_best || remaining < best_remaining || (remaining == best_remaining && score < best_score);
                    if (better) {
                        have_best = true;
                        best_remaining = remaining;
                        best_score = score;
                        best_dx = dx;
                        best_dy = dy;
                    }
                }
            }
            if (have_best) {
                nit->second.x += best_dx;
                nit->second.y += best_dy;
                moved = true;
            }
        }
        if (!moved) break;
    }
}

// Several disconnected components (no subgraphs): lay them out one after another on the main axis.
static void align_disconnected_components(const Graph& graph, std::map<std::string, NodeLayout>& nodes,
                                          const LayoutConfig& config) {
    if (!graph.subgraphs.empty()) return;
    std::vector<std::string> visible_nodes;
    for (const auto& [id, n] : nodes)
        if (!n.hidden) visible_nodes.push_back(n.id);
    if (visible_nodes.size() < 2) return;
    std::sort(visible_nodes.begin(), visible_nodes.end());

    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    for (const auto& id : visible_nodes) adjacency[id];
    for (const Edge& e : graph.edges) {
        if (!adjacency.count(e.from) || !adjacency.count(e.to)) continue;
        adjacency[e.from].push_back(e.to);
        adjacency[e.to].push_back(e.from);
    }
    std::unordered_set<std::string> visited;
    std::vector<std::vector<std::string>> components;
    for (const auto& id : visible_nodes) {
        if (visited.count(id)) continue;
        std::vector<std::string> stack{id};
        std::vector<std::string> component;
        visited.insert(id);
        while (!stack.empty()) {
            std::string current = stack.back();
            stack.pop_back();
            component.push_back(current);
            auto it = adjacency.find(current);
            if (it != adjacency.end())
                for (const auto& next : it->second)
                    if (visited.insert(next).second) stack.push_back(next);
        }
        if (!component.empty()) components.push_back(std::move(component));
    }
    if (components.size() < 2) return;

    struct CB {
        std::vector<std::string> nodes;
        float min_x, min_y, max_x, max_y;
    };
    std::vector<CB> bounds;
    for (auto& component : components) {
        NodeBox box = member_box(component, nodes);
        if (box.empty()) continue;
        bounds.push_back({component, box.min_x, box.min_y, box.max_x, box.max_y});
    }
    if (bounds.size() < 2) return;

    bool horizontal = horizontal_dir(graph.direction);
    std::stable_sort(bounds.begin(), bounds.end(), [&](const CB& a, const CB& b) {
        return cmp_f(horizontal ? a.min_x : a.min_y, horizontal ? b.min_x : b.min_y) < 0;
    });
    float target_cross = F32_MAX;
    for (const CB& b : bounds) target_cross = fmin_(target_cross, horizontal ? b.min_y : b.min_x);
    float spacing = fmax_(config.node_spacing, MIN_NODE_SPACING_FLOOR);
    float cursor = F32_MAX;
    for (const CB& b : bounds) cursor = fmin_(cursor, horizontal ? b.min_x : b.min_y);

    for (const CB& b : bounds) {
        float min_main = horizontal ? b.min_x : b.min_y;
        float max_main = horizontal ? b.max_x : b.max_y;
        float current_cross = horizontal ? b.min_y : b.min_x;
        float delta_main = cursor - min_main;
        float delta_cross = target_cross - current_cross;
        for (const auto& id : b.nodes) {
            auto it = nodes.find(id);
            if (it == nodes.end()) continue;
            if (horizontal) { it->second.x += delta_main; it->second.y += delta_cross; }
            else { it->second.x += delta_cross; it->second.y += delta_main; }
        }
        cursor += fmax_(max_main - min_main, 1.0f) + spacing;
    }
}

// apply_flowchart_node_layout_cleanup
void apply_flowchart_node_layout_cleanup(const Graph& graph, std::map<std::string, NodeLayout>& nodes,
                                         const Theme& theme, const LayoutConfig& config) {
    if (graph.kind != DiagramKind::Flowchart) return;
    compress_linear_subgraphs(graph, nodes, config);
    if (graph.subgraphs.empty()) {
        align_disconnected_components(graph, nodes, config);
        return;
    }
    enforce_top_level_subgraph_gap(graph, nodes, theme, config);
    separate_sibling_subgraphs(graph, nodes, theme, config);
    align_single_entry_top_level_subgraphs(graph, nodes, config);
    align_disconnected_top_level_subgraphs(graph, nodes);
    evict_non_member_nodes_from_subgraphs(graph, nodes, theme, config);
}

// ======================================================================================
// objectives.rs: top-level subgraph groups move together
// ======================================================================================

SubgraphGroups collect_flowchart_top_level_groups(const Graph& graph) {
    SubgraphGroups g;
    if (graph.kind != DiagramKind::Flowchart || graph.subgraphs.empty()) return g;
    for (size_t idx : top_level_subgraph_indices(graph)) {
        const Subgraph& sub = graph.subgraphs[idx];
        if (is_region_subgraph(sub)) continue;
        std::vector<std::string> in_group;
        for (const auto& id : sub.nodes) {
            auto [it, inserted] = g.node_to_group.insert({id, idx});
            if (!inserted) {
                size_t existing = it->second;
                it->second = idx;  // HashMap::insert replaces the value
                if (existing != idx) return SubgraphGroups{};  // a node in two top-level groups: no grouping
            }
            in_group.push_back(id);
        }
        if (!in_group.empty()) g.group_nodes[idx] = std::move(in_group);
    }
    if (g.group_nodes.empty()) return SubgraphGroups{};
    g.valid = true;
    return g;
}


// ======================================================================================
// Subgraph anchors ("a subgraph used as an edge endpoint"): layout/subgraphs.rs
// ======================================================================================

namespace {

Direction subgraph_layout_direction(const Graph& graph, const Subgraph& sub) {
    if (graph.kind == DiagramKind::State) return graph.direction;
    return sub.direction ? *sub.direction : graph.direction;
}

constexpr float STATE_RANK_SPACING_BOOST = 25.0f;

LayoutConfig subgraph_layout_config(const Graph& graph, bool anchorable, const LayoutConfig& config) {
    LayoutConfig local = config;
    if (graph.kind == DiagramKind::Flowchart && anchorable) local.rank_spacing = config.rank_spacing + STATE_RANK_SPACING_BOOST;
    return local;
}

bool subgraph_is_anchorable(const Subgraph& sub, const Graph& graph, const std::map<std::string, NodeLayout>& nodes) {
    if (sub.nodes.empty()) return false;
    std::optional<std::string> anchor_id = subgraph_anchor_id(sub, nodes);
    std::set<std::string> set(sub.nodes.begin(), sub.nodes.end());
    for (const Edge& edge : graph.edges) {
        if (anchor_id && (edge.from == *anchor_id || edge.to == *anchor_id)) return false;
        bool from_in = set.count(edge.from) > 0, to_in = set.count(edge.to) > 0;
        if (from_in != to_in) return false;
    }
    return true;
}

bool subgraph_should_anchor(const Subgraph& sub, const Graph& graph, const std::map<std::string, NodeLayout>& nodes) {
    if (sub.nodes.empty()) return false;
    if (graph.kind == DiagramKind::Flowchart || graph.kind == DiagramKind::State) return subgraph_anchor_id(sub, nodes).has_value();
    return subgraph_is_anchorable(sub, graph, nodes);
}

// (width, height, padding_x, top_padding) of the box a subgraph would get, laid out on its own.
std::optional<std::array<float, 4>> estimate_subgraph_box_size(const Graph& graph, const Subgraph& sub,
                                                               const std::map<std::string, NodeLayout>& nodes, const Theme& theme,
                                                               const LayoutConfig& config, bool anchorable) {
    if (sub.nodes.empty()) return std::nullopt;
    Direction direction = subgraph_layout_direction(graph, sub);
    std::map<std::string, NodeLayout> temp_nodes;
    for (const auto& id : sub.nodes)
        if (auto it = nodes.find(id); it != nodes.end()) {
            NodeLayout clone = it->second;
            clone.x = 0.0f;
            clone.y = 0.0f;
            temp_nodes.emplace(id, std::move(clone));
        }
    LayoutConfig local_config = subgraph_layout_config(graph, anchorable, config);
    RankMap ranks = compute_ranks_subset(sub.nodes, graph.edges, graph.node_order);
    assign_positions(sub.nodes, ranks, direction, local_config, temp_nodes, 0.0f, 0.0f);
    NodeBox b = member_box(sub.nodes, temp_nodes);
    if (b.empty()) return std::nullopt;
    TextBlock label_block = measured_sub_label(sub, theme, config, true);
    auto [padding_x, padding_y, top_padding] = subgraph_padding_from_label(graph, sub, theme, label_block);
    float width = (b.max_x - b.min_x) + padding_x * 2.0f;
    float height = (b.max_y - b.min_y) + padding_y + top_padding;
    return std::array<float, 4>{width, height, padding_x, top_padding};
}

}  // namespace

std::set<std::string> mark_subgraph_anchor_nodes_hidden(const Graph& graph, std::map<std::string, NodeLayout>& nodes) {
    std::set<std::string> anchor_ids;
    for (const Subgraph& sub : graph.subgraphs) {
        auto anchor_id = subgraph_anchor_id(sub, nodes);
        if (!anchor_id) continue;
        anchor_ids.insert(*anchor_id);
        if (auto it = nodes.find(*anchor_id); it != nodes.end()) it->second.hidden = true;
    }
    return anchor_ids;
}

std::optional<std::string> pick_subgraph_anchor_child(const Subgraph& sub, const Graph& graph, const std::set<std::string>& anchor_ids) {
    std::vector<const std::string*> candidates;
    for (const auto& id : sub.nodes)
        if (!anchor_ids.count(id)) candidates.push_back(&id);
    if (candidates.empty())
        for (const auto& id : sub.nodes) candidates.push_back(&id);
    auto order_of = [&](const std::string* id) {
        auto it = graph.node_order.find(*id);
        return it == graph.node_order.end() ? std::numeric_limits<size_t>::max() : it->second;
    };
    std::stable_sort(candidates.begin(), candidates.end(), [&](const std::string* a, const std::string* b) { return order_of(a) < order_of(b); });
    if (candidates.empty()) return std::nullopt;
    return *candidates.front();
}

AnchorInfoMap apply_subgraph_anchor_sizes(const Graph& graph, std::map<std::string, NodeLayout>& nodes, const Theme& theme,
                                          const LayoutConfig& config) {
    AnchorInfoMap anchors;
    if (graph.subgraphs.empty()) return anchors;
    for (size_t idx = 0; idx < graph.subgraphs.size(); ++idx) {
        const Subgraph& sub = graph.subgraphs[idx];
        if (is_region_subgraph(sub) || !subgraph_should_anchor(sub, graph, nodes)) continue;
        auto anchor_id = subgraph_anchor_id(sub, nodes);
        if (!anchor_id) continue;
        auto est = estimate_subgraph_box_size(graph, sub, nodes, theme, config, true);
        if (!est) continue;
        if (auto it = nodes.find(*anchor_id); it != nodes.end()) {
            it->second.width = (*est)[0];
            it->second.height = (*est)[1];
        }
        anchors[*anchor_id] = SubgraphAnchorInfo{idx, (*est)[2], (*est)[3]};
    }
    return anchors;
}

std::set<std::string> align_subgraphs_to_anchor_nodes(const Graph& graph, const AnchorInfoMap& anchor_info,
                                                      std::map<std::string, NodeLayout>& nodes, const LayoutConfig& config) {
    std::set<std::string> anchored_nodes;
    if (anchor_info.empty()) return anchored_nodes;
    SubgraphTree tree = SubgraphTree::build(graph);
    size_t sub_count = graph.subgraphs.size();
    std::vector<size_t> subgraph_depth(sub_count, 0);
    for (size_t idx = 0; idx < sub_count; ++idx) {
        size_t depth = 0, cur = idx;
        while (cur < tree.parent.size() && tree.parent[cur]) {
            ++depth;
            cur = *tree.parent[cur];
            if (depth > sub_count) break;
        }
        subgraph_depth[idx] = depth;
    }
    std::vector<std::pair<const std::string*, const SubgraphAnchorInfo*>> ordered;
    for (const auto& [id, info] : anchor_info) ordered.push_back({&id, &info});
    std::stable_sort(ordered.begin(), ordered.end(), [&](const auto& a, const auto& b) {
        size_t ad = a.second->sub_idx < sub_count ? subgraph_depth[a.second->sub_idx] : std::numeric_limits<size_t>::max();
        size_t bd = b.second->sub_idx < sub_count ? subgraph_depth[b.second->sub_idx] : std::numeric_limits<size_t>::max();
        if (ad != bd) return ad < bd;
        if (a.second->sub_idx != b.second->sub_idx) return a.second->sub_idx < b.second->sub_idx;
        return *a.first < *b.first;
    });
    for (const auto& [anchor_id, info] : ordered) {
        auto ait = nodes.find(*anchor_id);
        if (ait == nodes.end()) continue;
        float anchor_x = ait->second.x, anchor_y = ait->second.y;
        if (info->sub_idx >= graph.subgraphs.size()) continue;
        const Subgraph& sub = graph.subgraphs[info->sub_idx];
        Direction direction = subgraph_layout_direction(graph, sub);
        LayoutConfig local_config = subgraph_layout_config(graph, true, config);
        RankMap ranks = compute_ranks_subset(sub.nodes, graph.edges, graph.node_order);
        assign_positions(sub.nodes, ranks, direction, local_config, nodes, anchor_x + info->padding_x, anchor_y + info->top_padding);
        if (direction == Direction::RightLeft || direction == Direction::BottomTop) mirror_subgraph_nodes(sub.nodes, nodes, direction);
        for (const auto& id : sub.nodes) anchored_nodes.insert(id);
    }
    return anchored_nodes;
}

void apply_state_subgraph_layouts(const Graph& graph, std::map<std::string, NodeLayout>& nodes, const LayoutConfig& config,
                                  const std::set<size_t>& skip_indices) {
    size_t sub_count = graph.subgraphs.size();
    std::vector<size_t> depth(sub_count, 0);
    std::vector<std::optional<size_t>> parent_of(sub_count);
    auto contains_node = [](const Subgraph& s, const std::string& id) {
        return std::find(s.nodes.begin(), s.nodes.end(), id) != s.nodes.end();
    };
    for (size_t i = 0; i < sub_count; ++i)
        for (size_t j = 0; j < sub_count; ++j) {
            if (i == j) continue;
            const Subgraph& sub_a = graph.subgraphs[i];
            const Subgraph& sub_b = graph.subgraphs[j];
            std::string b_id = sub_b.id ? *sub_b.id : "";
            if ((contains_node(sub_a, b_id) || contains_node(sub_a, sub_b.label)) && !parent_of[j]) parent_of[j] = i;
        }
    for (size_t i = 0; i < sub_count; ++i) {
        size_t d = 0, cur = i;
        while (parent_of[cur]) {
            ++d;
            cur = *parent_of[cur];
            if (d > sub_count) break;
        }
        depth[i] = d;
    }
    std::vector<size_t> order(sub_count);
    for (size_t i = 0; i < sub_count; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return depth[b] < depth[a]; });

    std::map<size_t, std::array<float, 4>> inner_boxes;  // x, y, w, h
    for (size_t idx : order) {
        const Subgraph& sub = graph.subgraphs[idx];
        if (skip_indices.count(idx) || sub.nodes.size() <= 1) continue;
        NodeBox b0 = member_box(sub.nodes, nodes);
        if (b0.empty()) continue;
        float min_x = b0.min_x, min_y = b0.min_y;

        std::vector<std::tuple<std::string, float, float>> saved_sizes;
        std::vector<std::string> inner_anchor_ids;
        for (const auto& node_id : sub.nodes)
            for (size_t j = 0; j < sub_count; ++j) {
                auto ib = inner_boxes.find(j);
                if (ib == inner_boxes.end()) continue;
                const Subgraph& inner_sub = graph.subgraphs[j];
                std::string inner_id = inner_sub.id ? *inner_sub.id : "";
                if (node_id == inner_id || node_id == inner_sub.label) {
                    if (std::find(inner_anchor_ids.begin(), inner_anchor_ids.end(), node_id) == inner_anchor_ids.end()) inner_anchor_ids.push_back(node_id);
                    if (auto nit = nodes.find(node_id); nit != nodes.end()) saved_sizes.push_back({node_id, nit->second.width, nit->second.height});
                    if (auto nit = nodes.find(node_id); nit != nodes.end()) {
                        nit->second.width = ib->second[2];
                        nit->second.height = ib->second[3];
                    }
                }
            }

        RankMap ranks = compute_ranks_subset(sub.nodes, graph.edges, graph.node_order);
        assign_positions(sub.nodes, ranks, graph.direction, config, nodes, min_x, min_y);

        float nested_anchor_min_y = min_y + fmax_(config.node_spacing * 0.4f, 20.0f);
        for (const auto& anchor_id : inner_anchor_ids)
            if (auto it = nodes.find(anchor_id); it != nodes.end() && it->second.y < nested_anchor_min_y) it->second.y = nested_anchor_min_y;

        for (const auto& [id, w, h] : saved_sizes)
            if (auto it = nodes.find(id); it != nodes.end()) {
                it->second.width = w;
                it->second.height = h;
            }

        for (size_t j = 0; j < sub_count; ++j) {
            auto ib = inner_boxes.find(j);
            if (ib == inner_boxes.end()) continue;
            float old_x = ib->second[0], old_y = ib->second[1];
            const Subgraph& inner_sub = graph.subgraphs[j];
            std::string inner_id = inner_sub.id ? *inner_sub.id : "";
            if (!(contains_node(sub, inner_id) || contains_node(sub, inner_sub.label))) continue;
            const std::string& anchor_id = contains_node(sub, inner_id) ? inner_id : inner_sub.label;
            auto ait = nodes.find(anchor_id);
            if (ait == nodes.end()) continue;
            float dx = ait->second.x - old_x, dy = ait->second.y - old_y;
            if (std::fabs(dx) > 0.01f || std::fabs(dy) > 0.01f)
                for (const auto& inner_node_id : inner_sub.nodes)
                    if (auto it = nodes.find(inner_node_id); it != nodes.end()) {
                        it->second.x += dx;
                        it->second.y += dy;
                    }
        }

        NodeBox bb = member_box(sub.nodes, nodes);
        float bmin_x = bb.min_x, bmin_y = bb.min_y, bmax_x = bb.max_x, bmax_y = bb.max_y;
        for (size_t j = 0; j < sub_count; ++j) {
            if (!inner_boxes.count(j)) continue;
            const Subgraph& inner_sub = graph.subgraphs[j];
            std::string inner_id = inner_sub.id ? *inner_sub.id : "";
            if (contains_node(sub, inner_id) || contains_node(sub, inner_sub.label))
                for (const auto& inner_node_id : inner_sub.nodes)
                    if (auto it = nodes.find(inner_node_id); it != nodes.end()) {
                        const NodeLayout& n = it->second;
                        bmin_x = fmin_(bmin_x, n.x);
                        bmin_y = fmin_(bmin_y, n.y);
                        bmax_x = fmax_(bmax_x, n.x + n.width);
                        bmax_y = fmax_(bmax_y, n.y + n.height);
                    }
        }
        float padding = config.node_spacing;
        if (bmin_x < F32_MAX) inner_boxes[idx] = {bmin_x, bmin_y, bmax_x - bmin_x + padding, bmax_y - bmin_y + padding};
    }
}

void apply_orthogonal_region_bands(const Graph& graph, std::map<std::string, NodeLayout>& nodes, const LayoutConfig& config) {
    std::vector<size_t> region_indices;
    for (size_t idx = 0; idx < graph.subgraphs.size(); ++idx)
        if (is_region_subgraph(graph.subgraphs[idx])) region_indices.push_back(idx);
    if (region_indices.empty()) return;
    std::vector<std::set<std::string>> sets;
    for (const auto& sub : graph.subgraphs) sets.emplace_back(sub.nodes.begin(), sub.nodes.end());
    std::map<size_t, std::vector<size_t>> parent_map;
    for (size_t region_idx : region_indices) {
        const auto& region_set = sets[region_idx];
        std::optional<size_t> parent;
        for (size_t idx = 0; idx < sets.size(); ++idx) {
            const auto& set = sets[idx];
            if (idx == region_idx || set.size() <= region_set.size() ||
                !std::includes(set.begin(), set.end(), region_set.begin(), region_set.end()))
                continue;
            if (is_region_subgraph(graph.subgraphs[idx])) continue;
            if (!parent) parent = idx;
            else if (set.size() < sets[*parent].size()) parent = idx;
        }
        if (parent) parent_map[*parent].push_back(region_idx);
    }
    float spacing = config.rank_spacing * 0.6f;
    bool stack_along_x = horizontal_dir(graph.direction);
    for (const auto& [parent_idx, region_list] : parent_map) {
        struct RB {
            size_t idx;
            float min_x, min_y, max_x, max_y;
        };
        std::vector<RB> boxes;
        for (size_t region_idx : region_list) {
            NodeBox b = member_box(graph.subgraphs[region_idx].nodes, nodes);
            if (!b.empty()) boxes.push_back({region_idx, b.min_x, b.min_y, b.max_x, b.max_y});
        }
        if (boxes.size() <= 1) continue;
        if (stack_along_x) {
            std::stable_sort(boxes.begin(), boxes.end(), [](const RB& a, const RB& b) { return a.min_x < b.min_x; });
            float cursor = boxes.front().min_x;
            for (const RB& rb : boxes) {
                float offset = cursor - rb.min_x;
                for (const auto& id : graph.subgraphs[rb.idx].nodes)
                    if (auto it = nodes.find(id); it != nodes.end()) it->second.x += offset;
                cursor += (rb.max_x - rb.min_x) + spacing;
            }
        } else {
            std::stable_sort(boxes.begin(), boxes.end(), [](const RB& a, const RB& b) { return a.min_y < b.min_y; });
            float cursor = boxes.front().min_y;
            for (const RB& rb : boxes) {
                float offset = cursor - rb.min_y;
                for (const auto& id : graph.subgraphs[rb.idx].nodes)
                    if (auto it = nodes.find(id); it != nodes.end()) it->second.y += offset;
                cursor += (rb.max_y - rb.min_y) + spacing;
            }
        }
    }
}

void apply_subgraph_node_layout_passes(const Graph& graph, std::map<std::string, NodeLayout>& nodes, const LayoutConfig& config,
                                       const std::set<size_t>& anchored_indices, const AnchorInfoMap& anchor_info) {
    if (graph.subgraphs.empty()) return;
    if (graph.kind != DiagramKind::State) apply_subgraph_direction_overrides(graph, nodes, config, anchored_indices);
    if (!anchor_info.empty()) align_subgraphs_to_anchor_nodes(graph, anchor_info, nodes, config);
    if (graph.kind == DiagramKind::State && !anchor_info.empty()) apply_state_subgraph_layouts(graph, nodes, config, anchored_indices);
    apply_orthogonal_region_bands(graph, nodes, config);
    if (graph.kind != DiagramKind::State && graph.kind != DiagramKind::Flowchart) throw Unsupported("generic subgraph bands");
}

void apply_subgraph_anchors(const Graph& graph, const std::vector<SubgraphLayout>& subgraphs, std::map<std::string, NodeLayout>& nodes) {
    if (subgraphs.empty()) return;
    std::map<std::string, size_t> label_to_index;
    for (size_t idx = 0; idx < subgraphs.size(); ++idx) label_to_index[subgraphs[idx].label] = idx;
    for (const Subgraph& sub : graph.subgraphs) {
        auto lit = label_to_index.find(sub.label);
        if (lit == label_to_index.end()) continue;
        size_t layout_idx = lit->second;
        const SubgraphLayout& layout = subgraphs[layout_idx];
        std::set<std::string> anchor_ids;  // (HashSet in Rust: at most two distinct ids)
        if (sub.id) anchor_ids.insert(*sub.id);
        anchor_ids.insert(sub.label);
        for (const auto& anchor_id : anchor_ids) {
            if (std::find(sub.nodes.begin(), sub.nodes.end(), anchor_id) != sub.nodes.end()) continue;
            auto nit = nodes.find(anchor_id);
            if (nit == nodes.end()) continue;
            NodeLayout& node = nit->second;
            node.anchor_subgraph = layout_idx;
            float size = 2.0f;
            node.width = size;
            node.height = size;
            node.x = layout.x + layout.width / 2.0f - size / 2.0f;
            node.y = layout.y + layout.height / 2.0f - size / 2.0f;
        }
    }
}

NodeLayout anchor_layout_for_edge(const NodeLayout& anchor, const SubgraphLayout& subgraph, Direction direction, bool is_from) {
    float size = 2.0f;
    NodeLayout node = anchor;
    node.width = size;
    node.height = size;
    if (horizontal_dir(direction)) {
        node.x = is_from ? subgraph.x + subgraph.width - size : subgraph.x;
        node.y = subgraph.y + subgraph.height / 2.0f - size / 2.0f;
    } else {
        node.x = subgraph.x + subgraph.width / 2.0f - size / 2.0f;
        node.y = is_from ? subgraph.y + subgraph.height - size : subgraph.y;
    }
    return node;
}

NodeLayout anchor_layout_for_edge_towards(const NodeLayout& anchor, const SubgraphLayout& subgraph, const NodeLayout& remote,
                                          Direction direction, bool is_from) {
    float rcx = remote.x + remote.width * 0.5f, rcy = remote.y + remote.height * 0.5f;
    bool inside_x = rcx >= subgraph.x && rcx <= subgraph.x + subgraph.width;
    bool inside_y = rcy >= subgraph.y && rcy <= subgraph.y + subgraph.height;
    if (inside_x && inside_y) return anchor_layout_for_edge(anchor, subgraph, direction, is_from);

    float size = 2.0f;
    float min_x = subgraph.x, max_x = subgraph.x + subgraph.width, min_y = subgraph.y, max_y = subgraph.y + subgraph.height;
    float pad_raw = fmin_(subgraph.width, subgraph.height) * 0.08f;
    float pad = pad_raw < 0.0f ? 0.0f : (pad_raw > 18.0f ? 18.0f : pad_raw);
    auto clamp_x = [&](float x) { float lo = min_x + pad, hi = max_x - pad; return x < lo ? lo : (x > hi ? hi : x); };
    auto clamp_y = [&](float y) { float lo = min_y + pad, hi = max_y - pad; return y < lo ? lo : (y > hi ? hi : y); };
    EdgeSide preferred_side;
    if (horizontal_dir(direction)) preferred_side = is_from ? EdgeSide::Right : EdgeSide::Left;
    else preferred_side = is_from ? EdgeSide::Bottom : EdgeSide::Top;

    struct Cand {
        EdgeSide side;
        Point point;
    };
    Cand candidates[4] = {{EdgeSide::Left, {min_x, clamp_y(rcy)}},
                          {EdgeSide::Right, {max_x, clamp_y(rcy)}},
                          {EdgeSide::Top, {clamp_x(rcx), min_y}},
                          {EdgeSide::Bottom, {clamp_x(rcx), max_y}}};
    Cand best = candidates[0];
    float best_score = std::numeric_limits<float>::infinity();
    for (const Cand& c : candidates) {
        float manhattan = std::fabs(c.point.first - rcx) + std::fabs(c.point.second - rcy);
        float tie_bias = c.side == preferred_side ? -0.01f : 0.0f;
        float score = manhattan + tie_bias;
        if (score < best_score) {
            best = c;
            best_score = score;
        }
    }
    NodeLayout node = anchor;
    node.width = size;
    node.height = size;
    switch (best.side) {
    case EdgeSide::Left: node.x = min_x; node.y = best.point.second - size * 0.5f; break;
    case EdgeSide::Right: node.x = max_x - size; node.y = best.point.second - size * 0.5f; break;
    case EdgeSide::Top: node.x = best.point.first - size * 0.5f; node.y = min_y; break;
    case EdgeSide::Bottom: node.x = best.point.first - size * 0.5f; node.y = max_y - size; break;
    }
    return node;
}

std::optional<std::pair<NodeLayout, NodeLayout>> effective_edge_endpoint_layouts(const Graph& graph, const std::map<std::string, NodeLayout>& nodes,
                                                                                 const std::vector<SubgraphLayout>& subgraphs, const Edge& edge) {
    auto fit = nodes.find(edge.from);
    auto tit = nodes.find(edge.to);
    if (fit == nodes.end() || tit == nodes.end()) return std::nullopt;
    const NodeLayout& from_layout = fit->second;
    const NodeLayout& to_layout = tit->second;
    NodeLayout from = from_layout, to = to_layout;
    if (from_layout.anchor_subgraph && *from_layout.anchor_subgraph < subgraphs.size())
        from = anchor_layout_for_edge_towards(from_layout, subgraphs[*from_layout.anchor_subgraph], to_layout, graph.direction, true);
    if (to_layout.anchor_subgraph && *to_layout.anchor_subgraph < subgraphs.size())
        to = anchor_layout_for_edge_towards(to_layout, subgraphs[*to_layout.anchor_subgraph], from_layout, graph.direction, false);
    return std::make_pair(std::move(from), std::move(to));
}

}  // namespace mermaid
