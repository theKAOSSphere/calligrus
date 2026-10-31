// Mermaid port: the laid-out diagram. Mirrors layout/types.rs (flowchart subset).
#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "c4_types.h"
#include "ir.h"
#include "text.h"

namespace mermaid {

using Point = std::pair<float, float>;

struct NodeLayout {
    std::string id;
    float x = 0, y = 0, width = 0, height = 0;
    TextBlock label;
    NodeShape shape = NodeShape::Rectangle;
    NodeStyle style;
    std::optional<NodeLink> link;
    std::optional<size_t> anchor_subgraph;
    bool hidden = false;
    std::optional<std::string> icon;
};

struct EdgeLayout {
    std::string from, to;
    std::optional<TextBlock> label, start_label, end_label;
    std::optional<Point> label_anchor, start_label_anchor, end_label_anchor;
    std::vector<Point> points;
    bool directed = false;
    bool arrow_start = false, arrow_end = false;
    std::optional<EdgeArrowhead> arrow_start_kind, arrow_end_kind;
    std::optional<EdgeDecoration> start_decoration, end_decoration;
    EdgeStyle style = EdgeStyle::Solid;
    EdgeStyleOverride override_style;
};

struct SubgraphLayout {
    std::string label;
    TextBlock label_block;
    std::vector<std::string> nodes;
    float x = 0, y = 0, width = 0, height = 0;
    NodeStyle style;
    std::optional<std::string> icon;
};

// ---- sequence diagram pieces (layout/types.rs) ----
struct Lifeline {
    std::string id;
    float x = 0, y1 = 0, y2 = 0;
};
struct SequenceLabel {
    float x = 0, y = 0;
    TextBlock text;
};
struct SequenceFrameLayout {
    SequenceFrameKind kind = SequenceFrameKind::Alt;
    float x = 0, y = 0, width = 0, height = 0;
    std::array<float, 4> label_box{0, 0, 0, 0};  // x, y, w, h
    SequenceLabel label;
    std::vector<SequenceLabel> section_labels;
    std::vector<float> dividers;
};
struct SequenceBoxLayout {
    float x = 0, y = 0, width = 0, height = 0;
    std::optional<TextBlock> label;
    std::optional<std::string> color;
};
struct SequenceNoteLayout {
    float x = 0, y = 0, width = 0, height = 0;
    TextBlock label;
    SequenceNotePosition position = SequenceNotePosition::Over;
    std::vector<std::string> participants;
    size_t index = 0;
};
struct SequenceActivationLayout {
    float x = 0, y = 0, width = 0, height = 0;
    std::string participant;
    size_t depth = 0;
};
struct SequenceNumberLayout {
    float x = 0, y = 0;
    size_t value = 0;
};
struct SequenceData {
    std::vector<Lifeline> lifelines;
    std::vector<NodeLayout> footboxes;
    std::vector<SequenceBoxLayout> boxes;
    std::vector<SequenceFrameLayout> frames;
    std::vector<SequenceNoteLayout> notes;
    std::vector<SequenceActivationLayout> activations;
    std::vector<SequenceNumberLayout> numbers;
};

// ---- pie chart pieces ----
struct PieSliceLayout {
    TextBlock label;
    float value = 0, start_angle = 0, end_angle = 0;
    std::string color;
};
struct PieLegendItem {
    float x = 0, y = 0;
    TextBlock label;
    std::string color;
    float marker_size = 0, value = 0;
};
struct PieTitleLayout {
    float x = 0, y = 0;
    TextBlock text;
};
struct PieData {
    std::vector<PieSliceLayout> slices;
    std::vector<PieLegendItem> legend;
    Point center{0, 0};
    float radius = 0;
    std::optional<PieTitleLayout> title;
};

// ---- gantt chart pieces ----
struct GanttSectionLayout {
    TextBlock label;
    float y = 0, height = 0;
    std::string color, band_color;
};
struct GanttTaskLayout {
    TextBlock label;
    float x = 0, y = 0, width = 0, height = 0;
    std::string color;
    float start = 0, duration = 0;
    std::optional<GanttStatus> status;
};
struct GanttTick {
    float x = 0;
    std::string label;
};
struct GanttLayout {
    std::optional<TextBlock> title;
    std::vector<GanttSectionLayout> sections;
    std::vector<GanttTaskLayout> tasks;
    float time_start = 0, time_end = 0, chart_x = 0, chart_y = 0, chart_width = 0, chart_height = 0, row_height = 0;
    float label_x = 0, label_width = 0, section_label_x = 0, section_label_width = 0, task_label_x = 0, task_label_width = 0, title_y = 0;
    std::vector<GanttTick> ticks;
    bool compact = false;
};

// ---- journey pieces ----
struct JourneyActorLayout {
    std::string name, color;
    float x = 0, y = 0, radius = 0;
};
struct JourneyTaskLayout {
    std::string id;
    TextBlock label;
    float x = 0, y = 0, width = 0, height = 0;
    std::optional<float> score;
    std::string score_color;
    float score_y = 0;
    std::vector<std::string> actors;
    std::optional<float> actor_y;
    size_t section_idx = 0;
};
struct JourneySectionLayout {
    TextBlock label;
    float x = 0, y = 0, width = 0, height = 0;
    std::string color;
};
struct JourneyLayout {
    std::optional<TextBlock> title;
    float title_y = 0;
    std::vector<JourneyActorLayout> actors;
    float actor_label_y = 0;
    std::vector<JourneyTaskLayout> tasks;
    std::vector<JourneySectionLayout> sections;
    std::optional<std::array<float, 3>> baseline;
    float score_radius = 0, actor_radius = 0, actor_gap = 0, card_gap_y = 0, width = 0, height = 0;
};

// ---- timeline pieces ----
struct TimelineEventLayout {
    TextBlock time;
    std::vector<TextBlock> events;
    float x = 0, y = 0, width = 0, height = 0, circle_y = 0;
};
struct TimelineSectionLayout {
    TextBlock label;
    float x = 0, y = 0, width = 0, height = 0;
};
struct TimelineLayout {
    std::optional<TextBlock> title;
    float title_y = 0;
    std::vector<TimelineEventLayout> events;
    std::vector<TimelineSectionLayout> sections;
    Direction direction = Direction::LeftRight;
    float line_y = 0, line_start_x = 0, line_end_x = 0, line_start_y = 0, line_end_y = 0, width = 0, height = 0;
};

// ---- quadrant pieces ----
struct QuadrantPointLayout {
    TextBlock label;
    float x = 0, y = 0;
    std::string color;
};
struct QuadrantLayout {
    std::optional<TextBlock> title;
    float title_y = 0;
    std::optional<TextBlock> x_axis_left, x_axis_right, y_axis_bottom, y_axis_top;
    std::optional<TextBlock> quadrant_labels[4];
    std::vector<QuadrantPointLayout> points;
    float grid_x = 0, grid_y = 0, grid_width = 0, grid_height = 0;
};

// ---- xy chart pieces ----
struct XYChartBarLayout {
    float x = 0, y = 0, width = 0, height = 0, value = 0;
    std::string color;
};
struct XYChartLineLayout {
    std::vector<Point> points;
    std::string color;
};
struct XYChartLayout {
    std::optional<TextBlock> title;
    float title_y = 0;
    std::optional<TextBlock> x_axis_label;
    float x_axis_label_y = 0;
    std::optional<TextBlock> y_axis_label;
    float y_axis_label_x = 0;
    std::vector<std::pair<std::string, float>> x_axis_categories, y_axis_ticks;
    std::vector<XYChartBarLayout> bars;
    std::vector<XYChartLineLayout> lines;
    float plot_x = 0, plot_y = 0, plot_width = 0, plot_height = 0, width = 0, height = 0;
};

// A state diagram note box.
struct StateNoteLayout {
    float x = 0, y = 0, width = 0, height = 0;
    TextBlock label;
    StateNotePosition position = StateNotePosition::RightOf;
    std::string target;
};

// ---- git graph pieces (Stage 3b) ----
struct GitGraphBranchLabelLayout {
    float bg_x = 0, bg_y = 0, bg_width = 0, bg_height = 0, text_x = 0, text_y = 0, text_width = 0, text_height = 0;
};
struct GitGraphBranchLayout {
    std::string name;
    size_t index = 0;
    float pos = 0;
    GitGraphBranchLabelLayout label;
};
struct GitGraphTransform {
    float translate_x = 0, translate_y = 0, rotate_deg = 0, rotate_cx = 0, rotate_cy = 0;
};
struct GitGraphCommitLabelLayout {
    std::string text;
    float text_x = 0, text_y = 0, bg_x = 0, bg_y = 0, bg_width = 0, bg_height = 0;
    std::optional<GitGraphTransform> transform;
};
struct GitGraphTagLayout {
    std::string text;
    float text_x = 0, text_y = 0;
    std::vector<Point> points;
    float hole_x = 0, hole_y = 0;
    std::optional<GitGraphTransform> transform;
};
struct GitGraphCommitLayout {
    std::string id;
    size_t seq = 0, branch_index = 0;
    float x = 0, y = 0, axis_pos = 0;
    GitGraphCommitType commit_type = GitGraphCommitType::Normal;
    std::optional<GitGraphCommitType> custom_type;
    std::vector<GitGraphTagLayout> tags;
    std::optional<GitGraphCommitLabelLayout> label;
};
struct GitGraphArrowLayout {
    std::string path;
    size_t color_index = 0;
};
struct GitGraphLayout {
    std::vector<GitGraphBranchLayout> branches;
    std::vector<GitGraphCommitLayout> commits;
    std::vector<GitGraphArrowLayout> arrows;
    float width = 0, height = 0, offset_x = 0, offset_y = 0, max_pos = 0;
    Direction direction = Direction::LeftRight;
};

// ---- sankey pieces (Stage 3b) ----
struct SankeyNodeLayout {
    std::string id, label;
    float total = 0;
    size_t rank = 0;
    float x = 0, y = 0, width = 0, height = 0;
    std::string color;
};
struct SankeyLinkLayout {
    std::string source, target;
    float value = 0, thickness = 0;
    Point start{0, 0}, end{0, 0};
    std::string color_start, color_end, gradient_id;
};
struct SankeyData {
    float width = 0, height = 0, node_width = 0;
    std::vector<SankeyNodeLayout> nodes;
    std::vector<SankeyLinkLayout> links;
};

struct Layout {
    DiagramKind kind = DiagramKind::Flowchart;
    std::map<std::string, NodeLayout> nodes;  // BTreeMap in Rust: sorted by id
    std::vector<EdgeLayout> edges;
    std::vector<SubgraphLayout> subgraphs;
    float width = 0, height = 0;
    std::optional<SequenceData> sequence;  // set for sequence diagrams
    std::vector<StateNoteLayout> state_notes;  // state diagrams
    std::optional<PieData> pie;                // pie charts
    std::optional<GanttLayout> gantt;          // gantt charts
    std::optional<JourneyLayout> journey;      // journeys
    std::optional<TimelineLayout> timeline;    // timelines
    std::optional<QuadrantLayout> quadrant;    // quadrant charts
    std::optional<XYChartLayout> xychart;      // xy charts
    std::optional<SankeyData> sankey;          // sankey diagrams
    std::optional<GitGraphLayout> gitgraph;    // git graphs
    std::optional<C4Layout> c4;                // c4 diagrams
};

inline bool is_horizontal(Direction d) { return d == Direction::LeftRight || d == Direction::RightLeft; }

}  // namespace mermaid
