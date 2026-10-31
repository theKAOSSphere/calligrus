// Port of the tiny-skia (Skia) code paths the Rust TeX engine uses:
//   painter.rs (fill_path / fill_rect / stroke_path + tiling),
//   scan/path_aa.rs + scan/path.rs (supersampled scan conversion),
//   edge.rs, edge_builder.rs, alpha_runs.rs, fixed_point.rs,
//   path_geometry.rs (cubic chopping), scan/hairline*.rs, line_clipper.rs,
//   and the butt-cap case of tiny-skia-path's stroker.
// Names follow the Rust/Skia originals so the two can be compared.
#include "canvas.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace texmath {

namespace {

// ---------------------------------------------------------------------------
// Fixed-point helpers (fixed_point.rs, math.rs)
// ---------------------------------------------------------------------------

using FDot6 = int32_t;   // 26.6 fixed point
using FDot8 = int32_t;   // 24.8
using FDot16 = int32_t;  // 16.16

int32_t left_shift(int32_t value, int shift) { return static_cast<int32_t>(static_cast<uint32_t>(value) << shift); }
int64_t left_shift64(int64_t value, int shift) { return static_cast<int64_t>(static_cast<uint64_t>(value) << shift); }

// Rust's `f32 as i32`: truncate toward zero, saturate, NaN -> 0.
int32_t sat_i32(float v) {
    if (std::isnan(v)) return 0;
    if (v <= -2147483648.0f) return std::numeric_limits<int32_t>::min();
    if (v >= 2147483648.0f) return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(v);
}
int32_t sat_i32(double v) {
    if (std::isnan(v)) return 0;
    if (v <= -2147483648.0) return std::numeric_limits<int32_t>::min();
    if (v >= 2147483647.0) return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(v);
}

namespace fdot6 {
constexpr FDot6 ONE = 64;
FDot6 from_f32(float n) { return sat_i32(n * 64.0f); }
FDot6 floor(FDot6 n) { return n >> 6; }
FDot6 ceil(FDot6 n) { return (n + 63) >> 6; }
FDot6 round(FDot6 n) { return (n + 32) >> 6; }
FDot16 to_fdot16(FDot6 n) { return left_shift(n, 10); }
uint8_t small_scale(uint8_t value, FDot6 dot6) { return static_cast<uint8_t>((int32_t(value) * dot6) >> 6); }
}  // namespace fdot6

namespace fdot16 {
constexpr FDot16 HALF = (1 << 16) / 2;
constexpr FDot16 ONE = 1 << 16;
int32_t floor_to_i32(FDot16 x) { return x >> 16; }
int32_t ceil_to_i32(FDot16 x) { return (x + ONE - 1) >> 16; }
int32_t round_to_i32(FDot16 x) { return (x + HALF) >> 16; }
FDot16 mul(FDot16 a, FDot16 b) { return static_cast<FDot16>((int64_t(a) * int64_t(b)) >> 16); }
// The divide may exceed 32 bits. Clamp to a signed 32 bit result.
FDot16 div(FDot6 numer, FDot6 denom) {
    const int64_t v = left_shift64(numer, 16) / denom;
    return static_cast<int32_t>(std::clamp<int64_t>(v, std::numeric_limits<int32_t>::min(),
                                                    std::numeric_limits<int32_t>::max()));
}
FDot16 fast_div(FDot6 a, FDot6 b) { return left_shift(a, 16) / b; }
}  // namespace fdot16

namespace fdot6 {
FDot16 div(FDot6 a, FDot6 b) {
    if (a >= -32768 && a <= 32767) return left_shift(a, 16) / b;
    return fdot16::div(a, b);
}
}  // namespace fdot6

constexpr float SCALAR_NEARLY_ZERO = 1.0f / 4096.0f;

// ---------------------------------------------------------------------------
// Integer rectangles (ScreenIntRect = non-negative IntRect)
// ---------------------------------------------------------------------------

struct IntRect {
    int32_t x, y;
    int32_t w, h;  // always > 0
    int32_t right() const { return x + w; }
    int32_t bottom() const { return y + h; }
    bool contains(const IntRect& o) const {
        return x <= o.x && y <= o.y && right() >= o.right() && bottom() >= o.bottom();
    }
};

std::optional<IntRect> int_rect_ltrb(int64_t l, int64_t t, int64_t r, int64_t b) {
    if (r - l <= 0 || b - t <= 0) return std::nullopt;
    if (l < INT32_MIN || t < INT32_MIN || r > INT32_MAX || b > INT32_MAX) return std::nullopt;
    return IntRect{int32_t(l), int32_t(t), int32_t(r - l), int32_t(b - t)};
}

std::optional<IntRect> intersect(const IntRect& a, const IntRect& b) {
    return int_rect_ltrb(std::max(a.x, b.x), std::max(a.y, b.y), std::min(a.right(), b.right()),
                         std::min(a.bottom(), b.bottom()));
}

// Rect::round_out (width/height at least 1).
std::optional<IntRect> round_out(const RectF& r) {
    const int64_t l = sat_i32(std::floor(r.left)), t = sat_i32(std::floor(r.top));
    const int64_t rr = sat_i32(std::ceil(r.right)), b = sat_i32(std::ceil(r.bottom));
    return int_rect_ltrb(l, t, l + std::max<int64_t>(1, rr - l), t + std::max<int64_t>(1, b - t));
}

bool finite_rect(const RectF& r) {
    return std::isfinite(r.left) && std::isfinite(r.top) && std::isfinite(r.right) && std::isfinite(r.bottom) &&
           r.left <= r.right && r.top <= r.bottom && std::isfinite(r.right - r.left) &&
           std::isfinite(r.bottom - r.top);
}

std::optional<RectF> bounds_of(const std::vector<Point>& pts) {
    if (pts.empty()) return std::nullopt;
    RectF r{pts[0].x, pts[0].y, pts[0].x, pts[0].y};
    for (const Point& p : pts) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return std::nullopt;
        r.left = std::min(r.left, p.x);
        r.top = std::min(r.top, p.y);
        r.right = std::max(r.right, p.x);
        r.bottom = std::max(r.bottom, p.y);
    }
    if (!finite_rect(r)) return std::nullopt;
    return r;
}

// ---------------------------------------------------------------------------
// Blitters: where coverage turns into pixels
// ---------------------------------------------------------------------------

class Blitter {
public:
    virtual ~Blitter() = default;
    virtual void blit_h(uint32_t x, uint32_t y, uint32_t width) = 0;
    // `runs[i]` = length of a run of pixels with coverage `aa[i]`; 0 ends.
    virtual void blit_anti_h(uint32_t x, uint32_t y, uint8_t* aa, uint16_t* runs) = 0;
    virtual void blit_v(uint32_t x, uint32_t y, uint32_t height, uint8_t alpha) = 0;
    virtual void blit_anti_h2(uint32_t x, uint32_t y, uint8_t a0, uint8_t a1) = 0;
    virtual void blit_anti_v2(uint32_t x, uint32_t y, uint8_t a0, uint8_t a1) = 0;
};

// Paints opaque white into a (sub-)pixmap. tiny-skia's RasterPipelineBlitter
// for an opaque solid colour: full coverage is a plain store, partial coverage
// is its "lowp" lerp `(d*(255-c) + s*c + 255) >> 8` with s = 255.
class PixmapBlitter final : public Blitter {
public:
    PixmapBlitter(Pixmap& pm, uint32_t ox, uint32_t oy, uint32_t w, uint32_t h)
        : pm_(pm), ox_(ox), oy_(oy), w_(w), h_(h) {}

    void blit_h(uint32_t x, uint32_t y, uint32_t width) override {
        for (uint32_t i = 0; i < width; i++) store(x + i, y, 255);
    }
    void blit_anti_h(uint32_t x, uint32_t y, uint8_t* aa, uint16_t* runs) override {
        size_t i = 0;
        while (runs[i] != 0) {
            const uint32_t width = runs[i];
            const uint8_t alpha = aa[i];
            if (alpha == 255) {
                blit_h(x, y, width);
            } else if (alpha != 0) {
                // current_coverage = alpha / 255, then lowp's from_float().
                const float coverage = float(alpha) * (1.0f / 255.0f);
                const auto c = static_cast<uint16_t>(coverage * 255.0f + 0.5f);
                for (uint32_t k = 0; k < width; k++) store(x + k, y, c);
            }
            x += width;
            i += runs[i];
        }
    }
    void blit_v(uint32_t x, uint32_t y, uint32_t height, uint8_t alpha) override {
        for (uint32_t k = 0; k < height; k++) store(x, y + k, alpha);
    }
    void blit_anti_h2(uint32_t x, uint32_t y, uint8_t a0, uint8_t a1) override {
        store(x, y, a0);
        store(x + 1, y, a1);
    }
    void blit_anti_v2(uint32_t x, uint32_t y, uint8_t a0, uint8_t a1) override {
        store(x, y, a0);
        store(x, y + 1, a1);
    }

private:
    Pixmap& pm_;
    uint32_t ox_, oy_, w_, h_;

