// CFF (Compact Font Format) glyph outlines: a Type 2 charstring interpreter.
// Port of ttf-parser's `tables/cff/{cff1,charstring,dict,index}.rs`, so the
// outline points (and the bounding boxes derived from them) are identical.
//
// A CFF charstring is a tiny stack-based program: numbers are pushed, then an
// operator such as `rlineto` consumes them and moves a pen. Subroutines
// (shared snippets) are called by index, with a "bias" added to the index.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

#include "../charconv_compat.h"
#include "font.h"

namespace texmath {

namespace {

// Limits according to the Adobe Technical Note #5177 Appendix B.
constexpr uint8_t STACK_LIMIT = 10;  // subroutine nesting
constexpr size_t MAX_ARGUMENTS_STACK_LEN = 48;
constexpr size_t MAX_OPERANDS_LEN = 48;  // DICT operands

struct CffError {};  // any malformed charstring: the glyph simply has no outline

// Rust's `f32 as i32` / `f64 as i32`: truncate, saturate, NaN -> 0.
int32_t sat_i32(double v) {
    if (std::isnan(v)) return 0;
    if (v <= -2147483648.0) return std::numeric_limits<int32_t>::min();
    if (v >= 2147483647.0) return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(v);
}

// ttf-parser's `i32::try_num_from(f32)`: truncation, but out of range = failure.
std::optional<int32_t> try_i32(float v) {
    constexpr float MIN = -2147483648.0f;
    constexpr float MAX_P1 = 2147483648.0f;
    if (v >= MIN && v < MAX_P1) return static_cast<int32_t>(v);
    return std::nullopt;
}

std::optional<int16_t> try_i16(float v) {
    auto i = try_i32(v);
    if (!i || *i < -32768 || *i > 32767) return std::nullopt;
    return static_cast<int16_t>(*i);
}

// Adobe Technical Note #5176, Chapter 16 "Local / Global Subrs INDEXes"
uint16_t calc_subroutine_bias(uint32_t len) {
    if (len < 1240) return 107;
    if (len < 33900) return 1131;
    return 32768;
}

}  // namespace

// ---------------------------------------------------------------------------
// INDEX: a count, an offset size, count+1 offsets, then the object data.
// ---------------------------------------------------------------------------

std::optional<std::pair<size_t, size_t>> Font::Index::get(const Font& f, uint32_t i) const {
    // Offsets are stored "plus one" in the font.
    auto offset_at = [&](uint32_t k) -> std::optional<uint32_t> {
        if (count == 0 || k > count) return std::nullopt;  // there are count+1 offsets
        const size_t at = offsets + size_t(k) * off_size;
        uint32_t n = 0;
        for (uint8_t b = 0; b < off_size; b++) {
            auto byte = f.u8(at + b);
            if (!byte) return std::nullopt;
            n = (n << 8) | *byte;
        }
        if (n == 0) return std::nullopt;
        return n - 1;
    };
    if (i == std::numeric_limits<uint32_t>::max()) return std::nullopt;
    auto start = offset_at(i), end = offset_at(i + 1);
    if (!start || !end || *start > *end || *end > data_len) return std::nullopt;
    return std::pair{data + *start, data + *end};
}

// Parse an INDEX starting at `pos` (absolute); advances `pos` past it.
bool Font::parse_index(size_t& pos, size_t limit, Index& out) const {
    out = Index{};
    auto count = u16(pos);
    if (!count) return false;
    pos += 2;
    if (*count == 0) return true;
    auto off_size = u8(pos);
    if (!off_size || *off_size < 1 || *off_size > 4) return false;
    pos += 1;
    const size_t offsets_len = (size_t(*count) + 1) * *off_size;
    if (pos + offsets_len > limit) return false;
    out.offsets = pos;
    out.off_size = *off_size;
    out.count = *count;
    pos += offsets_len;
    // The last offset gives the size of the object data.
    uint32_t last = 0;
    for (uint8_t b = 0; b < *off_size; b++) last = (last << 8) | *u8(out.offsets + size_t(*count) * *off_size + b);
    if (last == 0) {
        out = Index{};
        return true;
    }
    last -= 1;
    if (pos + last > limit) return false;
    out.data = pos;
    out.data_len = last;
    pos += last;
    return true;
}

namespace {

// A DICT is a sequence of operands followed by an operator, repeated.
class DictParser {
public:
    DictParser(const std::vector<uint8_t>& d, size_t start, size_t end) : d_(d), offset_(start), end_(end) {}

