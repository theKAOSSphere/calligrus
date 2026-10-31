// calligrus-session-test: console checks (no window) for anchors, link
// resolution, history and the state file. Exit code 0 = all passed.
#include <cstdio>
#include <fstream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "session.h"

using namespace session;
namespace fs = std::filesystem;

// Test paths are written Windows-style; on Linux they become absolute POSIX paths (C:\x\y -> /x/y)
// so that absolute_path() leaves them alone.
static std::string tp(std::string s) {
#ifndef _WIN32
    if (s.size() > 2 && s[1] == ':') s.erase(0, 2);
    for (char& c : s) if (c == '\\') c = '/';
#endif
    return s;
}

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += !ok;
}

int main() {
    // --- slugify (the Rust port render.rs) ---
    check(slugify("Hello World") == "hello-world", "slug: spaces to dashes, lowercase");
    check(slugify("  Hello, World!  ") == "hello-world", "slug: trim, drop punctuation");
    check(slugify("foo_bar-baz") == "foo_bar-baz", "slug: keeps _ and -");
    check(slugify("C++ & Rust: 2 > 1") == "c--rust-2--1", "slug: each space is a dash");
    check(slugify("") == "", "slug: empty");
    check(slugify("\xC3\x9Cn\xC3\xAF Caf\xC3\x89 \xE2\x80\x94 Ok") == "\xC3\xBCn\xC3\xAF-caf\xC3\xA9--ok",
          "slug: Unicode lowercase, em dash dropped");
    check(slugify("\xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7 \xE6\x97\xA5\xE6\x9C\xAC") == "\xCE\xBA\xCE\xB1\xCE\xBB\xCE\xB7-\xE6\x97\xA5\xE6\x9C\xAC",
          "slug: Greek lowercased, CJK kept");
    check(slugify("Emoji \xF0\x9F\x8E\x89 test") == "emoji--test", "slug: emoji dropped");
    check((unique_slugs({"Foo", "Bar", "foo", "Foo!"}) == std::vector<std::string>{"foo", "bar", "foo-1", "foo-2"}),
          "unique slugs foo, foo-1, foo-2");

    // --- links ---
    const fs::path dir = fs::temp_directory_path() / "calligrus-session-test";
    fs::create_directories(dir);
    std::ofstream(dir / "a.md") << "# a\n";
    std::ofstream(dir / "B.MARKDOWN") << "# b\n";
    std::ofstream(dir / "x.txt") << "x\n";
    check(has_scheme("https://x.org") && has_scheme("mailto:a@b.c") && !has_scheme("C:\\x\\y.md") && !has_scheme("a.md#x:y"),
          "has_scheme");
    auto l = resolve_link("#My Heading", dir);
    check(l.kind == LinkAction::Kind::Anchor && l.anchor == "my-heading", "link: #anchor");
    l = resolve_link("https://x.org/a.md", dir);
    check(l.kind == LinkAction::Kind::OpenExternal && l.url == "https://x.org/a.md", "link: URL");
    l = resolve_link("a.md#Intro", dir);
    check(l.kind == LinkAction::Kind::OpenMarkdown && l.anchor == "intro" && l.path.filename() == "a.md", "link: md + anchor");
    l = resolve_link("B.MARKDOWN", dir);
    check(l.kind == LinkAction::Kind::OpenMarkdown, "link: .markdown, case-insensitive");
    l = resolve_link("x.txt", dir);
    check(l.kind == LinkAction::Kind::OpenExternal && l.path.filename() == "x.txt", "link: other file -> shell");
    l = resolve_link("nope.md", dir);
    check(l.kind == LinkAction::Kind::Missing, "link: missing file");
    fs::remove_all(dir);

    // --- history ---
    History h;
    check(!h.back({"cur", 0}), "history: empty back");
    h.push({"a", 1});
    h.push({"b", 2});
    auto b = h.back({"c", 3});
    check(b && b->path == "b" && b->scroll == 2, "history: back");
    auto f = h.forward({"b", 2});
    check(f && f->path == "c" && f->scroll == 3, "history: forward");
    h.back({"c", 3});
    h.push({"b", 2});  // new navigation after going back
    check(h.forward_size() == 0, "history: forward truncated");
    History cap;
    for (int i = 0; i < 150; i++) cap.push({"p", float(i)});
    check(cap.back_size() == 100, "history: capped at 100");

    // --- state file ---
    State s;
    s.window = WindowRect{100, 80, 1100, 1300, true};
    s.remember(tp("C:\\Share\\me\\notes\\a.md"), 1234.5);
    s.remember(tp("C:\\Share\\it's\\b.md"), 7);
    {
        // --- marks (spec 28; as session.rs's marks_round_trip_and_cpp_format) ---
        State m;
        m.set_mark({'a', tp("C:\\tmp\\a.md"), 3, 0.25f, "Setup \"quoted\""});
        m.set_mark({'a', tp("C:\\tmp\\b.md"), 0, 0.0f, "B"});
        m.set_mark({'Q', tp("C:\\tmp\\a.md"), 7, 0.33333f, ""});
        check(m.marks.size() == 3, "marks: a in two files, Q");
        check(m.mark('a', tp("C:\\tmp\\a.md")) && m.mark('a', tp("C:\\tmp\\a.md"))->block == 3, "marks: local lookup");
        check(m.mark('Q', tp("C:\\tmp\\b.md")) && m.mark('Q', tp("C:\\tmp\\b.md"))->block == 7, "marks: global from anywhere");
        check(!m.mark('b', tp("C:\\tmp\\a.md")) && m.marks_in(tp("C:\\tmp\\a.md")).size() == 2, "marks: missing, marks_in");
        m.set_mark({'Q', tp("C:\\tmp\\b.md"), 1, 0.5f, "B"});
        check(m.marks.size() == 3 && m.marks_in(tp("C:\\tmp\\a.md")).size() == 1, "marks: a global mark moves");
        const std::string mt = write_state(m, false);
        check(mt.find("\n[[mark]]\nname = 'Q'\npath = ") != std::string::npos &&
                  mt.find("block = 3\nfrac = 0.250\ntext = \"Setup \\\"quoted\\\"\"\n") != std::string::npos,
              "marks: written as session.rs writes them");
        const State back = parse_state(mt);
        bool same = back.marks.size() == m.marks.size();
        for (size_t i = 0; same && i < m.marks.size(); i++)
            same = back.marks[i].name == m.marks[i].name && back.marks[i].path == m.marks[i].path && back.marks[i].block == m.marks[i].block &&
                   back.marks[i].frac == m.marks[i].frac && back.marks[i].text == m.marks[i].text;
        check(same, "marks: round trip");
        const State bad = parse_state(
            "[[mark]]\nname = 'ab'\npath = 'x'\nblock = 1\nfrac = 0.5\n[[mark]]\nname = '1'\npath = 'x'\nblock = 1\nfrac = 0.5\n"
            "[[mark]]\nname = 'c'\npath = 'x'\nblock = -1\nfrac = 0.5\n[[mark]]\nname = 'd'\npath = 'x'\nblock = 1\nfrac = 2\n"
            "[[mark]]\nname = 'e'\npath = 'x'\nblock = 1\nfrac = 1\n");
        check(bad.marks.size() == 1 && bad.marks[0].name == 'e' && bad.marks[0].frac == 1.0f && bad.marks[0].text.empty(), "marks: bad entries skipped");
    }
    const std::string text = write_state(s, true);
    State r = parse_state(text);
    check(r.window && r.window->x == 100 && r.window->width == 1100 && r.window->maximized, "state: window round trip");
    check(r.scroll.size() == 2 && r.scroll[0].path == tp("C:\\Share\\it's\\b.md") && r.scroll[1].offset == 1234.5, "state: scroll round trip (quote in path)");
    check(write_state(s, false).find("[window]") == std::string::npos, "state: no window table when remember_window is off");
    check(parse_state("this is [ not toml").scroll.empty(), "state: broken file -> defaults");
    check(!parse_state("[window]\nx = 1\n").window, "state: incomplete window ignored");
    s.remember(tp("c:\\share\\ME\\notes\\A.md"), 5);
#ifdef _WIN32
    check(s.scroll.size() == 2 && s.scroll[0].offset == 5, "state: paths compare case-insensitively");
    check(s.lookup(tp("C:\\Share\\me\\notes\\a.md")) == 5.0, "state: lookup");
#else
    check(s.scroll.size() == 3 && s.scroll[0].offset == 5, "state: paths differing in case stay distinct (Linux)");
    check(s.lookup(tp("c:\\share\\ME\\notes\\A.md")) == 5.0 && s.lookup(tp("C:\\Share\\me\\notes\\a.md")) != 5.0, "state: lookup is case-sensitive");
#endif
    State many;
    for (int i = 0; i < 60; i++) many.remember(tp("C:\\f") + std::to_string(i) + ".md", i);
    check(many.scroll.size() == 50 && many.scroll[0].offset == 59, "state: 50 entries, newest first");

    // --- recent files ---
    State rec;
    rec.add_recent(tp("C:\\a.md"));
    rec.add_recent(tp("C:\\b.md"));
    rec.add_recent(tp("c:\\A.MD"));
#ifdef _WIN32
    check(rec.recent.size() == 2 && rec.recent[0] == tp("c:\\A.MD") && rec.recent[1] == tp("C:\\b.md"), "recent: newest first, case-insensitive dedupe");
#else
    check(rec.recent.size() == 3 && rec.recent[0] == tp("c:\\A.MD"), "recent: newest first, names differing in case stay distinct (Linux)");
#endif
    for (int i = 0; i < 15; i++) rec.add_recent(tp("C:\\r") + std::to_string(i) + ".md");
    check(rec.recent.size() == 10 && rec.recent[0] == tp("C:\\r14.md"), "recent: capped at 10");
    rec.window = WindowRect{1, 2, 300, 400, false};
    const std::string rtext = write_state(rec, true);
    check(rtext.find("recent") < rtext.find("[window]"), "recent: written before [window]");
    check(parse_state(rtext).recent == rec.recent, "recent: round trip");
    rec.recent.clear();
    check(write_state(rec, true).find("recent") == std::string::npos, "recent: omitted when empty");

#ifdef _WIN32
    // --- "Open with" registration, against a scratch root (never Software\\Classes) ---
    {
        const std::wstring root = L"Software\\calligrus-test\\Classes";
        const fs::path exe = "C:\\tools\\calligrus-x.exe";
        auto has_key = [&](const std::wstring& k) {
            HKEY h;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, (root + L"\\" + k).c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
            RegCloseKey(h);
            return true;
        };
        auto has_value = [&](const std::wstring& k, const wchar_t* v) {
            return RegGetValueW(HKEY_CURRENT_USER, (root + L"\\" + k).c_str(), v, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        };
        check(register_app(root, exe, L"calligrus").empty(), "register: ok");
        check(has_key(L"calligrus-x.markdown\\shell\\open\\command") && has_key(L"calligrus-x.markdown\\DefaultIcon"), "register: ProgId keys");
        check(has_value(L".md\\OpenWithProgids", L"calligrus-x.markdown") && has_value(L".markdown\\OpenWithProgids", L"calligrus-x.markdown"),
              "register: OpenWithProgids");
        check(has_value(L"Applications\\calligrus-x.exe", L"FriendlyAppName") && has_value(L"Applications\\calligrus-x.exe\\SupportedTypes", L".md"),
              "register: Applications entry");
        check(unregister_app(root, exe).empty() && !has_key(L"calligrus-x.markdown") && !has_key(L"Applications\\calligrus-x.exe") &&
                  !has_value(L".md\\OpenWithProgids", L"calligrus-x.markdown"),
              "unregister: removes what register added");
        check(unregister_app(root, exe).empty(), "unregister: missing keys are fine");
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\calligrus-test");
    }
#endif

    // --- file on disk, atomic write ---
    const fs::path sp = fs::temp_directory_path() / "calligrus-session-test-state" / "sub" / "state.toml";
    check(save_state(sp, s, true) && load_state(sp).scroll.size() == s.scroll.size(), "state: save creates dirs, load reads back");
    check(load_state(sp.parent_path() / "missing.toml").scroll.empty(), "state: missing file -> defaults");
    fs::remove_all(sp.parent_path().parent_path());

    // --- window placement ---
    const std::vector<Area> mons = {{0, 0, 1920, 1040}, {1920, 0, 1280, 1000}};
    check(title_bar_visible({100, 80, 1100, 800, false}, mons), "rect: on monitor 1");
    check(title_bar_visible({2000, 50, 900, 700, false}, mons), "rect: on monitor 2");
    check(!title_bar_visible({4000, 50, 900, 700, false}, mons), "rect: off every monitor");
    check(!title_bar_visible({100, 1200, 900, 700, false}, mons), "rect: title bar below the screen");
    check(!title_bar_visible({-850, 50, 900, 700, false}, mons), "rect: only 50 px visible");

    {
        // Keys this version doesn't know survive a save (plain values above the first table).
        const char* text = "future = 3\nrecent = ['a.md']\n[window]\nx = 1\ny = 2\nwidth = 30\nheight = 40\nmaximized = false\n\n"
                           "[tabs]\nopen = ['a.md', 'b.md']\n\n[[marks]]\nname = 'x'\n";
        State s = parse_state(text);
        std::string out = write_state(s, true);
        State back = parse_state(out);
        check(back.recent == s.recent && back.window.has_value(), "unknown keys: known ones still read");
        check(out.find("future = 3") < out.find("[window]"), "unknown keys: plain value above [window]");
        check(out.find("[tabs]") != std::string::npos && out.find("[[marks]]") != std::string::npos &&
                  back.extra_tables == s.extra_tables && back.extra_values == s.extra_values,
              "unknown keys: tables kept, round trip stable");
    }

    {
        // Tabs (spec 34): written after `recent`, only with 2 or more; active_tab is 1-based.
        State t;
        t.recent = {"r.md"};
        t.tabs = {"C:\\a.md", "it's.md"};
        t.active_tab = 2;
        t.window = WindowRect{1, 2, 300, 400, false};
        const std::string out = write_state(t, true);
        check(out.find("recent = ['r.md']\ntabs = ['C:\\a.md', \"it's.md\"]\nactive_tab = 2\n") != std::string::npos, "tabs: format, after recent");
        check(out.find("active_tab") < out.find("[window]"), "tabs: before the first table");
        const State back = parse_state(out);
        check(back.tabs == t.tabs && back.active_tab == 2 && back.extra_values.empty(), "tabs: round trip, known keys");
        t.tabs = {"only.md"};
        check(write_state(t, true).find("tabs") == std::string::npos, "tabs: one tab is not written");
        check(parse_state("tabs = ['a', 5, '', 'b']\nactive_tab = 9\n").tabs.size() == 2 && parse_state("tabs = ['a']\nactive_tab = 9\n").active_tab == 1,
              "tabs: bad entries skipped, active_tab clamped");
        const State known = parse_state("tabs = ['a','b']\nactive_tab = 2\nfuture = 1\n");
        check(known.extra_values.find("future = 1") != std::string::npos && known.extra_values.find("tabs") == std::string::npos &&
                  known.extra_values.find("active_tab") == std::string::npos,
              "tabs: not unknown keys");
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
