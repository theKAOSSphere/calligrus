// Mermaid port of layout/ranking.rs (see ranking.h). Function names match the Rust file.
#include "ranking.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <unordered_set>

namespace mermaid {

namespace {

constexpr size_t kMax = std::numeric_limits<size_t>::max();

size_t lookup_or(const OrderMap& m, const std::string& id, size_t dflt) {
    auto it = m.find(id);
    return it == m.end() ? dflt : it->second;
}

using Adj = std::unordered_map<std::string, std::vector<std::string>>;

// ---- order_rank_nodes ----

float median_position(const std::string& node_id, const Adj& neighbors, const OrderMap& positions,
                      const OrderMap& current_positions) {
    auto cur = [&]() {
        auto it = current_positions.find(node_id);
        return static_cast<float>(it == current_positions.end() ? 0 : it->second);
    };
    auto it = neighbors.find(node_id);
    if (it == neighbors.end()) return cur();
    std::vector<float> values;
    for (const auto& nb : it->second) {
        auto p = positions.find(nb);
        if (p != positions.end()) values.push_back(static_cast<float>(p->second));
    }
    if (values.empty()) return cur();
    std::sort(values.begin(), values.end());
    size_t mid = values.size() / 2;
    if (values.size() % 2 == 1) return values[mid];
    return (values[mid - 1] + values[mid]) * 0.5f;
}

std::pair<size_t, size_t> pair_crossings(const std::string& a, const std::string& b, const Adj& neighbors,
                                         const OrderMap& positions) {
    auto collect = [&](const std::string& id) {
        std::vector<size_t> v;
        auto it = neighbors.find(id);
        if (it != neighbors.end())
            for (const auto& n : it->second) {
                auto p = positions.find(n);
                if (p != positions.end()) v.push_back(p->second);
            }
        return v;
    };
    std::vector<size_t> a_pos = collect(a), b_pos = collect(b);
    if (a_pos.empty() || b_pos.empty()) return {0, 0};
    std::sort(a_pos.begin(), a_pos.end());
    std::sort(b_pos.begin(), b_pos.end());
    size_t ab = 0, ba = 0;
    for (size_t pa : a_pos)
        for (size_t pb : b_pos) {
            if (pa > pb) ++ab;
            else if (pb > pa) ++ba;
        }
    return {ab, ba};
}

void transpose_bucket(std::vector<std::string>& bucket, const Adj& neighbors, const OrderMap& positions,
                      const OrderMap& node_order) {
    if (bucket.size() <= 1) return;
    bool improved = true;
    while (improved) {
        improved = false;
        for (size_t i = 0; i + 1 < bucket.size(); ++i) {
            const std::string& a = bucket[i];
            const std::string& b = bucket[i + 1];
            auto [ab, ba] = pair_crossings(a, b, neighbors, positions);
            bool should_swap;
            if (ba < ab) should_swap = true;
            else if (ba > ab) should_swap = false;
            else should_swap = lookup_or(node_order, a, kMax) > lookup_or(node_order, b, kMax);
            if (should_swap) {
                std::swap(bucket[i], bucket[i + 1]);
                improved = true;
            }
        }
    }
}

// ---- strongly connected components / topological order (compute_ranks_subset) ----

void dfs_finish_order(const std::string& node_id, const Adj& adj, std::unordered_set<std::string>& visited,
                      std::vector<std::string>& finish_order) {
    if (!visited.insert(node_id).second) return;
    auto it = adj.find(node_id);
    if (it != adj.end())
        for (const auto& next : it->second) dfs_finish_order(next, adj, visited, finish_order);
    finish_order.push_back(node_id);
}

std::vector<std::vector<std::string>> strongly_connected_components(const std::vector<std::string>& node_ids,
                                                                    const std::vector<Edge>& edges) {
    Adj adj, rev;
    for (const auto& e : edges) {
        adj[e.from].push_back(e.to);
        rev[e.to].push_back(e.from);
    }
    std::unordered_set<std::string> visited;
    std::vector<std::string> finish_order;
    for (const auto& id : node_ids) dfs_finish_order(id, adj, visited, finish_order);

    std::unordered_set<std::string> assigned;
    std::vector<std::vector<std::string>> components;
    while (!finish_order.empty()) {
        std::string node_id = finish_order.back();
        finish_order.pop_back();
        if (!assigned.insert(node_id).second) continue;
        std::vector<std::string> component;
        std::vector<std::string> stack{node_id};
        while (!stack.empty()) {
            std::string current = stack.back();
            stack.pop_back();
            component.push_back(current);
            auto it = rev.find(current);
            if (it != rev.end())
                for (const auto& prev : it->second)
                    if (assigned.insert(prev).second) stack.push_back(prev);
        }
        components.push_back(std::move(component));
    }
    return components;
}

// Kahn's algorithm with a min-heap on (order key, item): ties resolve by the item's own order.
template <class T, class Map, class KeyFn>
std::vector<T> stable_topology_order(const std::vector<T>& items, const Map& adj, const Map& rev, KeyFn order_key) {
    std::map<T, size_t> indeg;
    for (const auto& item : items) {
        auto it = rev.find(item);
        indeg[item] = it == rev.end() ? 0 : it->second.size();
    }
    using Entry = std::pair<size_t, T>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> ready;
    for (const auto& item : items)
        if (indeg[item] == 0) ready.push({order_key(item), item});

    std::vector<T> ordered;
    std::set<T> processed;
    while (true) {
        while (!ready.empty()) {
            Entry top = ready.top();
            ready.pop();
            T item = top.second;
            if (processed.count(item)) continue;
            ordered.push_back(item);
            processed.insert(item);
            auto it = adj.find(item);
            if (it != adj.end())
                for (const auto& next : it->second) {
                    if (processed.count(next)) continue;
                    auto d = indeg.find(next);
                    if (d != indeg.end()) {
                        d->second = d->second > 0 ? d->second - 1 : 0;
                        if (d->second == 0) ready.push({order_key(next), next});
                    }
                }
        }
        if (processed.size() >= items.size()) break;
        // Cycle among the rest: restart from the unprocessed item with the smallest key.
        bool have = false;
        size_t best_key = 0;
        T best{};
        for (const auto& item : items)
            if (!processed.count(item)) {
                size_t key = order_key(item);
                if (!have || key < best_key) {
                    have = true;
                    best_key = key;
                    best = item;
                }
            }
        if (!have) break;
        ready.push({best_key, best});
    }
    return ordered;
}

std::unordered_map<std::string, size_t> layered_ranks_from_order(const std::vector<std::string>& order,
                                                                 const Adj& adj) {
    std::unordered_map<std::string, size_t> order_index;
    for (size_t i = 0; i < order.size(); ++i) order_index[order[i]] = i;
    std::unordered_map<std::string, size_t> ranks;
    for (const auto& node : order) {
        size_t rank = 0;
        if (auto it = ranks.find(node); it != ranks.end()) rank = it->second;
        ranks.emplace(node, rank);
        auto a = adj.find(node);
        if (a != adj.end()) {
            size_t from_idx = order_index.count(node) ? order_index[node] : 0;
            for (const auto& next : a->second) {
                size_t to_idx = order_index.count(next) ? order_index[next] : from_idx;
                if (to_idx <= from_idx) continue;
                size_t& entry = ranks.try_emplace(next, 0).first->second;
                entry = std::max(entry, rank + 1);
            }
        }
    }
    return ranks;
}

}  // namespace

std::vector<Edge> rank_edges_for_manual_layout(const Graph& graph, const std::vector<std::string>& layout_node_ids,
                                               const std::vector<Edge>& layout_edges) {
    if (graph.kind != DiagramKind::Flowchart || layout_edges.size() < 3) return layout_edges;
    std::vector<Edge> primary;
    for (const auto& e : layout_edges)
        if (e.style != EdgeStyle::Dotted) primary.push_back(e);
    if (primary.empty()) return layout_edges;
    std::unordered_set<std::string> covered;
    for (const auto& e : primary) {
        covered.insert(e.from);
        covered.insert(e.to);
    }
    size_t min_covered = (layout_node_ids.size() + 1) / 2;
    if (covered.size() >= min_covered) return primary;
    return layout_edges;
}

void order_rank_nodes(std::vector<std::vector<std::string>>& rank_nodes, const std::vector<Edge>& edges,
                      const OrderMap& node_order, size_t passes) {
    if (rank_nodes.size() <= 1) return;
    Adj incoming, outgoing;
    for (const auto& e : edges) {
        outgoing[e.from].push_back(e.to);
        incoming[e.to].push_back(e.from);
    }
    OrderMap positions;
    auto update_positions = [&]() {
        positions.clear();
        for (const auto& bucket : rank_nodes)
            for (size_t i = 0; i < bucket.size(); ++i) positions[bucket[i]] = i;
    };
    update_positions();

    auto sort_bucket = [&](std::vector<std::string>& bucket, const Adj& neighbors) {
        OrderMap current_positions;
        for (size_t i = 0; i < bucket.size(); ++i) current_positions[bucket[i]] = i;
        // Precompute the medians (the Rust comparator recomputes them; the values are identical).
        std::unordered_map<std::string, float> score;
        for (const auto& id : bucket) score[id] = median_position(id, neighbors, positions, current_positions);
        std::stable_sort(bucket.begin(), bucket.end(), [&](const std::string& a, const std::string& b) {
            float sa = score[a], sb = score[b];
            if (sa < sb) return true;
            if (sa > sb) return false;
            size_t pa = lookup_or(current_positions, a, 0), pb = lookup_or(current_positions, b, 0);
            if (pa != pb) return pa < pb;
            return lookup_or(node_order, a, kMax) < lookup_or(node_order, b, kMax);
        });
    };

    passes = std::max<size_t>(passes, 1);
    for (size_t pass = 0; pass < passes; ++pass) {
        for (size_t rank = 1; rank < rank_nodes.size(); ++rank) {
            if (rank_nodes[rank].size() <= 1) continue;
            sort_bucket(rank_nodes[rank], incoming);
            transpose_bucket(rank_nodes[rank], incoming, positions, node_order);
            update_positions();
        }
        for (size_t rank = rank_nodes.size() - 1; rank-- > 0;) {
            if (rank_nodes[rank].size() <= 1) continue;
            sort_bucket(rank_nodes[rank], outgoing);
            transpose_bucket(rank_nodes[rank], outgoing, positions, node_order);
            update_positions();
        }
    }
}

RankMap compute_ranks_subset(const std::vector<std::string>& node_ids, const std::vector<Edge>& edges,
                             const OrderMap& node_order) {
    std::unordered_set<std::string> set(node_ids.begin(), node_ids.end());
    std::vector<Edge> subset_edges;
    for (const auto& e : edges)
        if (set.count(e.from) && set.count(e.to)) subset_edges.push_back(e);

    OrderMap fallback_order;
    for (size_t i = 0; i < node_ids.size(); ++i) fallback_order[node_ids[i]] = i;
    auto order_key = [&](const std::string& id) -> size_t {
        auto it = node_order.find(id);
        if (it != node_order.end()) return it->second;
        auto f = fallback_order.find(id);
        return f != fallback_order.end() ? f->second : kMax;
    };

    auto components = strongly_connected_components(node_ids, subset_edges);
    std::unordered_map<std::string, size_t> node_to_component;
    for (size_t c = 0; c < components.size(); ++c)
        for (const auto& id : components[c]) node_to_component[id] = c;

    std::map<size_t, std::vector<size_t>> comp_adj, comp_rev;
    for (const auto& e : subset_edges) {
        auto f = node_to_component.find(e.from);
        auto t = node_to_component.find(e.to);
        if (f == node_to_component.end() || t == node_to_component.end()) continue;
        if (f->second == t->second) continue;
        comp_adj[f->second].push_back(t->second);
        comp_rev[t->second].push_back(f->second);
    }
    for (auto* m : {&comp_adj, &comp_rev})
        for (auto& [k, v] : *m) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }

