// Mermaid port: C4 diagrams. Mirrors parser.rs parse_c4_diagram (and helpers), layout/c4.rs and render.rs render_c4.
// The crate's C4 config (config.rs C4Config) has one font size / family / weight per shape kind, but they
// all default to the same values and %%{init}%% cannot change them (the config is ignored), so the
// defaults are constants here.
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "kinds.h"
#include "kind_util.h"
#include "numfmt.h"
#include "parser_internal.h"
#include "strutil.h"
#include "svg_common.h"
#include "text.h"

namespace mermaid {

namespace {

// ---- config.rs C4Config defaults ----
constexpr float kDiagramMarginX = 32.0f, kDiagramMarginY = 8.0f;
constexpr float kShapeMargin = 32.0f, kShapePadding = 16.0f;
constexpr float kShapeWidth = 200.0f, kShapeHeight = 56.0f;
constexpr size_t kDefaultShapeInRow = 4, kDefaultBoundaryInRow = 2;
constexpr float kNextLinePaddingX = 0.0f;
constexpr float kTextLineHeight = 1.0f, kTextLineHeightSmallAdd = 1.0f, kTextLineHeightSmallThreshold = 14.0f;
constexpr float kShapeCornerRadius = 2.5f, kShapeStrokeWidth = 0.5f, kBoundaryCornerRadius = 2.5f;
constexpr float kPersonIconSize = 48.0f, kDbEllipseHeight = 10.0f, kQueueCurveRadius = 5.0f;
const char* const kBoundaryStroke = "#444444";
const char* const kBoundaryDasharray = "7.0,7.0";
constexpr float kBoundaryStrokeWidth = 1.0f;
const char* const kBoundaryFill = "none";
constexpr float kBoundaryFontSize = 14.0f, kMessageFontSize = 12.0f, kShapeFontSize = 14.0f;
const char* const kFontFamily = "\"Open Sans\", sans-serif";
const char* const kFontWeight = "normal";

// Default background / border colours per shape kind.
struct KindColors {
    const char* bg;
    const char* border;
};

KindColors shape_colors(C4ShapeKind kind) {
    switch (kind) {
    case C4ShapeKind::Person: return {"#08427B", "#073B6F"};
    case C4ShapeKind::ExternalPerson: return {"#686868", "#8A8A8A"};
    case C4ShapeKind::System:
    case C4ShapeKind::SystemDb:
    case C4ShapeKind::SystemQueue: return {"#1168BD", "#3C7FC0"};
    case C4ShapeKind::ExternalSystem:
    case C4ShapeKind::ExternalSystemDb:
    case C4ShapeKind::ExternalSystemQueue: return {"#999999", "#8A8A8A"};
    case C4ShapeKind::Container:
    case C4ShapeKind::ContainerDb:
    case C4ShapeKind::ContainerQueue: return {"#438DD5", "#3C7FC0"};
    case C4ShapeKind::ExternalContainer:
    case C4ShapeKind::ExternalContainerDb:
    case C4ShapeKind::ExternalContainerQueue: return {"#B3B3B3", "#A6A6A6"};
    case C4ShapeKind::Component:
    case C4ShapeKind::ComponentDb:
    case C4ShapeKind::ComponentQueue: return {"#85BBF0", "#78A8D8"};
    case C4ShapeKind::ExternalComponent:
    case C4ShapeKind::ExternalComponentDb:
    case C4ShapeKind::ExternalComponentQueue: return {"#CCCCCC", "#BFBFBF"};
    }
    return {"#1168BD", "#3C7FC0"};
}

bool kind_is_db(C4ShapeKind k) {
    return k == C4ShapeKind::SystemDb || k == C4ShapeKind::ExternalSystemDb || k == C4ShapeKind::ContainerDb ||
           k == C4ShapeKind::ExternalContainerDb || k == C4ShapeKind::ComponentDb || k == C4ShapeKind::ExternalComponentDb;
}
bool kind_is_queue(C4ShapeKind k) {
    return k == C4ShapeKind::SystemQueue || k == C4ShapeKind::ExternalSystemQueue || k == C4ShapeKind::ContainerQueue ||
           k == C4ShapeKind::ExternalContainerQueue || k == C4ShapeKind::ComponentQueue || k == C4ShapeKind::ExternalComponentQueue;
}
bool kind_is_person(C4ShapeKind k) { return k == C4ShapeKind::Person || k == C4ShapeKind::ExternalPerson; }

// ir.rs C4ShapeKind::as_str
const char* kind_name(C4ShapeKind k) {
    switch (k) {
    case C4ShapeKind::Person: return "person";
    case C4ShapeKind::ExternalPerson: return "external_person";
    case C4ShapeKind::System: return "system";
    case C4ShapeKind::SystemDb: return "system_db";
    case C4ShapeKind::SystemQueue: return "system_queue";
    case C4ShapeKind::ExternalSystem: return "external_system";
    case C4ShapeKind::ExternalSystemDb: return "external_system_db";
    case C4ShapeKind::ExternalSystemQueue: return "external_system_queue";
    case C4ShapeKind::Container: return "container";
    case C4ShapeKind::ContainerDb: return "container_db";
    case C4ShapeKind::ContainerQueue: return "container_queue";
    case C4ShapeKind::ExternalContainer: return "external_container";
    case C4ShapeKind::ExternalContainerDb: return "external_container_db";
    case C4ShapeKind::ExternalContainerQueue: return "external_container_queue";
    case C4ShapeKind::Component: return "component";
    case C4ShapeKind::ComponentDb: return "component_db";
    case C4ShapeKind::ComponentQueue: return "component_queue";
    case C4ShapeKind::ExternalComponent: return "external_component";
    case C4ShapeKind::ExternalComponentDb: return "external_component_db";
    case C4ShapeKind::ExternalComponentQueue: return "external_component_queue";
    }
    return "";
}

// ---------------------------------------------------------------- parser

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// parser.rs parse_function_call: "Func(args...)" -> (func, args with quotes stripped).
std::optional<std::pair<std::string, std::vector<std::string>>> parse_function_call(const std::string& line) {
    std::string trimmed = trim(line);
    size_t open = trimmed.find('(');
    size_t close = trimmed.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open) return std::nullopt;
    std::string func = trim(trimmed.substr(0, open));
    std::vector<std::string> args;
    for (const std::string& arg : split_args(trimmed.substr(open + 1, close - open - 1))) args.push_back(pp::strip_quotes(trim(arg)));
    if (func.empty()) return std::nullopt;
    return std::make_pair(func, args);
}

struct C4Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> kv;
    const std::string* get(const char* key) const {
        auto it = kv.find(key);
        return it == kv.end() ? nullptr : &it->second;
    }
};

// parser.rs parse_c4_args: "key=value" (also "$key=value") goes to kv, everything else is positional.
C4Args parse_c4_args(const std::vector<std::string>& args) {
    C4Args out;
    for (const std::string& arg : args) {
        std::string trimmed = trim(arg);
        size_t eq = trimmed.find('=');
        if (eq != std::string::npos) {
            std::string key = trim(trimmed.substr(0, eq));
            size_t k = 0;
            while (k < key.size() && key[k] == '$') ++k;
            key = key.substr(k);
            std::string value = trim(trimmed.substr(eq + 1));
            if (!key.empty()) {
                out.kv[key] = value;
                continue;
            }
        }
        if (!trimmed.empty()) out.positional.push_back(trimmed);
    }
    return out;
}

