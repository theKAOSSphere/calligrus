// calligrus-pages-test: the page-break rules (spec 29), no window.
// Exit code 0 = all passed.
#include <cstdio>
#include <vector>

#include "pages.h"

namespace {

using pages::Block;
using pages::Kind;
using Ends = std::vector<float>;

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

// A block of n lines, each lh tall, starting at top: one group.
Block text(Kind kind, float top, int n, float lh) {
    Block b;
    b.kind = kind;
    b.top = top;
    b.height = lh * n;
    std::vector<float> group;
    for (int i = 1; i <= n; i++) group.push_back(top + lh * i);
    b.groups.push_back(group);
    return b;
}

Block atomic(float top, float height) {
    Block b;
    b.kind = Kind::Atomic;
    b.top = top;
    b.height = height;
    return b;
}

}  // namespace

int main() {
    {
        float w = 0, h = 0;
        check(pages::paper_size("A4", w, h) && w == 595.3f && h == 841.9f, "paper a4");
        check(pages::paper_size("letter", w, h) && w == 612 && h == 792, "paper letter");
        check(!pages::paper_size("a3", w, h), "unknown paper");
    }
    {
        const std::vector<Block> b = {text(Kind::Text, 0, 5, 20)};
        check(pages::page_ends(b, 200, 100) == Ends{100}, "fits one page");
        check(pages::page_ends(b, 99.8f, 100) == Ends{100}, "a hair over still fits (epsilon)");
    }
    check(pages::page_ends({text(Kind::Text, 0, 9, 20)}, 100, 180) == Ends({100, 180}), "paragraph splits at a line");
    {
        // para (4 lines), heading, para (5 lines): no break right after the heading
        const std::vector<Block> b = {text(Kind::Text, 0, 4, 20), text(Kind::Heading, 80, 1, 20), text(Kind::Text, 100, 5, 20)};
        check(pages::page_ends(b, 100, 200) == Ends({80, 160, 200}), "heading moves down");
    }
    {
        // widow: 5 lines of 25, the page would end after line 4, leaving 1: end after line 3
        check(pages::page_ends({text(Kind::Text, 0, 5, 25)}, 100, 125) == Ends({75, 125}), "widow");
        // orphan: the page would end after line 1 of the second paragraph: end before it
        check(pages::page_ends({text(Kind::Text, 0, 1, 70), text(Kind::Text, 80, 5, 20)}, 100, 180) == Ends({80, 180}), "orphan");
        // two items of a list: lines are grouped per item
        Block list;
        list.kind = Kind::Text;
        list.top = 0;
        list.height = 160;
        list.groups = {{20, 40, 60}, {80, 100, 120, 140, 160}};
        check(pages::page_ends({list}, 90, 160) == Ends({60, 120, 160}), "list items are grouped");
    }
    check(pages::page_ends({text(Kind::Text, 0, 2, 20), text(Kind::Code, 50, 4, 25)}, 110, 150) == Ends({100, 150}), "code splits between lines");
    {
        // 6 code lines of 25 from y 25: a page of 100 ends after line 3 (3 + 3), but a page of 150
        // would leave 1 line on the next page, so it ends after line 4
        std::vector<pages::Block> b = {text(Kind::Text, 0, 1, 25), text(Kind::Code, 25, 6, 25)};
        check(pages::page_ends(b, 100, 175) == Ends({100, 175}), "code: 3 + 3 lines");
        check(pages::page_ends(b, 150, 175) == Ends({125, 175}), "code: no lone last line");
    }
    check(pages::page_ends({text(Kind::Text, 0, 3, 20), text(Kind::Table, 70, 4, 20)}, 100, 150) == Ends({90, 150}), "table splits between rows");
    check(pages::page_ends({text(Kind::Text, 0, 2, 20), atomic(50, 80)}, 100, 130) == Ends({50, 130}), "tall picture moves to next page");
    check(pages::page_ends({atomic(0, 250)}, 100, 250) == Ends({100, 200, 250}), "picture taller than a page is cut");
    check(pages::page_ends({text(Kind::Text, 0, 1, 10), text(Kind::Heading, 10, 1, 20), atomic(30, 300)}, 100, 330) == Ends({30, 130, 230, 330}),
          "fallback ignores the heading rule");
    check(pages::page_ends({text(Kind::Code, 0, 10, 30)}, 100, 300) == Ends({90, 180, 240, 300}), "one huge code block (270 would leave 1 line)");
    check(pages::page_ends({text(Kind::Text, 0, 5, 18), text(Kind::Heading, 90, 1, 20)}, 100, 110) == Ends({90, 110}), "last block a heading");

    check(pages::export_body_size(16, 1.0) == 12.0f, "body size: 16 px = 12 pt");
    check(pages::export_body_size(13, 1.0) == 10.0f, "body size: 9.75 rounds to a half point");
    check(pages::export_body_size(16, 1.1) == 13.0f, "body size: 13.2 rounds to a half point");
    check(pages::export_body_size(20, 1.3) == 19.5f, "body size: 19.5");
    check(pages::export_body_size(12, 0.5) == 8.0f, "body size: at least 8");
    check(pages::export_body_size(28, 5.0) == 64.0f, "body size: at most 64");

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
