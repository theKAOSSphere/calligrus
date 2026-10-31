#include "session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <toml++/toml.hpp>

#include "marks.h"
#include "platform.h"
#include "userdir.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <cwctype>
#else
#include <clocale>
#include <cwctype>
#include <locale.h>
#include <wctype.h>
#endif

namespace fs = std::filesystem;

namespace session {

// --- anchors -----------------------------------------------------------------

// Like Rust's char::to_lowercase / is_alphanumeric / is_whitespace, for the
// Basic Multilingual Plane (Win32 does the Unicode tables). Characters outside
// it (mostly emoji) are dropped, as Rust drops them.
static void slug_char(char32_t c, std::string& out) {
    auto put = [&out](char32_t c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    };
    if (c == '-' || c == '_') { put(c); return; }
    if (c > 0xFFFF) return;
#ifdef _WIN32
    wchar_t w = wchar_t(c);
    if (IsCharAlphaNumericW(w)) { CharLowerBuffW(&w, 1); put(char32_t(w)); }
    else if (iswspace(w)) put('-');
#else
    // glibc's Unicode tables, through a UTF-8 locale object (independent of the process locale).
    static const locale_t loc = newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
    if (loc) {
        if (iswalnum_l(wint_t(c), loc)) put(char32_t(towlower_l(wint_t(c), loc)));
        else if (iswspace_l(wint_t(c), loc)) put('-');
    } else if (c >= 'A' && c <= 'Z') put(c - 'A' + 'a');
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) put(c);
    else if (c == ' ' || (c >= '\t' && c <= '\r')) put('-');
#endif
}

std::string slugify(std::string_view text) {
    auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
    while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
    std::string out;
    for (size_t i = 0; i < text.size();) {
        // decode one UTF-8 sequence (invalid bytes are skipped)
        const unsigned char b = static_cast<unsigned char>(text[i]);
        const int len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + len > text.size()) { ++i; continue; }
        char32_t c = len == 1 ? b : b & (0x7F >> len);
        for (int k = 1; k < len; ++k) c = (c << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        slug_char(c, out);
        i += len;
    }
    return out;
}

std::vector<std::string> unique_slugs(const std::vector<std::string>& titles) {
    std::map<std::string, int> seen;
    std::vector<std::string> out;
    for (const std::string& t : titles) {
        const std::string base = slugify(t);
        int& n = seen[base];
        out.push_back(n == 0 ? base : base + "-" + std::to_string(n));
        n++;
    }
    return out;
}

// --- links -------------------------------------------------------------------

bool has_scheme(std::string_view url) {
    size_t i = 0;
    while (i < url.size() && ((url[i] >= 'a' && url[i] <= 'z') || (url[i] >= 'A' && url[i] <= 'Z') ||
                              (i > 0 && ((url[i] >= '0' && url[i] <= '9') || url[i] == '+' || url[i] == '-' || url[i] == '.'))))
        i++;
    return i >= 2 && i < url.size() && url[i] == ':';
}

static bool is_markdown(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".md" || ext == ".markdown" || ext == ".org";
}

LinkAction resolve_link(std::string_view url, const fs::path& base_dir) {
    LinkAction a;
    if (!url.empty() && url[0] == '#') {
        a.kind = LinkAction::Kind::Anchor;
        a.anchor = slugify(url.substr(1));
        return a;
    }
    if (url.empty() || has_scheme(url)) {
        a.kind = LinkAction::Kind::OpenExternal;
        a.url = std::string(url);
        return a;
    }
    const size_t hash = url.find('#');
    const std::string_view file = url.substr(0, hash);
    if (hash != std::string_view::npos) a.anchor = slugify(url.substr(hash + 1));
    a.path = base_dir / fs::path(std::u8string(file.begin(), file.end()));
    std::error_code ec;
    if (!fs::is_regular_file(a.path, ec)) a.kind = LinkAction::Kind::Missing;
    else a.kind = is_markdown(a.path) ? LinkAction::Kind::OpenMarkdown : LinkAction::Kind::OpenExternal;
    return a;
}

// --- history -----------------------------------------------------------------

void History::push(const Location& current) {
    back_.push_back(current);
    if (back_.size() > kMax) back_.erase(back_.begin());
    fwd_.clear();
}

std::optional<Location> History::back(const Location& current) {
    if (back_.empty()) return std::nullopt;
    Location l = back_.back();
    back_.pop_back();
    fwd_.push_back(current);
    return l;
}

