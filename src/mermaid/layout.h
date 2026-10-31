// Mermaid port: flowchart layout. Mirrors layout/mod.rs (compute_flowchart_layout and friends),
// layout/flowchart/{policy,manual_layout,analysis}.rs.
#pragma once

#include "config.h"
#include "ir.h"
#include "layout_types.h"

namespace mermaid {

enum class LayoutStage {
    NodesOnly,  // stop after node placement (before routing); for stage-by-stage tests
    Full,
};

// Throws Unsupported for features that are not ported yet.
Layout compute_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config,
                      LayoutStage stage = LayoutStage::Full);

}  // namespace mermaid
