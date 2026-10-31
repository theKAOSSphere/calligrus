// Mermaid port of render.rs (flowchart path): the Layout as SVG text, byte-identical to the Rust crate.
// Only the pieces flowcharts use are here (nodes, edges, labels, subgraph boxes, defs).
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "geometry.h"
#include "layout_internal.h"
#include "numfmt.h"
#include "render.h"
#include "strutil.h"
#include "kinds.h"
#include "svg_common.h"
#include "text.h"

namespace mermaid {

namespace {

float fmax_(float a, float b) { return a > b ? a : (a < b ? b : a); }
float fmin_(float a, float b) { return a < b ? a : (a > b ? b : a); }
float clamp_(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::string F(float v) { return fmt_shortest(v); }       // Rust "{}"
std::string F2(float v) { return fmt_fixed(v, 2); }      // Rust "{:.2}"
std::string F3(float v) { return fmt_fixed(v, 3); }      // Rust "{:.3}"

std::string escape_xml(const std::string& in) {
    std::string out;
    for (char c : in) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c;
        }
    }
    return out;
}

// normalize_font_family: drop quotes, join the non-empty families with "," (default sans-serif).
std::string normalize_font_family(const std::string& font_family) {
    std::string out;
    size_t pos = 0;
    while (true) {
        size_t j = font_family.find(',', pos);
        std::string part = font_family.substr(pos, j == std::string::npos ? std::string::npos : j - pos);
        // trim(), then trim_matches('\''), then trim_matches('"')
        std::string t = trim(part);
        size_t a = 0, b = t.size();
        while (a < b && t[a] == '\'') ++a;
        while (b > a && t[b - 1] == '\'') --b;
        t = t.substr(a, b - a);
        a = 0;
        b = t.size();
        while (a < b && t[a] == '"') ++a;
        while (b > a && t[b - 1] == '"') --b;
        t = t.substr(a, b - a);
        if (!t.empty()) {
            if (!out.empty()) out += ",";
            out += t;
        }
        if (j == std::string::npos) break;
        pos = j + 1;
    }
    return out.empty() ? "sans-serif" : out;
}

bool is_divider_line(const std::string& line) { return trim_view(line) == "---"; }

// ---- paths ----

std::vector<Point> dedupe_points(const std::vector<Point>& points) {
    std::vector<Point> out;
    for (const Point& p : points) {
        if (!out.empty() && std::fabs(out.back().first - p.first) < 1e-3f && std::fabs(out.back().second - p.second) < 1e-3f) continue;
        out.push_back(p);
    }
    return out;
}

std::string points_to_path(const std::vector<Point>& points) {
    if (points.empty()) return "";
    auto deduped = dedupe_points(points);
    std::string d = "M " + F3(deduped[0].first) + "," + F3(deduped[0].second);
    for (size_t i = 1; i < deduped.size(); ++i) d += " L " + F3(deduped[i].first) + "," + F3(deduped[i].second);
    return d;
}

// Interior corners become quadratic curves; the first and last segments stay straight.
std::string rounded_polyline_path(const std::vector<Point>& points, float radius) {
    auto pts = dedupe_points(points);
    if (pts.size() <= 2 || radius <= 0.0f) return points_to_path(pts);
    std::string d = "M " + F3(pts[0].first) + "," + F3(pts[0].second);
    for (size_t idx = 1; idx + 1 < pts.size(); ++idx) {
        Point prev = pts[idx - 1], cur = pts[idx], next = pts[idx + 1];
        Point in_vec{cur.first - prev.first, cur.second - prev.second};
        Point out_vec{next.first - cur.first, next.second - cur.second};
        float in_len = std::hypot(in_vec.first, in_vec.second);
        float out_len = std::hypot(out_vec.first, out_vec.second);
        if (in_len <= 1e-3f || out_len <= 1e-3f) continue;
        Point in_unit{in_vec.first / in_len, in_vec.second / in_len};
        Point out_unit{out_vec.first / out_len, out_vec.second / out_len};
        float cross = in_unit.first * out_unit.second - in_unit.second * out_unit.first;
        float dot = in_unit.first * out_unit.first + in_unit.second * out_unit.second;
        if (std::fabs(cross) <= 1e-3f || dot < -0.95f) {
            d += " L " + F3(cur.first) + "," + F3(cur.second);
            continue;
        }
        float trim_len = fmin_(fmin_(radius, in_len * 0.45f), out_len * 0.45f);
        if (trim_len <= 0.5f) {
            d += " L " + F3(cur.first) + "," + F3(cur.second);
            continue;
        }
        Point before{cur.first - in_unit.first * trim_len, cur.second - in_unit.second * trim_len};
        Point after{cur.first + out_unit.first * trim_len, cur.second + out_unit.second * trim_len};
        d += " L " + F3(before.first) + "," + F3(before.second) + " Q " + F3(cur.first) + "," + F3(cur.second) + " " +
             F3(after.first) + "," + F3(after.second);
    }
    Point last = pts.back();
    d += " L " + F3(last.first) + "," + F3(last.second);
    return d;
}

// render.rs basis_curve_path: d3's curveBasis through the points (mindmap edges with control points).
std::string basis_curve_path(const std::vector<Point>& points) {
    auto pts = dedupe_points(points);
    size_t n = pts.size();
    if (n == 0) return "";
    if (n == 1) return "M " + F3(pts[0].first) + "," + F3(pts[0].second);
    if (n == 2) return "M " + F3(pts[0].first) + "," + F3(pts[0].second) + " L " + F3(pts[1].first) + "," + F3(pts[1].second);
    std::string d = "M " + F3(pts[0].first) + "," + F3(pts[0].second);
    Point p0 = pts[0], p1 = pts[1];
    d += " L " + F3((5.0f * p0.first + p1.first) / 6.0f) + "," + F3((5.0f * p0.second + p1.second) / 6.0f);
    float x0 = p0.first, y0 = p0.second, x1 = p1.first, y1 = p1.second;
    for (size_t i = 2; i < n; ++i) {
        float x = pts[i].first, y = pts[i].second;
        d += " C " + F3((2.0f * x0 + x1) / 3.0f) + "," + F3((2.0f * y0 + y1) / 3.0f) + " " + F3((x0 + 2.0f * x1) / 3.0f) + "," +
             F3((y0 + 2.0f * y1) / 3.0f) + " " + F3((x0 + 4.0f * x1 + x) / 6.0f) + "," + F3((y0 + 4.0f * y1 + y) / 6.0f);
        x0 = x1;
        y0 = y1;
        x1 = x;
        y1 = y;
    }
    // closing segment, as d3's lineEnd for `case 3`: another cubic pretending the last point repeats, then a line to it
    d += " C " + F3((2.0f * x0 + x1) / 3.0f) + "," + F3((2.0f * y0 + y1) / 3.0f) + " " + F3((x0 + 2.0f * x1) / 3.0f) + "," + F3((y0 + 2.0f * y1) / 3.0f) +
         " " + F3((x0 + 5.0f * x1) / 6.0f) + "," + F3((y0 + 5.0f * y1) / 6.0f);
    d += " L " + F3(x1) + "," + F3(y1);
    return d;
}

// edge_geometry::edge_endpoint_angle (degrees)
float edge_endpoint_angle(const std::vector<Point>& points, bool start) {
    if (points.size() < 2) return 0.0f;
    Point p0 = start ? points[0] : points[points.size() - 2];
    Point p1 = start ? points[1] : points[points.size() - 1];
    float dx = p1.first - p0.first, dy = p1.second - p0.second;
    return std::atan2(dy, dx) * 57.2957795130823208767981548141051703f;  // f32::to_degrees
}

// ---- label background visibility (render.rs) ----

struct LabelRect {
    float x, y, width, height;
    static LabelRect from_center(float cx, float cy, float lw, float lh, float pad_x, float pad_y) {
        float width = fmax_(lw + pad_x * 2.0f, 0.0f);
        float height = fmax_(lh + pad_y * 2.0f, 0.0f);
        return {cx - width * 0.5f, cy - height * 0.5f, width, height};
    }
};

float lr_orient(Point a, Point b, Point c) {
    return (b.first - a.first) * (c.second - a.second) - (b.second - a.second) * (c.first - a.first);
}
bool lr_on_segment(Point a, Point b, Point c, float eps) {
    return c.first >= fmin_(a.first, b.first) - eps && c.first <= fmax_(a.first, b.first) + eps &&
           c.second >= fmin_(a.second, b.second) - eps && c.second <= fmax_(a.second, b.second) + eps;
}
bool lr_segments_intersect(Point a, Point b, Point c, Point d) {
    float eps = 1e-6f;
    float o1 = lr_orient(a, b, c), o2 = lr_orient(a, b, d), o3 = lr_orient(c, d, a), o4 = lr_orient(c, d, b);
    if (std::fabs(o1) < eps && lr_on_segment(a, b, c, eps)) return true;
    if (std::fabs(o2) < eps && lr_on_segment(a, b, d, eps)) return true;
    if (std::fabs(o3) < eps && lr_on_segment(c, d, a, eps)) return true;
    if (std::fabs(o4) < eps && lr_on_segment(c, d, b, eps)) return true;
    return ((o1 > 0.0f) != (o2 > 0.0f)) && ((o3 > 0.0f) != (o4 > 0.0f));
}
bool lr_point_in_rect(Point p, const LabelRect& r) {
    return p.first >= r.x && p.first <= r.x + r.width && p.second >= r.y && p.second <= r.y + r.height;
}
bool lr_segment_intersects_rect(Point a, Point b, const LabelRect& r) {
    if (lr_point_in_rect(a, r) || lr_point_in_rect(b, r)) return true;
    Point c[4] = {{r.x, r.y}, {r.x + r.width, r.y}, {r.x + r.width, r.y + r.height}, {r.x, r.y + r.height}};
    for (int i = 0; i < 4; ++i)
        if (lr_segments_intersect(a, b, c[i], c[(i + 1) % 4])) return true;
    return false;
}
float lr_point_rect_distance(Point p, const LabelRect& r) {
    float x1 = r.x, y1 = r.y, x2 = r.x + r.width, y2 = r.y + r.height;
    float dx = p.first < x1 ? x1 - p.first : (p.first > x2 ? p.first - x2 : 0.0f);
    float dy = p.second < y1 ? y1 - p.second : (p.second > y2 ? p.second - y2 : 0.0f);
    return std::sqrt(dx * dx + dy * dy);
}
float lr_point_segment_distance(Point point, Point a, Point b) {
    float abx = b.first - a.first, aby = b.second - a.second;
    float len_sq = abx * abx + aby * aby;
    if (len_sq <= 1e-9f) {
        float dx = point.first - a.first, dy = point.second - a.second;
        return std::sqrt(dx * dx + dy * dy);
    }
    float t = ((point.first - a.first) * abx + (point.second - a.second) * aby) / len_sq;
    t = clamp_(t, 0.0f, 1.0f);
    float px = a.first + abx * t, py = a.second + aby * t;
    float dx = point.first - px, dy = point.second - py;
    return std::sqrt(dx * dx + dy * dy);
}
float lr_segment_rect_gap(Point a, Point b, const LabelRect& r) {
    if (lr_segment_intersects_rect(a, b, r)) return 0.0f;
    float best = fmin_(lr_point_rect_distance(a, r), lr_point_rect_distance(b, r));
    Point corners[4] = {{r.x, r.y}, {r.x + r.width, r.y}, {r.x + r.width, r.y + r.height}, {r.x, r.y + r.height}};
    for (const Point& c : corners) best = fmin_(best, lr_point_segment_distance(c, a, b));
    return best;
}
float polyline_rect_gap(const std::vector<Point>& points, const LabelRect& r) {
    if (points.size() < 2) return std::numeric_limits<float>::infinity();
    float best = std::numeric_limits<float>::infinity();
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        float dist = lr_segment_rect_gap(points[i], points[i + 1], r);
        best = fmin_(best, dist);
        if (best <= 1e-6f) return 0.0f;
    }
    return best;
}

