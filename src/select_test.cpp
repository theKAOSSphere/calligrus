// calligrus-select-test: selection checks,
// run against the C++ select.cpp (no window). Exit code 0 = all passed.
#include <cstdio>
#include <string>

#include "select.h"

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

ImVec2 v(float x, float y) { return ImVec2(x, y); }

// Words on one line from (x, y); each char 10px wide, a 10px space token between words.
void line(Collector& c, std::initializer_list<const char*> words, float x, float y, bool hard) {
    c.new_line();
    float cx = x;
    bool first = true;
    for (const char* w : words) {
        if (!first) {
            c.push(v(cx, y), v(cx + 10, y + 20), {cx, cx + 10}, " ", hard);
            cx += 10;
        }
        first = false;
        std::string s = w;
        std::vector<float> xs;
        for (size_t k = 0; k <= s.size(); k++) xs.push_back(cx + 10.0f * k);
        c.push(v(cx, y), v(cx + 10.0f * s.size(), y + 20), xs, s, hard);
        cx += 10.0f * s.size();
    }
}

TextPos pos(uint32_t b, uint32_t t, uint32_t ch) { return {b, t, ch}; }

// Same sample as the Rust test: wrapped paragraph + hard break, 2x2 table, code, equation.
Collector sample() {
    Collector c;
    c.begin_frame(std::nullopt);
    c.begin_block(0);
    c.begin_group(Sep::Para);
    line(c, {"Hello", "brave"}, 0, 0, false);
    line(c, {"new", "world"}, 0, 20, false);
    line(c, {"after"}, 0, 40, true);
    c.begin_block(1);
    const char* cells[2][2] = {{"a", "b"}, {"c", "d"}};
    for (int r = 0; r < 2; r++)
        for (int k = 0; k < 2; k++) {
            c.begin_group(r == 0 && k == 0 ? Sep::Para : k == 0 ? Sep::Row : Sep::Cell);
            line(c, {cells[r][k]}, 100.0f * k, 80.0f + 20.0f * r, false);
        }
    c.begin_block(2);
    c.begin_group(Sep::Para);
    line(c, {"fn main() {"}, 0, 140, false);
    c.begin_group(Sep::Line);
    line(c, {"}"}, 0, 160, false);
    c.begin_block(3);
    c.begin_group(Sep::Para);
    c.new_line();
    c.push(v(0, 200), v(50, 220), {0, 50}, "$$x^2$$", false);
    return c;
}

}  // namespace

int main() {
    Collector c = sample();
    check_eq(selected_text(c.runs, {pos(0, 0, 2), pos(0, 2, 3)}), "llo bra", "partial words at both ends");
    check_eq(selected_text(c.runs, {pos(0, 0, 0), pos(0, UINT32_MAX, 0)}), "Hello brave new world\nafter",
             "soft wrap -> space, hard break -> newline");
    check_eq(selected_text(c.runs, {TextPos::start(), TextPos::end()}),
             "Hello brave new world\nafter\n\na\tb\nc\td\n\nfn main() {\n}\n\n$$x^2$$",
             "paragraphs, tables, code and math separators");
    check_eq(selected_text(c.runs, {pos(3, 0, 0), pos(3, 0, 1)}), "$$x^2$$", "equation copies as TeX");

    Collector h;
    h.begin_frame(TextRange{pos(0, 0, 1), pos(0, 0, 3)});
    h.begin_block(0);
    h.begin_group(Sep::Para);
    h.new_line();
    auto hl = h.push(v(0, 0), v(50, 20), {0, 10, 20, 30, 40, 50}, "Hello", false);
    check(hl && hl->first == 10 && hl->second == 30, "highlight range of a partly selected word");
    check(!h.push(v(60, 0), v(90, 20), {60, 70, 80, 90}, "you", false), "no highlight outside the selection");

    check(hit(c.runs, v(12, 10)) == pos(0, 0, 1), "hit: nearest character boundary");
    check(hit(c.runs, v(500, 5)) == pos(0, 2, 5), "hit: right of a line -> end of its last word");
    check(hit(c.runs, v(0, 21)) == pos(0, 3, 0), "hit: between lines -> closest line");

    auto w = word_at(c.runs, pos(0, 2, 2));
    check(w && w->first == pos(0, 2, 0) && w->second == pos(0, 2, 5), "double-click selects the word");
    auto g = group_at(c.runs, pos(0, 4, 1));
    check(g && selected_text(c.runs, *g) == "Hello brave new world\nafter", "triple-click selects the paragraph");

    Selection s;
    s.set(pos(1, 0, 0), pos(0, 3, 2));
    check(s.range() && s.range()->first == pos(0, 3, 2), "backwards drag gives an ordered range");
    s.set(pos(0, 1, 1), pos(0, 1, 1));
    check(!s.range(), "collapsed selection is empty");

    std::printf("%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
