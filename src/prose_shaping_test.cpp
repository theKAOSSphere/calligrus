// calligrus-prose-shaping-test: complex scripts in prose (spec 32). Console only, no window.
// Exit code 0 = all passed.
//
//   1. script ranges and run splitting (Common / Inherited characters join a neighbour);
//   2. choosing the font of a run, and the cluster grouping, on made-up data;
//   3. real HarfBuzz shaping of Devanagari with the test font in spec/tests/fonts/
//      (OFL: Noto Sans Devanagari, which also has Latin): conjuncts make fewer glyphs than
//      characters, and a cluster spans several characters.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "prose_shaping.h"

#ifndef CALLIGRUS_SPEC_TESTS_DIR
#error "CALLIGRUS_SPEC_TESTS_DIR must point at spec/tests (set by CMakeLists.txt)"
#endif

using namespace prose_shaping;

static int failures = 0, passed = 0;
static void check(bool ok, const std::string& what) {
    if (!ok) std::printf("FAIL %s\n", what.c_str());
    failures += !ok;
    passed += ok;
}

// The runs of a UTF-8 string (written with \u escapes, which the compiler turns into UTF-8).
static std::vector<Run> runs_of(const char* utf8) { return split_runs(decode(utf8)); }

static void test_scripts() {
    check(script_of('a') == Script::Simple, "a is simple");
    check(script_of(0x4E2D) == Script::Simple, "CJK is simple");
    check(script_of(' ') == Script::Neutral && script_of('7') == Script::Neutral && script_of(',') == Script::Neutral,
          "space, digit, comma are neutral");
    check(script_of(0x200D) == Script::Neutral && script_of(0x0301) == Script::Neutral,
          "ZWJ and a combining accent are neutral");
    check(script_of(0x0915) == Script::Devanagari, "U+0915 is Devanagari");
    check(script_of(0x0964) == Script::Neutral, "the danda is neutral (Common) though inside the Devanagari block");
    check(script_of(0x0A97) == Script::Gujarati, "U+0A97 is Gujarati");
    check(script_of(0x0E01) == Script::Thai && script_of(0x0B95) == Script::Tamil && script_of(0x1780) == Script::Khmer,
          "Thai, Tamil, Khmer");
    check(is_complex(Script::Myanmar) && !is_complex(Script::Simple) && !is_complex(Script::Neutral), "is_complex");
}

static void test_runs() {
    check(split_runs({}).empty(), "no characters, no runs");
    check(runs_of("abc def") == std::vector<Run>{{0, 7, Script::Simple}}, "Latin text is one simple run");
    // "ab " (3 characters), हिन्दी (ह ि न ् द ी: 6 characters), ", cd"
    check(runs_of("ab हिन्दी, cd") ==
              std::vector<Run>{{0, 3, Script::Simple}, {3, 9, Script::Devanagari}, {9, 13, Script::Simple}},
          "Latin, Devanagari, Latin: the space and comma stay out of the Devanagari run");
    // Brackets, spaces and a danda are Simple (old path); the letters around them are shaped.
    check(runs_of("(हि)") ==
              std::vector<Run>{{0, 1, Script::Simple}, {1, 3, Script::Devanagari}, {3, 4, Script::Simple}},
          "(हि): bracket, run, bracket");
    check(runs_of("  क") == std::vector<Run>{{0, 2, Script::Simple}, {2, 3, Script::Devanagari}}, "leading spaces are simple");
    check(runs_of("क। ख") ==
              std::vector<Run>{{0, 1, Script::Devanagari}, {1, 3, Script::Simple}, {3, 4, Script::Devanagari}},
          "danda and space split two runs");
    check(runs_of("  ") == std::vector<Run>{{0, 2, Script::Simple}}, "only neutral characters: simple");
    // ZWJ between letters of a run stays inside it; so does a combining mark.
    check(runs_of("क्‍ष") == std::vector<Run>{{0, 4, Script::Devanagari}}, "ZWJ stays inside a run");
    check(runs_of("क॑") == std::vector<Run>{{0, 2, Script::Devanagari}}, "a Vedic accent (a sign of the block) stays");
    check(runs_of("क́") == std::vector<Run>{{0, 2, Script::Devanagari}}, "a combining accent joins the letter before it");
    // Two scripts next to each other: two runs, the space between them simple.
    check(runs_of("क ગ") ==
              std::vector<Run>{{0, 1, Script::Devanagari}, {1, 2, Script::Simple}, {2, 3, Script::Gujarati}},
          "Devanagari, space, Gujarati");
    check(runs_of("कગ") == std::vector<Run>{{0, 1, Script::Devanagari}, {1, 2, Script::Gujarati}},
          "Devanagari then Gujarati: two runs");
    // A combining mark after a Latin letter is part of the Latin run (nothing to shape).
    check(runs_of("é") == std::vector<Run>{{0, 2, Script::Simple}}, "e + accent is simple");
}

