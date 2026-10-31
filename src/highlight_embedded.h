// Data embedded into the exe at build time by cmake/highlight.cmake, from
// the Rust port (generated file: <build dir>/highlight_embedded.cpp).
#pragma once

#include <cstddef>

struct TSLanguage;

struct EmbeddedGrammar {
    const char* name;                    // as in languages.txt
    const TSLanguage* (*language)();     // tree_sitter_<name>
    const char* highlights;              // highlights.scm
    size_t highlights_len;
    const char* injections;              // injections.scm (length 0: none)
    size_t injections_len;
};

extern const EmbeddedGrammar kEmbeddedGrammars[];
extern const size_t kEmbeddedGrammarCount;
extern const char* const kEmbeddedThemeTxt;      // theme.txt
extern const size_t kEmbeddedThemeTxtLen;
extern const char* const kEmbeddedLanguagesTxt;  // languages.txt
extern const size_t kEmbeddedLanguagesTxtLen;
