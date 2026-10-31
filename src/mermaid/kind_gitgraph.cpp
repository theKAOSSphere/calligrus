// Mermaid port: git graphs. Mirrors parser.rs parse_gitgraph_diagram, layout/gitgraph.rs and render.rs render_gitgraph.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

#include "kind_util.h"
#include "kinds.h"
#include "numfmt.h"
#include "parser_internal.h"
#include "strutil.h"
#include "svg_common.h"
#include "text.h"

namespace mermaid {

namespace {

// ---------------------------------------------------------------- parser

// Rust's `DefaultHasher::new()` is SipHash-1-3 with zero keys; `str::hash` feeds the bytes and then a 0xFF byte.
uint64_t rotl64(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

uint64_t hash_seed(const std::string& input) {
    std::string data = input;
    data.push_back(static_cast<char>(0xFF));
    uint64_t v0 = 0x736f6d6570736575ULL, v1 = 0x646f72616e646f6dULL, v2 = 0x6c7967656e657261ULL, v3 = 0x7465646279746573ULL;
    auto round = [&]() {
        v0 += v1; v1 = rotl64(v1, 13); v1 ^= v0; v0 = rotl64(v0, 32);
        v2 += v3; v3 = rotl64(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl64(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl64(v1, 17); v1 ^= v2; v2 = rotl64(v2, 32);
    };
    size_t len = data.size();
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t m = 0;
        for (int k = 0; k < 8; ++k) m |= static_cast<uint64_t>(static_cast<unsigned char>(data[i + static_cast<size_t>(k)])) << (8 * k);
        v3 ^= m;
        round();
        v0 ^= m;
    }
    uint64_t b = static_cast<uint64_t>(len) << 56;
    for (size_t k = 0; i + k < len; ++k) b |= static_cast<uint64_t>(static_cast<unsigned char>(data[i + k])) << (8 * k);
    v3 ^= b;
    round();
    v0 ^= b;
    v2 ^= 0xff;
    round(); round(); round();
    return v0 ^ v1 ^ v2 ^ v3;
}

// GitGraphIdRng: xorshift64* seeded from the input hash; ids look like "3-08d93d9".
struct IdRng {
    uint64_t state;
    explicit IdRng(uint64_t seed) : state(seed == 0 ? 0xA5A5A5A55A5A5A5AULL : seed) {}
    uint32_t next_u32() {
        uint64_t x = state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state = x;
        return static_cast<uint32_t>(x >> 32);
    }
    std::string next_hex(size_t len) {
        std::string out;
        for (size_t i = 0; i < len; ++i) out.push_back("0123456789abcdef"[next_u32() & 0xF]);
        return out;
    }
};

bool ascii_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// extract_gitgraph_attr: the value after `key:` (quoted or up to a blank / comma).
std::optional<std::string> extract_attr(const std::string& line, const std::string& key) {
    std::string needle = key + ":";
    size_t idx = line.find(needle);
    if (idx == std::string::npos) return std::nullopt;
    std::string rest(trim_start_view(std::string_view(line).substr(idx + needle.size())));
    if (rest.empty()) return std::nullopt;
    char first = rest[0];
    if (first == '"' || first == '\'') {
        rest = rest.substr(1);
        size_t end = rest.find(first);
        if (end != std::string::npos) return rest.substr(0, end);
        return rest;
    }
    size_t end = 0;
    while (end < rest.size() && !is_space(rest[end]) && rest[end] != ',') ++end;
    return rest.substr(0, end);
}

// extract_gitgraph_attrs: every `key:` occurrence (tags).
std::vector<std::string> extract_attrs(const std::string& line, const std::string& key) {
    std::vector<std::string> values;
    std::string lower = to_ascii_lower(line);
    std::string needle = key + ":";
    size_t start = 0;
    while (start <= lower.size()) {
        size_t found = lower.find(needle, start);
        if (found == std::string::npos) break;
        size_t idx = found + needle.size();
        while (idx < line.size() && ascii_space(line[idx])) ++idx;
        if (idx >= line.size()) break;
        char first = line[idx];
        std::string value;
        size_t next;
        if (first == '"' || first == '\'') {
            ++idx;
            size_t begin = idx;
            while (idx < line.size() && line[idx] != first) ++idx;
            value = line.substr(begin, idx - begin);
            next = std::min(idx + 1, line.size());
        } else {
            size_t begin = idx;
            while (idx < line.size() && !ascii_space(line[idx]) && line[idx] != ',') ++idx;
            value = line.substr(begin, idx - begin);
            next = idx;
        }
        values.push_back(value);
        start = next;
    }
    return values;
}

std::optional<GitGraphCommitType> extract_commit_type(const std::string& line) {
    auto raw = extract_attr(line, "type");
    if (!raw) return std::nullopt;
    std::string u = to_ascii_upper(*raw);
    if (u == "NORMAL") return GitGraphCommitType::Normal;
    if (u == "REVERSE") return GitGraphCommitType::Reverse;
    if (u == "HIGHLIGHT") return GitGraphCommitType::Highlight;
    return std::nullopt;
}

std::optional<Direction> parse_gitgraph_direction(const std::string& line) {
    std::string trimmed = trim(line);
    std::string lower = to_ascii_lower(trimmed);
    if (lower == "lr") return Direction::LeftRight;
    if (lower == "tb") return Direction::TopDown;
    if (lower == "bt") return Direction::BottomTop;
    if (starts_with(lower, "direction")) {
        std::string token = trim(lower.substr(9));
        if (token == "lr") return Direction::LeftRight;
        if (token == "tb") return Direction::TopDown;
        if (token == "bt") return Direction::BottomTop;
    }
    return std::nullopt;
}

std::string tail_from(const std::string& line, size_t n) { return trim(line.size() > n ? line.substr(n) : std::string()); }

}  // namespace

Graph parse_gitgraph_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::GitGraph;
    graph.direction = Direction::LeftRight;

    // branch name -> head commit id (none yet)
    std::unordered_map<std::string, std::optional<std::string>> branch_heads;
    std::unordered_map<std::string, size_t> branch_insertion;

    std::string main_branch = "main";
    graph.gitgraph.main_branch = main_branch;
    branch_heads[main_branch] = std::nullopt;
    branch_insertion[main_branch] = 0;
    graph.gitgraph.branches.push_back({main_branch, 0.0f, 0});

    std::string current_branch = main_branch;
    size_t commit_seq = 0;
    IdRng rng(hash_seed(input));

    auto head_of = [&](const std::string& name) -> std::optional<std::string> {
        auto it = branch_heads.find(name);
        return it == branch_heads.end() ? std::nullopt : it->second;
    };
    // the id of a commit line: `id:"x"` or "<seq>-<random hex>"
    auto commit_id = [&](const std::string& line) {
        if (auto v = extract_attr(line, "id")) return std::make_pair(*v, true);
        std::string hex = rng.next_hex(7);
        return std::make_pair(std::to_string(commit_seq) + "-" + hex, false);
    };

    for (const std::string& raw_line : pp::preprocess_input(input)) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        std::string lower = to_ascii_lower(line);
        if (starts_with(lower, "gitgraph")) continue;
        if (auto direction = parse_gitgraph_direction(line)) {
            graph.direction = *direction;
            continue;
        }
        if (starts_with(lower, "branch ")) {
            std::string name = tail_from(line, 7);
            if (!name.empty()) {
                std::optional<float> order;
                if (auto raw = extract_attr(line, "order")) {
                    float parsed;
                    if (parse_rust_f32(*raw, parsed)) order = parsed;
                }
                branch_heads[name] = head_of(current_branch);
                if (!branch_insertion.count(name)) {
                    size_t idx = graph.gitgraph.branches.size();
                    branch_insertion[name] = idx;
                    graph.gitgraph.branches.push_back({name, order, idx});
                }
                current_branch = name;
            }
            continue;
        }
        if (starts_with(lower, "checkout ") || starts_with(lower, "switch ")) {
            std::string name = starts_with(lower, "checkout ") ? tail_from(line, 9) : tail_from(line, 7);
            if (!name.empty()) {
                current_branch = name;
                branch_heads.emplace(current_branch, std::nullopt);
                if (!branch_insertion.count(name)) {
                    size_t idx = graph.gitgraph.branches.size();
                    branch_insertion[name] = idx;
                    graph.gitgraph.branches.push_back({name, std::nullopt, idx});
                }
            }
            continue;
        }
        if (starts_with(lower, "merge ")) {
            std::string from_branch = tail_from(line, 6);
            if (from_branch.empty()) continue;
            auto from_head = head_of(from_branch);
            auto current_head = head_of(current_branch);
            if (!from_head && !current_head) continue;
            std::vector<std::string> parents;
            if (current_head) parents.push_back(*current_head);
            if (from_head) parents.push_back(*from_head);
            auto [id, custom_id] = commit_id(line);
            GitGraphCommit commit;
            commit.id = id;
            commit.message = "merged branch " + from_branch + " into " + current_branch;
            commit.seq = commit_seq;
            commit.commit_type = GitGraphCommitType::Merge;
            commit.custom_type = extract_commit_type(line);
            commit.tags = extract_attrs(line, "tag");
            commit.parents = parents;
            commit.branch = current_branch;
            commit.custom_id = custom_id;
            ++commit_seq;
            graph.gitgraph.commits.push_back(std::move(commit));
            branch_heads[current_branch] = id;
            continue;
        }
        if (starts_with(lower, "commit")) {
            auto [id, custom_id] = commit_id(line);
            GitGraphCommit commit;
            commit.id = id;
            commit.message = extract_attr(line, "msg");
            commit.seq = commit_seq;
            commit.commit_type = extract_commit_type(line).value_or(GitGraphCommitType::Normal);
            commit.tags = extract_attrs(line, "tag");
            if (auto parent = head_of(current_branch)) commit.parents.push_back(*parent);
            commit.branch = current_branch;
            commit.custom_id = custom_id;
            ++commit_seq;
            graph.gitgraph.commits.push_back(std::move(commit));
            branch_heads[current_branch] = id;
            continue;
        }
    }
    return graph;
}

// ---------------------------------------------------------------- layout

namespace {

constexpr float kEpsilon = 1.1920929e-7f;  // f32::EPSILON
constexpr float kPi = 3.14159265358979323846f;

using BranchPos = std::unordered_map<std::string, std::tuple<float, size_t, float, float>>;  // pos, index, label w, h
using CommitPos = std::unordered_map<std::string, Point>;

float default_branch_order(size_t index) {
    if (index == 0) return 0.0f;
    float denom = 1.0f;
    size_t value = index;
    while (value > 0) {
        denom *= 10.0f;
        value /= 10;
    }
    return static_cast<float>(index) / denom;
}

std::pair<float, float> measure_gitgraph_text(const std::string& text, float font_size, float line_height, float width_scale) {
    std::vector<std::string> lines = split_lines(text);
    float max_width = 0.0f;
    for (const auto& line : lines) max_width = rmax(max_width, measure_text_width(line, font_size));
    return {max_width * width_scale, static_cast<float>(lines.size()) * font_size * line_height};
}

const GitGraphCommit* commit_by_id(const std::vector<GitGraphCommit>& commits, const std::string& id) {
    for (const auto& c : commits)
        if (c.id == id) return &c;
    return nullptr;
}

std::optional<std::string> find_closest_parent(const std::vector<std::string>& parents, const CommitPos& commit_pos, Direction dir) {
    std::optional<std::string> chosen;
    float target = dir == Direction::BottomTop ? std::numeric_limits<float>::infinity() : 0.0f;
    for (const auto& parent : parents) {
        auto it = commit_pos.find(parent);
        if (it == commit_pos.end()) continue;
        float pos = (dir == Direction::TopDown || dir == Direction::BottomTop) ? it->second.second : it->second.first;
        bool accept = dir == Direction::BottomTop ? pos <= target : pos >= target;
        if (accept) {
            target = pos;
            chosen = parent;
        }
    }
    return chosen;
}

std::optional<std::string> find_closest_parent_bt(const std::vector<std::string>& parents, const CommitPos& commit_pos) {
    std::optional<std::string> chosen;
    float max_pos = std::numeric_limits<float>::infinity();
    for (const auto& parent : parents) {
        auto it = commit_pos.find(parent);
        if (it != commit_pos.end() && it->second.second <= max_pos) {
            max_pos = it->second.second;
            chosen = parent;
        }
    }
    return chosen;
}

float branch_axis(const BranchPos& branch_pos, const std::string& branch) {
    auto it = branch_pos.find(branch);
    return it == branch_pos.end() ? 0.0f : std::get<0>(it->second);
}

float calculate_commit_position(const GitGraphCommit& commit, float commit_step, const CommitPos& commit_pos) {
    float closest_parent_pos = 0.0f;
    if (auto parent = find_closest_parent(commit.parents, commit_pos, Direction::BottomTop)) {
        auto it = commit_pos.find(*parent);
        if (it != commit_pos.end()) closest_parent_pos = it->second.second;
    }
    return closest_parent_pos + commit_step;
}

void set_commit_position(const GitGraphCommit& commit, float cur_pos, float layout_offset, const BranchPos& branch_pos, CommitPos& commit_pos) {
    commit_pos[commit.id] = {branch_axis(branch_pos, commit.branch), cur_pos + layout_offset};
}

void set_root_position(const GitGraphCommit& commit, float cur_pos, float default_pos, const BranchPos& branch_pos, CommitPos& commit_pos) {
    commit_pos[commit.id] = {branch_axis(branch_pos, commit.branch), cur_pos + default_pos};
}

void set_parallel_bt_pos(const std::vector<const GitGraphCommit*>& commits, float default_pos, float commit_step, float layout_offset,
                         const BranchPos& branch_pos, CommitPos& commit_pos) {
    float cur_pos = default_pos, max_position = default_pos;
    std::vector<const GitGraphCommit*> roots;
    for (const GitGraphCommit* commit : commits) {
        if (!commit->parents.empty()) {
            cur_pos = calculate_commit_position(*commit, commit_step, commit_pos);
            max_position = rmax(max_position, cur_pos);
        } else {
            roots.push_back(commit);
        }
        set_commit_position(*commit, cur_pos, layout_offset, branch_pos, commit_pos);
    }
    cur_pos = max_position;
    for (const GitGraphCommit* commit : roots) set_root_position(*commit, cur_pos, default_pos, branch_pos, commit_pos);
    for (const GitGraphCommit* commit : commits) {
        if (commit->parents.empty()) continue;
        auto closest = find_closest_parent_bt(commit->parents, commit_pos);
        if (!closest) continue;
        auto it = commit_pos.find(*closest);
        if (it == commit_pos.end()) continue;
        cur_pos = it->second.second - commit_step;
        if (cur_pos <= max_position) max_position = cur_pos;
        commit_pos[commit->id] = {branch_axis(branch_pos, commit->branch), cur_pos - layout_offset};
    }
}

float calculate_position(const GitGraphCommit& commit, Direction dir, float default_pos, float commit_step, const CommitPos& commit_pos) {
    auto current_of = [&]() {
        auto it = commit_pos.find(commit.id);
        return it == commit_pos.end() ? Point{0.0f, 0.0f} : it->second;
    };
    if (!commit.parents.empty()) {
        if (auto parent = find_closest_parent(commit.parents, commit_pos, dir)) {
            auto it = commit_pos.find(*parent);
            Point parent_pos = it == commit_pos.end() ? Point{0.0f, 0.0f} : it->second;
            if (dir == Direction::TopDown) return parent_pos.second + commit_step;
            if (dir == Direction::BottomTop) return current_of().second - commit_step;
            return parent_pos.first + commit_step;
        }
    } else if (dir == Direction::TopDown) {
        return default_pos;
    } else if (dir == Direction::BottomTop) {
        return current_of().second - commit_step;
    } else {
        return 0.0f;
    }
    return 0.0f;
}

// (x, y, pos_with_offset)
std::tuple<float, float, float> commit_position(const GitGraphCommit& commit, float pos, bool is_parallel, Direction dir, float layout_offset,
                                                const BranchPos& branch_pos) {
    float pos_with_offset = (dir == Direction::BottomTop && is_parallel) ? pos : pos + layout_offset;
    float axis = branch_axis(branch_pos, commit.branch);
    if (dir == Direction::TopDown || dir == Direction::BottomTop) return {axis, pos_with_offset, pos_with_offset};
    return {pos_with_offset, axis, pos_with_offset};
}

void update_bounds_point(float& min_x, float& min_y, float& max_x, float& max_y, float px, float py) {
    min_x = rmin(min_x, px);
    min_y = rmin(min_y, py);
    max_x = rmax(max_x, px);
    max_y = rmax(max_y, py);
}

Point apply_transform_point(float x, float y, const std::optional<GitGraphTransform>& transform) {
    if (!transform) return {x, y};
    float px = x + transform->translate_x;
    float py = y + transform->translate_y;
    if (std::fabs(transform->rotate_deg) > kEpsilon) {
        float angle = transform->rotate_deg * (kPi / 180.0f);  // f32::to_radians
        float c = std::cos(angle), s = std::sin(angle);
        float dx = px - transform->rotate_cx;
        float dy = py - transform->rotate_cy;
        px = transform->rotate_cx + dx * c - dy * s;
        py = transform->rotate_cy + dx * s + dy * c;
    }
    return {px, py};
}

void update_bounds_points(float& min_x, float& min_y, float& max_x, float& max_y, const std::vector<Point>& points, const std::optional<GitGraphTransform>& t) {
    for (const auto& [x, y] : points) {
        auto [px, py] = apply_transform_point(x, y, t);
        update_bounds_point(min_x, min_y, max_x, max_y, px, py);
    }
}

void update_bounds_rect(float& min_x, float& min_y, float& max_x, float& max_y, float x, float y, float w, float h, const std::optional<GitGraphTransform>& t) {
    update_bounds_points(min_x, min_y, max_x, max_y, {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}, t);
}

bool should_reroute_arrow(Direction dir, const GitGraphCommit& a, const GitGraphCommit& b, Point p1, Point p2, const std::vector<GitGraphCommit>& commits) {
    bool b_is_furthest = (dir == Direction::TopDown || dir == Direction::BottomTop) ? p1.first < p2.first : p1.second < p2.second;
    const std::string& branch = b_is_furthest ? b.branch : a.branch;
    for (const auto& c : commits)
        if (c.seq > a.seq && c.seq < b.seq && c.branch == branch) return true;
    return false;
}

float find_lane(float y1, float y2, std::vector<float>& lanes, const GitGraphConfig& config, size_t depth) {
    float candidate = y1 + std::fabs(y2 - y1) / 2.0f;
    if (depth > config.lane_max_depth) return candidate;
    bool ok = true;
    for (float lane : lanes)
        if (!(std::fabs(lane - candidate) >= config.lane_spacing)) ok = false;
    if (ok) {
        lanes.push_back(candidate);
        return candidate;
    }
    float diff = std::fabs(y1 - y2);
    return find_lane(y1, y2 - diff / 5.0f, lanes, config, depth + 1);
}

std::string gitgraph_arrow_path(Direction dir, const GitGraphCommit& a, const GitGraphCommit& b, Point p1, Point p2, const std::vector<GitGraphCommit>& commits,
                                const GitGraphConfig& config, std::vector<float>& lanes) {
    using svg::F;
    auto [p1x, p1y] = p1;
    auto [p2x, p2y] = p2;
    bool reroute = should_reroute_arrow(dir, a, b, p1, p2, commits);
    float radius = reroute ? config.arrow_reroute_radius : config.arrow_radius;
    float offset = radius;
    std::string arc = "A " + F(radius) + " " + F(radius) + ", 0, 0, 0,";
    std::string arc2 = "A " + F(radius) + " " + F(radius) + ", 0, 0, 1,";
    std::string P1X = F(p1x), P1Y = F(p1y), P2X = F(p2x), P2Y = F(p2y);
    std::string def;

    if (reroute) {
        float line_y = p1y < p2y ? find_lane(p1y, p2y, lanes, config, 0) : find_lane(p2y, p1y, lanes, config, 0);
        float line_x = p1x < p2x ? find_lane(p1x, p2x, lanes, config, 0) : find_lane(p2x, p1x, lanes, config, 0);
        std::string LX = F(line_x), LY = F(line_y);
        if (dir == Direction::TopDown) {
            if (p1x < p2x)
                return "M " + P1X + " " + P1Y + " L " + F(line_x - radius) + " " + P1Y + " " + arc2 + " " + LX + " " + F(p1y + offset) + " L " + LX + " " +
                       F(p2y - radius) + " " + arc + " " + F(line_x + offset) + " " + P2Y + " L " + P2X + " " + P2Y;
            return "M " + P1X + " " + P1Y + " L " + F(line_x + radius) + " " + P1Y + " " + arc + " " + LX + " " + F(p1y + offset) + " L " + LX + " " +
                   F(p2y - radius) + " " + arc2 + " " + F(line_x - offset) + " " + P2Y + " L " + P2X + " " + P2Y;
        }
        if (dir == Direction::BottomTop) {
            if (p1x < p2x)
                return "M " + P1X + " " + P1Y + " L " + F(line_x - radius) + " " + P1Y + " " + arc + " " + LX + " " + F(p1y - offset) + " L " + LX + " " +
                       F(p2y + radius) + " " + arc2 + " " + F(line_x + offset) + " " + P2Y + " L " + P2X + " " + P2Y;
            return "M " + P1X + " " + P1Y + " L " + F(line_x + radius) + " " + P1Y + " " + arc2 + " " + LX + " " + F(p1y - offset) + " L " + LX + " " +
                   F(p2y + radius) + " " + arc + " " + F(line_x - offset) + " " + P2Y + " L " + P2X + " " + P2Y;
        }
        if (p1y < p2y)
            return "M " + P1X + " " + P1Y + " L " + P1X + " " + F(line_y - radius) + " " + arc + " " + F(p1x + offset) + " " + LY + " L " + F(p2x - radius) +
                   " " + LY + " " + arc2 + " " + P2X + " " + F(line_y + offset) + " L " + P2X + " " + P2Y;
        return "M " + P1X + " " + P1Y + " L " + P1X + " " + F(line_y + radius) + " " + arc2 + " " + F(p1x + offset) + " " + LY + " L " + F(p2x - radius) + " " +
               LY + " " + arc + " " + P2X + " " + F(line_y - offset) + " L " + P2X + " " + P2Y;
    }

    bool merge_from_other = b.commit_type == GitGraphCommitType::Merge && a.id != (b.parents.empty() ? std::string() : b.parents.front());
    if (dir == Direction::TopDown) {
        if (p1x < p2x) {
            if (merge_from_other)
                def = "M " + P1X + " " + P1Y + " L " + P1X + " " + F(p2y - radius) + " " + arc + " " + F(p1x + offset) + " " + P2Y + " L " + P2X + " " + P2Y;
            else
                def = "M " + P1X + " " + P1Y + " L " + F(p2x - radius) + " " + P1Y + " " + arc2 + " " + P2X + " " + F(p1y + offset) + " L " + P2X + " " + P2Y;
        }
        if (p1x > p2x) {
            if (merge_from_other)
                def = "M " + P1X + " " + P1Y + " L " + P1X + " " + F(p2y - radius) + " " + arc2 + " " + F(p1x - offset) + " " + P2Y + " L " + P2X + " " + P2Y;
            else
                def = "M " + P1X + " " + P1Y + " L " + F(p2x + radius) + " " + P1Y + " " + arc + " " + P2X + " " + F(p1y + offset) + " L " + P2X + " " + P2Y;
        }
        if (std::fabs(p1x - p2x) < kEpsilon) def = "M " + P1X + " " + P1Y + " L " + P2X + " " + P2Y;
    } else if (dir == Direction::BottomTop) {
        if (p1x < p2x) {
            if (merge_from_other)
                def = "M " + P1X + " " + P1Y + " L " + P1X + " " + F(p2y + radius) + " " + arc2 + " " + F(p1x + offset) + " " + P2Y + " L " + P2X + " " + P2Y;
            else
                def = "M " + P1X + " " + P1Y + " L " + F(p2x - radius) + " " + P1Y + " " + arc + " " + P2X + " " + F(p1y - offset) + " L " + P2X + " " + P2Y;
        }
        if (p1x > p2x) {
            if (merge_from_other)
                def = "M " + P1X + " " + P1Y + " L " + P1X + " " + F(p2y + radius) + " " + arc + " " + F(p1x - offset) + " " + P2Y + " L " + P2X + " " + P2Y;
            else
                def = "M " + P1X + " " + P1Y + " L " + F(p2x - radius) + " " + P1Y + " " + arc + " " + P2X + " " + F(p1y - offset) + " L " + P2X + " " + P2Y;
        }
        if (std::fabs(p1x - p2x) < kEpsilon) def = "M " + P1X + " " + P1Y + " L " + P2X + " " + P2Y;
    } else {
        if (p1y < p2y) {
            if (merge_from_other)
                def = "M " + P1X + " " + P1Y + " L " + F(p2x - radius) + " " + P1Y + " " + arc2 + " " + P2X + " " + F(p1y + offset) + " L " + P2X + " " + P2Y;
            else
                def = "M " + P1X + " " + P1Y + " L " + P1X + " " + F(p2y - radius) + " " + arc + " " + F(p1x + offset) + " " + P2Y + " L " + P2X + " " + P2Y;
        }
        if (p1y > p2y) {
            if (merge_from_other)
                def = "M " + P1X + " " + P1Y + " L " + F(p2x - radius) + " " + P1Y + " " + arc + " " + P2X + " " + F(p1y - offset) + " L " + P2X + " " + P2Y;
            else
                def = "M " + P1X + " " + P1Y + " L " + P1X + " " + F(p2y + radius) + " " + arc2 + " " + F(p1x + offset) + " " + P2Y + " L " + P2X + " " + P2Y;
        }
        if (std::fabs(p1y - p2y) < kEpsilon) def = "M " + P1X + " " + P1Y + " L " + P2X + " " + P2Y;
    }
    if (def.empty()) def = "M " + P1X + " " + P1Y + " L " + P2X + " " + P2Y;
    return def;
}

}  // namespace

Layout compute_gitgraph_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    const GitGraphConfig& gg = config.gitgraph;
    bool is_vertical = graph.direction == Direction::TopDown || graph.direction == Direction::BottomTop;
    bool is_bottom_top = graph.direction == Direction::BottomTop;
    std::vector<GitGraphBranch> branches = graph.gitgraph.branches;
    if (branches.empty()) branches.push_back({gg.main_branch_name, gg.main_branch_order, 0});

