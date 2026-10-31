// Mermaid port: gantt charts. Mirrors parser.rs parse_gantt_diagram, layout/gantt.rs and render.rs render_gantt.
#include <algorithm>
#include <cmath>
#include <map>

#include "kinds.h"
#include "numfmt.h"
#include "parser_internal.h"
#include "strutil.h"
#include "svg_common.h"
#include "text.h"

namespace mermaid {

namespace {

float fmax_(float a, float b) { return a > b ? a : (a < b ? b : a); }
float fmin_(float a, float b) { return a < b ? a : (a > b ? b : a); }

// ---- parser helpers ----

std::optional<std::string> extract_frontmatter_value(const std::string& input, const std::string& key) {
    bool in_frontmatter = false;
    for (const std::string& line : lines_of(input)) {
        std::string trimmed = trim(line);
        if (trimmed == "---") {
            if (in_frontmatter) return std::nullopt;
            in_frontmatter = true;
            continue;
        }
        if (in_frontmatter) {
            size_t colon = trimmed.find(':');
            if (colon != std::string::npos && trim(trimmed.substr(0, colon)) == key) {
                std::string val = trim(trimmed.substr(colon + 1));
                if (!val.empty()) return val;
            }
        }
    }
    return std::nullopt;
}

std::optional<GanttStatus> gantt_status_from_token(const std::string& t) {
    if (t == "done") return GanttStatus::Done;
    if (t == "active") return GanttStatus::Active;
    if (t == "crit") return GanttStatus::Crit;
    if (t == "milestone") return GanttStatus::Milestone;
    return std::nullopt;
}

bool looks_like_date(const std::string& t) { return t.find('-') != std::string::npos || t.find('/') != std::string::npos || t.find('.') != std::string::npos; }

bool looks_like_duration(const std::string& token) {
    std::string lower = to_ascii_lower(token);
    if (lower.empty()) return false;
    char unit = lower.back();
    if (unit != 'd' && unit != 'h' && unit != 'w' && unit != 'm' && unit != 'y') return false;
    std::string digits = lower.substr(0, lower.size() - 1);
    if (digits.empty()) return false;
    bool any_digit = false;
    for (char c : digits) {
        if (c >= '0' && c <= '9') any_digit = true;
        else if (c != '.') return false;
    }
    return any_digit;
}

struct TaskMeta {
    std::optional<std::string> id;
    std::vector<std::string> details;
    std::optional<std::string> after;
    std::optional<GanttStatus> status;
};

TaskMeta parse_gantt_task_meta(const std::string& meta) {
    TaskMeta m;
    size_t pos = 0;
    while (true) {
        size_t j = meta.find(',', pos);
        std::string token = trim(meta.substr(pos, j == std::string::npos ? std::string::npos : j - pos));
        if (!token.empty()) {
            std::string lower = to_ascii_lower(token);
            if (starts_with(lower, "after ")) {
                std::string dep = trim(lower.substr(6));
                if (!dep.empty()) m.after = dep;
            } else if (auto st = gantt_status_from_token(lower)) {
                m.status = st;
                m.details.push_back(token);
            } else if (looks_like_date(token) || looks_like_duration(token)) {
                m.details.push_back(token);
            } else if (!m.id) {
                m.id = token;
            } else {
                m.details.push_back(token);
            }
        }
        if (j == std::string::npos) break;
        pos = j + 1;
    }
    return m;
}

}  // namespace

Graph parse_gantt_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::Gantt;
    graph.direction = Direction::LeftRight;
    graph.gantt_display_mode = extract_frontmatter_value(input, "displayMode");
    std::optional<std::string> current_section_name;
    size_t task_count = 0;  // (the crate also adds nodes / edges; the layout never reads them)

