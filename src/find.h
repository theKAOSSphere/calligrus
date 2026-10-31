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
