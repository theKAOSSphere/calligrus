// Mermaid port: layout configuration and theme. Mirrors the defaults of
// mermaid-rs-renderer 0.3.1 src/config.rs (LayoutConfig::default) and the theme that
// the reference implementation builds (Theme::modern() with the "ink" overrides).
// Only the fields the flowchart pipeline reads are present; add the rest when needed.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace mermaid {

struct FlowchartAutoSpacingBucket {
    size_t min_nodes;
    float scale;
};

struct FlowchartAutoSpacingConfig {
    bool enabled = true;
    float min_spacing = 24.0f;
    float density_threshold = 1.5f;
    float dense_scale_floor = 0.7f;
    std::vector<FlowchartAutoSpacingBucket> buckets = {
        {0, 1.0f}, {50, 0.75f}, {80, 0.6f}, {120, 0.45f}, {160, 0.3f}};
};

struct FlowchartRoutingConfig {
    bool enable_grid_router = true;
    float grid_cell = 16.0f;
    float turn_penalty = 0.6f;
    float occupancy_weight = 1.2f;
    size_t max_steps = 160000;
    bool snap_ports_to_grid = true;
};

struct FlowchartObjectiveConfig {
    bool enabled = true;
    float max_aspect_ratio = 9.0f;
    size_t wrap_min_groups = 4;
    float wrap_main_gap_scale = 1.15f;
    float wrap_cross_gap_scale = 1.35f;
    size_t edge_relax_passes = 6;
    float edge_gap_floor_ratio = 0.55f;
    float edge_label_weight = 0.9f;
    float endpoint_label_weight = 0.75f;
    float backedge_cross_weight = 0.65f;
};

struct FlowchartLayoutConfig {
    size_t order_passes = 4;
    float port_pad_ratio = 0.2f;
    float port_pad_min = 4.0f;
    float port_pad_max = 30.0f;
    float port_side_bias = 0.0f;
    FlowchartAutoSpacingConfig auto_spacing;
    FlowchartRoutingConfig routing;
    FlowchartObjectiveConfig objective;
};

// config.rs RequirementConfig (defaults).
struct RequirementConfig {
    std::string fill = "#ECECFF";
    std::string box_stroke = "#C7D2E5";
    float box_stroke_width = 1.0f;
    std::string stroke = "#9370DB";
    float stroke_width = 1.3f;
    std::string label_color = "#131300";
    std::string divider_color = "#9370DB";
    float divider_width = 1.3f;
    float header_band_height = 52.0f;
    float header_line_gap = 16.0f;
    float label_padding_x = 8.0f;
    float label_padding_y = 6.0f;
    std::string edge_stroke = "#333333";
    std::string edge_dasharray = "10,7";
    float edge_stroke_width = 1.0f;
    std::string edge_label_color = "#000000";
    std::string edge_label_background = "rgba(232,232,232, 0.8)";
    float edge_label_padding_x = 3.0f;
    float edge_label_padding_y = 2.0f;
    bool edge_label_brackets = true;
    float render_padding_x = 4.0f;
    float render_padding_y = 0.0f;
};

// config.rs PieConfig (the fields layout and writer read).
struct PieConfig {
    float text_position = 0.75f;
    float height = 360.0f;
    float margin = 28.0f;
    float legend_rect_size = 14.0f;
    float legend_spacing = 3.0f;
    float min_percent = 1.0f;
};

