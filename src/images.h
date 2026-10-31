// Picture loading: decode an image file and upload it as a GL texture (cached).
//
// PNG / JPEG / GIF (first frame) / BMP are decoded with stb_image, WebP with
// libwebp: the same formats the Rust version supports.
#pragma once

#include <filesystem>
#include <vector>

#include <imgui.h>

#include "imagefilter.h"

struct LoadedImage {
    ImTextureID tex = 0;
    float w = 0, h = 0;  // natural size in pixels
};

// Load (once) and return the picture at `path`, or nullptr if it can't be read
// or decoded (failures are remembered so we don't retry every frame). With Marp's `filters`
// applied to its pixels (spec 26; cached separately).
const LoadedImage* image_get(const std::filesystem::path& path, const std::vector<imagefilter::Filter>& filters = {});

// Forget the pictures whose files changed (or appeared) since they were read, so they are
// loaded again. True if any did (the document must then be measured again).
bool image_refresh();
