// The config file (spec section 5): ~/.config/calligrus/config.toml (or another
// place, see find_config_file).
//
// Reading uses toml++. Writing never rewrites the file: `config_set` edits
// one line in place (or appends the key at the end of its table), keeping
// comments, unknown keys and formatting byte for byte. So a hand-edited file
// stays the way the user wrote it.
#pragma once

#include <filesystem>
#include "userdir.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct Config {
    std::string theme = "auto";              // "auto" or a theme name
    std::string dark_theme = "calligrus-dark";  // used by "auto" when the system is dark
    std::string light_theme = "calligrus-light";
    double zoom = 1.0;       // 0.5 .. 3.0
    int body_size = 16;      // body text size in logical pixels at zoom 1.0 (12 .. 28)
    int max_width = 860;     // the widest the reading column gets, points at zoom 1.0 (400 .. 2400)
    bool show_outline = true;
    bool vim_keys = true;
    bool system_fallback = true;  // installed fonts for characters no bundled font has (spec 30)
    bool remember_window = true;  // keep window size/position between runs (state.toml)
    bool editor_marker = true;    // cursor marker in editor previews (spec 18); scroll sync is unaffected

    struct Fonts {
        // Each is "" (built-in), a file path, or a family name (spec 8a, font_index.h).
        std::string prose, prose_bold, prose_italic, prose_bold_italic, mono;
        bool operator==(const Fonts&) const = default;
    } fonts;

    bool code_line_numbers = false;  // [code] line_numbers

    bool operator==(const Config&) const = default;
};

constexpr int kBodySizeMin = 12, kBodySizeMax = 28;
constexpr int kMaxWidthMin = 400, kMaxWidthMax = 2400;

// Parse config.toml text. Never fails: a broken file gives all defaults, an
// invalid value the default for that key; each problem adds a warning.
Config parse_config(std::string_view text, std::vector<std::string>* warnings = nullptr);

// Every key with its resolved value, one `key = value` per line in a fixed
// order (TOML syntax, dotted keys for tables): what the shared tests'
// config-*.expected files contain.
std::string config_dump(const Config& c);

// --- the in-place writer ---------------------------------------------------------

// Set `key` (e.g. "zoom", "fonts.mono", "code.line_numbers") to `value`, a TOML
// value already formatted ("1.5", "true", "\"C:\\\\x.ttf\""). Returns the new
// file text: the line defining the key gets the new value (keeping its trailing
// comment and everything else byte for byte), or the key is appended at the
// end of its table, creating the [table] header if needed.
std::string config_set(std::string_view file_text, std::string_view key, std::string_view value);

// A complete, commented config.toml holding `c` (the layout of the spec's
// example file). Used when "Open config file" finds no file yet.
std::string config_file_text(const Config& c);

// TOML literals for config_set.
std::string toml_string(std::string_view s);  // "..." with escapes
std::string toml_bool(bool b);
std::string toml_float(double f);             // shortest form with a decimal point: 1.0, 1.5
std::string toml_int(int i);

// --- files ---------------------------------------------------------------------------

// Where config.toml is (spec 5, "Location"), the same on every OS: $CALLIGRUS_CONFIG (a file path) if
// set, else <XDG_CONFIG_HOME or ~/.config>/calligrus/config.toml (~ = %USERPROFILE% on Windows).
// The file is created there by the first setting change. See userdir.h, which also migrates the
// files of the old (mdview) versions.
//
// The inputs are parameters so the rules can be tested without touching the real environment
// (config_test.cpp). An empty path counts as "not set".
using ConfigEnv = userdir::Env;
std::filesystem::path find_config_file(const ConfigEnv& env);

// The real environment, read now.
ConfigEnv current_config_env();

// The config file in use, or where it would be created: find_config_file()
// with the current environment. Looked up again on every call (cheap), so a
// file created later in a higher-priority place is picked up.
std::filesystem::path config_path();
// The folder holding config_path() (its `themes` subfolder holds user themes).
std::filesystem::path config_dir();

// "~" (home) and %VARIABLES% expanded; "" stays "".
std::filesystem::path expand_path(std::string_view path);
