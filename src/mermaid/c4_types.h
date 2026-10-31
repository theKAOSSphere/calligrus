// Mermaid port: C4 diagrams, parsed data (ir.rs C4*) and laid-out data (layout/types.rs C4*).
// Kept in its own header so ir.h and layout_types.h only need one include each.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mermaid {

enum class C4ShapeKind {
    Person, ExternalPerson, System, SystemDb, SystemQueue, ExternalSystem, ExternalSystemDb, ExternalSystemQueue,
    Container, ContainerDb, ContainerQueue, ExternalContainer, ExternalContainerDb, ExternalContainerQueue,
    Component, ComponentDb, ComponentQueue, ExternalComponent, ExternalComponentDb, ExternalComponentQueue,
};

struct C4Shape {
    std::string id, label;
    std::optional<std::string> type_label, techn, descr, sprite, tags, link;
    std::string parent_boundary;
    C4ShapeKind kind = C4ShapeKind::Person;
    std::optional<std::string> bg_color, border_color, font_color;
};

struct C4Boundary {
    std::string id, label, boundary_type;
    std::optional<std::string> descr, sprite, tags, link;
    std::string parent_boundary;
    std::optional<std::string> bg_color, border_color, font_color;
};

enum class C4RelKind { Rel, BiRel, RelUp, RelDown, RelLeft, RelRight, RelBack };

struct C4Rel {
    C4RelKind kind = C4RelKind::Rel;
    std::string from, to, label;
    std::optional<std::string> techn, descr, sprite, tags, link;
    float offset_x = 0, offset_y = 0;
    std::optional<std::string> line_color, text_color;
};

struct C4Data {
    std::vector<C4Shape> shapes;
    std::vector<C4Boundary> boundaries;
    std::vector<C4Rel> rels;
    std::optional<size_t> c4_shape_in_row_override, c4_boundary_in_row_override;
};

// ---- layout ----
struct C4TextLayout {
    std::string text;
    std::vector<std::string> lines;
    float width = 0, height = 0, y = 0;
};

struct C4ShapeLayout {
    std::string id;
    C4ShapeKind kind = C4ShapeKind::Person;
    std::optional<std::string> bg_color, border_color, font_color;
    float x = 0, y = 0, width = 0, height = 0, margin = 0;
    C4TextLayout type_label, label;
    std::optional<C4TextLayout> type_or_techn, descr;
    std::optional<float> image_y;
};

struct C4BoundaryLayout {
    std::string id;
    C4TextLayout label;
    std::optional<C4TextLayout> boundary_type, descr;
    std::optional<std::string> bg_color, border_color, font_color;
    float x = 0, y = 0, width = 0, height = 0;
};

struct C4RelLayout {
    C4RelKind kind = C4RelKind::Rel;
    std::string from, to;
    C4TextLayout label;
    std::optional<C4TextLayout> techn;
    std::pair<float, float> start{0, 0}, end{0, 0};
    std::vector<std::pair<float, float>> waypoints;  // full routed polyline including start and end
    float offset_x = 0, offset_y = 0;
    std::optional<std::string> line_color, text_color;
};

struct C4Layout {
    std::vector<C4ShapeLayout> shapes;
    std::vector<C4BoundaryLayout> boundaries;
    std::vector<C4RelLayout> rels;
    float viewbox_x = 0, viewbox_y = 0, viewbox_width = 0, viewbox_height = 0;
    bool use_max_width = true;
};

}  // namespace mermaid
