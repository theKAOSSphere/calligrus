// What the GL context allows, shared by everything that uploads a texture (spec 36).
//
// main.cpp reads GL_MAX_TEXTURE_SIZE once after the context exists (set_max_texture); the picture, formula, diagram and
// glyph-page code ask max_texture() before they upload, and shrink what does not fit. No GL headers here: the test
// executables can include it too.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "trace.h"

namespace glcaps {

inline int& max_texture_ref() {
    static int n = 2048;  // (until main.cpp has asked the driver; the smallest limit of the GPUs we aim at)
    return n;
}
inline int max_texture() { return max_texture_ref(); }
inline void set_max_texture(int n) { max_texture_ref() = std::max(n, 256); }

// A texture upload the driver refused (a size or memory error): always said on stderr (and in the trace), never silent.
inline void upload_failed(const char* what, unsigned gl_error, int w, int h) {
#ifndef __ANDROID__
    std::fprintf(stderr, "calligrus: %s: texture upload failed (GL error 0x%x, %dx%d, limit %d)\n", what, gl_error, w, h, max_texture());
#endif
    trace::log("%s: texture upload failed (GL error 0x%x, %dx%d, limit %d)", what, gl_error, w, h, max_texture());
}

// The factor (at most 1) that makes a w x h bitmap fit the texture limit.
inline double fit_factor(double w, double h) {
    const double m = std::max(w, h), cap = max_texture();
    return m <= cap ? 1.0 : cap / m;
}

// A straight-alpha RGBA8 bitmap resampled to nw x nh with an area (box) filter, which for a reduction averages every source
// pixel under the new one: no aliasing, any ratio. Colour is weighted by alpha so transparent pixels do not darken the edges.
inline std::vector<unsigned char> resample_rgba(const unsigned char* src, int w, int h, int nw, int nh) {
    // Horizontal pass: nw x h floats per channel (premultiplied), then vertical.
    auto weights = [](int from, int to, int i, int& first, int& last, double& lead, double& trail) {
        const double a = double(i) * from / to, b = double(i + 1) * from / to;
        first = int(std::floor(a));
        last = std::min(from - 1, int(std::ceil(b)) - 1);
        lead = (first + 1) - a;   // how much of the first source pixel counts
        trail = b - last;         // how much of the last one counts
    };
    std::vector<float> mid(size_t(nw) * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < nw; x++) {
            int f, l;
            double lead, trail;
            weights(w, nw, x, f, l, lead, trail);
            double sum[4] = {0, 0, 0, 0}, total = 0;
            for (int s = f; s <= l; s++) {
                double wt = s == f ? lead : s == l ? trail : 1.0;
                if (f == l) wt = double(w) / nw;
                const unsigned char* p = src + (size_t(y) * w + s) * 4;
                const double a = p[3] / 255.0;
                sum[0] += wt * a * p[0], sum[1] += wt * a * p[1], sum[2] += wt * a * p[2], sum[3] += wt * a;
                total += wt;
            }
            float* o = &mid[(size_t(y) * nw + x) * 4];
            for (int c = 0; c < 4; c++) o[c] = float(sum[c] / total);
        }
    std::vector<unsigned char> out(size_t(nw) * nh * 4);
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++) {
            int f, l;
            double lead, trail;
            weights(h, nh, y, f, l, lead, trail);
            double sum[4] = {0, 0, 0, 0}, total = 0;
            for (int s = f; s <= l; s++) {
                double wt = s == f ? lead : s == l ? trail : 1.0;
                if (f == l) wt = double(h) / nh;
                const float* p = &mid[(size_t(s) * nw + x) * 4];
                for (int c = 0; c < 4; c++) sum[c] += wt * p[c];
                total += wt;
            }
            unsigned char* o = &out[(size_t(y) * nw + x) * 4];
            const double a = sum[3] / total;  // premultiplied average alpha (0..1)
            for (int c = 0; c < 3; c++) o[c] = a > 0 ? static_cast<unsigned char>(std::clamp(std::lround(sum[c] / total / a), 0L, 255L)) : 0;
            o[3] = static_cast<unsigned char>(std::clamp(std::lround(a * 255.0), 0L, 255L));
        }
    return out;
}

}  // namespace glcaps
