// calligrus-config-test: runs EVERY shared test file in spec/tests
// (rules in their README.md, Python
// reference in check.py) against the C++ code, plus a few checks of the
// embedded built-in themes. Console only, no window. Exit code 0 = all passed.
//
//   config-*.toml + config-*.expected      config parsing   (config.cpp)
//   edit-*.toml + .ops + .expected-toml    in-place writer  (config.cpp)
//   theme-*.toml + theme-*.expected        theme parsing    (theme.cpp)
//   syntax-lookup.txt                      capture -> entry -> style (syntax_entries.cpp, theme.cpp)
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "config.h"
#include "syntax_entries.h"
#include "theme.h"

#ifndef CALLIGRUS_SPEC_TESTS_DIR
#error "CALLIGRUS_SPEC_TESTS_DIR must point at spec/tests (set by CMakeLists.txt)"
#endif
#ifndef CALLIGRUS_GRAMMARS_DIR
#error "CALLIGRUS_GRAMMARS_DIR must point at grammars/"
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

static std::vector<fs::path> files(const fs::path& dir, const std::string& prefix, const std::string& ext) {
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string name = e.path().filename().string();
        if (name.starts_with(prefix) && e.path().extension() == ext) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Show where two texts first differ.
static void show_diff(const std::string& expected, const std::string& actual) {
    size_t i = 0;
    while (i < expected.size() && i < actual.size() && expected[i] == actual[i]) i++;
    auto excerpt = [&](const std::string& s) {
        std::string out;
        for (size_t k = i > 20 ? i - 20 : 0; k < std::min(s.size(), i + 40); k++) {
            char c = s[k];
            out += c == '\n' ? std::string("\\n") : c == '\r' ? std::string("\\r") : c == '\t' ? std::string("\\t") : std::string(1, c);
        }
        return out;
    };
    std::printf("     first difference at byte %zu\n       expected: ...%s...\n       actual:   ...%s...\n", i,
                excerpt(expected).c_str(), excerpt(actual).c_str());
}

static void compare(const std::string& name, const std::string& expected, const std::string& actual) {
    check(expected == actual, name);
    if (expected != actual) show_diff(expected, actual);
}

// "#rrggbbaa", lower case, always 8 digits (the expected files' form).
static std::string hex8(ImU32 c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", unsigned((c >> IM_COL32_R_SHIFT) & 0xFF),
                  unsigned((c >> IM_COL32_G_SHIFT) & 0xFF), unsigned((c >> IM_COL32_B_SHIFT) & 0xFF),
                  unsigned((c >> IM_COL32_A_SHIFT) & 0xFF));
    return buf;
}

static std::string theme_dump(const Theme& t) {
    const std::map<std::string, ImU32> v = {
        {"ui.panel", t.ui.panel}, {"ui.border", t.ui.border}, {"ui.widget", t.ui.widget},
        {"ui.widget_hover", t.ui.widget_hover}, {"ui.widget_active", t.ui.widget_active}, {"ui.text", t.ui.text},
        {"ui.weak", t.ui.weak}, {"ui.field", t.ui.field}, {"ui.selection", t.ui.selection},
        {"ui.selection_text", t.ui.selection_text}, {"ui.accent", t.ui.accent},
        {"doc.bg", t.bg}, {"doc.text", t.text}, {"doc.weak", t.weak}, {"doc.link", t.link}, {"doc.heading", t.heading},
        {"doc.code_bg", t.code_bg}, {"doc.code_border", t.code_border}, {"doc.code_fg", t.code_fg}, {"doc.rule", t.rule},
        {"doc.quote_bar", t.quote_bar}, {"doc.table_head_bg", t.table_head_bg}, {"doc.zebra", t.zebra},
        {"doc.error", t.error}, {"doc.find_match", t.find_match}, {"doc.find_current", t.find_current},
        {"doc.heading_1", t.headings[0]}, {"doc.heading_2", t.headings[1]}, {"doc.heading_3", t.headings[2]},
        {"doc.heading_4", t.headings[3]}, {"doc.heading_5", t.headings[4]}, {"doc.heading_6", t.headings[5]},
        {"doc.bold", t.bold}, {"doc.italic", t.italic}, {"doc.quote", t.quote},
    };
    std::string s;
    for (const auto& [k, c] : v) s += k + " = " + hex8(c) + "\n";
    return s;
}