static void test_choice_and_clusters() {
    // Made-up coverage: font 0 has a..c, font 1 has a..z, font 2 has everything.
    auto has = [](size_t f, uint32_t cp) {
        return f == 2 || (f == 0 && cp >= 'a' && cp <= 'c') || (f == 1 && cp >= 'a' && cp <= 'z');
    };
    const uint32_t abc[] = {'a', 'b', 'c'}, abx[] = {'a', 'b', 'x'}, hi[] = {0x0939};
    check(choose_source(3, abc, 3, has) == 0, "the first font with every character wins");
    check(choose_source(3, abx, 3, has) == 1, "a font lacking one character is skipped");
    check(choose_source(3, hi, 1, has) == 2, "only the last font has it");
    check(choose_source(2, hi, 1, has) == -1, "no font has it: -1 (the run is drawn the old way)");
    check(choose_source(0, abc, 3, has) == -1, "no fonts: -1");

    // 7 characters, glyph clusters 0,0,3,5: three clusters (the first with two glyphs).
    std::vector<Glyph> g(4);
    g[0].cluster = 0, g[1].cluster = 0, g[2].cluster = 3, g[3].cluster = 5;
    const std::vector<Cluster> c = clusters(g, 7);
    check(c == std::vector<Cluster>{{0, 3, 0, 2}, {3, 5, 2, 3}, {5, 7, 3, 4}},
          "clusters reach up to the next cluster / the run end");
    check(clusters({}, 0).empty(), "no glyphs, no clusters");
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void test_real_shaping() {
    const std::string font_file = std::string(CALLIGRUS_SPEC_TESTS_DIR) + "/fonts/NotoSansDevanagari-Regular.ttf";
    static const std::string bytes = read_file(font_file);  // lives as long as the chains
    check(!bytes.empty(), "the test font is there: " + font_file);
    if (bytes.empty()) return;

    int one = 1, two = 2, three = 3;  // font ids: any distinct pointers
    // `one`: the test font alone. `two`: an unusable first source, then the test font (fallback order).
    // `three`: no fonts.
    set_chains({Chain{&one, {Source{bytes, 0, 1.0f, 1.2f}}},
                Chain{&two, {Source{std::string_view(), 0, 1.0f, 1.0f}, Source{bytes, 0, 1.0f, 1.2f}}},
                Chain{&three, {}}});

    check(plan(&one, "plain English text") == nullptr, "Latin only: no plan (old path)");
    check(plan(&one, "") == nullptr, "empty: no plan");
    int unknown = 0;
    check(plan(&unknown, "हि") == nullptr, "an unknown font: no plan");
    check(plan(&three, "हि") == nullptr, "a font without sources: no plan");

    // "हिन्दी" = ह ि न ् द ी: six characters. ि sits before ह and न्द is a conjunct.
    const char* hindi = "हिन्दी";
    auto p = plan(&one, hindi);
    check(p != nullptr && p->parts.size() == 1 && p->parts[0].shaped, "हिन्दी: one shaped part");
    if (p && p->parts.size() == 1) {
        const Part& part = p->parts[0];
        check(p->chars.size() == 6 && part.end == 6, "six characters");
        check(part.glyphs.size() < 6 && !part.glyphs.empty(),
              "fewer glyphs than characters (" + std::to_string(part.glyphs.size()) + ")");
        bool wide = false, nonzero = true;
        for (const Cluster& cl : part.clusters) wide |= cl.last - cl.first > 1;
        for (const Glyph& gl : part.glyphs) nonzero &= gl.id != 0;
        check(wide, "a cluster spans several characters");
        check(nonzero, "no .notdef glyph");
        check(!part.clusters.empty() && part.clusters.front().first == 0 && part.clusters.back().last == 6,
              "the clusters cover all characters");
        float total = 0;
        for (const Glyph& gl : part.glyphs) total += gl.x_adv;
        check(total > 1.0f && total < 6.0f, "the advances sum to a plausible width (em): " + std::to_string(total));
    }

    // "क्षत्रिय": क ् ष त ् र ि य = 8 characters, two conjuncts.
    auto q = plan(&one, "क्षत्रिय");
    check(q && q->parts.size() == 1 && q->parts[0].glyphs.size() < 8, "क्षत्रिय: fewer glyphs than its 8 characters");
    if (q && q->parts.size() == 1) {
        size_t widest = 0;
        for (const Cluster& cl : q->parts[0].clusters) widest = std::max(widest, cl.last - cl.first);
        check(widest >= 3, "क्ष makes a cluster of three characters (widest: " + std::to_string(widest) + ")");
    }

    // Mixed text: a plain part for the English (old path), a shaped one for the Hindi; they cover everything.
    auto m = plan(&one, "word हिन्दी, end");
    check(m && m->parts.size() == 3 && !m->parts[0].shaped && m->parts[1].shaped && !m->parts[2].shaped,
          "mixed text: plain, shaped, plain");
    if (m && m->parts.size() == 3)
        check(m->parts[0].begin == 0 && m->parts[0].end == m->parts[1].begin && m->parts[1].end == m->parts[2].begin &&
                  m->parts[2].end == m->chars.size(),
              "the parts cover the text without gaps");

    // Fallback: the first source of `two` has nothing, so the second shapes it.
    auto f = plan(&two, hindi);
    check(f && f->parts.size() == 1 && f->parts[0].shaped && f->parts[0].source.bytes.data() == bytes.data(),
          "the first source that has the characters is used");
    // Thai: the test font has no Thai, so no part is shaped (the whole string stays on the old path)...
    check(plan(&one, "กา") == nullptr, "a run no font covers: no plan");
    // ...but a Hindi word next to it is still shaped, and the Thai part is plain.
    auto t = plan(&one, "हि กา");
    check(t && t->parts.size() == 3 && t->parts[0].shaped && !t->parts[1].shaped && !t->parts[2].shaped,
          "Hindi shaped, the space and the Thai left plain");

    // ppem: sizes scale like ImGui's merged fonts.
    check(ppem(Source{bytes, 0, 1.0f, 1.25f}, 20.0f) == 16.0f, "ppem = exact size x ratio / em");

    // The cache returns the same plan.
    check(plan(&one, hindi) == p, "the cache returns the same plan");
    set_chains({});
    check(plan(&one, hindi) == nullptr, "set_chains clears the fonts");
}

int main() {
    test_scripts();
    test_runs();
    test_choice_and_clusters();
    test_real_shaping();
    std::printf("%d passed, %d failed\n", passed, failures);
    return failures ? 1 : 0;
}
