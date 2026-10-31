// Syntax highlighting for code blocks, with tree-sitter (C library + grammars
// compiled into the exe, see cmake/highlight.cmake). Same grammars, queries,
// theme and algorithm as the Rust port, checked against the same
// golden files, so both viewers colour code identically.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A highlighted stretch of code. Its colour is looked up at draw time in the
// active theme: Theme::entry_style(entry) (theme.h), so switching themes
// doesn't re-run the highlighter.
struct CodeSpan {
    uint32_t start, end;  // byte range in the code text
    uint32_t entry;       // index into highlight_entry_names() (never a "plain" entry)
};

// Highlighted spans for `code` in `lang` (a fence info string; cached).
// Empty = unknown language (or nothing to colour): draw plain.
const std::vector<CodeSpan>& highlight_code(const std::string& code, const std::string& lang);

// The entry names of grammars/theme.txt, indexed like CodeSpan::entry
// (for Theme::bind_entries).
std::vector<std::string> highlight_entry_names();

// Compile the most common query in the background so the first code block doesn't stall.
void highlight_warm_up();

// --- for tests (calligrus-highlight-test) ---------------------------------------

// A highlighted stretch named by its theme entry (what the golden files hold).
struct NamedSpan {
    uint32_t start, end;
    std::string style;
};

// Like highlight_code, uncached and naming theme entries. Returns false where
// the Rust version returns None (unknown language, too big or too slow).
bool highlight_named(std::string_view code, std::string_view lang, std::vector<NamedSpan>& out);

// Golden-file text: one `start end style "text"` line per span (as in Rust).
std::string highlight_golden_text(std::string_view code, const std::vector<NamedSpan>& spans);

// Names of the compiled-in languages, and the query errors (empty = all fine).
std::vector<std::string> highlight_language_names();
std::vector<std::string> highlight_check_all_languages();

// Bigger code is drawn plain (the same limit as the Rust port).
constexpr size_t kHighlightMaxBytes = 256 * 1024;
