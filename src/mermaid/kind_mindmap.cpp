// Mermaid port: mindmaps. Mirrors parser.rs parse_mindmap_diagram, layout/mindmap.rs (the radial layout, plus the
// tidy-tree / lr-tree layouts that an init directive can select) and the mindmap bits of the generic graph writer
// (svg_writer.cpp: percent width header, the "default" box shape, curveBasis edges).
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include "kind_util.h"
#include "kinds.h"
#include "numfmt.h"
#include "parser_internal.h"
#include "strutil.h"

namespace mermaid {

namespace {

// ---------------------------------------------------------------- parser

// parse_mindmap_shape: `((x))` circle, `{{x}}` hexagon, `[x]` rect, `(x)` rounded rect.
std::optional<std::pair<std::string, MindmapNodeType>> parse_mindmap_shape(const std::string& raw) {
    std::string t = trim(raw);
    auto inner = [&](size_t n) { return pp::strip_quotes(t.substr(n, t.size() - 2 * n)); };
    if (starts_with(t, "((") && ends_with(t, "))") && t.size() >= 4) return std::make_pair(inner(2), MindmapNodeType::Circle);
    if (starts_with(t, "{{") && ends_with(t, "}}") && t.size() >= 4) return std::make_pair(inner(2), MindmapNodeType::Hexagon);
    if (starts_with(t, "[") && ends_with(t, "]") && t.size() >= 2) return std::make_pair(inner(1), MindmapNodeType::Rect);
    if (starts_with(t, "(") && ends_with(t, ")") && t.size() >= 2) return std::make_pair(inner(1), MindmapNodeType::RoundedRect);
    return std::nullopt;
}

// sanitize_id: keep alphanumerics, turn runs of blanks / '-' / '_' into one '_', trim '_' at both ends.
std::string sanitize_id(const std::string& input) {
    std::string out;
    for (unsigned char ch : input) {
        // Rust's char::is_alphanumeric is Unicode-aware; the port only knows ASCII, so refuse the rest.
        if (ch >= 0x80) throw Unsupported("non-ASCII mindmap id");
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) out.push_back(static_cast<char>(ch));
        else if ((is_space(static_cast<char>(ch)) || ch == '-' || ch == '_') && !ends_with(out, "_")) out.push_back('_');
    }
    size_t a = 0, b = out.size();
    while (a < b && out[a] == '_') ++a;
    while (b > a && out[b - 1] == '_') --b;
    return out.substr(a, b - a);
}

struct MindmapToken {
    std::string id, label;
    MindmapNodeType node_type;
    std::vector<std::string> classes;
};

MindmapToken parse_mindmap_node_token(const std::string& token) {
    auto [base, classes] = pp::split_inline_classes(token);
    std::string trimmed = trim(base);
    if (trimmed.empty()) return {"", "", MindmapNodeType::Default, classes};

    std::string id;
    std::string label = trimmed;
    MindmapNodeType node_type = MindmapNodeType::Default;

    size_t found = trimmed.find_first_of("[({");
    size_t shape_start = found == std::string::npos ? 0 : found;
    if (shape_start > 0 && trimmed.substr(0, shape_start).find(' ') == std::string::npos) {
        id = trim(trimmed.substr(0, shape_start));
        std::string raw = trim(trimmed.substr(shape_start));
        if (auto shape = parse_mindmap_shape(raw)) {
            label = shape->first;
            node_type = shape->second;
        }
    } else if (auto shape = parse_mindmap_shape(trimmed)) {
        label = shape->first;
        node_type = shape->second;
    }
    if (id.empty()) id = sanitize_id(label);
    return {id, label, node_type, classes};
}

}  // namespace

