// Find in page (spec section 7): plain substring search over the document's
// text runs, the same runs text selection uses (select.h). A match lives in
// one group (paragraph, heading, list item, table cell or code line) and may
// cover several runs (words and the spaces between them). Equations are
// atomic runs and never match.
//
// Smartcase: case-insensitive unless the query contains an uppercase letter.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "select.h"

// Does `query` (UTF-8) switch to case-sensitive matching (has an uppercase letter)?
bool find_case_sensitive(std::string_view query);

// Non-overlapping matches of `needle` in `hay` (both UTF-32), left to right,
// as [start, end) character offsets.
std::vector<std::pair<size_t, size_t>> find_in_text(std::u32string_view hay, std::u32string_view needle,
                                                    bool case_sensitive);

// The document's searchable text, built once per layout (not per keystroke): one entry per group, with the text
// as searched (and case-folded for the smartcase / insensitive search), and where each character starts and ends
// as a selection position; plus every run's position and line box (to know where a match is on the page).
struct FindIndex {
    struct Group {
        std::u32string text, folded;
        std::vector<TextPos> starts, ends;
    };
    struct Row {
        TextPos pos;
        float min_y, max_y;
    };
    std::vector<Group> groups;
    std::vector<Row> rows;  // one per run, in reading order
};
FindIndex build_find_index(const std::vector<Run>& runs);
// The same as the `runs` overload below, on the prepared text.
std::vector<TextRange> find_matches(const FindIndex& index, std::string_view query, int case_mode = -1);

// All matches of `query` in `runs` (a full layout of the document, in
// reading order), as selection-style ranges: start = first character,
// end = one past the last character. `case_mode`: -1 = smartcase (the find bar),
// 0 = case-insensitive, 1 = case-sensitive (editor search, spec 17).
std::vector<TextRange> find_matches(const std::vector<Run>& runs, std::string_view query, int case_mode = -1);

// Simple case folding used by smartcase (ASCII, Latin-1, Latin Extended-A,
// Greek and Cyrillic): maps an uppercase letter to its lowercase form.
char32_t fold_case(char32_t c);

// UTF-8 -> UTF-32 (invalid bytes become U+FFFD).
std::u32string to_utf32(std::string_view s);
