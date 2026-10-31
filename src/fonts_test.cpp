// calligrus-fonts-test: fonts by family name (spec 8a). Console only, no window.
// Exit code 0 = all passed.
//
//   1. the fixtures in spec/tests (a fake index
//      plus `kind` / `pick` / `families` queries; format in their README.md §5),
//      run against font_index.cpp's resolver;
//   2. resolve_fonts(): the config values -> faces rules (styles, overrides,
//      unknown names, index not ready yet);
//   3. the real scanner on this machine's font folders (timed), and
//      font_em_ratio() on a .ttc face other than the first;
//   4. code ligatures (shaping.h) on the bundled Cascadia Code.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "font_index.h"
#include "fonts.h"
#include "resources.h"
#include "shaping.h"

#ifndef CALLIGRUS_SPEC_TESTS_DIR
#error "CALLIGRUS_SPEC_TESTS_DIR must point at spec/tests (set by CMakeLists.txt)"
#endif

namespace fs = std::filesystem;

static int failures = 0, passed = 0;
static void check(bool ok, const std::string& what) {
    if (!ok) std::printf("FAIL %s\n", what.c_str());
    failures += !ok;
    passed += ok;
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string trim(std::string s) {
    const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static std::string lower_name(std::string s) {
    for (char& c : s) c = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    return s;
}

static std::string face_id(const FontFace& f) { return f.file + "#" + std::to_string(f.face); }

// --- 1. the shared fontpick-*.txt fixtures -------------------------------------------------

static int run_fixture(const fs::path& path) {
    const std::string name = path.filename().string();
    std::vector<FontFace> index;
    std::vector<std::pair<int, std::string>> queries;  // (line number, line)
    std::istringstream in(read_file(path));
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        lineno++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty() || line[0] == '#') continue;
        if (line.find(" => ") != std::string::npos) {
            queries.emplace_back(lineno, line);
            continue;
        }
        // family | subfamily | weight | italic | mono | file#face [| width]
        std::vector<std::string> fields;
        size_t start = 0;
        for (size_t bar; (bar = line.find('|', start)) != std::string::npos; start = bar + 1)
            fields.push_back(trim(line.substr(start, bar - start)));
        fields.push_back(trim(line.substr(start)));
        const bool shape_ok = fields.size() == 6 || fields.size() == 7;
        const size_t hash = shape_ok ? fields[5].rfind('#') : std::string::npos;
        if (hash == std::string::npos) {
            check(false, name + ":" + std::to_string(lineno) + " is a well-formed face line");
            continue;
        }
        FontFace f;
        f.family = fields[0];
        f.subfamily = fields[1];
        f.weight = std::stoi(fields[2]);
        f.italic = fields[3] == "1";
        f.mono = fields[4] == "1";
        f.file = fields[5].substr(0, hash);
        f.face = std::stoi(fields[5].substr(hash + 1));
        if (fields.size() == 7) f.width = std::stoi(fields[6]);  // optional, default 5
        index.push_back(f);
    }

    for (const auto& [n, q] : queries) {
        const size_t arrow = q.rfind(" => ");
        const std::string want = q.substr(arrow + 4);
        std::string got;
        if (q.starts_with("kind ")) {
            got = is_font_family_name(q.substr(5, arrow - 5)) ? "name" : "path";
        } else if (q.starts_with("pick ")) {
            const FacePicks p = pick_faces(index, q.substr(5, arrow - 5));
            auto id = [](const std::optional<FontFace>& f) { return f ? face_id(*f) : std::string("none"); };
            got = !p.regular ? "not found"
                             : "regular=" + id(p.regular) + " bold=" + id(p.bold) + " italic=" + id(p.italic) +
                                   " bold_italic=" + id(p.bold_italic);
        } else if (q.starts_with("families ")) {
            const std::string which = q.substr(9, arrow - 9);
            for (const FontFamily& f : list_families(index))
                if (which == "all" || f.mono) got += (got.empty() ? "" : " | ") + f.name;
            if (got.empty()) got = "-";
        } else {
            got = "(unknown query)";
        }
        check(got == want, name + ":" + std::to_string(n) + ": " + q.substr(0, arrow) + "\n       want " + want +
                               "\n       got  " + got);
    }
    return int(queries.size());
}