std::optional<Location> History::forward(const Location& current) {
    if (fwd_.empty()) return std::nullopt;
    Location l = fwd_.back();
    fwd_.pop_back();
    back_.push_back(current);
    return l;
}

// --- state file ----------------------------------------------------------------

static std::string to_u8(const fs::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

static fs::path from_u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

static fs::path absolute_path(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    return (ec ? p : abs).lexically_normal();
}

// Paths are compared as absolute strings, case-insensitive on Windows only (like the Rust app).
static std::string path_key(const fs::path& p) {
    std::string s = to_u8(absolute_path(p));
#ifdef _WIN32
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#endif
    return s;
}

void State::remember(const fs::path& path, double offset) {
    if (path.empty()) return;
    const std::string key = path_key(path);
    std::erase_if(scroll, [&](const ScrollEntry& e) { return path_key(from_u8(e.path)) == key; });
    scroll.insert(scroll.begin(), ScrollEntry{to_u8(absolute_path(path)), offset});
    if (scroll.size() > kMaxScroll) scroll.resize(kMaxScroll);
}

void State::add_recent(const fs::path& path) {
    if (path.empty()) return;
    const fs::path abs = absolute_path(path);
    const std::string key = path_key(abs);
    std::erase_if(recent, [&](const std::string& r) { return path_key(from_u8(r)) == key; });
    recent.insert(recent.begin(), to_u8(abs));
    if (recent.size() > kMaxRecent) recent.resize(kMaxRecent);
}

std::optional<double> State::lookup(const fs::path& path) const {
    const std::string key = path_key(path);
    for (const ScrollEntry& e : scroll)
        if (path_key(from_u8(e.path)) == key) return e.offset;
    return std::nullopt;
}

void State::set_mark(Mark m) {
    m.path = to_u8(absolute_path(from_u8(m.path)));
    m.frac = std::clamp(std::round(std::clamp(m.frac, 0.0f, 1.0f) * 1000.0f) / 1000.0f, 0.0f, 1.0f);
    remove_mark(m.name, from_u8(m.path));
    marks.insert(marks.begin(), std::move(m));
    if (marks.size() > marks::kMaxMarks) marks.resize(marks::kMaxMarks);
}

const Mark* State::mark(char name, const fs::path& path) const {
    const std::string key = path_key(path);
    for (const Mark& m : marks)
        if (m.name == name && (marks::is_global(char32_t(name)) || path_key(from_u8(m.path)) == key)) return &m;
    return nullptr;
}

void State::remove_mark(char name, const fs::path& path) {
    const std::string key = path_key(path);
    std::erase_if(marks, [&](const Mark& m) { return m.name == name && (marks::is_global(char32_t(name)) || path_key(from_u8(m.path)) == key); });
}

std::vector<const Mark*> State::marks_in(const fs::path& path) const {
    const std::string key = path_key(path);
    std::vector<const Mark*> out;
    for (const Mark& m : marks)
        if (path_key(from_u8(m.path)) == key) out.push_back(&m);
    return out;
}

static std::string env(const char* name) {
#pragma warning(suppress : 4996)
    const char* v = std::getenv(name);
    return v ? v : "";
}

fs::path state_path() { return userdir::state_file(userdir::current_env()); }

State parse_state(std::string_view text) {
    State s;
    toml::table root;
    try {
        root = toml::parse(text);
    } catch (const toml::parse_error&) {
        return s;
    }
    if (const toml::array* arr = root["recent"].as_array())
        for (const toml::node& n : *arr)
            if (auto p = n.value<std::string>(); p && !p->empty() && s.recent.size() < State::kMaxRecent) s.recent.push_back(*p);
    if (const toml::array* arr = root["tabs"].as_array())
        for (const toml::node& n : *arr)
            if (auto p = n.value<std::string>(); p && !p->empty() && s.tabs.size() < State::kMaxTabs) s.tabs.push_back(*p);
    if (auto n = root["active_tab"].value<int64_t>(); n && *n >= 1 && size_t(*n) <= s.tabs.size()) s.active_tab = size_t(*n);
    if (const toml::table* w = root["window"].as_table()) {
        auto x = (*w)["x"].value<int64_t>(), y = (*w)["y"].value<int64_t>();
        auto width = (*w)["width"].value<int64_t>(), height = (*w)["height"].value<int64_t>();
        if (x && y && width && height && *width > 0 && *height > 0 && *width < 100000 && *height < 100000 &&
            std::abs(*x) < 1000000 && std::abs(*y) < 1000000) {
            s.window = WindowRect{int(*x), int(*y), int(*width), int(*height), (*w)["maximized"].value_or(false)};
        }
    }
    if (const toml::array* arr = root["scroll"].as_array())
        for (const toml::node& n : *arr)
            if (const toml::table* t = n.as_table()) {
                auto path = (*t)["path"].value<std::string>();
                auto off = (*t)["offset"].value<double>();
                if (!off)
                    if (auto i = (*t)["offset"].value<int64_t>()) off = double(*i);
                if (path && off && !path->empty() && *off >= 0 && s.scroll.size() < State::kMaxScroll)
                    s.scroll.push_back({*path, *off});
            }
    if (const toml::array* arr = root["mark"].as_array())
        for (const toml::node& n : *arr)
            if (const toml::table* t = n.as_table()) {
                auto name = (*t)["name"].value<std::string>();
                auto path = (*t)["path"].value<std::string>();
                auto block = (*t)["block"].value<int64_t>();
                auto frac = (*t)["frac"].value<double>();
                if (!frac)
                    if (auto i = (*t)["frac"].value<int64_t>()) frac = double(*i);
                if (name && name->size() == 1 && marks::is_name(char32_t((*name)[0])) && path && !path->empty() && block && *block >= 0 &&
                    *block < 1000000000 && frac && *frac >= 0 && *frac <= 1 && s.marks.size() < marks::kMaxMarks &&
                    !s.mark((*name)[0], from_u8(*path)))  // (a second mark of the same name and file: the first one wins)
                    s.marks.push_back({(*name)[0], *path, size_t(*block), float(*frac), (*t)["text"].value_or(std::string())});
            }
    // Unknown keys, split so plain values can go above the first [table].
    toml::table values, tables;
    for (auto&& [k, v] : root) {
        if (k == "recent" || (k == "tabs" && v.is_array()) || (k == "active_tab" && v.is_integer()) || k == "window" || k == "scroll" || k == "mark") continue;
        const bool section = v.is_table() || v.is_array_of_tables();
        (section ? tables : values).insert(k, v);
    }
    auto text_of = [](const toml::table& t) {
        if (t.empty()) return std::string();
        std::ostringstream o;
        o << toml::toml_formatter(t, toml::format_flags::none);
        return o.str() + "\n";
    };
    s.extra_values = text_of(values);
    s.extra_tables = text_of(tables);
    return s;
}

static std::string toml_str(const std::string& s) {
    // A literal string, unless it holds a ' (or a newline): then a basic one.
    if (s.find('\'') == std::string::npos && s.find('\n') == std::string::npos) return "'" + s + "'";
    std::string o = "\"";
    for (char c : s) {
        if (c == '\n') o += "\\n";
        else {
            if (c == '\\' || c == '"') o += '\\';
            o += c;
        }
    }
    return o + "\"";
}

// A TOML basic string, escaped as config.rs's toml_string (so both apps write the same bytes).
static std::string toml_basic(const std::string& s) {
    std::string o = "\"";
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '"': o += "\\\""; break;
            case '\b': o += "\\b"; break;
            case '\t': o += "\\t"; break;
            case '\n': o += "\\n"; break;
            case '\f': o += "\\f"; break;
            case '\r': o += "\\r"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char u[8];
                    std::snprintf(u, sizeof(u), "\\u%04X", unsigned(c));
                    o += u;
                } else {
                    o += ch;
                }
        }
    }
    return o + "\"";
}