enum class EdgeLabelKind { Center, Start, End };

// edge_label_background_visible(): is the label close enough to its edge to deserve a background box?
bool edge_label_background_visible(DiagramKind diagram, EdgeLabelKind kind, const std::vector<Point>& points, const LabelRect& rect) {
    if (points.size() < 2 || rect.width <= 0.0f || rect.height <= 0.0f) return true;
    float gap = polyline_rect_gap(points, rect);
    if (kind == EdgeLabelKind::Center) {
        float limit;
        switch (diagram) {
        case DiagramKind::Flowchart: limit = 1.2f; break;
        case DiagramKind::Sequence: limit = clamp_(rect.height * 0.16f, 1.2f, 2.4f); break;
        case DiagramKind::Requirement: limit = 1.0f; break;
        default: limit = 0.9f; break;
        }
        return gap <= limit;
    }
    switch (diagram) {
    case DiagramKind::Sequence: return gap <= clamp_(rect.height * 0.12f, 0.6f, 1.4f);
    case DiagramKind::Flowchart:
    case DiagramKind::Requirement: return gap <= 0.35f;
    default: return false;
    }
}

// ---- text ----

// text_block_svg_with_font_size_weight: one <text> with a <tspan> per line (divider lines are empty).
std::string text_block_svg_with_font_size_weight(float x, float y, const TextBlock& label, const Theme& theme,
                                                 const LayoutConfig& config, float font_size, const std::string& anchor,
                                                 const std::optional<std::string>& override_color,
                                                 const std::optional<std::string>& font_weight, bool baseline) {
    float total_height = static_cast<float>(label.lines.size()) * font_size * config.label_line_height;
    float start_y = baseline ? y : y - total_height / 2.0f + font_size;
    const std::string& fill = override_color ? *override_color : theme.primary_text_color;
    std::string weight_attr;
    if (font_weight && !trim_view(*font_weight).empty()) weight_attr = " font-weight=\"" + *font_weight + "\"";
    std::string text = "<text x=\"" + F2(x) + "\" y=\"" + F2(start_y) + "\" text-anchor=\"" + anchor + "\" font-family=\"" +
                       normalize_font_family(theme.font_family) + "\" font-size=\"" + F(font_size) + "\" fill=\"" + fill + "\"" +
                       weight_attr + ">";
    float line_height = font_size * config.label_line_height;
    for (size_t idx = 0; idx < label.lines.size(); ++idx) {
        float dy = idx == 0 ? 0.0f : line_height;
        std::string rendered = is_divider_line(label.lines[idx]) ? std::string() : escape_xml(label.lines[idx]);
        text += "<tspan x=\"" + F2(x) + "\" dy=\"" + F2(dy) + "\">" + rendered + "</tspan>";
    }
    text += "</text>";
    return text;
}

std::string text_block_svg_with_font_size(float x, float y, const TextBlock& label, const Theme& theme, const LayoutConfig& config,
                                          float font_size, const std::string& anchor,
                                          const std::optional<std::string>& override_color, bool baseline) {
    return text_block_svg_with_font_size_weight(x, y, label, theme, config, font_size, anchor, override_color, std::nullopt, baseline);
}

std::string text_block_svg_anchor(float x, float y, const TextBlock& label, const Theme& theme, const LayoutConfig& config,
                                  const std::string& anchor, const std::optional<std::string>& override_color) {
    return text_block_svg_with_font_size(x, y, label, theme, config, theme.font_size, anchor, override_color, false);
}

std::string text_block_svg(float x, float y, const TextBlock& label, const Theme& theme, const LayoutConfig& config,
                           const std::optional<std::string>& override_color) {
    return text_block_svg_anchor(x, y, label, theme, config, "middle", override_color);
}

// text_lines_svg: the selected lines (original index, text) as one <text>; dy follows the original indices.
using IndexedLine = std::pair<size_t, std::string>;
std::string text_lines_svg(const std::vector<IndexedLine>& lines, float x, float start_y, float line_height, const std::string& anchor,
                           const Theme& theme, const std::string& fill, std::optional<size_t> bold_line) {
    if (lines.empty()) return "";
    float first_y = start_y + static_cast<float>(lines[0].first) * line_height;
    std::string text = "<text x=\"" + F2(x) + "\" y=\"" + F2(first_y) + "\" text-anchor=\"" + anchor + "\" font-family=\"" +
                       normalize_font_family(theme.font_family) + "\" font-size=\"" + F(theme.font_size) + "\" fill=\"" + fill + "\">";
    size_t prev_idx = lines[0].first;
    for (size_t pos = 0; pos < lines.size(); ++pos) {
        float dy = pos == 0 ? 0.0f : static_cast<float>(lines[pos].first - prev_idx) * line_height;
        std::string weight = bold_line && *bold_line == pos ? " font-weight=\"600\"" : "";
        text += "<tspan x=\"" + F2(x) + "\" dy=\"" + F2(dy) + "\"" + weight + ">" + escape_xml(lines[pos].second) + "</tspan>";
        prev_idx = lines[pos].first;
    }
    text += "</text>";
    return text;
}

// text_block_svg_class: a class box label with "---" divider lines: title centred (last title line bold),
// members left aligned.
std::string text_block_svg_class(const NodeLayout& node, const Theme& theme, const LayoutConfig& config,
                                 const std::optional<std::string>& override_color) {
    float line_height = theme.font_size * config.class_label_line_height();
    float total_height = static_cast<float>(node.label.lines.size()) * line_height;
    float start_y = node.y + node.height / 2.0f - total_height / 2.0f + theme.font_size;
    float center_x = node.x + node.width / 2.0f;
    float left_x = node.x + fmax_(config.node_padding_x, 10.0f);
    const std::string& fill = override_color ? *override_color : theme.primary_text_color;
    std::optional<size_t> divider_idx;
    for (size_t i = 0; i < node.label.lines.size(); ++i)
        if (is_divider_line(node.label.lines[i])) { divider_idx = i; break; }
    if (!divider_idx) {
        std::vector<IndexedLine> lines;
        for (size_t i = 0; i < node.label.lines.size(); ++i) lines.push_back({i, node.label.lines[i]});
        return text_lines_svg(lines, center_x, start_y, line_height, "middle", theme, fill, std::nullopt);
    }
    std::vector<IndexedLine> title_lines, member_lines;
    for (size_t i = 0; i < *divider_idx; ++i)
        if (!trim_view(node.label.lines[i]).empty()) title_lines.push_back({i, node.label.lines[i]});
    for (size_t i = *divider_idx + 1; i < node.label.lines.size(); ++i)
        if (!trim_view(node.label.lines[i]).empty() && !is_divider_line(node.label.lines[i])) member_lines.push_back({i, node.label.lines[i]});
    std::string svg;
    if (!title_lines.empty())
        svg += text_lines_svg(title_lines, center_x, start_y, line_height, "middle", theme, fill, title_lines.size() - 1);
    if (!member_lines.empty()) svg += text_lines_svg(member_lines, left_x, start_y, line_height, "start", theme, fill, std::nullopt);
    return svg;
}

std::string divider_lines_svg(const NodeLayout& node, const Theme& theme, float line_height) {
    bool any = false;
    for (const auto& l : node.label.lines)
        if (is_divider_line(l)) any = true;
    if (!any) return "";
    float total_height = static_cast<float>(node.label.lines.size()) * line_height;
    float start_y = node.y + node.height / 2.0f - total_height / 2.0f + theme.font_size;
    const std::string& stroke = node.style.stroke ? *node.style.stroke : theme.primary_border_color;
    float x1 = node.x + 6.0f, x2 = node.x + node.width - 6.0f;
    std::string svg;
    for (size_t idx = 0; idx < node.label.lines.size(); ++idx) {
        if (!is_divider_line(node.label.lines[idx])) continue;
        float baseline_y = start_y + static_cast<float>(idx) * line_height;
        float y = baseline_y - theme.font_size * 0.35f;
        svg += "<line x1=\"" + F2(x1) + "\" y1=\"" + F2(y) + "\" x2=\"" + F2(x2) + "\" y2=\"" + F2(y) + "\" stroke=\"" + stroke +
               "\" stroke-width=\"1.0\"/>";
    }
    return svg;
}

// ---- shapes ----

