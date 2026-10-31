// Mermaid port: flowchart parser. Mirrors mermaid-rs-renderer 0.3.1 src/parser.rs
// (parse_mermaid / parse_flowchart and their helpers) plus the flowchart-relevant part of
// src/validator.rs.
#pragma once

#include <string>

#include "ir.h"

namespace mermaid {

// Parse Mermaid source. Throws ParseError for input the Rust crate rejects too, and
// Unsupported for diagram types / directives this port does not handle yet.
Graph parse_mermaid(const std::string& input);

}  // namespace mermaid
