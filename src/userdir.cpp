#include "userdir.h"

#include <cstdlib>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace userdir {

#ifdef _WIN32
static fs::path env_path(const char* name) {
    std::wstring w;
    for (const char* c = name; *c; c++) w += wchar_t(*c);  // the names are ASCII
    DWORD n = GetEnvironmentVariableW(w.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring v(n, L'\0');
    v.resize(GetEnvironmentVariableW(w.c_str(), v.data(), n));
    return fs::path(v);
}
#else
static fs::path env_path(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return {};
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(v)));
}
#endif

static fs::path env_path_compat(const char* name, const char* old_name) {
    fs::path p = env_path(name);
    return p.empty() ? env_path(old_name) : p;
}

std::string getenv_compat(const char* name, const char* old_name) {
    fs::path p = env_path_compat(name, old_name);
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

Env current_env() {
    Env e;
    e.calligrus_config = env_path_compat("CALLIGRUS_CONFIG", "MDVIEW_CONFIG");
    e.calligrus_state = env_path_compat("CALLIGRUS_STATE", "MDVIEW_STATE");
    e.xdg_config_home = env_path("XDG_CONFIG_HOME");
    e.xdg_state_home = env_path("XDG_STATE_HOME");
    e.home = env_path("USERPROFILE");
    if (e.home.empty()) e.home = env_path("HOME");
    e.appdata = env_path("APPDATA");
    return e;
}

static bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

fs::path config_home(const Env& e) {
    // A calligrus folder that exists already wins, so the location never moves under the user
    // (e.g. when some other tool creates ~/.config later).
    for (const fs::path& base : {e.xdg_config_home, e.home.empty() ? fs::path() : e.home / ".config", e.appdata})
        if (!base.empty() && is_dir(base / "calligrus")) return base / "calligrus";
    // First launch: the XDG-style folder when this machine has one ($XDG_CONFIG_HOME set or a
    // ~/.config folder), else %APPDATA% (Windows). Without %APPDATA% (Linux), ~/.config regardless.
    if (!e.xdg_config_home.empty()) return e.xdg_config_home / "calligrus";
    if (!e.home.empty() && (e.appdata.empty() || is_dir(e.home / ".config"))) return e.home / ".config" / "calligrus";
    if (!e.appdata.empty()) return e.appdata / "calligrus";
    return {};
}

fs::path config_file(const Env& e) {
    if (!e.calligrus_config.empty()) return e.calligrus_config;
    const fs::path d = config_home(e);
    return d.empty() ? fs::path("config.toml") : d / "config.toml";  // (no home folder at all: last resort)
}

fs::path state_file(const Env& e) {
    if (!e.calligrus_state.empty()) return e.calligrus_state;
    const fs::path d = config_home(e);
    return d.empty() ? fs::path("state.toml") : d / "state.toml";
}

std::vector<fs::path> legacy_dirs(const Env& e) {
    std::vector<fs::path> v;
    if (!e.xdg_config_home.empty()) v.push_back(e.xdg_config_home / "mdview");
    if (!e.home.empty()) v.push_back(e.home / ".config" / "mdview");
    if (!e.appdata.empty()) v.push_back(e.appdata / "mdview");
    return v;
}

static bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

static bool copy_file_to(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::skip_existing, ec);
    return !ec;
}

bool migrate_legacy(const Env& e) {
    const fs::path dest = config_home(e);
    if (dest.empty() || is_dir(dest)) return false;  // nothing to do, or already migrated / in use

    bool copied = false;
    // config.toml and themes/ from the first old folder that has either.
    for (const fs::path& dir : legacy_dirs(e)) {
        if (!is_file(dir / "config.toml") && !is_dir(dir / "themes") && !is_file(dir / "state.toml")) continue;
        if (is_file(dir / "config.toml")) copied |= copy_file_to(dir / "config.toml", dest / "config.toml");
        if (is_dir(dir / "themes")) {
            std::error_code ec;
            fs::create_directories(dest / "themes", ec);
            fs::copy(dir / "themes", dest / "themes", fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
            copied |= !ec;
        }
        break;
    }
    // state.toml: next to the old config, else where the old versions kept it ($XDG_STATE_HOME or ~/.local/state).
    std::vector<fs::path> state_candidates;
    for (const fs::path& dir : legacy_dirs(e)) state_candidates.push_back(dir / "state.toml");
    if (!e.xdg_state_home.empty()) state_candidates.push_back(e.xdg_state_home / "mdview" / "state.toml");
    if (!e.home.empty()) state_candidates.push_back(e.home / ".local" / "state" / "mdview" / "state.toml");
    for (const fs::path& s : state_candidates)
        if (is_file(s)) {
            copied |= copy_file_to(s, dest / "state.toml");
            break;
        }
    return copied;
}

}  // namespace userdir