Graph parse_mindmap_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::Mindmap;
    graph.direction = Direction::LeftRight;
    std::vector<std::string> stack;
    std::optional<size_t> base_indent;
    std::unordered_map<std::string, size_t> node_index;

    for (const std::string& raw_line : pp::preprocess_input_keep_indent(input)) {
        std::string trimmed = trim(raw_line);
        if (trimmed.empty()) continue;
        if (starts_with(to_ascii_lower(trimmed), "mindmap")) continue;

        size_t indent = pp::count_indent(raw_line);
        if (!base_indent) base_indent = indent;
        size_t rel_indent = indent > *base_indent ? indent - *base_indent : 0;
        size_t level = rel_indent / 2;
        if (level > stack.size()) level = stack.size();

        MindmapToken tok = parse_mindmap_node_token(trimmed);
        std::string id = tok.id;
        const std::string& label = tok.label;
        if (id.empty()) id = sanitize_id(label);
        if (id.empty()) id = "mindmap_" + std::to_string(graph.mindmap.nodes.size());
        if (graph.nodes.count(id)) id = id + "_" + std::to_string(graph.nodes.size());

        NodeShape shape;
        switch (tok.node_type) {
        case MindmapNodeType::Circle: shape = NodeShape::Circle; break;
        case MindmapNodeType::RoundedRect: shape = NodeShape::RoundRect; break;
        case MindmapNodeType::Rect: shape = NodeShape::Rectangle; break;
        case MindmapNodeType::Hexagon: shape = NodeShape::Hexagon; break;
        case MindmapNodeType::Cloud:
        case MindmapNodeType::Bang: shape = NodeShape::RoundRect; break;
        default: shape = NodeShape::MindmapDefault; break;
        }

        graph.ensure_node(id, label, shape);
        pp::apply_node_classes(graph, id, tok.classes);
        if (!graph.mindmap.root_id) graph.mindmap.root_id = id;

        if (level > 0 && stack.size() > level) stack.resize(level);
        std::optional<std::string> parent_id;
        if (level > 0 && !stack.empty()) parent_id = stack.back();

        // the section (colour group) is the index of the root's child a node descends from
        std::optional<size_t> section;
        if (level != 0 && parent_id) {
            auto parent_it = node_index.find(*parent_id);
            if (parent_it != node_index.end()) {
                const MindmapNode& parent = graph.mindmap.nodes[parent_it->second];
                if (parent.level == 0) section = parent.children.size();
                else section = parent.section;
            }
        }

        MindmapNode node;
        node.id = id;
        node.label = label;
        node.level = level;
        node.section = section;
        node.node_type = tok.node_type;
        size_t idx = graph.mindmap.nodes.size();
        graph.mindmap.nodes.push_back(std::move(node));
        node_index[id] = idx;

        if (parent_id) {
            auto parent_it = node_index.find(*parent_id);
            if (parent_it != node_index.end()) graph.mindmap.nodes[parent_it->second].children.push_back(id);
            Edge e;
            e.from = *parent_id;
            e.to = id;
            graph.edges.push_back(std::move(e));
        } else {
            stack.clear();
        }
        stack.push_back(id);
    }
    return graph;
}

// ---------------------------------------------------------------- layout