std::string shape_svg(const NodeLayout& node, const Theme& theme, const LayoutConfig& config) {
    const std::string& stroke = node.style.stroke ? *node.style.stroke : theme.primary_border_color;
    const std::string& fill = node.style.fill ? *node.style.fill : theme.primary_color;
    std::string dash = node.style.stroke_dasharray ? " stroke-dasharray=\"" + *node.style.stroke_dasharray + "\"" : "";
    const std::string join = " stroke-linejoin=\"round\" stroke-linecap=\"round\"";
    float x = node.x, y = node.y, w = node.width, h = node.height;
    std::string sw = F(node.style.stroke_width ? *node.style.stroke_width : 1.0f);
    auto rect = [&](const std::string& rx) {
        return "<rect x=\"" + F2(x) + "\" y=\"" + F2(y) + "\" width=\"" + F2(w) + "\" height=\"" + F2(h) + "\" rx=\"" + rx +
               "\" ry=\"" + rx + "\" fill=\"" + fill + "\" stroke=\"" + stroke + "\" stroke-width=\"" + sw + "\"" + dash + join + "/>";
    };
    auto polygon = [&](const std::vector<Point>& pts) {
        std::string points;
        for (size_t i = 0; i < pts.size(); ++i) points += (i ? " " : "") + F2(pts[i].first) + "," + F2(pts[i].second);
        return "<polygon points=\"" + points + "\" fill=\"" + fill + "\" stroke=\"" + stroke + "\" stroke-width=\"" + sw + "\"" + dash +
               join + "/>";
    };
    switch (node.shape) {
    case NodeShape::Rectangle:
    case NodeShape::ActorBox: return rect("3");
    case NodeShape::ForkJoin: return rect("2");
    case NodeShape::Diamond: {
        float cx = x + w / 2.0f, cy = y + h / 2.0f;
        return polygon({{cx, y}, {x + w, cy}, {cx, y + h}, {x, cy}});
    }
    case NodeShape::Circle:
    case NodeShape::DoubleCircle: {
        bool label_empty = true;
        for (const auto& l : node.label.lines)
            if (!trim_view(l).empty()) label_empty = false;
        bool is_state_start = starts_with(node.id, "__start_");
        bool is_state_end = starts_with(node.id, "__end_");
        std::string circle_fill, circle_stroke;
        if (is_state_start) { circle_fill = theme.line_color; circle_stroke = theme.line_color; }
        else if (is_state_end) { circle_fill = theme.primary_border_color; circle_stroke = theme.primary_border_color; }
        else if (label_empty) {
            if (node.shape == NodeShape::Circle) { circle_fill = theme.primary_text_color; circle_stroke = theme.primary_text_color; }
            else { circle_fill = theme.primary_border_color; circle_stroke = theme.background; }
        } else { circle_fill = fill; circle_stroke = stroke; }
        float cx = x + w / 2.0f, cy = y + h / 2.0f;
        float r = fmin_(w, h) / 2.0f;
        std::string svg = "<circle cx=\"" + F2(cx) + "\" cy=\"" + F2(cy) + "\" r=\"" + F2(r) + "\" fill=\"" + circle_fill +
                          "\" stroke=\"" + circle_stroke + "\" stroke-width=\"" + sw + "\"" + dash + join + "/>";
        if (node.shape == NodeShape::DoubleCircle) {
            float r2 = r - 4.0f;
            if (r2 > 0.0f) {
                bool filled = label_empty || is_state_end;
                std::string inner_fill = filled ? theme.background : "none";
                std::string inner_stroke = filled ? theme.background : circle_stroke;
                std::string inner_sw = filled ? "1.2" : "1";
                svg += "<circle cx=\"" + F2(cx) + "\" cy=\"" + F2(cy) + "\" r=\"" + F2(r2) + "\" fill=\"" + inner_fill + "\" stroke=\"" +
                       inner_stroke + "\" stroke-width=\"" + inner_sw + "\"" + join + "/>";
            }
        }
        return svg;
    }
    case NodeShape::Stadium: return rect(F2(h / 2.0f));
    case NodeShape::RoundRect: return rect("10");
    case NodeShape::Cylinder: {
        float cx = x + w / 2.0f;
        float ry = clamp_(h * 0.12f, 6.0f, 14.0f);
        float rx = w / 2.0f;
        std::string svg = "<ellipse cx=\"" + F2(cx) + "\" cy=\"" + F2(y + ry) + "\" rx=\"" + F2(rx) + "\" ry=\"" + F2(ry) + "\" fill=\"" +
                          fill + "\" stroke=\"" + stroke + "\" stroke-width=\"" + sw + "\"" + dash + join + "/>";
        svg += "<rect x=\"" + F2(x) + "\" y=\"" + F2(y + ry) + "\" width=\"" + F2(w) + "\" height=\"" + F2(fmax_(h - 2.0f * ry, 0.0f)) +
               "\" fill=\"" + fill + "\" stroke=\"" + stroke + "\" stroke-width=\"" + sw + "\"" + dash + join + "/>";
        svg += "<ellipse cx=\"" + F2(cx) + "\" cy=\"" + F2(y + h - ry) + "\" rx=\"" + F2(rx) + "\" ry=\"" + F2(ry) +
               "\" fill=\"none\" stroke=\"" + stroke + "\" stroke-width=\"" + sw + "\"" + dash + join + "/>";
        return svg;
    }
    case NodeShape::Subroutine: {
        float inset = 6.0f;
        std::string svg = rect("6");
        float y1 = y + 2.0f, y2 = y + h - 2.0f, x1 = x + inset, x2 = x + w - inset;
        svg += "<line x1=\"" + F2(x1) + "\" y1=\"" + F2(y1) + "\" x2=\"" + F2(x1) + "\" y2=\"" + F2(y2) + "\" stroke=\"" + stroke +
               "\" stroke-width=\"" + sw + "\"" + join + "/>";
        svg += "<line x1=\"" + F2(x2) + "\" y1=\"" + F2(y1) + "\" x2=\"" + F2(x2) + "\" y2=\"" + F2(y2) + "\" stroke=\"" + stroke +
               "\" stroke-width=\"" + sw + "\"" + join + "/>";
        return svg;
    }
    case NodeShape::Hexagon: {
        float x1 = x + w * 0.25f, x2 = x + w * 0.75f, y_mid = y + h / 2.0f;
        return polygon({{x1, y}, {x2, y}, {x + w, y_mid}, {x2, y + h}, {x1, y + h}, {x, y_mid}});
    }
    case NodeShape::Parallelogram:
    case NodeShape::ParallelogramAlt: {
        float offset = w * 0.18f;
        if (node.shape == NodeShape::Parallelogram) return polygon({{x + offset, y}, {x + w, y}, {x + w - offset, y + h}, {x, y + h}});
        return polygon({{x, y}, {x + w - offset, y}, {x + w, y + h}, {x + offset, y + h}});
    }
    case NodeShape::Trapezoid:
    case NodeShape::TrapezoidAlt: {
        float offset = w * 0.18f;
        if (node.shape == NodeShape::Trapezoid) return polygon({{x + offset, y}, {x + w - offset, y}, {x + w, y + h}, {x, y + h}});
        return polygon({{x, y}, {x + w, y}, {x + w - offset, y + h}, {x + offset, y + h}});
    }
    case NodeShape::Asymmetric: {
        float slant = w * 0.22f;
        return polygon({{x, y}, {x + w - slant, y}, {x + w, y + h / 2.0f}, {x + w - slant, y + h}, {x, y + h}});
    }
    case NodeShape::MindmapDefault: {
        // a rounded box with a coloured line along its bottom edge
        float rd = fmax_(config.mindmap.default_corner_radius, 0.0f);
        float inner_h = fmax_(h - 2.0f * rd, 0.0f);
        float inner_w = fmax_(w - 2.0f * rd, 0.0f);
        std::string rds = F2(rd);
        std::string rect_path = "M" + F2(x) + " " + F2(y + h - rd) + " v" + F2(-inner_h) + " q0,-" + rds + " " + rds + ",-" + rds + " h" + F2(inner_w) +
                                " q" + rds + ",0 " + rds + "," + rds + " v" + F2(inner_h) + " q0," + rds + " -" + rds + "," + rds + " h" + F2(-inner_w) +
                                " q-" + rds + ",0 -" + rds + ",-" + rds + " Z";
        float stroke_width = node.style.stroke_width ? *node.style.stroke_width : 1.0f;
        std::string svg = "<path d=\"" + rect_path + "\" fill=\"" + fill + "\" stroke=\"" + stroke + "\" stroke-width=\"" + F(stroke_width) + "\"" + dash + join + "/>";
        const std::string& line_color = node.style.line_color ? *node.style.line_color : stroke;
        float line_y = y + h - fmax_(stroke_width, 0.8f);
        svg += "<line x1=\"" + F2(x) + "\" y1=\"" + F2(line_y) + "\" x2=\"" + F2(x + w) + "\" y2=\"" + F2(line_y) + "\" stroke=\"" + line_color +
               "\" stroke-width=\"" + F2(config.mindmap.divider_line_width) + "\" stroke-opacity=\"0.35\"/>";
        return svg;
    }
    default:
        return rect("6");
    }
}

std::string link_attrs(const NodeLink& link) {
    std::string url = escape_xml(link.url);
    std::string attrs = "href=\"" + url + "\" xlink:href=\"" + url + "\"";
    if (link.target) {
        std::string target = escape_xml(*link.target);
        attrs += " target=\"" + target + "\"";
        if (target == "_blank") attrs += " rel=\"noopener noreferrer\"";
    }
    return attrs;
}

std::string arrowhead_svg(Point point, float angle_deg, const std::string& stroke, float stroke_width) {
    float size = clamp_(stroke_width * 1.5f + 4.5f, 5.5f, 10.0f);
    float half = size * 0.52f;
    auto [x, y] = point;
    return "<g transform=\"translate(" + F2(x) + " " + F2(y) + ") rotate(" + F2(angle_deg) + ")\"><polygon points=\"0,0 " + F2(-size) + "," +
           F2(half) + " " + F2(-size) + "," + F2(-half) + "\" fill=\"" + stroke + "\" stroke=\"" + stroke + "\" stroke-width=\"" +
           F(stroke_width) + "\" stroke-linejoin=\"round\" stroke-linecap=\"round\"/></g>";
}

std::string edge_decoration_svg(Point point, float angle_deg, EdgeDecoration decoration, const std::string& stroke, float stroke_width,
                                bool at_start) {
    auto [x, y] = point;
    float angle = angle_deg;
    if ((decoration == EdgeDecoration::Diamond || decoration == EdgeDecoration::DiamondFilled) && !at_start) angle += 180.0f;
    const std::string join = " stroke-linejoin=\"round\" stroke-linecap=\"round\"";
    const std::string sw = F(stroke_width);
    std::string shape;
    switch (decoration) {
    case EdgeDecoration::Circle:
        shape = "<circle cx=\"0\" cy=\"0\" r=\"5\" fill=\"none\" stroke=\"" + stroke + "\" stroke-width=\"" + sw + "\"/>";
        break;
    case EdgeDecoration::Cross:
        shape = "<path d=\"M -5 -5 L 5 5 M -5 5 L 5 -5\" fill=\"none\" stroke=\"" + stroke + "\" stroke-width=\"" + sw + "\"" + join + "/>";
        break;
    case EdgeDecoration::Diamond:
        shape = "<polygon points=\"0,0 9,6 18,0 9,-6\" fill=\"none\" stroke=\"" + stroke + "\" stroke-width=\"" + sw + "\"" + join + "/>";
        break;
    case EdgeDecoration::DiamondFilled:
        shape = "<polygon points=\"0,0 9,6 18,0 9,-6\" fill=\"" + stroke + "\" stroke=\"" + stroke + "\" stroke-width=\"" + sw + "\"" + join + "/>";
        break;
    case EdgeDecoration::CrowsFootOne:
        shape = "<path d=\"M 0 -6 L 0 6 M 5 -6 L 5 6\" fill=\"none\" stroke=\"" + stroke + "\" stroke-width=\"" + sw + "\"" + join + "/>";
        break;
    case EdgeDecoration::CrowsFootZeroOne:
        shape = "<g><circle cx=\"-4\" cy=\"0\" r=\"4\" fill=\"none\" stroke=\"" + stroke + "\" stroke-width=\"" + sw +
                "\"/><path d=\"M 4 -6 L 4 6\" fill=\"none\" stroke=\"" + stroke + "\" stroke-width=\"" + sw + "\"" + join + "/></g>";
        break;
    case EdgeDecoration::CrowsFootMany:
        shape = "<path d=\"M 0 -6 L 0 6 M 0 0 L 8 -6 M 0 0 L 8 6\" fill=\"none\" stroke=\"" + stroke + "\" stroke-width=\"" + sw + "\"" + join + "/>";
        break;
    case EdgeDecoration::CrowsFootZeroMany:
        shape = "<g><circle cx=\"-4\" cy=\"0\" r=\"4\" fill=\"none\" stroke=\"" + stroke + "\" stroke-width=\"" + sw +
                "\"/><path d=\"M 4 0 L 12 -6 M 4 0 L 12 6\" fill=\"none\" stroke=\"" + stroke + "\" stroke-width=\"" + sw + "\"" + join + "/></g>";
        break;
    }
    return "<g transform=\"translate(" + F2(x) + " " + F2(y) + ") rotate(" + F2(angle) + ")\">" + shape + "</g>";
}

// ---- sequence diagrams (render.rs: the `is_sequence` branches of render_svg_with_dimensions) ----

constexpr float SEQUENCE_VIEWBOX_PAD_LEFT = 50.0f;
constexpr float SEQUENCE_VIEWBOX_PAD_RIGHT = 50.0f;
constexpr float SEQUENCE_VIEWBOX_PAD_TOP = 10.0f;
constexpr float SEQUENCE_VIEWBOX_PAD_BOTTOM = 11.0f;