    std::vector<size_t> comp_ids(components.size());
    for (size_t i = 0; i < comp_ids.size(); ++i) comp_ids[i] = i;
    std::vector<size_t> component_order = stable_topology_order<size_t>(
        comp_ids, comp_adj, comp_rev, [&](const size_t& comp_idx) {
            size_t best = kMax;
            for (const auto& id : components[comp_idx]) best = std::min(best, order_key(id));
            return best;
        });

    std::vector<std::unordered_map<std::string, size_t>> local_ranks(components.size());
    for (size_t c = 0; c < components.size(); ++c) {
        const auto& component = components[c];
        std::unordered_set<std::string> cset(component.begin(), component.end());
        // stable_component_node_order
        Adj cadj, crev;
        for (const auto& e : subset_edges)
            if (cset.count(e.from) && cset.count(e.to)) {
                cadj[e.from].push_back(e.to);
                crev[e.to].push_back(e.from);
            }
        std::vector<std::string> internal_order = stable_topology_order<std::string>(
            component, cadj, crev, [&](const std::string& id) { return order_key(id); });
        Adj internal_adj;
        for (const auto& e : subset_edges)
            if (cset.count(e.from) && cset.count(e.to)) internal_adj[e.from].push_back(e.to);
        local_ranks[c] = layered_ranks_from_order(internal_order, internal_adj);
    }

