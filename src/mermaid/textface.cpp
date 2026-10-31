// Mermaid port: HarfBuzz-backed font face for drawing text (see textface.h).
#include "textface.h"

#include <cmath>

#include <hb-ot.h>
#include <hb.h>

namespace mermaid {

namespace {

// HarfBuzz calls these while drawing one glyph; `data` is the OutlineSink.
void draw_move_to(hb_draw_funcs_t*, void* data, hb_draw_state_t*, float x, float y, void*) {
    static_cast<OutlineSink*>(data)->move_to(x, y);
}
void draw_line_to(hb_draw_funcs_t*, void* data, hb_draw_state_t*, float x, float y, void*) {
    static_cast<OutlineSink*>(data)->line_to(x, y);
}
void draw_quad_to(hb_draw_funcs_t*, void* data, hb_draw_state_t*, float cx, float cy, float x, float y, void*) {
    static_cast<OutlineSink*>(data)->quad_to(cx, cy, x, y);
}
void draw_cubic_to(hb_draw_funcs_t*, void* data, hb_draw_state_t*, float c1x, float c1y, float c2x, float c2y,
                   float x, float y, void*) {
    static_cast<OutlineSink*>(data)->cubic_to(c1x, c1y, c2x, c2y, x, y);
}
void draw_close(hb_draw_funcs_t*, void* data, hb_draw_state_t*, void*) { static_cast<OutlineSink*>(data)->close(); }

hb_draw_funcs_t* draw_funcs() {
    static hb_draw_funcs_t* funcs = [] {
        hb_draw_funcs_t* f = hb_draw_funcs_create();
        hb_draw_funcs_set_move_to_func(f, draw_move_to, nullptr, nullptr);
        hb_draw_funcs_set_line_to_func(f, draw_line_to, nullptr, nullptr);
        hb_draw_funcs_set_quadratic_to_func(f, draw_quad_to, nullptr, nullptr);
        hb_draw_funcs_set_cubic_to_func(f, draw_cubic_to, nullptr, nullptr);
        hb_draw_funcs_set_close_path_func(f, draw_close, nullptr, nullptr);
        hb_draw_funcs_make_immutable(f);
        return f;
    }();
    return funcs;
}

}  // namespace

TextFace::~TextFace() {
    if (font_) hb_font_destroy(font_);
    if (face_) hb_face_destroy(face_);
    if (blob_) hb_blob_destroy(blob_);
}

bool TextFace::load(std::vector<uint8_t> data) {
    data_ = std::move(data);  // HarfBuzz reads it in place, so it must stay alive and unmoved
    blob_ = hb_blob_create(reinterpret_cast<const char*>(data_.data()), static_cast<unsigned>(data_.size()),
                           HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    face_ = hb_face_create(blob_, 0);
    unsigned upem = hb_face_get_upem(face_);
    if (upem == 0 || hb_face_get_glyph_count(face_) == 0) return false;
    upem_ = static_cast<float>(upem);
    font_ = hb_font_create(face_);
    hb_font_set_scale(font_, static_cast<int>(upem), static_cast<int>(upem));  // positions in font units
    return true;
}

std::vector<ShapedGlyph> TextFace::shape(std::string_view utf8) const {
    std::vector<ShapedGlyph> out;
    if (!font_ || utf8.empty()) return out;
    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, utf8.data(), static_cast<int>(utf8.size()), 0, static_cast<int>(utf8.size()));
    hb_buffer_guess_segment_properties(buf);
    hb_shape(font_, buf, nullptr, 0);
    unsigned n = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
    for (unsigned i = 0; i < n; ++i)
        out.push_back({info[i].codepoint, static_cast<float>(pos[i].x_advance), static_cast<float>(pos[i].x_offset),
                       static_cast<float>(pos[i].y_offset)});
    hb_buffer_destroy(buf);
    return out;
}

float TextFace::ascent() const {
    hb_position_t v = 0;
    if (font_) hb_ot_metrics_get_position(font_, HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER, &v);
    return static_cast<float>(v);
}

float TextFace::descent() const {
    hb_position_t v = 0;
    if (font_) hb_ot_metrics_get_position(font_, HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER, &v);
    return static_cast<float>(v);
}

float TextFace::x_height() const {
    hb_position_t v = 0;
    if (font_) hb_ot_metrics_get_position(font_, HB_OT_METRICS_TAG_X_HEIGHT, &v);
    if (v <= 0) return std::floor((ascent() - descent()) * 0.45f);
    return static_cast<float>(v);
}

void TextFace::outline(uint32_t gid, OutlineSink& sink) const {
    if (font_) hb_font_draw_glyph(font_, gid, draw_funcs(), &sink);
}

}  // namespace mermaid