// label_placement::edge_endpoint_label_position: fallback spot for a start/end label.
std::optional<Point> edge_endpoint_label_position(const EdgeLayout& edge, bool start, float offset) {
    if (edge.points.size() < 2) return std::nullopt;
    Point p0, p1;
    if (start) { p0 = edge.points[0]; p1 = edge.points[1]; }
    else { p0 = edge.points.back(); p1 = edge.points[edge.points.size() - 2]; }
    float dx = p1.first - p0.first, dy = p1.second - p0.second;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 1.1920929e-7f) return std::nullopt;
    float dir_x = dx / len, dir_y = dy / len;
    float base_x = p0.first + dir_x * offset * 1.4f, base_y = p0.second + dir_y * offset * 1.4f;
    float perp_x = -dir_y, perp_y = dir_x;
    return Point{base_x + perp_x * offset, base_y + perp_y * offset};
}

std::string text_line_svg(float x, float y, const std::string& text, const Theme& theme, const std::string& fill,
                          const std::string& anchor) {
    return "<text x=\"" + F2(x) + "\" y=\"" + F2(y) + "\" text-anchor=\"" + anchor + "\" font-family=\"" +
           normalize_font_family(theme.font_family) + "\" font-size=\"" + F(theme.font_size) + "\" fill=\"" + fill + "\">" +
           escape_xml(text) + "</text>";
}

std::string seq_label_group(const std::string& edge_id, const char* kind, float x, float y, const TextBlock& label,
                            const Theme& theme, const LayoutConfig& config, const std::string& label_color) {
    return std::string("<g class=\"edgeLabel\" data-edge-id=\"") + edge_id + "\" data-label-kind=\"" + kind + "\">" +
           text_block_svg(x, y, label, theme, config, label_color) + "</g>";
}

std::string seq_label_background(const std::string& edge_id, const char* kind, const LabelRect& rect, const Theme& theme,
                                 float fill_opacity, float stroke_opacity, const char* stroke_width, const char* css_class) {
    return std::string("<rect class=\"") + css_class + "\" data-edge-id=\"" + edge_id + "\" data-label-kind=\"" + kind + "\" x=\"" +
           F2(rect.x) + "\" y=\"" + F2(rect.y) + "\" width=\"" + F2(rect.width) + "\" height=\"" + F2(rect.height) +
           "\" rx=\"2\" ry=\"2\" fill=\"" + theme.edge_label_background + "\" fill-opacity=\"" + F2(fill_opacity) + "\" stroke=\"" +
           theme.primary_border_color + "\" stroke-opacity=\"" + F2(stroke_opacity) + "\" stroke-width=\"" + stroke_width + "\"/>";
}

std::string render_sequence_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config) {
    const SequenceData& seq = *layout.sequence;
    std::string svg;
    float width = fmax_(layout.width + SEQUENCE_VIEWBOX_PAD_LEFT + SEQUENCE_VIEWBOX_PAD_RIGHT, 1.0f);
    float height = fmax_(layout.height + SEQUENCE_VIEWBOX_PAD_TOP + SEQUENCE_VIEWBOX_PAD_BOTTOM, 1.0f);
    float vx = -SEQUENCE_VIEWBOX_PAD_LEFT, vy = -SEQUENCE_VIEWBOX_PAD_TOP;
    bool has_links = false;
    for (const auto& [id, n] : layout.nodes)
        if (n.link) has_links = true;
    for (const auto& f : seq.footboxes)
        if (f.link) has_links = true;
    svg += std::string("<svg xmlns=\"http://www.w3.org/2000/svg\"") + (has_links ? " xmlns:xlink=\"http://www.w3.org/1999/xlink\"" : "") +
           " width=\"" + F(width) + "\" height=\"" + F(height) + "\" viewBox=\"" + F(vx) + " " + F(vy) + " " + F(width) + " " + F(height) + "\">";
    svg += "<rect x=\"" + F(vx) + "\" y=\"" + F(vy) + "\" width=\"" + F(width) + "\" height=\"" + F(height) + "\" fill=\"" + theme.background + "\"/>";

    std::vector<std::string> colors{theme.line_color};
    for (const auto& e : layout.edges)
        if (e.override_style.stroke && std::find(colors.begin(), colors.end(), *e.override_style.stroke) == colors.end())
            colors.push_back(*e.override_style.stroke);
    std::map<std::string, size_t> color_ids;
    for (size_t i = 0; i < colors.size(); ++i) color_ids[colors[i]] = i;
    svg += "<defs>";
    for (size_t idx = 0; idx < colors.size(); ++idx) {
        const std::string& c = colors[idx];
        std::string id = std::to_string(idx);
        svg += "<marker id=\"arrow-" + id + "\" viewBox=\"0 0 10 10\" refX=\"5\" refY=\"5\" markerUnits=\"userSpaceOnUse\" markerWidth=\"8\" markerHeight=\"8\" orient=\"auto\"><path d=\"M 0 0 L 10 5 L 0 10 z\" fill=\"" + c + "\" stroke=\"" + c + "\" stroke-width=\"1\" stroke-dasharray=\"1,0\"/></marker>";
        svg += "<marker id=\"arrow-start-" + id + "\" viewBox=\"0 0 10 10\" refX=\"4.5\" refY=\"5\" markerUnits=\"userSpaceOnUse\" markerWidth=\"8\" markerHeight=\"8\" orient=\"auto\"><path d=\"M 0 5 L 10 10 L 10 0 z\" fill=\"" + c + "\" stroke=\"" + c + "\" stroke-width=\"1\" stroke-dasharray=\"1,0\"/></marker>";
        svg += "<marker id=\"arrow-seq-" + id + "\" viewBox=\"-1 0 12 10\" refX=\"7.9\" refY=\"5\" markerUnits=\"userSpaceOnUse\" markerWidth=\"12\" markerHeight=\"12\" orient=\"auto-start-reverse\"><path d=\"M -1 0 L 10 5 L 0 10 z\" fill=\"" + c + "\" stroke=\"" + c + "\" stroke-width=\"1\" stroke-dasharray=\"1,0\"/></marker>";
        svg += "<marker id=\"arrow-start-seq-" + id + "\" viewBox=\"-1 0 12 10\" refX=\"2.1\" refY=\"5\" markerUnits=\"userSpaceOnUse\" markerWidth=\"12\" markerHeight=\"12\" orient=\"auto\"><path d=\"M 11 0 L 0 5 L 11 10 z\" fill=\"" + c + "\" stroke=\"" + c + "\" stroke-width=\"1\" stroke-dasharray=\"1,0\"/></marker>";
    }
    svg += "</defs>";

    const std::string& stroke = theme.primary_border_color;
    for (const auto& b : seq.boxes) {
        std::string fill = b.color ? *b.color : "none";
        std::string fill_attr = "fill=\"" + fill + "\"";
        if (b.color && fill != "none") fill_attr += " fill-opacity=\"0.12\"";
        svg += "<rect x=\"" + F2(b.x) + "\" y=\"" + F2(b.y) + "\" width=\"" + F2(b.width) + "\" height=\"" + F2(b.height) + "\" " + fill_attr +
               " stroke=\"" + stroke + "\" stroke-width=\"1.2\"/>";
        if (b.label) {
            float pad_x = theme.font_size * 0.8f, pad_y = theme.font_size * 0.9f;
            svg += text_block_svg_anchor(b.x + pad_x, b.y + pad_y + b.label->height / 2.0f, *b.label, theme, config, "start",
                                         theme.primary_text_color);
        }
    }

    for (const auto& frame : seq.frames) {
        svg += "<rect x=\"" + F2(frame.x) + "\" y=\"" + F2(frame.y) + "\" width=\"" + F2(frame.width) + "\" height=\"" + F2(frame.height) +
               "\" fill=\"none\" stroke=\"" + stroke + "\" stroke-width=\"2.0\" stroke-dasharray=\"2 2\"/>";
        for (float divider_y : frame.dividers)
            svg += "<line x1=\"" + F2(frame.x) + "\" y1=\"" + F2(divider_y) + "\" x2=\"" + F2(frame.x + frame.width) + "\" y2=\"" + F2(divider_y) +
                   "\" stroke=\"" + stroke + "\" stroke-width=\"2.0\" stroke-dasharray=\"3 3\"/>";
        float box_x = frame.label_box[0], box_y = frame.label_box[1], box_w = frame.label_box[2], box_h = frame.label_box[3];
        float notch_x = box_x + box_w * 0.8f, notch_y = box_y + box_h, mid_y = box_y + box_h * 0.65f, end_x = box_x + box_w;
        svg += "<polygon points=\"" + F2(box_x) + "," + F2(box_y) + " " + F2(end_x) + "," + F2(box_y) + " " + F2(end_x) + "," + F2(mid_y) + " " +
               F2(notch_x) + "," + F2(notch_y) + " " + F2(box_x) + "," + F2(notch_y) + "\" fill=\"" + theme.primary_color + "\" stroke=\"" + stroke +
               "\" stroke-width=\"1.1\"/>";
        svg += text_block_svg(frame.label.x, frame.label.y, frame.label.text, theme, config, theme.primary_text_color);
        for (const auto& label : frame.section_labels)
            svg += text_block_svg(label.x, label.y, label.text, theme, config, std::nullopt);
    }

    for (const auto& l : seq.lifelines)
        svg += "<line x1=\"" + F2(l.x) + "\" y1=\"" + F2(l.y1) + "\" x2=\"" + F2(l.x) + "\" y2=\"" + F2(l.y2) + "\" stroke=\"" +
               theme.sequence_actor_line + "\" stroke-width=\"0.5\"/>";

    for (const auto& a : seq.activations)
        svg += "<rect x=\"" + F2(a.x) + "\" y=\"" + F2(a.y) + "\" width=\"" + F2(a.width) + "\" height=\"" + F2(a.height) + "\" fill=\"" +
               theme.sequence_activation_fill + "\" stroke=\"" + theme.sequence_activation_border + "\" stroke-width=\"1\"/>";

    for (const auto& note : seq.notes) {
        const std::string& fill = theme.sequence_note_fill;
        const std::string& nstroke = theme.sequence_note_border;
        float fold = fmin_(fmax_(theme.font_size * 0.8f, 8.0f), fmin_(note.width, note.height) * 0.3f);
        float x = note.x, y = note.y, x2 = note.x + note.width, y2 = note.y + note.height;
        float fold_x = x2 - fold, fold_y = y + fold;
        svg += "<path d=\"M " + F2(x) + " " + F2(y) + " L " + F2(fold_x) + " " + F2(y) + " L " + F2(x2) + " " + F2(fold_y) + " L " + F2(x2) + " " +
               F2(y2) + " L " + F2(x) + " " + F2(y2) + " Z\" fill=\"" + fill + "\" stroke=\"" + nstroke + "\" stroke-width=\"1.1\"/>";
        svg += "<polyline points=\"" + F2(fold_x) + "," + F2(y) + " " + F2(fold_x) + "," + F2(fold_y) + " " + F2(x2) + "," + F2(fold_y) +
               "\" fill=\"none\" stroke=\"" + nstroke + "\" stroke-width=\"1.0\"/>";
        svg += text_block_svg(note.x + note.width / 2.0f, note.y + note.height / 2.0f, note.label, theme, config, theme.primary_text_color);
    }

    auto [center_pad_x, center_pad_y] = std::pair<float, float>{4.0f, 2.0f};      // label_placement::edge_label_padding (default)
    auto [endpoint_pad_x, endpoint_pad_y] = std::pair<float, float>{3.0f, 1.6f};  // endpoint_label_padding (default)
    for (size_t edge_idx = 0; edge_idx < layout.edges.size(); ++edge_idx) {
        const EdgeLayout& edge = layout.edges[edge_idx];
        std::string d = points_to_path(edge.points);
        std::string estroke = theme.line_color;
        std::string edge_id = "edge-" + std::to_string(edge_idx);
        if (edge.override_style.stroke) estroke = *edge.override_style.stroke;
        size_t marker_id = color_ids.count(estroke) ? color_ids.at(estroke) : 0;
        std::string marker_end = edge.arrow_end ? "marker-end=\"url(#arrow-seq-" + std::to_string(marker_id) + ")\"" : "";
        std::string marker_start = edge.arrow_start ? "marker-start=\"url(#arrow-start-seq-" + std::to_string(marker_id) + ")\"" : "";
        std::string dash;
        if (edge.style == EdgeStyle::Dotted) dash = "stroke-dasharray=\"2,2\"";
        if (edge.override_style.dasharray) dash = "stroke-dasharray=\"" + *edge.override_style.dasharray + "\"";
        float stroke_width = edge.override_style.stroke_width ? *edge.override_style.stroke_width : 1.5f;
        svg += "<path id=\"" + edge_id + "\" class=\"edgePath\" data-edge-id=\"" + edge_id + "\" d=\"" + d + "\" fill=\"none\" stroke=\"" + estroke +
               "\" stroke-width=\"" + F(stroke_width) + "\" " + marker_end + " " + marker_start + " " + dash +
               " stroke-linecap=\"round\" stroke-linejoin=\"round\" />";
        if (!edge.points.empty() && edge.start_decoration)
            svg += edge_decoration_svg(edge.points.front(), edge_endpoint_angle(edge.points, true), *edge.start_decoration, estroke, stroke_width, true);
        if (!edge.points.empty() && edge.end_decoration)
            svg += edge_decoration_svg(edge.points.back(), edge_endpoint_angle(edge.points, false), *edge.end_decoration, estroke, stroke_width, false);

        const std::string label_color = edge.override_style.label_color ? *edge.override_style.label_color : theme.primary_text_color;
        const std::string& label_fill = theme.edge_label_background;
        if (edge.label) {
            float mid_x, label_y;
            if (edge.label_anchor) {
                mid_x = edge.label_anchor->first;
                label_y = edge.label_anchor->second;
            } else {
                Point start = edge.points.empty() ? Point{0.0f, 0.0f} : edge.points.front();
                Point end = edge.points.empty() ? start : edge.points.back();
                mid_x = (start.first + end.first) / 2.0f;
                float gap = layout.kind == DiagramKind::Sequence ? clamp_(theme.font_size * 0.25f, 3.0f, 5.0f) : fmax_(theme.font_size * 0.6f, 8.0f);
                label_y = start.second - gap - edge.label->height / 2.0f;
            }
            if (label_fill != "none") {
                LabelRect rect = LabelRect::from_center(mid_x, label_y, edge.label->width, edge.label->height, center_pad_x, center_pad_y);
                bool visible = edge_label_background_visible(layout.kind, EdgeLabelKind::Center, edge.points, rect);
                svg += seq_label_background(edge_id, "center", rect, theme, visible ? 0.90f : 0.0f, visible ? 0.30f : 0.0f, "0.8",
                                            "edgeLabel sequenceEdgeLabel");
            }
            svg += seq_label_group(edge_id, "center", mid_x, label_y, *edge.label, theme, config, label_color);
        }
        float end_label_offset = fmax_(theme.font_size * 0.6f, 8.0f);
        for (int which = 0; which < 2; ++which) {
            bool start = which == 0;
            const auto& label = start ? edge.start_label : edge.end_label;
            const auto& anchor = start ? edge.start_label_anchor : edge.end_label_anchor;
            if (!label) continue;
            std::optional<Point> pos = anchor ? anchor : edge_endpoint_label_position(edge, start, end_label_offset);
            if (!pos) continue;
            if (label_fill != "none") {
                LabelRect rect = LabelRect::from_center(pos->first, pos->second, label->width, label->height, endpoint_pad_x, endpoint_pad_y);
                bool visible = edge_label_background_visible(layout.kind, start ? EdgeLabelKind::Start : EdgeLabelKind::End, edge.points, rect);
                svg += seq_label_background(edge_id, start ? "start" : "end", rect, theme, visible ? 0.88f : 0.0f, visible ? 0.28f : 0.0f, "0.75",
                                            "edgeLabel sequenceEndpointLabel");
            }
            svg += seq_label_group(edge_id, start ? "start" : "end", pos->first, pos->second, *label, theme, config, label_color);
        }
    }

    for (const auto& n : seq.numbers) {
        float r = fmax_(theme.font_size * 0.45f, 6.0f);
        svg += "<circle cx=\"" + F2(n.x) + "\" cy=\"" + F2(n.y) + "\" r=\"" + F2(r) + "\" fill=\"" + theme.sequence_activation_fill +
               "\" stroke=\"" + theme.sequence_activation_border + "\" stroke-width=\"1\"/>";
        svg += text_line_svg(n.x, n.y + theme.font_size * 0.35f, std::to_string(n.value), theme, theme.primary_text_color, "middle");
    }

    auto participant_box = [&](const NodeLayout& node) {
        if (node.link) {
            svg += "<a " + link_attrs(*node.link) + ">";
            if (node.link->title) svg += "<title>" + escape_xml(*node.link->title) + "</title>";
        }
        svg += "<rect x=\"" + F2(node.x) + "\" y=\"" + F2(node.y) + "\" width=\"" + F2(node.width) + "\" height=\"" + F2(node.height) +
               "\" rx=\"3\" ry=\"3\" fill=\"" + theme.sequence_actor_fill + "\" stroke=\"" + theme.sequence_actor_border + "\" stroke-width=\"1.0\"/>";
        float center_x = node.x + node.width / 2.0f, center_y = node.y + node.height / 2.0f;
        bool all_blank = true;
        for (const auto& l : node.label.lines)
            if (!trim_view(l).empty()) all_blank = false;
        bool hide_label = all_blank || starts_with(node.id, "__start_") || starts_with(node.id, "__end_");
        if (!hide_label) svg += text_block_svg(center_x, center_y, node.label, theme, config, node.style.text_color);
        if (node.link) svg += "</a>";
    };
    for (const auto& [id, node] : layout.nodes) {
        if (node.hidden || node.anchor_subgraph) continue;
        participant_box(node);
    }
    for (const auto& foot : seq.footboxes) participant_box(foot);
    svg += "</svg>";
    return svg;
}

