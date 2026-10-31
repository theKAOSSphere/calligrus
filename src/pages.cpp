// Where the pages of a document export end (spec 29; mirrors pages.rs function by function).
#include "pages.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace pages {

float export_body_size(int body_size, double zoom) {
    const float pt = float(std::round(body_size * zoom * kPtPerPx * 2.0) / 2.0);
    return std::min(std::max(pt, kBodyPtMin), kBodyPtMax);
}

bool paper_size(const std::string& name, float& width, float& height) {
    std::string n = name;
    for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (n == "a4") return width = 595.3f, height = 841.9f, true;
    if (n == "letter") return width = 612.0f, height = 792.0f, true;
    return false;
}

bool widow_ok(size_t k, size_t n) { return n < 4 || k == n || (k >= 2 && n - k >= 2); }

void inner_places(const Block& b, bool strict, std::vector<float>& out) {
    if (b.kind == Kind::Atomic) return;  // never inside a picture row, display formula, diagram or rule
    const bool widows = strict && (b.kind == Kind::Text || b.kind == Kind::Heading || b.kind == Kind::Code);
    for (size_t g = 0; g < b.groups.size(); g++) {
        const std::vector<float>& group = b.groups[g];
        const size_t n = group.size();
        for (size_t i = 0; i < n; i++) {
            const size_t k = i + 1;  // lines above the break
            const bool block_end = g + 1 == b.groups.size() && k == n;
            if (block_end || (widows && !widow_ok(k, n))) continue;
            out.push_back(group[i]);
        }
    }
}

std::vector<float> allowed_places(const std::vector<Block>& blocks, bool strict) {
    std::vector<float> out;
    for (size_t i = 0; i < blocks.size(); i++) {
        const bool after_heading = i > 0 && blocks[i - 1].kind == Kind::Heading;
        if (i > 0 && !(strict && after_heading)) out.push_back(blocks[i].top);
        inner_places(blocks[i], strict, out);
    }
    return out;
}

bool largest_in(const std::vector<float>& places, float y0, float limit, float& found) {
    bool any = false;
    for (float p : places) {
        if (p > y0 + kEps && p <= limit + kEps && (!any || p > found)) {
            found = p;
            any = true;
        }
    }
    return any;
}

std::vector<float> page_ends(const std::vector<Block>& blocks, float h, float total) {
    if (h <= 0.0f) return {total};  // no room at all: do not loop forever
    const std::vector<float> strict = allowed_places(blocks, true);
    const std::vector<float> relaxed = allowed_places(blocks, false);
    std::vector<float> ends;
    float y0 = 0.0f;
    for (;;) {
        if (total <= y0 + h + kEps) {
            ends.push_back(total);  // the rest fits
            return ends;
        }
        float end = 0.0f;
        if (!(largest_in(strict, y0, y0 + h, end) && end >= y0 + h / 4.0f)) {
            if (!largest_in(relaxed, y0, y0 + h, end)) end = y0 + h;
        }
        ends.push_back(end);
        y0 = end;
    }
}

}  // namespace pages