    std::vector<std::pair<GitGraphBranch, float>> branch_entries;
    for (auto& b : branches) {
        float order = b.order ? *b.order : default_branch_order(b.insertion_index);
        branch_entries.emplace_back(b, order);
    }
    std::stable_sort(branch_entries.begin(), branch_entries.end(), [](const auto& a, const auto& b) { return a.second < b.second; });

    BranchPos branch_pos;
    std::vector<GitGraphBranchLayout> branch_layouts;
    float pos = 0.0f;
    for (size_t index = 0; index < branch_entries.size(); ++index) {
        const GitGraphBranch& branch = branch_entries[index].first;
        float measure_font_size = gg.branch_label_font_size > 0.0f ? gg.branch_label_font_size : theme.font_size;
        auto [label_width, label_height] = measure_gitgraph_text(branch.name, measure_font_size, gg.branch_label_line_height, gg.text_width_scale);
        float spacing_rotate_extra = gg.rotate_commit_label ? gg.branch_spacing_rotate_extra : 0.0f;
        float label_rotate_extra = gg.rotate_commit_label ? gg.branch_label_rotate_extra : 0.0f;
        float bg_width = label_width + gg.branch_label_bg_pad_x;
        float bg_height = label_height + gg.branch_label_bg_pad_y;
        float bg_final_x, bg_final_y, text_x, text_y;
        if (is_vertical) {
            float bg_x = pos - label_width / 2.0f - gg.branch_label_tb_bg_offset_x;
            text_x = pos - label_width / 2.0f - gg.branch_label_tb_text_offset_x;
            float base_y = is_bottom_top ? 0.0f : gg.branch_label_tb_offset_y;
            bg_final_x = bg_x;
            bg_final_y = base_y;
            text_y = base_y;
        } else {
            float bg_x = -label_width - gg.branch_label_bg_offset_x - label_rotate_extra;
            float bg_y = -label_height / 2.0f + gg.branch_label_bg_offset_y;
            bg_final_x = bg_x + gg.branch_label_translate_x;
            bg_final_y = bg_y + (pos - label_height / 2.0f);
            text_x = -label_width - gg.branch_label_text_offset_x - label_rotate_extra;
            text_y = pos - label_height / 2.0f + gg.branch_label_text_offset_y;
        }
        GitGraphBranchLayout bl;
        bl.name = branch.name;
        bl.index = index;
        bl.pos = pos;
        bl.label = {bg_final_x, bg_final_y, bg_width, bg_height, text_x, text_y, label_width, label_height};
        branch_layouts.push_back(bl);
        branch_pos[branch.name] = {pos, index, label_width, label_height};
        float width_extra = is_vertical ? label_width / 2.0f : 0.0f;
        pos += gg.branch_spacing + spacing_rotate_extra + width_extra;
    }