// config.rs MindmapConfig (defaults; layout_algorithm is "cose-bilkent" = the radial layout unless an init directive says otherwise).
struct MindmapConfig {
    bool use_max_width = true;
    float padding = 9.853f;
    float max_node_width = 200.0f;
    std::string layout_algorithm = "cose-bilkent";
    float node_spacing = 50.0f;
    float rank_spacing = 50.0f;
    float node_spacing_multiplier = 1.0f;
    float rank_spacing_multiplier = 1.07f;
    float text_width_scale = 1.0f;
    float rounded_padding = 15.0f;
    float rect_padding = 10.0f;
    float circle_padding = 10.271f;
    float hexagon_padding_multiplier = 2.0f;
    float default_corner_radius = 5.0f;
    float edge_depth_base_width = 17.0f;
    float edge_depth_step = -3.0f;
    float divider_line_width = 3.0f;
    std::vector<std::string> section_colors = {
        "hsl(240, 100%, 76.2745098039%)", "hsl(60, 100%, 73.5294117647%)",  "hsl(80, 100%, 76.2745098039%)",
        "hsl(270, 100%, 76.2745098039%)", "hsl(300, 100%, 76.2745098039%)", "hsl(330, 100%, 76.2745098039%)",
        "hsl(0, 100%, 76.2745098039%)",   "hsl(30, 100%, 76.2745098039%)",  "hsl(90, 100%, 76.2745098039%)",
        "hsl(150, 100%, 76.2745098039%)", "hsl(180, 100%, 76.2745098039%)", "hsl(210, 100%, 76.2745098039%)"};
    std::vector<std::string> section_label_colors = {"#ffffff", "black", "black", "#ffffff", "black", "black",
                                                     "black",   "black", "black", "black",   "black", "black"};
    std::vector<std::string> section_line_colors = {
        "hsl(60, 100%, 86.2745098039%)",  "hsl(240, 100%, 83.5294117647%)", "hsl(260, 100%, 86.2745098039%)",
        "hsl(90, 100%, 86.2745098039%)",  "hsl(120, 100%, 86.2745098039%)", "hsl(150, 100%, 86.2745098039%)",
        "hsl(180, 100%, 86.2745098039%)", "hsl(210, 100%, 86.2745098039%)", "hsl(270, 100%, 86.2745098039%)",
        "hsl(330, 100%, 86.2745098039%)", "hsl(0, 100%, 86.2745098039%)",   "hsl(30, 100%, 86.2745098039%)"};
    std::optional<std::string> root_fill, root_text, edge_color;
};

// config.rs GitGraphConfig (defaults).
struct GitGraphConfig {
    float diagram_padding = 6.0f, title_top_margin = 22.0f;
    bool use_max_width = true;
    std::string main_branch_name = "main";
    float main_branch_order = 0.0f;
    bool show_commit_label = true, show_branches = true, rotate_commit_label = true, parallel_commits = false;
    float commit_step = 34.0f, layout_offset = 8.0f, default_pos = 24.0f, branch_spacing = 40.0f;
    float branch_spacing_rotate_extra = 32.0f, branch_label_rotate_extra = 24.0f, branch_label_translate_x = -16.0f;
    float branch_label_bg_offset_x = 3.0f, branch_label_bg_offset_y = 6.0f, branch_label_bg_pad_x = 14.0f, branch_label_bg_pad_y = 3.0f;
    float branch_label_text_offset_x = 10.0f, branch_label_text_offset_y = -1.0f;
    float branch_label_tb_bg_offset_x = 8.0f, branch_label_tb_text_offset_x = 4.0f, branch_label_tb_offset_y = 0.0f, branch_label_bt_offset_y = 0.0f;
    float branch_label_corner_radius = 4.0f, branch_label_font_size = 0.0f, branch_label_line_height = 1.54f, text_width_scale = 1.0f;
    float commit_label_font_size = 10.0f, commit_label_line_height = 1.2f, commit_label_offset_y = 20.0f, commit_label_bg_offset_y = 10.5f;
    float commit_label_padding = 1.5f, commit_label_bg_opacity = 0.5f, commit_label_rotate_angle = -45.0f;
    float commit_label_rotate_translate_x_base = -6.0f, commit_label_rotate_translate_x_scale = 8.0f / 25.0f;
    float commit_label_rotate_translate_x_width_offset = 8.0f, commit_label_rotate_translate_y_base = 8.0f;
    float commit_label_rotate_translate_y_scale = 7.5f / 25.0f;
    float commit_label_tb_text_extra = 12.0f, commit_label_tb_bg_extra = 16.0f, commit_label_tb_text_offset_y = -10.0f, commit_label_tb_bg_offset_y = -10.0f;
    float tag_label_font_size = 10.0f, tag_label_line_height = 1.2f, tag_text_offset_y = 13.0f, tag_polygon_offset_y = 16.0f, tag_spacing_y = 16.0f;
    float tag_padding_x = 3.0f, tag_padding_y = 1.5f, tag_hole_radius = 1.3f, tag_rotate_translate = 10.0f, tag_text_rotate_translate = 12.0f;
    float tag_rotate_angle = 45.0f, tag_text_offset_x_tb = 4.0f, tag_text_offset_y_tb = 2.0f;
    float arrow_reroute_radius = 8.0f, arrow_radius = 16.0f, lane_spacing = 8.0f;
    size_t lane_max_depth = 5;
    float commit_radius = 8.0f, merge_radius_outer = 7.5f, merge_radius_inner = 5.0f, highlight_outer_size = 16.0f, highlight_inner_size = 10.0f;
    float reverse_cross_size = 4.0f, reverse_stroke_width = 2.5f, cherry_pick_dot_radius = 2.2f, cherry_pick_dot_offset_x = 2.5f;
    float cherry_pick_dot_offset_y = 1.6f, cherry_pick_stem_start_offset_y = 0.8f, cherry_pick_stem_end_offset_y = -4.0f;
    float cherry_pick_stem_stroke_width = 0.8f;
    std::string cherry_pick_accent_color = "#fff";
    float arrow_stroke_width = 6.0f, branch_stroke_width = 0.8f;
    std::string branch_dasharray = "2";
};