// --- 2. resolve_fonts ---------------------------------------------------------------------------

static void test_resolve() {
    const std::vector<FontFace> index = {
        {"Inter", "Regular", 400, false, false, "C:/f/Inter-Regular.otf", 0},
        {"Inter", "Bold", 700, false, false, "C:/f/Inter-Bold.otf", 0},
        {"Inter", "Italic", 400, true, false, "C:/f/Inter-Italic.otf", 0},
        {"Plain", "Regular", 400, false, false, "C:/f/plain.ttc", 3},
        {"Mono Thing", "Regular", 400, false, true, "C:/f/mono.ttf", 0},
    };
    auto expand = [](const std::string& s) { return "X:" + s; };  // marks expanded paths
    auto src = [](const char* file, int face = 0) { return FontSource{file, face}; };
    auto run = [&](Config::Fonts v, const std::vector<FontFace>* idx = nullptr) { return resolve_fonts(v, idx, expand); };

    {  // all built-in
        ResolvedFonts r = run({}, &index);
        check(r.files == FontFiles{} && r.errors.empty() && !r.waiting, "resolve: empty values = built-in");
    }
    {  // prose by name: automatic picks; missing bold italic -> the family's regular
        ResolvedFonts r = run({.prose = "inter"}, &index);
        check(r.files.prose == src("C:/f/Inter-Regular.otf") && r.files.prose_bold == src("C:/f/Inter-Bold.otf") &&
                  r.files.prose_italic == src("C:/f/Inter-Italic.otf") &&
                  r.files.prose_bold_italic == src("C:/f/Inter-Regular.otf") && r.files.mono.builtin(),
              "resolve: prose by name picks styles, missing style = family regular");
    }
    {  // a .ttc face index is carried through
        ResolvedFonts r = run({.prose = "Plain"}, &index);
        check(r.files.prose == src("C:/f/plain.ttc", 3) && r.files.prose_bold == src("C:/f/plain.ttc", 3),
              "resolve: ttc face index kept");
    }
    {  // explicit overrides: a path, and a name (that family's pick for the style, else its regular)
        ResolvedFonts r = run({.prose = "Inter", .prose_bold = "~/b.ttf", .prose_italic = "Plain"}, &index);
        check(r.files.prose_bold == src("X:~/b.ttf") && r.files.prose_italic == src("C:/f/plain.ttc", 3) &&
                  r.files.prose_bold_italic == src("C:/f/Inter-Regular.otf"),
              "resolve: explicit style values override the automatic pick");
    }
    {  // prose by path: styles use that file
        ResolvedFonts r = run({.prose = "C:\\p.ttf", .mono = "Mono Thing"}, &index);
        check(r.files.prose == src("X:C:\\p.ttf") && r.files.prose_bold == src("X:C:\\p.ttf") &&
                  r.files.mono == src("C:/f/mono.ttf"),
              "resolve: prose by path, mono by name");
    }
    {  // unknown names: built-in + one message each
        ResolvedFonts r = run({.prose = "Nope", .prose_bold = "Nope", .mono = "Zilch"}, &index);
        check(r.files == FontFiles{} && r.errors.size() == 2 && r.errors[0] == "font \"Nope\" not found" &&
                  r.errors[1] == "font \"Zilch\" not found",
              "resolve: unknown family -> built-in + error");
    }
    {  // the index isn't ready: built-in for now, no errors, waiting
        ResolvedFonts r = run({.prose = "Inter", .mono = "D:/m.ttf"}, nullptr);
        check(r.files.prose.builtin() && r.files.prose_bold.builtin() && r.files.mono == src("X:D:/m.ttf") &&
                  r.errors.empty() && r.waiting,
              "resolve: index not ready -> built-in, waiting, no error");
    }
}