std::string normalize_c4_key(const std::string& key) {
    std::string out;
    for (char ch : key) {
        if (ch == '_' || ch == '-') continue;
        out += (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
    }
    return out;
}

std::optional<std::string> get_c4_kv(const C4Args& a, const char* key) {
    std::string target = normalize_c4_key(key);
    for (const auto& [k, v] : a.kv)
        if (normalize_c4_key(k) == target) return v;
    return std::nullopt;
}

std::optional<std::string> positional_at(const C4Args& a, size_t i) {
    if (i < a.positional.size()) return a.positional[i];
    return std::nullopt;
}
std::optional<std::string> kv_opt(const C4Args& a, const char* key) {
    if (const std::string* v = a.get(key)) return *v;
    return std::nullopt;
}
// Option::or_else chain
std::optional<std::string> either(std::optional<std::string> a, const std::optional<std::string>& b) { return a ? a : b; }

bool is_c4_boundary(const std::string& f) {
    return contains(f, "boundary") || starts_with(f, "deployment_node") || f == "node" || f == "node_l" || f == "node_r";
}

std::string c4_boundary_default_type(const std::string& f) {
    if (contains(f, "enterprise")) return "enterprise";
    if (contains(f, "container")) return "container";
    if (contains(f, "system")) return "system";
    if (contains(f, "node")) return "node";
    return "system";
}

std::optional<C4ShapeKind> c4_shape_kind_for(const std::string& func_lower) {
    std::string f = replace_all(func_lower, "-", "_");
    bool is_ext = contains(f, "ext");
    if (contains(f, "person")) return is_ext ? C4ShapeKind::ExternalPerson : C4ShapeKind::Person;
    auto pick = [&](C4ShapeKind plain, C4ShapeKind db, C4ShapeKind queue, C4ShapeKind ext, C4ShapeKind ext_db, C4ShapeKind ext_queue) {
        bool is_db = contains(f, "db"), is_queue = contains(f, "queue");
        if (is_ext && is_db) return ext_db;
        if (is_ext && is_queue) return ext_queue;
        if (is_ext) return ext;
        if (is_db) return db;
        if (is_queue) return queue;
        return plain;
    };
    if (contains(f, "system"))
        return pick(C4ShapeKind::System, C4ShapeKind::SystemDb, C4ShapeKind::SystemQueue, C4ShapeKind::ExternalSystem,
                    C4ShapeKind::ExternalSystemDb, C4ShapeKind::ExternalSystemQueue);
    if (contains(f, "container"))
        return pick(C4ShapeKind::Container, C4ShapeKind::ContainerDb, C4ShapeKind::ContainerQueue, C4ShapeKind::ExternalContainer,
                    C4ShapeKind::ExternalContainerDb, C4ShapeKind::ExternalContainerQueue);
    if (contains(f, "component"))
        return pick(C4ShapeKind::Component, C4ShapeKind::ComponentDb, C4ShapeKind::ComponentQueue, C4ShapeKind::ExternalComponent,
                    C4ShapeKind::ExternalComponentDb, C4ShapeKind::ExternalComponentQueue);
    return std::nullopt;
}

bool kind_uses_techn(C4ShapeKind k) {
    switch (k) {
    case C4ShapeKind::Container: case C4ShapeKind::ContainerDb: case C4ShapeKind::ContainerQueue:
    case C4ShapeKind::ExternalContainer: case C4ShapeKind::ExternalContainerDb: case C4ShapeKind::ExternalContainerQueue:
    case C4ShapeKind::Component: case C4ShapeKind::ComponentDb: case C4ShapeKind::ComponentQueue:
    case C4ShapeKind::ExternalComponent: case C4ShapeKind::ExternalComponentDb: case C4ShapeKind::ExternalComponentQueue: return true;
    default: return false;
    }
}

std::optional<C4RelKind> c4_rel_kind_for(const std::string& func_lower) {
    std::string f = replace_all(func_lower, "-", "_");
    if (starts_with(f, "birel")) return C4RelKind::BiRel;
    if (starts_with(f, "rel_u") || starts_with(f, "rel_up")) return C4RelKind::RelUp;
    if (starts_with(f, "rel_d") || starts_with(f, "rel_down")) return C4RelKind::RelDown;
    if (starts_with(f, "rel_l") || starts_with(f, "rel_left")) return C4RelKind::RelLeft;
    if (starts_with(f, "rel_r") || starts_with(f, "rel_right")) return C4RelKind::RelRight;
    if (starts_with(f, "rel_b") || starts_with(f, "rel_back")) return C4RelKind::RelBack;
    if (starts_with(f, "rel") || starts_with(f, "relindex")) return C4RelKind::Rel;
    return std::nullopt;
}

// Rust str::parse::<usize>() (digits, optional leading '+').
bool parse_usize(const std::string& t, size_t& out) {
    size_t i = (!t.empty() && t[0] == '+') ? 1 : 0;
    if (i >= t.size()) return false;
    size_t v = 0;
    for (; i < t.size(); ++i) {
        if (t[i] < '0' || t[i] > '9') return false;
        v = v * 10 + static_cast<size_t>(t[i] - '0');
    }
    out = v;
    return true;
}

void process_c4_line(const std::string& line, C4Data& c4, std::vector<std::string>& boundary_stack) {
    auto call = parse_function_call(line);
    if (!call) return;
    const std::string& func = call->first;
    std::string func_lower = to_ascii_lower(func);
    C4Args args = parse_c4_args(call->second);
    const std::vector<std::string>& positional = args.positional;

    if (is_c4_boundary(func_lower)) {
        std::string id = positional.empty() ? "boundary_" + std::to_string(c4.boundaries.size()) : positional[0];
        std::string label = positional.size() > 1 ? positional[1] : id;
        std::optional<std::string> boundary_type = positional_at(args, 2);
        if (const std::string* v = args.get("type")) boundary_type = *v;
        C4Boundary b;
        b.id = id;
        b.label = label;
        b.boundary_type = boundary_type ? *boundary_type : c4_boundary_default_type(func_lower);
        b.descr = either(kv_opt(args, "descr"), kv_opt(args, "description"));
        b.sprite = kv_opt(args, "sprite");
        b.tags = kv_opt(args, "tags");
        b.link = kv_opt(args, "link");
        b.parent_boundary = boundary_stack.empty() ? "" : boundary_stack.back();
        c4.boundaries.push_back(std::move(b));
        boundary_stack.push_back(id);
        return;
    }

    if (auto rel_kind = c4_rel_kind_for(func_lower)) {
        std::vector<std::string> rel_args = positional;
        if (starts_with(func_lower, "relindex") && rel_args.size() > 1) rel_args.erase(rel_args.begin());
        if (rel_args.size() >= 3) {
            auto at = [&](size_t i) -> std::optional<std::string> {
                if (i < rel_args.size()) return rel_args[i];
                return std::nullopt;
            };
            C4Rel r;
            r.kind = *rel_kind;
            r.from = rel_args[0];
            r.to = rel_args[1];
            r.label = rel_args[2];
            r.techn = either(at(3), kv_opt(args, "techn"));
            r.descr = either(at(4), kv_opt(args, "descr"));
            r.sprite = either(at(5), kv_opt(args, "sprite"));
            r.tags = either(at(6), kv_opt(args, "tags"));
            r.link = either(at(7), kv_opt(args, "link"));
            c4.rels.push_back(std::move(r));
        }
        return;
    }

    if (func_lower == "updateelementstyle" || func_lower == "update_el_style" || func_lower == "updateelstyle") {
        std::optional<std::string> element = either(positional_at(args, 0), get_c4_kv(args, "element"));
        if (element) {
            auto bg_color = either(get_c4_kv(args, "bgColor"), positional_at(args, 1));
            auto font_color = either(get_c4_kv(args, "fontColor"), positional_at(args, 2));
            auto border_color = either(get_c4_kv(args, "borderColor"), positional_at(args, 3));
            auto sprite = either(get_c4_kv(args, "sprite"), positional_at(args, 6));
            auto techn = either(get_c4_kv(args, "techn"), positional_at(args, 7));
            auto shape = std::find_if(c4.shapes.begin(), c4.shapes.end(), [&](const C4Shape& s) { return s.id == *element; });
            if (shape != c4.shapes.end()) {
                if (bg_color) shape->bg_color = bg_color;
                if (font_color) shape->font_color = font_color;
                if (border_color) shape->border_color = border_color;
                if (sprite) shape->sprite = sprite;
                if (techn) shape->techn = techn;
            } else {
                auto boundary = std::find_if(c4.boundaries.begin(), c4.boundaries.end(), [&](const C4Boundary& b) { return b.id == *element; });
                if (boundary != c4.boundaries.end()) {
                    if (bg_color) boundary->bg_color = bg_color;
                    if (font_color) boundary->font_color = font_color;
                    if (border_color) boundary->border_color = border_color;
                    if (sprite) boundary->sprite = sprite;
                }
            }
        }
        return;
    }

    if (func_lower == "updaterelstyle" || func_lower == "update_rel_style") {
        auto from = either(positional_at(args, 0), get_c4_kv(args, "from"));
        auto to = either(positional_at(args, 1), get_c4_kv(args, "to"));
        if (from && to) {
            auto rel = std::find_if(c4.rels.begin(), c4.rels.end(), [&](const C4Rel& r) { return r.from == *from && r.to == *to; });
            if (rel != c4.rels.end()) {
                auto text_color = either(get_c4_kv(args, "textColor"), positional_at(args, 2));
                auto line_color = either(get_c4_kv(args, "lineColor"), positional_at(args, 3));
                auto offset_x = either(get_c4_kv(args, "offsetX"), positional_at(args, 4));
                auto offset_y = either(get_c4_kv(args, "offsetY"), positional_at(args, 5));
                if (text_color) rel->text_color = text_color;
                if (line_color) rel->line_color = line_color;
                float num;
                if (offset_x && parse_rust_f32(trim(*offset_x), num)) rel->offset_x = num;
                if (offset_y && parse_rust_f32(trim(*offset_y), num)) rel->offset_y = num;
            }
        }
        return;
    }

    if (func_lower == "updatelayoutconfig" || func_lower == "update_layout_config") {
        auto shape_in_row = either(get_c4_kv(args, "c4ShapeInRow"), positional_at(args, 0));
        auto boundary_in_row = either(get_c4_kv(args, "c4BoundaryInRow"), positional_at(args, 1));
        size_t num;
        if (shape_in_row && parse_usize(trim(*shape_in_row), num) && num >= 1) c4.c4_shape_in_row_override = num;
        if (boundary_in_row && parse_usize(trim(*boundary_in_row), num) && num >= 1) c4.c4_boundary_in_row_override = num;
        return;
    }

    auto kind = c4_shape_kind_for(func_lower);
    if (kind && !positional.empty()) {
        C4Shape s;
        s.id = positional[0];
        s.label = positional.size() > 1 ? positional[1] : s.id;
        s.kind = *kind;
        if (const std::string* v = args.get("type")) s.type_label = *v;
        s.techn = either(kv_opt(args, "techn"), kv_opt(args, "technology"));
        s.descr = either(kv_opt(args, "descr"), kv_opt(args, "description"));
        s.sprite = kv_opt(args, "sprite");
        s.tags = kv_opt(args, "tags");
        s.link = kv_opt(args, "link");
        if (kind_uses_techn(*kind)) {
            if (!s.techn) s.techn = positional_at(args, 2);
        } else {
            if (!s.type_label) s.type_label = positional_at(args, 2);
        }
        if (!s.descr) s.descr = positional_at(args, 3);
        if (!s.sprite) s.sprite = positional_at(args, 4);
        if (!s.tags) s.tags = positional_at(args, 5);
        if (!s.link) s.link = positional_at(args, 6);
        s.parent_boundary = boundary_stack.empty() ? "" : boundary_stack.back();
        c4.shapes.push_back(std::move(s));
    }
}

}  // namespace

