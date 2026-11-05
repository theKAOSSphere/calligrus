// Link resolution, back/forward history and the state file (state.toml), kept
// free of ImGui and the window so they can be tested from the console
// (calligrus-session-test). main.cpp wires them to the UI.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace session {

// ---------------------------------------------------------------------------
// Heading anchors (same as the Rust port render.rs: slugify / slug_for)
// ---------------------------------------------------------------------------

// GitHub-style anchor: trimmed, lowercased, spaces to dashes, punctuation dropped.
// (Non-ASCII letters are kept as they are; only ASCII is lowercased.)
std::string slugify(std::string_view text);

// The anchor of each heading in document order: `foo`, `foo-1`, `foo-2`, ...
std::vector<std::string> unique_slugs(const std::vector<std::string>& titles);

// ---------------------------------------------------------------------------
// Links
// ---------------------------------------------------------------------------

struct LinkAction {
    enum class Kind {
        Anchor,        // `#anchor`: scroll in this document
        OpenMarkdown,  // an existing .md file (then jump to `anchor`, if any)
        OpenExternal,  // hand `url` / `path` to the shell (URLs, other existing files)
        Missing,       // relative path that doesn't exist
    } kind = Kind::OpenExternal;
    std::filesystem::path path;  // OpenMarkdown / OpenExternal (file) / Missing
    std::string anchor;          // slug (already slugified), may be empty
    std::string url;             // OpenExternal for URLs (path is empty then)
};

// Does the link start with a URL scheme (`https:`, `mailto:`)? A single letter
// before the colon is a drive (`C:\x`), not a scheme.
bool has_scheme(std::string_view url);

// What clicking `url` in a document living in `base_dir` should do.
LinkAction resolve_link(std::string_view url, const std::filesystem::path& base_dir);

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

struct Location {
    std::filesystem::path path;
    float scroll = 0;  // document points (scroll px / zoom scale)
};

// Browser-style history: the current location is passed in, as the app knows it.
class History {
public:
    static constexpr size_t kMax = 100;
    // Navigating somewhere new: remember where we were; the forward list is dropped.
    void push(const Location& current);
    // Step back / forward. nullopt = nothing there.
    std::optional<Location> back(const Location& current);
    std::optional<Location> forward(const Location& current);
    size_t back_size() const { return back_.size(); }
    size_t forward_size() const { return fwd_.size(); }

private:
    std::vector<Location> back_, fwd_;
};

// ---------------------------------------------------------------------------
// state.toml
// ---------------------------------------------------------------------------

struct WindowRect {
    int x = 0, y = 0, width = 0, height = 0;
    bool maximized = false;
};

struct ScrollEntry {
    std::string path;  // absolute, UTF-8
    double offset = 0;
};

// A vim-style mark (spec 28; marks.h): the top-level block at the top of the view, how far down
// into it (0..1), and the block's first words to find it again.
struct Mark {
    char name = 'a';   // a-z: this file only; A-Z: any file
    std::string path;  // absolute, UTF-8
    size_t block = 0;
    float frac = 0;
    std::string text;
};

struct State {
    static constexpr size_t kMaxScroll = 50;
    std::optional<WindowRect> window;
    std::vector<ScrollEntry> scroll;  // most recent first
    static constexpr size_t kMaxRecent = 10;
    static constexpr size_t kMaxTabs = 50;
    std::vector<std::string> recent;  // absolute UTF-8 paths, newest first
    std::vector<Mark> marks;          // newest first (spec 28)
    // The open tabs (spec 34): absolute UTF-8 paths, and the active one (1-based). Written only
    // while 2 or more tabs with a file are open; the apps fill them in just before saving.
    std::vector<std::string> tabs;
    size_t active_tab = 1;
    // Written even with one tab (Android: a killed process comes back to its document, spec 35). Not read or saved.
    bool keep_single_tab = false;
    // Top-level keys this version doesn't know (from a newer app or the Rust port),
    // as TOML text so saving doesn't drop them: plain values, then [tables].
    std::string extra_values, extra_tables;

    // Move `path` to the front of `recent` (case-insensitive, no duplicates, max 10).
    void add_recent(const std::filesystem::path& path);

    // Record `offset` for `path` (moved to the front; the oldest are dropped).
    void remember(const std::filesystem::path& path, double offset);
    std::optional<double> lookup(const std::filesystem::path& path) const;

    // Set a mark: it replaces the mark of that name (A-Z), or of that name in that file (a-z), and
    // goes to the front. `frac` is kept to 3 decimals (as written). (As session.rs.)
    void set_mark(Mark m);
    // Mark `name` as seen from file `path`: a global one wherever it is, a local one of this file.
    const Mark* mark(char name, const std::filesystem::path& path) const;
    void remove_mark(char name, const std::filesystem::path& path);
    // The marks in file `path` (its own and the global ones set in it).
    std::vector<const Mark*> marks_in(const std::filesystem::path& path) const;
};

// Where the file lives: $CALLIGRUS_STATE, else state.toml next to config.toml
// (~/.config/calligrus/state.toml; see userdir.h).
std::filesystem::path state_path();
// Broken / missing text gives the defaults.
State parse_state(std::string_view text);
std::string write_state(const State& s, bool with_window);
State load_state(const std::filesystem::path& p);
bool save_state(const std::filesystem::path& p, const State& s, bool with_window);

// A work area (monitor minus taskbar).
struct Area {
    int x, y, w, h;
};
// Is the window's title-bar strip (top 40 px) at least 100 px wide on some area?
bool title_bar_visible(const WindowRect& r, const std::vector<Area>& areas);

// ---------------------------------------------------------------------------
// "Open with" registration (Windows; HKCU only, no admin)
// ---------------------------------------------------------------------------

// Registers `exe` for .md/.markdown under HKCU\<classes_root> (the real one is
// `Software\Classes`; tests pass their own). Returns "" or an error message.
std::string register_app(const std::wstring& classes_root, const std::filesystem::path& exe, const std::wstring& friendly_name);
// Removes exactly what register_app added.
std::string unregister_app(const std::wstring& classes_root, const std::filesystem::path& exe);

}  // namespace session