// An .ops value ("1.5", "'C:\x'", "true") as the canonical TOML text the writer writes.
static std::optional<std::string> canonical_value(const std::string& toml_value) {
    try {
        toml::table t = toml::parse("v = " + toml_value);
        const toml::node& n = *t.get("v");
        if (auto b = n.value_exact<bool>()) return toml_bool(*b);
        if (auto i = n.value_exact<int64_t>()) return std::to_string(*i);
        if (auto d = n.value_exact<double>()) return toml_float(*d);
        if (auto s = n.value_exact<std::string>()) return toml_string(*s);
    } catch (const toml::parse_error&) {
    }
    return std::nullopt;
}

// --- config file location (spec 5) -------------------------------------------------------
// find_config_file() / state_file() with a fake environment, and migrate_legacy() in a temporary folder.
static void test_config_location() {
    const fs::path home = "/h/me", xdg = "/x/cfg";
    struct Case {
        const char* what;
        bool explicit_file, xdg_set;  // which variables are set
        fs::path want;
    };
    const fs::path f_explicit = "/my/own.toml";
    const Case cases[] = {
        {"no XDG: ~/.config/calligrus", false, false, home / ".config" / "calligrus" / "config.toml"},
        {"XDG set: $XDG_CONFIG_HOME/calligrus", false, true, xdg / "calligrus" / "config.toml"},
        {"CALLIGRUS_CONFIG wins", true, true, f_explicit},
        {"CALLIGRUS_CONFIG wins, no XDG", true, false, f_explicit},
    };
    for (const Case& c : cases) {
        ConfigEnv env;
        if (c.explicit_file) env.calligrus_config = f_explicit;
        if (c.xdg_set) env.xdg_config_home = xdg;
        env.home = home;
        // no %APPDATA%: these are the Linux rules (Windows ones: test_location_by_existence)
        const fs::path got = find_config_file(env);
        check(got == c.want, std::string("config location: ") + c.what + " (got " + got.generic_string() + ")");
    }
    {
        ConfigEnv env;
        env.home = home;
        check(userdir::state_file(env) == home / ".config" / "calligrus" / "state.toml", "state file: next to config.toml");
        env.calligrus_state = "/s/state.toml";
        check(userdir::state_file(env) == fs::path("/s/state.toml"), "state file: CALLIGRUS_STATE wins");
    }
}

