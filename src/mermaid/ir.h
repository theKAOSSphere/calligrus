// Mermaid port: the parsed diagram ("IR"). Mirrors mermaid-rs-renderer 0.3.1 src/ir.rs, but only
// the flowchart subset (the fields other diagram types use are left out until they are ported).
//
// Names follow the Rust code on purpose, so the two sources can be read side by side.
#pragma once

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "c4_types.h"

namespace mermaid {

// Thrown when the diagram uses something this port does not implement (yet). The app hook
// catches it and falls back to the Rust renderer, so "unsupported" is never a wrong picture.
struct Unsupported : std::runtime_error {
    explicit Unsupported(const std::string& what) : std::runtime_error(what) {}
};

// Thrown for input the Rust renderer would reject as well (a syntax error).
struct ParseError : std::runtime_error {
    explicit ParseError(const std::string& what) : std::runtime_error(what) {}
};

enum class Direction { TopDown, LeftRight, BottomTop, RightLeft };

enum class DiagramKind {
    Flowchart, Class, State, Sequence, Er, Pie, Mindmap, Journey, Timeline, Gantt, Requirement,
    GitGraph, C4, Sankey, Quadrant, ZenUML, Block, Packet, Kanban, Architecture, Radar, Treemap,
    XYChart,
};

enum class NodeShape {
    Rectangle, ForkJoin, RoundRect, Stadium, Subroutine, Cylinder, ActorBox, Circle, DoubleCircle,
    Diamond, Hexagon, Parallelogram, ParallelogramAlt, Trapezoid, TrapezoidAlt, Asymmetric,
    MindmapDefault, Text,
};

enum class EdgeStyle { Solid, Dotted, Thick };
enum class EdgeDecoration {
    Circle, Cross, Diamond, DiamondFilled,
    // crow's foot notation (ER diagrams)
    CrowsFootOne, CrowsFootZeroOne, CrowsFootMany, CrowsFootZeroMany,
};
enum class EdgeArrowhead { OpenTriangle, ClassDependency };

// ---- pie charts ----
struct PieSlice {
    std::string label;
    float value;
};

// ---- architecture diagrams ----
enum class ArchDir { Left, Right, Top, Bottom };

// ---- xy charts ----
enum class XYSeriesKind { Bar, Line };
struct XYSeries {
    XYSeriesKind kind;
    std::optional<std::string> label;
    std::vector<float> values;
};
struct XYChartData {
    std::optional<std::string> title, x_axis_label;
    std::vector<std::string> x_axis_categories;
    std::optional<std::string> y_axis_label;
    std::optional<float> y_axis_min, y_axis_max;
    std::vector<XYSeries> series;
};

// ---- quadrant charts ----
struct QuadrantPoint {
    std::string label;
    float x, y;
};
struct QuadrantData {
    std::optional<std::string> title, x_axis_left, x_axis_right, y_axis_bottom, y_axis_top;
    std::optional<std::string> quadrant_labels[4];  // top-right, top-left, bottom-left, bottom-right
    std::vector<QuadrantPoint> points;
};

// ---- timelines ----
struct TimelineEvent {
    std::string time;
    std::vector<std::string> events;
    std::optional<std::string> section;
};
struct TimelineData {
    std::optional<std::string> title;
    std::vector<TimelineEvent> events;
    std::vector<std::string> sections;
    std::optional<Direction> direction;
};

// ---- gantt charts ----
enum class GanttStatus { Done, Active, Crit, Milestone };
struct GanttTask {
    std::string id, label;
    std::optional<std::string> start, duration, after, section;
    std::optional<GanttStatus> status;
};

// ---- state diagrams ----
enum class StateNotePosition { LeftOf, RightOf };
struct StateNote {
    StateNotePosition position;
    std::string target, label;
};

// ---- sequence diagrams ----
enum class SequenceFrameKind { Alt, Opt, Loop, Par, Rect, Critical, Break };
enum class SequenceNotePosition { LeftOf, RightOf, Over };
enum class SequenceActivationKind { Activate, Deactivate };

struct SequenceActivation {
    std::string participant;
    size_t index;  // message index the event happens before
    SequenceActivationKind kind;
};
struct SequenceNote {
    SequenceNotePosition position;
    std::vector<std::string> participants;
    std::string label;
    size_t index;
};
struct SequenceBox {
    std::optional<std::string> label, color;
    std::vector<std::string> participants;
};
struct SequenceFrameSection {
    std::optional<std::string> label;
    size_t start_idx, end_idx;
};
struct SequenceFrame {
    SequenceFrameKind kind;
    std::vector<SequenceFrameSection> sections;
    size_t start_idx, end_idx;
};

// `Option<f32>` in Rust is std::optional<float> here.
struct NodeStyle {
    std::optional<std::string> fill, stroke, text_color;
    std::optional<float> stroke_width;
    std::optional<std::string> stroke_dasharray, line_color;
};

struct EdgeStyleOverride {
    std::optional<std::string> stroke;
    std::optional<float> stroke_width;
    std::optional<std::string> dasharray, label_color;
};

struct NodeLink {
    std::string url;
    std::optional<std::string> title, target;
};

struct Node {
    std::string id;
    std::string label;
    NodeShape shape = NodeShape::Rectangle;
    std::optional<float> value;        // journey score, sankey / treemap value
    std::optional<std::string> icon;   // architecture / mindmap icon
};

struct Edge {
    std::string from, to;
    std::optional<std::string> label, start_label, end_label;
    bool directed = false;
    bool arrow_start = false, arrow_end = false;
    std::optional<EdgeArrowhead> arrow_start_kind, arrow_end_kind;
    std::optional<EdgeDecoration> start_decoration, end_decoration;
    EdgeStyle style = EdgeStyle::Solid;
};

struct Subgraph {
    std::optional<std::string> id;
    std::string label;
    std::vector<std::string> nodes;
    std::optional<Direction> direction;
    std::optional<std::string> icon;
};

// ---- mindmaps (Stage 3b) ----
enum class MindmapNodeType { Default, RoundedRect, Rect, Circle, Cloud, Bang, Hexagon };
struct MindmapNode {
    std::string id, label;
    size_t level = 0;
    std::optional<size_t> section;
    MindmapNodeType node_type = MindmapNodeType::Default;
    std::vector<std::string> children;
};
struct MindmapData {
    std::vector<MindmapNode> nodes;
    std::optional<std::string> root_id;
};

// ---- git graphs (Stage 3b) ----
enum class GitGraphCommitType { Normal, Reverse, Highlight, Merge, CherryPick };
struct GitGraphCommit {
    std::string id;
    std::optional<std::string> message;
    size_t seq = 0;
    GitGraphCommitType commit_type = GitGraphCommitType::Normal;
    std::optional<GitGraphCommitType> custom_type;
    std::vector<std::string> tags, parents;
    std::string branch;
    bool custom_id = false;
};
struct GitGraphBranch {
    std::string name;
    std::optional<float> order;
    size_t insertion_index = 0;
};
struct GitGraphData {
    std::string main_branch;
    std::vector<GitGraphCommit> commits;
    std::vector<GitGraphBranch> branches;
};

// ---- block diagrams (Stage 3b) ----
struct BlockNode {
    std::string id;
    size_t span = 1;
    bool is_space = false;
};
struct BlockDiagram {
    std::optional<size_t> columns;
    std::vector<BlockNode> nodes;
};

struct Graph {
    DiagramKind kind = DiagramKind::Flowchart;
    Direction direction = Direction::TopDown;
    // Rust uses a BTreeMap (sorted by id): std::map sorts by bytes, the same order.
    std::map<std::string, Node> nodes;
    std::unordered_map<std::string, size_t> node_order;  // id -> first-seen index
    std::vector<Edge> edges;
    std::vector<Subgraph> subgraphs;
    std::map<std::string, NodeStyle> class_defs;
    std::map<std::string, std::vector<std::string>> node_classes;
    std::map<std::string, NodeStyle> node_styles;
    std::map<std::string, NodeStyle> subgraph_styles;
    std::map<std::string, std::vector<std::string>> subgraph_classes;
    std::map<std::string, NodeLink> node_links;
    std::map<size_t, EdgeStyleOverride> edge_styles;
    std::optional<EdgeStyleOverride> edge_style_default;

