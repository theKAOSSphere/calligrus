// calligrus-nav-test: console checks (no window) for the input logic of the
// zoom steps, vim keys, heading jumps, autoscroll and find
// matching. Exit code 0 = all passed.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "find.h"
#include "navigation.h"
#include "select.h"

using namespace nav;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += !ok;
}
static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

// --- vim key helpers ---------------------------------------------------------------

static KeyInput ch(char32_t c) { return KeyInput{.ch = c}; }
static KeyInput ctrl(char32_t c) { return KeyInput{.ch = c, .ctrl = true}; }
static KeyInput named(Named n, bool shift = false) { return KeyInput{.named = n, .shift = shift}; }

// Feed a string of typed characters at time `t` (each 0.1 s apart); returns the commands.
static std::vector<Command> type(VimKeys& v, const std::string& keys, double& t, bool vim = true) {
    std::vector<Command> out;
    for (char c : keys) {
        if (auto cmd = v.feed(ch(char32_t(c)), t, vim)) out.push_back(*cmd);
        t += 0.1;
    }
    return out;
}
static bool is(const std::vector<Command>& cmds, Action a, int count = 1) {
    return cmds.size() == 1 && cmds[0].action == a && cmds[0].count == count;
}

static void test_zoom() {
    check(near(clamp_zoom(1.0f), 1.0f), "zoom 1.0 stays");
    check(near(step_zoom(1.0f, 1), 1.1f) && near(step_zoom(1.0f, -1), 0.9f), "zoom steps of 0.1");
    float z = 1.0f;
    for (int i = 0; i < 7; i++) z = step_zoom(z, 1);
    check(near(z, 1.7f, 1e-6f), "seven steps in land exactly on 1.7");
    check(near(step_zoom(2.95f, 5), 3.0f) && near(step_zoom(0.6f, -5), 0.5f), "zoom clamped to 0.5 .. 3.0");
    {  // the zoomed picture (as zoom.rs's `lightbox` test)
        ImVec2 a = lightbox_size(2000, 1000, 1000, 700, false, 4);
        check(near(a.x, 896, 1e-3f) && near(a.y, 448, 1e-3f), "lightbox: a wide picture fills the width");
        ImVec2 b = lightbox_size(100, 1000, 1000, 700, true, 4);
        check(near(b.y, 564, 1e-3f) && near(b.x, 56.4f, 1e-3f), "lightbox: a tall one, with a caption");
        ImVec2 c = lightbox_size(50, 40, 1000, 700, false, 4);
        check(near(c.x, 200) && near(c.y, 160), "lightbox: at most 4x");
        ImVec2 d = lightbox_size(50, 40, 10, 10, false, 4);
        check(near(d.x, 1) && near(d.y, 1), "lightbox: never below 1");
    }
    check(near(clamp_zoom(NAN), 1.0f), "NaN zoom -> 1.0");
}