namespace {

// ---- the non-layered tidy tree (A. J. van der Ploeg / J. van Roosmalen), as in `mod tidy_tree`.
// Indices into Arena::nodes stand in for the original linked-list pointers.
namespace tidy_tree {

constexpr size_t kNone = SIZE_MAX;

struct TidyNode {
    float w = 0, h = 0, y = 0;
    std::vector<size_t> children;
    float x = 0, prelim = 0, modf = 0, shift = 0, change = 0;
    size_t tl = kNone, tr = kNone, el = kNone, er = kNone;
    float msel = 0, mser = 0;
};

struct Arena {
    std::vector<TidyNode> nodes;
    size_t alloc(float w, float h, float y, std::vector<size_t> children) {
        TidyNode n;
        n.w = w;
        n.h = h;
        n.y = y;
        n.children = std::move(children);
        nodes.push_back(std::move(n));
        return nodes.size() - 1;
    }
};

float bottom(const Arena& a, size_t t) { return a.nodes[t].y + a.nodes[t].h; }

void set_extremes(Arena& a, size_t t) {
    size_t cs = a.nodes[t].children.size();
    if (cs == 0) {
        a.nodes[t].el = t;
        a.nodes[t].er = t;
        a.nodes[t].msel = 0.0f;
        a.nodes[t].mser = 0.0f;
    } else {
        size_t first = a.nodes[t].children[0];
        size_t last = a.nodes[t].children[cs - 1];
        a.nodes[t].el = a.nodes[first].el;
        a.nodes[t].msel = a.nodes[first].msel;
        a.nodes[t].er = a.nodes[last].er;
        a.nodes[t].mser = a.nodes[last].mser;
    }
}

size_t next_left_contour(const Arena& a, size_t t) {
    return a.nodes[t].children.empty() ? a.nodes[t].tl : a.nodes[t].children[0];
}
size_t next_right_contour(const Arena& a, size_t t) {
    size_t cs = a.nodes[t].children.size();
    return cs == 0 ? a.nodes[t].tr : a.nodes[t].children[cs - 1];
}

void distribute_extra(Arena& a, size_t t, size_t i, size_t si, float distance) {
    if (si + 1 != i) {
        float nr = static_cast<float>(i - si);
        size_t mid = a.nodes[t].children[si + 1];
        size_t target = a.nodes[t].children[i];
        a.nodes[mid].shift += distance / nr;
        a.nodes[target].shift -= distance / nr;
        a.nodes[target].change -= distance - distance / nr;
    }
}

void move_subtree(Arena& a, size_t t, size_t i, size_t si, float distance) {
    size_t child = a.nodes[t].children[i];
    a.nodes[child].modf += distance;
    a.nodes[child].msel += distance;
    a.nodes[child].mser += distance;
    distribute_extra(a, t, i, si, distance);
}

void set_left_thread(Arena& a, size_t t, size_t i, size_t cl, float modsumcl) {
    size_t first = a.nodes[t].children[0];
    size_t li = a.nodes[first].el;
    a.nodes[li].tl = cl;
    float diff = (modsumcl - a.nodes[cl].modf) - a.nodes[first].msel;
    a.nodes[li].modf += diff;
    a.nodes[li].prelim -= diff;
    size_t target = a.nodes[t].children[i];
    a.nodes[first].el = a.nodes[target].el;
    a.nodes[first].msel = a.nodes[target].msel;
}

void set_right_thread(Arena& a, size_t t, size_t i, size_t sr, float modsumsr) {
    size_t cur = a.nodes[t].children[i];
    size_t prev = a.nodes[t].children[i - 1];
    size_t ri = a.nodes[cur].er;
    a.nodes[ri].tr = sr;
    float diff = (modsumsr - a.nodes[sr].modf) - a.nodes[cur].mser;
    a.nodes[ri].modf += diff;
    a.nodes[ri].prelim -= diff;
    a.nodes[cur].er = a.nodes[prev].er;
    a.nodes[cur].mser = a.nodes[prev].mser;
}

void separate(Arena& a, size_t t, size_t i, std::vector<std::pair<float, size_t>>& ih_stack) {
    size_t sr = a.nodes[t].children[i - 1];
    float mssr = a.nodes[sr].modf;
    size_t cl = a.nodes[t].children[i];
    float mscl = a.nodes[cl].modf;
    while (sr != kNone && cl != kNone) {
        if (bottom(a, sr) > ih_stack.back().first) ih_stack.pop_back();
        float distance = mssr + a.nodes[sr].prelim + a.nodes[sr].w - (mscl + a.nodes[cl].prelim);
        if (distance > 0.0f) {
            mscl += distance;
            size_t ih_index = ih_stack.back().second;
            move_subtree(a, t, i, ih_index, distance);
        }
        float sy = bottom(a, sr);
        float cy = bottom(a, cl);
        if (sy <= cy) {
            sr = next_right_contour(a, sr);
            if (sr != kNone) mssr += a.nodes[sr].modf;
        }
        if (sy >= cy) {
            cl = next_left_contour(a, cl);
            if (cl != kNone) mscl += a.nodes[cl].modf;
        }
    }
    if (sr == kNone && cl != kNone) set_left_thread(a, t, i, cl, mscl);
    else if (sr != kNone && cl == kNone) set_right_thread(a, t, i, sr, mssr);
}

void position_root(Arena& a, size_t t) {
    size_t cs = a.nodes[t].children.size();
    size_t first = a.nodes[t].children[0];
    size_t last = a.nodes[t].children[cs - 1];
    float prelim = (a.nodes[first].prelim + a.nodes[first].modf + a.nodes[last].modf + a.nodes[last].prelim + a.nodes[last].w) / 2.0f -
                   a.nodes[t].w / 2.0f;
    a.nodes[t].prelim = prelim;
}

void first_walk(Arena& a, size_t t) {
    size_t cs = a.nodes[t].children.size();
    if (cs == 0) {
        set_extremes(a, t);
        return;
    }
    size_t first = a.nodes[t].children[0];
    first_walk(a, first);
    std::vector<std::pair<float, size_t>> ih_stack;
    float low_y = bottom(a, a.nodes[first].el);
    ih_stack.emplace_back(low_y, 0);
    for (size_t i = 1; i < cs; ++i) {
        size_t child = a.nodes[t].children[i];
        first_walk(a, child);
        size_t er = a.nodes[child].er;
        float min_y = bottom(a, er);
        separate(a, t, i, ih_stack);
        // updateIYL: pop hidden then push (min_y, i)
        while (!ih_stack.empty()) {
            if (min_y >= ih_stack.back().first) ih_stack.pop_back();
            else break;
        }
        ih_stack.emplace_back(min_y, i);
    }
    position_root(a, t);
    set_extremes(a, t);
}

void add_child_spacing(Arena& a, size_t t) {
    size_t cs = a.nodes[t].children.size();
    float d = 0.0f, modsumdelta = 0.0f;
    for (size_t i = 0; i < cs; ++i) {
        size_t c = a.nodes[t].children[i];
        d += a.nodes[c].shift;
        modsumdelta += d + a.nodes[c].change;
        a.nodes[c].modf += modsumdelta;
    }
}

void second_walk(Arena& a, size_t t, float modsum) {
    modsum = modsum + a.nodes[t].modf;
    a.nodes[t].x = a.nodes[t].prelim + modsum;
    add_child_spacing(a, t);
    size_t cs = a.nodes[t].children.size();
    for (size_t i = 0; i < cs; ++i) second_walk(a, a.nodes[t].children[i], modsum);
}

void layout(Arena& a, size_t root) {
    first_walk(a, root);
    second_walk(a, root, 0.0f);
}

}  // namespace tidy_tree

enum class MindmapSide { Left, Right };

constexpr float kCircleAnchorGap = 15.0f;  // CIRCLE_ANCHOR_GAP

struct MindmapNodeInfo {
    size_t level = 0;
    std::optional<size_t> section;
    std::vector<std::string> children;
};
using InfoMap = std::unordered_map<std::string, MindmapNodeInfo>;
using NodeMap = std::map<std::string, NodeLayout>;
using SideMap = std::unordered_map<std::string, MindmapSide>;

// layout_horizontal_subtrees: run the tidy tree over a forest; returns (id, depth offset, vertical centre).
std::vector<std::tuple<std::string, float, float>> layout_horizontal_subtrees(const std::vector<std::string>& roots, const InfoMap& info_map,
                                                                              const NodeMap& nodes, float horizontal_gap, float vertical_gap) {
    tidy_tree::Arena arena;
    std::vector<std::string> id_lookup;

    struct Builder {
        tidy_tree::Arena& arena;
        std::vector<std::string>& id_lookup;
        const InfoMap& info_map;
        const NodeMap& nodes;
        float horizontal_gap, vertical_gap;
        size_t build(const std::string& node_id, float depth_y) {
            float w = 10.0f, h = 10.0f;
            if (auto it = nodes.find(node_id); it != nodes.end()) {
                w = it->second.width;
                h = it->second.height;
            }
            // Tidy-tree expects vertical trees; transpose so the rendered tree grows horizontally.
            float tt_w = h + vertical_gap;
            float tt_h = w + horizontal_gap;
            std::vector<std::string> children;
            if (auto it = info_map.find(node_id); it != info_map.end()) children = it->second.children;
            std::vector<size_t> child_indices;
            for (const std::string& child : children) child_indices.push_back(build(child, depth_y + tt_h));
            size_t idx = arena.alloc(tt_w, tt_h, depth_y, std::move(child_indices));
            id_lookup.push_back(node_id);
            return idx;
        }
    } builder{arena, id_lookup, info_map, nodes, horizontal_gap, vertical_gap};

    std::vector<size_t> child_indices;
    for (const std::string& root : roots) child_indices.push_back(builder.build(root, 0.0f));

    // virtual super-root with negligible footprint so several top-level subtrees are arranged together
    size_t virt = arena.alloc(0.0f, 0.0f, -1.0f, std::move(child_indices));
    id_lookup.push_back("");

    tidy_tree::layout(arena, virt);

    // Centre the tree using only the first-level subtree roots' span (Mermaid's combineAndPositionTrees).
    float first_level_min_y = std::numeric_limits<float>::max();
    float first_level_max_y = std::numeric_limits<float>::lowest();
    std::vector<std::tuple<std::string, float, float>> raw;
    for (size_t idx = 0; idx < id_lookup.size(); ++idx) {
        const std::string& id = id_lookup[idx];
        if (id.empty() || idx == virt) continue;
        const tidy_tree::TidyNode& n = arena.nodes[idx];
        float center_along = n.x + n.w / 2.0f;  // vertical centre after rotation
        float depth_offset = n.y;               // horizontal distance from the root edge
        float original_height = rmax(n.w - vertical_gap, 0.0f);
        if (depth_offset < 1e-3f) {
            first_level_min_y = rmin(first_level_min_y, center_along - original_height / 2.0f);
            first_level_max_y = rmax(first_level_max_y, center_along + original_height / 2.0f);
        }
        raw.emplace_back(id, depth_offset, center_along);
    }
    if (raw.empty()) return {};
    float mid = first_level_min_y == std::numeric_limits<float>::max() ? 0.0f : (first_level_min_y + first_level_max_y) / 2.0f;
    std::vector<std::tuple<std::string, float, float>> out;
    for (auto& [id, dx, cy] : raw) out.emplace_back(id, dx, cy - mid);
    return out;
}

// place_tidy_tree: returns the side of the root each non-root node landed on.
SideMap place_tidy_tree(const std::string& root_id, const InfoMap& info_map, NodeMap& nodes, float horizontal_gap, float vertical_gap, bool lr_only) {
    SideMap side_map;
    float root_cx = 0.0f, root_cy = 0.0f;
    float root_width = nodes.count(root_id) ? nodes.at(root_id).width : 0.0f;
    if (auto it = nodes.find(root_id); it != nodes.end()) {
        it->second.x = root_cx - it->second.width / 2.0f;
        it->second.y = root_cy - it->second.height / 2.0f;
    }
    auto info_it = info_map.find(root_id);
    if (info_it == info_map.end()) return side_map;
    const MindmapNodeInfo& info = info_it->second;

    std::vector<std::string> left_children, right_children;
    if (lr_only) {
        right_children = info.children;
    } else {
        for (size_t idx = 0; idx < info.children.size(); ++idx) (idx % 2 == 0 ? left_children : right_children).push_back(info.children[idx]);
    }

    float h_gap = rmax(horizontal_gap, 1.0f);
    float v_gap = rmax(vertical_gap, 1.0f);
    // Mermaid JS's `treeSpacing = rootNode.width / 2 + 30` adds clearance between the root and its first-level children.
    float root_extra_pad = 30.0f;

    if (!right_children.empty()) {
        auto positions = layout_horizontal_subtrees(right_children, info_map, nodes, h_gap, v_gap);
        float edge_x = root_cx + root_width / 2.0f + h_gap + root_extra_pad;
        for (auto& [id, dx, cy] : positions) {
            if (auto it = nodes.find(id); it != nodes.end()) {
                it->second.x = edge_x + dx;
                it->second.y = root_cy + cy - it->second.height / 2.0f;
            }
            side_map[id] = MindmapSide::Right;
        }
    }
    if (!left_children.empty()) {
        auto positions = layout_horizontal_subtrees(left_children, info_map, nodes, h_gap, v_gap);
        float edge_x = root_cx - root_width / 2.0f - h_gap - root_extra_pad;
        for (auto& [id, dx, cy] : positions) {
            if (auto it = nodes.find(id); it != nodes.end()) {
                it->second.x = edge_x - dx - it->second.width;
                it->second.y = root_cy + cy - it->second.height / 2.0f;
            }
            side_map[id] = MindmapSide::Left;
        }
    }
    return side_map;
}

// clip_to_rect: where a line from the node centre toward `outside` leaves the node (Mermaid's `intersection()`, quirks included).
Point clip_to_rect(const NodeLayout& node, Point center, Point outside) {
    if (node.width == 0.0f || node.height == 0.0f) return outside;
    if (node.shape == NodeShape::Circle || node.shape == NodeShape::DoubleCircle) {
        float dx = outside.first - center.first;
        float dy = outside.second - center.second;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-3f) return center;
        // anchor a little *outside* the circle so the curve visibly leaves the circumference
        float radius = rmin(node.width, node.height) / 2.0f + kCircleAnchorGap;
        return {center.first + dx / len * radius, center.second + dy / len * radius};
    }
    float x = center.first, y = center.second;
    Point inside = center;
    float w = node.width / 2.0f, h = node.height / 2.0f;
    float big_q = std::fabs(outside.second - inside.second);
    float big_r = std::fabs(outside.first - inside.first);

