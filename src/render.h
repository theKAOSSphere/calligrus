// Draws a parsed Document with ImDrawList calls.
//
// Everything is laid out by hand (top to bottom, one block after another) in a
// single reading column. The caller puts this inside a scrolling ImGui child
// window and reserves `height` pixels so the scrollbar knows the content size.
// Sizes and gaps mirror the Rust port so the two apps look the same.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>

#include "fonts.h"
#include "inline_layout.h"
#include "model.h"
#include "slides.h"

struct OutlineEntry {
    int level;
    std::string title;
    float y;  // offset of the heading from the top of the document, in pixels
};

struct RenderResult {
    float height = 0;                   // total height of the drawn document
    std::vector<OutlineEntry> outline;  // headings, for the side panel
    std::vector<LinkHit> links;         // clickable rectangles (screen coords)
    bool mouse_captured = false;        // a code block's Copy button / scrollbar took the mouse press
    bool over_code_control = false;     // the mouse is over such a control (normal cursor, no I-beam)
    bool animating = false;             // something changes by itself soon ("Copied" label): keep drawing
};

// Per-frame switches for render_document.
struct RenderOptions {
    bool line_numbers = false;  // code blocks get a line-number gutter (config [code] line_numbers)
    bool interactive = false;   // handle the mouse (code blocks' Copy button, horizontal scrolling);
                                // false for off-screen measuring passes
    bool mouse_in_view = false; // the mouse is over the document view (not the toolbar, a popup...)
    const std::vector<TextRange>* find = nullptr;  // find-in-page matches to highlight
    int find_current = -1;                         // ...and which one is the current one
    float margin = 600.0f;      // blocks this far (px) outside [view_top, view_bottom] are still drawn (slides: 0)
    // Slides mode (spec 26): where each top-level block goes (column layouts); null = one column.
    const std::vector<slides::Placement>* columns = nullptr;
    // Slides mode pauses: blocks from this index on keep their space but are not drawn (nor clickable),
    // except that block `hide_from` shows its first `hide_items` items when that is > 0 (an incremental list).
    size_t hide_from = SIZE_MAX;
    size_t hide_items = 0;
    // Slides mode: each top-level block's empty lines above, alignment and text size (null = all plain).
    const std::vector<slides::TextStyle>* styles = nullptr;
    // Slides mode: blocks that are background pictures (non-zero): not drawn, no height (null: none).
    const std::vector<char>* backgrounds = nullptr;
};

// A document export (pages or slides, PDF or pictures) is being drawn: no scrollbars, and code lines wider
// than the column wrap (continuation lines indented by 2 columns with a small arrow) instead of scrolling.
// Set once at start-up; the on-screen app never sets it.
void render_set_export(bool on);

// Resolve an image reference relative to the open document's folder `base_dir`.
std::filesystem::path resolve(const std::filesystem::path& base_dir, std::string src);

// A code block's horizontal scroll state (long lines scroll instead of wrapping).
struct CodeScroll {
    float offset = 0;          // px scrolled to the right
    double copied_at = -1e9;   // ImGui time of the last Copy click ("Copied" for 1.5 s)
};

// One row of a code block (a line) or a table: for the editor's Ctrl+click (spec 23).
struct RowBand {
    float top = 0, height = 0;  // from the top of the document
};

// Where one list item / quote child (a callout's title first) sits: for the editor's cursor marker.
// `rows`: the code lines / table rows when the child is a code block / table.
struct SubRect {
    float top = 0, height = 0;  // from the top of the document
    std::vector<RowBand> rows;
};

// Mixed into a layout key when column layouts are on (they change every block's width).
constexpr size_t kColumnsKey = 0x9e3779b9;
constexpr size_t kStylesKey = 0x85ebca6b;  // (slides text styles change heights too)
constexpr size_t kBackgroundsKey = 0xc2b2ae35;  // (...and so do background pictures, which take no room)

// Remembers each top-level block's height so blocks far outside the visible
// area can be skipped without laying them out (the Rust version does the same).
struct RenderCache {
    size_t key = 0;               // document version + width + text size
    std::vector<float> height;    // NaN = not measured yet
    std::vector<float> head_dy;   // headings: outline offset from the block's top
    std::vector<float> top;       // top of each block, from the top of the document
    std::vector<float> space;     // slides `new_lines:`: empty space just above each block (px)
    // Slides columns: the x range (from the document's left edge) whose clicks belong to each
    // block: its column widened to the middle of the gaps; full width blocks: everything.
    std::vector<std::pair<float, float>> hit_x;
    std::vector<std::vector<SubRect>> sub;  // top-level lists: each item; quotes: [callout title,] each child
    std::vector<std::vector<RowBand>> rows;  // top-level code blocks / tables: their lines / rows
    std::vector<RowBand> rows_now;           // the code block / table drawn last (taken by whoever records it)

    // Code blocks, keyed by their text (stable while the document is unchanged).
    std::map<const char*, CodeScroll> code;
    const char* dragging = nullptr;  // code block whose scrollbar is being dragged
    float drag_grab = 0;             // mouse x minus thumb x when the drag started
    size_t code_version = SIZE_MAX;  // document version `code` belongs to
};

// Draw `doc` with its top-left corner at `origin` (screen coords), wrapping to
// `width`. Only blocks overlapping [view_top, view_bottom] (screen y) are drawn
// once their height is known. `version` must change whenever `doc` changes.
// Relative image paths are resolved against `base_dir` (the document's folder).
// Painted text is reported to `sel` (for selection); `sel_range` gets highlighted.
RenderResult render_document(ImDrawList* dl, const md::Document& doc, const Fonts& fonts, const Theme& theme,
                             ImVec2 origin, float width, RenderCache& cache, size_t version,
                             float view_top, float view_bottom, const std::filesystem::path& base_dir,
                             Collector& sel, std::optional<TextRange> sel_range, ImU32 highlight,
                             const RenderOptions& opts);