static void test_vim() {
    double t = 0;
    VimKeys v;
    check(is(type(v, "j", t), Action::LineDown), "j = line down");
    check(is(type(v, "k", t), Action::LineUp), "k = line up");
    check(is(type(v, "5j", t), Action::LineDown, 5), "5j = 5 lines down");
    check(is(type(v, "12k", t), Action::LineUp, 12), "12k");
    check(is(type(v, "1000j", t), Action::LineDown, 999), "count capped at 999");
    check(type(v, "0", t).empty() && v.pending().empty(), "a lone 0 does nothing");
    check(is(type(v, "10j", t), Action::LineDown, 10), "0 continues a count (10j)");
    check(is(type(v, "gg", t), Action::Top), "gg = top");
    check(is(type(v, "G", t), Action::Bottom), "G = bottom");
    check(is(type(v, "]]", t), Action::NextHeading), "]] = next heading");
    check(is(type(v, "[[", t), Action::PrevHeading), "[[ = previous heading");
    check(is(type(v, "3]]", t), Action::NextHeading, 3), "3]] = 3 headings down");
    check(is(type(v, "d", t), Action::HalfPageDown) && is(type(v, "u", t), Action::HalfPageUp), "d / u = half pages");
    check(is(type(v, "f", t), Action::PageDown) && is(type(v, "b", t), Action::PageUp), "f / b = pages");
    check(is(type(v, "/", t), Action::OpenFind) && is(type(v, "n", t), Action::NextMatch) &&
              is(type(v, "N", t), Action::PrevMatch),
          "/ n N");
    check(is(type(v, "o", t), Action::ToggleOutline) && is(type(v, "r", t), Action::Reload), "o r");
    check(is(type(v, "q", t), Action::Quit), "q quits");
    {
        // Marks and * # (spec 28; as vim.rs's marks_and_star)
        auto m = type(v, "ma'a", t);
        check(m.size() == 2 && m[0].action == Action::SetMark && m[0].mark == 'a' && m[1].action == Action::GoMark && m[1].mark == 'a', "ma 'a");
        m = type(v, "mZ'Z''", t);
        check(m.size() == 3 && m[0].mark == 'Z' && m[1].action == Action::GoMark && m[2].action == Action::HistoryBack, "mZ 'Z ''");
        check(is(type(v, "m1j", t), Action::LineDown) && is(type(v, "'.j", t), Action::LineDown), "m / ' + not a letter: dropped");
        m = type(v, "m'a", t);
        check(m.size() == 1 && m[0].action == Action::GoMark && m[0].mark == 'a', "' after m: a new prefix");
        m = type(v, "*#", t);
        check(m.size() == 2 && m[0].action == Action::FindWordNext && m[1].action == Action::FindWordPrev, "* #");
    }
    check(is(type(v, "gt", t), Action::NextTab) && is(type(v, "2gT", t), Action::PrevTab, 2) && type(v, "gt", t, false).empty(), "gt gT tabs");
    check(is(type(v, "t", t), Action::ThemePicker) && type(v, "t", t, false).empty() && type(v, "T", t).empty(), "t: theme picker (vim keys only), T nothing");
    check(is(type(v, "gt", t), Action::NextTab) && type(v, "3tj", t).size() == 2 && type(v, "gT", t).size() == 1, "gt after t still next tab; a count before t is dropped");
    check(is(type(v, "3H", t), Action::HistoryBack, 3) && is(type(v, "L", t), Action::HistoryForward), "3H L history");
    {
        auto c1 = v.feed(ctrl('e'), t, true), c2 = v.feed(ctrl('y'), t + 1, true);
        check(c1 && c1->action == Action::OneLineDown && c2 && c2->action == Action::OneLineUp, "Ctrl+E / Ctrl+Y");
        check(!v.feed(ctrl('e'), t + 2, false), "Ctrl+E off without vim keys");
        check(type(v, "qHL", t, false).empty(), "q H L off without vim keys");
    }

    // Prefix handling.
    check(type(v, "g", t).empty() && v.pending() == "g", "g waits for a second key");
    check(is(type(v, "j", t), Action::LineDown) && v.pending().empty(), "g then j: prefix dropped, j scrolls");
    check(type(v, "]", t).empty() && v.pending() == "]", "] alone does nothing yet");
    check(is(type(v, "[[", t), Action::PrevHeading), "] then [[: the second [ starts over");
    check(type(v, "5x", t).empty() && v.pending().empty(), "an unknown key cancels a count");
    check(is(type(v, "j", t), Action::LineDown, 1), "...so the next j is a single line");

    // Timeout: 1.5 s.
    type(v, "5", t);
    t += 1.6;
    check(is(type(v, "j", t), Action::LineDown, 1), "a count expires after 1.5 s");
    type(v, "g", t);
    t += 1.6;
    check(type(v, "g", t).empty() && v.pending() == "g", "a g prefix expires after 1.5 s (second g starts over)");
    v.reset();
    type(v, "4", t);
    t += 1.0;
    check(is(type(v, "j", t), Action::LineDown, 4), "...but not before");

    // Standard keys, with vim keys off too.
    for (bool vim : {true, false}) {
        const std::string tag = vim ? " (vim on)" : " (vim off)";
        auto one = [&](const KeyInput& k) { return v.feed(k, t += 0.1, vim); };
        auto o = one(named(Named::Down));
        check(o && o->action == Action::LineDown, "Down" + tag);
        o = one(named(Named::Space, true));
        check(o && o->action == Action::PageUp, "Shift+Space = page up" + tag);
        o = one(named(Named::End));
        check(o && o->action == Action::Bottom, "End" + tag);
        o = one(ctrl('f'));
        check(o && o->action == Action::OpenFind, "Ctrl+F" + tag);
        o = one(named(Named::Escape));
        check(o && o->action == Action::Escape, "Esc" + tag);
        o = one(ctrl('d'));
        check(vim ? (o && o->action == Action::HalfPageDown) : !o, "Ctrl+D only with vim keys" + tag);
    }
    check(type(v, "j", t, false).empty(), "j does nothing with vim keys off");
    type(v, "3", t);
    auto o = v.feed(named(Named::Down), t, true);
    check(o && o->action == Action::LineDown && o->count == 3, "a count applies to Down too (3<Down>)");
    type(v, "g", t);
    o = v.feed(named(Named::Escape), t, true);
    check(!o && v.pending().empty(), "Esc with keys pending only cancels them");
    o = v.feed(named(Named::Escape), t, true);
    check(o && o->action == Action::Escape, "...the next Esc acts");
}