    if (std::fabs(y - outside.second) * w > std::fabs(x - outside.first) * h) {
        // top / bottom edge
        float q = inside.second < outside.second ? outside.second - h - y : y - h - outside.second;
        float r = big_q == 0.0f ? 0.0f : (big_r * q) / big_q;
        float res_x = inside.first < outside.first ? inside.first + r : inside.first - big_r + r;
        float res_y = inside.second < outside.second ? inside.second + big_q - q : inside.second - big_q + q;
        if (r == 0.0f) {
            res_x = outside.first;
            res_y = outside.second;
        }
        if (big_r == 0.0f) res_x = outside.first;
        if (big_q == 0.0f) res_y = outside.second;
        return {res_x, res_y};
    }
    // left / right edge
    float r = inside.first < outside.first ? outside.first - w - x : x - w - outside.first;
    float q = big_r == 0.0f ? 0.0f : (big_q * r) / big_r;
    float res_x = inside.first < outside.first ? inside.first + big_r - r : inside.first - big_r + r;
    float res_y = inside.second < outside.second ? inside.second + q : inside.second - q;
    if (r == 0.0f) {
        res_x = outside.first;
        res_y = outside.second;
    }
    if (big_r == 0.0f) res_x = outside.first;
    if (big_q == 0.0f) res_y = outside.second;
    return {res_x, res_y};
}