    std::map<size_t, std::vector<std::pair<size_t, long long>>> weighted_comp_edges;
    for (const auto& e : subset_edges) {
        auto f = node_to_component.find(e.from);
        auto t = node_to_component.find(e.to);
        if (f == node_to_component.end() || t == node_to_component.end()) continue;
        if (f->second == t->second) continue;
        auto fl = local_ranks[f->second].find(e.from);
        auto tl = local_ranks[t->second].find(e.to);
        long long from_local = fl == local_ranks[f->second].end() ? 0 : static_cast<long long>(fl->second);
        long long to_local = tl == local_ranks[t->second].end() ? 0 : static_cast<long long>(tl->second);
        weighted_comp_edges[f->second].push_back({t->second, from_local + 1 - to_local});
    }

    std::vector<long long> component_start(components.size(), 0);
    for (size_t comp_idx : component_order) {
        long long start = component_start[comp_idx];
        auto it = weighted_comp_edges.find(comp_idx);
        if (it != weighted_comp_edges.end())
            for (auto [next, weight] : it->second)
                component_start[next] = std::max(component_start[next], start + weight);
    }

    RankMap ranks;
    for (size_t c = 0; c < components.size(); ++c) {
        size_t base_rank = static_cast<size_t>(std::max<long long>(component_start[c], 0));
        for (const auto& id : components[c]) {
            auto it = local_ranks[c].find(id);
            size_t local = it == local_ranks[c].end() ? 0 : it->second;
            ranks[id] = base_rank + local;
        }
    }
    return ranks;
}

std::unordered_map<std::string, size_t> scc_component_ids(const std::vector<std::string>& node_ids,
                                                          const std::vector<Edge>& edges) {
    std::vector<Edge> sub;
    std::unordered_set<std::string> set(node_ids.begin(), node_ids.end());
    for (const auto& e : edges)
        if (set.count(e.from) && set.count(e.to)) sub.push_back(e);
    auto components = strongly_connected_components(node_ids, sub);
    std::unordered_map<std::string, size_t> ids;
    for (size_t c = 0; c < components.size(); ++c)
        for (const auto& id : components[c]) ids[id] = c;
    return ids;
}

}  // namespace mermaid