static void write_text(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

static std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Windows first launch: ~/.config/calligrus when ~/.config exists, else %APPDATA%\calligrus; an
// existing calligrus folder always wins (temporary folders, so the existence checks are real).
static void test_location_by_existence() {
    const fs::path t = fs::temp_directory_path() / "calligrus-config-test-where";
    std::error_code ec;
    fs::remove_all(t, ec);
    ConfigEnv env;
    env.home = t / "home";
    env.appdata = t / "appdata";
    fs::create_directories(env.home);
    fs::create_directories(env.appdata);
    check(userdir::config_home(env) == env.appdata / "calligrus", "where: no ~/.config -> %APPDATA%/calligrus");
    fs::create_directories(env.home / ".config");
    check(userdir::config_home(env) == env.home / ".config" / "calligrus", "where: ~/.config exists -> ~/.config/calligrus");
    fs::create_directories(env.appdata / "calligrus");
    check(userdir::config_home(env) == env.appdata / "calligrus", "where: an existing %APPDATA%/calligrus wins over a later ~/.config");
    fs::create_directories(env.home / ".config" / "calligrus");
    check(userdir::config_home(env) == env.home / ".config" / "calligrus", "where: with both, ~/.config/calligrus wins");
    ConfigEnv linux_env;
    linux_env.home = t / "no-config-folder";
    check(userdir::config_home(linux_env) == linux_env.home / ".config" / "calligrus", "where: no %APPDATA% (Linux) -> ~/.config even if missing");
    fs::remove_all(t, ec);
}

// Old (mdview) folders are copied to the new calligrus folder once; nothing is moved or deleted.
static void test_migration() {
    const fs::path root = fs::temp_directory_path() / "calligrus-config-test-migrate";
    auto fresh = [&](const char* name) {
        const fs::path d = root / name;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    };
    {  // Windows layout: the old %APPDATA%/mdview folder
        const fs::path t = fresh("win");
        ConfigEnv env;
        env.home = t / "home";
        env.appdata = t / "appdata";
        fs::create_directories(env.home / ".config");  // this user has a ~/.config folder
        write_text(env.appdata / "mdview" / "config.toml", "zoom = 1.5\n");
        write_text(env.appdata / "mdview" / "state.toml", "active_tab = 1\n");
        write_text(env.appdata / "mdview" / "themes" / "mine.toml", "name = \"mine\"\n");
        check(userdir::migrate_legacy(env), "migrate: copies from the old %APPDATA% folder");
        const fs::path dest = env.home / ".config" / "calligrus";
        check(read_text(dest / "config.toml") == "zoom = 1.5\n", "migrate: config.toml copied");
        check(read_text(dest / "state.toml") == "active_tab = 1\n", "migrate: state.toml copied");
        check(read_text(dest / "themes" / "mine.toml") == "name = \"mine\"\n", "migrate: themes/ copied");
        check(fs::exists(env.appdata / "mdview" / "config.toml") && fs::exists(env.appdata / "mdview" / "themes" / "mine.toml"),
              "migrate: the old files are left in place");
        write_text(dest / "config.toml", "zoom = 2.0\n");
        check(!userdir::migrate_legacy(env) && read_text(dest / "config.toml") == "zoom = 2.0\n", "migrate: second run does nothing");
    }
    {  // Linux layout: ~/.config/mdview, state in ~/.local/state/mdview
        const fs::path t = fresh("linux");
        ConfigEnv env;
        env.home = t / "home";
        write_text(env.home / ".config" / "mdview" / "config.toml", "zoom = 1.25\n");
        write_text(env.home / ".local" / "state" / "mdview" / "state.toml", "active_tab = 2\n");
        check(userdir::migrate_legacy(env), "migrate: copies from ~/.config/mdview");
        const fs::path dest = env.home / ".config" / "calligrus";
        check(read_text(dest / "config.toml") == "zoom = 1.25\n", "migrate: Linux config.toml copied");
        check(read_text(dest / "state.toml") == "active_tab = 2\n", "migrate: state.toml found in the old state folder");
    }
    {  // The new folder exists already: leave it alone.
        const fs::path t = fresh("exists");
        ConfigEnv env;
        env.home = t / "home";
        write_text(env.home / ".config" / "mdview" / "config.toml", "zoom = 1.25\n");
        fs::create_directories(env.home / ".config" / "calligrus");
        check(!userdir::migrate_legacy(env) && !fs::exists(env.home / ".config" / "calligrus" / "config.toml"),
              "migrate: an existing ~/.config/calligrus is never touched");
    }
    {  // Nothing old: nothing created.
        const fs::path t = fresh("none");
        ConfigEnv env;
        env.home = t / "home";
        check(!userdir::migrate_legacy(env) && !fs::exists(env.home / ".config" / "calligrus"), "migrate: nothing to copy, nothing created");
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

// Old built-in theme names keep working as aliases.
static void test_theme_aliases() {
    ThemeSet set;
    set.load({});
    check(set.find("mdview-dark") && set.find("mdview-dark")->name == "calligrus-dark", "theme alias: mdview-dark -> calligrus-dark");
    check(set.find("mdview-light") && set.find("mdview-light")->name == "calligrus-light", "theme alias: mdview-light -> calligrus-light");
    check(set.find("calligrus-light") && set.find("calligrus-light")->display_name == "Calligrus Light", "theme: display name");
}

int main() {
    test_config_location();
    test_location_by_existence();
    test_migration();
    test_theme_aliases();

    const fs::path dir = CALLIGRUS_SPEC_TESTS_DIR;
    if (!fs::is_directory(dir)) {
        std::printf("FAIL %s does not exist\n", dir.string().c_str());
        return 1;
    }

    // --- config parsing ------------------------------------------------------------
    int n_config = 0;
    for (const fs::path& p : files(dir, "config-", ".toml")) {
        n_config++;
        fs::path expected = p;
        expected.replace_extension(".expected");
        compare(p.filename().string(), read_file(expected), config_dump(parse_config(read_file(p))));
    }

    // --- in-place writer --------------------------------------------------------------
    int n_edit = 0;
    for (const fs::path& p : files(dir, "edit-", ".toml")) {
        n_edit++;
        fs::path ops = p, expected = p;
        ops.replace_extension(".ops");
        expected.replace_extension(".expected-toml");
        std::string text = read_file(p);
        std::istringstream lines(read_file(ops));
        std::string line;
        bool ops_ok = true;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line[first] == '#') continue;
            // set <key> <value>
            const size_t sp = line.find(' ', 4);
            auto value = line.starts_with("set ") && sp != std::string::npos ? canonical_value(line.substr(sp + 1)) : std::nullopt;
            if (!value) {
                ops_ok = false;
                std::printf("     bad op in %s: %s\n", ops.filename().string().c_str(), line.c_str());
                continue;
            }
            text = config_set(text, line.substr(4, sp - 4), *value);
        }
        check(ops_ok, ops.filename().string() + " parses");
        compare(p.filename().string(), read_file(expected), text);
    }

    // --- themes ------------------------------------------------------------------------
    ThemeSet set;
    set.load({});  // the embedded built-ins only
    for (const std::string& w : set.warnings) std::printf("     built-in theme warning: %s\n", w.c_str());
    check(set.warnings.empty(), "built-in themes load without warnings");
    check(set.find("calligrus-dark") && set.find("calligrus-light"), "calligrus-dark and calligrus-light are built in");
    check(set.all().size() >= 3, "at least 3 built-in themes (" + std::to_string(set.all().size()) + ")");
    const Theme& base_dark = set.default_theme(true);
    const Theme& base_light = set.default_theme(false);

    int n_theme = 0;
    for (const fs::path& p : files(dir, "theme-", ".toml")) {
        if (p.filename() == "theme-syntax.toml") continue;  // a helper for syntax-lookup.txt
        n_theme++;
        fs::path expected = p;
        expected.replace_extension(".expected");
        std::string err;
        auto t = parse_theme(read_file(p), &base_dark, &base_light, &err);
        check(t.has_value(), p.filename().string() + " parses" + (err.empty() ? "" : ": " + err));
        if (t) compare(p.filename().string(), read_file(expected), theme_dump(*t));
    }

    // --- syntax lookups ------------------------------------------------------------------
    const auto entries = parse_syntax_entries(read_file(fs::path(CALLIGRUS_GRAMMARS_DIR) / "theme.txt"));
    std::string err;
    auto syntax_test = parse_theme(read_file(dir / "theme-syntax.toml"), &base_dark, &base_light, &err);
    check(syntax_test.has_value(), "theme-syntax.toml parses");
    std::istringstream lookups(read_file(dir / "syntax-lookup.txt"));
    std::string line;
    int n_lookup = 0, lineno = 0;
    while (std::getline(lookups, line)) {
        lineno++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        // <theme> <capture> => <result>
        const size_t sp1 = line.find(' '), arrow = line.find(" => ");
        if (sp1 == std::string::npos || arrow == std::string::npos || arrow <= sp1) {
            check(false, "syntax-lookup.txt:" + std::to_string(lineno) + " is well-formed");
            continue;
        }
        n_lookup++;
        const std::string theme_name = line.substr(0, sp1), capture = line.substr(sp1 + 1, arrow - sp1 - 1),
                          want = line.substr(arrow + 4);
        const Theme* theme = syntax_test && theme_name == syntax_test->name ? &*syntax_test : set.find(theme_name);
        if (!theme) {
            check(false, "syntax-lookup.txt:" + std::to_string(lineno) + ": unknown theme " + theme_name);
            continue;
        }
        std::string got = "plain";
        if (auto e = find_syntax_entry(entries, capture); e && !entries[*e].plain)
            if (const SyntaxStyle* s = theme->syntax_for(entries[*e].name))
                got = hex8(s->color) + (s->bold ? " bold" : "") + (s->italic ? " italic" : "");
        check(got == want, "syntax-lookup.txt:" + std::to_string(lineno) + ": " + theme_name + " " + capture + " => " + want +
                               " (got " + got + ")");
    }

    // --- built-in themes: every theme.txt entry that isn't plain has a style ---------------
    for (const Theme& t : set.all())
        for (const SyntaxEntry& e : entries)
            if (!e.plain && !t.syntax.count(e.name)) check(false, t.name + " [syntax] lacks " + e.name);

    std::printf("config %d, edit %d, theme %d, syntax lookups %d files/lines; %d checks passed, %d failed\n", n_config,
                n_edit, n_theme, n_lookup, passed, failures);
    check(n_config > 0 && n_edit > 0 && n_theme > 0 && n_lookup > 0, "found the shared test files");
    std::printf("%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
