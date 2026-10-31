// The settings window (spec section 9), opened with the toolbar's ⚙ button.
//
// It edits the live Config directly; main.cpp notices the differences
// (theme, zoom, text size, fonts...) and applies them, and the returned
// list of changed keys is written to config.toml by the in-place writer.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "font_index.h"
#include "theme.h"

// (dotted key, TOML value) pairs to write, e.g. {"zoom", "1.2"}.
using ConfigWrites = std::vector<std::pair<std::string, std::string>>;

// Draw the window while `open` is true (its close button clears it).
// `fonts` lists the installed families for the font dropdowns (spec 8a).
// `hwnd` owns the native file dialogs; `scale` = pixels per point.
ConfigWrites draw_settings(bool& open, Config& cfg, const ThemeSet& themes, const FontIndex& fonts, void* hwnd, float scale);

// The keyboard theme picker (`t`, spec 6): the toolbar dropdown opened from the keyboard.
// main.cpp sets `open_request`; theme_combo does the rest (keys, live preview in cfg.theme, Enter / Esc).
struct ThemePicker {
    bool open_request = false;  // `t` was pressed: open the dropdown this frame
    bool active = false;        // open from `t`: cfg.theme is a preview, not yet kept
    std::string orig;           // the theme before `t`
    int sel = 0;                // highlighted row (0 = "Auto (system)")
    bool scroll = false;        // the highlight just moved: scroll it into view
};

// The toolbar's theme dropdown ("Auto (system)" + every theme). Returns the
// config writes for a change (empty if nothing changed).
// width <= 0: size it like the Rust port's egui ComboBox (text + arrow, at least 100 px x scale).
ConfigWrites theme_combo(const char* id, Config& cfg, const ThemeSet& themes, float width, float scale = 1.0f, ThemePicker* picker = nullptr);