    std::vector<GitGraphCommit> commits = graph.gitgraph.commits;
    std::stable_sort(commits.begin(), commits.end(), [](const auto& a, const auto& b) { return a.seq < b.seq; });
    std::vector<GitGraphCommitLayout> commit_layouts;
    CommitPos commit_pos;
    pos = is_vertical ? gg.default_pos : 0.0f;
    float max_pos = pos;
    bool is_parallel = gg.parallel_commits;
    std::vector<const GitGraphCommit*> commit_order;
    for (const auto& c : commits) commit_order.push_back(&c);
    if (is_bottom_top && is_parallel) set_parallel_bt_pos(commit_order, gg.default_pos, gg.commit_step, gg.layout_offset, branch_pos, commit_pos);
    if (is_bottom_top) std::reverse(commit_order.begin(), commit_order.end());

    for (const GitGraphCommit* cp : commit_order) {
        const GitGraphCommit& commit = *cp;
        if (is_parallel) pos = calculate_position(commit, graph.direction, gg.default_pos, gg.commit_step, commit_pos);
        auto [x, y, pos_with_offset] = commit_position(commit, pos, is_parallel, graph.direction, gg.layout_offset, branch_pos);
        float axis_pos = pos;
        size_t branch_index = 0;
        if (auto it = branch_pos.find(commit.branch); it != branch_pos.end()) branch_index = std::get<1>(it->second);

        bool show_label = gg.show_commit_label && commit.commit_type != GitGraphCommitType::CherryPick &&
                          (commit.commit_type != GitGraphCommitType::Merge || commit.custom_id);
        std::optional<GitGraphCommitLabelLayout> label;
        if (show_label) {
            auto [label_width, label_height] = measure_gitgraph_text(commit.id, gg.commit_label_font_size, gg.commit_label_line_height, gg.text_width_scale);
            float text_x, text_y, bg_x, bg_y;
            std::optional<GitGraphTransform> transform;
            if (is_vertical) {
                text_x = x - (label_width + gg.commit_label_tb_text_extra);
                text_y = y + label_height + gg.commit_label_tb_text_offset_y;
                bg_x = x - (label_width + gg.commit_label_tb_bg_extra);
                bg_y = y + gg.commit_label_tb_bg_offset_y;
                if (gg.rotate_commit_label) transform = GitGraphTransform{0.0f, 0.0f, gg.commit_label_rotate_angle, x, y};
            } else {
                text_x = pos_with_offset - label_width / 2.0f;
                text_y = y + gg.commit_label_offset_y;
                bg_x = pos_with_offset - label_width / 2.0f - gg.commit_label_padding;
                bg_y = y + gg.commit_label_bg_offset_y;
                if (gg.rotate_commit_label) {
                    float rotate_x = gg.commit_label_rotate_translate_x_base -
                                     (label_width + gg.commit_label_rotate_translate_x_width_offset) * gg.commit_label_rotate_translate_x_scale;
                    float rotate_y = gg.commit_label_rotate_translate_y_base + label_width * gg.commit_label_rotate_translate_y_scale;
                    transform = GitGraphTransform{rotate_x, rotate_y, gg.commit_label_rotate_angle, axis_pos, y};
                }
            }
            float bg_width = label_width + 2.0f * gg.commit_label_padding;
            float bg_height = label_height + 2.0f * gg.commit_label_padding;
            label = GitGraphCommitLabelLayout{commit.id, text_x, text_y, bg_x, bg_y, bg_width, bg_height, transform};
        }

        std::vector<GitGraphTagLayout> tag_layouts;
        if (!commit.tags.empty()) {
            float max_width = 0.0f, max_height = 0.0f;
            struct TagDef { std::string text; float w, offset; };
            std::vector<TagDef> tag_defs;
            float y_offset = 0.0f;
            for (auto it = commit.tags.rbegin(); it != commit.tags.rend(); ++it) {
                auto [w, h] = measure_gitgraph_text(*it, gg.tag_label_font_size, gg.tag_label_line_height, gg.text_width_scale);
                max_width = rmax(max_width, w);
                max_height = rmax(max_height, h);
                tag_defs.push_back({*it, w, y_offset});
                y_offset += gg.tag_spacing_y;
            }
            float half_h = max_height / 2.0f;
            for (const TagDef& def : tag_defs) {
                GitGraphTagLayout tl;
                tl.text = def.text;
                float px = gg.tag_padding_x, py = gg.tag_padding_y;
                if (is_vertical) {
                    float y_origin = axis_pos + def.offset;
                    float text_translate_delta = gg.tag_text_rotate_translate - gg.tag_rotate_translate;
                    tl.text_x = x + gg.tag_text_offset_x_tb + text_translate_delta;
                    tl.text_y = y_origin + gg.tag_text_offset_y_tb + text_translate_delta;
                    tl.points = {{x, y_origin + py},
                                 {x, y_origin - py},
                                 {x + gg.layout_offset, y_origin - half_h - py},
                                 {x + gg.layout_offset + max_width + px, y_origin - half_h - py},
                                 {x + gg.layout_offset + max_width + px, y_origin + half_h + py},
                                 {x + gg.layout_offset, y_origin + half_h + py}};
                    tl.hole_x = x + px / 2.0f;
                    tl.hole_y = y_origin;
                    tl.transform = GitGraphTransform{gg.tag_rotate_translate, gg.tag_rotate_translate, gg.tag_rotate_angle, x, axis_pos};
                } else {
                    tl.text_x = pos_with_offset - def.w / 2.0f;
                    tl.text_y = y - gg.tag_text_offset_y - def.offset;
                    float ly = y - gg.tag_polygon_offset_y - def.offset;
                    tl.points = {{axis_pos - max_width / 2.0f - px / 2.0f, ly + py},
                                 {axis_pos - max_width / 2.0f - px / 2.0f, ly - py},
                                 {pos_with_offset - max_width / 2.0f - px, ly - half_h - py},
                                 {pos_with_offset + max_width / 2.0f + px, ly - half_h - py},
                                 {pos_with_offset + max_width / 2.0f + px, ly + half_h + py},
                                 {pos_with_offset - max_width / 2.0f - px, ly + half_h + py}};
                    tl.hole_x = axis_pos - max_width / 2.0f + px / 2.0f;
                    tl.hole_y = ly;
                }
                tag_layouts.push_back(std::move(tl));
            }
        }

        GitGraphCommitLayout cl;
        cl.id = commit.id;
        cl.seq = commit.seq;
        cl.branch_index = branch_index;
        cl.x = x;
        cl.y = y;
        cl.axis_pos = axis_pos;
        cl.commit_type = commit.commit_type;
        cl.custom_type = commit.custom_type;
        cl.tags = std::move(tag_layouts);
        cl.label = std::move(label);
        commit_layouts.push_back(std::move(cl));

        commit_pos[commit.id] = is_vertical ? Point{x, pos_with_offset} : Point{pos_with_offset, y};
        pos = (is_bottom_top && is_parallel) ? pos + gg.commit_step : pos + gg.commit_step + gg.layout_offset;
        if (pos > max_pos) max_pos = pos;
    }
    if (is_bottom_top)
        for (auto& branch : branch_layouts) {
            branch.label.bg_y = max_pos + gg.branch_label_bt_offset_y;
            branch.label.text_y = max_pos + gg.branch_label_bt_offset_y;
        }

