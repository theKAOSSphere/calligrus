// calligrus-sysfont-test: system fonts for missing characters (spec 30). Console only, no window.
// Exit code 0 = all passed.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "model.h"
#include "resources.h"
#include "sysfont.h"

static int failures = 0, passed = 0;
static void check(bool ok, const std::string& what) {
    if (!ok) std::printf("FAIL %s\n", what.c_str());
    failures += !ok;
    passed += ok;
}

using Names = std::vector<std::string>;

static FontFace face(const std::string& family, const std::string& file) {
    FontFace f;
    f.family = family;
    f.file = file;
    return f;
}

// Made-up fonts: (family, characters it has, colour font?). Returns the chosen families.
struct Fake {
    std::string family, has;
    bool colour = false;
};
static Names pick(const std::vector<Fake>& fonts, const Names& preferred, const std::u32string& missing, size_t max) {
    std::vector<FontFace> faces;
    std::map<std::string, const Fake*> by_file;
    for (const Fake& f : fonts) {
        faces.push_back(face(f.family, f.family + ".ttf"));
        by_file[f.family + ".ttf"] = &f;
    }
    auto coverage = [&](const FontFace& face, const std::vector<char32_t>& m, std::vector<char32_t>& covered) {
        const Fake& f = *by_file[face.file];
        if (f.colour) return false;
        for (char32_t c : m)
            if (c < 128 && f.has.find(char(c)) != std::string::npos) covered.push_back(c);
        return true;
    };
    Names out;
    for (const FontSource& s : sysfont::pick_with(faces, preferred, std::vector<char32_t>(missing.begin(), missing.end()), max, coverage))
        out.push_back(s.file.substr(0, s.file.size() - 4));
    return out;
}

int main() {
    {  // greedy
        const std::vector<Fake> fonts = {{"A", "ab"}, {"B", "abcd"}, {"C", "ef"}};
        check(pick(fonts, {}, U"abcdef", 4) == Names({"B", "C"}), "greedy takes the biggest cover first");
        check(pick(fonts, {}, U"z", 4).empty(), "nothing covers z");
        check(pick(fonts, {}, U"", 4).empty(), "nothing missing");
    }
    {  // ties
        std::vector<Fake> fonts = {{"Zed", "ab"}, {"Alpha", "ab"}};
        check(pick(fonts, {}, U"ab", 4) == Names({"Alpha"}), "ties: by name");
        check(pick(fonts, {"Zed"}, U"ab", 4) == Names({"Zed"}), "ties: by the preferred order");
        fonts = {{"Zed", "a"}, {"Alpha", "ab"}};
        check(pick(fonts, {"Zed"}, U"ab", 4) == Names({"Alpha"}), "a bigger cover wins over a preferred family");
    }
    {  // limit
        const std::vector<Fake> fonts = {{"A", "a"}, {"B", "b"}, {"C", "c"}, {"D", "d"}, {"E", "e"}};
        check(pick(fonts, {}, U"abcde", 4) == Names({"A", "B", "C", "D"}), "at most 4");
        check(pick(fonts, {}, U"abcde", 2) == Names({"A", "B"}), "at most 2");
    }
    check(pick({{"Emoji", "abc", true}, {"Plain", "a"}}, {}, U"abc", 4) == Names({"Plain"}), "colour fonts are skipped");
    {  // prefix families and case
        std::vector<FontFace> faces;
        for (const char* f : {"Noto Sans Gujarati", "noto sans arabic", "Aaa", "Noto Serif"}) faces.push_back(face(f, f));
        auto order = [&](const Names& preferred) {
            Names out;
            for (const FontFace& f : sysfont::ordered_candidates(faces, preferred)) out.push_back(f.family);
            return out;
        };
        check(order({"Noto Sans*"}) == Names({"noto sans arabic", "Noto Sans Gujarati", "Aaa", "Noto Serif"}), "prefix family group");
        check(order({"NOTO SERIF", "Noto Sans*"})[0] == "Noto Serif", "preferred order, any case");
    }
    {  // ignorable characters
        for (char32_t c : {U' ', U'\n', U'\t', char32_t(0x200D), char32_t(0xFE0F), char32_t(0xFEFF)})
            check(sysfont::ignorable(c), "ignorable " + std::to_string(unsigned(c)));
        check(!sysfont::ignorable(U'a') && !sysfont::ignorable(0x0A97), "letters are not ignorable");
    }
    {  // the coverage check on the bundled fonts: "a" is there, a Gujarati letter is not
        const char* names[] = {"fonts/IBMPlexSans-Regular.ttf", "fonts/CascadiaCode-Regular.ttf"};
        std::vector<std::string_view> data;
        for (const char* n : names) {
            auto bytes = resource(n);
            check(bytes.has_value(), std::string("resource ") + n);
            if (bytes) data.push_back(*bytes);
        }
        // (A cmap straight from the bytes: find the table directory's cmap record.)
        auto cmap_of = [](std::string_view font) {
            auto be16 = [&](size_t at) { return (size_t(uint8_t(font[at])) << 8) | uint8_t(font[at + 1]); };
            auto be32 = [&](size_t at) { return (be16(at) << 16) | be16(at + 2); };
            for (size_t i = 0; i < be16(4); i++) {
                const size_t rec = 12 + i * 16;
                if (font.compare(rec, 4, "cmap") == 0) return font.substr(be32(rec + 8), be32(rec + 12));
            }
            return std::string_view();
        };
        std::vector<std::string_view> cmaps;
        for (auto d : data) cmaps.push_back(cmap_of(d));
        auto has = [&](char32_t c) {
            for (auto cm : cmaps)
                if (sysfont::cmap_has(cm, c)) return true;
            return false;
        };
        check(has(U'a') && !has(0x0A97), "bundled fonts: a covered, U+0A97 not");
        const std::set<char32_t> chars = {U'a', 0x0A97, U' ', U'\n', 0x0A97, U'b'};
        check(sysfont::missing_chars(chars, has) == std::vector<char32_t>({0x0A97}), "missing_chars");
        check(!sysfont::cmap_has("junk", U'a'), "junk has nothing");
    }
    {  // a document's characters: prose, code and table cells, not maths
        md::Document doc = md::parse("ab `c` $x$\n\n```\nd\n```\n\n| e |\n|---|\n| f |\n");
        const std::set<char32_t> chars = sysfont::document_chars(doc);
        for (char32_t c : U"abcdef") if (c) check(chars.count(c) == 1, std::string("document char ") + char(c));
        check(chars.count(U'x') == 0, "no maths");
    }
    std::printf(failures ? "%d failed\n" : "all %d passed\n", failures ? failures : passed);
    return failures ? 1 : 0;
}
