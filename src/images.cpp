#include "images.h"

#include <map>
#include <optional>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#include <stb_image.h>
#include <webp/decode.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <GL/gl.h>  // GL 1.1 texture calls are exported by opengl32 directly

#include "platform.h"

namespace {

struct Pixels {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;  // straight (not premultiplied) alpha
};

std::optional<Pixels> decode(const std::string& bytes) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const int size = static_cast<int>(bytes.size());
    Pixels p;
    // WebP files start with "RIFF....WEBP".
    if (bytes.size() > 12 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0) {
        unsigned char* out = WebPDecodeRGBA(data, bytes.size(), &p.w, &p.h);
        if (!out) return std::nullopt;
        p.rgba.assign(out, out + size_t(p.w) * p.h * 4);
        WebPFree(out);
        return p;
    }
    int channels = 0;
    unsigned char* out = stbi_load_from_memory(data, size, &p.w, &p.h, &channels, 4);
    if (!out) return std::nullopt;
    p.rgba.assign(out, out + size_t(p.w) * p.h * 4);
    stbi_image_free(out);
    return p;
}

// Very large pictures are shrunk (box filter, integer factor) to keep GPU memory
// sane, like the Rust version's 4096px limit.
void shrink_to_fit(Pixels& p, int limit = 4096) {
    int factor = 1;
    while (p.w / factor > limit || p.h / factor > limit) factor++;
    if (factor == 1) return;
    int w = p.w / factor, h = p.h / factor;
    std::vector<unsigned char> out(size_t(w) * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 4; c++) {
                unsigned sum = 0;
                for (int dy = 0; dy < factor; dy++)
                    for (int dx = 0; dx < factor; dx++)
                        sum += p.rgba[(size_t(y * factor + dy) * p.w + (x * factor + dx)) * 4 + c];
                out[(size_t(y) * w + x) * 4 + c] = static_cast<unsigned char>(sum / (factor * factor));
            }
    p = Pixels{w, h, std::move(out)};
}

// When a file was last changed (nullopt = it can't be read).
std::optional<std::filesystem::file_time_type> modified(const std::filesystem::path& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    return ec ? std::nullopt : std::optional(t);
}

struct Entry {
    std::optional<LoadedImage> image;  // nullopt = could not be loaded
    std::optional<std::filesystem::file_time_type> when;  // the file's change time when read
};

// By path and filters (imagefilter::key, "" for none).
std::map<std::pair<std::filesystem::path, std::string>, Entry> cache;

}  // namespace

bool image_refresh() {
    bool changed = false;
    for (auto it = cache.begin(); it != cache.end();) {
        if (modified(it->first.first) == it->second.when) {
            ++it;
            continue;
        }
        if (it->second.image) {
            GLuint id = static_cast<GLuint>(it->second.image->tex);
            glDeleteTextures(1, &id);
        }
        it = cache.erase(it);
        changed = true;
    }
    return changed;
}

const LoadedImage* image_get(const std::filesystem::path& path, const std::vector<imagefilter::Filter>& filters) {
    const auto k = std::make_pair(path, imagefilter::key(filters));
    if (auto it = cache.find(k); it != cache.end()) return it->second.image ? &*it->second.image : nullptr;
    const auto when = modified(path);

    std::optional<LoadedImage> result;
    if (auto bytes = platform::read_file(path)) {
        if (auto px = decode(*bytes)) {
            shrink_to_fit(*px);
            imagefilter::apply(px->rgba, px->w, px->h, filters);
            GLuint id = 0;
            glGenTextures(1, &id);
            glBindTexture(GL_TEXTURE_2D, id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, px->w, px->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px->rgba.data());
            result = LoadedImage{static_cast<ImTextureID>(id), float(px->w), float(px->h)};
        }
    }
    auto& slot = cache[k] = Entry{result, when};
    return slot.image ? &*slot.image : nullptr;
}