    std::vector<GitGraphArrowLayout> arrows;
    std::vector<float> lanes;
    for (const GitGraphCommit& commit : graph.gitgraph.commits) {
        for (const std::string& parent : commit.parents) {
            auto p1 = commit_pos.find(parent), p2 = commit_pos.find(commit.id);
            if (p1 == commit_pos.end() || p2 == commit_pos.end()) continue;
            const GitGraphCommit* a = commit_by_id(graph.gitgraph.commits, parent);
            const GitGraphCommit* b = commit_by_id(graph.gitgraph.commits, commit.id);
            if (!a || !b) continue;
            std::string path = gitgraph_arrow_path(graph.direction, *a, *b, p1->second, p2->second, graph.gitgraph.commits, gg, lanes);
            size_t color_index = 0;
            if (auto it = branch_pos.find(b->branch); it != branch_pos.end()) color_index = std::get<1>(it->second);
            if (b->commit_type == GitGraphCommitType::Merge && a->id != (b->parents.empty() ? std::string() : b->parents.front()))
                if (auto it = branch_pos.find(a->branch); it != branch_pos.end()) color_index = std::get<1>(it->second);
            arrows.push_back({path, color_index});
        }
    }

    float min_x = std::numeric_limits<float>::infinity(), min_y = min_x;
    float max_x = -std::numeric_limits<float>::infinity(), max_y = max_x;
    for (const auto& branch : branch_layouts) {
        float x1, y1, x2, y2;
        if (is_vertical) {
            float start = is_bottom_top ? max_pos : gg.default_pos;
            float end = is_bottom_top ? gg.default_pos : max_pos;
            x1 = branch.pos; y1 = start; x2 = branch.pos; y2 = end;
        } else {
            x1 = 0.0f; y1 = branch.pos; x2 = max_pos; y2 = branch.pos;
        }
        min_x = rmin(min_x, rmin(x1, x2));
        min_y = rmin(min_y, rmin(y1, y2));
        max_x = rmax(max_x, rmax(x1, x2));
        max_y = rmax(max_y, rmax(y1, y2));
        update_bounds_rect(min_x, min_y, max_x, max_y, branch.label.bg_x, branch.label.bg_y, branch.label.bg_width, branch.label.bg_height, std::nullopt);
    }
    for (const auto& commit : commit_layouts) {
        float radius = commit.commit_type == GitGraphCommitType::Merge ? gg.merge_radius_outer : gg.commit_radius;
        update_bounds_rect(min_x, min_y, max_x, max_y, commit.x - radius, commit.y - radius, radius * 2.0f, radius * 2.0f, std::nullopt);
        if (commit.label)
            update_bounds_rect(min_x, min_y, max_x, max_y, commit.label->bg_x, commit.label->bg_y, commit.label->bg_width, commit.label->bg_height,
                               commit.label->transform);
        for (const auto& tag : commit.tags) update_bounds_points(min_x, min_y, max_x, max_y, tag.points, tag.transform);
    }
    if (!std::isfinite(min_x)) {
        min_x = 0.0f; min_y = 0.0f; max_x = 1.0f; max_y = 1.0f;
    }
    min_x -= gg.diagram_padding;
    min_y -= gg.diagram_padding;
    max_x += gg.diagram_padding;
    max_y += gg.diagram_padding;
    float width = rmax(max_x - min_x, 1.0f);
    float height = rmax(max_y - min_y, 1.0f);

