// Mermaid port: SVG writer and app-level pipeline (see render.h).
#include "render.h"

#include <cstdlib>
#include <optional>

#include "ir.h"
#include "layout.h"
#include "numfmt.h"
#include "parser.h"
#include "strutil.h"

namespace mermaid {

namespace {

// Value of attribute `name` inside a single tag string (the reference implementation `attr`).
std::optional<std::string> attr(const std::string& tag, const std::string& name) {
    std::string key = " " + name + "=\"";
    size_t p = tag.find(key);
    if (p == std::string::npos) return std::nullopt;
    size_t i = p + key.size();
    size_t j = tag.find('"', i);
    if (j == std::string::npos) return std::nullopt;
    return tag.substr(i, j - i);
}

// Rust `"..".parse::<f32>()` on a whole string (nullopt if it is not a plain number).
std::optional<float> parse_f32(const std::string& s) {
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return std::nullopt;
    return v;
}

}  // namespace

// Port of fix_svg() in the reference implementation: drop the inner ring of double circles, and move
// each subgraph title to the end of the document on a box filled like its cluster.
std::string fix_svg(const std::string& svg) {
    std::string out;
    out.reserve(svg.size() + 256);
    std::string titles;
    std::string_view rest = svg;
    std::optional<std::pair<std::string, std::string>> prev_circle;
    std::optional<std::string> cluster_fill;
    while (true) {
        size_t lt = rest.find('<');
        if (lt == std::string_view::npos) break;
        out.append(rest.substr(0, lt));
        rest = rest.substr(lt);
        size_t gt = rest.find('>');
        if (gt == std::string_view::npos) break;
        std::string tag(rest.substr(0, gt + 1));
        if (starts_with(tag, "<circle")) {
            std::pair<std::string, std::string> c{attr(tag, "cx").value_or(""), attr(tag, "cy").value_or("")};
            bool dup = attr(tag, "fill") == std::optional<std::string>("none") && prev_circle && *prev_circle == c;
            prev_circle = c;
            cluster_fill.reset();
            rest = rest.substr(gt + 1);
            if (!dup) out += tag;
            continue;
        }
        if (starts_with(tag, "<text")) {
            std::optional<std::string> fill = std::move(cluster_fill);  // Option::take()
            cluster_fill.reset();
            size_t end = rest.find("</text>");
            if (fill && end != std::string_view::npos) {
                std::string whole(rest.substr(0, end + 7));
                float x = parse_f32(attr(tag, "x").value_or("")).value_or(0.0f);
                float y = parse_f32(attr(tag, "y").value_or("")).value_or(0.0f);
                float fs = parse_f32(attr(tag, "font-size").value_or("")).value_or(14.0f);
                // Visible characters only (skip nested tags).
                size_t n = 0;
                bool in_tag = false;
                for (size_t i = gt + 1; i < whole.size(); ++i) {
                    char ch = whole[i];
                    if (ch == '<') in_tag = true;
                    else if (ch == '>') in_tag = false;
                    else if (!in_tag && (static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++n;
                }
                float w = static_cast<float>(n) * fs * 0.6f + 8.0f;
                float h = fs * 1.3f;
                titles += "<rect x=\"" + fmt_fixed(x - w / 2.0f, 2) + "\" y=\"" + fmt_fixed(y - fs * 1.0f, 2) +
                          "\" width=\"" + fmt_fixed(w, 2) + "\" height=\"" + fmt_fixed(h, 2) + "\" fill=\"" + *fill +
                          "\" stroke=\"none\"/>" + whole;
                rest = rest.substr(whole.size());
                prev_circle.reset();
                continue;
            }
        }
        // A subgraph box: rounded rect with a stroke, followed by its title.
        if (starts_with(tag, "<rect") && attr(tag, "rx") == std::optional<std::string>("10") && attr(tag, "stroke"))
            cluster_fill = attr(tag, "fill");
        else
            cluster_fill.reset();
        prev_circle.reset();
        out += tag;
        rest = rest.substr(gt + 1);
    }
    out.append(rest);
    if (!titles.empty()) {
        size_t i = out.rfind("</svg>");
        if (i != std::string::npos) out.insert(i, titles);
    }
    return out;
}

std::string render_mermaid_svg(const std::string& source) {
    if (trim_view(source).empty()) throw ParseError("empty diagram");
    Graph graph = parse_mermaid(source);
    Theme theme;
    LayoutConfig config;
    Layout layout = compute_layout(graph, theme, config);
    return fix_svg(render_svg(layout, theme, config));
}


}  // namespace mermaid