    void store(uint32_t x, uint32_t y, uint32_t c) {
        // Skia never writes outside its clip; the check is just a safety net.
        if (x >= w_ || y >= h_) return;
        uint8_t* px = pm_.data().data() + (size_t(oy_ + y) * pm_.width() + (ox_ + x)) * 4;
        uint8_t v;
        if (c >= 255) {
            v = 255;
        } else {
            // White ink keeps r = g = b = a, so one value serves all channels.
            v = static_cast<uint8_t>((uint32_t(px[3]) * (255 - c) + 255 * c + 255) >> 8);
        }
        px[0] = px[1] = px[2] = px[3] = v;
    }
};

// ---------------------------------------------------------------------------
// AlphaRuns: one scanline of run-length encoded coverage (alpha_runs.rs)
// ---------------------------------------------------------------------------

struct AlphaRuns {
    std::vector<uint16_t> runs;  // 0 = "None" (end)
    std::vector<uint8_t> alpha;

    explicit AlphaRuns(uint32_t width) : runs(width + 1, 0), alpha(width + 1, 0) { reset(width); }

    static uint8_t catch_overflow(uint16_t a) { return static_cast<uint8_t>(a - (a >> 8)); }

    bool is_empty() const {
        if (runs[0] == 0) return true;
        return alpha[0] == 0 && runs[runs[0]] == 0;
    }

    void reset(uint32_t width) {
        runs[0] = static_cast<uint16_t>(width);
        runs[width] = 0;
        alpha[0] = 0;
    }

    // Accumulate one supersampled span: a partial pixel, `middle_count` full
    // pixels, and another partial pixel. Returns the new `offset_x` hint.
    size_t add(uint32_t x_in, uint8_t start_alpha, size_t middle_count, uint8_t stop_alpha, uint8_t max_value,
               size_t offset_x) {
        size_t x = x_in;
        size_t runs_offset = offset_x;
        size_t alpha_offset = offset_x;
        size_t last_alpha_offset = offset_x;
        x -= offset_x;

        if (start_alpha != 0) {
            break_run(runs.data() + runs_offset, alpha.data() + alpha_offset, x, 1);
            // If the trailing edge of the previous span and the leading edge
            // of this one round to the same supersampled x, this add could
            // reach 256, hence the (tmp - (tmp >> 8)).
            const uint16_t tmp = uint16_t(alpha[alpha_offset + x]) + start_alpha;
            alpha[alpha_offset + x] = static_cast<uint8_t>(tmp - (tmp >> 8));
            runs_offset += x + 1;
            alpha_offset += x + 1;
            x = 0;
        }

        if (middle_count != 0) {
            break_run(runs.data() + runs_offset, alpha.data() + alpha_offset, x, middle_count);
            alpha_offset += x;
            runs_offset += x;
            x = 0;
            for (;;) {
                alpha[alpha_offset] = catch_overflow(uint16_t(alpha[alpha_offset]) + max_value);
                const size_t n = runs[runs_offset];
                if (n == 0) break;  // malformed; Rust would panic here
                alpha_offset += n;
                runs_offset += n;
                middle_count -= std::min(n, middle_count);
                if (middle_count == 0) break;
            }
            last_alpha_offset = alpha_offset;
        }

        if (stop_alpha != 0) {
            break_run(runs.data() + runs_offset, alpha.data() + alpha_offset, x, 1);
            alpha_offset += x;
            alpha[alpha_offset] = static_cast<uint8_t>(alpha[alpha_offset] + stop_alpha);
            last_alpha_offset = alpha_offset;
        }
        return last_alpha_offset;
    }

    // Split runs so that a run boundary falls at `x` and at `x + count`.
    static void break_run(uint16_t* runs, uint8_t* alpha, size_t x, size_t count) {
        const size_t orig_x = x;
        size_t off = 0;
        while (x > 0) {
            const size_t n = runs[off];
            if (n == 0) return;
            if (x < n) {
                alpha[off + x] = alpha[off];
                runs[off] = static_cast<uint16_t>(x);
                runs[off + x] = static_cast<uint16_t>(n - x);
                break;
            }
            off += n;
            x -= n;
        }
        off = orig_x;
        x = count;
        for (;;) {
            const size_t n = runs[off];
            if (n == 0) return;
            if (x < n) {
                alpha[off + x] = alpha[off];
                runs[off] = static_cast<uint16_t>(x);
                runs[off + x] = static_cast<uint16_t>(n - x);
                break;
            }
            x -= n;
            if (x == 0) break;
            off += n;
        }
    }

    static void break_at(uint8_t* alpha, uint16_t* runs, int32_t x) {
        size_t ai = 0, ri = 0;
        while (x > 0) {
            const int32_t n = runs[ri];
            if (n == 0) return;
            if (x < n) {
                alpha[ai + x] = alpha[ai];
                runs[0] = static_cast<uint16_t>(x);
                runs[x] = static_cast<uint16_t>(n - x);
                break;
            }
            ri += n;
            ai += n;
            x -= n;
        }
    }
};

// ---------------------------------------------------------------------------
// SuperBlitter: collects 4 sub-scanlines per pixel row (scan/path_aa.rs)
// ---------------------------------------------------------------------------

constexpr uint32_t SHIFT = 2;  // SUPERSAMPLE_SHIFT: 4x4 subsamples per pixel
constexpr uint32_t SCALE = 1u << SHIFT;
constexpr uint32_t MASK = SCALE - 1;

class SuperBlitter {
public:
    // `sect` = path bounds clipped to the clip rectangle (non-negative).
    SuperBlitter(const IntRect& sect, Blitter& real)
        : real_(real),
          curr_iy_(sect.y - 1),
          width_(uint32_t(sect.w)),
          left_(uint32_t(sect.x)),
          super_left_(uint32_t(sect.x) << SHIFT),
          curr_y_(int32_t(uint32_t(sect.y) << SHIFT) - 1),
          top_(sect.y),
          runs_(uint32_t(sect.w)) {}

    ~SuperBlitter() { flush(); }
    SuperBlitter(const SuperBlitter&) = delete;
    SuperBlitter& operator=(const SuperBlitter&) = delete;

    // A span of full coverage on one sub-scanline (supersampled coordinates).
    void blit_h(uint32_t x, uint32_t y, uint32_t width) {
        const int32_t iy = int32_t(y >> SHIFT);
        // "hack, until I figure out why my cubics (I think) go beyond the bounds"
        if (x >= super_left_) {
            x -= super_left_;
        } else {
            width = x + width;
            x = 0;
        }
        if (width == 0) return;
        if (curr_y_ != int32_t(y)) {
            offset_x_ = 0;
            curr_y_ = int32_t(y);
        }
        if (iy != curr_iy_) {  // new scanline
            flush();
            curr_iy_ = iy;
        }
        const uint32_t start = x;
        const uint32_t stop = x + width;
        // integer-pixel-aligned ends of blit, rounded out
        uint32_t fb = start & MASK;
        uint32_t fe = stop & MASK;
        int32_t n = int32_t(stop >> SHIFT) - int32_t(start >> SHIFT) - 1;
        if (n < 0) {
            fb = fe - fb;
            n = 0;
            fe = 0;
        } else if (fb == 0) {
            n += 1;
        } else {
            fb = SCALE - fb;
        }
        // Each sub-scanline adds 64 to a full pixel, except the last of the
        // four, which adds 63, so a fully covered pixel sums to 255.
        const auto max_value = static_cast<uint8_t>((1u << (8 - SHIFT)) - (((y & MASK) + 1) >> SHIFT));
        if ((x >> SHIFT) + uint32_t(n) + 1 > width_ + 1) return;  // safety net (Rust would panic)
        offset_x_ = runs_.add(x >> SHIFT, partial(fb), size_t(n), partial(fe), max_value, offset_x_);
    }

private:
    Blitter& real_;
    int32_t curr_iy_;
    uint32_t width_, left_, super_left_;
    int32_t curr_y_;
    int32_t top_;
    AlphaRuns runs_;
    size_t offset_x_ = 0;

    // coverage of a partial pixel (in sub-pixels) -> its share of 0..256
    static uint8_t partial(uint32_t aa) { return static_cast<uint8_t>(aa << (8 - 2 * SHIFT)); }

