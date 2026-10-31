#include "theme.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <tuple>

#include <toml++/toml.hpp>

#include "resources.h"

namespace fs = std::filesystem;

// --- colours -----------------------------------------------------------------------

std::optional<ImU32> parse_color(std::string_view s) {
    if (s.size() != 7 && s.size() != 9) return std::nullopt;
    if (s[0] != '#') return std::nullopt;
    unsigned v[4] = {0, 0, 0, 255};
    for (size_t i = 1; i < s.size(); i += 2) {
        unsigned byte = 0;
        for (size_t k = i; k < i + 2; k++) {
            const char c = s[k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return std::nullopt;
            byte = byte * 16 + unsigned(d);
        }
        v[i / 2] = byte;
    }
    return IM_COL32(v[0], v[1], v[2], v[3]);
}

std::string color_to_string(ImU32 c) {
    char buf[16];
    const unsigned r = (c >> IM_COL32_R_SHIFT) & 0xFF, g = (c >> IM_COL32_G_SHIFT) & 0xFF,
                   b = (c >> IM_COL32_B_SHIFT) & 0xFF, a = (c >> IM_COL32_A_SHIFT) & 0xFF;
    if (a == 255) std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    else std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", r, g, b, a);
    return buf;
}

std::optional<SyntaxStyle> parse_syntax_style(std::string_view s) {
    // Words separated by whitespace: the colour first, then bold / italic in
    // any order (unknown words are ignored).
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    std::vector<std::string_view> words;
    for (size_t i = 0; i < s.size();) {
        while (i < s.size() && space(s[i])) i++;
        size_t start = i;
        while (i < s.size() && !space(s[i])) i++;
        if (i > start) words.push_back(s.substr(start, i - start));
    }
    if (words.empty()) return std::nullopt;
    auto color = parse_color(words[0]);
    if (!color) return std::nullopt;
    SyntaxStyle style{*color};
    for (size_t i = 1; i < words.size(); i++) {
        if (words[i] == "bold") style.bold = true;
        if (words[i] == "italic") style.italic = true;
    }
    return style;
}

// --- Theme ---------------------------------------------------------------------------

const SyntaxStyle* Theme::syntax_for(std::string_view entry) const {
    for (;;) {
        if (auto it = syntax.find(entry); it != syntax.end()) return &it->second;
        size_t dot = entry.rfind('.');
        if (dot == std::string_view::npos) return nullptr;
        entry = entry.substr(0, dot);
    }
}

void Theme::bind_entries(const std::vector<std::string>& entry_names) {
    by_entry.clear();
    for (const std::string& entry : entry_names) {
        const SyntaxStyle* s = syntax_for(entry);
        by_entry.push_back(s ? std::optional<SyntaxStyle>(*s) : std::nullopt);
    }
}

// --- parsing -------------------------------------------------------------------------

namespace {

// Every colour key of [ui] and [doc], with where it's stored, so parsing and
// falling back are one loop instead of 26 copies of the same code.
struct ColorKey {
    const char* table;
    const char* key;
    ImU32 Theme::*doc;      // set for [doc] keys
    ImU32 Theme::Ui::*ui;   // set for [ui] keys
};

const ColorKey kColorKeys[] = {
    {"ui", "panel", nullptr, &Theme::Ui::panel},
    {"ui", "border", nullptr, &Theme::Ui::border},
    {"ui", "widget", nullptr, &Theme::Ui::widget},
    {"ui", "widget_hover", nullptr, &Theme::Ui::widget_hover},
    {"ui", "widget_active", nullptr, &Theme::Ui::widget_active},
    {"ui", "text", nullptr, &Theme::Ui::text},
    {"ui", "weak", nullptr, &Theme::Ui::weak},
    {"ui", "field", nullptr, &Theme::Ui::field},
    {"ui", "selection", nullptr, &Theme::Ui::selection},
    {"ui", "selection_text", nullptr, &Theme::Ui::selection_text},
    {"ui", "accent", nullptr, &Theme::Ui::accent},
    {"doc", "bg", &Theme::bg, nullptr},
    {"doc", "text", &Theme::text, nullptr},
    {"doc", "weak", &Theme::weak, nullptr},
    {"doc", "link", &Theme::link, nullptr},
    {"doc", "heading", &Theme::heading, nullptr},
    {"doc", "code_bg", &Theme::code_bg, nullptr},
    {"doc", "code_border", &Theme::code_border, nullptr},
    {"doc", "code_fg", &Theme::code_fg, nullptr},
    {"doc", "rule", &Theme::rule, nullptr},
    {"doc", "quote_bar", &Theme::quote_bar, nullptr},
    {"doc", "table_head_bg", &Theme::table_head_bg, nullptr},
    {"doc", "zebra", &Theme::zebra, nullptr},
    {"doc", "error", &Theme::error, nullptr},
    {"doc", "find_match", &Theme::find_match, nullptr},
    {"doc", "find_current", &Theme::find_current, nullptr},
};

ImU32& slot(Theme& t, const ColorKey& k) { return k.doc ? t.*k.doc : t.ui.*k.ui; }
ImU32 slot(const Theme& t, const ColorKey& k) { return k.doc ? t.*k.doc : t.ui.*k.ui; }

}  // namespace

std::optional<Theme> parse_theme(std::string_view toml_text, const Theme* base_dark, const Theme* base_light,
                                 std::string* error, std::vector<std::string>* warnings) {
    auto warn = [&](std::string msg) {
        if (warnings) warnings->push_back(std::move(msg));
    };
    toml::table root;
    try {
        root = toml::parse(toml_text);
    } catch (const toml::parse_error& e) {
        if (error) {
            std::ostringstream ss;
            ss << e.description() << " (line " << e.source().begin.line << ")";
            *error = ss.str();
        }
        return std::nullopt;
    }

    Theme t;
    auto name = root["name"].value<std::string>();
    if (!name || name->empty()) {
        if (error) *error = "theme has no `name`";
        return std::nullopt;
    }
    t.name = *name;
    t.display_name = root["display_name"].value_or(t.name);
    if (auto dark = root["dark"]; dark && !dark.is_boolean()) warn(t.name + ": `dark` must be true or false");
    t.dark = root["dark"].value_or(false);

    // Colours: the theme's value, else the base theme's (calligrus-dark / -light).
    const Theme* base = t.dark ? base_dark : base_light;
    bool heading_given = false;
    for (const ColorKey& k : kColorKeys) {
        auto node = root[k.table][k.key];
        std::optional<ImU32> c;
        if (node) {
            if (auto s = node.value<std::string>()) c = parse_color(*s);
            if (!c) warn(t.name + ": invalid colour for " + k.table + "." + k.key);
        }
        if (c) {
            slot(t, k) = *c;
            heading_given = heading_given || std::string_view(k.key) == "heading" && k.doc;
        } else if (base) {
            slot(t, k) = slot(*base, k);
        } else if (std::string_view(k.key) != "heading" || !k.doc) {
            warn(t.name + ": missing " + k.table + "." + k.key);
        }
    }
    // doc.heading is optional and defaults to the theme's own text colour.
    if (!heading_given) t.heading = t.text;
    for (int i = 0; i < 6; i++) {
        const std::string key = "heading_" + std::to_string(i + 1);
        std::optional<ImU32> c;
        if (auto node = root["doc"][key]) {
            if (auto s = node.value<std::string>()) c = parse_color(*s);
            if (!c) warn(t.name + ": invalid colour for doc." + key);
        }
        t.headings[i] = c ? *c : i == 5 ? t.weak : t.heading;
    }
    for (auto [key, field, fallback] : {std::tuple{"bold", &Theme::bold, &Theme::text},
                                        std::tuple{"italic", &Theme::italic, &Theme::text},
                                        std::tuple{"quote", &Theme::quote, &Theme::weak}}) {
        std::optional<ImU32> c;
        if (auto node = root["doc"][key]) {
            if (auto s = node.value<std::string>()) c = parse_color(*s);
            if (!c) warn(t.name + ": invalid colour for doc." + key);
        }
        t.*field = c ? *c : t.*fallback;
    }

    // [syntax]: "entry" = "#rrggbb [bold] [italic]". A theme without a
    // [syntax] table inherits its base theme's; one with a table uses only
    // that (a missing entry falls back along its dotted prefix at lookup
    // time, else plain). Spec 6a.
    if (!root["syntax"].as_table() && base) t.syntax = base->syntax;
    if (const toml::table* syn = root["syntax"].as_table()) {
        for (auto&& [key, value] : *syn) {
            std::optional<SyntaxStyle> style;
            if (auto s = value.value<std::string>()) style = parse_syntax_style(*s);
            if (style) t.syntax[std::string(key.str())] = *style;
            else warn(t.name + ": invalid style for syntax." + std::string(key.str()));
        }
    }
    return t;
}

// --- ThemeSet --------------------------------------------------------------------------

void ThemeSet::load(const fs::path& user_dir) {
    themes_.clear();
    warnings.clear();
    auto add = [&](Theme t) {
        // Same name as one already loaded (a user theme replacing a built-in): replace.
        for (Theme& existing : themes_)
            if (existing.name == t.name) {
                existing = std::move(t);
                return;
            }
        themes_.push_back(std::move(t));
    };

    // The two base themes first: every other theme falls back to them.
    std::string err;
    for (bool dark : {true, false}) {
        auto text = resource(dark ? "themes/calligrus-dark.toml" : "themes/calligrus-light.toml");
        auto t = text ? parse_theme(*text, nullptr, nullptr, &err, &warnings) : std::nullopt;
        (dark ? base_dark_ : base_light_) = t ? *t : Theme{};
        if (!t) warnings.push_back(std::string("built-in theme ") + (dark ? "calligrus-dark" : "calligrus-light") + " is broken: " + err);
    }
    if (base_dark_.name.empty()) base_dark_.name = "calligrus-dark", base_dark_.dark = true;
    if (base_light_.name.empty()) base_light_.name = "calligrus-light";

    for (const Resource& r : resources_with_prefix("themes/")) {
        if (!r.name.ends_with(".toml")) continue;
        if (auto t = parse_theme(r.data, &base_dark_, &base_light_, &err, &warnings)) add(std::move(*t));
        else warnings.push_back(std::string(r.name) + ": " + err);
    }

    // User themes.
    std::error_code ec;
    if (fs::is_directory(user_dir, ec)) {
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(user_dir, ec))
            if (entry.path().extension() == ".toml") files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        for (const fs::path& p : files) {
            std::ifstream in(p, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            const std::u8string u8 = p.filename().u8string();
            const std::string file(u8.begin(), u8.end());
            if (auto t = parse_theme(ss.str(), &base_dark_, &base_light_, &err, &warnings)) add(std::move(*t));
            else warnings.push_back(file + ": " + err);
        }
    }

    auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::sort(themes_.begin(), themes_.end(),
              [&](const Theme& a, const Theme& b) { return lower(a.name) < lower(b.name); });
}

void ThemeSet::bind_entries(const std::vector<std::string>& entry_names) {
    for (Theme& t : themes_) t.bind_entries(entry_names);
    base_dark_.bind_entries(entry_names);
    base_light_.bind_entries(entry_names);
}

const Theme* ThemeSet::find(std::string_view name) const {
    for (const Theme& t : themes_)
        if (t.name == name) return &t;
    // The built-in themes used to be called mdview-dark / mdview-light: old config files keep working.
    if (name == "mdview-dark") return find("calligrus-dark");
    if (name == "mdview-light") return find("calligrus-light");
    return nullptr;
}

const Theme& ThemeSet::default_theme(bool dark) const {
    // A user theme may have replaced the built-in one: prefer what's in the list.
    if (const Theme* t = find(dark ? "calligrus-dark" : "calligrus-light")) return *t;
    return dark ? base_dark_ : base_light_;
}

const Theme& resolve_theme(const ThemeSet& set, std::string_view theme, std::string_view dark_theme,
                           std::string_view light_theme, bool system_dark) {
    std::string_view name = theme;
    bool dark = system_dark;
    if (theme == "auto") name = system_dark ? dark_theme : light_theme;
    if (const Theme* t = set.find(name)) return *t;
    return set.default_theme(dark);
}