// --- 3. the real scanner -------------------------------------------------------------------------

static void test_scanner() {
    const auto t0 = std::chrono::steady_clock::now();
    FontIndex idx;
    idx.start();
    const bool ok = idx.wait(30.0);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(ok, "font index finished");
    std::printf("     scanned %zu faces, %zu families in %.2f s\n", idx.faces().size(), idx.families().size(), secs);
    check(idx.faces().size() > 20, "font index found system fonts");

#ifdef _WIN32
    // Families every Windows 10/11 install has.
    for (const char* fam : {"Segoe UI", "Consolas", "Arial", "Courier New"}) {
        const FacePicks p = pick_faces(idx.faces(), fam);
        check(p.regular.has_value(), std::string("system family ") + fam + " found");
        if (p.regular)
            std::printf("     %-12s regular %s  bold %s\n", fam, face_id(*p.regular).c_str(),
                        p.bold ? face_id(*p.bold).c_str() : "none");
    }
    // Width rule: "Arial" must not pick Arial Narrow (same family name, width class 3).
    if (const FacePicks p = pick_faces(idx.faces(), "Arial"); p.regular)
        check(lower_name(fs::path(std::u8string(p.regular->file.begin(), p.regular->file.end())).filename().string()) == "arial.ttf",
              "Arial regular is arial.ttf (not Arial Narrow): got " + p.regular->file);
    auto family = [&](const char* name) -> const FontFamily* {
        for (const FontFamily& f : idx.families())
            if (same_family(f.name, name)) return &f;
        return nullptr;
    };
    if (const FontFamily* f = family("Consolas")) check(f->mono, "Consolas is listed as monospace");
    if (const FontFamily* f = family("Segoe UI")) check(!f->mono, "Segoe UI is not monospace");
    if (const FontFamily* f = family("Cascadia Code")) check(f->mono, "Cascadia Code is listed as monospace");
#else
    // Linux: whatever the box has; DejaVu Sans is the usual guaranteed one.
    if (const FacePicks p = pick_faces(idx.faces(), "DejaVu Sans"); p.regular)
        std::printf("     DejaVu Sans regular %s\n", face_id(*p.regular).c_str());
    auto family = [&](const char* name) -> const FontFamily* {
        for (const FontFamily& f : idx.families())
            if (same_family(f.name, name)) return &f;
        return nullptr;
    };
    if (const FontFamily* f = family("DejaVu Sans Mono")) check(f->mono, "DejaVu Sans Mono is listed as monospace");
    if (const FontFamily* f = family("DejaVu Sans")) check(!f->mono, "DejaVu Sans is not monospace");
#endif

#ifdef _WIN32
    // A .ttc face other than the first: the em ratio must come from that face.
    // Prefer Cambria Math (cambria.ttc #1), whose metrics differ a lot from Cambria's (#0).
    std::vector<FontFace> ttc_faces;
    for (const FontFace& f : idx.faces())
        if (f.face != 0) ttc_faces.push_back(f);
    std::stable_partition(ttc_faces.begin(), ttc_faces.end(), [](const FontFace& f) { return same_family(f.family, "Cambria Math"); });
    bool tested_ttc = false;
    for (const FontFace& f : ttc_faces) {
        const std::string bytes = read_file(fs::path(std::u8string(f.file.begin(), f.file.end())));
        const float em = font_em_ratio(bytes, f.face);
        check(em > 0, "font_em_ratio of " + face_id(f));
        if (same_family(f.family, "Cambria Math")) check(em != font_em_ratio(bytes, 0), "Cambria Math's em ratio is its own, not Cambria's");
        check(font_em_ratio(bytes, 9999) == 0, "font_em_ratio of a missing ttc face is 0");
        std::printf("     em ratio of %s (%s) = %.4f, face 0 = %.4f\n", face_id(f).c_str(), f.family.c_str(), em,
                    font_em_ratio(bytes, 0));
        tested_ttc = true;
        break;
    }
    check(tested_ttc, "found a .ttc with more than one face to test");
#endif
}