// ---- ER entity boxes (render_er_node and friends) ----

struct ErAttribute {
    std::string name, data_type;
    std::vector<std::string> keys;
};

// Rust trim_matches(|ch| ch == ',' || ch == ';')
std::string trim_commas_semicolons(const std::string& t) {
    size_t a = 0, b = t.size();
    while (a < b && (t[a] == ',' || t[a] == ';')) ++a;
    while (b > a && (t[b - 1] == ',' || t[b - 1] == ';')) --b;
    return t.substr(a, b - a);
}

std::pair<std::string, std::vector<ErAttribute>> parse_er_attributes(const std::vector<std::string>& lines) {
    std::string title = lines.empty() ? std::string() : trim(lines[0]);
    std::vector<ErAttribute> attrs;
    bool in_body = false;
    for (size_t li = 1; li < lines.size(); ++li) {
        const std::string& line = lines[li];
        if (is_divider_line(line)) {
            in_body = true;
            continue;
        }
        if (!in_body) {
            if (!trim_view(line).empty()) title = trim(line);
            continue;
        }
        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;
        std::vector<std::string> keys, parts;
        for (const std::string& token : split_whitespace(trimmed)) {
            std::string cleaned = to_ascii_upper(trim_commas_semicolons(token));
            if (cleaned == "PK" || cleaned == "FK" || cleaned == "UK") {
                keys.push_back(cleaned);
                continue;
            }
            if (cleaned.find(',') != std::string::npos) {
                bool handled = false;
                size_t pos = 0;
                while (true) {
                    size_t j = cleaned.find(',', pos);
                    std::string piece = cleaned.substr(pos, j == std::string::npos ? std::string::npos : j - pos);
                    if (piece == "PK" || piece == "FK" || piece == "UK") {
                        keys.push_back(piece);
                        handled = true;
                    }
                    if (j == std::string::npos) break;
                    pos = j + 1;
                }
                if (handled) continue;
            }
            parts.push_back(token);
        }
        if (parts.empty()) continue;
        ErAttribute a;
        if (parts.size() >= 2) {
            a.data_type = parts[0];
            for (size_t k = 1; k < parts.size(); ++k) a.name += (k > 1 ? " " : "") + parts[k];
        } else {
            a.name = parts[0];
        }
        a.keys = keys;
        attrs.push_back(std::move(a));
    }
    return {title, attrs};
}

// text_metrics::measure_text_width(text, size, <normalised family>).unwrap_or(fallback): the crate asks for
// the *normalised* family key here, which is not the key its font cache was seeded with. The native port
// always measures with the bundled face (see PORTING.md, "font families").
float er_text_width(const std::string& text, float font_size) { return measure_text_width(text, font_size); }

std::pair<std::string, float> er_badge_svg(float x, float y, const std::string& text, float font_size, const std::string& fill,
                                           const std::string& text_color, const std::string& font_family) {
    std::string family = normalize_font_family(font_family);
    float pad_x = fmax_(font_size * 0.45f, 4.0f);
    float text_width = er_text_width(text, font_size * 0.72f);
    float width = text_width + pad_x * 2.0f;
    float height = fmax_(font_size * 0.9f, 10.0f);
    float rect_y = y - height / 2.0f;
    float rx = fmax_(height / 2.0f, 4.0f);
    std::string svg = "<rect x=\"" + F2(x) + "\" y=\"" + F2(rect_y) + "\" width=\"" + F2(width) + "\" height=\"" + F2(height) + "\" rx=\"" +
                      F2(rx) + "\" ry=\"" + F2(rx) + "\" fill=\"" + fill + "\"/>";
    svg += "<text x=\"" + F2(x + width / 2.0f) + "\" y=\"" + F2(y + font_size * 0.26f) + "\" text-anchor=\"middle\" font-family=\"" + family +
           "\" font-size=\"" + F2(font_size * 0.72f) + "\" font-weight=\"600\" fill=\"" + text_color + "\">" + escape_xml(text) + "</text>";
    return {svg, width};
}