Graph parse_c4_diagram(const std::string& input) {
    Graph graph;
    graph.kind = DiagramKind::C4;
    graph.direction = Direction::LeftRight;
    C4Boundary global;
    global.id = "global";
    global.label = "global";
    global.boundary_type = "global";
    graph.c4.boundaries.push_back(std::move(global));
    std::vector<std::string> boundary_stack{"global"};

    for (const std::string& raw_line : pp::preprocess_input(input)) {
        std::string line = trim(raw_line);
        if (line.empty()) continue;
        std::string lower = to_ascii_lower(line);
        if (starts_with(lower, "c4")) continue;  // the header line
        if (line == "}" || lower == "end") {
            if (boundary_stack.size() > 1) boundary_stack.pop_back();
            continue;
        }
        size_t brace = line.find('{');
        if (brace != std::string::npos) {
            std::string before = trim(line.substr(0, brace));
            std::string after = trim(line.substr(brace + 1));
            if (!before.empty()) process_c4_line(before, graph.c4, boundary_stack);
            if (!after.empty()) {
                bool closes = ends_with(after, "}");
                std::string after_trimmed = after;
                while (!after_trimmed.empty() && after_trimmed.back() == '}') after_trimmed.pop_back();  // trim_end_matches('}')
                after_trimmed = trim(after_trimmed);
                if (!after_trimmed.empty()) process_c4_line(after_trimmed, graph.c4, boundary_stack);
                if (closes && boundary_stack.size() > 1) boundary_stack.pop_back();
            }
            continue;
        }
        process_c4_line(line, graph.c4, boundary_stack);
    }
    return graph;
}

// ---------------------------------------------------------------- layout

namespace {

constexpr float kEps = std::numeric_limits<float>::epsilon();  // f32::EPSILON

struct Bounds {
    struct Data {
        float startx = 0, stopx = 0, starty = 0, stopy = 0, width_limit = 0;
    } data;
    struct Next {
        float startx = 0, stopx = 0, starty = 0, stopy = 0;
        size_t cnt = 0;
    } next;
    size_t shape_in_row = kDefaultShapeInRow;

    void set_data(float startx, float stopx, float starty, float stopy, float width_limit) {
        data.startx = startx;
        data.stopx = stopx;
        data.starty = starty;
        data.stopy = stopy;
        data.width_limit = width_limit;
        next.startx = startx;
        next.stopx = stopx;
        next.starty = starty;
        next.stopy = stopy;
        next.cnt = 0;
    }

    void bump_last_margin(float margin) {
        data.stopx += margin;
        data.stopy += margin;
    }