    for (const std::string& raw_line : pp::preprocess_input(input)) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        std::string lower = to_ascii_lower(line);
        if (starts_with(lower, "gantt")) continue;
        if (starts_with(lower, "title")) {
            std::string title = trim(line.substr(5));
            if (!title.empty()) graph.gantt_title = title;
            continue;
        }
        if (starts_with(lower, "dateformat") || starts_with(lower, "axisformat") || starts_with(lower, "tickinterval") ||
            starts_with(lower, "todaymarker") || starts_with(lower, "excludes") || starts_with(lower, "includes"))
            continue;
        if (starts_with(lower, "section")) {
            std::string label = trim(line.substr(7));
            current_section_name = label;
            graph.gantt_sections.push_back(label);
            continue;
        }
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string label = trim(line.substr(0, colon));
        if (label.empty()) continue;
        TaskMeta meta = parse_gantt_task_meta(line.substr(colon + 1));
        std::string node_id = meta.id ? *meta.id : "gantt_" + std::to_string(task_count);
        ++task_count;
        GanttTask t;
        t.id = node_id;
        t.label = label;
        for (const std::string& d : meta.details) {
            if (looks_like_date(d) && !t.start) t.start = d;
            else if (looks_like_duration(d) && !t.duration) t.duration = d;
        }
        t.after = meta.after;
        t.section = current_section_name;
        t.status = meta.status;
        graph.gantt_tasks.push_back(std::move(t));
    }
    return graph;
}

// ======================================================================================
// layout
// ======================================================================================