    // The next operator (two-byte operators are 1200 + second byte), or
    // nullopt at the end / on malformed data.
    std::optional<uint16_t> parse_next() {
        size_t s = offset_;
        operands_offset_ = offset_;
        while (s < end_) {
            const uint8_t b = d_[s++];
            if (is_op(b)) {
                uint16_t op = b;
                if (b == 12) {
                    if (s >= end_) return std::nullopt;
                    op = static_cast<uint16_t>(1200 + d_[s++]);
                }
                offset_ = s;
                return op;
            }
            if (!skip_number(b, s)) return std::nullopt;
        }
        return std::nullopt;
    }

    // Operands of the operator just returned by parse_next.
    bool parse_operands() {
        size_t s = operands_offset_;
        operands_.clear();
        while (s < end_) {
            const uint8_t b = d_[s++];
            if (is_op(b)) break;
            auto v = parse_number(b, s);
            if (!v) return false;
            operands_.push_back(*v);
            if (operands_.size() >= MAX_OPERANDS_LEN) break;
        }
        return true;
    }

    std::optional<double> parse_number_op() {
        if (!parse_operands() || operands_.empty()) return std::nullopt;
        return operands_[0];
    }

    std::optional<size_t> parse_offset() {
        if (!parse_operands() || operands_.size() != 1) return std::nullopt;
        const int32_t v = sat_i32(operands_[0]);
        if (v < 0) return std::nullopt;
        return size_t(v);
    }

    std::optional<std::pair<size_t, size_t>> parse_range() {  // [start, end)
        if (!parse_operands() || operands_.size() != 2) return std::nullopt;
        const int32_t len = sat_i32(operands_[0]);
        const int32_t start = sat_i32(operands_[1]);
        if (len < 0 || start < 0) return std::nullopt;
        return std::pair{size_t(start), size_t(start) + size_t(len)};
    }

    const std::vector<double>& operands() const { return operands_; }

private:
    const std::vector<uint8_t>& d_;
    size_t offset_, end_;
    size_t operands_offset_ = 0;
    std::vector<double> operands_;

    // One-byte CFF DICT operators (Adobe Technical Note #5176, Appendix H).
    static bool is_op(uint8_t b) { return b <= 27 || b == 31 || b == 255; }

    bool skip_number(uint8_t b0, size_t& s) const {
        if (b0 == 28) return (s += 2) <= end_;
        if (b0 == 29) return (s += 4) <= end_;
        if (b0 == 30) {
            while (s < end_) {
                const uint8_t b1 = d_[s++];
                if ((b1 >> 4) == 0xF || (b1 & 0xF) == 0xF) break;
            }
            return true;
        }
        if (b0 >= 32 && b0 <= 246) return true;
        if (b0 >= 247 && b0 <= 254) return (s += 1) <= end_;
        return false;
    }

    std::optional<double> parse_number(uint8_t b0, size_t& s) const {
        auto byte = [&]() -> std::optional<uint8_t> {
            if (s >= end_) return std::nullopt;
            return d_[s++];
        };
        if (b0 == 28) {
            auto a = byte(), b = byte();
            if (!a || !b) return std::nullopt;
            return double(static_cast<int16_t>((*a << 8) | *b));
        }
        if (b0 == 29) {
            uint32_t n = 0;
            for (int i = 0; i < 4; i++) {
                auto b = byte();
                if (!b) return std::nullopt;
                n = (n << 8) | *b;
            }
            return double(static_cast<int32_t>(n));
        }
        if (b0 == 30) return parse_float(s);
        if (b0 >= 32 && b0 <= 246) return double(int(b0) - 139);
        if (b0 >= 247 && b0 <= 250) {
            auto b1 = byte();
            if (!b1) return std::nullopt;
            return double((int(b0) - 247) * 256 + *b1 + 108);
        }
        if (b0 >= 251 && b0 <= 254) {
            auto b1 = byte();
            if (!b1) return std::nullopt;
            return double(-(int(b0) - 251) * 256 - *b1 - 108);
        }
        return std::nullopt;
    }

