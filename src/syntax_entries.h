// The entry names of grammars/theme.txt (shared with the Rust port): which
// highlight capture names exist and which are "plain".
//
// Highlighting resolves a capture to an *entry* here (stage 1); the colour of
// an entry comes from the active theme's [syntax] table at draw time
// (stage 2, theme.h). theme.txt's colour columns are ignored: only the name
// and whether the first column is `-` (plain: the capture claims its text
// but draws it uncoloured) matter.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct SyntaxEntry {
    std::string name;
    bool plain = false;
};

// Parse theme.txt: one `name [colour|-] ...` line per entry, `#` comments.
std::vector<SyntaxEntry> parse_syntax_entries(std::string_view theme_txt);

// The entry for a capture name: the name itself, else its longest listed
// dotted prefix (`function.method.call` -> `function.method` -> `function`).
// Names starting with `_` are helpers for predicates: none. (Plain entries
// are returned too; callers check `.plain`.)
std::optional<size_t> find_syntax_entry(const std::vector<SyntaxEntry>& entries, std::string_view capture);