    // sequence diagrams (edges are the messages)
    std::vector<std::string> sequence_participants;
    std::vector<SequenceFrame> sequence_frames;
    std::vector<SequenceNote> sequence_notes;
    std::vector<SequenceActivation> sequence_activations;
    std::optional<size_t> sequence_autonumber;
    std::vector<SequenceBox> sequence_boxes;

    // state diagrams
    std::vector<StateNote> state_notes;

    // block diagrams
    std::optional<BlockDiagram> block;

    // mindmaps
    MindmapData mindmap;

    // git graphs
    GitGraphData gitgraph;

    // c4 diagrams
    C4Data c4;

    // architecture: edge port directions as written (from:PORT -- PORT:to), keyed by edge index
    std::map<size_t, std::pair<std::optional<ArchDir>, std::optional<ArchDir>>> arch_edge_ports;

    // xy chart
    XYChartData xychart;

    // quadrant
    QuadrantData quadrant;

    // timeline
    TimelineData timeline;

    // journey
    std::optional<std::string> journey_title;

    // pie charts
    std::vector<PieSlice> pie_slices;
    std::optional<std::string> pie_title;
    bool pie_show_data = false;

    // gantt charts
    std::vector<GanttTask> gantt_tasks;
    std::optional<std::string> gantt_title;
    std::vector<std::string> gantt_sections;
    std::optional<std::string> gantt_display_mode;

    // Rust Graph::ensure_node: create the node on first mention; a later label/shape overrides.
    void ensure_node(const std::string& id, const std::optional<std::string>& label,
                     const std::optional<NodeShape>& shape);
};

}  // namespace mermaid