    // A real number, packed as decimal nibbles (Technical Note #5176, Table 5).
    std::optional<double> parse_float(size_t& s) const {
        std::string text;
        auto nibble = [&](uint8_t n) -> bool {
            if (text.size() >= 64) return false;
            if (n <= 9) {
                text += char('0' + n);
            } else if (n == 10) {
                text += '.';
            } else if (n == 11) {
                text += 'E';
            } else if (n == 12) {
                if (text.size() + 1 >= 64) return false;
                text += "E-";
            } else if (n == 14) {
                text += '-';
            } else {
                return false;
            }
            return true;
        };
        for (;;) {
            if (s >= end_) return std::nullopt;
            const uint8_t b1 = d_[s++];
            if ((b1 >> 4) == 0xF) break;
            if (!nibble(b1 >> 4)) return std::nullopt;
            if ((b1 & 0xF) == 0xF) break;
            if (!nibble(b1 & 0xF)) return std::nullopt;
        }
        double v = 0.0;
        auto res = compat::from_chars_fp(text.data(), text.data() + text.size(), v);
        if (res.ec != std::errc() || res.ptr != text.data() + text.size()) return std::nullopt;
        return v;
    }
};

}  // namespace

bool Font::parse_cff(std::string* error) {
    auto bad = [error](const char* why) {
        if (error) *error = why;
        return false;
    };
    const size_t base = cff_, limit = cff_ + cff_len_;
    auto major = u8(base);
    auto header_size = u8(base + 2);
    if (!major || !header_size || *major != 1) return bad("unsupported CFF version");
    size_t pos = base + 4;
    if (*header_size > 4) pos += *header_size - 4;

    Index name_index, top_index, strings_index;
    if (!parse_index(pos, limit, name_index)) return bad("bad CFF Name INDEX");
    if (!parse_index(pos, limit, top_index)) return bad("bad CFF Top DICT INDEX");
    auto top = top_index.get(*this, 0);
    if (!top) return bad("CFF has no Top DICT");

    // Top DICT: where the charstrings and the Private DICT are.
    size_t char_strings_offset = 0;
    std::optional<std::pair<size_t, size_t>> private_range;
    {
        DictParser dp(data_, top->first, top->second);
        while (auto op = dp.parse_next()) {
            if (*op == 17) {  // CharStrings
                auto o = dp.parse_offset();
                if (!o) return bad("bad CFF CharStrings offset");
                char_strings_offset = *o;
            } else if (*op == 18) {  // Private: size and offset
                private_range = dp.parse_range();
            } else if (*op == 1207) {  // FontMatrix (must parse, though outlines ignore it)
                if (!dp.parse_operands()) return bad("bad CFF FontMatrix");
            } else if (*op == 1230) {  // ROS: a CID-keyed font
                return bad("CID-keyed CFF fonts are not supported");
            }
        }
    }
    if (char_strings_offset == 0) return bad("CFF has no CharStrings");

    if (!parse_index(pos, limit, strings_index)) return bad("bad CFF String INDEX");
    if (!parse_index(pos, limit, global_subrs_)) return bad("bad CFF global subrs");

    size_t cs = base + char_strings_offset;
    if (!parse_index(cs, limit, char_strings_) || char_strings_.count == 0) {
        return bad("bad CFF CharStrings INDEX");
    }

    // Private DICT: the local subroutines (offset relative to the Private DICT).
    if (private_range) {
        const size_t pstart = base + private_range->first, pend = base + private_range->second;
        if (pend > limit || pstart > pend) return bad("bad CFF Private DICT range");
        DictParser dp(data_, pstart, pend);
        std::optional<size_t> subrs;
        while (auto op = dp.parse_next()) {
            if (*op == 19) subrs = dp.parse_offset();
        }
        if (subrs) {
            size_t s = pstart + *subrs;
            if (s > limit) return bad("bad CFF local subrs offset");
            if (!parse_index(s, limit, local_subrs_)) return bad("bad CFF local subrs");
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// The charstring interpreter
// ---------------------------------------------------------------------------

struct CharStringParser {
    const Font& font;
    OutlineSink& sink;

    // Bounding box of every point the pen visits, control points included.
    float x_min = std::numeric_limits<float>::max(), y_min = std::numeric_limits<float>::max();
    float x_max = std::numeric_limits<float>::lowest(), y_max = std::numeric_limits<float>::lowest();

    float stack[MAX_ARGUMENTS_STACK_LEN] = {};
    size_t len = 0;
    float x = 0.0f, y = 0.0f;
    bool has_move_to = false;
    bool is_first_move_to = true;

    bool has_width = false;
    uint32_t stems_len = 0;
    bool has_endchar = false;
    bool has_seac = false;

    void extend(float px, float py) {
        x_min = std::min(x_min, px);
        y_min = std::min(y_min, py);
        x_max = std::max(x_max, px);
        y_max = std::max(y_max, py);
    }
    void move_to(float px, float py) {
        extend(px, py);
        sink.move_to(px, py);
    }
    void line_to(float px, float py) {
        extend(px, py);
        sink.line_to(px, py);
    }
    void curve_to(float x1, float y1, float x2, float y2, float px, float py) {
        extend(x1, y1);
        extend(x2, y2);
        extend(px, py);
        sink.curve_to(x1, y1, x2, y2, px, py);
    }

    void push(float v) {
        if (len == MAX_ARGUMENTS_STACK_LEN) throw CffError{};
        stack[len++] = v;
    }
    float at(size_t i) const { return stack[i]; }
    float pop() {
        if (len == 0) throw CffError{};
        return stack[--len];
    }
    void clear() { len = 0; }
    void reverse() { std::reverse(stack, stack + len); }

    void require_move_to() const {
        if (!has_move_to) throw CffError{};
    }
    void begin_contour() {
        // A new move closes the previous contour.
        if (is_first_move_to) {
            is_first_move_to = false;
        } else {
            sink.close();
        }
        has_move_to = true;
    }

    // ---- path operators (Type 2 charstring spec, section 4.1) ----

    void rmoveto(size_t offset) {  // dx1 dy1
        if (len != offset + 2) throw CffError{};
        begin_contour();
        x += at(offset + 0);
        y += at(offset + 1);
        move_to(x, y);
        clear();
    }
    void hmoveto(size_t offset) {  // dx1
        if (len != offset + 1) throw CffError{};
        begin_contour();
        x += at(offset);
        move_to(x, y);
        clear();
    }
    void vmoveto(size_t offset) {  // dy1
        if (len != offset + 1) throw CffError{};
        begin_contour();
        y += at(offset);
        move_to(x, y);
        clear();
    }
    void rlineto() {  // {dxa dya}+
        require_move_to();
        if (len % 2 != 0) throw CffError{};
        for (size_t i = 0; i < len; i += 2) {
            x += at(i + 0);
            y += at(i + 1);
            line_to(x, y);
        }
        clear();
    }
    // Alternating horizontal and vertical lines; `horizontal_first` picks hlineto/vlineto.
    void hvlineto(bool horizontal_first) {
        require_move_to();
        if (len == 0) throw CffError{};
        size_t i = 0;
        bool horizontal = horizontal_first;
        while (i < len) {
            if (horizontal) {
                x += at(i);
            } else {
                y += at(i);
            }
            i += 1;
            line_to(x, y);
            horizontal = !horizontal;
        }
        clear();
    }
    void rrcurveto() {  // {dxa dya dxb dyb dxc dyc}+
        require_move_to();
        if (len % 6 != 0) throw CffError{};
        for (size_t i = 0; i < len; i += 6) {
            const float x1 = x + at(i + 0);
            const float y1 = y + at(i + 1);
            const float x2 = x1 + at(i + 2);
            const float y2 = y1 + at(i + 3);
            x = x2 + at(i + 4);
            y = y2 + at(i + 5);
            curve_to(x1, y1, x2, y2, x, y);
        }
        clear();
    }
    void rcurveline() {  // {dxa dya dxb dyb dxc dyc}+ dxd dyd
        require_move_to();
        if (len < 8 || (len - 2) % 6 != 0) throw CffError{};
        size_t i = 0;
        while (i < len - 2) {
            const float x1 = x + at(i + 0);
            const float y1 = y + at(i + 1);
            const float x2 = x1 + at(i + 2);
            const float y2 = y1 + at(i + 3);
            x = x2 + at(i + 4);
            y = y2 + at(i + 5);
            curve_to(x1, y1, x2, y2, x, y);
            i += 6;
        }
        x += at(i + 0);
        y += at(i + 1);
        line_to(x, y);
        clear();
    }
    void rlinecurve() {  // {dxa dya}+ dxb dyb dxc dyc dxd dyd
        require_move_to();
        if (len < 8 || (len - 6) % 2 != 0) throw CffError{};
        size_t i = 0;
        while (i < len - 6) {
            x += at(i + 0);
            y += at(i + 1);
            line_to(x, y);
            i += 2;
        }
        const float x1 = x + at(i + 0);
        const float y1 = y + at(i + 1);
        const float x2 = x1 + at(i + 2);
        const float y2 = y1 + at(i + 3);
        x = x2 + at(i + 4);
        y = y2 + at(i + 5);
        curve_to(x1, y1, x2, y2, x, y);
        clear();
    }
    void hhcurveto() {  // dy1? {dxa dxb dyb dxc}+
        require_move_to();
        size_t i = 0;
        if (len % 2 == 1) {  // the odd argument is a Y position
            y += at(0);
            i += 1;
        }
        if ((len - i) % 4 != 0) throw CffError{};
        while (i < len) {
            const float x1 = x + at(i + 0);
            const float y1 = y;
            const float x2 = x1 + at(i + 1);
            const float y2 = y1 + at(i + 2);
            x = x2 + at(i + 3);
            y = y2;
            curve_to(x1, y1, x2, y2, x, y);
            i += 4;
        }
        clear();
    }
    void vvcurveto() {  // dx1? {dya dxb dyb dyc}+
        require_move_to();
        size_t i = 0;
        if (len % 2 == 1) {  // the odd argument is an X position
            x += at(0);
            i += 1;
        }
        if ((len - i) % 4 != 0) throw CffError{};
        while (i < len) {
            const float x1 = x;
            const float y1 = y + at(i + 0);
            const float x2 = x1 + at(i + 1);
            const float y2 = y1 + at(i + 2);
            x = x2;
            y = y2 + at(i + 3);
            curve_to(x1, y1, x2, y2, x, y);
            i += 4;
        }
        clear();
    }
    // One curve that starts horizontal and ends vertical (or the reverse).
    void curve_hv() {
        const float x1 = x + pop();
        const float y1 = y;
        const float x2 = x1 + pop();
        const float y2 = y1 + pop();
        y = y2 + pop();
        x = x2;
        if (len == 1) x += pop();
        curve_to(x1, y1, x2, y2, x, y);
    }
    void curve_vh() {
        const float x1 = x;
        const float y1 = y + pop();
        const float x2 = x1 + pop();
        const float y2 = y1 + pop();
        x = x2 + pop();
        y = y2;
        if (len == 1) y += pop();
        curve_to(x1, y1, x2, y2, x, y);
    }
    // hvcurveto / vhcurveto: alternate the two shapes until the stack is empty.
    void alternating_curves(bool horizontal_first) {
        require_move_to();
        if (len < 4) throw CffError{};
        reverse();
        bool horizontal = horizontal_first;
        while (len != 0) {
            if (len < 4) throw CffError{};
            if (horizontal) {
                curve_hv();
            } else {
                curve_vh();
            }
            horizontal = !horizontal;
        }
    }
    void flex() {  // dx1 dy1 dx2 dy2 dx3 dy3 dx4 dy4 dx5 dy5 dx6 dy6 fd
        require_move_to();
        if (len != 13) throw CffError{};
        const float dx1 = x + at(0), dy1 = y + at(1);
        const float dx2 = dx1 + at(2), dy2 = dy1 + at(3);
        const float dx3 = dx2 + at(4), dy3 = dy2 + at(5);
        const float dx4 = dx3 + at(6), dy4 = dy3 + at(7);
        const float dx5 = dx4 + at(8), dy5 = dy4 + at(9);
        x = dx5 + at(10);
        y = dy5 + at(11);
        curve_to(dx1, dy1, dx2, dy2, dx3, dy3);
        curve_to(dx4, dy4, dx5, dy5, x, y);
        clear();
    }
    void flex1() {  // dx1 dy1 dx2 dy2 dx3 dy3 dx4 dy4 dx5 dy5 d6
        require_move_to();
        if (len != 11) throw CffError{};
        const float dx1 = x + at(0), dy1 = y + at(1);
        const float dx2 = dx1 + at(2), dy2 = dy1 + at(3);
        const float dx3 = dx2 + at(4), dy3 = dy2 + at(5);
        const float dx4 = dx3 + at(6), dy4 = dy3 + at(7);
        const float dx5 = dx4 + at(8), dy5 = dy4 + at(9);
        if (std::fabs(dx5 - x) > std::fabs(dy5 - y)) {
            x = dx5 + at(10);
        } else {
            y = dy5 + at(10);
        }
        curve_to(dx1, dy1, dx2, dy2, dx3, dy3);
        curve_to(dx4, dy4, dx5, dy5, x, y);
        clear();
    }
    void hflex() {  // dx1 dx2 dy2 dx3 dx4 dx5 dx6
        require_move_to();
        if (len != 7) throw CffError{};
        const float dx1 = x + at(0), dy1 = y;
        const float dx2 = dx1 + at(1), dy2 = dy1 + at(2);
        const float dx3 = dx2 + at(3), dy3 = dy2;
        const float dx4 = dx3 + at(4), dy4 = dy2;
        const float dx5 = dx4 + at(5), dy5 = y;
        x = dx5 + at(6);
        curve_to(dx1, dy1, dx2, dy2, dx3, dy3);
        curve_to(dx4, dy4, dx5, dy5, x, y);
        clear();
    }
    void hflex1() {  // dx1 dy1 dx2 dy2 dx3 dx4 dx5 dy5 dx6
        require_move_to();
        if (len != 9) throw CffError{};
        const float dx1 = x + at(0), dy1 = y + at(1);
        const float dx2 = dx1 + at(2), dy2 = dy1 + at(3);
        const float dx3 = dx2 + at(4), dy3 = dy2;
        const float dx4 = dx3 + at(5), dy4 = dy2;
        const float dx5 = dx4 + at(6), dy5 = dy4 + at(7);
        x = dx5 + at(8);
        curve_to(dx1, dy1, dx2, dy2, dx3, dy3);
        curve_to(dx4, dy4, dx5, dy5, x, y);
        clear();
    }

    // If the stack length is odd, the first value is the glyph's width
    // (which we don't need: advances come from hmtx).
    void take_width_if(bool cond) {
        if (cond && !has_width) has_width = true;
    }

    void call_subroutine(const Font::Index& subrs, uint8_t depth) {
        if (len == 0) throw CffError{};
        if (depth == STACK_LIMIT) throw CffError{};
        const uint16_t bias = calc_subroutine_bias(subrs.count);
        auto index = try_i32(pop());
        if (!index) throw CffError{};
        const int64_t i = int64_t(*index) + bias;
        if (i < 0 || i > std::numeric_limits<uint32_t>::max()) throw CffError{};
        auto range = subrs.get(font, static_cast<uint32_t>(i));
        if (!range) throw CffError{};
        run(range->first, range->second, depth + 1);
    }

    void run(size_t start, size_t end, uint8_t depth) {
        size_t s = start;
        auto read_u8 = [&]() -> uint8_t {
            if (s >= end) throw CffError{};
            return font.data_[s++];
        };
        while (s < end) {
            const uint8_t op = read_u8();
            switch (op) {
                case 0: case 2: case 9: case 13: case 15: case 16: case 17:
                    throw CffError{};  // reserved
                case 1: case 3: case 18: case 23: {  // hstem, vstem, hstemhm, vstemhm
                    size_t n = len;
                    if (len % 2 == 1 && !has_width) {
                        has_width = true;
                        n = len - 1;
                    }
                    stems_len += static_cast<uint32_t>(n) >> 1;
                    clear();  // hints are ignored
                    break;
                }
                case 4:  // vmoveto
                    if (len == 2) {
                        take_width_if(true);
                        vmoveto(1);
                    } else {
                        vmoveto(0);
                    }
                    break;
                case 5: rlineto(); break;
                case 6: hvlineto(true); break;   // hlineto
                case 7: hvlineto(false); break;  // vlineto
                case 8: rrcurveto(); break;
                case 10: {  // callsubr (local)
                    if (len == 0) throw CffError{};
                    if (depth == STACK_LIMIT) throw CffError{};
                    if (font.local_subrs_.count == 0 && font.local_subrs_.offsets == 0) throw CffError{};
                    call_subroutine(font.local_subrs_, depth);
                    if (has_endchar && !has_seac) {
                        if (s < end) throw CffError{};  // data after endchar
                        return;
                    }
                    break;
                }
                case 11:  // return
                    return;
                case 12: {  // two-byte operators: the flex family
                    const uint8_t op2 = read_u8();
                    switch (op2) {
                        case 34: hflex(); break;
                        case 35: flex(); break;
                        case 36: hflex1(); break;
                        case 37: flex1(); break;
                        default: throw CffError{};
                    }
                    break;
                }
                case 14: {  // endchar
                    if (len == 4 || (!has_width && len == 5)) {
                        // `seac` (accented character built from two others):
                        // not used by the bundled font, so not supported.
                        throw CffError{};
                    }
                    if (len == 1 && !has_width) {
                        has_width = true;
                        pop();
                    }
                    if (!is_first_move_to) {
                        is_first_move_to = true;
                        sink.close();
                    }
                    if (s < end) throw CffError{};  // data after endchar
                    has_endchar = true;
                    return;
                }
                case 19: case 20: {  // hintmask, cntrmask
                    size_t n = len;
                    clear();
                    if (n % 2 == 1) {
                        n -= 1;
                        if (!has_width) has_width = true;
                    }
                    stems_len += static_cast<uint32_t>(n) >> 1;
                    s += (stems_len + 7) >> 3;  // skip the mask bytes
                    break;
                }
                case 21:  // rmoveto
                    if (len == 3) {
                        take_width_if(true);
                        rmoveto(1);
                    } else {
                        rmoveto(0);
                    }
                    break;
                case 22:  // hmoveto
                    if (len == 2) {
                        take_width_if(true);
                        hmoveto(1);
                    } else {
                        hmoveto(0);
                    }
                    break;
                case 24: rcurveline(); break;
                case 25: rlinecurve(); break;
                case 26: vvcurveto(); break;
                case 27: hhcurveto(); break;
                case 28: {  // a 16-bit integer
                    const uint8_t hi = read_u8();
                    const uint8_t lo = read_u8();
                    push(float(static_cast<int16_t>((hi << 8) | lo)));
                    break;
                }
                case 29:  // callgsubr
                    call_subroutine(font.global_subrs_, depth);
                    if (has_endchar && !has_seac) {
                        if (s < end) throw CffError{};
                        return;
                    }
                    break;
                case 30: alternating_curves(false); break;  // vhcurveto
                case 31: alternating_curves(true); break;   // hvcurveto
                case 255: {  // a 16.16 fixed-point number
                    uint32_t n = 0;
                    for (int k = 0; k < 4; k++) n = (n << 8) | read_u8();
                    push(float(static_cast<int32_t>(n)) / 65536.0f);
                    break;
                }
                default:
                    if (op >= 32 && op <= 246) {
                        push(float(int16_t(op) - 139));
                    } else if (op >= 247 && op <= 250) {
                        const uint8_t b1 = read_u8();
                        push(float((int16_t(op) - 247) * 256 + int16_t(b1) + 108));
                    } else {  // 251..=254
                        const uint8_t b1 = read_u8();
                        push(float(-(int16_t(op) - 251) * 256 - int16_t(b1) - 108));
                    }
                    break;
            }
        }
    }
};

std::optional<GlyphRect> Font::outline_cff(uint16_t gid, OutlineSink& sink) const {
    auto range = char_strings_.get(*this, gid);
    if (!range) return std::nullopt;
    CharStringParser p{*this, sink};
    try {
        p.run(range->first, range->second, 0);
    } catch (const CffError&) {
        return std::nullopt;
    }
    if (!p.has_endchar) return std::nullopt;
    // No points at all (e.g. the space glyph): ttf-parser's "ZeroBBox" error.
    if (p.x_min == std::numeric_limits<float>::max() && p.y_min == std::numeric_limits<float>::max() &&
        p.x_max == std::numeric_limits<float>::lowest() && p.y_max == std::numeric_limits<float>::lowest()) {
        return std::nullopt;
    }
    auto x0 = try_i16(p.x_min), y0 = try_i16(p.y_min), x1 = try_i16(p.x_max), y1 = try_i16(p.y_max);
    if (!x0 || !y0 || !x1 || !y1) return std::nullopt;
    return GlyphRect{*x0, *y0, *x1, *y1};
}

namespace {
struct NullSink : OutlineSink {
    void move_to(float, float) override {}
    void line_to(float, float) override {}
    void curve_to(float, float, float, float, float, float) override {}
    void close() override {}
};
}  // namespace

std::optional<GlyphRect> Font::outline_glyph(uint16_t gid, OutlineSink& sink) const { return outline_cff(gid, sink); }

std::optional<GlyphRect> Font::glyph_bounding_box(uint16_t gid) const {
    NullSink sink;
    return outline_cff(gid, sink);
}

}  // namespace texmath