std::string write_state(const State& s, bool with_window) {
    std::string t = "# Calligrus state (written by the app; safe to delete)\n";
    // Top-level keys must come before the first [table].
    if (!s.recent.empty()) {
        t += "recent = [";
        for (size_t i = 0; i < s.recent.size(); ++i) t += (i ? ", " : "") + toml_str(s.recent[i]);
        t += "]\n";
    }
    if (s.tabs.size() >= 2) {  // (spec 34)
        t += "tabs = [";
        for (size_t i = 0; i < s.tabs.size(); ++i) t += (i ? ", " : "") + toml_str(s.tabs[i]);
        t += "]\nactive_tab = " + std::to_string(std::clamp<size_t>(s.active_tab, 1, s.tabs.size())) + "\n";
    }
    t += s.extra_values;
    if (with_window && s.window) {
        const WindowRect& w = *s.window;
        t += "[window]\nx = " + std::to_string(w.x) + "\ny = " + std::to_string(w.y) + "\nwidth = " + std::to_string(w.width) +
             "\nheight = " + std::to_string(w.height) + "\nmaximized = " + (w.maximized ? "true" : "false") + "\n";
    }
    for (const ScrollEntry& e : s.scroll) {
        char num[64];
        std::snprintf(num, sizeof(num), "%.1f", e.offset);
        t += "\n[[scroll]]\npath = " + toml_str(e.path) + "\noffset = " + num + "\n";
    }
    for (const Mark& m : s.marks) {
        char frac[32];
        std::snprintf(frac, sizeof(frac), "%.3f", double(m.frac));
        t += "\n[[mark]]\nname = '" + std::string(1, m.name) + "'\npath = " + toml_str(m.path) + "\nblock = " + std::to_string(m.block) +
             "\nfrac = " + frac + "\ntext = " + toml_basic(m.text) + "\n";
    }
    if (!s.extra_tables.empty()) t += "\n" + s.extra_tables;
    return t;
}

