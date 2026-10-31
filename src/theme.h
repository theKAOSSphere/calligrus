// Colour themes (spec section 6): TOML files with UI colours, document colours
// and syntax-highlighting styles. The built-in themes are the files in
// the Rust port, embedded in the exe at build time (cmake/resources.cmake);
// user themes live in <config dir>/themes/*.toml and replace a built-in theme
// of the same `name`.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>  // ImU32

// How a syntax-highlighting entry (grammars/theme.txt name) is drawn.
struct SyntaxStyle {
    ImU32 color = 0;
    bool bold = false;    // (the regular code face is the only one: bold not drawn, like egui)
    bool italic = false;  // sheared, like egui
};

struct Theme {
    std::string name;          // identifier used in config.toml
    std::string display_name;  // shown in the picker
    bool dark = false;         // dark UI chrome, title bar and egui's dark coverage curve

    // [ui]: toolbar, outline, popups, widgets.
    struct Ui {
        ImU32 panel, border, widget, widget_hover, widget_active, text, weak, field, selection, selection_text, accent;
    } ui{};

    // [doc]: the document. (Flat rather than `doc.x` because the render code
    // uses these everywhere.)
    ImU32 bg = 0, text = 0, weak = 0, link = 0, heading = 0, code_bg = 0, code_border = 0, code_fg = 0, rule = 0,
          quote_bar = 0, table_head_bg = 0, zebra = 0, error = 0, find_match = 0, find_current = 0;
    // Colour of each heading level, h1..h6: [doc] heading_1 .. heading_6 when given, else
    // `heading` (h6: `weak`, small and grey like GitHub's). Never taken from the base theme,
    // so a theme without them keeps one colour for every level (the Rust port theme.rs).
    ImU32 headings[6] = {};
    // Bold / italic body text: [doc] bold / italic when given, else `text`. Only plain body
    // text takes them (not headings, links, code or bold labels). Text inside quotes and
    // callouts: [doc] quote, else `weak`. Like `headings`, never taken from the base theme.
    ImU32 bold = 0, italic = 0, quote = 0;

    // [syntax]: entry name -> style.
    std::map<std::string, SyntaxStyle, std::less<>> syntax;

    // Style for a highlight entry: the entry itself, else its dotted prefixes
    // (function.builtin -> function). nullptr = plain text.
    const SyntaxStyle* syntax_for(std::string_view entry) const;

    // The same lookup done once per entry index of the highlighter
    // (highlight_entry_names()), so drawing doesn't do string lookups.
    std::vector<std::optional<SyntaxStyle>> by_entry;
    void bind_entries(const std::vector<std::string>& entry_names);
    const SyntaxStyle* entry_style(size_t entry) const {
        return entry < by_entry.size() && by_entry[entry] ? &*by_entry[entry] : nullptr;
    }
};

// "#rrggbb" or "#rrggbbaa" (straight alpha) -> ImU32. nullopt if malformed.
std::optional<ImU32> parse_color(std::string_view s);
// "#rrggbb [bold] [italic]" -> style. nullopt if malformed.
std::optional<SyntaxStyle> parse_syntax_style(std::string_view s);
// ImU32 -> "#rrggbb" / "#rrggbbaa" (alpha only when not opaque).
std::string color_to_string(ImU32 c);

// Parse a theme file. Missing [ui] / [doc] keys come from `base_dark` (if the
// theme says dark = true) or `base_light`; with no base they stay 0 and are
// reported in `warnings`. Invalid values are reported and fall back the same
// way. Returns nullopt (and sets `error`) if the file isn't usable at all
// (TOML syntax error, no `name`).
std::optional<Theme> parse_theme(std::string_view toml_text, const Theme* base_dark, const Theme* base_light,
                                 std::string* error, std::vector<std::string>* warnings = nullptr);

// All available themes: built-in ones plus the user's.
class ThemeSet {
public:
    // (Re)load: the embedded built-ins, then <user_dir>/*.toml (may not exist).
    void load(const std::filesystem::path& user_dir);

    // Sorted by name (case-insensitive); the picker shows display names.
    const std::vector<Theme>& all() const { return themes_; }
    const Theme* find(std::string_view name) const;
    // The built-in calligrus-dark / calligrus-light (always present).
    const Theme& default_theme(bool dark) const;

    // Resolve every theme's [syntax] for the highlighter's entries (Theme::bind_entries).
    void bind_entries(const std::vector<std::string>& entry_names);

    // Problems found while loading (bad user theme files...), for the error bar.
    std::vector<std::string> warnings;

private:
    std::vector<Theme> themes_;
    Theme base_dark_, base_light_;
};

// The theme config.toml asks for: `theme` = "auto" picks `dark_theme` or
// `light_theme` by the system setting; an unknown name falls back to the
// built-in calligrus-dark / calligrus-light (by the system setting).
const Theme& resolve_theme(const ThemeSet& set, std::string_view theme, std::string_view dark_theme,
                           std::string_view light_theme, bool system_dark);
