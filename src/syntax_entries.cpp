#include "syntax_entries.h"

std::vector<SyntaxEntry> parse_syntax_entries(std::string_view text) {
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    std::vector<SyntaxEntry> entries;
    while (!text.empty()) {
        size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);

        // First two whitespace-separated words: the name and the first colour.
        std::vector<std::string_view> words;
        for (size_t i = 0; i < line.size() && words.size() < 2;) {
            while (i < line.size() && space(line[i])) i++;
            size_t start = i;
            while (i < line.size() && !space(line[i])) i++;
            if (i > start) words.push_back(line.substr(start, i - start));
        }
        if (words.empty() || words[0].starts_with('#')) continue;
        entries.push_back({std::string(words[0]), words.size() > 1 && words[1] == "-"});
    }
    return entries;
}

std::optional<size_t> find_syntax_entry(const std::vector<SyntaxEntry>& entries, std::string_view capture) {
    if (capture.empty() || capture[0] == '_') return std::nullopt;
    std::string_view name = capture;
    for (;;) {
        for (size_t i = 0; i < entries.size(); i++)
            if (entries[i].name == name) return i;
        size_t dot = name.rfind('.');
        if (dot == std::string_view::npos) return std::nullopt;
        name = name.substr(0, dot);
    }
}