// tidy_tree_edge_points: the control polyline Mermaid feeds to curveBasis.
std::vector<Point> tidy_tree_edge_points(const NodeLayout& from_layout, const NodeLayout& to_layout, const SideMap& side_map, const std::string& from_id,
                                         const std::string& to_id) {
    Point from_center{from_layout.x + from_layout.width / 2.0f, from_layout.y + from_layout.height / 2.0f};
    Point to_center{to_layout.x + to_layout.width / 2.0f, to_layout.y + to_layout.height / 2.0f};
    float intersection_shift = 30.0f;

    bool from_is_root = !side_map.count(from_id);
    bool to_is_root = !side_map.count(to_id);
    MindmapSide side = MindmapSide::Right;
    if (auto it = side_map.find(to_id); it != side_map.end()) side = it->second;
    else if (auto it2 = side_map.find(from_id); it2 != side_map.end()) side = it2->second;
    float direction = side == MindmapSide::Right ? 1.0f : -1.0f;

    std::vector<Point> points;
    points.push_back(from_center);  // placeholder for the start; clipped once the middle points are known
    if (!from_is_root) points.push_back({from_center.first + direction * (from_layout.width / 2.0f + intersection_shift), from_center.second});
    if (!to_is_root) points.push_back({to_center.first - direction * (to_layout.width / 2.0f + intersection_shift), to_center.second});
    points.push_back(to_center);

    Point second = points[1];
    points[0] = clip_to_rect(from_layout, from_center, second);
    size_t last = points.size() - 1;
    Point second_last = points[last - 1];
    points[last] = clip_to_rect(to_layout, to_center, second_last);
    return points;
}