    std::pair<float, float> insert(float width, float height, float margin) {
        next.cnt += 1;
        float startx = std::fabs(next.startx - next.stopx) < kEps ? next.stopx + margin : next.stopx + margin * 2.0f;
        float stopx = startx + width;
        float starty = next.starty + margin * 2.0f;
        float stopy = starty + height;

        if (startx >= data.width_limit || stopx >= data.width_limit || next.cnt > shape_in_row) {
            startx = next.startx + margin + kNextLinePaddingX;
            starty = next.stopy + margin * 2.0f;
            stopx = startx + width;
            stopy = starty + height;
            next.starty = next.stopy;
            next.stopy = stopy;
            next.stopx = stopx;
            next.cnt = 1;
        }

        data.startx = data.startx == 0.0f ? startx : rmin(data.startx, startx);
        data.starty = data.starty == 0.0f ? starty : rmin(data.starty, starty);
        data.stopx = rmax(data.stopx, stopx);
        data.stopy = rmax(data.stopy, stopy);

        next.startx = rmin(next.startx, startx);
        next.starty = rmin(next.starty, starty);
        next.stopx = rmax(next.stopx, stopx);
        next.stopy = rmax(next.stopy, stopy);
        return {startx, starty};
    }
};

float text_line_height(float font_size) {
    float height = font_size + kTextLineHeight;
    if (font_size <= kTextLineHeightSmallThreshold) height += kTextLineHeightSmallAdd;
    return rmax(height, 1.0f);
}

// layout/c4.rs estimate_text_width (fast_text_metrics is off: always the font metrics).
float estimate_text_width(const std::string& text, float font_size) { return measure_text_width(text, font_size); }

std::vector<std::string> wrap_text_to_width(const std::string& text, float max_width, float font_size) {
    std::vector<std::string> lines;
    std::string current;
    for (const std::string& word : split_whitespace(text)) {
        std::string candidate = current.empty() ? word : current + " " + word;
        if (estimate_text_width(candidate, font_size) <= max_width || current.empty()) {
            current = candidate;
        } else {
            lines.push_back(current);
            current = word;
        }
    }
    if (!current.empty()) lines.push_back(current);
    if (lines.empty()) lines.push_back(text);
    return lines;
}

C4TextLayout text_layout(const std::string& text, float font_size, float y, bool wrap, float max_width, float line_height) {
    std::vector<std::string> lines;
    for (const std::string& raw : split_lines(text)) {
        if (wrap) {
            for (auto& l : wrap_text_to_width(raw, max_width, font_size)) lines.push_back(std::move(l));
        } else {
            lines.push_back(raw);
        }
    }
    if (lines.empty()) lines.push_back("");
    float width = 0.0f;
    for (const auto& line : lines) width = rmax(width, estimate_text_width(line, font_size));
    C4TextLayout t;
    t.text = text;
    t.width = width;
    t.height = line_height * static_cast<float>(std::max<size_t>(lines.size(), 1));
    t.y = y;
    t.lines = std::move(lines);
    return t;
}

using P = std::pair<float, float>;

P intersect_point(const C4ShapeLayout& node, P end) {
    float x1 = node.x, y1 = node.y;
    float x2 = end.first, y2 = end.second;
    float from_center_x = x1 + node.width / 2.0f;
    float from_center_y = y1 + node.height / 2.0f;
    float dx = std::fabs(x1 - x2);
    float dy = std::fabs(y1 - y2);
    float tan_dyx = std::fabs(dx) < kEps ? 0.0f : dy / dx;
    float from_dyx = node.height / node.width;
    if (std::fabs(y1 - y2) < kEps && x1 < x2) return {x1 + node.width, from_center_y};
    if (std::fabs(y1 - y2) < kEps && x1 > x2) return {x1, from_center_y};
    if (std::fabs(x1 - x2) < kEps && y1 < y2) return {from_center_x, y1 + node.height};
    if (std::fabs(x1 - x2) < kEps && y1 > y2) return {from_center_x, y1};
    if (x1 > x2 && y1 < y2) {
        if (from_dyx >= tan_dyx) return {x1, from_center_y + tan_dyx * node.width / 2.0f};
        return {from_center_x - dx / dy * node.height / 2.0f, y1 + node.height};
    } else if (x1 < x2 && y1 < y2) {
        if (from_dyx >= tan_dyx) return {x1 + node.width, from_center_y + tan_dyx * node.width / 2.0f};
        return {from_center_x + dx / dy * node.height / 2.0f, y1 + node.height};
    } else if (x1 < x2 && y1 > y2) {
        if (from_dyx >= tan_dyx) return {x1 + node.width, from_center_y - tan_dyx * node.width / 2.0f};
        return {from_center_x + node.height / 2.0f * dx / dy, y1};
    } else if (x1 > x2 && y1 > y2) {
        if (from_dyx >= tan_dyx) return {x1, from_center_y - node.width / 2.0f * tan_dyx};
        return {from_center_x - node.height / 2.0f * dx / dy, y1};
    }
    return {from_center_x, from_center_y};
}

std::pair<P, P> intersect_points(const C4ShapeLayout& from, const C4ShapeLayout& to) {
    P end_center{to.x + to.width / 2.0f, to.y + to.height / 2.0f};
    P start = intersect_point(from, end_center);
    P start_center{from.x + from.width / 2.0f, from.y + from.height / 2.0f};
    P end = intersect_point(to, start_center);
    return {start, end};
}

// Liang-Barsky segment / AABB clip against `rect` shrunk by `pad`.
bool segment_hits_rect(P a, P b, const C4ShapeLayout& rect, float pad) {
    float min_x = rect.x + pad, min_y = rect.y + pad;
    float max_x = rect.x + rect.width - pad, max_y = rect.y + rect.height - pad;
    if (max_x <= min_x || max_y <= min_y) return false;
    float dx = b.first - a.first, dy = b.second - a.second;
    float t0 = 0.0f, t1 = 1.0f;
    const float edges[4][2] = {{-dx, a.first - min_x}, {dx, max_x - a.first}, {-dy, a.second - min_y}, {dy, max_y - a.second}};
    for (const auto& e : edges) {
        float p = e[0], q = e[1];
        if (std::fabs(p) < 1e-6f) {
            if (q < 0.0f) return false;
        } else {
            float r = q / p;
            if (p < 0.0f) {
                if (r > t1) return false;
                if (r > t0) t0 = r;
            } else {
                if (r < t0) return false;
                if (r < t1) t1 = r;
            }
        }
    }
    return t0 <= t1;
}

std::vector<P> route_around_shapes(P start, P end, const std::string& from_id, const std::string& to_id, const std::vector<C4ShapeLayout>& shapes) {
    const float pad = 4.0f;
    std::vector<const C4ShapeLayout*> blockers;
    for (const auto& s : shapes)
        if (s.id != from_id && s.id != to_id && segment_hits_rect(start, end, s, pad)) blockers.push_back(&s);
    if (blockers.empty()) return {start, end};

    float band_top = std::numeric_limits<float>::infinity();
    float band_bottom = -std::numeric_limits<float>::infinity();
    for (const auto* s : blockers) band_top = rmin(band_top, s->y);
    for (const auto* s : blockers) band_bottom = rmax(band_bottom, s->y + s->height);
    const float clearance = 16.0f;

    for (float detour_y : {band_top - clearance, band_bottom + clearance}) {
        std::vector<P> candidate{start, {start.first, detour_y}, {end.first, detour_y}, end};
        bool clear = true;
        for (size_t i = 0; i + 1 < candidate.size() && clear; ++i)
            for (const auto& s : shapes)
                if (s.id != from_id && s.id != to_id && segment_hits_rect(candidate[i], candidate[i + 1], s, pad)) {
                    clear = false;
                    break;
                }
        if (clear) return candidate;
    }
    return {start, end};
}

struct Rect {
    float x, y, width, height;
};

float rect_overlap_area(Rect a, Rect b) {
    float ax2 = a.x + a.width, ay2 = a.y + a.height, bx2 = b.x + b.width, by2 = b.y + b.height;
    float ix = rmin(ax2, bx2) - rmax(a.x, b.x);
    float iy = rmin(ay2, by2) - rmax(a.y, b.y);
    if (ix <= 0.0f || iy <= 0.0f) return 0.0f;
    return ix * iy;
}

Rect rel_label_rect(const C4RelLayout& rel, P delta) {
    float center_x = rmin(rel.start.first, rel.end.first) + std::fabs(rel.start.first - rel.end.first) / 2.0f + rel.offset_x + delta.first;
    float center_y = rmin(rel.start.second, rel.end.second) + std::fabs(rel.start.second - rel.end.second) / 2.0f + rel.offset_y + delta.second;
    float primary_height = rmax(rel.label.height, kMessageFontSize);
    float secondary_height = rel.techn ? rmax(rel.techn->height, kMessageFontSize) : 0.0f;
    float secondary_center_y = center_y + kMessageFontSize + 5.0f;
    float top = secondary_height > 0.0f ? rmin(center_y - primary_height / 2.0f, secondary_center_y - secondary_height / 2.0f)
                                        : center_y - primary_height / 2.0f;
    float bottom = secondary_height > 0.0f ? rmax(center_y + primary_height / 2.0f, secondary_center_y + secondary_height / 2.0f)
                                           : center_y + primary_height / 2.0f;
    float width = rmax(rmax(rel.techn ? rel.techn->width : 0.0f, rel.label.width), kMessageFontSize * 1.2f);
    return {center_x - width / 2.0f, top, width, rmax(bottom - top, primary_height)};
}

float rel_label_score(Rect rect, const std::vector<Rect>& shape_obstacles, const std::vector<Rect>& placed_labels, float displacement) {
    float shape_overlap = 0.0f;
    for (const auto& o : shape_obstacles) shape_overlap += rect_overlap_area(rect, o);
    float label_overlap = 0.0f;
    for (const auto& p : placed_labels) label_overlap += rect_overlap_area(rect, p);
    return shape_overlap * 6.0f + label_overlap * 9.0f + displacement * 0.015f;
}

void resolve_rel_label_offsets(std::vector<C4RelLayout>& rels, const std::vector<C4ShapeLayout>& shapes) {
    if (rels.empty()) return;
    std::vector<Rect> shape_obstacles;
    for (const auto& s : shapes) shape_obstacles.push_back({s.x, s.y, s.width, s.height});
    std::vector<Rect> placed_labels;
    float step = rmax(kMessageFontSize * 1.2f, 10.0f);

    for (auto& rel : rels) {
        float dx = rel.end.first - rel.start.first;
        float dy = rel.end.second - rel.start.second;
        float len = std::sqrt(dx * dx + dy * dy);
        float tangent_x, tangent_y, normal_x, normal_y;
        if (len > 1e-3f) {
            float tx = dx / len, ty = dy / len;
            tangent_x = tx; tangent_y = ty; normal_x = -ty; normal_y = tx;
        } else {
            tangent_x = 1.0f; tangent_y = 0.0f; normal_x = 0.0f; normal_y = -1.0f;
        }

        std::vector<P> candidates;
        candidates.push_back({0.0f, 0.0f});
        for (int ring = 1; ring <= 6; ++ring) {
            float dist = step * static_cast<float>(ring);
            for (float normal_sign : {-1.0f, 1.0f}) {
                candidates.push_back({normal_x * dist * normal_sign, normal_y * dist * normal_sign});
                if (ring <= 3) {
                    for (float tangent_sign : {-1.0f, 1.0f}) {
                        float tangent_dist = dist * 0.75f * tangent_sign;
                        candidates.push_back({normal_x * dist * normal_sign + tangent_x * tangent_dist,
                                              normal_y * dist * normal_sign + tangent_y * tangent_dist});
                    }
                }
            }
        }

        P best_delta{0.0f, 0.0f};
        Rect best_rect = rel_label_rect(rel, {0.0f, 0.0f});
        float best_score = rel_label_score(best_rect, shape_obstacles, placed_labels, 0.0f);
        for (size_t i = 1; i < candidates.size(); ++i) {
            P delta = candidates[i];
            Rect rect = rel_label_rect(rel, delta);
            float displacement = std::sqrt(delta.first * delta.first + delta.second * delta.second);
            float score = rel_label_score(rect, shape_obstacles, placed_labels, displacement);
            if (score < best_score) {
                best_score = score;
                best_delta = delta;
                best_rect = rect;
                if (best_score < 1e-3f) break;
            }
        }
        rel.offset_x += best_delta.first;
        rel.offset_y += best_delta.second;
        placed_labels.push_back(best_rect);
    }
}

struct LayoutCtx {
    std::map<std::string, std::vector<std::string>> shapes_by_boundary, boundaries_by_parent;
    std::map<std::string, const C4Shape*> shape_map;
    std::map<std::string, const C4Boundary*> boundary_map;
    size_t shape_in_row = kDefaultShapeInRow, boundary_in_row = kDefaultBoundaryInRow;
    std::vector<C4ShapeLayout> shapes_out;
    std::vector<C4BoundaryLayout> boundaries_out;
    float global_max_x = 0, global_max_y = 0;
};

void layout_shapes(Bounds& bounds, const std::vector<std::string>& shape_ids, LayoutCtx& ctx) {
    for (const std::string& shape_id : shape_ids) {
        auto it = ctx.shape_map.find(shape_id);
        if (it == ctx.shape_map.end()) continue;
        const C4Shape& shape = *it->second;
        float type_font_size = rmax(kShapeFontSize - 2.0f, 1.0f);
        std::string type_label_text = std::string("<<") + kind_name(shape.kind) + ">>";
        C4TextLayout type_layout;
        type_layout.text = type_label_text;
        type_layout.lines = {type_label_text};
        type_layout.width = estimate_text_width(type_label_text, type_font_size);
        type_layout.height = type_font_size + 2.0f;
        type_layout.y = kShapePadding;
        float y = type_layout.y + type_layout.height - 4.0f;

        std::optional<float> image_y;
        if (kind_is_person(shape.kind)) {
            image_y = y;
            y += kPersonIconSize;
        } else if (shape.sprite) {
            image_y = y;
            y += kPersonIconSize;
        }

        float label_font_size = kShapeFontSize + 2.0f;
        float text_limit_width = kShapeWidth - kShapePadding * 2.0f;
        C4TextLayout label_layout = text_layout(shape.label, label_font_size, y + 8.0f, true, text_limit_width, text_line_height(label_font_size));
        y = label_layout.y + label_layout.height;

        std::optional<C4TextLayout> type_or_techn_layout;
        const std::optional<std::string>& tt = shape.techn ? shape.techn : shape.type_label;
        if (tt) {
            C4TextLayout l = text_layout("[" + *tt + "]", kShapeFontSize, y + 5.0f, true, text_limit_width, text_line_height(kShapeFontSize));
            y = l.y + l.height;
            type_or_techn_layout = std::move(l);
        }

        std::optional<C4TextLayout> descr_layout;
        float rect_height = y;
        float rect_width = label_layout.width;
        if (shape.descr) {
            C4TextLayout l = text_layout(*shape.descr, kShapeFontSize, y + 20.0f, true, text_limit_width, text_line_height(kShapeFontSize));
            y = l.y + l.height;
            rect_width = rmax(rect_width, l.width);
            float lines = static_cast<float>(l.lines.size());
            rect_height = y - lines * 5.0f;
            descr_layout = std::move(l);
        }
        rect_width += kShapePadding;
        float width = rmax(kShapeWidth, rect_width);
        float height = rmax(kShapeHeight, rect_height);
        float margin = kShapeMargin;
        auto [x, y_pos] = bounds.insert(width, height, margin);

        C4ShapeLayout out;
        out.id = shape.id;
        out.kind = shape.kind;
        out.bg_color = shape.bg_color;
        out.border_color = shape.border_color;
        out.font_color = shape.font_color;
        out.x = x;
        out.y = y_pos;
        out.width = width;
        out.height = height;
        out.margin = margin;
        out.type_label = std::move(type_layout);
        out.label = std::move(label_layout);
        out.type_or_techn = std::move(type_or_techn_layout);
        out.descr = std::move(descr_layout);
        out.image_y = image_y;
        ctx.shapes_out.push_back(std::move(out));
    }
    bounds.bump_last_margin(kShapeMargin);
}

void layout_boundaries(Bounds& parent_bounds, const std::vector<std::string>& boundary_ids, LayoutCtx& ctx) {
    if (boundary_ids.empty()) return;
    Bounds current_bounds;
    current_bounds.shape_in_row = ctx.shape_in_row;
    size_t limit_div = std::min(std::max<size_t>(ctx.boundary_in_row, 1), boundary_ids.size());
    current_bounds.data.width_limit = parent_bounds.data.width_limit / static_cast<float>(limit_div);

    for (size_t idx = 0; idx < boundary_ids.size(); ++idx) {
        const std::string& boundary_id = boundary_ids[idx];
        auto bit = ctx.boundary_map.find(boundary_id);
        if (bit == ctx.boundary_map.end()) continue;
        const C4Boundary& boundary = *bit->second;
        float y = 0.0f;
        float label_font_size = kBoundaryFontSize + 2.0f;
        C4TextLayout label_layout = text_layout(boundary.label, label_font_size, y + 8.0f, true, current_bounds.data.width_limit, text_line_height(label_font_size));
        y = label_layout.y + label_layout.height;
        std::optional<C4TextLayout> boundary_type_layout;
        if (!boundary.boundary_type.empty()) {
            C4TextLayout l = text_layout("[" + boundary.boundary_type + "]", kBoundaryFontSize, y + 5.0f, true, current_bounds.data.width_limit,
                                         text_line_height(kBoundaryFontSize));
            y = l.y + l.height;
            boundary_type_layout = std::move(l);
        }
        std::optional<C4TextLayout> boundary_descr_layout;
        if (boundary.descr) {
            float fs = rmax(kBoundaryFontSize - 2.0f, 1.0f);
            C4TextLayout l = text_layout(*boundary.descr, fs, y + 20.0f, true, current_bounds.data.width_limit, text_line_height(fs));
            y = l.y + l.height;
            boundary_descr_layout = std::move(l);
        }

        if (idx == 0 || idx % ctx.boundary_in_row == 0) {
            float startx = parent_bounds.data.startx + kDiagramMarginX;
            float starty = parent_bounds.data.stopy + kDiagramMarginY + y;
            current_bounds.set_data(startx, startx, starty, starty, current_bounds.data.width_limit);
        } else {
            float startx = std::fabs(current_bounds.data.stopx - current_bounds.data.startx) > kEps ? current_bounds.data.stopx + kDiagramMarginX
                                                                                                    : current_bounds.data.startx;
            float starty = current_bounds.data.starty;
            current_bounds.set_data(startx, startx, starty, starty, current_bounds.data.width_limit);
        }

        auto sit = ctx.shapes_by_boundary.find(boundary_id);
        if (sit != ctx.shapes_by_boundary.end()) layout_shapes(current_bounds, sit->second, ctx);

        auto cit = ctx.boundaries_by_parent.find(boundary_id);
        if (cit != ctx.boundaries_by_parent.end()) layout_boundaries(current_bounds, cit->second, ctx);

        if (boundary_id != "global") {
            C4BoundaryLayout b;
            b.id = boundary_id;
            b.label = std::move(label_layout);
            b.boundary_type = std::move(boundary_type_layout);
            b.descr = std::move(boundary_descr_layout);
            b.bg_color = boundary.bg_color;
            b.border_color = boundary.border_color;
            b.font_color = boundary.font_color;
            b.x = current_bounds.data.startx;
            b.y = current_bounds.data.starty;
            b.width = current_bounds.data.stopx - current_bounds.data.startx;
            b.height = current_bounds.data.stopy - current_bounds.data.starty;
            ctx.boundaries_out.push_back(std::move(b));
        }

        parent_bounds.data.stopy = rmax(parent_bounds.data.stopy, current_bounds.data.stopy + kShapeMargin);
        parent_bounds.data.stopx = rmax(parent_bounds.data.stopx, current_bounds.data.stopx + kShapeMargin);
        ctx.global_max_x = rmax(ctx.global_max_x, parent_bounds.data.stopx);
        ctx.global_max_y = rmax(ctx.global_max_y, parent_bounds.data.stopy);
    }
}

}  // namespace

