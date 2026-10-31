#include "config.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>

#include <toml++/toml.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cstdlib>
#endif

#include "navigation.h"  // zoom limits

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------
//
// The rules (and the shared tests in the Rust port, whose reference
// implementation is check.py there): a key with the wrong type, an empty theme
// name or a number out of range keeps its default (numbers are NOT clamped);
// unknown keys and tables are ignored; a file that isn't valid TOML gives all
// defaults.

Config parse_config(std::string_view text, std::vector<std::string>* warnings) {
    auto warn = [&](std::string msg) {
        if (warnings) warnings->push_back("config.toml: " + std::move(msg));
    };
    Config c;
    toml::table root;
    try {
        root = toml::parse(text);
    } catch (const toml::parse_error& e) {
        warn(std::string(e.description()) + " (line " + std::to_string(e.source().begin.line) + "); using the defaults");
        return c;
    }

    // `table.key` (or just `key` at the top level) as a node; empty if missing,
    // or if `table` isn't a table.
    auto node_of = [&](const char* table, const char* key) {
        return table ? root[table][key] : root[key];
    };
    auto name_of = [](const char* table, const char* key) { return table ? std::string(table) + "." + key : std::string(key); };

    auto read_string = [&](const char* table, const char* key, std::string& out, bool allow_empty) {
        auto node = node_of(table, key);
        if (!node) return;
        auto v = node.value_exact<std::string>();
        if (v && (allow_empty || !v->empty())) out = *v;
        else warn(name_of(table, key) + " must be a" + (allow_empty ? "" : " non-empty") + " string");
    };
    auto read_bool = [&](const char* table, const char* key, bool& out) {
        auto node = node_of(table, key);
        if (!node) return;
        if (auto v = node.value_exact<bool>()) out = *v;
        else warn(name_of(table, key) + " must be true or false");
    };

    read_string(nullptr, "theme", c.theme, false);
    read_string(nullptr, "dark_theme", c.dark_theme, false);
    read_string(nullptr, "light_theme", c.light_theme, false);

    if (auto node = root["zoom"]) {
        // A number (an integer is fine: 3 = 3.0) in 0.5 .. 3.0.
        std::optional<double> v;
        if (node.is_integer()) v = double(*node.value_exact<int64_t>());
        else v = node.value_exact<double>();
        if (v && std::isfinite(*v) && *v >= nav::kZoomMin && *v <= nav::kZoomMax) c.zoom = *v;
        else warn("zoom must be a number from 0.5 to 3.0");
    }
    if (auto node = root["body_size"]) {
        // A whole number (16, not 16.0) in 12 .. 28.
        auto v = node.value_exact<int64_t>();
        if (v && *v >= kBodySizeMin && *v <= kBodySizeMax) c.body_size = int(*v);
        else warn("body_size must be a whole number from 12 to 28");
    }
    if (auto node = root["max_width"]) {
        auto v = node.value_exact<int64_t>();
        if (v && *v >= kMaxWidthMin && *v <= kMaxWidthMax) c.max_width = int(*v);
        else warn("max_width must be a whole number from 400 to 2400");
    }
    read_bool(nullptr, "show_outline", c.show_outline);
    read_bool(nullptr, "vim_keys", c.vim_keys);
    read_bool(nullptr, "system_fallback", c.system_fallback);
    read_bool(nullptr, "remember_window", c.remember_window);
    read_bool(nullptr, "editor_marker", c.editor_marker);

    read_string("fonts", "prose", c.fonts.prose, true);
    read_string("fonts", "prose_bold", c.fonts.prose_bold, true);
    read_string("fonts", "prose_italic", c.fonts.prose_italic, true);
    read_string("fonts", "prose_bold_italic", c.fonts.prose_bold_italic, true);
    read_string("fonts", "mono", c.fonts.mono, true);

    read_bool("code", "line_numbers", c.code_line_numbers);
    return c;
}

std::string config_dump(const Config& c) {
    // Sorted by key, like the .expected files.
    const std::map<std::string, std::string> values = {
        {"theme", toml_string(c.theme)},
        {"dark_theme", toml_string(c.dark_theme)},
        {"light_theme", toml_string(c.light_theme)},
        {"zoom", toml_float(c.zoom)},
        {"body_size", toml_int(c.body_size)},
        {"max_width", toml_int(c.max_width)},
        {"show_outline", toml_bool(c.show_outline)},
        {"remember_window", toml_bool(c.remember_window)},
        {"vim_keys", toml_bool(c.vim_keys)},
        {"system_fallback", toml_bool(c.system_fallback)},
        {"editor_marker", toml_bool(c.editor_marker)},
        {"fonts.prose", toml_string(c.fonts.prose)},
        {"fonts.prose_bold", toml_string(c.fonts.prose_bold)},
        {"fonts.prose_italic", toml_string(c.fonts.prose_italic)},
        {"fonts.prose_bold_italic", toml_string(c.fonts.prose_bold_italic)},
        {"fonts.mono", toml_string(c.fonts.mono)},
        {"code.line_numbers", toml_bool(c.code_line_numbers)},
    };
    std::string s;
    for (const auto& [key, value] : values) s += key + " = " + value + "\n";
    return s;
}