float mindmap_subtree_height(const std::string& node_id, const InfoMap& info, const NodeMap& nodes, std::unordered_map<std::string, float>& memo,
                             float spacing) {
    if (auto it = memo.find(node_id); it != memo.end()) return it->second;
    auto node_it = nodes.find(node_id);
    if (node_it == nodes.end()) return 0.0f;
    float height = node_it->second.height;
    if (auto info_it = info.find(node_id); info_it != info.end() && !info_it->second.children.empty()) {
        float total = 0.0f;
        for (const std::string& child : info_it->second.children) total += mindmap_subtree_height(child, info, nodes, memo, spacing);
        if (info_it->second.children.size() > 1) total += spacing * (static_cast<float>(info_it->second.children.size()) - 1.0f);
        height = rmax(height, total);
    }
    memo[node_id] = height;
    return height;
}

void assign_mindmap_positions(const std::string& node_id, float direction, float center_x, float center_y, const InfoMap& info, NodeMap& nodes,
                              const std::unordered_map<std::string, float>& subtree_heights, float horizontal_gap, float vertical_gap) {
    auto node_it = nodes.find(node_id);
    if (node_it == nodes.end()) return;
    node_it->second.x = center_x - node_it->second.width / 2.0f;
    node_it->second.y = center_y - node_it->second.height / 2.0f;
    float parent_width = node_it->second.width;
    auto info_it = info.find(node_id);
    if (info_it == info.end() || info_it->second.children.empty()) return;
    const auto& children = info_it->second.children;
    auto height_of = [&](const std::string& id) {
        auto it = subtree_heights.find(id);
        return it == subtree_heights.end() ? 0.0f : it->second;
    };
    float total = 0.0f;
    for (const std::string& child : children) total += height_of(child);
    if (children.size() > 1) total += vertical_gap * (static_cast<float>(children.size()) - 1.0f);
    float cursor = center_y - total / 2.0f;
    for (const std::string& child_id : children) {
        float child_height = height_of(child_id);
        float child_width = nodes.count(child_id) ? nodes.at(child_id).width : 0.0f;
        float child_center_y = cursor + child_height / 2.0f;
        float child_center_x = center_x + direction * (parent_width / 2.0f + child_width / 2.0f + horizontal_gap);
        assign_mindmap_positions(child_id, direction, child_center_x, child_center_y, info, nodes, subtree_heights, horizontal_gap, vertical_gap);
        cursor += child_height + vertical_gap;
    }
}

void place_mindmap_children(const std::vector<std::string>& children, float direction, Point parent_center, float parent_width, const InfoMap& info,
                            NodeMap& nodes, const std::unordered_map<std::string, float>& subtree_heights, float horizontal_gap, float vertical_gap) {
    if (children.empty()) return;
    auto height_of = [&](const std::string& id) {
        auto it = subtree_heights.find(id);
        return it == subtree_heights.end() ? 0.0f : it->second;
    };
    float total = 0.0f;
    for (const std::string& child : children) total += height_of(child);
    if (children.size() > 1) total += vertical_gap * (static_cast<float>(children.size()) - 1.0f);
    float cursor = parent_center.second - total / 2.0f;
    for (const std::string& child_id : children) {
        float child_height = height_of(child_id);
        float child_width = nodes.count(child_id) ? nodes.at(child_id).width : 0.0f;
        float child_center_y = cursor + child_height / 2.0f;
        float child_center_x = parent_center.first + direction * (parent_width / 2.0f + child_width / 2.0f + horizontal_gap);
        assign_mindmap_positions(child_id, direction, child_center_x, child_center_y, info, nodes, subtree_heights, horizontal_gap, vertical_gap);
        cursor += child_height + vertical_gap;
    }
}