    Layout layout;
    layout.kind = graph.kind;
    NodeLayout metrics;
    metrics.id = "__gitgraph_metrics_content";
    metrics.x = gg.diagram_padding;
    metrics.y = gg.diagram_padding;
    metrics.width = rmax(width - gg.diagram_padding * 2.0f, 1.0f);
    metrics.height = rmax(height - gg.diagram_padding * 2.0f, 1.0f);
    metrics.label.lines = {""};
    layout.nodes.emplace(metrics.id, std::move(metrics));
    GitGraphLayout gl;
    gl.branches = std::move(branch_layouts);
    gl.commits = std::move(commit_layouts);
    gl.arrows = std::move(arrows);
    gl.width = width;
    gl.height = height;
    gl.offset_x = -min_x;
    gl.offset_y = -min_y;
    gl.max_pos = max_pos;
    gl.direction = graph.direction;
    layout.gitgraph = std::move(gl);
    layout.width = width;
    layout.height = height;
    return layout;
}

// ---------------------------------------------------------------- writer

namespace {

std::string multiline_text(float x, float y, const std::string& text, const std::string& font_family, float font_size, float line_height, const std::string& color) {
    using svg::F;
    using svg::F2;
    std::vector<std::string> lines;
    size_t pos = 0;
    while (true) {
        size_t j = text.find('\n', pos);
        lines.push_back(text.substr(pos, j == std::string::npos ? std::string::npos : j - pos));
        if (j == std::string::npos) break;
        pos = j + 1;
    }
    float start_y = y + font_size;
    std::string out = "<text x=\"" + F2(x) + "\" y=\"" + F2(start_y) + "\" text-anchor=\"start\" font-family=\"" + svg::normalize_font_family(font_family) +
                      "\" font-size=\"" + F(font_size) + "\" fill=\"" + svg::escape_xml(color) + "\">";
    float line_height_px = font_size * line_height;
    for (size_t idx = 0; idx < lines.size(); ++idx) {
        float dy = idx == 0 ? 0.0f : line_height_px;
        out += "<tspan x=\"" + F2(x) + "\" dy=\"" + F2(dy) + "\">" + svg::escape_xml(lines[idx]) + "</tspan>";
    }
    return out + "</text>";
}

std::string transform_group(const GitGraphTransform& t, const std::string& inner) {
    using svg::F2;
    return "<g transform=\"translate(" + F2(t.translate_x) + ", " + F2(t.translate_y) + ") rotate(" + F2(t.rotate_deg) + ", " + F2(t.rotate_cx) + ", " +
           F2(t.rotate_cy) + ")\">" + inner + "</g>";
}

}  // namespace