Layout compute_c4_layout(const Graph& graph, const Theme&, const LayoutConfig&) {
    const C4Data& c4 = graph.c4;
    LayoutCtx ctx;
    ctx.shape_in_row = c4.c4_shape_in_row_override.value_or(kDefaultShapeInRow);
    ctx.boundary_in_row = c4.c4_boundary_in_row_override.value_or(kDefaultBoundaryInRow);
    for (const auto& s : c4.shapes) {
        ctx.shapes_by_boundary[s.parent_boundary].push_back(s.id);
        ctx.shape_map[s.id] = &s;
    }
    for (const auto& b : c4.boundaries) {
        ctx.boundaries_by_parent[b.parent_boundary].push_back(b.id);
        ctx.boundary_map[b.id] = &b;
    }
    std::vector<std::string> root_boundaries;
    if (auto it = ctx.boundaries_by_parent.find(""); it != ctx.boundaries_by_parent.end()) root_boundaries = it->second;

    ctx.global_max_x = kDiagramMarginX;
    ctx.global_max_y = kDiagramMarginY;
    Bounds screen_bounds;
    screen_bounds.shape_in_row = ctx.shape_in_row;
    screen_bounds.set_data(kDiagramMarginX, kDiagramMarginX, kDiagramMarginY, kDiagramMarginY, 1920.0f);
    layout_boundaries(screen_bounds, root_boundaries, ctx);

    std::vector<C4RelLayout> rels_out;
    for (const auto& rel : c4.rels) {
        auto from = std::find_if(ctx.shapes_out.begin(), ctx.shapes_out.end(), [&](const C4ShapeLayout& s) { return s.id == rel.from; });
        if (from == ctx.shapes_out.end()) continue;
        auto to = std::find_if(ctx.shapes_out.begin(), ctx.shapes_out.end(), [&](const C4ShapeLayout& s) { return s.id == rel.to; });
        if (to == ctx.shapes_out.end()) continue;
        auto [start, end] = intersect_points(*from, *to);
        C4RelLayout r;
        r.kind = rel.kind;
        r.from = rel.from;
        r.to = rel.to;
        float lh = text_line_height(kMessageFontSize);
        r.label = text_layout(rel.label, kMessageFontSize, 0.0f, true, estimate_text_width(rel.label, kMessageFontSize), lh);
        if (rel.techn) r.techn = text_layout(*rel.techn, kMessageFontSize, 0.0f, true, estimate_text_width(*rel.techn, kMessageFontSize), lh);
        r.start = start;
        r.end = end;
        r.waypoints = route_around_shapes(start, end, rel.from, rel.to, ctx.shapes_out);
        r.offset_x = rel.offset_x;
        r.offset_y = rel.offset_y;
        r.line_color = rel.line_color;
        r.text_color = rel.text_color;
        rels_out.push_back(std::move(r));
    }
    resolve_rel_label_offsets(rels_out, ctx.shapes_out);

    Layout layout;
    layout.kind = graph.kind;
    for (const auto& shape : ctx.shapes_out) {
        NodeLayout n;
        n.id = shape.id;
        n.x = shape.x;
        n.y = shape.y;
        n.width = shape.width;
        n.height = shape.height;
        n.label.lines = shape.label.lines;
        n.label.width = shape.label.width;
        n.label.height = shape.label.height;
        n.shape = NodeShape::Rectangle;
        layout.nodes[shape.id] = std::move(n);
    }
    for (const auto& rel : rels_out) {
        EdgeLayout e;
        e.from = rel.from;
        e.to = rel.to;
        e.points = rel.waypoints;
        e.directed = rel.kind != C4RelKind::BiRel;
        e.arrow_end = rel.kind != C4RelKind::BiRel;
        layout.edges.push_back(std::move(e));
    }

    layout.width = rmax(ctx.global_max_x - kDiagramMarginX + 2.0f * kDiagramMarginX, 1.0f);
    layout.height = rmax(ctx.global_max_y - kDiagramMarginY + 2.0f * kDiagramMarginY, 1.0f);
    C4Layout out;
    out.shapes = std::move(ctx.shapes_out);
    out.boundaries = std::move(ctx.boundaries_out);
    out.rels = std::move(rels_out);
    out.viewbox_x = 0.0f;
    out.viewbox_y = -kDiagramMarginY;
    out.viewbox_width = layout.width;
    out.viewbox_height = layout.height;
    out.use_max_width = true;
    layout.c4 = std::move(out);
    return layout;
}