static void test_headings() {
    const std::vector<float> tops = {0, 100, 250, 600};
    check(heading_target(tops, 0, 1) == 100.0f, "]] from the first heading -> second");
    check(heading_target(tops, 120, 1) == 250.0f, "]] from between headings -> the next one");
    check(heading_target(tops, 120, 2) == 600.0f, "2]] skips one");
    check(heading_target(tops, 120, 9) == 600.0f, "a big count stops at the last heading");
    check(!heading_target(tops, 600, 1), "]] at the last heading: nothing");
    check(heading_target(tops, 120, -1) == 100.0f, "[[ from between -> the heading above");
    check(heading_target(tops, 250, -1) == 100.0f, "[[ from a heading -> the previous one");
    check(heading_target(tops, 250, -5) == 0.0f, "a big count stops at the first heading");
    check(!heading_target(tops, 0, -1), "[[ at the top: nothing");
}

static void test_autoscroll() {
    check(autoscroll_velocity(0) == 0 && autoscroll_velocity(10) == 0 && autoscroll_velocity(-10) == 0, "dead zone of 10 px");
    check(near(autoscroll_velocity(110), std::pow(100.0f, 1.5f) * 0.6f, 0.01f), "velocity = (|dy| - 10)^1.5 * 0.6");
    check(near(autoscroll_velocity(-110), -600.0f, 0.01f), "negative above the anchor");
    check(autoscroll_velocity(5000) == 6000.0f && autoscroll_velocity(-5000) == -6000.0f, "capped at 6000 px/s");

    Autoscroll a;
    a.middle_pressed(100, 100, 0.0);
    check(a.active() && !a.toggle_mode(), "press starts autoscroll");
    check(near(a.scroll_delta(210, 0.5f), 300.0f, 0.01f), "time-based: half a second at 600 px/s = 300 px");
    a.middle_released(0.1);
    check(a.active() && a.toggle_mode(), "quick click without moving -> toggle mode");
    a.wheel();
    check(!a.active(), "wheel ends toggle mode");

    a.middle_pressed(100, 100, 1.0);
    a.mouse_moved(100, 130);
    a.middle_released(1.1);
    check(!a.active(), "moved >= 6 px before releasing -> hold mode, ends on release");
    a.middle_pressed(100, 100, 2.0);
    a.middle_released(2.4);
    check(!a.active(), "held longer than 250 ms -> hold mode, ends on release");

    a.middle_pressed(100, 100, 3.0);
    a.middle_released(3.05);
    a.other_button_pressed();
    check(!a.active(), "a click ends toggle mode");
    a.middle_pressed(100, 100, 4.0);
    a.middle_released(4.05);
    a.middle_pressed(100, 100, 4.5);
    check(!a.active(), "a middle click ends toggle mode (and doesn't restart)");
    a.middle_pressed(100, 100, 5.0);
    a.middle_released(5.05);
    a.escape();
    check(!a.active(), "Esc ends toggle mode");
    a.middle_pressed(100, 100, 6.0);
    a.wheel();
    check(a.active(), "wheel doesn't end hold mode");
    a.middle_released(6.5);
}