std::string render_er_node(const NodeLayout& node, const Theme& theme, const LayoutConfig& config) {
    auto [title, attrs] = parse_er_attributes(node.label.lines);
    float font_size = theme.font_size;
    float line_height = font_size * config.label_line_height;
    float header_height = attrs.empty() ? node.height
                                        : fmax_(fmin_(line_height + font_size * 0.6f, node.height * 0.5f), line_height + 6.0f);
    const std::string& border = node.style.stroke ? *node.style.stroke : theme.primary_border_color;
    const std::string& body_fill = node.style.fill ? *node.style.fill : theme.background;
    const std::string& header_fill = theme.cluster_background;
    const std::string& grid_color = theme.cluster_border;
    const std::string& header_text_color = theme.primary_text_color;
    const std::string& name_text_color = theme.primary_text_color;
    const std::string& type_text_color = theme.line_color;
    float x = node.x, y = node.y, w = node.width, h = node.height;
    float radius = 6.0f;

    std::string svg = "<rect x=\"" + F2(x) + "\" y=\"" + F2(y) + "\" width=\"" + F2(w) + "\" height=\"" + F2(h) + "\" rx=\"" + F2(radius) +
                      "\" ry=\"" + F2(radius) + "\" fill=\"" + body_fill + "\" stroke=\"" + border + "\" stroke-width=\"" +
                      F(node.style.stroke_width ? *node.style.stroke_width : 1.2f) + "\"/>";
    svg += "<rect x=\"" + F2(x) + "\" y=\"" + F2(y) + "\" width=\"" + F2(w) + "\" height=\"" + F2(header_height) + "\" rx=\"" + F2(radius) +
           "\" ry=\"" + F2(radius) + "\" fill=\"" + header_fill + "\"/>";
    TextBlock header_label;
    header_label.lines = {title};
    float header_y = y + header_height / 2.0f;
    svg += text_block_svg_anchor(x + w / 2.0f, header_y, header_label, theme, config, "middle", header_text_color);
    if (attrs.empty()) return svg;

    float pad_x = fmax_(font_size * 0.8f, 10.0f);
    float max_type_width = 0.0f, max_name_width = 0.0f, max_badge_width = 0.0f;
    for (const ErAttribute& attr : attrs) {
        if (!attr.data_type.empty()) max_type_width = fmax_(max_type_width, er_text_width(attr.data_type, font_size));
        max_name_width = fmax_(max_name_width, er_text_width(attr.name, font_size));
        if (!attr.keys.empty()) {
            float row_badge_width = 0.0f;
            for (size_t k = 0; k < attr.keys.size() && k < 2; ++k) {
                float text_width = er_text_width(attr.keys[k], font_size * 0.72f);
                float badge_width = text_width + fmax_(font_size * 0.45f, 4.0f) * 2.0f;
                row_badge_width += badge_width + font_size * 0.4f;
            }
            if (row_badge_width > 0.0f) row_badge_width -= font_size * 0.4f;
            max_badge_width = fmax_(max_badge_width, row_badge_width);
        }
    }
    float type_col_pad = font_size * 0.9f;
    float available = fmax_(w - pad_x * 2.0f, font_size * 4.0f);
    float type_col_width = max_type_width > 0.0f ? fmin_(max_type_width + type_col_pad * 2.0f, available * 0.45f) : 0.0f;
    float min_name_width = fmin_(max_name_width + font_size * 0.6f, available * 0.7f);
    float min_type_width = max_type_width > 0.0f ? fmax_(font_size * 2.8f, 36.0f) : 0.0f;
    if (type_col_width < min_type_width) type_col_width = min_type_width;
    float col_x = x + w - pad_x - type_col_width;
    float min_col_x = x + pad_x + max_badge_width + min_name_width;
    if (col_x < min_col_x) col_x = min_col_x;
    bool show_type_col = type_col_width > 0.0f && col_x < x + w - pad_x - 8.0f;

    svg += "<line x1=\"" + F2(x) + "\" y1=\"" + F2(y + header_height) + "\" x2=\"" + F2(x + w) + "\" y2=\"" + F2(y + header_height) +
           "\" stroke=\"" + grid_color + "\" stroke-width=\"1.0\" stroke-opacity=\"0.6\"/>";
    if (show_type_col)
        svg += "<line x1=\"" + F2(col_x) + "\" y1=\"" + F2(y + header_height) + "\" x2=\"" + F2(col_x) + "\" y2=\"" + F2(y + h) + "\" stroke=\"" +
               grid_color + "\" stroke-width=\"1.0\" stroke-opacity=\"0.45\"/>";
    float row_height = line_height;
    float body_height = fmax_(h - header_height, line_height);
    {
        float needed = static_cast<float>(attrs.size()) * row_height;
        if (needed > body_height) row_height = body_height / static_cast<float>(attrs.size());
    }
    for (size_t idx = 0; idx < attrs.size(); ++idx) {
        const ErAttribute& attr = attrs[idx];
        float row_top = y + header_height + static_cast<float>(idx) * row_height;
        float row_center = row_top + row_height / 2.0f;
        if (idx > 0)
            svg += "<line x1=\"" + F2(x) + "\" y1=\"" + F2(row_top) + "\" x2=\"" + F2(x + w) + "\" y2=\"" + F2(row_top) + "\" stroke=\"" +
                   grid_color + "\" stroke-width=\"1.0\" stroke-opacity=\"0.35\"/>";
        float cursor_x = x + pad_x;
        for (size_t k = 0; k < attr.keys.size() && k < 2; ++k) {
            const std::string& key = attr.keys[k];
            std::string fill = key == "PK" ? "#1D4ED8" : key == "FK" ? "#0F766E" : key == "UK" ? "#7C3AED" : "#475569";
            auto [badge_svg, badge_width] = er_badge_svg(cursor_x, row_center, key, font_size, fill, "#FFFFFF", theme.font_family);
            svg += badge_svg;
            cursor_x += badge_width + font_size * 0.4f;
        }
        TextBlock name_label;
        name_label.lines = {attr.name};
        svg += text_block_svg_anchor(cursor_x, row_center, name_label, theme, config, "start", name_text_color);
        if (show_type_col && !attr.data_type.empty()) {
            TextBlock type_label;
            type_label.lines = {attr.data_type};
            svg += text_block_svg_anchor(x + w - pad_x, row_center, type_label, theme, config, "end", type_text_color);
        }
    }
    return svg;
}

// render_er_node_label: only the labels of the generic (non-ER) path use text_block_svg_class; ER nodes
// are drawn completely by render_er_node.

std::pair<float, float> endpoint_label_padding_for(DiagramKind kind) {
    switch (kind) {
    case DiagramKind::State: return {2.6f, 1.4f};
    case DiagramKind::Flowchart: return {3.4f, 1.8f};
    case DiagramKind::Class: return {3.2f, 1.6f};
    default: return {3.0f, 1.6f};
    }
}

// The marker definitions (<defs>) of the generic graph path: arrows per stroke colour, plus the state
// and class variants.
std::string graph_defs(const std::vector<std::string>& colors, bool is_state, bool is_class) {
    std::string svg = "<defs>";
    for (size_t idx = 0; idx < colors.size(); ++idx) {
        const std::string& c = colors[idx];
        std::string id = std::to_string(idx);
        svg += "<marker id=\"arrow-" + id + "\" viewBox=\"0 0 10 10\" refX=\"5\" refY=\"5\" markerUnits=\"userSpaceOnUse\" markerWidth=\"8\" markerHeight=\"8\" orient=\"auto\"><path d=\"M 0 0 L 10 5 L 0 10 z\" fill=\"" + c + "\" stroke=\"" + c + "\" stroke-width=\"1\" stroke-dasharray=\"1,0\"/></marker>";
        svg += "<marker id=\"arrow-start-" + id + "\" viewBox=\"0 0 10 10\" refX=\"4.5\" refY=\"5\" markerUnits=\"userSpaceOnUse\" markerWidth=\"8\" markerHeight=\"8\" orient=\"auto\"><path d=\"M 0 5 L 10 10 L 10 0 z\" fill=\"" + c + "\" stroke=\"" + c + "\" stroke-width=\"1\" stroke-dasharray=\"1,0\"/></marker>";
        if (is_state)
            svg += "<marker id=\"arrow-state-" + id + "\" viewBox=\"0 0 20 14\" refX=\"19\" refY=\"7\" markerUnits=\"userSpaceOnUse\" markerWidth=\"20\" markerHeight=\"14\" orient=\"auto\"><path d=\"M 19 7 L 9 13 L 14 7 L 9 1 Z\" fill=\"" + c + "\" stroke=\"" + c + "\" stroke-width=\"1\" stroke-dasharray=\"1,0\"/></marker>";
        if (is_class) {
            svg += "<marker id=\"arrow-class-open-" + id + "\" viewBox=\"0 0 20 14\" refX=\"1\" refY=\"7\" markerUnits=\"userSpaceOnUse\" markerWidth=\"20\" markerHeight=\"14\" orient=\"auto\"><path d=\"M 1 7 L 18 13 V 1 Z\" fill=\"none\" stroke=\"" + c + "\" stroke-width=\"1\" stroke-dasharray=\"1,0\"/></marker>";
            svg += "<marker id=\"arrow-class-open-start-" + id + "\" viewBox=\"0 0 20 14\" refX=\"18\" refY=\"7\" markerUnits=\"userSpaceOnUse\" markerWidth=\"20\" markerHeight=\"14\" orient=\"auto\"><path d=\"M 1 7 L 18 13 V 1 Z\" fill=\"none\" stroke=\"" + c + "\" stroke-width=\"1\" stroke-dasharray=\"1,0\"/></marker>";
            svg += "<marker id=\"arrow-class-dep-" + id + "\" viewBox=\"0 0 20 14\" refX=\"13\" refY=\"7\" markerUnits=\"userSpaceOnUse\" markerWidth=\"20\" markerHeight=\"14\" orient=\"auto\"><path d=\"M 18 7 L 9 13 L 14 7 L 9 1 Z\" fill=\"" + c + "\" stroke=\"" + c + "\" stroke-width=\"1\" stroke-dasharray=\"1,0\"/></marker>";
            svg += "<marker id=\"arrow-class-dep-start-" + id + "\" viewBox=\"0 0 20 14\" refX=\"6\" refY=\"7\" markerUnits=\"userSpaceOnUse\" markerWidth=\"20\" markerHeight=\"14\" orient=\"auto\"><path d=\"M 5 7 L 9 13 L 1 7 L 9 1 Z\" fill=\"" + c + "\" stroke=\"" + c + "\" stroke-width=\"1\" stroke-dasharray=\"1,0\"/></marker>";
        }
    }
    svg += "</defs>";
    return svg;
}

// A folded-corner note box (state diagram notes).
std::string note_svg(float nx, float ny, float nw, float nh, const TextBlock& label, const Theme& theme, const LayoutConfig& config,
                     float font_size) {
    const std::string& fill = theme.sequence_note_fill;
    const std::string& stroke = theme.sequence_note_border;
    float fold = fmin_(fmax_(theme.font_size * 0.8f, 8.0f), fmin_(nw, nh) * 0.3f);
    float x = nx, y = ny, x2 = nx + nw, y2 = ny + nh;
    float fold_x = x2 - fold, fold_y = y + fold;
    std::string svg = "<path d=\"M " + F2(x) + " " + F2(y) + " L " + F2(fold_x) + " " + F2(y) + " L " + F2(x2) + " " + F2(fold_y) + " L " + F2(x2) + " " +
                      F2(y2) + " L " + F2(x) + " " + F2(y2) + " Z\" fill=\"" + fill + "\" stroke=\"" + stroke + "\" stroke-width=\"1.1\"/>";
    svg += "<polyline points=\"" + F2(fold_x) + "," + F2(y) + " " + F2(fold_x) + "," + F2(fold_y) + " " + F2(x2) + "," + F2(fold_y) +
           "\" fill=\"none\" stroke=\"" + stroke + "\" stroke-width=\"1.0\"/>";
    svg += text_block_svg_with_font_size(nx + nw / 2.0f, ny + nh / 2.0f, label, theme, config, font_size, "middle", theme.primary_text_color, false);
    return svg;
}

