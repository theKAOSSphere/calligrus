#include "select.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// Number of UTF-8 code points in `s`.
size_t char_count(const std::string& s) {
    return size_t(std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

// Byte offset of code point number `n` in `s` (or s.size()).
size_t byte_of_char(const std::string& s, size_t n) {
    size_t i = 0;
    for (size_t seen = 0; i < s.size(); i++) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (seen == n) return i;
            seen++;
        }
    }
    return s.size();
}

const Run* run_at(const std::vector<Run>& runs, TextPos p) {
    for (const Run& r : runs)
        if (r.pos.block == p.block && r.pos.tok == p.tok) return &r;
    return nullptr;
}

}  // namespace

std::optional<std::pair<uint32_t, uint32_t>> Run::selected(const TextRange& r) const {
    const auto [a, b] = r;
    const uint32_t n = len();
    const TextPos start = pos, end{pos.block, pos.tok, n};
    if (b <= start || a >= end) return std::nullopt;
    auto same = [&](TextPos p) { return p.block == start.block && p.tok == start.tok; };
    uint32_t c0 = same(a) ? std::min(a.ch, n) : 0;
    uint32_t c1 = same(b) ? std::min(b.ch, n) : n;
    if (c0 >= c1) return std::nullopt;
    return std::pair{c0, c1};
}

std::optional<TextRange> Selection::range() const {
    if (!anchor || !focus || *anchor == *focus) return std::nullopt;
    return *anchor < *focus ? TextRange{*anchor, *focus} : TextRange{*focus, *anchor};
}

void Collector::begin_frame(std::optional<TextRange> range, const std::vector<TextRange>* find, int find_current) {
    runs.clear();
    maths.clear();
    range_ = range;
    find_ = find && !find->empty() ? find : nullptr;
    find_current_ = find_current;
    group_ = 0;
    line_ = 0;
}

// The part of run `r` that `range` covers, as whole-pixel x coordinates.
// Covered up to an edge: fill the whole token box, so neighbouring highlights
// meet without a seam.
static std::optional<std::pair<float, float>> highlight_x(const Run& r, const TextRange& range) {
    auto s = r.selected(range);
    if (!s) return std::nullopt;
    float x0 = s->first == 0 ? r.min.x : r.xs[s->first];
    float x1 = s->second == r.len() ? r.max.x : r.xs[s->second];
    return std::pair{std::round(x0), std::round(x1)};
}

std::optional<std::pair<float, float>> Collector::push(ImVec2 min, ImVec2 max, std::vector<float> xs, std::string text,
                                                       bool hard) {
    Run r{{block_, tok_, 0}, min, max, std::move(xs), std::move(text), group_, sep_, line_, hard};
    r.glued = next_glued;
    next_glued = false;
    tok_++;
    std::optional<std::pair<float, float>> hl;
    if (range_) hl = highlight_x(r, *range_);

    find_hl_.clear();
    if (find_) {
        // Matches are sorted and don't overlap: start at the first one ending after this run's start.
        const TextPos start = r.pos, end{r.pos.block, r.pos.tok, r.len()};
        auto it = std::lower_bound(find_->begin(), find_->end(), start,
                                   [](const TextRange& m, const TextPos& p) { return m.second <= p; });
        for (; it != find_->end() && it->first < end; ++it)
            if (auto x = highlight_x(r, *it)) find_hl_.push_back({x->first, x->second, int(it - find_->begin()) == find_current_});
    }
    runs.push_back(std::move(r));
    return hl;
}

std::optional<TextPos> hit(const std::vector<Run>& runs, ImVec2 p) {
    auto dist = [](float lo, float hi, float v) { return v < lo ? lo - v : v > hi ? v - hi : 0.0f; };
    const Run* best = nullptr;
    std::pair<float, float> best_d{std::numeric_limits<float>::max(), 0.0f};
    for (const Run& r : runs) {
        std::pair<float, float> d{dist(r.min.y, r.max.y, p.y), dist(r.min.x, r.max.x, p.x)};
        if (d < best_d) best_d = d, best = &r;
    }
    if (!best) return std::nullopt;
    uint32_t ch = 0;
    float bd = std::numeric_limits<float>::max();
    for (size_t i = 0; i < best->xs.size(); i++)
        if (float d = std::abs(best->xs[i] - p.x); d < bd) bd = d, ch = uint32_t(i);
    return TextPos{best->pos.block, best->pos.tok, ch};
}

std::optional<TextRange> word_at(const std::vector<Run>& runs, TextPos p) {
    const Run* r = run_at(runs, p);
    if (!r) return std::nullopt;
    return TextRange{r->pos, {r->pos.block, r->pos.tok, r->len()}};
}

std::optional<TextRange> group_at(const std::vector<Run>& runs, TextPos p) {
    const Run* r = run_at(runs, p);
    if (!r) return std::nullopt;
    const Run *first = nullptr, *last = nullptr;
    for (const Run& x : runs)
        if (x.group == r->group) {
            if (!first) first = &x;
            last = &x;
        }
    return TextRange{first->pos, {last->pos.block, last->pos.tok, last->len()}};
}

std::string selected_text(const std::vector<Run>& runs, const TextRange& range) {
    std::string out;
    const Run* prev = nullptr;
    for (const Run& r : runs) {
        auto s = r.selected(range);
        if (!s) continue;
        if (prev) {
            if (r.group != prev->group) {
                switch (r.sep) {
                    case Sep::Para: out += "\n\n"; break;
                    case Sep::Line:
                    case Sep::Row: out += "\n"; break;
                    case Sep::Cell: out += "\t"; break;
                }
            } else if (r.line != prev->line) {
                if (r.hard) out += "\n";
                else if (r.glued) {}  // the same word, cut at a soft wrap: nothing to insert
                else if (!out.empty() && out.back() != ' ' && r.text != " ") out += " ";  // a soft wrap swallowed the space
            }
        }
        if (r.len() != char_count(r.text)) {
            out += r.text;  // an atomic run (equation): one unit on screen, copied whole
        } else {
            size_t b0 = byte_of_char(r.text, s->first), b1 = byte_of_char(r.text, s->second);
            out.append(r.text, b0, b1 - b0);
        }
        prev = &r;
    }
    return out;
}