std::string config_file_text(const Config& c) {
    // The spec's example file with the defaults; the writer then puts in the
    // current values (keeping the comments in their columns).
    std::string t =
        "# Calligrus configuration\n"
        "theme = \"auto\"            # \"auto\" = follow the system light/dark setting, or a theme name\n"
        "dark_theme = \"calligrus-dark\"   # used by \"auto\" when the system is dark\n"
        "light_theme = \"calligrus-light\" # used by \"auto\" when the system is light\n"
        "zoom = 1.0                # 0.5 .. 3.0\n"
        "body_size = 16            # body text size in logical pixels at zoom 1.0 (12 .. 28)\n"
        "max_width = 860           # widest reading column in points at zoom 1.0 (400 .. 2400)\n"
        "show_outline = true       # outline panel visible at start-up\n"
        "vim_keys = true\n"
        "system_fallback = true   # installed fonts fill characters the bundled ones lack\n"
        "remember_window = true    # restore window size and position (kept in state.toml)\n"
        "editor_marker = true      # cursor marker in editor previews (scroll sync is unaffected)\n"
        "\n"
        "[fonts]\n"
        "prose = \"\"                # path to a TTF/OTF for body text and headings; \"\" = built-in IBM Plex Sans\n"
        "prose_bold = \"\"           # optional; \"\" = built-in bold if prose is built-in, else the regular file\n"
        "prose_italic = \"\"         # optional, same rule\n"
        "prose_bold_italic = \"\"    # optional, same rule\n"
        "mono = \"\"                 # path for code; \"\" = built-in Cascadia Code\n"
        "\n"
        "[code]\n"
        "line_numbers = false\n";
    t = config_set(t, "theme", toml_string(c.theme));
    t = config_set(t, "dark_theme", toml_string(c.dark_theme));
    t = config_set(t, "light_theme", toml_string(c.light_theme));
    t = config_set(t, "zoom", toml_float(c.zoom));
    t = config_set(t, "body_size", toml_int(c.body_size));
    t = config_set(t, "max_width", toml_int(c.max_width));
    t = config_set(t, "show_outline", toml_bool(c.show_outline));
    t = config_set(t, "vim_keys", toml_bool(c.vim_keys));
    t = config_set(t, "system_fallback", toml_bool(c.system_fallback));
    t = config_set(t, "remember_window", toml_bool(c.remember_window));
    t = config_set(t, "editor_marker", toml_bool(c.editor_marker));
    t = config_set(t, "fonts.prose", toml_string(c.fonts.prose));
    t = config_set(t, "fonts.prose_bold", toml_string(c.fonts.prose_bold));
    t = config_set(t, "fonts.prose_italic", toml_string(c.fonts.prose_italic));
    t = config_set(t, "fonts.prose_bold_italic", toml_string(c.fonts.prose_bold_italic));
    t = config_set(t, "fonts.mono", toml_string(c.fonts.mono));
    t = config_set(t, "code.line_numbers", toml_bool(c.code_line_numbers));
    return t;
}

// ---------------------------------------------------------------------------
// TOML literals (formatted exactly like check.py's fmt_value)
// ---------------------------------------------------------------------------

std::string toml_string(std::string_view s) {
    std::string out = "\"";
    for (char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\f': out += "\\f"; break;
            case '\r': out += "\\r"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned char>(ch));
                    out += buf;
                } else {
                    out += ch;  // UTF-8 bytes go through unchanged
                }
        }
    }
    return out + "\"";
}

std::string toml_bool(bool b) { return b ? "true" : "false"; }

std::string toml_float(double f) {
    char buf[32];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), f);  // shortest text that reads back the same
    std::string s(buf, ec == std::errc() ? end : buf);
    if (s.find('.') == std::string::npos) s += ".0";  // 2 -> 2.0 (a TOML float)
    return s;
}

std::string toml_int(int i) { return std::to_string(i); }

