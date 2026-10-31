// Vim-style marks (spec 28; marks.rs), without a window so they can be tested.
//
// A viewer has no cursor, so a mark remembers a scroll position: the top-level block at the top of
// the view and how far down into it (0..1), plus the first words of that block. When the file has
// changed since, the mark is looked up again by those words (`resolve`). Marks a-z belong to one
// file, A-Z work across files. They live in state.toml (`[[mark]]`, session.h).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "model.h"

namespace marks {

// How many characters of a block's text a mark keeps to find it again.
constexpr size_t kKeyChars = 40;
// At most this many marks are kept (local ones of all files together, and the global ones).
constexpr size_t kMaxMarks = 300;

// Can `c` name a mark? Does mark `c` work across files?
inline bool is_name(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool is_global(char32_t c) { return c >= 'A' && c <= 'Z'; }

// A block's first words, as a mark keeps them: its searchable text (what find searches, as
// find.rs's groups: spans, code lines, table cells, a callout's title; an equation counts as
// U+FFFC), runs of white space as one space, at most kKeyChars characters. UTF-8.
std::string block_key(const md::Block& b);

// Where a mark set on block `block` with key `text` is now, in a document whose blocks have these
// `keys`: the same block if its key is unchanged, else the block with that key nearest to it;
// nullopt if no block has it. A mark on a block without text is only found at its own index, on a
// block that still has no text.
std::optional<size_t> resolve(size_t block, const std::string& text, const std::vector<std::string>& keys);

// A position's mark letters as the margin shows them: in order, a-z then A-Z.
std::string margin_label(std::vector<char> names);

}  // namespace marks