State load_state(const fs::path& p) {
    auto text = platform::read_file(p);
    return text ? parse_state(*text) : State{};
}

// Unknown keys come from the file as it is now on disk (another app may have
// added some since we loaded it).
bool save_state(const fs::path& p, const State& s, bool with_window) {
    State out = s;
    const State now = load_state(p);
    out.extra_values = now.extra_values;
    out.extra_tables = now.extra_tables;
    return platform::write_file_atomic(p, write_state(out, with_window));
}

bool title_bar_visible(const WindowRect& r, const std::vector<Area>& areas) {
    const int sx0 = r.x, sx1 = r.x + r.width, sy0 = r.y, sy1 = r.y + std::min(40, r.height);
    for (const Area& a : areas) {
        const int w = std::min(sx1, a.x + a.w) - std::max(sx0, a.x);
        const int h = std::min(sy1, a.y + a.h) - std::max(sy0, a.y);
        if (w >= 100 && h >= 1) return true;
    }
    return false;
}

// --- "Open with" registration ----------------------------------------------------

#ifdef _WIN32
static std::string win_error(LONG rc) { return "registry error " + std::to_string(rc); }

// Sets the default value (name == nullptr) or a named value of HKCU\\key.
static LONG reg_set(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY k;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (rc != ERROR_SUCCESS) return rc;
    rc = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return rc;
}

static void reg_delete_value(const std::wstring& key, const wchar_t* name) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegDeleteValueW(k, name);
    RegCloseKey(k);
}

static void notify_assoc_changed() { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); }

std::string register_app(const std::wstring& root, const fs::path& exe, const std::wstring& friendly_name) {
    const std::wstring prog_id = exe.stem().wstring() + L".markdown";
    const std::wstring file = exe.filename().wstring();
    const std::wstring path = absolute_path(exe).wstring();
    const std::wstring pk = root + L"\\" + prog_id;
    const std::wstring ak = root + L"\\Applications\\" + file;
    LONG rc = ERROR_SUCCESS;
    auto step = [&](LONG r) { if (rc == ERROR_SUCCESS) rc = r; };
    step(reg_set(pk, nullptr, L"Markdown document"));
    step(reg_set(pk + L"\\DefaultIcon", nullptr, L"\"" + path + L"\",0"));
    step(reg_set(pk + L"\\shell\\open\\command", nullptr, L"\"" + path + L"\" \"%1\""));
    for (const wchar_t* ext : {L".md", L".markdown", L".org"}) {
        step(reg_set(root + L"\\" + ext + L"\\OpenWithProgids", prog_id.c_str(), L""));
        step(reg_set(ak + L"\\SupportedTypes", ext, L""));
    }
    step(reg_set(ak, L"FriendlyAppName", friendly_name));
    notify_assoc_changed();
    return rc == ERROR_SUCCESS ? "" : win_error(rc);
}

std::string unregister_app(const std::wstring& root, const fs::path& exe) {
    const std::wstring prog_id = exe.stem().wstring() + L".markdown";
    RegDeleteTreeW(HKEY_CURRENT_USER, (root + L"\\" + prog_id).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (root + L"\\Applications\\" + exe.filename().wstring()).c_str());
    for (const wchar_t* ext : {L".md", L".markdown", L".org"}) reg_delete_value(root + L"\\" + ext + L"\\OpenWithProgids", prog_id.c_str());
    notify_assoc_changed();
    return "";
}
#else
std::string register_app(const std::wstring&, const fs::path&, const std::wstring&) { return "Windows only"; }
std::string unregister_app(const std::wstring&, const fs::path&) { return "Windows only"; }
#endif

}  // namespace session
