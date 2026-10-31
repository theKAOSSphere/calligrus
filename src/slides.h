// Slides mode (spec 26): splitting a document into slides and the slide zoom.
// Pure logic, no drawing (main.cpp does the drawing with the normal renderer).
#pragma once

#include <array>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imagefilter.h"
#include "model.h"

namespace slides {

// One slide: a range of top-level block indices, inclusive.
struct Range {
    size_t first = 0, last = 0;
};

// Split `doc` into slides: the runs between top-level rules (rules not drawn, empty runs
// skipped) or, with no rule, a slide per top-level heading of the smallest level present
// (blocks before the first one form a slide of their own); no rules and no headings: one slide.
// An empty document has no slides.
std::vector<Range> split(const md::Document& doc);

// The slide that holds `block` (a rule between slides: the slide before it).
size_t slide_of_block(const std::vector<Range>& slides, size_t block);

// Slide `n`'s source lines (1-based): the first line of its first block, and the last line
// before the next slide's first block (`line_count`, the document's last line, for the last
// slide). The editor puts its cursor on the first when the slide comes on screen.
std::pair<int, int> slide_lines(const std::vector<Range>& slides, const md::Document& doc, size_t n, int line_count);

// Pause lines (spec 26): the 1-based source lines holding only `<!-- pause -->` (Markdown,
// indented by at most 3 spaces) or `# pause` (Org, a comment line, not indented), outside
// fenced blocks (Markdown ``` / ~~~, Org #+begin_ ... #+end_).
std::vector<int> pause_lines(std::string_view src, bool org);

// Like `split`, but with `end_slide` lines (see end_slide_lines) the slides are the runs between
// them instead: a slide ends before the first block below each line, and rules are just rules.
std::vector<Range> split_with_ends(const md::Document& doc, const std::vector<int>& ends);

// The 1-based lines of `<!-- end_slide -->` (Org: `# end_slide`), found like pauses.
std::vector<int> end_slide_lines(std::string_view src, bool org);

// `incremental_lists: true` / `false` lines (presenterm), found like pauses: (line, on).
std::vector<std::pair<int, bool>> incremental_lines(std::string_view src, bool org);

// Where a step starts: (top-level block, item). Shown before it: the blocks before `first`, and
// the first `second` items of block `first` (a list); item 0 = the whole block is hidden.
using Step = std::pair<size_t, size_t>;

// For each slide, where its steps start, ascending: step 0 shows the slide up to the first of
// them, step k up to the (k+1)-th, the last step all of it.
// A pause starts its step at the next top-level block, and counts only with blocks of the
// same slide before and after it. An incremental list (after `incremental_lists: true` on its
// slide, until `false`; a switch applies from the next block of a slide below it) starts a step
// at each item after its first.
std::vector<std::vector<Step>> steps(const std::vector<Range>& slides, const md::Document& doc, const std::vector<int>& pauses,
                                     const std::vector<std::pair<int, bool>>& incremental);

// The step of a slide (its `steps` entry) that shows `block`.
size_t step_of_block(const std::vector<Step>& steps, size_t block);

// A column directive line (spec 26, presenterm syntax), recognised like pauses:
// `<!-- column_layout: [2, 1] -->`, `<!-- column: 0 -->`, `<!-- reset_layout -->` (Org: `# column_layout: [2, 1]`...).
struct Directive {
    enum class Kind { Layout, Column, Reset };
    int line = 0;              // 1-based source line
    Kind kind = Kind::Reset;
    std::vector<int> weights;  // Layout: 2 to 4 positive weights
    int column = 0;            // Column: the 0-based column
};

// The column directives of `src`, in source order (malformed ones left out).
std::vector<Directive> layout_lines(std::string_view src, bool org);

// Where a top-level block goes in slides mode.
struct Placement {
    int row = -1;              // which layout (row) it is in; -1 = full width
    int column = 0;            // ...its column there
    std::vector<int> weights;  // ...and that layout's column weights
};

// Per top-level block, its place under `directives` (a directive applies from the next top-level
// block on; the end of a slide closes a layout). Empty when no block is in a column.
std::vector<Placement> placements(const std::vector<Range>& slides, const md::Document& doc, const std::vector<Directive>& directives);

// A slide's background picture (Marp's `![bg]`, `![bg right:40%]`, spec 26): behind the whole
// slide, or a strip on the left / right edge `frac` of the slide wide (the text goes in the rest).
enum class BgSide { Full, Left, Right };
struct Background {
    std::string src;
    BgSide side = BgSide::Full;
    float frac = 0.5f;     // Left / Right: the strip's share
    bool contain = false;  // `contain` / `fit`: the whole picture shows; else it covers its area (cropped)
    bool vertical = false;  // `vertical`: a slide's several background pictures are stacked, not side by side
    std::vector<imagefilter::Filter> filters;  // Marp's filters (`sepia`, `blur:4px`...), in the order written
    bool operator==(const Background&) const = default;
};

// Is this picture's alt text a Marp background (`bg`, then `left` / `right[:NN%]`, `contain` / `fit`,
// `cover`, `vertical` and filters, in any order)? An unknown word means it is just a picture.
std::optional<Background> background_of(std::string_view alt, std::string_view src);

// A block that is only background pictures: their backgrounds (empty for any other block). In
// slides mode such a block is not drawn in the text; its pictures are the slide's background.
std::vector<Background> block_backgrounds(const md::Block& b);

// For every slide, its background pictures in order (empty: none).
std::vector<std::vector<Background>> slide_backgrounds(const std::vector<Range>& slides, const md::Document& doc);

// Where a slide's background pictures go: the first one that names a side (all of them share that
// strip), else nullptr (behind the whole slide).
const Background* backgrounds_strip(const std::vector<Background>& bgs);

// The cells of `area` (x, y, w, h) that a slide's background pictures get: equal parts side by side,
// or stacked if any of them says `vertical` (Marp).
std::vector<std::array<float, 4>> background_cells(const std::vector<Background>& bgs, std::array<float, 4> area);

// A strip background splits its slide like a two-column layout: the weights, the column the text
// goes in, and where the picture starts / ends (the middle of the gap, units from the slide's left
// edge); the picture reaches from there to the window's edge. Nothing for a full background.
struct BgSplit {
    std::vector<int> weights;
    int text_column = 0;
    float edge = 0;
};
std::optional<BgSplit> background_split(const Background& bg);

// Slides with a strip background: their full-width blocks go in the text column (blocks in the
// slide's own columns stay where they are). `places` may be empty (all full width) on entry.
void place_backgrounds(std::vector<Placement>& places, const std::vector<Range>& slides,
                       const std::vector<std::vector<Background>>& bgs, size_t blocks);

// The overview grid (spec 26, `o`): `n` slides in a `w` x `h` view as pictures `aspect` wide per unit
// of height (the audience window's shape: the grid may be in the presenter window), with `gap` around
// the cells and a `label` line under each. As many columns as make the cells biggest.
struct Grid {
    size_t cols = 1;
    float cell_w = 0, cell_h = 0;
};
Grid overview_grid(size_t n, float w, float h, float aspect, float gap, float label);

// Overview cells are never narrower than this (pixels): a deck too long for that scrolls.
constexpr float kOverviewMinCell = 160.0f;
// overview_grid, unless its cells would be narrower than `min_w`: then as many columns of cells at
// least `min_w` wide as fit across (at least one), and the grid is taller than the view (it scrolls).
Grid overview_grid_min(size_t n, float w, float h, float aspect, float gap, float label, float min_w);
// The overview grid's whole height: its rows (cell and label line) with `gap` around them.
float overview_height(size_t n, size_t cols, float cell_h, float gap, float label);
// Scroll the overview (`scroll` px from its top, in a `view_h` tall view) so the picked row, `top` ..
// `bottom` px from the grid's top, shows with `gap` around it.
float overview_follow(float scroll, float top, float bottom, float view_h, float gap);

// The presenter view's parts (spec 26, `s`) in a `w` x `h` window (pixels): the top bar's height, then
// the boxes {x, y, width, height} of the current slide's picture (left), the next one's (right, at the
// top) and the notes (right, below it). The pictures have the audience window's `aspect`; each box has
// a label line above it.
struct PresenterLayout {
    float bar = 0;
    float now[4] = {}, next[4] = {}, notes[4] = {};
};
constexpr float kPresenterPad = 16, kPresenterBar = 56, kPresenterLabel = 24;
PresenterLayout presenter_layout(float w, float h, float aspect);

// The overview's arrow keys: from slide `at`, move by `dx` columns and `dy` rows in a grid of `cols`
// columns holding `n` slides (stopping at the edges).
size_t overview_move(size_t at, int dx, int dy, size_t cols, size_t n);

// Presenterm text directives (spec 26), found like pauses: `new_lines: n` / `newline` (n empty
// lines before the next block), `alignment: left|center|right` and `font_size: n` (1 to 7; both
// from the next block to the end of the slide).
enum class TextAlign { Left, Center, Right };
struct TextDirective {
    enum class Kind { NewLines, Align, FontSize };
    int line = 0;  // 1-based source line
    Kind kind = Kind::NewLines;
    int n = 0;     // NewLines: how many; FontSize: the size
    TextAlign align = TextAlign::Left;
};
std::vector<TextDirective> text_lines(std::string_view src, bool org);

// How a top-level block is drawn on its slide; the default is what normal mode does.
struct TextStyle {
    int lines_before = 0;                // empty body lines above it
    TextAlign align = TextAlign::Left;   // paragraphs and headings
    int font_size = 1;                   // text size, times normal
    bool operator==(const TextStyle&) const = default;
};

// Per top-level block (a directive applies from the next block of a slide below it; each slide
// starts plain).
std::vector<TextStyle> text_styles(const std::vector<Range>& slides, const md::Document& doc, const std::vector<TextDirective>& directives);

// Speaker notes (spec 26): (1-based first line, text). Markdown: `<!-- speaker_note: text -->`
// on one line, or `<!-- speaker_note: |`, the text lines, then `-->`. Org: `#+begin_notes` ..
// `#+end_notes`. Not inside fenced / other blocks.
std::vector<std::pair<int, std::string>> note_lines(std::string_view src, bool org);

// Each slide's notes, joined by blank lines: a note belongs to the slide of the block at or
// above it (one just below a slide-ending rule: the next slide).
std::vector<std::string> slide_notes(const std::vector<Range>& slides, const md::Document& doc, const std::vector<std::pair<int, std::string>>& notes);

// Zoom for a slide `h` units tall in a window of w x h_win units (both at zoom 1):
// min(w / 760, 3, h_win / (h + 80)), but never below 0.6 * min(w / 760, 3).
float zoom(float w, float h_win, float h);

}  // namespace slides
