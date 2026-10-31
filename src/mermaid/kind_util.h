// Mermaid port: small helpers shared by the per-kind files of the "Stage 3b" kinds (radar, kanban, ...).
#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include "ir.h"
#include "layout_internal.h"
#include "layout_types.h"
#include "text.h"

namespace mermaid {

// Rust f32::max / f32::min (a NaN operand is ignored).
inline float rmax(float a, float b) { return std::isnan(a) ? b : (std::isnan(b) ? a : (a > b ? a : b)); }
inline float rmin(float a, float b) { return std::isnan(a) ? b : (std::isnan(b) ? a : (a < b ? a : b)); }

// layout/mod.rs build_node_layout: a NodeLayout with the standard defaults (origin, not hidden).
inline NodeLayout build_node_layout(const Node& node, TextBlock label, float width, float height, NodeStyle style,
                                    const Graph& graph) {
    NodeLayout nl;
    nl.id = node.id;
    nl.width = width;
    nl.height = height;
    nl.label = std::move(label);
    nl.shape = node.shape;
    nl.style = std::move(style);
    auto link = graph.node_links.find(node.id);
    if (link != graph.node_links.end()) nl.link = link->second;
    return nl;
}

}  // namespace mermaid