    void flush() {
        if (curr_iy_ >= top_) {
            if (!runs_.is_empty()) {
                real_.blit_anti_h(left_, uint32_t(curr_iy_), runs_.alpha.data(), runs_.runs.data());
                runs_.reset(width_);
                offset_x_ = 0;
            }
            curr_iy_ = top_ - 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Edges (edge.rs): lines and cubics walked one scanline at a time
// ---------------------------------------------------------------------------

constexpr int32_t MAX_COEFF_SHIFT = 6;

struct Edge {
    bool cubic = false;
    int32_t prev = -1, next = -1;  // a linked list inside a vector (-1 = none)
    FDot16 x = 0, dx = 0;
    int32_t first_y = 0, last_y = 0;
    int8_t winding = 0;  // 1 or -1
    // cubic forward-differencing state
    int8_t curve_count = 0;
    uint8_t curve_shift = 0, dshift = 0;
    FDot16 cx = 0, cy = 0, cdx = 0, cdy = 0, cddx = 0, cddy = 0, cdddx = 0, cdddy = 0, c_last_x = 0, c_last_y = 0;
};

// This correctly favors the lower-pixel when y0 is on a 1/2 pixel boundary
FDot6 compute_dy(FDot6 top, FDot6 y0) { return left_shift(top, 6) + 32 - y0; }

std::optional<Edge> line_edge(Point p0, Point p1, int32_t shift) {
    const float scale = float(1 << (shift + 6));
    int32_t x0 = sat_i32(p0.x * scale), y0 = sat_i32(p0.y * scale);
    int32_t x1 = sat_i32(p1.x * scale), y1 = sat_i32(p1.y * scale);
    int8_t winding = 1;
    if (y0 > y1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
        winding = -1;
    }
    const int32_t top = fdot6::round(y0);
    const int32_t bottom = fdot6::round(y1);
    if (top == bottom) return std::nullopt;  // zero-height line
    const FDot16 slope = fdot6::div(x1 - x0, y1 - y0);
    const FDot6 dy = compute_dy(top, y0);
    Edge e;
    e.x = fdot6::to_fdot16(x0 + fdot16::mul(slope, dy));
    e.dx = slope;
    e.first_y = top;
    e.last_y = bottom - 1;
    e.winding = winding;
    return e;
}

bool line_update(Edge& e, FDot16 x0, FDot16 y0, FDot16 x1, FDot16 y1) {
    y0 >>= 10;
    y1 >>= 10;
    const int32_t top = fdot6::round(y0);
    const int32_t bottom = fdot6::round(y1);
    if (top == bottom) return false;
    x0 >>= 10;
    x1 >>= 10;
    const FDot16 slope = fdot6::div(x1 - x0, y1 - y0);
    const FDot6 dy = compute_dy(top, y0);
    e.x = fdot6::to_fdot16(x0 + fdot16::mul(slope, dy));
    e.dx = slope;
    e.first_y = top;
    e.last_y = bottom - 1;
    return true;
}

FDot6 cheap_distance(FDot6 dx, FDot6 dy) {
    dx = std::abs(dx);
    dy = std::abs(dy);
    return dx > dy ? dx + (dy >> 1) : dy + (dx >> 1);  // max + min/2
}

int32_t diff_to_shift(FDot6 dx, FDot6 dy, int32_t shift_aa) {
    // cheap calc of distance from center of p0-p2 to the center of the curve
    FDot6 dist = cheap_distance(dx, dy);
    // shift down dist (it is currently in dot6)
    // down by 3 should give us 1/8 pixel accuracy (assuming our dist is accurate...)
    dist = (dist + (1 << (2 + shift_aa))) >> (3 + shift_aa);
    // each subdivision (shift value) cuts this dist (error) by 1/4
    return (32 - int32_t(std::countl_zero(uint32_t(dist)))) >> 1;
}

// f(1/3) and f(2/3) distance from the chord, using 16/512 to approximate 1/27
FDot6 cubic_delta_from_line(FDot6 a, FDot6 b, FDot6 c, FDot6 d) {
    const int32_t one_third = ((a * 8 - b * 15 + 6 * c + d) * 19) >> 9;
    const int32_t two_third = ((a + 6 * b - c * 15 + d * 8) * 19) >> 9;
    return std::max(std::abs(one_third), std::abs(two_third));
}

bool cubic_update(Edge& e) {
    bool success;
    int8_t count = e.curve_count;
    FDot16 oldx = e.cx, oldy = e.cy, newx, newy;
    const uint8_t ddshift = e.curve_shift;
    const uint8_t dshift = e.dshift;
    for (;;) {
        count += 1;
        if (count < 0) {
            newx = oldx + (e.cdx >> dshift);
            e.cdx += e.cddx >> ddshift;
            e.cddx += e.cdddx;
            newy = oldy + (e.cdy >> dshift);
            e.cdy += e.cddy >> ddshift;
            e.cddy += e.cdddy;
        } else {  // last segment
            newx = e.c_last_x;
            newy = e.c_last_y;
        }
        // our finite fixed point doesn't always keep y monotonic: pin it
        if (newy < oldy) newy = oldy;
        success = line_update(e, oldx, oldy, newx, newy);
        oldx = newx;
        oldy = newy;
        if (count == 0 || success) break;
    }
    e.cx = newx;
    e.cy = newy;
    e.curve_count = count;
    return success;
}

std::optional<Edge> cubic_edge(const Point* pts, int32_t shift) {
    const float scale = float(1 << (shift + 6));
    int32_t x0 = sat_i32(pts[0].x * scale), y0 = sat_i32(pts[0].y * scale);
    int32_t x1 = sat_i32(pts[1].x * scale), y1 = sat_i32(pts[1].y * scale);
    int32_t x2 = sat_i32(pts[2].x * scale), y2 = sat_i32(pts[2].y * scale);
    int32_t x3 = sat_i32(pts[3].x * scale), y3 = sat_i32(pts[3].y * scale);
    int8_t winding = 1;
    if (y0 > y3) {
        std::swap(x0, x3);
        std::swap(x1, x2);
        std::swap(y0, y3);
        std::swap(y1, y2);
        winding = -1;
    }
    const int32_t top = fdot6::round(y0);
    const int32_t bot = fdot6::round(y3);
    if (top == bot) return std::nullopt;  // zero-height cubic

    // compute number of steps needed (1 << shift)
    {
        const FDot6 dx = cubic_delta_from_line(x0, x1, x2, x3);
        const FDot6 dy = cubic_delta_from_line(y0, y1, y2, y3);
        shift = diff_to_shift(dx, dy, 2) + 1;  // add 1 (by observation)
    }
    if (shift > MAX_COEFF_SHIFT) shift = MAX_COEFF_SHIFT;

    // The incoming data is shifted down by 8 (antialias); we compute
    // coefficients with a 3*, so the safest upshift is 6.
    int32_t up_shift = 6;
    int32_t down_shift = shift + up_shift - 10;
    if (down_shift < 0) {
        down_shift = 0;
        up_shift = 10 - shift;
    }

    Edge e;
    e.cubic = true;
    e.winding = winding;
    e.curve_count = static_cast<int8_t>(left_shift(-1, shift));
    e.curve_shift = static_cast<uint8_t>(shift);
    e.dshift = static_cast<uint8_t>(down_shift);

    int32_t b = left_shift(3 * (x1 - x0), up_shift);
    int32_t c = left_shift(3 * (x0 - x1 - x1 + x2), up_shift);
    int32_t d = left_shift(x3 + 3 * (x1 - x2) - x0, up_shift);
    e.cx = fdot6::to_fdot16(x0);
    e.cdx = b + (c >> shift) + (d >> (2 * shift));  // biased by shift
    e.cddx = 2 * c + ((3 * d) >> (shift - 1));      // biased by 2*shift
    e.cdddx = (3 * d) >> (shift - 1);               // biased by 2*shift

    b = left_shift(3 * (y1 - y0), up_shift);
    c = left_shift(3 * (y0 - y1 - y1 + y2), up_shift);
    d = left_shift(y3 + 3 * (y1 - y2) - y0, up_shift);
    e.cy = fdot6::to_fdot16(y0);
    e.cdy = b + (c >> shift) + (d >> (2 * shift));
    e.cddy = 2 * c + ((3 * d) >> (shift - 1));
    e.cdddy = (3 * d) >> (shift - 1);

    e.c_last_x = fdot6::to_fdot16(x3);
    e.c_last_y = fdot6::to_fdot16(y3);
    if (!cubic_update(e)) return std::nullopt;
    return e;
}

// ---------------------------------------------------------------------------
// Cubic chopping at Y extrema (path_geometry.rs), so every cubic edge is
// monotonic in y.
// ---------------------------------------------------------------------------

std::optional<float> valid_unit_divide(float numer, float denom) {
    if (numer < 0.0f) {
        numer = -numer;
        denom = -denom;
    }
    if (denom == 0.0f || numer == 0.0f || numer >= denom) return std::nullopt;
    const float r = numer / denom;
    if (!(r > 0.0f && r < 1.0f)) return std::nullopt;  // NormalizedF32Exclusive
    return r;
}

// From Numerical Recipes in C: roots of At^2 + Bt + C strictly inside (0, 1).
size_t find_unit_quad_roots(float a, float b, float c, float roots[3]) {
    if (a == 0.0f) {
        if (auto r = valid_unit_divide(-c, b)) {
            roots[0] = *r;
            return 1;
        }
        return 0;
    }
    // use doubles so we don't overflow temporarily trying to compute R
    double dr = double(b) * double(b) - 4.0 * double(a) * double(c);
    if (dr < 0.0) return 0;
    dr = std::sqrt(dr);
    const float r = static_cast<float>(dr);
    if (!std::isfinite(r)) return 0;
    const float q = b < 0.0f ? -(b - r) / 2.0f : -(b + r) / 2.0f;
    size_t n = 0;
    if (auto v = valid_unit_divide(q, a)) roots[n++] = *v;
    if (auto v = valid_unit_divide(c, q)) roots[n++] = *v;
    if (n == 2) {
        if (roots[0] > roots[1]) {
            std::swap(roots[0], roots[1]);
        } else if (roots[0] == roots[1]) {
            n -= 1;  // skip the double root
        }
    }
    return n;
}

Point interp(Point v0, Point v1, float t) { return Point{v0.x + (v1.x - v0.x) * t, v0.y + (v1.y - v0.y) * t}; }

// de Casteljau split at t: dst[0..7]
void chop_cubic_at2(const Point src[4], float t, Point* dst) {
    const Point ab = interp(src[0], src[1], t);
    const Point bc = interp(src[1], src[2], t);
    const Point cd = interp(src[2], src[3], t);
    const Point abc = interp(ab, bc, t);
    const Point bcd = interp(bc, cd, t);
    const Point abcd = interp(abc, bcd, t);
    dst[0] = src[0];
    dst[1] = ab;
    dst[2] = abc;
    dst[3] = abcd;
    dst[4] = bcd;
    dst[5] = cd;
    dst[6] = src[3];
}

void chop_cubic_at(const Point src_in[4], const float* t_values, size_t n, Point* dst) {
    if (n == 0) {
        for (int i = 0; i < 4; i++) dst[i] = src_in[i];
        return;
    }
    float t = t_values[0];
    Point tmp[4];
    const Point* src = src_in;
    size_t dst_offset = 0;
    for (size_t i = 0; i < n; i++) {
        chop_cubic_at2(src, t, dst + dst_offset);
        if (i == n - 1) break;
        dst_offset += 3;
        // have src point to the remaining cubic (after the chop)
        for (int k = 0; k < 4; k++) tmp[k] = dst[dst_offset + k];
        src = tmp;
        // watch out in case the renormalized t isn't in range
        if (auto r = valid_unit_divide(t_values[i + 1] - t_values[i], 1.0f - t_values[i])) {
            t = *r;
        } else {
            // if we can't, just create a degenerate cubic
            dst[dst_offset + 4] = src[3];
            dst[dst_offset + 5] = src[3];
            dst[dst_offset + 6] = src[3];
            break;
        }
    }
}

// Cubic'(t) = At^2 + Bt + C (divided by 3); solve inside (0, 1).
size_t find_cubic_extrema(float a, float b, float c, float d, float t_values[3]) {
    const float na = d - a + 3.0f * (b - c);
    const float nb = 2.0f * (a - b - b + c);
    const float nc = b - a;
    return find_unit_quad_roots(na, nb, nc, t_values);
}

size_t chop_cubic_at_y_extrema(const Point src[4], Point dst[10]) {
    float t_values[3];
    const size_t n = find_cubic_extrema(src[0].y, src[1].y, src[2].y, src[3].y, t_values);
    chop_cubic_at(src, t_values, n, dst);
    if (n > 0) {
        // we do some cleanup to ensure our Y extrema are flat
        dst[2].y = dst[3].y;
        dst[4].y = dst[3].y;
        if (n == 2) {
            dst[5].y = dst[6].y;
            dst[7].y = dst[6].y;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Line clipping (line_clipper.rs)
// ---------------------------------------------------------------------------

float sect_with_horizontal(const Point src[2], float y) {
    const float dy = src[1].y - src[0].y;
    if (std::fabs(dy) <= SCALAR_NEARLY_ZERO) return (src[0].x + src[1].x) * 0.5f;
    const double x0 = src[0].x, y0 = src[0].y, x1 = src[1].x, y1 = src[1].y;
    double result = x0 + (double(y) - y0) * (x1 - x0) / (y1 - y0);
    // pin to [x0, x1] against rounding
    const double lo = std::min(x0, x1), hi = std::max(x0, x1);
    result = std::clamp(result, lo, hi);
    return static_cast<float>(result);
}

float sect_with_vertical(const Point src[2], float x) {
    const float dx = src[1].x - src[0].x;
    if (std::fabs(dx) <= SCALAR_NEARLY_ZERO) return (src[0].y + src[1].y) * 0.5f;
    const double x0 = src[0].x, y0 = src[0].y, x1 = src[1].x, y1 = src[1].y;
    return static_cast<float>(y0 + (double(x) - x0) * (y1 - y0) / (x1 - x0));
}

float sect_clamp_with_vertical(const Point src[2], float x) {
    const float y = sect_with_vertical(src, x);
    const float lo = std::min(src[0].y, src[1].y), hi = std::max(src[0].y, src[1].y);
    return y < lo ? lo : (y > hi ? hi : y);
}

// Clip a line to `clip`; returns 0..4 points (1..3 lines). Parts left/right
// of the clip become vertical lines on its edge, keeping the winding.
size_t clip_line(const Point src[2], const RectF& clip, Point out[4]) {
    size_t index0 = src[0].y < src[1].y ? 0 : 1;
    size_t index1 = 1 - index0;
    if (src[index1].y <= clip.top) return 0;     // above
    if (src[index0].y >= clip.bottom) return 0;  // below
    Point tmp[2] = {src[0], src[1]};
    if (src[index0].y < clip.top) tmp[index0] = Point{sect_with_horizontal(src, clip.top), clip.top};
    if (tmp[index1].y > clip.bottom) tmp[index1] = Point{sect_with_horizontal(src, clip.bottom), clip.bottom};

    Point storage[4];
    size_t line_count = 1;
    bool reverse;
    if (src[0].x < src[1].x) {
        index0 = 0, index1 = 1, reverse = false;
    } else {
        index0 = 1, index1 = 0, reverse = true;
    }
    const Point* result;
    if (tmp[index1].x <= clip.left) {  // wholly to the left
        tmp[0].x = clip.left;
        tmp[1].x = clip.left;
        reverse = false;
        result = tmp;
    } else if (tmp[index0].x >= clip.right) {  // wholly to the right (can't cull)
        tmp[0].x = clip.right;
        tmp[1].x = clip.right;
        reverse = false;
        result = tmp;
    } else {
        size_t offset = 0;
        if (tmp[index0].x < clip.left) {
            storage[offset++] = Point{clip.left, tmp[index0].y};
            storage[offset] = Point{clip.left, sect_clamp_with_vertical(tmp, clip.left)};
        } else {
            storage[offset] = tmp[index0];
        }
        offset += 1;
        if (tmp[index1].x > clip.right) {
            storage[offset++] = Point{clip.right, sect_clamp_with_vertical(tmp, clip.right)};
            storage[offset] = Point{clip.right, tmp[index1].y};
        } else {
            storage[offset] = tmp[index1];
        }
        line_count = offset;
        result = storage;
    }
    if (reverse) {
        for (size_t i = 0; i <= line_count; i++) out[line_count - i] = result[i];
    } else {
        for (size_t i = 0; i <= line_count; i++) out[i] = result[i];
    }
    return line_count + 1;
}

// line_clipper::intersect: clip a hairline to a rectangle (false = outside).
bool intersect_line(const Point src[2], const RectF& clip, Point dst[2]) {
    const RectF b{std::min(src[0].x, src[1].x), std::min(src[0].y, src[1].y), std::max(src[0].x, src[1].x),
                  std::max(src[0].y, src[1].y)};
    if (finite_rect(b)) {
        if (clip.left <= b.left && clip.top <= b.top && clip.right >= b.right && clip.bottom >= b.bottom) {
            dst[0] = src[0];
            dst[1] = src[1];
            return true;
        }
        auto nested_lt = [](float a, float c, float dim) { return a <= c && (a < c || dim > 0.0f); };
        const float bw = b.right - b.left, bh = b.bottom - b.top;
        if (nested_lt(b.right, clip.left, bw) || nested_lt(clip.right, b.left, bw) ||
            nested_lt(b.bottom, clip.top, bh) || nested_lt(clip.bottom, b.top, bh)) {
            return false;
        }
    }
    size_t index0 = src[0].y < src[1].y ? 0 : 1;
    size_t index1 = 1 - index0;
    Point tmp[2] = {src[0], src[1]};
    if (tmp[index0].y < clip.top) tmp[index0] = Point{sect_with_horizontal(src, clip.top), clip.top};
    if (tmp[index1].y > clip.bottom) tmp[index1] = Point{sect_with_horizontal(src, clip.bottom), clip.bottom};
    index0 = tmp[0].x < tmp[1].x ? 0 : 1;
    index1 = 1 - index0;
    if (tmp[index1].x <= clip.left || tmp[index0].x >= clip.right) {
        // usually outside, unless the line is vertical and on the clip edge
        if (tmp[0].x != tmp[1].x || tmp[0].x < clip.left || tmp[0].x > clip.right) return false;
    }
    if (tmp[index0].x < clip.left) tmp[index0] = Point{clip.left, sect_with_vertical(src, clip.left)};
    if (tmp[index1].x > clip.right) tmp[index1] = Point{clip.right, sect_with_vertical(src, clip.right)};
    dst[0] = tmp[0];
    dst[1] = tmp[1];
    return true;
}

// ---------------------------------------------------------------------------
// Edge building (edge_builder.rs)
// ---------------------------------------------------------------------------

enum class Combine { No, Partial, Total };

// Merge a new vertical line edge into the previous one where they touch.
Combine combine_vertical(const Edge& edge, Edge& last) {
    if (last.dx != 0 || edge.x != last.x) return Combine::No;
    if (edge.winding == last.winding) {
        if (edge.last_y + 1 == last.first_y) {
            last.first_y = edge.first_y;
            return Combine::Partial;
        }
        if (edge.first_y == last.last_y + 1) {
            last.last_y = edge.last_y;
            return Combine::Partial;
        }
        return Combine::No;
    }
    if (edge.first_y == last.first_y) {
        if (edge.last_y == last.last_y) return Combine::Total;
        if (edge.last_y < last.last_y) {
            last.first_y = edge.last_y + 1;
            return Combine::Partial;
        }
        last.first_y = last.last_y + 1;
        last.last_y = edge.last_y;
        last.winding = edge.winding;
        return Combine::Partial;
    }
    if (edge.last_y == last.last_y) {
        if (edge.first_y > last.first_y) {
            last.last_y = edge.first_y - 1;
        } else {
            last.last_y = last.first_y - 1;
            last.first_y = edge.first_y;
            last.winding = edge.winding;
        }
        return Combine::Partial;
    }
    return Combine::No;
}

class EdgeBuilder {
public:
    std::vector<Edge> edges;

    void push_line(Point p0, Point p1) {
        auto e = line_edge(p0, p1, int32_t(SHIFT));
        if (!e) return;
        Combine combine = Combine::No;
        if (e->dx == 0 && !edges.empty() && !edges.back().cubic) combine = combine_vertical(*e, edges.back());
        if (combine == Combine::Total) {
            edges.pop_back();
        } else if (combine == Combine::No) {
            edges.push_back(*e);
        }
    }
    void push_cubic(const Point* pts) {
        if (auto e = cubic_edge(pts, int32_t(SHIFT))) edges.push_back(*e);
    }

    // Unclipped: lines as they are, cubics split into y-monotonic pieces.
    void add_line(Point p0, Point p1, const RectF* clip) {
        if (!clip) {
            push_line(p0, p1);
            return;
        }
        const Point src[2] = {p0, p1};
        Point pts[4];
        const size_t n = clip_line(src, *clip, pts);
        for (size_t i = 0; i + 1 < n; i++) push_line(pts[i], pts[i + 1]);
    }
    void add_cubic(const Point* p, const RectF* clip) {
        Point mono[10];
        const size_t n = chop_cubic_at_y_extrema(p, mono);
        for (size_t i = 0; i <= n; i++) {
            const Point* c = mono + i * 3;
            if (!clip || inside(c, *clip)) {
                push_cubic(c);
            } else {
                // Only for tiles of huge (> 8191 px) bitmaps: tiny-skia chops
                // the curve exactly at the clip edges; we flatten it instead.
                Point prev = c[0];
                constexpr int kSteps = 16;
                for (int s = 1; s <= kSteps; s++) {
                    const float t = float(s) / kSteps, u = 1.0f - t;
                    const Point q = s == kSteps ? c[3]
                                                : Point{u * u * u * c[0].x + 3 * u * u * t * c[1].x +
                                                            3 * u * t * t * c[2].x + t * t * t * c[3].x,
                                                        u * u * u * c[0].y + 3 * u * u * t * c[1].y +
                                                            3 * u * t * t * c[2].y + t * t * t * c[3].y};
                    add_line(prev, q, clip);
                    prev = q;
                }
            }
        }
    }

private:
    static bool inside(const Point* c, const RectF& clip) {
        for (int i = 0; i < 4; i++) {
            if (c[i].x < clip.left || c[i].x > clip.right || c[i].y < clip.top || c[i].y > clip.bottom) return false;
        }
        return true;
    }
};

// Feed every segment of `path` (closing open contours) to the edge builder,
// in the order tiny-skia's PathEdgeIter yields them.
void build_edges(const Path& path, const RectF* clip, EdgeBuilder& eb) {
    size_t pi = 0;
    Point move_to{0, 0};
    bool needs_close_line = false;
    auto close_line = [&] {
        needs_close_line = false;
        eb.add_line(path.points[pi - 1], move_to, clip);
    };
    for (Verb v : path.verbs) {
        switch (v) {
            case Verb::Move:
                if (needs_close_line) close_line();
                move_to = path.points[pi++];
                break;
            case Verb::Close:
                if (needs_close_line) close_line();
                break;
            case Verb::Line:
                needs_close_line = true;
                eb.add_line(path.points[pi - 1], path.points[pi], clip);
                pi += 1;
                break;
            case Verb::Cubic:
                needs_close_line = true;
                eb.add_cubic(&path.points[pi - 1], clip);
                pi += 3;
                break;
        }
    }
    if (needs_close_line) close_line();
}

// ---------------------------------------------------------------------------
// The scan converter (scan/path.rs)
// ---------------------------------------------------------------------------

void remove_edge(size_t curr, std::vector<Edge>& edges) {
    const int32_t prev = edges[curr].prev, next = edges[curr].next;
    edges[size_t(prev)].next = next;
    edges[size_t(next)].prev = prev;
}

void insert_edge_after(size_t curr, size_t after, std::vector<Edge>& edges) {
    edges[curr].prev = int32_t(after);
    edges[curr].next = edges[after].next;
    const size_t after_next = size_t(edges[after].next);
    edges[after_next].prev = int32_t(curr);
    edges[after].next = int32_t(curr);
}

// ripple an edge backwards until the list is x-sorted again
void backward_insert_edge_based_on_x(size_t curr, std::vector<Edge>& edges) {
    const FDot16 x = edges[curr].x;
    size_t prev = size_t(edges[curr].prev);
    while (prev != 0) {
        if (edges[prev].x > x) {
            prev = size_t(edges[prev].prev);
        } else {
            break;
        }
    }
    const size_t next = size_t(edges[prev].next);
    if (next != curr) {
        remove_edge(curr, edges);
        insert_edge_after(curr, prev, edges);
    }
}

size_t backward_insert_start(size_t prev, FDot16 x, std::vector<Edge>& edges) {
    while (edges[prev].prev >= 0) {
        prev = size_t(edges[prev].prev);
        if (edges[prev].x <= x) break;
    }
    return prev;
}

void insert_new_edges(size_t new_idx, int32_t curr_y, std::vector<Edge>& edges) {
    if (edges[new_idx].first_y != curr_y) return;
    const size_t prev = size_t(edges[new_idx].prev);
    if (edges[prev].x <= edges[new_idx].x) return;
    // find first x pos to insert
    size_t start = backward_insert_start(prev, edges[new_idx].x, edges);
    // insert the lot, fixing up the links as we go
    for (;;) {
        const size_t next = size_t(edges[new_idx].next);
        bool keep_edge = false;
        for (;;) {
            const size_t after = size_t(edges[start].next);
            if (after == new_idx) {
                keep_edge = true;
                break;
            }
            if (edges[after].x >= edges[new_idx].x) break;
            start = after;
        }
        if (!keep_edge) {
            remove_edge(new_idx, edges);
            insert_edge_after(new_idx, start, edges);
        }
        start = new_idx;
        new_idx = next;
        if (edges[new_idx].first_y != curr_y) break;
    }
}

// Walk the sorted edge list one sub-scanline at a time, emitting the spans
// where the winding number is non-zero.
void walk_edges(uint32_t start_y, uint32_t stop_y, uint32_t right_clip, std::vector<Edge>& edges,
                SuperBlitter& blitter) {
    uint32_t curr_y = start_y;
    const int32_t winding_mask = -1;  // non-zero fill rule
    for (;;) {
        int32_t w = 0;
        uint32_t left = 0;
        FDot16 prev_x = edges[0].x;
        size_t curr = size_t(edges[0].next);
        while (edges[curr].first_y <= int32_t(curr_y)) {
            const auto x = static_cast<uint32_t>(fdot16::round_to_i32(edges[curr].x));
            if ((w & winding_mask) == 0) left = x;  // we're starting interval
            w += edges[curr].winding;
            if ((w & winding_mask) == 0) {  // we finished an interval
                const uint32_t width = x - left;
                if (width != 0) blitter.blit_h(left, curr_y, width);
            }
            const size_t next = size_t(edges[curr].next);
            if (edges[curr].last_y == int32_t(curr_y)) {  // are we done with this edge?
                if (!edges[curr].cubic) {
                    remove_edge(curr, edges);
                } else if (edges[curr].curve_count < 0 && cubic_update(edges[curr])) {
                    const FDot16 new_x = edges[curr].x;
                    if (new_x < prev_x) {
                        backward_insert_edge_based_on_x(curr, edges);
                    } else {
                        prev_x = new_x;
                    }
                } else {
                    remove_edge(curr, edges);
                }
            } else {
                const FDot16 new_x = edges[curr].x + edges[curr].dx;
                edges[curr].x = new_x;
                if (new_x < prev_x) {
                    backward_insert_edge_based_on_x(curr, edges);
                } else {
                    prev_x = new_x;
                }
            }
            curr = next;
        }
        if ((w & winding_mask) != 0) {  // was our right-edge culled away?
            const uint32_t width = right_clip - left;
            if (width != 0) blitter.blit_h(left, curr_y, width);
        }
        curr_y += 1;
        if (curr_y >= stop_y) break;
        // now `curr` points to the first edge with a Yint larger than curr_y
        insert_new_edges(curr, int32_t(curr_y), edges);
    }
}

// scan::path_aa::fill_path for one clip rectangle (a whole pixmap or a tile).
void fill_path_aa(const Path& path, const IntRect& clip, Blitter& blitter) {
    // Round out first, then convert into a Rect.
    const RectF fb{std::floor(path.bounds.left), std::floor(path.bounds.top), std::ceil(path.bounds.right),
                   std::ceil(path.bounds.bottom)};
    if (!finite_rect(fb)) return;
    auto ir = round_out(fb);
    if (!ir) return;
    if (!intersect(*ir, clip)) return;
    if (clip.right() > 32767 || clip.bottom() > 32767) return;  // AA can't handle such clips

    auto sect = intersect(*ir, clip);
    if (!sect || sect->x < 0 || sect->y < 0) return;
    const bool contained = ir->x >= 0 && ir->y >= 0 && clip.contains(*ir);

    EdgeBuilder eb;
    eb.edges.reserve(64);
    const RectF clip_f{float(clip.x), float(clip.y), float(clip.right()), float(clip.bottom())};
    build_edges(path, contained ? nullptr : &clip_f, eb);
    std::vector<Edge>& edges = eb.edges;
    if (edges.size() < 2) return;

    std::stable_sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
        if (a.first_y != b.first_y) return a.first_y < b.first_y;
        return a.x < b.x;
    });
    for (size_t i = 0; i < edges.size(); i++) {
        // 0 will be the head sentinel, so the real edges start at 1.
        edges[i].prev = int32_t(i);
        edges[i].next = int32_t(i + 2);
    }
    Edge head;
    head.next = 1;
    head.x = std::numeric_limits<int32_t>::min();
    head.first_y = std::numeric_limits<int32_t>::min();
    edges.insert(edges.begin(), head);
    Edge tail;
    tail.prev = int32_t(edges.size() - 1);
    tail.first_y = std::numeric_limits<int32_t>::max();
    edges.push_back(tail);

    int64_t start_y = int64_t(ir->y) << SHIFT;
    int64_t stop_y = int64_t(ir->bottom()) << SHIFT;
    const int64_t top = int64_t(clip.y) << SHIFT;
    const int64_t bottom = int64_t(clip.bottom()) << SHIFT;
    if (!contained && start_y < top) start_y = top;
    if (!contained && stop_y > bottom) stop_y = bottom;
    if (start_y < 0 || stop_y < 0) return;

    SuperBlitter sb(*sect, blitter);
    walk_edges(uint32_t(start_y), uint32_t(stop_y), uint32_t(clip.right()) << SHIFT, edges, sb);
}

// ---------------------------------------------------------------------------
// Anti-aliased hairlines (scan/hairline_aa.rs), for lines of width <= 1px
// ---------------------------------------------------------------------------

// Forwards to another blitter, dropping whatever falls outside `clip`.
class RectClipBlitter final : public Blitter {
public:
    RectClipBlitter(Blitter& b, const IntRect& clip) : b_(b), clip_(clip) {}

    void blit_h(uint32_t, uint32_t, uint32_t) override {}  // not used by hairlines
    void blit_anti_h(uint32_t x, uint32_t y, uint8_t* aa, uint16_t* runs) override {
        if (!(y - uint32_t(clip_.y) < uint32_t(clip_.h)) || x >= uint32_t(clip_.right())) return;
        uint32_t x0 = x;
        uint32_t x1 = x + anti_width(runs);
        if (x1 <= uint32_t(clip_.x)) return;
        if (x0 < uint32_t(clip_.x)) {
            const uint32_t dx = uint32_t(clip_.x) - x0;
            AlphaRuns::break_at(aa, runs, int32_t(dx));
            aa += dx;
            runs += dx;
            x0 = uint32_t(clip_.x);
        }
        if (x1 > uint32_t(clip_.right())) {
            x1 = uint32_t(clip_.right());
            AlphaRuns::break_at(aa, runs, int32_t(x1 - x0));
            runs[x1 - x0] = 0;
        }
        b_.blit_anti_h(x0, y, aa, runs);
    }
    void blit_v(uint32_t x, uint32_t y, uint32_t height, uint8_t alpha) override {
        if (!(x - uint32_t(clip_.x) < uint32_t(clip_.w))) return;
        uint32_t y0 = y, y1 = y + height;
        if (y0 < uint32_t(clip_.y)) y0 = uint32_t(clip_.y);
        if (y1 > uint32_t(clip_.bottom())) y1 = uint32_t(clip_.bottom());
        if (y0 < y1) b_.blit_v(x, y0, y1 - y0, alpha);
    }
    void blit_anti_h2(uint32_t x, uint32_t y, uint8_t a0, uint8_t a1) override {
        uint8_t aa[2] = {a0, a1};
        uint16_t runs[3] = {1, 1, 0};
        blit_anti_h(x, y, aa, runs);
    }
    void blit_anti_v2(uint32_t x, uint32_t y, uint8_t a0, uint8_t a1) override {
        uint8_t aa0[1] = {a0};
        uint16_t runs0[2] = {1, 0};
        blit_anti_h(x, y, aa0, runs0);
        uint8_t aa1[1] = {a1};
        uint16_t runs1[2] = {1, 0};
        blit_anti_h(x, y + 1, aa1, runs1);
    }

private:
    Blitter& b_;
    IntRect clip_;

    static uint32_t anti_width(const uint16_t* runs) {
        uint32_t width = 0;
        size_t i = 0;
        while (runs[i] != 0) {
            width += runs[i];
            i += runs[i];
        }
        return width;
    }
};

// A run of `count` pixels at one coverage, in chunks of at most 100.
void call_hline_blitter(uint32_t x, std::optional<uint32_t> y, uint32_t count, uint8_t alpha, Blitter& blitter) {
    constexpr uint32_t HLINE_STACK_BUFFER = 100;
    uint16_t runs[HLINE_STACK_BUFFER + 1] = {};
    uint8_t aa[HLINE_STACK_BUFFER] = {};
    for (;;) {
        aa[0] = alpha;
        const uint32_t n = std::min(count, HLINE_STACK_BUFFER);
        runs[0] = static_cast<uint16_t>(n);
        runs[n] = 0;
        if (y) blitter.blit_anti_h(x, *y, aa, runs);
        x += n;
        if (n >= count || count == 0) break;
        count -= n;
    }
}

uint8_t i32_to_alpha(int32_t a) { return static_cast<uint8_t>(a & 0xFF); }

enum class HairKind { HLine, Horish, VLine, Vertish };

// The per-kind "draw one end" (cap) and "draw the middle" (line) steps.
FDot16 hair_draw_cap(HairKind kind, Blitter& b, uint32_t pos, FDot16 f, FDot16 slope, int32_t mod64) {
    f += fdot16::ONE / 2;
    f = std::max(f, 0);
    const uint32_t i = uint32_t(f >> 16);
    const uint8_t a = i32_to_alpha(f >> 8);
    switch (kind) {
        case HairKind::HLine: {
            uint8_t ma = fdot6::small_scale(a, mod64);
            if (ma != 0) call_hline_blitter(pos, i, 1, ma, b);
            ma = fdot6::small_scale(255 - a, mod64);
            if (ma != 0) call_hline_blitter(pos, i >= 1 ? std::optional<uint32_t>(i - 1) : std::nullopt, 1, ma, b);
            return f - fdot16::ONE / 2;
        }
        case HairKind::Horish:
            b.blit_anti_v2(pos, std::max(i, 1u) - 1, fdot6::small_scale(255 - a, mod64), fdot6::small_scale(a, mod64));
            return f + slope - fdot16::ONE / 2;
        case HairKind::VLine: {
            uint8_t ma = fdot6::small_scale(a, mod64);
            if (ma != 0) b.blit_v(i, pos, 1, ma);
            ma = fdot6::small_scale(255 - a, mod64);
            if (ma != 0) b.blit_v(std::max(i, 1u) - 1, pos, 1, ma);
            return f - fdot16::ONE / 2;
        }
        case HairKind::Vertish:
            b.blit_anti_h2(std::max(i, 1u) - 1, pos, fdot6::small_scale(255 - a, mod64), fdot6::small_scale(a, mod64));
            return f + slope - fdot16::ONE / 2;
    }
    return f;
}

FDot16 hair_draw_line(HairKind kind, Blitter& b, uint32_t pos, uint32_t stop, FDot16 f, FDot16 slope) {
    switch (kind) {
        case HairKind::HLine:
        case HairKind::VLine: {
            if (stop - pos == 0) return f;
            f += fdot16::ONE / 2;
            f = std::max(f, 0);
            const uint32_t i = uint32_t(f >> 16);
            uint8_t a = i32_to_alpha(f >> 8);
            if (kind == HairKind::HLine) {
                if (a != 0) call_hline_blitter(pos, i, stop - pos, a, b);
                a = 255 - a;
                if (a != 0) {
                    call_hline_blitter(pos, i >= 1 ? std::optional<uint32_t>(i - 1) : std::nullopt, stop - pos, a, b);
                }
            } else {
                if (a != 0) b.blit_v(i, pos, stop - pos, a);
                a = 255 - a;
                if (a != 0) b.blit_v(std::max(i, 1u) - 1, pos, stop - pos, a);
            }
            return f - fdot16::ONE / 2;
        }
        case HairKind::Horish:
        case HairKind::Vertish: {
            f += fdot16::ONE / 2;
            do {
                f = std::max(f, 0);
                const uint32_t i = uint32_t(f >> 16);
                const uint8_t a = i32_to_alpha(f >> 8);
                if (kind == HairKind::Horish) {
                    b.blit_anti_v2(pos, std::max(i, 1u) - 1, 255 - a, a);
                } else {
                    b.blit_anti_h2(std::max(i, 1u) - 1, pos, 255 - a, a);
                }
                f += slope;
                pos += 1;
            } while (pos < stop);
            return f - fdot16::ONE / 2;
        }
    }
    return f;
}

// We want the fractional part of ordinate, but multiples of 64 return 64.
int32_t contribution_64(FDot6 ordinate) { return ((ordinate - 1) & 63) + 1; }

void do_anti_hairline(FDot6 x0, FDot6 y0, FDot6 x1, FDot6 y1, std::optional<IntRect> clip_opt, Blitter& blitter) {
    // check for integer NaN (0x80000000) which we can't handle (can't negate it)
    if (x0 == INT32_MIN || y0 == INT32_MIN || x1 == INT32_MIN || y1 == INT32_MIN) return;

    if (std::abs(x1 - x0) > (511 << 6) || std::abs(y1 - y0) > (511 << 6)) {
        // split long lines (shift each half separately to avoid overflow)
        const FDot6 hx = (x0 >> 1) + (x1 >> 1);
        const FDot6 hy = (y0 >> 1) + (y1 >> 1);
        do_anti_hairline(x0, y0, hx, hy, clip_opt, blitter);
        do_anti_hairline(hx, hy, x1, y1, clip_opt, blitter);
        return;
    }

    int32_t scale_start, scale_stop, istart, istop;
    FDot16 fstart, slope;
    HairKind kind;

    if (std::abs(x1 - x0) > std::abs(y1 - y0)) {  // mostly horizontal
        if (x0 > x1) {  // we want to go left-to-right
            std::swap(x0, x1);
            std::swap(y0, y1);
        }
        istart = fdot6::floor(x0);
        istop = fdot6::ceil(x1);
        fstart = fdot6::to_fdot16(y0);
        if (y0 == y1) {  // completely horizontal, take fast case
            slope = 0;
            kind = HairKind::HLine;
        } else {
            slope = fdot16::fast_div(y1 - y0, x1 - x0);
            fstart += (slope * (32 - (x0 & 63)) + 32) >> 6;
            kind = HairKind::Horish;
        }
        if (istop - istart == 1) {  // we are within a single pixel
            scale_start = x1 - x0;
            scale_stop = 0;
        } else {
            scale_start = 64 - (x0 & 63);
            scale_stop = x1 & 63;
        }
        if (clip_opt) {
            const IntRect clip = *clip_opt;
            if (istart >= clip.right() || istop <= clip.x) return;
            if (istart < clip.x) {
                fstart += slope * (clip.x - istart);
                istart = clip.x;
                scale_start = 64;
                if (istop - istart == 1) {
                    scale_start = contribution_64(x1);
                    scale_stop = 0;
                }
            }
            if (istop > clip.right()) {
                istop = clip.right();
                scale_stop = 0;  // so we don't draw this last column
            }
            if (istart == istop) return;
            // now test if our Y values are completely inside the clip
            int32_t top, bottom;
            if (slope >= 0) {  // T2B
                top = fdot16::floor_to_i32(fstart - fdot16::HALF);
                bottom = fdot16::ceil_to_i32(fstart + (istop - istart - 1) * slope + fdot16::HALF);
            } else {  // B2T
                bottom = fdot16::ceil_to_i32(fstart + fdot16::HALF);
                top = fdot16::floor_to_i32(fstart + (istop - istart - 1) * slope - fdot16::HALF);
            }
            top -= 1;
            bottom += 1;
            if (top >= clip.bottom() || bottom <= clip.y) return;
            if (clip.y <= top && clip.bottom() >= bottom) clip_opt.reset();
        }
    } else {  // mostly vertical
        if (y0 > y1) {  // we want to go top-to-bottom
            std::swap(x0, x1);
            std::swap(y0, y1);
        }
        istart = fdot6::floor(y0);
        istop = fdot6::ceil(y1);
        fstart = fdot6::to_fdot16(x0);
        if (x0 == x1) {
            if (y0 == y1) return;  // zero length
            slope = 0;
            kind = HairKind::VLine;
        } else {
            slope = fdot16::fast_div(x1 - x0, y1 - y0);
            fstart += (slope * (32 - (y0 & 63)) + 32) >> 6;
            kind = HairKind::Vertish;
        }
        if (istop - istart == 1) {
            scale_start = y1 - y0;
            scale_stop = 0;
        } else {
            scale_start = 64 - (y0 & 63);
            scale_stop = y1 & 63;
        }
        if (clip_opt) {
            const IntRect clip = *clip_opt;
            if (istart >= clip.bottom() || istop <= clip.y) return;
            if (istart < clip.y) {
                fstart += slope * (clip.y - istart);
                istart = clip.y;
                scale_start = 64;
                if (istop - istart == 1) {
                    scale_start = contribution_64(y1);
                    scale_stop = 0;
                }
            }
            if (istop > clip.bottom()) {
                istop = clip.bottom();
                scale_stop = 0;
            }
            if (istart == istop) return;
            int32_t left, right;
            if (slope >= 0) {  // L2R
                left = fdot16::floor_to_i32(fstart - fdot16::HALF);
                right = fdot16::ceil_to_i32(fstart + (istop - istart - 1) * slope + fdot16::HALF);
            } else {  // R2L
                right = fdot16::ceil_to_i32(fstart + fdot16::HALF);
                left = fdot16::floor_to_i32(fstart + (istop - istart - 1) * slope - fdot16::HALF);
            }
            left -= 1;
            right += 1;
            if (left >= clip.right() || right <= clip.x) return;
            if (clip.x <= left && clip.right() >= right) clip_opt.reset();
        }
    }

    std::optional<RectClipBlitter> clipper;
    Blitter* b = &blitter;
    if (clip_opt) {
        clipper.emplace(blitter, *clip_opt);
        b = &*clipper;
    }
    if (istart < 0 || istop < 0) return;  // tiny-skia asserts these
    uint32_t ustart = uint32_t(istart);
    const uint32_t ustop = uint32_t(istop);
    fstart = hair_draw_cap(kind, *b, ustart, fstart, slope, scale_start);
    ustart += 1;
    const uint32_t full_spans = ustop - ustart - (scale_stop > 0 ? 1u : 0u);
    if (int32_t(full_spans) > 0) fstart = hair_draw_line(kind, *b, ustart, ustart + full_spans, fstart, slope);
    if (scale_stop > 0) hair_draw_cap(kind, *b, ustop - 1, fstart, slope, scale_stop);
}

void anti_hair_line(Point p0, Point p1, const IntRect& clip, Blitter& blitter) {
    constexpr float MAX = 32767.0f;
    const RectF fixed_bounds{-MAX, -MAX, MAX, MAX};
    // AA hairlines draw up to half a pixel outside, so clip to a 1px outset.
    const RectF clip_bounds{float(clip.x) - 1.0f, float(clip.y) - 1.0f, float(clip.right()) + 1.0f,
                            float(clip.bottom()) + 1.0f};
    const Point src[2] = {p0, p1};
    Point pts[2];
    if (!intersect_line(src, fixed_bounds, pts)) return;
    const Point tmp[2] = {pts[0], pts[1]};
    if (!intersect_line(tmp, clip_bounds, pts)) return;

    const FDot6 x0 = fdot6::from_f32(pts[0].x), y0 = fdot6::from_f32(pts[0].y);
    const FDot6 x1 = fdot6::from_f32(pts[1].x), y1 = fdot6::from_f32(pts[1].y);
    auto ir = int_rect_ltrb(fdot6::floor(std::min(x0, x1)) - 1, fdot6::floor(std::min(y0, y1)) - 1,
                            fdot6::ceil(std::max(x0, x1)) + 1, fdot6::ceil(std::max(y0, y1)) + 1);
    if (!ir) return;
    if (!intersect(clip, *ir)) return;
    if (!clip.contains(*ir)) {
        auto sub = intersect(*ir, clip);
        if (sub && sub->x >= 0 && sub->y >= 0) do_anti_hairline(x0, y0, x1, y1, *sub, blitter);
        return;
    }
    do_anti_hairline(x0, y0, x1, y1, std::nullopt, blitter);
}

// hairline::stroke_path_impl for a path made of one move + one line.
void stroke_hairline(const Path& path, const IntRect& clip, Blitter& blitter) {
    const RectF outset{path.bounds.left - 1.0f, path.bounds.top - 1.0f, path.bounds.right + 1.0f,
                       path.bounds.bottom + 1.0f};
    if (!finite_rect(outset)) return;
    auto ibounds = round_out(outset);
    if (!ibounds || !intersect(clip, *ibounds)) return;
    Point last{0, 0};
    size_t pi = 0;
    for (Verb v : path.verbs) {
        if (v == Verb::Move) {
            last = path.points[pi++];
        } else if (v == Verb::Line) {
            anti_hair_line(last, path.points[pi], clip, blitter);
            last = path.points[pi++];
        }
    }
}

// ---------------------------------------------------------------------------
// Tiling (painter.rs DrawTiler): supersampled coordinates must fit in
// 16.16 fixed point, so pixmaps above 8191 px are drawn in tiles.
// ---------------------------------------------------------------------------

constexpr uint32_t MAX_TILE = 8192 - 1;

bool tiling_required(uint32_t w, uint32_t h) { return w > MAX_TILE || h > MAX_TILE; }

// Run `draw(path, clip, blitter)` per tile on a copy of `path` that is moved
// into tile space and back again between tiles, exactly as tiny-skia does
// (the float round trips are part of its result).
template <class Draw>
void for_each_tile(Pixmap& pm, const Path& path_in, Draw draw) {
    Path path = path_in;
    auto translate = [&path](float tx, float ty) {
        if (tx == 0.0f && ty == 0.0f) return true;  // identity transform: no-op
        for (Point& p : path.points) {
            p.x += tx;
            p.y += ty;
        }
        auto b = bounds_of(path.points);
        if (!b) return false;
        path.bounds = *b;
        return true;
    };
    uint32_t x_off = 0, y_off = 0;
    while (x_off < pm.width() && y_off < pm.height()) {
        const uint32_t tw = std::min(pm.width() - x_off, MAX_TILE);
        const uint32_t th = std::min(pm.height() - y_off, MAX_TILE);
        if (!translate(-float(x_off), -float(y_off))) return;
        PixmapBlitter blitter(pm, x_off, y_off, tw, th);
        draw(path, IntRect{0, 0, int32_t(tw), int32_t(th)}, blitter);
        if (!translate(float(x_off), float(y_off))) return;
        x_off += MAX_TILE;
        if (x_off >= pm.width()) {
            x_off = 0;
            y_off += MAX_TILE;
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// PathBuilder
// ---------------------------------------------------------------------------

void PathBuilder::move_to(float x, float y) {
    if (!verbs.empty() && verbs.back() == Verb::Move) {
        points.back() = Point{x, y};
    } else {
        last_move_to_index_ = points.size();
        move_to_required_ = false;
        verbs.push_back(Verb::Move);
        points.push_back(Point{x, y});
    }
}

void PathBuilder::inject_move_to_if_needed() {
    if (move_to_required_) {
        if (last_move_to_index_ < points.size()) {
            const Point p = points[last_move_to_index_];
            move_to(p.x, p.y);
        } else {
            move_to(0.0f, 0.0f);
        }
    }
}

void PathBuilder::line_to(float x, float y) {
    inject_move_to_if_needed();
    verbs.push_back(Verb::Line);
    points.push_back(Point{x, y});
}

void PathBuilder::cubic_to(float x1, float y1, float x2, float y2, float x, float y) {
    inject_move_to_if_needed();
    verbs.push_back(Verb::Cubic);
    points.push_back(Point{x1, y1});
    points.push_back(Point{x2, y2});
    points.push_back(Point{x, y});
}

void PathBuilder::close() {
    // don't add a close if it's the first verb or a repeat
    if (!verbs.empty() && verbs.back() != Verb::Close) verbs.push_back(Verb::Close);
    move_to_required_ = true;
}

std::optional<Path> PathBuilder::finish() {
    if (verbs.empty() || verbs.size() == 1) return std::nullopt;  // empty, or just a move
    auto b = bounds_of(points);
    if (!b) return std::nullopt;
    return Path{std::move(verbs), std::move(points), *b};
}

// ---------------------------------------------------------------------------
// Pixmap
// ---------------------------------------------------------------------------

Pixmap::Pixmap(uint32_t width, uint32_t height) : width_(width), height_(height), rgba_(size_t(width) * height * 4, 0) {}

void Pixmap::fill_path(const Path& path) {
    // Skip empty paths and horizontal/vertical lines.
    if (std::fabs(path.bounds.right - path.bounds.left) <= SCALAR_NEARLY_ZERO ||
        std::fabs(path.bounds.bottom - path.bounds.top) <= SCALAR_NEARLY_ZERO) {
        return;
    }
    // path coordinates too big for the math
    constexpr float MAX = std::numeric_limits<float>::max() * 0.25f;
    const RectF& b = path.bounds;
    if (!(b.left >= -MAX && b.top >= -MAX && b.right <= MAX && b.bottom <= MAX)) return;

    if (tiling_required(width_, height_)) {
        for_each_tile(*this, path, [](const Path& p, const IntRect& clip, Blitter& bl) { fill_path_aa(p, clip, bl); });
    } else {
        PixmapBlitter blitter(*this, 0, 0, width_, height_);
        fill_path_aa(path, IntRect{0, 0, int32_t(width_), int32_t(height_)}, blitter);
    }
}

void Pixmap::fill_int_rect(int left, int top, int right, int bottom) {
    // With whole-pixel edges every covered pixel gets full coverage, so this
    // equals tiny-skia's anti-aliased fill_rect.
    const int l = std::max(left, 0), t = std::max(top, 0);
    const int r = int(std::min<int64_t>(right, width_));
    const int bt = int(std::min<int64_t>(bottom, height_));
    for (int y = t; y < bt; y++) {
        for (int x = l; x < r; x++) {
            uint8_t* px = rgba_.data() + (size_t(y) * width_ + size_t(x)) * 4;
            px[0] = px[1] = px[2] = px[3] = 255;
        }
    }
}

void Pixmap::stroke_line(Point a, Point b, float width) {
    PathBuilder pb;
    pb.move_to(a.x, a.y);
    pb.line_to(b.x, b.y);
    auto path = pb.finish();
    if (!path || width < 0.0f) return;

    // treat_as_hairline: a stroke no wider than a pixel is drawn as an AA
    // hairline (with coverage = width, which is 1 for our >= 1px strokes).
    if (width <= 1.0f) {
        if (tiling_required(width_, height_)) {
            for_each_tile(*this, *path, [](const Path& p, const IntRect& clip, Blitter& bl) { stroke_hairline(p, clip, bl); });
        } else {
            PixmapBlitter blitter(*this, 0, 0, width_, height_);
            stroke_hairline(*path, IntRect{0, 0, int32_t(width_), int32_t(height_)}, blitter);
        }
        return;
    }

    // The stroker's output for one segment with butt caps: a quadrilateral
    // offset by the unit normal times half the width.
    if (!(width > 0.0f) || !std::isfinite(width)) return;
    const float radius = width * 0.5f;
    constexpr float tolerance = SCALAR_NEARLY_ZERO * 0.25f;  // * inv_res_scale
    if (std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance) return;  // teeny line
    // set_normalize, computed in double like tiny-skia, then rotate_ccw
    float ux = b.x - a.x, uy = b.y - a.y;
    const double dmag = std::sqrt(double(ux) * double(ux) + double(uy) * double(uy));
    const double dscale = 1.0 / dmag;
    ux *= static_cast<float>(dscale);
    uy *= static_cast<float>(dscale);
    if (!std::isfinite(ux) || !std::isfinite(uy) || (ux == 0.0f && uy == 0.0f)) return;
    const float nx = uy * radius, ny = -ux * radius;

    PathBuilder q;
    q.move_to(a.x + nx, a.y + ny);
    q.line_to(b.x + nx, b.y + ny);
    q.line_to(b.x - nx, b.y - ny);
    q.line_to(a.x - nx, a.y - ny);
    q.line_to(a.x + nx, a.y + ny);
    q.close();
    if (auto quad = q.finish()) fill_path(*quad);
}

}  // namespace texmath
