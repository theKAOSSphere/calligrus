// Mermaid port: parser helpers for the indentation-sensitive diagrams (kanban, mindmap, block ...).
#include "kind_util.h"
#include "parser_internal.h"
#include "strutil.h"

namespace mermaid::pp {

namespace {
// parser.rs strip_trailing_comment_keep_indent: cut a "%%" comment outside quotes, keep leading blanks.
std::string strip_trailing_comment_keep_indent(const std::string& line) {
    char quote = 0;
    std::string out;
    for (size_t i = 0; i < line.size(); ++i) {
        char ch = line[i];
        if (quote) {
            if (ch == quote) quote = 0;
            out.push_back(ch);
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            out.push_back(ch);
            continue;
        }
        if (ch == '%' && i + 1 < line.size() && line[i + 1] == '%') break;
        out.push_back(ch);
    }
    return std::string(trim_end_view(out));
}
}  // namespace

std::vector<std::string> preprocess_input_keep_indent(const std::string& input) {
    std::vector<std::string> lines;
    bool in_frontmatter = false;
    for (const std::string& raw : lines_of(input)) {
        std::string t = trim(raw);
        if (t.empty()) continue;
        if (t == "---") {
            in_frontmatter = !in_frontmatter;
            continue;
        }
        if (in_frontmatter) continue;
        if (parse_init_directive(t)) continue;  // validated, then ignored: render_with_options ignores the config
        if (starts_with(t, "%%")) continue;
        std::string without = strip_trailing_comment_keep_indent(raw);
        if (trim_view(without).empty()) continue;
        lines.push_back(without);
    }
    return lines;
}

size_t count_indent(const std::string& line) {
    size_t count = 0;
    for (char ch : line) {
        if (ch == ' ') count += 1;
        else if (ch == '\t') count += 2;
        else break;
    }
    return count;
}

}  // namespace mermaid::pp
