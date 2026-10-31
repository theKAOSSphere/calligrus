// Mermaid port: the entry point the app uses (math.cpp diagram_get).
//
//   native_init(fonts)            once: load the bundled IBM Plex Sans faces
//   native_render(source, scale)  Mermaid text -> white-ink bitmap, or nullopt when the port
//                                 cannot draw this diagram (unsupported feature, syntax error):
//                                 the caller then falls back to the Rust renderer.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "svg_raster.h"

namespace mermaid {

struct NativeFonts {
    std::vector<uint8_t> regular, bold, italic;  // IBMPlexSans-Regular / -Bold / -Italic .ttf bytes
};

// False if the regular face cannot be read (native stays disabled). Safe to call repeatedly:
// only the first successful call counts.
bool native_init(NativeFonts fonts);

std::optional<InkImage> native_render(const std::string& source, float scale);

// Why the last native_render returned nullopt (empty if it did not).
const std::string& native_last_reason();

}  // namespace mermaid