// place_radial_layout: the default ("cose-bilkent") look: even sections right of the root, odd ones left.
SideMap place_radial_layout(const std::string& root_id, const InfoMap& info_map, NodeMap& nodes, float horizontal_gap, float vertical_gap) {
    std::unordered_map<std::string, float> subtree_heights;
    mindmap_subtree_height(root_id, info_map, nodes, subtree_heights, vertical_gap);
    Point root_center{0.0f, 0.0f};
    if (auto it = nodes.find(root_id); it != nodes.end()) {
        it->second.x = root_center.first - it->second.width / 2.0f;
        it->second.y = root_center.second - it->second.height / 2.0f;
    }
    std::vector<std::string> left_children, right_children;
    if (auto it = info_map.find(root_id); it != info_map.end()) {
        for (const std::string& child_id : it->second.children) {
            size_t section = 0;
            if (auto c = info_map.find(child_id); c != info_map.end() && c->second.section) section = *c->second.section;
            (section % 2 == 0 ? right_children : left_children).push_back(child_id);
        }
    }
    float root_width = nodes.count(root_id) ? nodes.at(root_id).width : 0.0f;
    place_mindmap_children(right_children, 1.0f, root_center, root_width, info_map, nodes, subtree_heights, horizontal_gap, vertical_gap);
    place_mindmap_children(left_children, -1.0f, root_center, root_width, info_map, nodes, subtree_heights, horizontal_gap, vertical_gap);
    return {};
}

std::string pick_palette_color(const std::vector<std::string>& values, size_t idx) {
    if (values.empty()) return "";
    return values[idx % values.size()];
}

// mindmap_node_size
std::pair<float, float> mindmap_node_size(NodeShape shape, const TextBlock& label, const LayoutConfig& config) {
    const MindmapConfig& mm = config.mindmap;
    switch (shape) {
    case NodeShape::MindmapDefault: return {label.width + mm.padding * 4.0f, label.height + mm.padding};
    case NodeShape::RoundRect: return {label.width + mm.rounded_padding * 2.0f, label.height + mm.rounded_padding * 2.0f};
    case NodeShape::Circle:
    case NodeShape::DoubleCircle: {
        float size = rmax(label.width, label.height) + mm.circle_padding * 2.0f;
        return {size, size};
    }
    case NodeShape::Hexagon: {
        float pad_x = mm.rect_padding * mm.hexagon_padding_multiplier;
        return {label.width + pad_x * 2.0f, label.height + mm.rect_padding * 2.0f};
    }
    default: return {label.width + mm.rect_padding * 2.0f, label.height + mm.rect_padding * 2.0f};  // Rectangle and the rest
    }
}

}  // namespace

