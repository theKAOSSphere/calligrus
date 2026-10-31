#include "texmath.h"

#include <cmath>
#include <fstream>
#include <memory>
#include <new>

#include "layout.h"
#include "parse.h"
#include "raster.h"
#include "utf.h"

namespace texmath {

namespace {

// The one shared layouter (Rust keeps it in a `OnceLock`). Set by init_font.
std::unique_ptr<Layouter>& layouter_slot() {
    static std::unique_ptr<Layouter> lay;
    return lay;
}

// Thrown for "formula is too large to draw" (an ordinary error, not a bug).
struct TooLarge {};

Rendered make(const Layouter& lay, const Nodes& nodes, bool display, float ppem, bool capture) {
    const MBox b = lay.layout(nodes, display);
    // The glyphs a PDF draws as text: uniformly scaled ones with a character (not size variants or pieces).
    std::vector<bool> skip(capture ? b.items.size() : 0, false);
    std::vector<std::pair<size_t, char32_t>> chars;
    for (size_t n = 0; n < skip.size(); n++) {
        const auto* g = std::get_if<GlyphItem>(&b.items[n]);
        if (!g || !(std::fabs(g->sx - g->sy) < 1e-4f) || !(g->sx > 0.0f)) continue;
        if (auto c = lay.text_of(g->gid)) skip[n] = true, chars.emplace_back(n, *c);
    }
    auto r = rasterize(lay, b, ppem, skip);
    if (!r) throw TooLarge{};
    Rendered out;
    out.width = r->width;
    out.height = r->height;
    out.rgba = std::move(r->rgba);
    out.origin_x = r->origin_x;
    out.origin_y = r->origin_y;
    out.em_w = b.w;
    out.em_h = b.h;
    out.em_d = b.d;
    for (const auto& [n, c] : chars) {
        const GlyphItem& g = std::get<GlyphItem>(b.items[n]);
        out.glyphs.push_back(MathGlyph{g.gid, to_utf8(c), r->origin_x + g.x * ppem, r->origin_y - g.y * ppem, g.sx * ppem});
    }
    return out;
}

}  // namespace

bool init_font(const std::filesystem::path& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path.string();
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return init_font_data(std::move(bytes), error);
}

bool init_font_data(std::vector<uint8_t> bytes, std::string* error) {
    Font font;
    if (!font.load(std::move(bytes), error)) return false;
    layouter_slot() = std::make_unique<Layouter>(std::move(font));
    return true;
}

bool font_ready() { return layouter_slot() != nullptr; }

Result render(std::string_view tex, bool display, float ppem, bool capture) {
    const Layouter* lay = layouter_slot().get();
    if (!lay) return std::string("math font not loaded");
    // A bug in the engine must never take the whole viewer down: turn any
    // exception into an ordinary error shown in place of the formula
    // (the Rust version does the same with catch_unwind).
    try {
        std::u32string chars;
        if (!decode_utf8(tex, chars)) return std::string("formula is not valid UTF-8");
        const Parsed parsed = parse(chars);
        Equation eq;
        eq.main = make(*lay, parsed.nodes, display, ppem, capture);
        if (parsed.tag) eq.tag = make(*lay, *parsed.tag, false, ppem, capture);
        return eq;
    } catch (const ParseError& e) {
        return std::string(e.what());
    } catch (const TooLarge&) {
        return std::string("formula is too large to draw");
    } catch (...) {
        return std::string("internal error while typesetting");
    }
}

}  // namespace texmath
