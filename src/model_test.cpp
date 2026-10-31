// calligrus-model-test: front matter, footnotes and callouts (no window).
// Exit code 0 = all passed.
#include <cstdio>
#include <string>
#include <vector>

#include "model.h"
#include "org.h"
#include "imagefilter.h"
#include "slides.h"
#include "marks.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

void check_eq(const std::string& got, const std::string& want, const char* what) {
    check(got == want, what);
    if (got != want) std::printf("     got:  \"%s\"\n     want: \"%s\"\n", got.c_str(), want.c_str());
}

}  // namespace

int main() {
    {
        // Marks (spec 28; marks.rs): the same block keys as the Rust app (both read one state.toml).
        const auto doc = md::parse(
            "# Title *one*\n\nPara with `code  x` and $x^2$ math.\n\n> [!NOTE]\n> Be kind.\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n"
            "```mermaid\ngraph TD\n```\n\n- item one\n- item two\n\n---\n");
        std::string keys;
        for (const md::Block& b : doc) keys += marks::block_key(b) + "|";
        check_eq(keys, "Title one|Para with code x and \xEF\xBF\xBC math.|Note Be kind.|a b 1 2||item one item two||", "marks: block keys as marks.rs");
        check(marks::block_key(md::parse(std::string(60, 'x'))[0]).size() == marks::kKeyChars, "marks: key cut to 40 characters");
        const std::vector<std::string> ks = {"Intro", "", "Setup", "Usage", "Setup"};
        check(marks::resolve(2, "Setup", ks) == 2u && marks::resolve(3, "Setup", ks) == 2u && marks::resolve(10, "Setup", ks) == 4u, "marks: resolve moved");
        check(!marks::resolve(0, "Gone", ks) && marks::resolve(1, "", ks) == 1u && !marks::resolve(2, "", ks), "marks: resolve lost / no text");
        check(marks::margin_label({'b', 'A', 'a', 'a'}) == "abA", "marks: margin label");
    }
    // Front matter.
    check_eq(md::strip_front_matter("---\ntitle: x\n---\n# Hi\n"), "# Hi\n", "front matter removed");
    check_eq(md::strip_front_matter("\xEF\xBB\xBF---\na: 1\n...\nbody"), "body", "BOM + `...` closer");
    check_eq(md::strip_front_matter("---\nno closer\n# Hi\n"), "---\nno closer\n# Hi\n", "no closing line: kept");
    check_eq(md::strip_front_matter("# Hi\n---\nx\n---\n"), "# Hi\n---\nx\n---\n", "not on first line: kept");

    // Callout markers.
    check_eq(md::callout_kind("[!NOTE]"), "note", "callout NOTE");
    check_eq(md::callout_kind("[!Warning]  "), "warning", "callout case + trailing spaces");
    check_eq(md::callout_kind("[!FOO]"), "", "unknown callout");
    check_eq(md::callout_kind("[!TIP] text"), "", "text after marker");
    {
        auto doc = md::parse("> [!TIP]\n> Be kind.\n\n> [!FOO]\n> plain\n\n> [!NOTE]\n>\n> second para\n");
        check(doc.size() == 3, "three quotes");
        check(doc.size() == 3 && doc[0].callout == "tip", "quote 1 is a tip callout");
        check(doc.size() == 3 && doc[0].children.size() == 1 && doc[0].children[0].spans[0].text == "Be kind.", "marker line removed");
        check(doc.size() == 3 && doc[1].callout.empty(), "[!FOO] stays a blockquote");
        check(doc.size() == 3 && doc[2].callout == "note" && doc[2].children.size() == 1, "marker-only paragraph dropped");
    }
    {
        auto doc = md::parse("> [!CAUTION]\n> | a | b |\n> |---|---|\n> | 1 | 2 |\n");
        check(doc.size() == 1 && doc[0].callout == "caution" && doc[0].children.size() == 1 &&
                  doc[0].children[0].kind == md::Block::Kind::Table,
              "table right after the marker is a table");
        auto lazy = md::parse("> text\n> [!NOTE]\n");
        check(lazy.size() == 1 && lazy[0].callout.empty(), "marker inside a paragraph: not a callout");
        auto code = md::parse("```\n> [!NOTE]\n> x\n```\n");
        check(code.size() == 1 && code[0].text == "> [!NOTE]\n> x\n", "marker in a code block untouched");
    }

    // Footnotes.
    {
        std::vector<std::string> notes;
        std::string out = md::extract_footnotes("A[^b] and B[^a] and A[^b] and [^zz] `[^a]`\n\n[^a]: first\n[^b]: second\n    more\n", notes);
        check(notes.size() == 2 && notes[0] == "second\nmore" && notes[1] == "first", "notes numbered by first reference");
        check_eq(out.substr(0, out.find('\n')),
                 "A[\\[1\\]](#fn-1) and B[\\[2\\]](#fn-2) and A[\\[1\\]](#fn-1) and [^zz] `[^a]`", "references replaced");
        auto doc = md::parse("Text[^1].\n\n```\n[^1]: code\n```\n\n[^1]: The note.\n");
        bool ok = doc.size() >= 4 && doc[0].spans.size() >= 2;
        check(ok, "footnote section appended (para, code, rule, note)");
        if (ok) {
            check(doc[0].spans[1].link == "#fn-1" && doc[0].spans[1].small, "reference is a small link to #fn-1");
            check(doc.back().footnote == 1 && doc.back().spans[0].text == "1. ", "footnote paragraph numbered");
            check(doc.back().spans.back().link == "#fnref-1" && doc.back().spans.back().text == " ↩", "footnote ends with a back-link to #fnref-1");
            check(doc[doc.size() - 2].kind == md::Block::Kind::Rule, "rule before footnotes");
        }
    }
    {
        // GFM lazy continuation: an unindented text line right after the definition belongs to it.
        std::vector<std::string> notes;
        md::extract_footnotes("x[^a]\n\n[^a]: one\ntwo\n\nthree\n", notes);
        check(notes.size() == 1 && notes[0] == "one\ntwo", "footnote: lazy continuation line joins");
        md::extract_footnotes("x[^a]\n\n[^a]: one\n# Heading\n", notes);
        check(notes.size() == 1 && notes[0] == "one", "footnote: a heading ends it");
    }
    // Source lines of top-level blocks (editor scroll sync).
    {
        auto lines = [](const md::Document& d) {
            std::string r;
            for (const md::Block& b : d) r += std::to_string(b.line_first) + "-" + std::to_string(b.line_last) + " ";
            return r;
        };
        check_eq(lines(md::parse("# Title\n\npara one\ncontinues\n\n```rust\nfn main() {}\n\nlet x;\n```\n\n---\n\n- a\n- b\n\n> q\n")),
                 "1-1 3-4 6-10 12-12 14-15 17-17 ", "lines: heading, paragraph, fence with blank line, rule, list, quote");
        {  // <style> / <script> blocks are not drawn and belong to no block (spec 31)
            const std::string src = "one\n\n<style>\n.a { color: red; }\n\n.b { x: y; }\n</style>\n\ntwo\n\n<script>\nlet a = 1;\n</script>\n\n"
                                    "three\n\nfour\n";
            const auto doc = md::parse(src);
            std::string texts;
            for (const md::Block& b : doc) texts += (b.spans.empty() ? "?" : b.spans[0].text) + " ";
            check_eq(texts, "one two three four ", "style/script: only the paragraphs remain");
            check_eq(lines(doc), "1-1 9-9 15-15 17-17 ", "style/script: line ranges of the paragraphs");
            check_eq(lines(md::parse("one\n<style>\nx\n</style>\ntwo\n")), "1-1 5-5 ", "style/script: block right after a paragraph");
            check(md::parse("one\n\n<STYLE type=\"text/css\">\np{}\n</STYLE>\n\ntwo\n").size() == 2, "style: upper case tag names");
            // Upper case closing tag, one-liners: the lines after the closing tag are Markdown again (CommonMark)
            auto texts_of = [](const md::Document& d) {
                std::string t;
                for (const md::Block& b : d) t += (b.spans.empty() ? "?" : b.spans[0].text) + "|";
                return t;
            };
            auto d1 = md::parse("one\n\n<STYLE type=\"text/css\">\np{}\n</STYLE>\ntwo\n\nthree\n");
            check_eq(texts_of(d1), "one|two|three|", "style: upper case closing tag, text after it");
            check_eq(lines(d1), "1-1 6-6 8-8 ", "style: upper case closing tag, lines");
            auto d2 = md::parse("<style>p{color:red}</style>\nafter\nmore\n\nlast\n");
            check_eq(texts_of(d2), "after|last|", "style: one-liner, paragraph on the next line");
            check_eq(lines(d2), "2-3 5-5 ", "style: one-liner, lines");
            auto d3 = md::parse("a\n\n<script>x()</script>\nb\n<script>\ny\n</SCRIPT>\nc\n");
            check_eq(texts_of(d3), "a|b|c|", "script: one-liner and upper case");
            check_eq(lines(d3), "1-1 4-4 8-8 ", "script: lines");
            check(md::parse("```\n<style>x</style>\nkept\n```\n").size() == 1, "style: inside a code fence it is code");
            const auto inl = md::parse("a <style>b</style> c");
            check(inl.size() == 1 && inl[0].spans.size() >= 1, "style: inline one is left alone");
        }
        check_eq(lines(md::parse("```\nbare\n```\n```\nnext\n```\n")), "1-3 4-6 ", "lines: adjacent fences without info string");
        check_eq(lines(md::parse("---\ntitle: x\n---\n# H\n\ntext\n")), "4-4 6-6 ", "lines: front matter lines added back");
        check_eq(lines(md::parse("| a | b |\n|---|---|\n| 1 | 2 |\n\nafter\n")), "1-3 5-5 ", "lines: table");
        check_eq(lines(md::parse("A[^n]\n\n[^n]: note\ncont\n\nB\n")).substr(0, 8), "1-1 6-6 ", "lines: footnote definition lines skipped");
        check_eq(lines(md::parse("> [!NOTE]\n> hi\n\nafter\n\n> [!TIP]\n> yo\n\nlast\n")), "1-2 4-4 6-7 9-9 ", "lines: callouts (marker split inserts a line)");
        check_eq(lines(md::parse("text\n\n$$\nx^2\n$$\n\nmore\n")), "1-1 3-5 7-7 ", "lines: display math starts at its $$ line");
    }
    // One level down (spec 18): list items, quote / callout children, a callout's title line.
    {
        auto items = [](const md::Block& l) {
            std::string r;
            for (const auto& it : l.item_lines) r += std::to_string(it.first) + "-" + std::to_string(it.last) + " ";
            return r;
        };
        auto kids = [](const md::Block& q) {
            std::string r;
            for (const md::Block& c : q.children) r += std::to_string(c.line_first) + "-" + std::to_string(c.line_last) + " ";
            return r;
        };
        // Items end at their last non-blank line (like the Rust version); blank lines map to the previous item.
        auto doc = md::parse("- one\n- two\n  more\n- three\n\npara\n");
        check_eq(items(doc[0]), "1-1 2-3 4-4 ", "item lines: tight list, items end at their last non-blank line");
        doc = md::parse("- a\n\n- b\n\n  b2\n\n- c\n");
        check_eq(items(doc[0]), "1-1 3-5 7-7 ", "item lines: loose list, multi-block item");
        doc = md::parse("1. ```\n   code\n   ```\n2. x\n");
        check_eq(items(doc[0]), "1-3 4-4 ", "item lines: item opening with a fence (no info string)");
        doc = md::parse("> first\n>\n> second\n> still\n>\n> ```\n> code\n> ```\n");
        check_eq(kids(doc[0]), "1-1 3-4 6-8 ", "child lines: quote paragraphs and a fence");
        doc = md::parse("---\nt: 1\n---\n> [!NOTE]\n> one\n>\n> two\n");
        check(doc[0].callout == "note" && doc[0].title_line == 4, "callout: title line = the marker line (front matter added)");
        check_eq(kids(doc[0]), "5-5 7-7 ", "child lines: callout children (marker paragraph dropped)");
        doc = md::parse("- a\n  - nested\n- b\n");
        check_eq(items(doc[0]), "1-2 3-3 ", "item lines: a nested list belongs to its item");
    }
    // Org (spec 25).
    {
        using K = md::Block::Kind;
        auto text = [](const md::Block& b) {
            std::string r;
            for (const md::Span& s : b.spans) r += s.text;
            return r;
        };
        check(md::is_org_path("a/b.ORG") && !md::is_org_path("a.md"), "org: extension check");
        auto doc = md::parse_org("#+TITLE: T\n#+AUTHOR: x\n# comment\n* TODO Head :tag:\n  SCHEDULED: <2026-10-09>\n  :PROPERTIES:\n  :ID: 1\n  :END:\nText *b* /i/ ~c~\ncont\n");
        check(doc.size() == 3 && doc[0].kind == K::Heading && doc[0].level == 1 && doc[0].line_first == 1, "org: #+TITLE is a level-1 heading");
        check_eq(text(doc[1]), "TODO Head", "org: headline: tags dropped, TODO kept");
        check(doc[1].spans[0].bold && doc[1].line_first == 4, "org: TODO keyword is bold");
        check_eq(text(doc[2]), "Text b i c cont", "org: paragraph joined, markers dropped");
        check(doc[2].line_first == 9 && doc[2].line_last == 10, "org: paragraph lines");
        check(doc[2].spans[1].bold && doc[2].spans[3].italic && doc[2].spans[5].code, "org: inline styles");
        doc = md::parse_org("- a\n  - b\n- [X] c\n1. one\n");
        check(doc.size() == 2 && doc[0].items.size() == 2 && doc[0].items[0].size() == 2 && doc[1].ordered, "org: lists nest by indentation");
        check(doc[0].item_lines[0].last == 2 && doc[0].item_lines[1].first == 3, "org: item lines");
        check_eq(text(doc[0].items[1][0]), "\xE2\x98\x91 c", "org: checkbox");
        doc = md::parse_org("#+BEGIN_SRC rust\nfn x() {}\n#+END_SRC\n#+begin_quote\nq\n#+end_quote\n#+BEGIN_NOTE\nn\n#+END_NOTE\n");
        check(doc.size() == 3 && doc[0].kind == K::Code && doc[0].lang == "rust" && doc[0].text == "fn x() {}" && doc[0].line_last == 3, "org: src block");
        check(doc[1].kind == K::Quote && doc[1].callout.empty() && doc[2].callout == "note" && doc[2].title_line == 7, "org: quote and callout");
        doc = md::parse_org("|---+---|\n| a | b |\n|---+---|\n| 1 | 2 |\n");
        check(doc[0].kind == K::Table && doc[0].header.empty() && doc[0].rows.size() == 2, "org: rules at the top: no header");
        doc = md::parse_org("| a | b |\n|---+---|\n| 1 | 2 |\n| <r> | |\n");
        check(doc[0].header.size() == 2 && doc[0].rows.size() == 1 && doc[0].aligns[0] == md::Align::Right, "org: header, cookie row hidden");
        doc = md::parse_org("See [[#x][there]] and [[*Some Head]] and [[img.png]]\n\n\\[ x^2 \\]\n\nRef[fn:a]\n\n[fn:a] The note.\n");
        check(doc[0].spans[1].link == "#x" && doc[0].spans[3].link == "#Some Head" && doc[0].spans.back().image_src == "img.png", "org: links and image");
        check(doc[1].kind == K::Math && doc[1].text == "x^2" && doc[1].line_first == 3, "org: display math");
        check(doc.size() == 5 && doc[4].footnote == 1 && doc[2].spans[1].link == "#fn-1", "org: footnote section");
        check(md::parse_org("-----\n").front().kind == K::Rule, "org: rule");
    }
    {
        // Pauses (spec 26), the same case as the Rust test: blocks 0 h1, 1 list, 2 b, 3 c,
        // 4 code, 5 rule, 6 d; the pause in the code block and the one opening slide 2 don't count.
        const std::string src = "# T\n\n- a\n\n<!-- pause -->\n\nb\n\n<!--pause-->\nc\n\n```\n<!-- pause -->\n```\n\n---\n\n<!-- pause -->\n\nd\n";
        const md::Document doc = md::parse(src);
        const auto pauses = slides::pause_lines(src, false);
        check(pauses == std::vector<int>{5, 9, 18}, "pause lines (not the one in the code block)");
        const auto st = slides::steps(slides::split(doc), doc, pauses, {});
        check(st.size() == 2 && st[0] == std::vector<slides::Step>{{2, 0}, {3, 0}} && st[1].empty(), "pause steps");
        check(slides::step_of_block(st[0], 1) == 0 && slides::step_of_block(st[0], 3) == 2, "step of block");
        check(slides::pause_lines("text\n# pause\n#pause\n#+pause\n#+BEGIN_SRC sh\n# pause\n#+END_SRC\n", true) == std::vector<int>{2}, "org pause lines");
        check(slides::pause_lines("- a\n    <!-- pause -->\n````\n```\n<!-- pause -->\n````\n<!-- pause -->\n", false) == std::vector<int>{7},
              "pauses: not indented, not in fences");
        // pandoc's `. . .`: only as a paragraph of its own, and then not shown
        const std::string dots = "a\n\n. . .\n\nb\n. . .\n\n```\n. . .\n```\n\n. . .\n";
        check(slides::pause_lines(dots, false) == std::vector<int>{3, 12}, "dots pause lines");
        const md::Document dd = md::parse(dots);
        check(dd.size() == 3 && dd[1].line_first == 5, "dots pauses are not shown");
    }
    {
        // Text directives (spec 26), the same case as the Rust test: blocks 0 h1, 1 para a,
        // 2 para b, 3 rule, 4 h2, 5 para c.
        const std::string src = "# T\n\n<!-- new_lines: 2 -->\n<!-- newline -->\n<!-- alignment: center -->\n<!-- font_size: 2 -->\n\na\n\nb\n\n---\n\n"
                                "<!-- alignment : right -->\n## U\n\nc\n<!-- new_lines: 0 -->\n<!-- alignment: middle -->\n<!-- font_size: 8 -->\n";
        const md::Document doc = md::parse(src);
        const auto d = slides::text_lines(src, false);
        check(d.size() == 5 && d[0].line == 3 && d[0].n == 2 && d[1].n == 1 && d[2].align == slides::TextAlign::Center && d[3].n == 2 &&
                  d[4].line == 14 && d[4].align == slides::TextAlign::Right,
              "text directive lines");
        const auto st = slides::text_styles(slides::split(doc), doc, d);
        const slides::TextStyle plain, big{0, slides::TextAlign::Center, 2};
        check(st[0] == plain && st[1] == slides::TextStyle{3, slides::TextAlign::Center, 2} && st[2] == big, "text styles last to the slide's end");
        check(st[4] == slides::TextStyle{0, slides::TextAlign::Right, 1} && st[5] == st[4], "a new slide starts plain");
        const auto o = slides::text_lines("# newline\n# font_size: 3\n", true);
        check(o.size() == 2 && o[0].n == 1 && o[1].kind == slides::TextDirective::Kind::FontSize && o[1].n == 3, "org text directives");
    }
    {
        // Speaker notes (spec 26), the same case as the Rust test.
        const std::string src = "# A\n\ntext\n\n<!-- speaker_note: say hi -->\n\n```\n<!-- speaker_note: no -->\n```\n\n---\n\n"
                                "<!-- speaker_note: |\n  first line\n  second line\n-->\n\n# B\n\n<!--speaker_note:more-->\n";
        const auto notes = slides::note_lines(src, false);
        check(notes == std::vector<std::pair<int, std::string>>{{5, "say hi"}, {13, "first line\nsecond line"}, {20, "more"}}, "speaker note lines");
        const md::Document doc = md::parse(src);
        check(slides::slide_notes(slides::split(doc), doc, notes) == std::vector<std::string>{"say hi", "first line\nsecond line\n\nmore"},
              "speaker notes per slide");
        const std::string org = "* A\n#+begin_notes\n  Hello\n  there\n#+end_notes\n#+begin_src sh\n#+begin_notes\n#+end_src\n";
        check(slides::note_lines(org, true) == std::vector<std::pair<int, std::string>>{{2, "Hello\nthere"}}, "org notes lines");
        check(md::parse_format(org, "org").size() == 2, "org hides the notes block");
    }
    {
        // end_slide and incremental lists (spec 26), the same case as the Rust test:
        // blocks 0 h1, 1 rule, 2 list(3), 3 list(2), 4 para, 5 list(2).
        const std::string src = "# T\n\n---\n\n<!-- end_slide -->\n\n<!-- incremental_lists: true -->\n\n- a\n- b\n- c\n\n"
                                "<!-- incremental_lists: false -->\n\n1. x\n2. y\n\ntext\n\n<!-- end_slide -->\n\n- p\n- q\n";
        const md::Document doc = md::parse(src);
        const auto ends = slides::end_slide_lines(src, false);
        check(ends == std::vector<int>{5, 20}, "end_slide lines");
        const auto ranges = slides::split_with_ends(doc, ends);
        check(ranges.size() == 3 && ranges[0].first == 0 && ranges[0].last == 1 && ranges[1].first == 2 && ranges[1].last == 4 &&
                  ranges[2].first == 5 && ranges[2].last == 5,
              "end_slide splits (the rule stays on slide 1)");
        const auto inc = slides::incremental_lines(src, false);
        check(inc == std::vector<std::pair<int, bool>>{{7, true}, {13, false}}, "incremental_lists lines");
        const auto st = slides::steps(ranges, doc, {}, inc);
        check(st.size() == 3 && st[0].empty() && st[1] == std::vector<slides::Step>{{2, 1}, {2, 2}} && st[2].empty(),
              "incremental list steps");
        check(slides::split_with_ends(doc, {}).size() == slides::split(doc).size(), "no end_slide: split as before");
        check(slides::incremental_lines("# incremental_lists: true\n# incremental_lists: maybe\n", true) ==
                  std::vector<std::pair<int, bool>>{{1, true}},
              "org incremental_lists");
    }
    {
        // Columns (spec 26). Blocks: 0 h1, 1 a (full width: before the first `column:`), 2 b (col 0),
        // 3 c (col 1), 4 d (col 7 is out of range: still col 1), 5 e (reset: full width),
        // 6 f (bad weights: ignored, so `column: 0` has no layout), 7 rule, 8 g (slide 2: the
        // directives just above the rule apply to it).
        const std::string src =
            "# T\n\n<!-- column_layout: [2,1] -->\n\na\n\n<!-- column: 0 -->\n\nb\n\n<!-- column: 1 -->\n\nc\n\n"
            "<!-- column: 7 -->\n\nd\n\n<!-- reset_layout -->\n\ne\n\n<!-- column_layout: [1, 0] -->\n<!-- column: 0 -->\n\nf\n\n"
            "<!-- column_layout: [1, 1, 1] -->\n<!-- column: 2 -->\n\n---\n\ng\n\n```\n<!-- column: 0 -->\n```\n";
        const md::Document doc = md::parse(src);
        const auto dirs = slides::layout_lines(src, false);
        check(dirs.size() == 8 && dirs[0].kind == slides::Directive::Kind::Layout && dirs[0].weights == std::vector<int>{2, 1} &&
                  dirs[0].line == 3 && dirs[1].kind == slides::Directive::Kind::Column && dirs[3].column == 7 &&
                  dirs[4].kind == slides::Directive::Kind::Reset && dirs[6].weights.size() == 3,
              "column directives (bad weights and the one in the code block left out)");
        const auto p = slides::placements(slides::split(doc), doc, dirs);
        check(p.size() == doc.size() && doc.size() == 10, "placements: one per block");
        if (p.size() == 10) {
            check(p[0].row < 0 && p[1].row < 0, "columns: full width before the first column:");
            check(p[2].row == 0 && p[2].column == 0 && p[2].weights == std::vector<int>{2, 1} && p[3].row == 0 && p[3].column == 1,
                  "columns: two columns");
            check(p[4].row == 0 && p[4].column == 1, "columns: out of range ignored");
            check(p[5].row < 0 && p[6].row < 0, "columns: reset_layout, bad weights");
            check(p[8].row >= 0 && p[8].column == 2 && p[8].weights.size() == 3, "columns: directives above --- open the next slide's layout");
        }
        check(slides::placements(slides::split(doc), doc, slides::layout_lines("text\n", false)).empty(), "no columns: empty");
        {
            // Directives just above a `---` belong to the next slide (blocks: 0 a, 1 rule, 2 b), as in Rust.
            const std::string s2 = "a\n\n<!-- column_layout: [1, 1] -->\n<!-- column: 1 -->\n\n---\n\nb\n";
            const md::Document d = md::parse(s2);
            const auto pp = slides::placements(slides::split(d), d, slides::layout_lines(s2, false));            check(pp.size() == 3 && pp[0].row < 0 && pp[1].row < 0 && pp[2].row >= 0 && pp[2].column == 1, "columns: directives above --- go to the next slide");
        }
        const std::string org = "* T\n# column_layout: [1, 2]\n# column: 1\nx\n\n# column: 0\ny\n# reset_layout\nz\n#+begin_src sh\n# column: 1\n#+end_src\n";
        const auto od = slides::layout_lines(org, true);
        check(od.size() == 4 && od[0].weights == std::vector<int>{1, 2} && od[1].column == 1 && od[2].line == 6 &&
                  od[3].kind == slides::Directive::Kind::Reset,
              "org column directives");
        const md::Document odoc = md::parse_org(org);
        const auto op = slides::placements(slides::split(odoc), odoc, od);
        check(op.size() == odoc.size() && odoc.size() >= 4 && op[1].column == 1 && op[2].column == 0 && op[2].row == op[1].row &&
                  op[3].row < 0,
              "org placements");
    }
    {
        // Marp backgrounds (spec 26), as in slides.rs's `backgrounds` test.
        auto bg = [](const char* alt) { return slides::background_of(alt, "a.png"); };
        check(bg("bg") && bg("bg")->side == slides::BgSide::Full && !bg("bg")->contain, "bg: full");
        check(bg("bg right")->side == slides::BgSide::Right && bg("bg right")->frac == 0.5f, "bg right");
        check(bg("bg left:40%")->side == slides::BgSide::Left && std::abs(bg("bg left:40%")->frac - 0.4f) < 1e-6f, "bg left:40%");
        check(std::abs(bg("bg left:5%")->frac - 0.1f) < 1e-6f, "bg: clamped");
        check(bg("bg contain right:30%")->contain, "bg contain");
        check(!bg("bg right:30") && !bg("bg drop-shadow") && !bg("background") && !bg("a bg"), "bg: not backgrounds");
        check(bg("bg sepia blur:4px")->filters ==
                  std::vector<imagefilter::Filter>{{imagefilter::Kind::Sepia, 1.0f}, {imagefilter::Kind::Blur, 4.0f}},
              "bg filters");
        check(bg("bg vertical")->vertical, "bg vertical");
        const std::string src = "# A\n\n![bg right:40%](x.png)\n\ntext\n\n---\n\n![bg](y.png) ![photo](z.png)\n\nmore\n";
        const md::Document d = md::parse(src);
        check(d.size() == 6 && slides::block_backgrounds(d[1]).size() == 1 && slides::block_backgrounds(d[4]).empty(), "bg blocks");
        const auto ranges = slides::split(d);
        const auto bgs = slides::slide_backgrounds(ranges, d);
        check(bgs.size() == 2 && slides::backgrounds_strip(bgs[0]) && slides::backgrounds_strip(bgs[0])->side == slides::BgSide::Right &&
                  bgs[1].empty(),
              "bg per slide");
        const auto split = slides::background_split(bgs[0][0]);
        check(split && split->weights == std::vector<int>{60, 40} && split->text_column == 0, "bg split");
        std::vector<slides::Placement> places;
        slides::place_backgrounds(places, ranges, bgs, d.size());
        check(places.size() == 6 && places[2].row == 1000000 && places[2].column == 0 && places[5].row < 0, "bg placements");
        // several pictures: all of them, side by side or stacked; the first side named wins
        const md::Document d2 = md::parse("![bg](a.png) ![bg left:30%](b.png)\n\n![bg](c.png)\n");
        auto many = slides::slide_backgrounds(slides::split(d2), d2)[0];
        check(many.size() == 3 && slides::backgrounds_strip(many) == &many[1], "bg: several, first side wins");
        const auto cells = slides::background_cells(many, {0, 0, 300, 100});
        check(cells.size() == 3 && cells[1] == std::array<float, 4>{100, 0, 100, 100}, "bg cells side by side");
        many[2].vertical = true;
        check(slides::background_cells(many, {0, 0, 100, 300})[1] == std::array<float, 4>{0, 100, 100, 100}, "bg cells stacked");
    }
    {
        // Marp image filters (spec 26), as in imagefilter.rs's tests.
        using imagefilter::Filter, imagefilter::Kind, imagefilter::parse_filter;
        check(parse_filter("sepia") == Filter{Kind::Sepia, 1.0f} && parse_filter("sepia:50%") == Filter{Kind::Sepia, 0.5f} &&
                  parse_filter("grayscale:3") == Filter{Kind::Grayscale, 1.0f} && parse_filter("brightness:1.2") == Filter{Kind::Brightness, 1.2f} &&
                  parse_filter("blur") == Filter{Kind::Blur, 10.0f} && parse_filter("blur:4px") == Filter{Kind::Blur, 4.0f} &&
                  parse_filter("hue-rotate:-90deg") == Filter{Kind::HueRotate, -90.0f} && parse_filter("opacity") == Filter{Kind::Opacity, 0.5f},
              "filters parse");
        check(!parse_filter("contrast:-1") && !parse_filter("blur:big") && !parse_filter("drop-shadow") && !parse_filter("photo"),
              "filters: not filters");
        auto px = [](std::vector<Filter> f) {
            std::vector<unsigned char> p{200, 100, 50, 255};
            imagefilter::apply(p, 1, 1, f);
            return p;
        };
        using V = std::vector<unsigned char>;
        check(px({{Kind::Grayscale, 1}}) == V{118, 118, 118, 255}, "filter grayscale");
        check(px({{Kind::Sepia, 1}}) == V{165, 147, 114, 255}, "filter sepia");
        check(px({{Kind::Invert, 1}}) == V{55, 155, 205, 255}, "filter invert");
        check(px({{Kind::Opacity, 0.5f}}) == V{200, 100, 50, 128}, "filter opacity");
        check(px({{Kind::Brightness, 1.5f}}) == V{255, 150, 75, 255}, "filter brightness");
        check(px({{Kind::Contrast, 2}}) == V{255, 73, 0, 255}, "filter contrast");
        check(px({{Kind::HueRotate, 180}}) == V{35, 135, 185, 255}, "filter hue-rotate");
        check(px({{Kind::Saturate, 0}, {Kind::Invert, 1}}) == V{137, 137, 137, 255}, "filter saturate then invert");
        check(imagefilter::blur_boxes(10.0f) == std::array<int, 3>{19, 19, 21}, "blur boxes");
        const int w = 64, h = 64;
        V img(size_t(w) * h * 4, 0);
        for (int y = 28; y <= 36; y++)
            for (int x = 28; x <= 36; x++) img[(size_t(y) * w + x) * 4] = 255;
        imagefilter::apply(img, w, h, {{Kind::Blur, 2.0f * imagefilter::kBlurSlideWidth / w}});
        auto red = [&](int x, int y) { return int(img[(size_t(y) * w + x) * 4]); };
        check(red(24, 32) == 6 && red(26, 32) == 57 && red(28, 32) == 153 && red(30, 32) == 232 && red(32, 32) == 255 && red(20, 32) == 0 &&
                  red(27, 32) == red(37, 32) && red(32, 27) == red(32, 37),
              "blur");
    }
    {
        // The overview grid and the presenter layout (spec 26), as in slides.rs's `overview` / `presenter` tests.
        const slides::Grid g = slides::overview_grid(11, 1000, 600, 1000.0f / 600.0f, 20, 20);
        check(g.cols == 4 && std::abs(g.cell_w / g.cell_h - 1000.0f / 600.0f) < 1e-4f && 4 * g.cell_w + 5 * 20 <= 1000.001f &&
                  3 * (g.cell_h + 20) + 4 * 20 <= 600.001f,
              "overview grid");
        check(slides::overview_grid(1, 1000, 600, 1, 20, 20).cols == 1 && slides::overview_grid(0, 1000, 600, 1, 20, 20).cols == 1, "overview grid: few");
        // a long deck: cells stay at least 160 px wide, in fewer columns, and the grid scrolls (as slides.rs)
        check(slides::overview_grid(40, 900, 700, 900.0f / 700.0f, 24, 22).cell_w < slides::kOverviewMinCell, "overview: long deck small");
        const slides::Grid lg = slides::overview_grid_min(40, 900, 700, 900.0f / 700.0f, 24, 22, slides::kOverviewMinCell);
        check(lg.cols == 4 && lg.cell_w == 195.0f, "overview: long deck scrolls");
        const slides::Grid fit = slides::overview_grid(11, 1000, 600, 1000.0f / 600.0f, 20, 20);
        const slides::Grid fit_min = slides::overview_grid_min(11, 1000, 600, 1000.0f / 600.0f, 20, 20, 100);
        check(fit.cols == fit_min.cols && fit.cell_w == fit_min.cell_w && slides::overview_grid_min(5, 100, 600, 1, 20, 20, 160).cols == 1,
              "overview: min only when needed");
        check(slides::overview_height(40, 4, 100, 20, 10) == 10 * 110.0f + 11 * 20.0f, "overview height");
        check(slides::overview_follow(0, 500, 600, 400, 20) == 220 && slides::overview_follow(300, 100, 200, 400, 20) == 80 &&
                  slides::overview_follow(50, 100, 200, 400, 20) == 50,
              "overview follow");
        const slides::Grid sq = slides::overview_grid(4, 800, 800, 16.0f / 9.0f, 20, 20);  // 16:9 slides in a square presenter window
        check(std::abs(sq.cell_w / sq.cell_h - 16.0f / 9.0f) < 1e-4f, "overview grid: the slides' shape");
        check(slides::overview_move(5, 1, 0, 4, 11) == 6 && slides::overview_move(0, -1, 0, 4, 11) == 0 && slides::overview_move(10, 1, 0, 4, 11) == 10 &&
                  slides::overview_move(5, 0, 1, 4, 11) == 9 && slides::overview_move(9, 0, 1, 4, 11) == 9 && slides::overview_move(2, 0, -1, 4, 11) == 2,
              "overview moves");
        const slides::PresenterLayout l = slides::presenter_layout(1000, 640, 16.0f / 9.0f);
        check(l.bar == slides::kPresenterBar && l.now[0] == slides::kPresenterPad && l.now[1] == slides::kPresenterBar + slides::kPresenterLabel &&
                  std::abs(l.now[2] / l.now[3] - 16.0f / 9.0f) < 1e-4f && l.now[0] + l.now[2] <= 620 && l.now[1] + l.now[3] <= 640,
              "presenter: now");
        check(l.next[0] > l.now[0] + l.now[2] && l.next[0] + l.next[2] <= 1000 - slides::kPresenterPad + 1e-3f && l.next[1] == l.now[1] &&
                  l.notes[0] == l.next[0] && l.notes[1] > l.next[1] + l.next[3] && l.notes[1] + l.notes[3] <= 640,
              "presenter: next, notes");
        const slides::PresenterLayout t = slides::presenter_layout(50, 50, 1.5f);
        check(t.now[2] > 0 && t.notes[3] >= 0, "presenter: tiny window");
    }
    {
        // Binary files (the Rust port): not opened as documents.
        auto kind = [](std::string_view s) { const char* k = md::looks_binary(s); return std::string(k ? k : "text"); };
        check_eq(kind(""), "text", "binary: empty is text");
        check_eq(kind("# Title\n\nSome *text*.\n"), "text", "binary: markdown is text");
        check_eq(kind("Gr\xC3\xBC\xC3\x9F" "e, \xE6\x95\xB0\xE5\xAD\xA6\n"), "text", "binary: utf-8 is text");
        check_eq(kind("MZ is a short name\n"), "text", "binary: MZ alone is text");
        check_eq(kind("%PDF-1.7\n%text-looking"), "a PDF", "binary: PDF");
        check_eq(kind("\x89PNG\r\n\x1a\n"), "a PNG image", "binary: PNG");
        check_eq(kind("\xFF\xD8\xFF\xE0\x00\x10JFIF"), "a JPEG image", "binary: JPEG");
        check_eq(kind("GIF89a"), "a GIF image", "binary: GIF");
        check_eq(kind(std::string_view("PK\x03\x04rest", 8)), "a ZIP archive", "binary: ZIP");
        check_eq(kind(std::string_view("MZ\x90\x00\x03\x00", 6)), "a Windows program", "binary: exe");
        check_eq(kind(std::string_view("plain text\0more", 15)), "binary data", "binary: NUL byte");
        std::string junk;
        for (int i = 0; i < 200; i++) junk += char(0x80 + i % 64);
        check_eq(kind(junk), "binary data", "binary: invalid UTF-8");
        std::string mostly;
        for (int i = 0; i < 50; i++) mostly += "abcdefghij";
        check_eq(kind(mostly + "\xE9"), "text", "binary: a stray byte is fine");
        check_eq(kind(std::string(8191, 'a') + "\xC3\xA9"), "text", "binary: a character cut by the sample end");
        check_eq(md::binary_message("a.pdf", "a PDF"), "a.pdf is not a text file (looks like a PDF). Calligrus shows Markdown and Org files.", "binary: message");
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