struct LayoutConfig {
    float node_spacing = 50.0f;
    float rank_spacing = 50.0f;
    float node_padding_x = 30.0f;
    float node_padding_y = 15.0f;
    float label_line_height = 1.5f;
    size_t max_label_width_chars = 22;
    bool fast_text_metrics = false;
    FlowchartLayoutConfig flowchart;
    RequirementConfig requirement;
    PieConfig pie;
    MindmapConfig mindmap;
    GitGraphConfig gitgraph;
    // class diagrams use tighter lines inside a class box
    float class_label_line_height() const { return label_line_height * 0.85f; }
    // preferred_aspect_ratio is None in the app, so the aspect-ratio passes are no-ops.
};

// The ink theme (see ink_theme() in the reference implementation): Theme::modern() with black lines and
// text, white background and pale grey node fills. The fields the ink theme does not override keep
// the modern theme's values (pie palette, git colours, ...), so those are listed too.
struct Theme {
    Theme();  // theme.cpp: fills pie_colors (derived with adjust_color) and the git palettes

    std::string font_family = "\"IBM Plex Sans\", sans-serif";
    float font_size = 14.0f;
    std::string primary_color = "#EBEBEB";
    std::string primary_text_color = "#000000";
    std::string primary_border_color = "#000000";
    std::string line_color = "#000000";
    std::string secondary_color = "#EBEBEB";
    std::string tertiary_color = "#FFFFFF";
    std::string edge_label_background = "#FFFFFF";
    std::string cluster_background = "#F5F5F5";
    std::string cluster_border = "#808080";
    std::string background = "#FFFFFF";
    std::string text_color = "#000000";
    // sequence diagrams
    std::string sequence_actor_fill = "#EBEBEB";
    std::string sequence_actor_border = "#000000";
    std::string sequence_actor_line = "#606060";
    std::string sequence_note_fill = "#F5F5F5";
    std::string sequence_note_border = "#606060";
    std::string sequence_activation_fill = "#EBEBEB";
    std::string sequence_activation_border = "#000000";
    // git graphs
    std::array<std::string, 8> git_colors, git_inv_colors, git_branch_label_colors;
    std::string git_commit_label_color, git_commit_label_background, git_tag_label_color, git_tag_label_background,
        git_tag_label_border;
    // pie charts
    std::array<std::string, 12> pie_colors;
    float pie_title_text_size = 25.0f;
    std::string pie_title_text_color = "#000000";
    float pie_section_text_size = 17.0f;
    std::string pie_section_text_color = "#000000";
    float pie_legend_text_size = 17.0f;
    std::string pie_legend_text_color = "#000000";
    std::string pie_stroke_color = "#000000";
    float pie_stroke_width = 1.6f;
    float pie_outer_stroke_width = 1.6f;
    std::string pie_outer_stroke_color = "#CBD5E1";
    float pie_opacity = 0.85f;
};

// theme.rs adjust_color(): shift the hue / saturation / lightness of a colour ("hsl(h, s%, l%)" text).
std::string adjust_color(const std::string& color, float delta_h, float delta_s, float delta_l);
// theme.rs parse_color_to_hsl(): (h, s, l) of "#rgb", "#rrggbb", "#rrggbbaa" or "hsl(...)" colours.
bool parse_color_to_hsl(const std::string& color, float& h, float& s, float& l);

}  // namespace mermaid
