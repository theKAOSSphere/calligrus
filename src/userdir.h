// Where the user's files live (config.toml, state.toml, themes/): a folder "calligrus" in
// $XDG_CONFIG_HOME, ~/.config or (Windows) %APPDATA%. One that exists already wins (in that order);
// on first launch it goes to $XDG_CONFIG_HOME if set, else ~/.config if that folder exists (always
// on Linux), else %APPDATA%. ~ = %USERPROFILE% on Windows.
//
// Older versions of the app (then called "mdview") kept these files elsewhere
// (%APPDATA%\mdview\ on Windows, ~/.config/mdview/ on Linux). migrate_legacy() copies them
// over once, the first time the new folder does not exist. It never moves or deletes anything.
//
// Every input is a parameter of Env so the rules can be tested with temporary folders.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace userdir {

struct Env {
    std::filesystem::path calligrus_config;  // $CALLIGRUS_CONFIG (a file), else $MDVIEW_CONFIG
    std::filesystem::path calligrus_state;   // $CALLIGRUS_STATE (a file), else $MDVIEW_STATE
    std::filesystem::path xdg_config_home;   // $XDG_CONFIG_HOME
    std::filesystem::path xdg_state_home;    // $XDG_STATE_HOME (only read by the old versions)
    std::filesystem::path home;              // %USERPROFILE% (Windows) / $HOME
    std::filesystem::path appdata;           // %APPDATA% (Windows only; "" elsewhere)
    bool (*exists)(const std::filesystem::path&) = nullptr;  // for file lookups; nullptr = the real file system
};

// The real environment, read now.
Env current_env();

// $NAME, or $FALLBACK_NAME when NAME is not set ("" if neither is). Used for the CALLIGRUS_* variables,
// which still accept their old MDVIEW_* names.
std::string getenv_compat(const char* name, const char* old_name);

// The calligrus folder by the rules above ("" with no home folder at all).
std::filesystem::path config_home(const Env& e);
// $CALLIGRUS_CONFIG if set, else config_home/config.toml.
std::filesystem::path config_file(const Env& e);
// $CALLIGRUS_STATE if set, else config_home/state.toml.
std::filesystem::path state_file(const Env& e);

// The folders the old versions used for config.toml / themes/, best first.
std::vector<std::filesystem::path> legacy_dirs(const Env& e);

// Copy config.toml, state.toml and themes/ from the old location into config_home(), if
// config_home() does not exist yet and an old file does. Returns true if it copied anything.
bool migrate_legacy(const Env& e);

}  // namespace userdir
