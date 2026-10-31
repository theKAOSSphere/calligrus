// Mermaid port: JSON5 syntax check (json5.cpp).
#pragma once

#include <string>

namespace mermaid {

// True when `text` is a valid JSON5 document (a superset of JSON).
bool json5_is_valid(const std::string& text);

}  // namespace mermaid