// ---------------------------------------------------------------------------
// The in-place writer
// ---------------------------------------------------------------------------
//
// Works on lines, like check.py's apply_set (the reference for the shared
// edit-*.toml tests):
//   - the file is split into sections: the top level, then one per [header];
//   - in the key's section, the line `key = ...` (or `"key" = ...`) gets its
//     value replaced; a trailing comment stays, and if only spaces separate
//     value and comment, the gap grows/shrinks so the comment keeps its column;
//   - no such line: `key = value` goes after the section's last non-blank line;
//   - no such section: a blank line, `[table]` and the key go at the end.
// Line endings follow the file's first line (CRLF stays CRLF).

namespace {

struct Line {
    std::string content;
    std::string eol;  // "\n", "\r\n", or "" (last line without a line break)
};

std::vector<Line> split_lines(std::string_view text) {
    std::vector<Line> lines;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find('\n', i);
        if (j == std::string_view::npos) {
            lines.push_back({std::string(text.substr(i)), ""});
            break;
        }
        if (j > i && text[j - 1] == '\r') lines.push_back({std::string(text.substr(i, j - 1 - i)), "\r\n"});
        else lines.push_back({std::string(text.substr(i, j - i)), "\n"});
        i = j + 1;
    }
    return lines;
}

std::string_view strip(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

std::string_view lstrip(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    return s;
}

bool is_blank(std::string_view s) { return strip(s).empty(); }

// "[ fonts ]  # x" -> "fonts"; not a header -> nullopt.
std::optional<std::string> header_name(std::string_view content) {
    std::string_view s = lstrip(content);
    if (!s.starts_with('[')) return std::nullopt;
    size_t end = s.find(']');
    if (end == std::string_view::npos) return std::nullopt;
    return std::string(strip(s.substr(1, end - 1)));
}

// If this line defines `key` (bare or "quoted"): the index just after its '='.
std::optional<size_t> match_key(std::string_view content, std::string_view key) {
    std::string_view s = lstrip(content);
    const size_t indent = content.size() - s.size();
    for (const std::string& form : {std::string(key), "\"" + std::string(key) + "\""}) {
        if (!s.starts_with(form)) continue;
        std::string_view rest = s.substr(form.size());
        std::string_view r = lstrip(rest);
        if (r.starts_with('=')) return indent + form.size() + (rest.size() - r.size()) + 1;
    }
    return std::nullopt;
}

// End of the (single-line) value starting at `start`.
size_t value_end(std::string_view content, size_t start) {
    if (start >= content.size()) return start;
    const char c = content[start];
    if (c == '"') {
        for (size_t i = start + 1; i < content.size(); i++) {
            if (content[i] == '\\') {
                i++;
                continue;
            }
            if (content[i] == '"') return i + 1;
        }
        return content.size();
    }
    if (c == '\'') {
        size_t j = content.find('\'', start + 1);
        return j == std::string_view::npos ? content.size() : j + 1;
    }
    size_t i = start;
    while (i < content.size() && content[i] != ' ' && content[i] != '\t' && content[i] != '#') i++;
    return i;
}

// Number of characters (UTF-8 code points), for keeping comment columns.
long chars(std::string_view s) {
    return long(std::count_if(s.begin(), s.end(), [](char ch) { return (static_cast<unsigned char>(ch) & 0xC0) != 0x80; }));
}

std::string replace_value(std::string_view content, size_t eq, std::string_view value) {
    size_t vstart = eq;
    while (vstart < content.size() && (content[vstart] == ' ' || content[vstart] == '\t')) vstart++;
    const size_t vend = value_end(content, vstart);
    const std::string_view old = content.substr(vstart, vend - vstart);
    std::string rest(content.substr(vend));
    const size_t hash = rest.find('#');
    if (hash != std::string::npos && is_blank(std::string_view(rest).substr(0, hash))) {
        std::string gap = rest.substr(0, hash), comment = rest.substr(hash);
        // Only spaces before the comment: keep it in the same column.
        if (!gap.empty() && gap.find_first_not_of(' ') == std::string::npos)
            gap = std::string(size_t(std::max(1L, long(gap.size()) + chars(old) - chars(value))), ' ');
        rest = gap + comment;
    }
    return std::string(content.substr(0, vstart)) + std::string(value) + rest;
}

std::string join(const std::vector<Line>& lines) {
    std::string out;
    for (const Line& l : lines) out += l.content + l.eol;
    return out;
}

}  // namespace