// Code ligatures: Cascadia's substitutions keep one glyph per character.
static void test_shaping() {
    auto cascadia = resource("fonts/CascadiaCode-Regular.ttf");
    check(cascadia.has_value(), "Cascadia Code is embedded");
    if (!cascadia) return;
    int font_id = 0;  // any address will do as the font's identity
    shaping::set_font(&font_id, *cascadia);

    // Shape `text`; true if one-to-one. changed[i]: character i got another glyph than its own.
    std::vector<uint32_t> shaped, nominal;
    auto shape = [&](const char* text, std::vector<bool>& changed) {
        const bool ok = shaping::shape_for_test(text, shaped, nominal);
        changed.assign(shaped.size(), false);
        for (size_t i = 0; i < shaped.size(); i++) changed[i] = shaped[i] != nominal[i];
        return ok;
    };
    std::vector<bool> ch;
    check(shape("->", ch) && shaped.size() == 2 && ch[0] && ch[1], "\"->\" becomes two ligature glyphs");
    check(shape("!=", ch) && shaped.size() == 2 && ch[0] && ch[1], "\"!=\" becomes a sign plus a spacer");
    check(shape("x->y", ch) && shaped.size() == 4 && !ch[0] && ch[1] && ch[2] && !ch[3], "\"x->y\": only the arrow changes");
    check(shape("fi", ch) && shaped.size() == 2 && !ch[0] && !ch[1], "\"fi\" stays two plain glyphs");
    check(shape("hello", ch) && !ch[0] && !ch[1] && !ch[2] && !ch[3] && !ch[4], "plain words are unchanged");
    check(shape("aéb", ch) && shaped.size() == 3, "a multi-byte character is one glyph");

    // The cached entry point: substituted characters only, nullptr when nothing changes.
    const std::vector<uint32_t>* g = shaping::glyphs(&font_id, "x->y");
    check(g && g->size() == 4 && (*g)[0] == 0 && (*g)[1] != 0 && (*g)[2] != 0 && (*g)[3] == 0, "glyphs(\"x->y\")");
    check(shaping::glyphs(&font_id, "hello") == nullptr, "glyphs(\"hello\") has nothing to substitute");
    check(shaping::glyphs(&font_id + 1, "->") == nullptr, "another font id is not shaped");

    // A face whose substitutions are not one-to-one: IBM Plex Sans joins "fi" into one glyph.
    if (auto plex = resource("fonts/IBMPlexSans-Regular.ttf")) {
        shaping::set_font(&font_id, *plex);
        const bool one_to_one = shaping::shape_for_test("fi", shaped, nominal);
        check(!one_to_one, "Plex \"fi\" is not one-to-one (falls back to plain drawing)");
        check(shaping::glyphs(&font_id, "fi") == nullptr, "glyphs() gives nothing for it");
    }
    shaping::set_font(nullptr, {});
}

int main() {
    const fs::path dir = CALLIGRUS_SPEC_TESTS_DIR;
    int n_files = 0, n_queries = 0;
    std::vector<fs::path> fixtures;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string n = e.path().filename().string();
        if (n.starts_with("fontpick-") && e.path().extension() == ".txt") fixtures.push_back(e.path());
    }
    std::sort(fixtures.begin(), fixtures.end());
    for (const fs::path& p : fixtures) {
        n_files++;
        n_queries += run_fixture(p);
    }
    check(n_files > 0, "found the shared fontpick-*.txt files");

    test_resolve();
    test_scanner();
    test_shaping();

    std::printf("fontpick %d files, %d queries; %d checks passed, %d failed\n", n_files, n_queries, passed, failures);
    std::printf("%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