namespace {

std::vector<std::string> gantt_palette(const Theme& theme) {
    return {theme.primary_border_color, "#0ea5e9", "#10b981", "#6366f1", "#f97316"};
}

std::string hsl_color(float h, float s, float l) { return "hsl(" + fmt_fixed(h, 10) + ", " + fmt_fixed(s, 10) + "%, " + fmt_fixed(l, 10) + "%)"; }

std::string shift_color(const std::string& color, float target_s, float target_l, float strength) {
    float h, s, l;
    if (!parse_color_to_hsl(color, h, s, l)) return color;
    float delta_s = (target_s - s) * strength;
    float delta_l = (target_l - l) * strength;
    return adjust_color(color, 0.0f, delta_s, delta_l);
}

std::map<std::string, std::string> gantt_section_palette(const Theme& theme, const std::vector<std::string>& sections) {
    std::map<std::string, std::string> map;
    if (sections.empty()) return map;
    const std::string& base = theme.primary_border_color;
    float step = 360.0f / static_cast<float>(std::max<size_t>(sections.size(), 1));
    for (size_t idx = 0; idx < sections.size(); ++idx) {
        float hue_shift = step * static_cast<float>(idx);
        std::string color = adjust_color(base, hue_shift, 0.0f, 0.0f);
        color = shift_color(color, 60.0f, 55.0f, 0.4f);
        map[sections[idx]] = color;
    }
    return map;
}

std::string gantt_task_color(std::optional<GanttStatus> status, const std::string& base_in, const std::string& fallback) {
    float h, s, l;
    std::string base = parse_color_to_hsl(base_in, h, s, l) ? base_in : fallback;
    bool ok = parse_color_to_hsl(base, h, s, l);
    auto clampf = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    if (!status) return base;
    switch (*status) {
    case GanttStatus::Done: return shift_color(base, 30.0f, 80.0f, 0.7f);
    case GanttStatus::Active: return shift_color(base, 70.0f, 52.0f, 0.6f);
    case GanttStatus::Crit: return ok ? hsl_color(0.0f, fmax_(s, 65.0f), clampf(l, 45.0f, 60.0f)) : "#ef4444";
    case GanttStatus::Milestone: return ok ? hsl_color(45.0f, fmax_(s, 65.0f), clampf(l, 50.0f, 65.0f)) : "#f59e0b";
    }
    return base;
}

std::optional<float> parse_gantt_duration(const std::string& value_in) {
    std::string value = trim(value_in);
    if (value.empty()) return std::nullopt;
    std::string digits;
    char unit = 0;
    for (char ch : value) {
        if ((ch >= '0' && ch <= '9') || ch == '.') digits += ch;
        else if (!is_space(ch)) unit = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
    }
    float number;
    if (!parse_rust_f32(digits, number)) return std::nullopt;
    float mult;
    switch (unit) {
    case 'd': mult = 1.0f; break;
    case 'w': mult = 7.0f; break;
    case 'h': mult = 1.0f / 24.0f; break;
    case 'm': mult = 30.0f; break;
    case 'y': mult = 365.0f; break;
    default: mult = 1.0f; break;
    }
    return number * mult;
}

// Rust integer parse: optional '+', then digits only.
bool parse_rust_uint(const std::string& t, long long& out) {
    size_t i = (!t.empty() && t[0] == '+') ? 1 : 0;
    if (i >= t.size()) return false;
    long long v = 0;
    for (; i < t.size(); ++i) {
        if (t[i] < '0' || t[i] > '9') return false;
        v = v * 10 + (t[i] - '0');
        if (v > 4000000000LL) return false;
    }
    out = v;
    return true;
}

int floordiv(int a, int b) { return (a >= 0 ? a : a - (b - 1)) / b; }

int days_from_civil(int year, int month, int day) {
    int y = year - (month <= 2 ? 1 : 0);
    int era = floordiv(y, 400);
    int yoe = y - era * 400;
    int m = month, d = day;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civil_from_days(int days, int& year, int& month, int& day) {
    int z = days + 719468;
    int era = floordiv(z, 146097);
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    int d = doy - (153 * mp + 2) / 5 + 1;
    int m = mp + (mp < 10 ? 3 : -9);
    year = y + (m <= 2 ? 1 : 0);
    month = m;
    day = d;
}

std::optional<int> parse_gantt_date(const std::string& value_in) {
    std::string value = trim(value_in);
    if (value.empty()) return std::nullopt;
    std::vector<std::string> parts;
    std::string cur;
    for (char c : value) {
        if (c == '-' || c == '/' || c == '.') { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);
    if (parts.size() != 3) return std::nullopt;
    long long y, m, d;
    if (!parse_rust_uint(parts[0], y) || !parse_rust_uint(parts[1], m) || !parse_rust_uint(parts[2], d)) return std::nullopt;
    if (m == 0 || m > 12 || d == 0 || d > 31) return std::nullopt;
    return days_from_civil(static_cast<int>(y), static_cast<int>(m), static_cast<int>(d));
}

std::string format_gantt_date(int days) {
    int y, m, d;
    civil_from_days(days, y, m, d);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
    return buf;
}

}  // namespace

Layout compute_gantt_layout(const Graph& graph, const Theme& theme, const LayoutConfig& config) {
    float padding = theme.font_size * 1.25f;
    float row_height = fmax_(theme.font_size * 1.5f, theme.font_size + 8.0f);
    float label_gap = theme.font_size * 1.05f;
    float default_duration = 3.0f;

    std::optional<TextBlock> title;
    if (graph.gantt_title) title = measure_label(*graph.gantt_title, theme, config);
    float title_height = title ? title->height + padding : 0.0f;

    float task_label_width = 0.0f, section_label_width = 0.0f;
    for (const auto& task : graph.gantt_tasks) {
        TextBlock label = measure_label(task.label, theme, config);
        task_label_width = fmax_(task_label_width, label.width);
        if (task.section) section_label_width = fmax_(section_label_width, measure_label(*task.section, theme, config).width);
    }
    task_label_width = fmax_(task_label_width, theme.font_size * 6.5f);

    float label_x = padding;
    float section_task_gap = section_label_width > 0.0f ? theme.font_size * 0.8f : 0.0f;
    float label_width = section_label_width + section_task_gap + task_label_width;
    float section_label_x = label_x;
    float task_label_x = label_x + section_label_width + section_task_gap;
    float chart_x = padding + label_width + label_gap;
    float chart_y = title_height + padding;
    float chart_width = theme.font_size * 26.0f;

    std::map<std::string, float> parsed_starts;
    std::optional<float> origin;
    for (const auto& task : graph.gantt_tasks) {
        if (task.start)
            if (auto start = parse_gantt_date(*task.start)) {
                float s = static_cast<float>(*start);
                parsed_starts[task.id] = s;
                origin = origin ? fmin_(*origin, s) : s;
            }
    }
    bool has_dates = origin.has_value();

    std::map<std::string, std::pair<float, float>> timing;
    float cursor = 0.0f;
    float time_start = std::numeric_limits<float>::max(), time_end = std::numeric_limits<float>::lowest();
    struct Computed {
        std::string label;
        float start, duration;
        std::optional<GanttStatus> status;
        std::optional<std::string> section;
    };
    std::vector<Computed> computed;
    for (const auto& task : graph.gantt_tasks) {
        float duration = default_duration;
        if (task.duration)
            if (auto d = parse_gantt_duration(*task.duration)) duration = *d;
        duration = fmax_(duration, 0.1f);
        std::optional<float> start;
        if (auto it = parsed_starts.find(task.id); it != parsed_starts.end()) start = it->second;
        if (!start && task.after)
            if (auto it = timing.find(*task.after); it != timing.end()) start = it->second.second;
        float fallback_base = origin ? *origin : 0.0f;
        float st = start ? *start : fallback_base + cursor;
        float end = st + duration;
        timing[task.id] = {st, end};
        cursor = fmax_(cursor, end + 0.5f);
        time_start = fmin_(time_start, st);
        time_end = fmax_(time_end, end);
        computed.push_back({task.label, st, duration, task.status, task.section});
    }
    if (!std::isfinite(time_start) || !std::isfinite(time_end)) {
        time_start = 0.0f;
        time_end = 1.0f;
    }
    if (std::fabs(time_end - time_start) < 0.01f) time_end = time_start + 1.0f;
    float time_span = fmax_(time_end - time_start, 1.0f);
    float time_scale = chart_width / time_span;

    std::vector<GanttTick> ticks;
    const int tick_count = 4;
    for (int i = 0; i <= tick_count; ++i) {
        float t = time_start + time_span * static_cast<float>(i) / static_cast<float>(tick_count);
        float x = chart_x + (t - time_start) * time_scale;
        std::string label = has_dates ? format_gantt_date(static_cast<int>(std::round(t))) : fmt_fixed(t - time_start, 0);
        ticks.push_back({x, label});
    }

    bool compact = false;
    if (graph.gantt_display_mode) compact = to_ascii_lower(*graph.gantt_display_mode) == "compact";

    std::vector<std::string> palette = gantt_palette(theme);
    auto section_palette = gantt_section_palette(theme, graph.gantt_sections);
    std::optional<std::string> current_section;
    std::optional<size_t> current_section_idx;
    GanttLayout g;
    float y = chart_y;
    std::vector<std::pair<float, float>> lanes;

    for (size_t idx = 0; idx < computed.size(); ++idx) {
        const Computed& c = computed[idx];
        if (c.section != current_section) {
            if (c.section) {
                if (current_section_idx) g.sections[*current_section_idx].height = fmax_(y - g.sections[*current_section_idx].y, row_height);
                lanes.clear();
                std::string base_color;
                if (auto it = section_palette.find(*c.section); it != section_palette.end()) base_color = it->second;
                else base_color = palette[idx % palette.size()];
                std::string band_color = shift_color(base_color, 20.0f, 92.0f, 0.7f);
                GanttSectionLayout sec;
                sec.label = measure_label(*c.section, theme, config);
                sec.y = y;
                sec.height = 0.0f;
                sec.color = base_color;
                sec.band_color = band_color;
                g.sections.push_back(std::move(sec));
                current_section_idx = g.sections.size() - 1;
            } else if (current_section_idx) {
                g.sections[*current_section_idx].height = fmax_(y - g.sections[*current_section_idx].y, row_height);
                current_section_idx.reset();
                lanes.clear();
            }
            current_section = c.section;
        }
        float bar_x = chart_x + (c.start - time_start) * time_scale;
        float bar_width = c.duration * time_scale;
        float min_width = row_height * 0.5f;
        if (bar_width < min_width) bar_width = min_width;
        std::string base_color;
        if (c.section) {
            if (auto it = section_palette.find(*c.section); it != section_palette.end()) base_color = it->second;
            else base_color = palette[idx % palette.size()];
        } else {
            base_color = palette[idx % palette.size()];
        }
        std::string color = gantt_task_color(c.status, base_color, palette[0]);
        float task_end = c.start + c.duration;
        float task_y;
        if (compact) {
            std::pair<float, float>* found = nullptr;
            for (auto& lane : lanes)
                if (c.start >= lane.second) { found = &lane; break; }
            if (found) {
                task_y = found->first;
                found->second = task_end;
            } else {
                task_y = y;
                lanes.push_back({task_y, task_end});
                y += row_height;
            }
        } else {
            task_y = y;
            y += row_height;
        }
        GanttTaskLayout t;
        t.label = measure_label(c.label, theme, config);
        t.x = bar_x;
        t.y = task_y;
        t.width = bar_width;
        t.height = row_height;
        t.color = color;
        t.start = c.start;
        t.duration = c.duration;
        t.status = c.status;
        g.tasks.push_back(std::move(t));
    }
    if (current_section_idx) g.sections[*current_section_idx].height = fmax_(y - g.sections[*current_section_idx].y, row_height);

    float tick_font = theme.font_size * 0.8f;
    float max_tick_half_width = 0.0f;
    for (const auto& tick : ticks)
        max_tick_half_width = fmax_(max_tick_half_width, measure_label_with_font_size(tick.label, tick_font, config, false).width / 2.0f);
    float axis_pad = row_height * 0.9f + theme.font_size;
    float height = y + padding + axis_pad;
    float label_overflow = compact ? padding + task_label_width : 0.0f;
    float right_padding = fmax_(fmax_(max_tick_half_width + padding * 0.4f, label_overflow), padding);
    float width = chart_x + chart_width + right_padding;

    g.title = title;
    g.time_start = time_start;
    g.time_end = time_end;
    g.chart_x = chart_x;
    g.chart_y = chart_y;
    g.chart_width = chart_width;
    g.chart_height = y - chart_y;
    g.row_height = row_height;
    g.label_x = label_x;
    g.label_width = label_width;
    g.section_label_x = section_label_x;
    g.section_label_width = section_label_width;
    g.task_label_x = task_label_x;
    g.task_label_width = task_label_width;
    g.title_y = chart_y - row_height * 0.6f;
    g.ticks = ticks;
    g.compact = compact;

    Layout layout;
    layout.kind = graph.kind;
    layout.width = width;
    layout.height = height;
    layout.gantt = std::move(g);
    return layout;
}

// ======================================================================================
// writer
// ======================================================================================

std::string render_gantt_svg(const Layout& layout, const Theme& theme, const LayoutConfig& config) {
    using svg::escape_xml;
    using svg::F;
    using svg::F2;
    const GanttLayout& gl = *layout.gantt;
    float width = fmax_(layout.width, 1.0f), height = fmax_(layout.height, 1.0f);
    std::string s = svg::document_start(width, height, 0.0f, 0.0f, width, height, false, false, theme);

    float chart_left = gl.chart_x, chart_right = gl.chart_x + gl.chart_width;
    float full_width = chart_right + gl.label_x;
    float bar_height = fmax_(fmin_(gl.row_height * 0.82f, gl.row_height - 4.0f), theme.font_size * 1.1f);

    if (gl.title)
        s += svg::text_block(gl.chart_x + gl.chart_width / 2.0f, gl.title_y, *gl.title, theme, config, theme.font_size, "middle",
                             theme.primary_text_color, false);

    float axis_y = gl.chart_y + gl.chart_height + gl.row_height * 0.85f;
    float tick_font = theme.font_size * 0.8f;
    for (const auto& tick : gl.ticks) {
        s += "<line x1=\"" + F2(tick.x) + "\" y1=\"" + F2(gl.chart_y) + "\" x2=\"" + F2(tick.x) + "\" y2=\"" + F2(gl.chart_y + gl.chart_height) +
             "\" stroke=\"#E2E8F0\" stroke-width=\"1\"/>";
        if (!trim_view(tick.label).empty()) s += svg::text_line(tick.x, axis_y, tick.label, theme, tick_font, theme.text_color, "middle");
    }
    s += "<line x1=\"" + F2(chart_left) + "\" y1=\"" + F2(gl.chart_y + gl.chart_height) + "\" x2=\"" + F2(chart_right) + "\" y2=\"" +
         F2(gl.chart_y + gl.chart_height) + "\" stroke=\"" + theme.line_color + "\" stroke-width=\"1\"/>";
    s += "<line x1=\"" + F2(chart_left) + "\" y1=\"" + F2(gl.chart_y) + "\" x2=\"" + F2(chart_left) + "\" y2=\"" + F2(gl.chart_y + gl.chart_height) +
         "\" stroke=\"#E2E8F0\" stroke-width=\"1\"/>";

    float section_font = theme.font_size * 0.9f, task_font = theme.font_size * 0.85f;
    for (const auto& section : gl.sections) {
        s += "<rect x=\"" + F2(0.0f) + "\" y=\"" + F2(section.y) + "\" width=\"" + F2(gl.chart_x) + "\" height=\"" + F2(section.height) +
             "\" fill=\"" + section.band_color + "\" fill-opacity=\"0.22\" stroke=\"none\"/>";
        s += "<rect x=\"" + F2(gl.chart_x) + "\" y=\"" + F2(section.y) + "\" width=\"" + F2(gl.chart_width) + "\" height=\"" + F2(section.height) +
             "\" fill=\"" + section.band_color + "\" fill-opacity=\"0.12\" stroke=\"none\"/>";
        s += "<rect x=\"" + F2(0.0f) + "\" y=\"" + F2(section.y) + "\" width=\"" + F2(fmax_(theme.font_size * 0.3f, 3.0f)) + "\" height=\"" +
             F2(section.height) + "\" fill=\"" + section.color + "\" fill-opacity=\"0.9\" stroke=\"none\"/>";
        float label_y = fmin_(section.y + gl.row_height * 0.55f, section.y + section.height - gl.row_height * 0.45f);
        s += svg::text_block(gl.section_label_x, label_y, section.label, theme, config, section_font, "start", theme.primary_text_color, false);
    }

    std::vector<float> row_lines{gl.chart_y};
    for (const auto& section : gl.sections) {
        row_lines.push_back(section.y);
        row_lines.push_back(section.y + section.height);
    }
    for (const auto& task : gl.tasks) row_lines.push_back(task.y);
    row_lines.push_back(gl.chart_y + gl.chart_height);
    std::stable_sort(row_lines.begin(), row_lines.end(), [](float a, float b) { return a < b; });
    std::vector<float> deduped;  // Vec::dedup_by: drop an element within 0.5 of the previously kept one
    for (float v : row_lines)
        if (deduped.empty() || !(std::fabs(v - deduped.back()) < 0.5f)) deduped.push_back(v);
    for (float y : deduped)
        s += "<line x1=\"" + F2(0.0f) + "\" y1=\"" + F2(y) + "\" x2=\"" + F2(full_width) + "\" y2=\"" + F2(y) + "\" stroke=\"#E2E8F0\" stroke-width=\"1\"/>";

    auto gantt_label_color = [&](const std::string& fill) -> std::string {
        float h, sat, l;
        if (parse_color_to_hsl(fill, h, sat, l)) return l < 55.0f ? "#FFFFFF" : "#0F172A";
        return theme.primary_text_color;
    };

    for (const auto& task : gl.tasks) {
        float row_center = task.y + gl.row_height / 2.0f;
        float bar_y = row_center - bar_height / 2.0f;
        bool label_rendered_inside = false;
        bool milestone = task.status == GanttStatus::Milestone;
        if (milestone) {
            float size = bar_height * 0.6f;
            float cx = task.x, cy = row_center;
            std::string points = F2(cx) + "," + F2(cy - size) + " " + F2(cx + size) + "," + F2(cy) + " " + F2(cx) + "," + F2(cy + size) + " " +
                                 F2(cx - size) + "," + F2(cy);
            s += "<polygon points=\"" + points + "\" fill=\"" + task.color + "\" stroke=\"" + theme.primary_border_color + "\" stroke-width=\"1\"/>";
        } else {
            s += "<rect x=\"" + F2(task.x) + "\" y=\"" + F2(bar_y) + "\" width=\"" + F2(task.width) + "\" height=\"" + F2(bar_height) +
                 "\" rx=\"3\" fill=\"" + task.color + "\" stroke=\"" + theme.primary_border_color + "\" stroke-width=\"1\"/>";
            std::string label_text;
            for (const auto& line : task.label.lines)
                if (!trim_view(line).empty()) { label_text = line; break; }
            if (!label_text.empty()) {
                float font_size = task_font * 0.95f;
                float text_width = measure_text_width(label_text, font_size);
                float pad = fmax_(font_size * 0.6f, 6.0f);
                if (task.width >= text_width + pad * 2.0f && bar_height >= font_size * 1.1f) {
                    float text_x = task.x + task.width / 2.0f, text_y = row_center;
                    s += "<text x=\"" + F2(text_x) + "\" y=\"" + F2(text_y) + "\" text-anchor=\"middle\" dominant-baseline=\"middle\" font-family=\"" +
                         svg::normalize_font_family(theme.font_family) + "\" font-size=\"" + F2(font_size) + "\" fill=\"" +
                         escape_xml(gantt_label_color(task.color)) + "\">" + escape_xml(label_text) + "</text>";
                    label_rendered_inside = true;
                }
            }
        }
        if (!label_rendered_inside) {
            float label_x;
            if (gl.compact) {
                float gap = theme.font_size * 0.4f;
                if (milestone) label_x = task.x + bar_height * 0.6f + gap;
                else label_x = task.x + task.width + gap;
            } else {
                label_x = gl.task_label_x;
            }
            s += svg::text_block(label_x, row_center, task.label, theme, config, task_font, "start", theme.primary_text_color, false);
        }
    }
    s += "</svg>";
    return s;
}

}  // namespace mermaid