// --- find ------------------------------------------------------------------------------

// Runs for a tiny document, built the way the renderer reports them.
static std::vector<Run> sample_runs() {
    Collector c;
    c.begin_frame(std::nullopt);
    auto word = [&](const std::string& s, bool hard = false) {
        std::vector<float> xs;
        size_t n = to_utf32(s).size();
        for (size_t i = 0; i <= n; i++) xs.push_back(float(i * 10));
        c.push(ImVec2(0, 0), ImVec2(float(n * 10), 20), xs, s, hard);
    };
    c.begin_block(0);
    c.begin_group(Sep::Para);  // "Hello big World" wrapped after "big" (space swallowed)
    c.new_line();
    word("Hello");
    word(" ");
    word("big");
    c.new_line();
    word("World");
    c.begin_block(1);
    c.begin_group(Sep::Para);  // "hello $x$ hello"
    c.new_line();
    word("hello");
    word(" ");
    c.push(ImVec2(0, 0), ImVec2(10, 20), {0, 10}, "$hello$", false);  // an equation (atomic)
    word(" ");
    word("Ünïcode");
    return c.runs;
}

static void test_find() {
    check(!find_case_sensitive("hello") && find_case_sensitive("Hello") && find_case_sensitive("ünÏ"), "smartcase detection");
    check(fold_case(U'Ä') == U'ä' && fold_case(U'Ω') == U'ω' && fold_case(U'Ж') == U'ж' && fold_case(U'Ł') == U'ł',
          "case folding beyond ASCII");
    auto m = find_in_text(U"aaaa", U"aa", true);
    check(m.size() == 2 && m[1].first == 2, "matches don't overlap");

    const auto runs = sample_runs();
    auto hello = find_matches(runs, "hello");
    check(hello.size() == 2, "'hello' (smartcase) finds Hello and hello, not the equation's source");
    check(hello.size() == 2 && hello[0].first == TextPos{0, 0, 0} && hello[0].second == TextPos{0, 0, 5}, "first match = Hello");
    check(find_matches(runs, "Hello").size() == 1, "'Hello' is case-sensitive");
    auto across = find_matches(runs, "big world");
    check(across.size() == 1 && across[0].first == TextPos{0, 2, 0} && across[0].second == TextPos{0, 3, 5},
          "a match across a soft wrap (the swallowed space counts)");
    auto spaced = find_matches(runs, "hello big");
    check(spaced.size() == 1 && spaced[0].second == TextPos{0, 2, 3}, "a match across words and spaces");
    check(find_matches(runs, "nïc").size() == 1, "non-ASCII text");
    check(find_matches(runs, "ÜNÏ").empty() && find_matches(runs, "Ünï").size() == 1 && find_matches(runs, "ünï").size() == 1,
          "non-ASCII smartcase");
    check(find_matches(runs, "world hello").empty(), "matches never cross groups (paragraphs)");
    check(find_matches(runs, "").empty(), "empty query: no matches");

    // Highlights while painting.
    Collector c;
    c.begin_frame(std::nullopt, &hello, 1);
    c.begin_block(1);
    c.begin_group(Sep::Para);
    c.push(ImVec2(0, 0), ImVec2(50, 20), {0, 10, 20, 30, 40, 50}, "hello", false);
    check(c.find_highlights().size() == 1 && c.find_highlights()[0].current && c.find_highlights()[0].x1 == 50,
          "painted match is highlighted as the current one");
    c.push(ImVec2(50, 0), ImVec2(60, 20), {50, 60}, " ", false);
    check(c.find_highlights().empty(), "no highlight outside the match");
}

int main() {
    test_zoom();
    test_vim();
    test_headings();
    test_autoscroll();
    test_find();
    std::printf("%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
