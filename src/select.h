// Text selection: where every visible character is, what's selected, and what
// text a selection copies. Same design as the Rust port:
//
// While painting, every word / space / equation is reported as a Run (its line
// box, the x of each character boundary, and the text it copies as). Selection
// endpoints are TextPos values (top-level block, token within that block,
// character within the token), which stay valid across frames as long as the
// document and column width don't change.
#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

struct TextPos {
    uint32_t block = 0, tok = 0, ch = 0;
    auto operator<=>(const TextPos&) const = default;  // reading order

    static constexpr TextPos start() { return {0, 0, 0}; }
    static constexpr TextPos end() { return {UINT32_MAX, UINT32_MAX, UINT32_MAX}; }
};

using TextRange = std::pair<TextPos, TextPos>;

// What separates a group from the previous one when copying.
enum class Sep {
    Para,  // blank line: paragraphs, headings, list items, equations
    Line,  // newline: lines of a code block
    Cell,  // tab: next cell in the same table row
    Row,   // newline: first cell of a new table row
};

struct Run {
    TextPos pos;              // first character (ch = 0)
    ImVec2 min, max;          // the line box it sits in (screen coordinates)
    std::vector<float> xs;    // x of each character boundary (chars + 1 values)
    std::string text;         // what copying it produces (UTF-8; "$..$" for equations)
    uint32_t group = 0;       // paragraph / cell / code-line group
    Sep sep = Sep::Para;      // separator before this group
    uint32_t line = 0;        // visual line (change within a group = soft wrap)
    bool hard = false;        // line starts after a hard line break
    bool glued = false;       // continues the previous run without a space (a long word cut at a soft wrap)

    uint32_t len() const { return xs.empty() ? 0 : uint32_t(xs.size() - 1); }
    // Selected character range of this run, if any.
    std::optional<std::pair<uint32_t, uint32_t>> selected(const TextRange& r) const;
    bool contains(ImVec2 p) const { return p.x >= min.x && p.x < max.x && p.y >= min.y && p.y < max.y; }
};

struct Selection {
    std::optional<TextPos> anchor, focus;
    std::optional<TextRange> range() const;
    void clear() { anchor.reset(), focus.reset(); }
    void set(TextPos a, TextPos f) { anchor = a, focus = f; }
};

// A find-in-page match painted on one run (see find.h).
struct FindHighlight {
    float x0, x1;
    bool current;  // the match the find bar is on
};

// Collects the runs painted during one frame.
class Collector {
public:
    std::vector<Run> runs;

    // `find` = all find matches (sorted, may be null), `find_current` = index of the current one.
    void begin_frame(std::optional<TextRange> range, const std::vector<TextRange>* find = nullptr, int find_current = -1);
    void begin_block(size_t block) { block_ = uint32_t(block), tok_ = 0; }
    void begin_group(Sep sep) { group_++, sep_ = sep; }
    void new_line() { line_++; }
    // The formulas painted this frame (box and TeX source with its `$` / `$$`): the right-click menu
    // offers "Copy LaTeX" on one (spec 27).
    struct Math {
        ImVec2 min, max;
        std::string src;
    };
    std::vector<Math> maths;
    bool next_glued = false;  // set before push() for a run that continues the previous one (Run::glued); push() clears it
    void math(ImVec2 min, ImVec2 max, std::string src) { maths.push_back({min, max, std::move(src)}); }
    // Record a painted run; returns the x-range to highlight if (part of) it is selected.
    std::optional<std::pair<float, float>> push(ImVec2 min, ImVec2 max, std::vector<float> xs, std::string text, bool hard);
    // Find matches on the run pushed last (drawn behind the selection highlight).
    const std::vector<FindHighlight>& find_highlights() const { return find_hl_; }

private:
    std::optional<TextRange> range_;
    const std::vector<TextRange>* find_ = nullptr;
    int find_current_ = -1;
    std::vector<FindHighlight> find_hl_;
    uint32_t block_ = 0, tok_ = 0, group_ = 0, line_ = 0;
    Sep sep_ = Sep::Para;
};

// Nearest position to `p`: closest line first, then closest character boundary.
std::optional<TextPos> hit(const std::vector<Run>& runs, ImVec2 p);
// Double-click: the word (run) at `p`. Triple-click: its paragraph / cell / code line.
std::optional<TextRange> word_at(const std::vector<Run>& runs, TextPos p);
std::optional<TextRange> group_at(const std::vector<Run>& runs, TextPos p);
// The text a selection copies, with paragraph breaks, newlines and tabs.
std::string selected_text(const std::vector<Run>& runs, const TextRange& range);