Layout compute_mindmap_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    const MindmapConfig& mm = config.mindmap;
    // palette
    std::vector<std::string> section_fills = mm.section_colors.empty() ? std::vector<std::string>{"#ECECFF"} : mm.section_colors;
    std::vector<std::string> section_labels = mm.section_label_colors.empty() ? std::vector<std::string>{theme.primary_text_color} : mm.section_label_colors;
    std::vector<std::string> section_lines = mm.section_line_colors.empty() ? std::vector<std::string>{theme.primary_border_color} : mm.section_line_colors;
    std::string root_fill = mm.root_fill ? *mm.root_fill : theme.git_colors[0];
    std::string root_text = mm.root_text ? *mm.root_text : theme.git_branch_label_colors[0];

    Layout layout;
    layout.kind = graph.kind;
    NodeMap& nodes = layout.nodes;
    InfoMap info_map;

    for (const MindmapNode& node : graph.mindmap.nodes) {
        auto gn = graph.nodes.find(node.id);
        std::string label_text = gn != graph.nodes.end() ? gn->second.label : node.label;
        TextBlock label = measure_label(label_text, theme, config);
        label.width *= mm.text_width_scale;
        if (mm.use_max_width) label.width = rmin(label.width, mm.max_node_width);
        NodeShape shape = gn != graph.nodes.end() ? gn->second.shape : NodeShape::MindmapDefault;
        auto [width, height] = mindmap_node_size(shape, label, config);
        NodeStyle style = resolve_node_style_of(node.id, graph);
        if (node.level == 0) {
            if (!style.fill) style.fill = root_fill;
            if (!style.text_color) style.text_color = root_text;
        } else if (node.section) {
            size_t index = *node.section + 1;
            if (!style.fill) style.fill = pick_palette_color(section_fills, index);
            if (!style.text_color) style.text_color = pick_palette_color(section_labels, index);
            if (!style.line_color) style.line_color = pick_palette_color(section_lines, index);
        }
        if (!style.stroke) style.stroke = "none";
        if (!style.stroke_width) style.stroke_width = 0.0f;

        NodeLayout nl;
        nl.id = node.id;
        nl.width = width;
        nl.height = height;
        nl.label = std::move(label);
        nl.shape = shape;
        nl.style = std::move(style);
        if (auto link = graph.node_links.find(node.id); link != graph.node_links.end()) nl.link = link->second;
        nodes.insert_or_assign(node.id, std::move(nl));
        info_map[node.id] = {node.level, node.section, node.children};
    }

    std::optional<std::string> root_id = graph.mindmap.root_id;
    if (!root_id && !graph.mindmap.nodes.empty()) root_id = graph.mindmap.nodes.front().id;

    float horizontal_gap = mm.rank_spacing * mm.rank_spacing_multiplier;
    float vertical_gap = mm.node_spacing * mm.node_spacing_multiplier;
    size_t node_count = graph.mindmap.nodes.size();
    float density_scale = node_count >= 10 ? 0.7f : (node_count >= 6 ? 0.8f : 1.0f);
    horizontal_gap = rmax(horizontal_gap * density_scale, theme.font_size * 1.1f);
    vertical_gap = rmax(vertical_gap * density_scale, theme.font_size * 0.9f);

    std::string algorithm = to_ascii_lower(mm.layout_algorithm);
    SideMap side_map;
    bool curve_edges = false;
    if (root_id) {
        // Mermaid JS's tidy-tree spacing: BoundingBox(20, 40) for sibling / depth gaps and +30 root padding.
        float tidy_h_gap = 40.0f, tidy_v_gap = 20.0f;
        if (algorithm == "tidy-tree" || algorithm == "tidy_tree" || algorithm == "tidytree") {
            curve_edges = true;
            side_map = place_tidy_tree(*root_id, info_map, nodes, tidy_h_gap, tidy_v_gap, false);
        } else if (algorithm == "lr-tree" || algorithm == "lr_tree" || algorithm == "lrtree") {
            curve_edges = true;
            side_map = place_tidy_tree(*root_id, info_map, nodes, tidy_h_gap, tidy_v_gap, true);
        } else {
            side_map = place_radial_layout(*root_id, info_map, nodes, horizontal_gap, vertical_gap);
        }
    }

    std::vector<EdgeLayout> edges;
    for (const Edge& edge : graph.edges) {
        auto from_it = nodes.find(edge.from);
        auto to_it = nodes.find(edge.to);
        if (from_it == nodes.end() || to_it == nodes.end()) continue;
        const NodeLayout& from_layout = from_it->second;
        const NodeLayout& to_layout = to_it->second;
        Point from_center{from_layout.x + from_layout.width / 2.0f, from_layout.y + from_layout.height / 2.0f};
        Point to_center{to_layout.x + to_layout.width / 2.0f, to_layout.y + to_layout.height / 2.0f};
        EdgeStyleOverride override_style;
        if (mm.edge_color) {
            override_style.stroke = *mm.edge_color;
        } else if (auto child = info_map.find(edge.to); child != info_map.end() && child->second.section) {
            override_style.stroke = pick_palette_color(section_fills, *child->second.section + 1);
        }
        size_t parent_level = 0;
        if (auto parent = info_map.find(edge.from); parent != info_map.end()) parent_level = parent->second.level;
        size_t edge_depth = parent_level + 1;
        override_style.stroke_width = mm.edge_depth_base_width + mm.edge_depth_step * (static_cast<float>(edge_depth) + 1.0f);
        EdgeLayout el;
        el.from = edge.from;
        el.to = edge.to;
        el.points = curve_edges ? tidy_tree_edge_points(from_layout, to_layout, side_map, edge.from, edge.to) : std::vector<Point>{from_center, to_center};
        el.override_style = override_style;
        edges.push_back(std::move(el));
    }

    float min_x = std::numeric_limits<float>::max(), min_y = min_x;
    float max_x = std::numeric_limits<float>::lowest(), max_y = max_x;
    for (const auto& [id, node] : nodes) {
        min_x = rmin(min_x, node.x);
        min_y = rmin(min_y, node.y);
        max_x = rmax(max_x, node.x + node.width);
        max_y = rmax(max_y, node.y + node.height);
    }
    for (const EdgeLayout& edge : edges)
        for (const Point& p : edge.points) {
            min_x = rmin(min_x, p.first);
            min_y = rmin(min_y, p.second);
            max_x = rmax(max_x, p.first);
            max_y = rmax(max_y, p.second);
        }
    if (min_x == std::numeric_limits<float>::max() || min_y == std::numeric_limits<float>::max()) {
        min_x = 0.0f;
        min_y = 0.0f;
        max_x = 1.0f;
        max_y = 1.0f;
    }
    float pad = rmax(mm.padding, 8.0f);
    float shift_x = pad - min_x;
    float shift_y = pad - min_y;
    if (std::fabs(shift_x) > 1e-3f || std::fabs(shift_y) > 1e-3f) {
        for (auto& [id, node] : nodes) {
            node.x += shift_x;
            node.y += shift_y;
        }
        for (EdgeLayout& edge : edges)
            for (Point& p : edge.points) {
                p.first += shift_x;
                p.second += shift_y;
            }
        min_x += shift_x;
        min_y += shift_y;
        max_x += shift_x;
        max_y += shift_y;
    }
    layout.edges = std::move(edges);
    layout.width = rmax(max_x - min_x + pad * 2.0f, 1.0f);
    layout.height = rmax(max_y - min_y + pad * 2.0f, 1.0f);
    // compute_layout's final pass for graph-data layouts
    finalize_graph_bounds(layout, config);
    return layout;
}

}  // namespace mermaid