// ---------------------------------------------------------------- writer

namespace {

const char* const kPersonIcon = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAIAAADYYG7QAAACD0lEQVR4Xu2YoU4EMRCGT+4j8Ai8AhaH4QHgAUjQuFMECUgMIUgwJAgMhgQsAYUiJCiQIBBY+EITsjfTdme6V24v4c8vyGbb+ZjOtN0bNcvjQXmkH83WvYBWto6PLm6v7p7uH1/w2fXD+PBycX1Pv2l3IdDm/vn7x+dXQiAubRzoURa7gRZWd0iGRIiJbOnhnfYBQZNJjNbuyY2eJG8fkDE3bbG4ep6MHUAsgYxmE3nVs6VsBWJSGccsOlFPmLIViMzLOB7pCVO2AtHJMohH7Fh6zqitQK7m0rJvAVYgGcEpe//PLdDz65sM4pF9N7ICcXDKIB5Nv6j7tD0NoSdM2QrU9Gg0ewE1LqBhHR3BBdvj2vapnidjHxD/q6vd7Pvhr31AwcY8eXMTXAKECZZJFXuEq27aLgQK5uLMohCenGGuGewOxSjBvYBqeG6B+Nqiblggdjnc+ZXDy+FNFpFzw76O3UBAROuXh6FoiAcf5g9eTvUgzy0nWg6I8cXHRUpg5bOVBCo+KDpFajOf23GgPme7RSQ+lacIENUgJ6gg1k6HjgOlqnLqip4tEuhv0hNEMXUD0clyXE3p6pZA0S2nnvTlXwLJEZWlb7cTQH1+USgTN4VhAenm/wea1OCAOmqo6fE1WCb9WSKBah+rbUWPWAmE2Rvk0ApiB45eOyNAzU8xcTvj8KvkKEoOaIYeHNA3ZuygAvFMUO0AAAAASUVORK5CYII=";
const char* const kExternalPersonIcon = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAIAAADYYG7QAAAB6ElEQVR4Xu2YLY+EMBCG9+dWr0aj0Wg0Go1Go0+j8Xdv2uTCvv1gpt0ebHKPuhDaeW4605Z9mJvx4AdXUyTUdd08z+u6flmWZRnHsWkafk9DptAwDPu+f0eAYtu2PEaGWuj5fCIZrBAC2eLBAnRCsEkkxmeaJp7iDJ2QMDdHsLg8SxKFEJaAo8lAXnmuOFIhTMpxxKATebo4UiFknuNo4OniSIXQyRxEA3YsnjGCVEjVXD7yLUAqxBGUyPv/Y4W2beMgGuS7kVQIBycH0fD+oi5pezQETxdHKmQKGk1eQEYldK+jw5GxPfZ9z7Mk0Qnhf1W1m3w//EUn5BDmSZsbR44QQLBEqrBHqOrmSKaQAxdnLArCrxZcM7A7ZKs4ioRq8LFC+NpC3WCBJsvpVw5edm9iEXFuyNfxXAgSwfrFQ1c0iNda8AdejvUgnktOtJQQxmcfFzGglc5WVCj7oDgFqU18boeFSs52CUh8LE8BIVQDT1ABrB0HtgSEYlX5doJnCwv9TXocKCaKbnwhdDKPq4lf3SwU3HLq4V/+WYhHVMa/3b4IlfyikAduCkcBc7mQ3/z/Qq/cTuikhkzB12Ae/mcJC9U+Vo8Ej1gWAtgbeGgFsAMHr50BIWOLCbezvhpBFUdY6EJuJ/QDW0XoMX60zZ0AAAAASUVORK5CYII=";

std::string fixed0(float v) { return fmt_fixed(v, 0); }

// render.rs c4_text_svg
std::string c4_text_svg(float x, float y, const std::vector<std::string>& lines, const std::string& font_family, float font_size,
                        const std::string& font_weight, const std::string& fill, bool italic) {
    using svg::escape_xml;
    std::string out;
    float line_count = static_cast<float>(lines.size());
    for (size_t idx = 0; idx < lines.size(); ++idx) {
        float dy = static_cast<float>(idx) * font_size - font_size * (line_count - 1.0f) / 2.0f;
        out += "<text x=\"" + svg::F2(x) + "\" y=\"" + svg::F2(y) + "\" dominant-baseline=\"middle\" fill=\"" + escape_xml(fill) +
               "\" style=\"text-anchor: middle; font-size: " + svg::F(font_size) + "px; font-weight: " + escape_xml(font_weight) +
               "; font-family: " + svg::normalize_font_family(font_family) + "\"" + (italic ? " font-style=\"italic\"" : "") +
               "><tspan dy=\"" + svg::F2(dy) + "\" alignment-baseline=\"mathematical\">" + escape_xml(lines[idx]) + "</tspan></text>";
    }
    return out;
}

std::string render_shape(const C4ShapeLayout& shape) {
    using svg::escape_xml;
    using svg::F;
    KindColors def = shape_colors(shape.kind);
    std::string fill = escape_xml(shape.bg_color ? *shape.bg_color : def.bg);
    std::string stroke = escape_xml(shape.border_color ? *shape.border_color : def.border);
    std::string font_color = escape_xml(shape.font_color ? *shape.font_color : "#FFFFFF");
    std::string s = "<g class=\"person-man\">";
    if (kind_is_db(shape.kind)) {
        float half = shape.width / 2.0f;
        std::string e = F(kDbEllipseHeight), h = fixed0(half);
        s += "<path fill=\"" + fill + "\" stroke-width=\"" + F(kShapeStrokeWidth) + "\" stroke=\"" + stroke + "\" d=\"M" + fixed0(shape.x) + "," +
             fixed0(shape.y) + "c0,-" + e + " " + h + ",-" + e + " " + h + ",-" + e + "c0,0 " + h + ",0 " + h + "," + e + "l0," + fixed0(shape.height) +
             "c0," + e + " -" + h + "," + e + " -" + h + "," + e + "c0,0 -" + h + ",0 -" + h + ",-" + e + "l0,-" + fixed0(shape.height) + "\"/>";
        s += "<path fill=\"none\" stroke-width=\"" + F(kShapeStrokeWidth) + "\" stroke=\"" + stroke + "\" d=\"M" + fixed0(shape.x) + "," + fixed0(shape.y) +
             "c0," + e + " " + h + "," + e + " " + h + "," + e + "c0,0 " + h + ",0 " + h + ",-" + e + "\"/>";
    } else if (kind_is_queue(shape.kind)) {
        std::string half = F(shape.height / 2.0f);
        std::string curve = F(kQueueCurveRadius);
        s += "<path fill=\"" + fill + "\" stroke-width=\"" + F(kShapeStrokeWidth) + "\" stroke=\"" + stroke + "\" d=\"M" + fixed0(shape.x) + "," +
             fixed0(shape.y) + "l" + fixed0(shape.width) + ",0c" + curve + ",0 " + curve + "," + half + " " + curve + "," + half + "c0,0 0," + half +
             " -" + curve + "," + half + "l-" + fixed0(shape.width) + ",0c-" + curve + ",0 -" + curve + ",-" + half + " -" + curve + ",-" + half +
             "c0,0 0,-" + half + " " + curve + ",-" + half + "\"/>";
        s += "<path fill=\"none\" stroke-width=\"" + F(kShapeStrokeWidth) + "\" stroke=\"" + stroke + "\" d=\"M" + fixed0(shape.x + shape.width) + "," +
             fixed0(shape.y) + "c-" + curve + ",0 -" + curve + "," + half + " -" + curve + "," + half + "c0," + half + " " + curve + "," + half + " " +
             curve + "," + half + "\"/>";
    } else {
        s += "<rect x=\"" + fixed0(shape.x) + "\" y=\"" + fixed0(shape.y) + "\" fill=\"" + fill + "\" stroke=\"" + stroke + "\" width=\"" +
             fixed0(shape.width) + "\" height=\"" + fixed0(shape.height) + "\" rx=\"" + fmt_fixed(kShapeCornerRadius, 1) + "\" ry=\"" +
             fmt_fixed(kShapeCornerRadius, 1) + "\" stroke-width=\"" + F(kShapeStrokeWidth) + "\"/>";
    }

    float type_font_size = kShapeFontSize - 2.0f;
    s += "<text fill=\"" + font_color + "\" font-family=\"" + svg::normalize_font_family(kFontFamily) + "\" font-size=\"" + F(type_font_size) +
         "\" font-style=\"italic\" lengthAdjust=\"spacing\" textLength=\"" + fixed0(std::round(shape.type_label.width)) + "\" x=\"" +
         fixed0(shape.x + shape.width / 2.0f - shape.type_label.width / 2.0f) + "\" y=\"" + fixed0(shape.y + shape.type_label.y) + "\">" +
         escape_xml(shape.type_label.text) + "</text>";

    if (shape.image_y && kind_is_person(shape.kind)) {
        const char* icon = shape.kind == C4ShapeKind::ExternalPerson ? kExternalPersonIcon : kPersonIcon;
        s += "<image width=\"" + fixed0(kPersonIconSize) + "\" height=\"" + fixed0(kPersonIconSize) + "\" x=\"" +
             fixed0(shape.x + shape.width / 2.0f - kPersonIconSize / 2.0f) + "\" y=\"" + fixed0(shape.y + *shape.image_y) + "\" xlink:href=\"" + icon + "\"/>";
    }

    float cx = shape.x + shape.width / 2.0f;
    s += c4_text_svg(cx, shape.y + shape.label.y, shape.label.lines, kFontFamily, kShapeFontSize + 2.0f, "bold", font_color, false);
    if (shape.type_or_techn) s += c4_text_svg(cx, shape.y + shape.type_or_techn->y, shape.type_or_techn->lines, kFontFamily, kShapeFontSize, kFontWeight, font_color, true);
    if (shape.descr) s += c4_text_svg(cx, shape.y + shape.descr->y, shape.descr->lines, kFontFamily, kShapeFontSize, kFontWeight, font_color, false);
    s += "</g>";
    return s;
}

std::string render_boundary(const C4BoundaryLayout& b) {
    using svg::escape_xml;
    using svg::F;
    std::string s = "<g>";
    std::string fill = escape_xml(b.bg_color ? *b.bg_color : kBoundaryFill);
    std::string stroke = escape_xml(b.border_color ? *b.border_color : kBoundaryStroke);
    std::string font_color = escape_xml(b.font_color ? *b.font_color : kBoundaryStroke);
    s += "<rect x=\"" + fixed0(b.x) + "\" y=\"" + fixed0(b.y) + "\" fill=\"" + fill + "\" stroke=\"" + stroke + "\" width=\"" + fixed0(b.width) +
         "\" height=\"" + fixed0(b.height) + "\" rx=\"" + fmt_fixed(kBoundaryCornerRadius, 1) + "\" ry=\"" + fmt_fixed(kBoundaryCornerRadius, 1) +
         "\" stroke-width=\"" + F(kBoundaryStrokeWidth) + "\"";
    s += " stroke-dasharray=\"" + escape_xml(kBoundaryDasharray) + "\"";
    // (boundary_fill is "none" by default: no fill-opacity attribute)
    s += "/>";
    float cx = b.x + b.width / 2.0f;
    s += c4_text_svg(cx, b.y + b.label.y, b.label.lines, kFontFamily, kBoundaryFontSize + 2.0f, "bold", font_color, false);
    if (b.boundary_type) s += c4_text_svg(cx, b.y + b.boundary_type->y, b.boundary_type->lines, kFontFamily, kBoundaryFontSize, kFontWeight, font_color, false);
    if (b.descr) s += c4_text_svg(cx, b.y + b.descr->y, b.descr->lines, kFontFamily, kBoundaryFontSize - 2.0f, kFontWeight, font_color, false);
    s += "</g>";
    return s;
}

std::string render_rel(const C4RelLayout& rel, bool straight) {
    using svg::escape_xml;
    using svg::F2;
    std::string stroke = rel.line_color ? *rel.line_color : kBoundaryStroke;
    std::string markers;  // marker-end then marker-start, as the crate appends them
    if (rel.kind != C4RelKind::RelBack) markers += " marker-end=\"url(#arrowhead)\"";
    if (rel.kind == C4RelKind::BiRel || rel.kind == C4RelKind::RelBack) markers += " marker-start=\"url(#arrowend)\"";
    std::string s;
    if (rel.waypoints.size() > 2) {
        std::string d = "M" + F2(rel.waypoints[0].first) + "," + F2(rel.waypoints[0].second);
        for (size_t i = 1; i < rel.waypoints.size(); ++i) d += " L" + F2(rel.waypoints[i].first) + "," + F2(rel.waypoints[i].second);
        s += "<path fill=\"none\" stroke-width=\"1\" stroke=\"" + escape_xml(stroke) + "\" d=\"" + d + "\"" + markers + "/>";
    } else if (straight) {
        s += "<line x1=\"" + F2(rel.start.first) + "\" y1=\"" + F2(rel.start.second) + "\" x2=\"" + F2(rel.end.first) + "\" y2=\"" + F2(rel.end.second) +
             "\" stroke-width=\"1\" stroke=\"" + escape_xml(stroke) + "\" style=\"fill: none;\"" + markers + " />";
    } else {
        float control_x = rel.start.first + (rel.end.first - rel.start.first) / 4.0f;
        float control_y = rel.start.second + (rel.end.second - rel.start.second) / 2.0f;
        s += "<path fill=\"none\" stroke-width=\"1\" stroke=\"" + escape_xml(stroke) + "\" d=\"M" + F2(rel.start.first) + "," + F2(rel.start.second) + " Q" +
             F2(control_x) + "," + F2(control_y) + " " + F2(rel.end.first) + "," + F2(rel.end.second) + "\"" + markers + "/>";
    }

    std::string text_color = rel.text_color ? *rel.text_color : kBoundaryStroke;
    float mid_x = rmin(rel.start.first, rel.end.first) + std::fabs(rel.start.first - rel.end.first) / 2.0f + rel.offset_x;
    float mid_y = rmin(rel.start.second, rel.end.second) + std::fabs(rel.start.second - rel.end.second) / 2.0f + rel.offset_y;
    s += c4_text_svg(mid_x, mid_y, rel.label.lines, kFontFamily, kMessageFontSize, kFontWeight, text_color, false);
    if (rel.techn) s += c4_text_svg(mid_x, mid_y + kMessageFontSize + 5.0f, rel.techn->lines, kFontFamily, kMessageFontSize, kFontWeight, text_color, true);
    return s;
}

}  // namespace