// Subgraph / composite-state boxes.
std::string subgraph_svg(const SubgraphLayout& sg, bool is_state, float state_font_size, const Theme& theme, const LayoutConfig& config) {
    std::string svg;
    bool label_empty = trim_view(sg.label).empty();
    if (is_state) {
        const std::string& sub_fill = sg.style.fill ? *sg.style.fill : theme.primary_color;
        const std::string& sub_stroke = sg.style.stroke ? *sg.style.stroke : theme.primary_border_color;
        float sub_stroke_width = sg.style.stroke_width ? *sg.style.stroke_width : 1.0f;
        bool invisible = label_empty && sub_fill == "none" && sub_stroke == "none" && sub_stroke_width <= 0.0f;
        if (invisible) return "";
        float header_h = label_empty ? 0.0f : fmax_(sg.label_block.height + theme.font_size * 0.75f, theme.font_size * 1.4f);
        std::string header_fill = sub_fill == "none" ? std::string("none") : adjust_color(sub_fill, 0.0f, 0.0f, -4.0f);
        std::string body_fill = sub_fill == "none" ? theme.background : adjust_color(sub_fill, 0.0f, -12.0f, 10.0f);
        if (header_h > 0.0f)
            svg += "<rect x=\"" + F2(sg.x) + "\" y=\"" + F2(sg.y) + "\" width=\"" + F2(sg.width) + "\" height=\"" + F2(header_h) +
                   "\" rx=\"6\" ry=\"6\" fill=\"" + header_fill + "\" stroke=\"none\"/>";
        float inner_y = sg.y + header_h;
        float inner_h = fmax_(sg.height - header_h, 0.0f);
        if (inner_h > 0.0f)
            svg += "<rect x=\"" + F2(sg.x) + "\" y=\"" + F2(inner_y) + "\" width=\"" + F2(sg.width) + "\" height=\"" + F2(inner_h) +
                   "\" fill=\"" + body_fill + "\" stroke=\"none\"/>";
        if (header_h > 0.0f)
            svg += "<line x1=\"" + F2(sg.x) + "\" y1=\"" + F2(inner_y) + "\" x2=\"" + F2(sg.x + sg.width) + "\" y2=\"" + F2(inner_y) +
                   "\" stroke=\"" + sub_stroke + "\" stroke-width=\"1\"/>";
        svg += "<rect x=\"" + F2(sg.x) + "\" y=\"" + F2(sg.y) + "\" width=\"" + F2(sg.width) + "\" height=\"" + F2(sg.height) +
               "\" rx=\"6\" ry=\"6\" fill=\"none\" stroke=\"" + sub_stroke + "\" stroke-width=\"" + F(sub_stroke_width) + "\"/>";
        if (!label_empty) {
            float label_pad_x = fmax_(theme.font_size * 0.6f, sg.label_block.height * 0.35f);
            float label_x = sg.x + label_pad_x;
            float label_y = sg.y + header_h / 2.0f;
            svg += text_block_svg_with_font_size_weight(label_x, label_y, sg.label_block, theme, config, state_font_size, "start",
                                                        sg.style.text_color, std::string("600"), false);
        }
        return svg;
    }
    const std::string& sub_fill = sg.style.fill ? *sg.style.fill : theme.cluster_background;
    const std::string& sub_stroke = sg.style.stroke ? *sg.style.stroke : theme.cluster_border;
    std::string sub_dash = sg.style.stroke_dasharray ? " stroke-dasharray=\"" + *sg.style.stroke_dasharray + "\"" : "";
    float sub_stroke_width = sg.style.stroke_width ? *sg.style.stroke_width : 1.0f;
    bool invisible = label_empty && sub_fill == "none" && sub_stroke == "none" && sub_stroke_width <= 0.0f;
    if (!invisible)
        svg += "<rect x=\"" + F2(sg.x) + "\" y=\"" + F2(sg.y) + "\" width=\"" + F2(sg.width) + "\" height=\"" + F2(sg.height) +
               "\" rx=\"10\" ry=\"10\" fill=\"" + sub_fill + "\" stroke=\"" + sub_stroke + "\" stroke-width=\"" + F(sub_stroke_width) + "\"" +
               sub_dash + " />";
    if (!label_empty) {
        float label_x = sg.x + sg.width / 2.0f;
        float label_y = sg.y + 12.0f + sg.label_block.height / 2.0f;
        std::string label_color = sg.style.text_color ? *sg.style.text_color : theme.primary_text_color;
        svg += text_block_svg(label_x, label_y, sg.label_block, theme, config, label_color);
    }
    return svg;
}

}  // namespace

// ---- exports for the other diagram writers (svg_common.h) ----
namespace svg {
std::string F(float v) { return mermaid::F(v); }
std::string F2(float v) { return mermaid::F2(v); }
std::string F3(float v) { return mermaid::F3(v); }
std::string escape_xml(const std::string& in) { return mermaid::escape_xml(in); }
std::string normalize_font_family(const std::string& f) { return mermaid::normalize_font_family(f); }

std::string document_start(float width, float height, float vx, float vy, float vw, float vh, bool percent_width, bool has_links,
                           const Theme& theme, bool with_defs, const std::vector<std::string>* extra_stroke_colors) {
    std::string s = std::string("<svg xmlns=\"http://www.w3.org/2000/svg\"") + (has_links ? " xmlns:xlink=\"http://www.w3.org/1999/xlink\"" : "");
    if (percent_width) s += " width=\"100%\" viewBox=\"" + F(vx) + " " + F(vy) + " " + F(vw) + " " + F(vh) + "\" style=\"max-width: " + F3(vw) + "px;\">";
    else s += " width=\"" + F(width) + "\" height=\"" + F(height) + "\" viewBox=\"" + F(vx) + " " + F(vy) + " " + F(vw) + " " + F(vh) + "\">";
    s += "<rect x=\"" + F(vx) + "\" y=\"" + F(vy) + "\" width=\"" + F(vw) + "\" height=\"" + F(vh) + "\" fill=\"" + theme.background + "\"/>";
    if (with_defs) {
        std::vector<std::string> colors{theme.line_color};
        if (extra_stroke_colors)
            for (const auto& c : *extra_stroke_colors)
                if (std::find(colors.begin(), colors.end(), c) == colors.end()) colors.push_back(c);
        s += graph_defs(colors, false, false);
    }
    return s;
}

std::string marker_defs(const std::vector<std::string>& colors) { return graph_defs(colors, false, false); }

std::string text_block(float x, float y, const TextBlock& label, const Theme& theme, const LayoutConfig& config, float font_size,
                       const std::string& anchor, const std::optional<std::string>& override_color, bool baseline,
                       const std::optional<std::string>& font_weight) {
    return text_block_svg_with_font_size_weight(x, y, label, theme, config, font_size, anchor, override_color, font_weight, baseline);
}

std::string points_to_path(const std::vector<std::pair<float, float>>& points) { return mermaid::points_to_path(points); }
std::array<float, 4> label_rect_from_center(float cx, float cy, float lw, float lh, float pad_x, float pad_y) {
    LabelRect r = LabelRect::from_center(cx, cy, lw, lh, pad_x, pad_y);
    return {r.x, r.y, r.width, r.height};
}
bool label_background_visible(DiagramKind kind, const std::vector<std::pair<float, float>>& points, const std::array<float, 4>& rect) {
    return edge_label_background_visible(kind, EdgeLabelKind::Center, points, LabelRect{rect[0], rect[1], rect[2], rect[3]});
}

std::string text_line(float x, float y, const std::string& text, const Theme& theme, float font_size, const std::string& fill,
                      const std::string& anchor) {
    return "<text x=\"" + F2(x) + "\" y=\"" + F2(y) + "\" text-anchor=\"" + anchor + "\" font-family=\"" +
           normalize_font_family(theme.font_family) + "\" font-size=\"" + F(font_size) + "\" fill=\"" + fill + "\">" + escape_xml(text) + "</text>";
}
}  // namespace svg

