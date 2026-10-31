// Mermaid port: app entry point (see native.h).
#include "native.h"

#include <exception>

#include "fontmetrics.h"
#include "ir.h"
#include "render.h"
#include "textface.h"

namespace mermaid {

namespace {
struct State {
    bool ready = false;
    FontMetrics measure;  // layout measures text with the regular face (like the oracle's seeded font)
    TextFace regular, bold, italic;
    bool has_bold = false, has_italic = false;
    std::string reason;
};
State& state() {
    static State s;
    return s;
}
}  // namespace

bool native_init(NativeFonts fonts) {
    State& s = state();
    if (s.ready) return true;
    if (!s.measure.load(fonts.regular)) return false;
    if (!s.regular.load(std::move(fonts.regular))) return false;
    s.has_bold = !fonts.bold.empty() && s.bold.load(std::move(fonts.bold));
    s.has_italic = !fonts.italic.empty() && s.italic.load(std::move(fonts.italic));
    set_measure_font(&s.measure);
    s.ready = true;
    return true;
}

const std::string& native_last_reason() { return state().reason; }

std::optional<InkImage> native_render(const std::string& source, float scale) {
    State& s = state();
    s.reason.clear();
    if (!s.ready) {
        s.reason = "native renderer not initialised";
        return std::nullopt;
    }
    try {
        std::string svg = render_mermaid_svg(source);
        FontSet fonts;
        fonts.regular = &s.regular;
        fonts.bold = s.has_bold ? &s.bold : nullptr;
        fonts.italic = s.has_italic ? &s.italic : nullptr;
        return rasterize_svg(svg, scale, fonts);
    } catch (const Unsupported& e) {
        s.reason = std::string("unsupported: ") + e.what();
    } catch (const ParseError& e) {
        s.reason = std::string("parse error: ") + e.what();
    } catch (const std::exception& e) {
        s.reason = std::string("error: ") + e.what();
    }
    return std::nullopt;
}

}  // namespace mermaid
