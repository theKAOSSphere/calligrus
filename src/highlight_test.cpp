// calligrus-highlight-test: the C++ side of syntax highlighting, no window.
// Compares against the golden files
// in grammars/tests.
// Exit code 0 = all passed.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "highlight.h"

#ifndef CALLIGRUS_GRAMMARS_DIR
#error "CALLIGRUS_GRAMMARS_DIR must point at spec/tests (set by cmake/highlight.cmake)"
#endif

namespace fs = std::filesystem;

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string without_cr(std::string s) {
    std::string out;
    for (char c : s)
        if (c != '\r') out += c;
    return out;
}

int main() {
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        failures += !ok;
    };

    // --- every query compiles, every language has a golden sample ---------------
    auto errors = highlight_check_all_languages();
    for (const auto& e : errors) std::printf("     %s\n", e.c_str());
    check(errors.empty(), "all queries compile");

    const fs::path tests = fs::path(CALLIGRUS_GRAMMARS_DIR) / "tests";
    std::set<std::string> sampled;
    int goldens = 0, golden_ok = 0;
    for (const auto& entry : fs::directory_iterator(tests)) {
        const fs::path p = entry.path();
        if (p.extension() == ".golden") continue;
        std::string file = p.filename().string();
        std::string lang = file.substr(0, file.find('.'));
        sampled.insert(lang);

        // --- golden files (shared with the Rust tests) -------------------------
        std::string code = read_file(p);
        std::vector<NamedSpan> spans;
        bool known = highlight_named(code, lang, spans);
        std::string actual = known ? highlight_golden_text(code, spans) : "(not highlighted)\n";
        std::string expected = without_cr(read_file(p.string() + ".golden"));
        goldens++;
        if (actual == expected) {
            golden_ok++;
        } else {
            // show the first differing line
            std::istringstream a(actual), e(expected);
            std::string la, le;
            int line = 0;
            while (true) {
                bool ga = bool(std::getline(a, la)), ge = bool(std::getline(e, le));
                line++;
                if (!ga && !ge) break;
                if (!ga || !ge || la != le) {
                    std::printf("     %s line %d:\n       got:      %s\n       expected: %s\n", file.c_str(), line,
                                ga ? la.c_str() : "(end)", ge ? le.c_str() : "(end)");
                    break;
                }
            }
            check(false, file + " matches its golden file");
        }

        // spans: ordered, in bounds, never inside a UTF-8 sequence
        bool sane = true;
        uint32_t last = 0;
        for (const CodeSpan& s : highlight_code(code, lang)) {
            auto boundary = [&](uint32_t at) { return at >= code.size() || (uint8_t(code[at]) & 0xC0) != 0x80; };
            sane = sane && s.start >= last && s.start < s.end && s.end <= code.size() && boundary(s.start) && boundary(s.end);
            last = s.end;
        }
        if (!sane) check(false, file + ": spans ordered, in bounds, on character boundaries");
    }
    check(golden_ok == goldens && goldens > 0,
          "golden files match (" + std::to_string(golden_ok) + "/" + std::to_string(goldens) + ")");
    bool all_sampled = true;
    for (const auto& name : highlight_language_names())
        if (!sampled.count(name)) {
            all_sampled = false;
            std::printf("     no sample for %s\n", name.c_str());
        }
    check(all_sampled, "every language has a golden sample");

    // --- the basics ------------------------------------------------------------
    const std::string code = "fn main() {\n    // greet\n    let s = \"hi\";\n}\n";
    const auto& spans = highlight_code(code, "rust");
    check(!spans.empty(), "rust code gets highlighted spans");
    const auto names = highlight_entry_names();
    auto entry_of = [&](const std::string& needle) -> std::string {
        size_t at = code.find(needle);
        for (const auto& s : spans)
            if (s.start <= at && at < s.end) return s.entry < names.size() ? names[s.entry] : "?";
        return "";
    };
    check(entry_of("fn") == "keyword" && entry_of("greet") == "comment" && entry_of("hi") == "string",
          "keyword, comment and string get their theme.txt entries");
    check(&highlight_code(code, "rust") == &spans, "results are cached");

    std::vector<NamedSpan> named;
    check(!highlight_named("x", "definitely-not-a-language", named), "unknown language -> plain");
    check(!highlight_named("x", "", named), "no language -> plain");
    check(!highlight_named("SELECT 1;", "sql", named), "sql is not compiled in");
    check(highlight_named("", "rust", named) && named.empty(), "empty code: known, no spans");
    check(highlight_code("x", "definitely-not-a-language").empty(), "unknown language -> no spans");
    for (const char* alias : {"py", "Rust", "rust,ignore", "c++", "ps1", "ahk", "yml", "tex", "el", "sh"})
        check(highlight_named("x = 1\n", alias, named), std::string("alias ") + alias + " is understood");

    std::string huge;
    while (huge.size() <= kHighlightMaxBytes) huge += "let x = 1;\n";
    check(!highlight_named(huge, "rust", named), "huge code -> plain");

    const std::string html = "<style>p { color: red; }</style>\n<script>const x = 1;</script>\n";
    highlight_named(html, "html", named);
    auto style_of = [&](const std::string& needle) -> std::string {
        size_t at = html.find(needle);
        for (const auto& s : named)
            if (s.start <= at && at < s.end) return s.style;
        return "";
    };
    check(style_of("const") == "keyword", "HTML <script> is highlighted as JavaScript");
    check(style_of("color") == "property", "HTML <style> is highlighted as CSS");

    std::printf("%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