std::string config_set(std::string_view text, std::string_view dotted, std::string_view value) {
    // A UTF-8 byte-order mark stays in front.
    std::string bom;
    if (text.starts_with("\xEF\xBB\xBF")) {
        bom = "\xEF\xBB\xBF";
        text.remove_prefix(3);
    }
    const size_t dot = dotted.find('.');
    const std::optional<std::string> table = dot == std::string_view::npos ? std::nullopt : std::optional(std::string(dotted.substr(0, dot)));
    const std::string key(dot == std::string_view::npos ? dotted : dotted.substr(dot + 1));

    std::vector<Line> lines = split_lines(text);
    std::string eol = "\n";
    for (const Line& l : lines)
        if (!l.eol.empty()) {
            eol = l.eol;
            break;
        }

    // The key's section: [first, end) lines, header at first - 1 (none for the top level).
    std::optional<std::pair<size_t, size_t>> section;
    std::optional<size_t> header;  // index of the [table] line
    if (!table) {
        size_t end = 0;
        while (end < lines.size() && !header_name(lines[end].content)) end++;
        section = {0, end};
    } else {
        for (size_t i = 0; i < lines.size() && !section; i++) {
            if (header_name(lines[i].content) != table) continue;
            size_t end = i + 1;
            while (end < lines.size() && !header_name(lines[end].content)) end++;
            section = {i + 1, end};
            header = i;
        }
    }

    const std::string new_line = key + " = " + std::string(value);
    if (section) {
        auto [first, end] = *section;
        for (size_t i = first; i < end; i++) {
            if (auto eq = match_key(lines[i].content, key)) {
                lines[i].content = replace_value(lines[i].content, *eq, value);
                return bom + join(lines);
            }
        }
        // Not found: after the section's last non-blank line (or its header,
        // or at the very start of the file for an empty top level).
        std::optional<size_t> last = header;
        for (size_t i = first; i < end; i++)
            if (!is_blank(lines[i].content)) last = i;
        if (last && lines[*last].eol.empty()) lines[*last].eol = eol;
        lines.insert(lines.begin() + std::ptrdiff_t(last ? *last + 1 : 0), Line{new_line, eol});
        return bom + join(lines);
    }

    // The table doesn't exist: blank line, header and key at the end.
    if (!lines.empty()) {
        if (lines.back().eol.empty()) lines.back().eol = eol;
        if (!is_blank(lines.back().content)) lines.push_back({"", eol});
    }
    lines.push_back({"[" + *table + "]", eol});
    lines.push_back({new_line, eol});
    return bom + join(lines);
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

#ifdef _WIN32
static std::wstring env(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return L"";
    std::wstring v(n, L'\0');
    v.resize(GetEnvironmentVariableW(name, v.data(), n));
    return v;
}
#else
// Environment variable as UTF-8 (empty if unset).
static std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

static fs::path u8_path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
#endif

fs::path find_config_file(const ConfigEnv& e) { return userdir::config_file(e); }

ConfigEnv current_config_env() { return userdir::current_env(); }

fs::path config_path() { return find_config_file(current_config_env()); }

fs::path config_dir() { return config_path().parent_path(); }

fs::path expand_path(std::string_view path) {
    if (path.empty()) return {};
#ifdef _WIN32
    // UTF-8 -> UTF-16 so non-ASCII paths and variables survive.
    std::wstring w = fs::path(std::u8string(path.begin(), path.end())).wstring();

    // %VAR% -> its value (unknown variables are left as they are).
    std::wstring out;
    for (size_t i = 0; i < w.size(); i++) {
        if (w[i] == L'%') {
            size_t close = w.find(L'%', i + 1);
            if (close != std::wstring::npos && close > i + 1) {
                std::wstring value = env(w.substr(i + 1, close - i - 1).c_str());
                if (!value.empty()) {
                    out += value;
                    i = close;
                    continue;
                }
            }
        }
        out += w[i];
    }
    // ~ or ~/... or ~\... -> the home folder.
    if (!out.empty() && out[0] == L'~' && (out.size() == 1 || out[1] == L'/' || out[1] == L'\\'))
        out = env(L"USERPROFILE") + out.substr(1);
    return fs::path(out);
#else
    // %VAR% -> its value (unknown variables are left as they are), then a leading ~ -> $HOME.
    const std::string s(path);
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%') {
            size_t close = s.find('%', i + 1);
            if (close != std::string::npos && close > i + 1) {
                std::string value = env(s.substr(i + 1, close - i - 1).c_str());
                if (!value.empty()) {
                    out += value;
                    i = close;
                    continue;
                }
            }
        }
        out += s[i];
    }
    if (!out.empty() && out[0] == '~' && (out.size() == 1 || out[1] == '/' || out[1] == '\\'))
        out = env("HOME") + out.substr(1);
    return u8_path(out);
#endif
}