std::string render_gitgraph_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config) {
    using svg::escape_xml;
    using svg::F;
    using svg::F2;
    const GitGraphConfig& gg = config.gitgraph;
    const GitGraphLayout& g = *layout.gitgraph;
    float width = rmax(layout.width, 1.0f), height = rmax(layout.height, 1.0f);
    std::string out = svg::document_start(width, height, -g.offset_x, -g.offset_y, g.width, g.height, gg.use_max_width, false, theme);
    out += "<g>";

    if (gg.show_branches) {
        for (const auto& branch : g.branches) {
            float x1, y1, x2, y2;
            switch (g.direction) {
            case Direction::TopDown: x1 = branch.pos; y1 = gg.default_pos; x2 = branch.pos; y2 = g.max_pos; break;
            case Direction::BottomTop: x1 = branch.pos; y1 = g.max_pos; x2 = branch.pos; y2 = gg.default_pos; break;
            default: x1 = 0.0f; y1 = branch.pos; x2 = g.max_pos; y2 = branch.pos; break;
            }
            out += "<line x1=\"" + F2(x1) + "\" y1=\"" + F2(y1) + "\" x2=\"" + F2(x2) + "\" y2=\"" + F2(y2) + "\" stroke=\"" + escape_xml(theme.line_color) +
                   "\" stroke-width=\"" + F(gg.branch_stroke_width) + "\" stroke-dasharray=\"" + escape_xml(gg.branch_dasharray) + "\"/>";
            size_t color_idx = branch.index % theme.git_colors.size();
            const std::string& label_color = theme.git_colors[color_idx];
            const std::string& text_color = theme.git_branch_label_colors[color_idx];
            const auto& label = branch.label;
            out += "<rect x=\"" + F2(label.bg_x) + "\" y=\"" + F2(label.bg_y) + "\" width=\"" + F2(label.bg_width) + "\" height=\"" + F2(label.bg_height) +
                   "\" rx=\"" + F2(gg.branch_label_corner_radius) + "\" ry=\"" + F2(gg.branch_label_corner_radius) + "\" fill=\"" + escape_xml(label_color) +
                   "\" stroke=\"none\"/>";
            float branch_font_size = gg.branch_label_font_size > 0.0f ? gg.branch_label_font_size : theme.font_size;
            out += multiline_text(label.text_x, label.text_y, branch.name, theme.font_family, branch_font_size, gg.branch_label_line_height, text_color);
        }
    }

    if (!g.arrows.empty()) {
        out += "<g class=\"commit-arrows\">";
        for (const auto& arrow : g.arrows) {
            const std::string& stroke = theme.git_colors[arrow.color_index % theme.git_colors.size()];
            out += "<path d=\"" + escape_xml(arrow.path) + "\" fill=\"none\" stroke=\"" + escape_xml(stroke) + "\" stroke-width=\"" + F(gg.arrow_stroke_width) +
                   "\" stroke-linecap=\"round\"/>";
        }
        out += "</g>";
    }

    out += "<g class=\"commit-bullets\">";
    for (const auto& commit : g.commits) {
        size_t color_idx = commit.branch_index % theme.git_colors.size();
        const std::string& color = theme.git_colors[color_idx];
        const std::string& highlight_color = theme.git_inv_colors[color_idx];
        GitGraphCommitType symbol = commit.custom_type ? *commit.custom_type : commit.commit_type;
        auto circle = [&](float cx, float cy, float r, const std::string& fill, const std::string& stroke) {
            return "<circle cx=\"" + F2(cx) + "\" cy=\"" + F2(cy) + "\" r=\"" + F2(r) + "\" fill=\"" + escape_xml(fill) + "\" stroke=\"" + escape_xml(stroke) + "\"/>";
        };
        if (symbol == GitGraphCommitType::Highlight) {
            float outer = gg.highlight_outer_size, inner = gg.highlight_inner_size;
            out += "<rect x=\"" + F2(commit.x - outer / 2.0f) + "\" y=\"" + F2(commit.y - outer / 2.0f) + "\" width=\"" + F2(outer) + "\" height=\"" + F2(outer) +
                   "\" fill=\"" + escape_xml(highlight_color) + "\" stroke=\"" + escape_xml(highlight_color) + "\"/>";
            out += "<rect x=\"" + F2(commit.x - inner / 2.0f) + "\" y=\"" + F2(commit.y - inner / 2.0f) + "\" width=\"" + F2(inner) + "\" height=\"" + F2(inner) +
                   "\" fill=\"" + escape_xml(theme.primary_color) + "\" stroke=\"" + escape_xml(theme.primary_color) + "\"/>";
        } else if (symbol == GitGraphCommitType::CherryPick) {
            out += circle(commit.x, commit.y, gg.commit_radius, color, color);
            std::string accent = escape_xml(gg.cherry_pick_accent_color);
            out += "<circle cx=\"" + F2(commit.x - gg.cherry_pick_dot_offset_x) + "\" cy=\"" + F2(commit.y + gg.cherry_pick_dot_offset_y) + "\" r=\"" +
                   F2(gg.cherry_pick_dot_radius) + "\" fill=\"" + accent + "\" stroke=\"none\"/>";
            out += "<circle cx=\"" + F2(commit.x + gg.cherry_pick_dot_offset_x) + "\" cy=\"" + F2(commit.y + gg.cherry_pick_dot_offset_y) + "\" r=\"" +
                   F2(gg.cherry_pick_dot_radius) + "\" fill=\"" + accent + "\" stroke=\"none\"/>";
            out += "<line x1=\"" + F2(commit.x + gg.cherry_pick_dot_offset_x) + "\" y1=\"" + F2(commit.y + gg.cherry_pick_stem_start_offset_y) + "\" x2=\"" +
                   F2(commit.x) + "\" y2=\"" + F2(commit.y + gg.cherry_pick_stem_end_offset_y) + "\" stroke=\"" + accent + "\" stroke-width=\"" +
                   F(gg.cherry_pick_stem_stroke_width) + "\"/>";
            out += "<line x1=\"" + F2(commit.x - gg.cherry_pick_dot_offset_x) + "\" y1=\"" + F2(commit.y + gg.cherry_pick_stem_start_offset_y) + "\" x2=\"" +
                   F2(commit.x) + "\" y2=\"" + F2(commit.y + gg.cherry_pick_stem_end_offset_y) + "\" stroke=\"" + accent + "\" stroke-width=\"" +
                   F(gg.cherry_pick_stem_stroke_width) + "\"/>";
        } else {
            float radius = commit.commit_type == GitGraphCommitType::Merge ? gg.merge_radius_outer : gg.commit_radius;
            out += circle(commit.x, commit.y, radius, color, color);
            if (symbol == GitGraphCommitType::Merge) out += circle(commit.x, commit.y, gg.merge_radius_inner, theme.primary_color, theme.primary_color);
            if (symbol == GitGraphCommitType::Reverse) {
                float size = gg.reverse_cross_size;
                out += "<path d=\"M " + F2(commit.x - size) + "," + F2(commit.y - size) + " L " + F2(commit.x + size) + "," + F2(commit.y + size) + " M " +
                       F2(commit.x - size) + "," + F2(commit.y + size) + " L " + F2(commit.x + size) + "," + F2(commit.y - size) + "\" stroke=\"" +
                       escape_xml(theme.primary_color) + "\" stroke-width=\"" + F(gg.reverse_stroke_width) + "\" fill=\"none\"/>";
            }
        }
    }
    out += "</g>";

    out += "<g class=\"commit-labels\">";
    std::string font = svg::normalize_font_family(theme.font_family);
    for (const auto& commit : g.commits) {
        if (commit.label) {
            const auto& label = *commit.label;
            std::string inner = "<rect x=\"" + F2(label.bg_x) + "\" y=\"" + F2(label.bg_y) + "\" width=\"" + F2(label.bg_width) + "\" height=\"" +
                                F2(label.bg_height) + "\" fill=\"" + escape_xml(theme.git_commit_label_background) + "\" opacity=\"" +
                                F(gg.commit_label_bg_opacity) + "\"/>";
            inner += "<text x=\"" + F2(label.text_x) + "\" y=\"" + F2(label.text_y) + "\" text-anchor=\"start\" font-family=\"" + font + "\" font-size=\"" +
                     F(gg.commit_label_font_size) + "\" fill=\"" + escape_xml(theme.git_commit_label_color) + "\">" + escape_xml(label.text) + "</text>";
            out += label.transform ? transform_group(*label.transform, inner) : inner;
        }
        for (const auto& tag : commit.tags) {
            std::string points;
            for (size_t i = 0; i < tag.points.size(); ++i) points += (i ? " " : "") + F2(tag.points[i].first) + "," + F2(tag.points[i].second);
            std::string inner = "<polygon points=\"" + points + "\" fill=\"" + escape_xml(theme.git_tag_label_background) + "\" stroke=\"" +
                                escape_xml(theme.git_tag_label_border) + "\"/>";
            inner += "<circle cx=\"" + F2(tag.hole_x) + "\" cy=\"" + F2(tag.hole_y) + "\" r=\"" + F2(gg.tag_hole_radius) + "\" fill=\"" +
                     escape_xml(theme.text_color) + "\"/>";
            inner += "<text x=\"" + F2(tag.text_x) + "\" y=\"" + F2(tag.text_y) + "\" text-anchor=\"start\" font-family=\"" + font + "\" font-size=\"" +
                     F(gg.tag_label_font_size) + "\" fill=\"" + escape_xml(theme.git_tag_label_color) + "\">" + escape_xml(tag.text) + "</text>";
            out += tag.transform ? transform_group(*tag.transform, inner) : inner;
        }
    }
    out += "</g></g></svg>";
    return out;
}

}  // namespace mermaid
