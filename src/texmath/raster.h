// Turn a laid-out `MBox` into pixels. C++ port of the Rust port
//
// Glyph outlines come straight from the font and are filled with the
// tiny-skia port in canvas.cpp. The bitmap is white-on-transparent so the UI
// can tint it with whatever text colour is current (light/dark themes for free).
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "layout.h"

namespace texmath {

struct Raster {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;  // premultiplied RGBA8, white ink
    // Pixel position of the box origin (left edge, baseline) inside the bitmap.
    float origin_x = 0.0f, origin_y = 0.0f;
};

// Rasterize at `ppem` pixels per em; nullopt if the bitmap would be absurdly large. `skip[n]` (when `skip` is
// long enough): leave item n out (it is drawn as real text elsewhere); the bitmap is the same size and
// origin either way.
std::optional<Raster> rasterize(const Layouter& lay, const MBox& b, float ppem, const std::vector<bool>& skip = {});

}  // namespace texmath