std::string render_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config) {
    if ((layout.kind == DiagramKind::Sequence || layout.kind == DiagramKind::ZenUML) && layout.sequence) return render_sequence_svg(layout, theme, config);
    if (std::optional<std::string> other = render_other_svg(layout, theme, config)) return *other;
    switch (layout.kind) {
    case DiagramKind::Flowchart: case DiagramKind::Class: case DiagramKind::State: case DiagramKind::Er: case DiagramKind::Kanban: case DiagramKind::Packet: case DiagramKind::Treemap: case DiagramKind::Block: case DiagramKind::Mindmap: break;
    default: throw Unsupported("svg of this diagram kind");
    }
    const DiagramKind kind = layout.kind;
    const bool is_state = kind == DiagramKind::State;
    const bool is_class = kind == DiagramKind::Class;
    const bool overlay_flowchart = kind == DiagramKind::Flowchart;
    const float state_font_size = is_state ? theme.font_size * 0.85f : theme.font_size;

    std::string svg;
    float width = fmax_(layout.width, 1.0f), height = fmax_(layout.height, 1.0f);
    bool has_links = false;
    for (const auto& [id, n] : layout.nodes)
        if (n.link) has_links = true;
    if (kind == DiagramKind::Mindmap) {
        // measure_svg_dimensions: the viewBox hugs the nodes plus config.mindmap.padding; width is 100% (use_max_width)
        float pad = config.mindmap.padding;
        float min_x = std::numeric_limits<float>::max(), min_y = min_x;
        float max_x = std::numeric_limits<float>::lowest(), max_y = max_x;
        for (const auto& [id, n] : layout.nodes) {
            min_x = fmin_(min_x, n.x);
            min_y = fmin_(min_y, n.y);
            max_x = fmax_(max_x, n.x + n.width);
            max_y = fmax_(max_y, n.y + n.height);
        }
        if (min_x == std::numeric_limits<float>::max()) { min_x = 0.0f; max_x = 1.0f; }
        if (min_y == std::numeric_limits<float>::max()) { min_y = 0.0f; max_y = 1.0f; }
        float vw = fmax_(max_x - min_x + pad * 2.0f, 1.0f), vh = fmax_(max_y - min_y + pad * 2.0f, 1.0f);
        svg += svg::document_start(vw, vh, min_x - pad, min_y - pad, vw, vh, config.mindmap.use_max_width, has_links, theme, false);
    } else {
    svg += std::string("<svg xmlns=\"http://www.w3.org/2000/svg\"") + (has_links ? " xmlns:xlink=\"http://www.w3.org/1999/xlink\"" : "") +
           " width=\"" + F(width) + "\" height=\"" + F(height) + "\" viewBox=\"0 0 " + F(width) + " " + F(height) + "\">";
    svg += "<rect x=\"0\" y=\"0\" width=\"" + F(width) + "\" height=\"" + F(height) + "\" fill=\"" + theme.background + "\"/>";
    }

    // Arrow markers: one set per distinct stroke colour (the theme line colour is index 0).
    std::vector<std::string> colors{theme.line_color};
    for (const auto& e : layout.edges)
        if (e.override_style.stroke && std::find(colors.begin(), colors.end(), *e.override_style.stroke) == colors.end())
            colors.push_back(*e.override_style.stroke);
    std::map<std::string, size_t> color_ids;
    for (size_t i = 0; i < colors.size(); ++i) color_ids[colors[i]] = i;
    svg += graph_defs(colors, is_state, is_class);

    // Subgraph boxes come before the edges.
    for (const auto& sg : layout.subgraphs) svg += subgraph_svg(sg, is_state, state_font_size, theme, config);

    // State diagram notes.
    for (const auto& note : layout.state_notes) svg += note_svg(note.x, note.y, note.width, note.height, note.label, theme, config, state_font_size);

    // Edges.
    float base_edge_width = (is_class || is_state || kind == DiagramKind::Er) ? 1.0f : 2.0f;
    for (size_t edge_idx = 0; edge_idx < layout.edges.size(); ++edge_idx) {
        const EdgeLayout& edge = layout.edges[edge_idx];
        std::string d = (kind == DiagramKind::Flowchart && edge.points.size() > 2)   ? rounded_polyline_path(edge.points, 10.0f)
                        : (kind == DiagramKind::Mindmap && edge.points.size() > 2) ? basis_curve_path(edge.points)
                                                                                   : points_to_path(edge.points);
        std::string stroke = theme.line_color;
        std::string edge_id = "edge-" + std::to_string(edge_idx);
        std::string dash;
        float stroke_width = base_edge_width;
        switch (edge.style) {
        case EdgeStyle::Solid: break;
        case EdgeStyle::Dotted: dash = "stroke-dasharray=\"4 4\""; break;
        case EdgeStyle::Thick: stroke_width = 3.5f; break;
        }
        if (edge.override_style.stroke) stroke = *edge.override_style.stroke;
        std::string marker_id = std::to_string(color_ids.count(stroke) ? color_ids.at(stroke) : 0);
        std::string marker_end, marker_start;
        if (edge.arrow_end && !overlay_flowchart) {
            if (is_state) marker_end = "marker-end=\"url(#arrow-state-" + marker_id + ")\"";
            else if (is_class) {
                if (edge.arrow_end_kind == EdgeArrowhead::OpenTriangle) marker_end = "marker-end=\"url(#arrow-class-open-" + marker_id + ")\"";
                else if (edge.arrow_end_kind == EdgeArrowhead::ClassDependency) marker_end = "marker-end=\"url(#arrow-class-dep-" + marker_id + ")\"";
                else marker_end = "marker-end=\"url(#arrow-" + marker_id + ")\"";
            } else marker_end = "marker-end=\"url(#arrow-" + marker_id + ")\"";
        }
        if (edge.arrow_start && !overlay_flowchart) {
            if (is_state) marker_start = "marker-start=\"url(#arrow-state-" + marker_id + ")\"";
            else if (is_class) {
                if (edge.arrow_start_kind == EdgeArrowhead::OpenTriangle) marker_start = "marker-start=\"url(#arrow-class-open-start-" + marker_id + ")\"";
                else if (edge.arrow_start_kind == EdgeArrowhead::ClassDependency) marker_start = "marker-start=\"url(#arrow-class-dep-start-" + marker_id + ")\"";
                else marker_start = "marker-start=\"url(#arrow-start-" + marker_id + ")\"";
            } else marker_start = "marker-start=\"url(#arrow-start-" + marker_id + ")\"";
        }
        if (edge.override_style.stroke_width) stroke_width = *edge.override_style.stroke_width;
        if (edge.override_style.dasharray) dash = "stroke-dasharray=\"" + *edge.override_style.dasharray + "\"";
        svg += "<path id=\"" + edge_id + "\" class=\"edgePath\" data-edge-id=\"" + edge_id + "\" d=\"" + d + "\" fill=\"none\" stroke=\"" + stroke +
               "\" stroke-width=\"" + F(stroke_width) + "\" " + marker_end + " " + marker_start + " " + dash +
               " stroke-linecap=\"round\" stroke-linejoin=\"round\" />";
        if (overlay_flowchart) {
            // flowcharts draw arrowheads as separate shapes
            if (edge.arrow_start && !edge.points.empty())
                svg += arrowhead_svg(edge.points.front(), edge_endpoint_angle(edge.points, true) + 180.0f, stroke, stroke_width);
            if (edge.arrow_end && !edge.points.empty())
                svg += arrowhead_svg(edge.points.back(), edge_endpoint_angle(edge.points, false), stroke, stroke_width);
        }
        if (!edge.points.empty() && edge.start_decoration)
            svg += edge_decoration_svg(edge.points.front(), edge_endpoint_angle(edge.points, true), *edge.start_decoration, stroke, stroke_width, true);
        if (!edge.points.empty() && edge.end_decoration)
            svg += edge_decoration_svg(edge.points.back(), edge_endpoint_angle(edge.points, false), *edge.end_decoration, stroke, stroke_width, false);

        if (edge.label && edge.label_anchor) {
            auto [x, y] = *edge.label_anchor;
            auto [pad_x, pad_y] = edge_label_padding(kind, config);
            float fill_opacity = 0.85f, stroke_opacity = 0.35f;
            if (is_state) { fill_opacity = 0.7f; stroke_opacity = 0.25f; }
            else if (overlay_flowchart) { fill_opacity = 0.95f; stroke_opacity = 0.45f; }
            float label_scale = is_state ? fmin_(state_font_size / theme.font_size, 1.0f) : 1.0f;
            float label_w = edge.label->width * label_scale, label_h = edge.label->height * label_scale;
            LabelRect rect = LabelRect::from_center(x, y, label_w, label_h, pad_x, pad_y);
            const std::string& label_fill = theme.edge_label_background;
            if (label_fill != "none") {
                bool visible = edge_label_background_visible(kind, EdgeLabelKind::Center, edge.points, rect);
                float fo = visible ? fill_opacity : 0.0f, so = visible ? stroke_opacity : 0.0f;
                svg += "<rect data-edge-id=\"" + edge_id + "\" data-label-kind=\"center\" x=\"" + F2(rect.x) + "\" y=\"" + F2(rect.y) +
                       "\" width=\"" + F2(rect.width) + "\" height=\"" + F2(rect.height) + "\" rx=\"2\" ry=\"2\" fill=\"" + label_fill +
                       "\" fill-opacity=\"" + F2(fo) + "\" stroke=\"" + theme.primary_border_color + "\" stroke-opacity=\"" + F2(so) +
                       "\" stroke-width=\"0.8\"/>";
            }
            svg += "<g class=\"edgeLabel\" data-edge-id=\"" + edge_id + "\" data-label-kind=\"center\">";
            if (is_state)
                svg += text_block_svg_with_font_size(x, y, *edge.label, theme, config, state_font_size, "middle", edge.override_style.label_color, false);
            else
                svg += text_block_svg(x, y, *edge.label, theme, config, edge.override_style.label_color);
            svg += "</g>";
        }

        // Start / end (multiplicity) labels.
        float endpoint_scale = is_state ? fmin_(state_font_size / theme.font_size, 1.0f) : 1.0f;
        auto [endpoint_pad_x, endpoint_pad_y] = endpoint_label_padding_for(kind);
        float endpoint_fill_opacity = 0.85f, endpoint_stroke_opacity = 0.35f;
        if (is_state) { endpoint_fill_opacity = 0.7f; endpoint_stroke_opacity = 0.25f; }
        else if (overlay_flowchart) { endpoint_fill_opacity = 0.95f; endpoint_stroke_opacity = 0.45f; }
        else if (is_class) { endpoint_fill_opacity = 0.9f; endpoint_stroke_opacity = 0.4f; }
        const std::string& endpoint_label_fill = theme.edge_label_background;
        std::string label_color = edge.override_style.label_color ? *edge.override_style.label_color : theme.primary_text_color;
        for (int which = 0; which < 2; ++which) {
            bool start = which == 0;
            const auto& label = start ? edge.start_label : edge.end_label;
            const auto& anchor = start ? edge.start_label_anchor : edge.end_label_anchor;
            if (!label || !anchor) continue;
            auto [x, y] = *anchor;
            float label_w = label->width * endpoint_scale, label_h = label->height * endpoint_scale;
            LabelRect rect = LabelRect::from_center(x, y, label_w, label_h, endpoint_pad_x, endpoint_pad_y);
            const char* kind_name = start ? "start" : "end";
            if (endpoint_label_fill != "none") {
                bool visible = edge_label_background_visible(kind, start ? EdgeLabelKind::Start : EdgeLabelKind::End, edge.points, rect);
                float fo = visible ? endpoint_fill_opacity : 0.0f, so = visible ? endpoint_stroke_opacity : 0.0f;
                svg += std::string("<rect data-edge-id=\"") + edge_id + "\" data-label-kind=\"" + kind_name + "\" x=\"" + F2(rect.x) + "\" y=\"" +
                       F2(rect.y) + "\" width=\"" + F2(rect.width) + "\" height=\"" + F2(rect.height) + "\" rx=\"2\" ry=\"2\" fill=\"" +
                       endpoint_label_fill + "\" fill-opacity=\"" + F2(fo) + "\" stroke=\"" + theme.primary_border_color + "\" stroke-opacity=\"" +
                       F2(so) + "\" stroke-width=\"0.8\"/>";
            }
            svg += std::string("<g class=\"edgeLabel\" data-edge-id=\"") + edge_id + "\" data-label-kind=\"" + kind_name + "\">";
            if (is_state) svg += text_block_svg_with_font_size(x, y, *label, theme, config, state_font_size, "middle", label_color, false);
            else svg += text_block_svg(x, y, *label, theme, config, label_color);
            svg += "</g>";
        }
    }

    // Nodes (treemaps draw the biggest boxes first so nested ones stay visible).
    std::vector<const NodeLayout*> nodes_to_draw;
    for (const auto& entry : layout.nodes) nodes_to_draw.push_back(&entry.second);
    if (kind == DiagramKind::Treemap)
        std::stable_sort(nodes_to_draw.begin(), nodes_to_draw.end(),
                         [](const NodeLayout* a, const NodeLayout* b) { return a->width * a->height > b->width * b->height; });
    for (const NodeLayout* node_ptr : nodes_to_draw) {
        const NodeLayout& node = *node_ptr;
        if (node.hidden || node.anchor_subgraph) continue;
        if (node.link) {
            svg += "<a " + link_attrs(*node.link) + ">";
            if (node.link->title) svg += "<title>" + escape_xml(*node.link->title) + "</title>";
        }
        if (kind == DiagramKind::Er) {
            svg += render_er_node(node, theme, config);
            if (node.link) svg += "</a>";
            continue;
        }
        svg += shape_svg(node, theme, config);
        float divider_line_height = is_class ? theme.font_size * config.class_label_line_height() : theme.font_size * config.label_line_height;
        svg += divider_lines_svg(node, theme, divider_line_height);
        float center_x = node.x + node.width / 2.0f, center_y = node.y + node.height / 2.0f;
        bool all_blank = true;
        for (const auto& l : node.label.lines)
            if (!trim_view(l).empty()) all_blank = false;
        bool hide_label = all_blank || starts_with(node.id, "__start_") || starts_with(node.id, "__end_");
        if (!hide_label && kind == DiagramKind::Treemap) {
            // treemap labels sit at the top-left corner of the box
            float label_x = node.x + 6.0f;  // config.treemap.label_padding_x
            float label_y = node.y + 4.0f + node.label.height / 2.0f;  // label_padding_y
            svg += text_block_svg_anchor(label_x, label_y, node.label, theme, config, "start", node.style.text_color);
        } else if (!hide_label) {
            bool has_divider = false;
            for (const auto& l : node.label.lines)
                if (is_divider_line(l)) has_divider = true;
            if (has_divider) svg += text_block_svg_class(node, theme, config, node.style.text_color);
            else if (is_state)
                svg += text_block_svg_with_font_size(center_x, center_y, node.label, theme, config, state_font_size, "middle", node.style.text_color, false);
            else svg += text_block_svg(center_x, center_y, node.label, theme, config, node.style.text_color);
        }
        if (node.link) svg += "</a>";
    }
    svg += "</svg>";
    return svg;
}

}  // namespace mermaid
