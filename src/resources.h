// Files embedded in the exe at build time (cmake/resources.cmake): the default
// fonts, the built-in themes (everything in the Rust port) and the demo
// document. So calligrus.exe runs without an assets folder.
//
// Names are paths like "fonts/CascadiaCode-Regular.ttf", "themes/nord.toml", "demo.md".
#pragma once

#include <optional>
#include <string_view>
#include <vector>

struct Resource {
    std::string_view name;
    std::string_view data;  // the file's bytes (lives as long as the program)
};

// The embedded file called `name`, if any.
std::optional<std::string_view> resource(std::string_view name);

// All embedded files whose name starts with `prefix` (e.g. "themes/"), sorted by name.
std::vector<Resource> resources_with_prefix(std::string_view prefix);