std::string render_c4_svg(const Layout& layout, const Theme& theme, const LayoutConfig&) {
    const C4Layout& c4 = *layout.c4;
    float width = rmax(layout.width, 1.0f), height = rmax(layout.height, 1.0f);
    // The root tag always carries xmlns:xlink (has_links is true for C4); no marker defs of the usual kind.
    std::string svg = svg::document_start(width, height, c4.viewbox_x, c4.viewbox_y, c4.viewbox_width, c4.viewbox_height, c4.use_max_width, true, theme, false);

    svg += "<defs><symbol id=\"computer\" width=\"24\" height=\"24\"><path transform=\"scale(.5)\" d=\"M2 2v13h20v-13h-20zm18 11h-16v-9h16v9zm-10.228 6l.466-1h3.524l.467 1h-4.457zm14.228 3h-24l2-6h2.104l-1.33 4h18.45l-1.297-4h2.073l2 6zm-5-10h-14v-7h14v7z\"/></symbol></defs>";
    svg += "<defs><symbol id=\"database\" fill-rule=\"evenodd\" clip-rule=\"evenodd\"><path transform=\"scale(.5)\" d=\"M12.258.001l.256.004.255.005.253.008.251.01.249.012.247.015.246.016.242.019.241.02.239.023.236.024.233.027.231.028.229.031.225.032.223.034.22.036.217.038.214.04.211.041.208.043.205.045.201.046.198.048.194.05.191.051.187.053.183.054.18.056.175.057.172.059.168.06.163.061.16.063.155.064.15.066.074.033.073.033.071.034.07.034.069.035.068.035.067.035.066.035.064.036.064.036.062.036.06.036.06.037.058.037.058.037.055.038.055.038.053.038.052.038.051.039.05.039.048.039.047.039.045.04.044.04.043.04.041.04.04.041.039.041.037.041.036.041.034.041.033.042.032.042.03.042.029.042.027.042.026.043.024.043.023.043.021.043.02.043.018.044.017.043.015.044.013.044.012.044.011.045.009.044.007.045.006.045.004.045.002.045.001.045v17l-.001.045-.002.045-.004.045-.006.045-.007.045-.009.044-.011.045-.012.044-.013.044-.015.044-.017.043-.018.044-.02.043-.021.043-.023.043-.024.043-.026.043-.027.042-.029.042-.03.042-.032.042-.033.042-.034.041-.036.041-.037.041-.039.041-.04.041-.041.04-.043.04-.044.04-.045.04-.047.039-.048.039-.05.039-.051.039-.052.038-.053.038-.055.038-.055.038-.058.037-.058.037-.06.037-.06.036-.062.036-.064.036-.064.036-.066.035-.067.035-.068.035-.069.035-.07.034-.071.034-.073.033-.074.033-.15.066-.155.064-.16.063-.163.061-.168.06-.172.059-.175.057-.18.056-.183.054-.187.053-.191.051-.194.05-.198.048-.201.046-.205.045-.208.043-.211.041-.214.04-.217.038-.22.036-.223.034-.225.032-.229.031-.231.028-.233.027-.236.024-.239.023-.241.02-.242.019-.246.016-.247.015-.249.012-.251.01-.253.008-.255.005-.256.004-.258.001-.258-.001-.256-.004-.255-.005-.253-.008-.251-.01-.249-.012-.247-.015-.245-.016-.243-.019-.241-.02-.238-.023-.236-.024-.234-.027-.231-.028-.228-.031-.226-.032-.223-.034-.22-.036-.217-.038-.214-.04-.211-.041-.208-.043-.204-.045-.201-.046-.198-.048-.195-.05-.19-.051-.187-.053-.184-.054-.179-.056-.176-.057-.172-.059-.167-.06-.164-.061-.159-.063-.155-.064-.151-.066-.074-.033-.072-.033-.072-.034-.07-.034-.069-.035-.068-.035-.067-.035-.066-.035-.064-.036-.063-.036-.062-.036-.061-.036-.06-.037-.058-.037-.057-.037-.056-.038-.055-.038-.053-.038-.052-.038-.051-.039-.049-.039-.049-.039-.046-.039-.046-.04-.044-.04-.043-.04-.041-.04-.04-.041-.039-.041-.037-.041-.036-.041-.034-.041-.033-.042-.032-.042-.03-.042-.029-.042-.027-.042-.026-.043-.024-.043-.023-.043-.021-.043-.02-.043-.018-.044-.017-.043-.015-.044-.013-.044-.012-.044-.011-.045-.009-.044-.007-.045-.006-.045-.004-.045-.002-.045-.001-.045v-17l.001-.045.002-.045.004-.045.006-.045.007-.045.009-.044.011-.045.012-.044.013-.044.015-.044.017-.043.018-.044.02-.043.021-.043.023-.043.024-.043.026-.043.027-.042.029-.042.03-.042.032-.042.033-.042.034-.041.036-.041.037-.041.039-.041.04-.041.041-.04.043-.04.044-.04.046-.04.046-.039.049-.039.049-.039.051-.039.052-.038.053-.038.055-.038.056-.038.057-.037.058-.037.06-.037.061-.036.062-.036.063-.036.064-.036.066-.035.067-.035.068-.035.069-.035.07-.034.072-.034.072-.033.074-.033.151-.066.155-.064.159-.063.164-.061.167-.06.172-.059.176-.057.179-.056.184-.054.187-.053.19-.051.195-.05.198-.048.201-.046.204-.045.208-.043.211-.041.214-.04.217-.038.22-.036.223-.034.226-.032.228-.031.231-.028.234-.027.236-.024.238-.023.241-.02.243-.019.245-.016.247-.015.249-.012.251-.01.253-.008.255-.005.256-.004.258-.001.258.001z\"/></symbol></defs>";
    svg += "<defs><symbol id=\"clock\" width=\"24\" height=\"24\"><path transform=\"scale(.5)\" d=\"M12 2c5.514 0 10 4.486 10 10s-4.486 10-10 10-10-4.486-10-10 4.486-10 10-10zm0-2c-6.627 0-12 5.373-12 12s5.373 12 12 12 12-5.373 12-12-5.373-12-12-12zm5.848 12.459c.202.038.202.333.001.372-1.907.361-6.045 1.111-6.547 1.111-.719 0-1.301-.582-1.301-1.301 0-.512.77-5.447 1.125-7.445.034-.192.312-.181.343.014l.985 6.238 5.394 1.011z\"/></symbol></defs>";

    for (const auto& shape : c4.shapes) svg += render_shape(shape);
    for (const auto& boundary : c4.boundaries) svg += render_boundary(boundary);

    svg += "<defs><marker id=\"arrowhead\" refX=\"9\" refY=\"5\" markerUnits=\"userSpaceOnUse\" markerWidth=\"12\" markerHeight=\"12\" orient=\"auto\"><path d=\"M 0 0 L 10 5 L 0 10 z\"/></marker></defs>";
    svg += "<defs><marker id=\"arrowend\" refX=\"1\" refY=\"5\" markerUnits=\"userSpaceOnUse\" markerWidth=\"12\" markerHeight=\"12\" orient=\"auto\"><path d=\"M 10 0 L 0 5 L 10 10 z\"/></marker></defs>";
    svg += "<defs><marker id=\"crosshead\" markerWidth=\"15\" markerHeight=\"8\" orient=\"auto\" refX=\"16\" refY=\"4\"><path fill=\"black\" stroke=\"#000000\" stroke-width=\"1px\" d=\"M 9,2 V 6 L16,4 Z\" style=\"stroke-dasharray: 0, 0;\"/><path fill=\"none\" stroke=\"#000000\" stroke-width=\"1px\" d=\"M 0,1 L 6,7 M 6,1 L 0,7\" style=\"stroke-dasharray: 0, 0;\"/></marker></defs>";
    svg += "<defs><marker id=\"filled-head\" refX=\"18\" refY=\"7\" markerWidth=\"20\" markerHeight=\"28\" orient=\"auto\"><path d=\"M 18,7 L9,13 L14,7 L9,1 Z\"/></marker></defs>";

    svg += "<g>";
    for (size_t idx = 0; idx < c4.rels.size(); ++idx) svg += render_rel(c4.rels[idx], idx == 0);
    svg += "</g></svg>";
    return svg;
}

}  // namespace mermaid
