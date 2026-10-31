// calligrus-dump: console tool that parses a Markdown file and prints the Block/Span
// tree. Handy for checking the parser without opening a window:
//   calligrus-dump.exe file.md
#include <cstdio>
#include <string>

#include "model.h"
#include "org.h"
#include "platform.h"

static void print_spans(const md::Spans& spans) {
    for (const auto& s : spans) {
        std::string flags;
        if (s.bold) flags += 'B';
        if (s.italic) flags += 'I';
        if (s.strike) flags += 'S';
        if (s.code) flags += 'C';
        if (s.math) flags += 'M';
        if (!s.link.empty()) flags += 'L';
        std::printf("[%s]\"%s\" ", flags.c_str(), s.text.c_str());
    }
    std::printf("\n");
}

static void print_blocks(const std::vector<md::Block>& blocks, int depth) {
    using K = md::Block::Kind;
    std::string ind(depth * 2, ' ');
    for (const auto& b : blocks) {
        switch (b.kind) {
            case K::Heading: std::printf("%sH%d ", ind.c_str(), b.level); print_spans(b.spans); break;
            case K::Paragraph: std::printf("%sP ", ind.c_str()); print_spans(b.spans); break;
            case K::Code: std::printf("%sCODE(%s) %zu bytes\n", ind.c_str(), b.lang.c_str(), b.text.size()); break;
            case K::Rule: std::printf("%sRULE\n", ind.c_str()); break;
            case K::Quote: std::printf("%sQUOTE\n", ind.c_str()); print_blocks(b.children, depth + 1); break;
            case K::List:
                std::printf("%sLIST %s\n", ind.c_str(), b.ordered ? "ordered" : "bullet");
                for (const auto& item : b.items) {
                    std::printf("%s - item\n", ind.c_str());
                    print_blocks(item, depth + 2);
                }
                break;
            case K::Table:
                std::printf("%sTABLE %zu cols, %zu rows\n", ind.c_str(), b.header.size(), b.rows.size());
                break;
            case K::Math: std::printf("%sMATH %s\n", ind.c_str(), b.text.c_str()); break;
            case K::Images:
                std::printf("%sIMAGES", ind.c_str());
                for (const auto& im : b.images)
                    std::printf(" [%s|alt=\"%s\"|w=%g%s%s]", im.src.c_str(), im.alt.c_str(), im.width,
                                im.link.empty() ? "" : "|link=", im.link.c_str());
                std::printf("\n");
                break;
        }
    }
}

int main() {
    auto a = platform::args();
    if (a.empty()) {
        std::fprintf(stderr, "usage: calligrus-dump file.md\n");
        return 2;
    }
    auto text = platform::read_file(a[0]);
    if (!text) {
        std::fprintf(stderr, "cannot read file\n");
        return 1;
    }
    print_blocks(md::parse_format(*text, md::is_org_path(a[0].string()) ? "org" : "markdown"), 0);
    return 0;
}
