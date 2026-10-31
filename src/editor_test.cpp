// Editor protocol parser tests (console, no window).
#include <cstdio>
#include <string>

#include "editor.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d\n", __FILE__, __LINE__);            \
            failures++;                                                 \
        }                                                               \
    } while (0)

using editor::Message;
#define BS "\\"  // one backslash

static bool parse(const std::string& line, Message& m, std::string& err) { return editor::parse_message(line, m, err); }

int main() {
    Message m;
    std::string err;
    // (\u escapes are spliced in: before C++23 a raw string literal still translates them itself.)
    CHECK(parse(R"({"type":"open","doc":"1","path":"C:\\a\\b.md","text":"# Hi\nline \"two\"\t)" BS "u00e9 " BS "ud83d" BS "ude00 " BS BS R"("})",
                m, err));
    CHECK(m.type == Message::Type::Open && m.doc == "1" && m.path == "C:\\a\\b.md");
    CHECK(m.text == "# Hi\nline \"two\"\t\xC3\xA9 \xF0\x9F\x98\x80 \\");

    CHECK(parse(R"({"type":"open","doc":"2","text":"x"})", m, err) && m.path.empty());  // path optional
    CHECK(parse(R"( { "type" : "text" , "doc" : "1" , "text" : "" , "extra" : [1,{"a":"}"}] , "n": null } )", m, err));
    CHECK(m.type == Message::Type::Text && m.text.empty());
    CHECK(parse(R"({"type":"cursor","doc":"1","line":60})", m, err) && m.type == Message::Type::Cursor && m.line == 60);
    CHECK(parse(R"({"line":7.0,"doc":"d","type":"cursor"})", m, err) && m.line == 7);  // member order is free
    CHECK(parse(R"({"type":"close","doc":"1"})", m, err) && m.type == Message::Type::Close);
    CHECK(parse(R"({"type":"search","doc":"1","text":"foo","case":true})", m, err) && m.type == Message::Type::Search);
    CHECK(m.doc == "1" && m.text == "foo" && m.case_sensitive);
    CHECK(parse(R"({"type":"search","doc":"1","text":"","case":false,"x":true})", m, err) && m.text.empty() && !m.case_sensitive);
    CHECK(parse(R"({"type":"search","doc":"1","text":"a"})", m, err) && !m.case_sensitive);  // case optional
    CHECK(!parse(R"({"type":"search","text":"a"})", m, err) && !err.empty());              // doc required
    CHECK(parse(R"({"type":"quit"})", m, err) && m.type == Message::Type::Quit);

    // Lone surrogates become U+FFFD instead of garbage.
    CHECK(parse(R"({"type":"text","doc":"1","text":"a)" BS "ud83dz" BS R"(ude00"})", m, err));
    CHECK(m.text == "a\xEF\xBF\xBDz\xEF\xBF\xBD");

    // Unknown type: ignored without an error.
    CHECK(!parse(R"({"type":"future","doc":"1"})", m, err) && err.empty());
    // Malformed / incomplete: reported.
    CHECK(!parse("not json", m, err) && !err.empty());
    CHECK(!parse(R"({"type":"text","doc":"1","text":"unterminated)", m, err) && !err.empty());
    CHECK(!parse(R"({"type":"text","doc":"1"})", m, err) && !err.empty());              // text missing
    CHECK(!parse(R"({"type":"cursor","doc":"1","line":"5"})", m, err) && !err.empty());  // line must be a number
    CHECK(!parse(R"({"doc":"1"})", m, err) && !err.empty());                              // no type
    CHECK(!parse(R"({"type":"quit"} x)", m, err) && !err.empty());                        // trailing junk
    CHECK(!parse(R"({"type":"text","doc":"1","text":"bad \q escape"})", m, err) && !err.empty());

    // Coalescing.
    Message a, b, c;
    parse(R"({"type":"text","doc":"1","text":"a"})", a, err);
    parse(R"({"type":"text","doc":"1","text":"b"})", b, err);
    parse(R"({"type":"text","doc":"2","text":"c"})", c, err);
    CHECK(editor::supersedes(b, a) && !editor::supersedes(c, a));

    // Line -> block mapping (shared by scroll sync, the cursor marker and sync-dump).
    const std::vector<editor::LineRange> bl = {{1, 1}, {3, 6}, {8, 8}, {0, 0}, {12, 14}};
    CHECK(editor::target_block(bl, 1) == 0 && editor::target_block(bl, 4) == 1 && editor::target_block(bl, 6) == 1);
    CHECK(editor::target_block(bl, 7) == 1);   // blank line: the last block starting before it
    CHECK(editor::target_block(bl, 13) == 4 && editor::target_block(bl, 99) == 4);
    CHECK(editor::target_block({}, 5) == 0);
    CHECK(editor::line_fraction(bl[1], 3) == 0.0f && editor::line_fraction(bl[1], 5) == 0.5f);
    CHECK(editor::line_fraction(bl[1], 99) == 1.0f && editor::line_fraction(bl[3], 5) == 0.0f);
    CHECK(editor::count_lines("") == 0 && editor::count_lines("a") == 1 && editor::count_lines("a\n") == 1);
    CHECK(editor::count_lines("a\nb") == 2 && editor::count_lines("a\n\n") == 2);

    // Item-level targets and the tick rule (spec 18).
    {
        using editor::SyncNode;
        const SyncNode para{{1, 3}, true, {}};
        const SyncNode code{{5, 9}, false, {}};
        const SyncNode list{{11, 20}, false,
                            {{{11, 11}, true, {}}, {{12, 15}, true, {}}, {{16, 20}, false, {}}}};  // 3rd item: several blocks
        const SyncNode callout{{22, 30}, false, {{{22, 22}, true, {}}, {{23, 23}, true, {}}, {{25, 30}, false, {}}}};
        const std::vector<SyncNode> doc = {para, code, list, callout};
        auto at = [&](int line) { return editor::select_target(doc, line); };
        CHECK(at(2).block == 0 && at(2).sub == -1 && at(2).lines.first == 1 && !at(2).tick);   // multi-line paragraph: no tick
        CHECK(at(7).block == 1 && at(7).sub == -1 && at(7).tick);                              // code block: tick
        CHECK(at(13).block == 2 && at(13).sub == 1 && at(13).lines.first == 12 && at(13).lines.last == 15);  // 2nd item
        CHECK(!at(13).tick);                                                                   // a single-paragraph item: no tick
        CHECK(at(18).sub == 2 && at(18).tick);                                                 // item with several blocks: tick
        CHECK(at(11).sub == 0 && !at(11).tick);                                                // one-line item: no tick
        CHECK(at(10).block == 1 && at(10).sub == -1);                                          // blank line before the list: the code block
        CHECK(at(24).block == 3 && at(24).sub == 1);                                           // blank `>` line: the previous child
        CHECK(at(22).sub == 0 && at(22).lines.first == 22);                                    // callout marker line: its title
        CHECK(at(27).sub == 2 && at(27).tick && at(27).lines.first == 25);
        CHECK(at(99).block == 3 && at(99).sub == 2);                                           // past the end: the last child
        // Subs without known lines fall back to the block itself.
        const std::vector<SyncNode> odd = {{{1, 4}, false, {{{0, 0}, true, {}}}}};
        CHECK(editor::select_target(odd, 2).sub == -1 && editor::select_target(odd, 2).lines.first == 1);
        CHECK(editor::select_target({}, 3).sub == -1);
    }

    // Reverse mapping: Ctrl+click -> source line (spec 23). Same cases as editor.rs's tests.
    {
        using editor::NodeKind;
        using editor::SyncNode;
        using editor::Band;
        // heading 1, paragraph 3-6, list 8-11 (items 8-9, 10-11), code 13-17 (fence, 3 lines, fence)
        SyncNode list{{8, 11}, false, {{{8, 9}, true, {}}, {{10, 11}, true, {}}}};
        SyncNode code{{13, 17}, false, {}, NodeKind::Code};
        const std::vector<SyncNode> doc = {{{1, 1}, true, {}}, {{3, 6}, true, {}}, list, code};
        const float tops[] = {0, 50, 140, 200}, hs[] = {40, 80, 50, 70};
        editor::SpanFn span = [&](size_t i, int sub) -> std::optional<std::pair<float, float>> {
            if (i == 2 && sub == 0) return std::make_pair(140.0f, 20.0f);
            if (i == 2 && sub == 1) return std::make_pair(160.0f, 30.0f);
            return std::make_pair(tops[i], hs[i]);
        };
        editor::RowsFn rows = [&](size_t i, int) {
            return i == 3 ? std::vector<Band>{{210, 20}, {230, 20}, {250, 20}} : std::vector<Band>{};
        };
        auto at = [&](float y) { return editor::line_at(doc, y, span, rows); };
        CHECK(at(5) == 1 && at(45) == 1 && at(-9) == 1);                  // heading; gap below it; above everything
        CHECK(at(50) == 3 && at(69) == 3 && at(70) == 4 && at(129) == 6);  // 4 lines in 80
        CHECK(at(141) == 8 && at(155) == 9 && at(165) == 10 && at(180) == 11);  // items
        // code: the padding above -> first code line, then each line, below -> the last
        CHECK(at(202) == 14 && at(215) == 14 && at(240) == 15 && at(255) == 16 && at(269) == 16);
        // the exact inverse: the marker's y of every text line lands back on that line
        for (int line : {1, 3, 4, 5, 6, 8, 9, 10, 11}) {
            const editor::SyncTarget t = editor::select_target(doc, line);
            const auto s = span(t.block, t.sub);
            const float y = s->first + editor::line_fraction(t.lines, line) * s->second;
            CHECK(at(y) == line);
        }
        CHECK(editor::line_at({}, 5, span, rows) == 0);

        // Tables, display math, unfenced code.
        const std::vector<SyncNode> d2 = {{{1, 4}, false, {}, NodeKind::Table}, {{7, 7}, false, {}, NodeKind::Atom}, {{9, 11}, false, {}, NodeKind::Code}};
        const float t2[] = {0, 100, 150}, h2[] = {90, 40, 60};
        editor::SpanFn span2 = [&](size_t i, int) -> std::optional<std::pair<float, float>> { return std::make_pair(t2[i], h2[i]); };
        editor::RowsFn rows2 = [&](size_t i, int) {
            if (i == 0) return std::vector<Band>{{0, 30}, {30, 30}, {60, 30}};  // header, body 0, body 1: lines 1, 3, 4
            if (i == 2) return std::vector<Band>{{155, 20}, {175, 20}, {195, 20}};
            return std::vector<Band>{};
        };
        auto at2 = [&](float y) { return editor::line_at(d2, y, span2, rows2); };
        CHECK(at2(10) == 1 && at2(40) == 3 && at2(80) == 4);  // body row 0 skips the delimiter row
        CHECK(at2(120) == 7);                                 // display math: its first line
        CHECK(at2(160) == 9 && at2(180) == 10 && at2(200) == 11);  // unfenced: as many lines as rows

        // Quote children and the callout title.
        SyncNode callout{{1, 6}, false, {{{1, 1}, true, {}}, {{2, 3}, true, {}}, {{5, 6}, false, {}, NodeKind::Code}}};
        const std::vector<SyncNode> d3 = {callout};
        editor::SpanFn span3 = [&](size_t, int sub) -> std::optional<std::pair<float, float>> {
            if (sub < 0) return std::make_pair(0.0f, 120.0f);
            if (sub == 0) return std::make_pair(0.0f, 30.0f);
            if (sub == 1) return std::make_pair(30.0f, 40.0f);
            return std::make_pair(80.0f, 40.0f);
        };
        editor::RowsFn rows3 = [&](size_t, int sub) { return sub == 2 ? std::vector<Band>{{90, 20}} : std::vector<Band>{}; };
        auto at3 = [&](float y) { return editor::line_at(d3, y, span3, rows3); };
        CHECK(at3(5) == 1 && at3(35) == 2 && at3(60) == 3);  // title, paragraph lines
        CHECK(at3(75) == 3);                                 // gap between children: the child above
        CHECK(at3(95) == 6);                                 // fence + 1 line: the code line is first + 1

        // The message, and comparing two messages as parsed objects.
        CHECK(editor::goto_message("7", 42) == R"({"type":"goto","doc":"7","line":42})");
        CHECK(editor::goto_message("a\"b\\", 1) == R"({"type":"goto","doc":"a\"b\\","line":1})");
        CHECK(editor::notes_message("7", 2, 5, "say \"hi\"\nthen go", {9, 17}) ==
              R"({"type":"notes","doc":"7","slide":2,"total":5,"notes":"say \"hi\"\nthen go","line":9,"end":17})");
        CHECK(editor::json_objects_equal(R"({"line":3,"doc":"g","type":"goto"})", R"({ "type" : "goto", "doc":"g", "line":3.0 })"));
        CHECK(!editor::json_objects_equal(R"({"type":"goto","doc":"g","line":3})", R"({"type":"goto","doc":"g","line":4})"));
        CHECK(!editor::json_objects_equal(R"({"type":"goto"})", R"({"type":"goto","doc":"g"})"));
        CHECK(editor::valid_json_object(R"({"a":1})") && !editor::valid_json_object("{nope"));
    }

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
