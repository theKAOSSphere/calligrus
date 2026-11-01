#include "settings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>  // (OpenPopupEx: the theme picker opens the combo's popup itself)

#include "font_index.h"
#include "navigation.h"
#include "platform.h"

namespace fs = std::filesystem;

namespace {

std::string to_utf8(const fs::path& p) {
    std::u8string u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

// A dropdown of themes. `auto_item` adds "Auto (system)" (value "auto") first.
// Returns true when `value` was changed (kept: with `pk`, only when the picker ends on another
// theme than it started with). `pk` = the keyboard picker's state (the toolbar's dropdown only).
bool theme_picker(const char* id, std::string& value, const ThemeSet& themes, bool auto_item, float width, float scale = 1.0f, ThemePicker* pk = nullptr) {
    // The rows: "Auto (system)" (if any), then the themes.
    std::vector<std::pair<std::string, std::string>> rows;  // (value, label)
    if (auto_item) rows.emplace_back("auto", "Auto (system)");
    for (const Theme& t : themes.all()) rows.emplace_back(t.name, t.display_name);
    auto row_of = [&](const std::string& name) {
        for (size_t i = 0; i < rows.size(); i++)
            if (rows[i].first == name) return int(i);
        return 0;
    };
    if (pk && pk->open_request) {
        pk->open_request = false;
        pk->active = true;
        pk->orig = value;
        pk->sel = row_of(value);
        pk->scroll = true;
        // BeginCombo's own popup id: what a click on the button would open.
        ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, ImGui::GetID(id)));
    }
    const Theme* current = themes.find(value);
    std::string preview = value == "auto" && auto_item ? "Auto (system)" : current ? current->display_name : value;
    bool changed = false;
    // width <= 0: egui's ComboBox rule (what the Rust port uses): as wide as the text plus
    // its arrow (4 px gap + 14 px icon + 2 x 4 px padding), but at least 100 px.
    if (width <= 0) width = std::max(100.0f * scale, ImGui::CalcTextSize(preview.c_str()).x + 26.0f * scale);
    ImGui::SetNextItemWidth(width);
    // Drawn like a button (egui's ComboBox), not like a text field.
    const ImGuiStyle& s = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, s.Colors[ImGuiCol_Button]);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, s.Colors[ImGuiCol_ButtonHovered]);
    // The arrow is painted below, as egui's small triangle (ImGui's is a big one in its own box).
    const ImVec2 box_min = ImGui::GetCursorScreenPos();
    const bool open = ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_NoArrowButton);
    ImGui::PopStyleColor(2);
    // The theme before the picker (while it previews, `value` is only the highlighted one).
    const std::string base = pk && pk->active ? pk->orig : value;
    if (!open && pk && pk->active) {
        value = pk->orig;  // closed by a click outside: same as Esc
        pk->active = false;
    }
    {
        // egui: a 14 px icon slot at the right edge (4 px padding), the triangle 70 % x 45 % of it.
        const float box_h = ImGui::GetFrameHeight();
        const ImVec2 c(box_min.x + width - (4.0f + 7.0f) * scale, box_min.y + box_h * 0.5f);
        const float hw = 14.0f * scale * 0.35f, hh = 14.0f * scale * 0.225f;
        // (Open: the current window is the popup, so go through the foreground list.)
        ImDrawList* dl = open ? ImGui::GetForegroundDrawList() : ImGui::GetWindowDrawList();
        if (width >= 8.0f) dl->AddTriangleFilled(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y - hh), ImVec2(c.x, c.y + hh),
                                                          ImGui::GetColorU32(ImGuiCol_Text));
    }
    if (open) {
        bool close = false;
        if (pk && pk->active) {
            // j / k / Down / Up move the highlight (no wrapping, like the recent-files popup); the page
            // previews it. Enter keeps it, Esc puts the old theme back.
            const int last = int(rows.size()) - 1;
            const int was = pk->sel;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_J)) pk->sel = std::min(pk->sel + 1, last);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_K)) pk->sel = std::max(pk->sel - 1, 0);
            pk->scroll = pk->scroll || pk->sel != was;
            value = rows[pk->sel].first;
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                value = base;
                pk->active = false;
                close = true;
            } else if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                changed = value != base;
                pk->active = false;
                close = true;
            }
        }
        for (size_t i = 0; i < rows.size(); i++) {
            ImGui::PushID(rows[i].first.c_str());
            const bool marked = pk && pk->active ? int(i) == pk->sel : value == rows[i].first;
            if (ImGui::Selectable(rows[i].second.c_str(), marked)) {
                changed = rows[i].first != base;  // (a click keeps it, as without the picker)
                value = rows[i].first;
                if (pk) pk->active = false;
            }
            if (marked && pk && pk->scroll) ImGui::SetScrollHereY(0.5f);
            ImGui::PopID();
        }
        if (pk) pk->scroll = false;
        if (close) ImGui::CloseCurrentPopup();
        ImGui::EndCombo();
    }
    return changed;
}

// Case-insensitive (ASCII) "does `name` contain `filter`".
bool matches_filter(const std::string& name, const char* filter) {
    if (!*filter) return true;
    auto lower = [](std::string s) {
        for (char& c : s) c = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
        return s;
    };
    return lower(name).find(lower(filter)) != std::string::npos;
}

// One font row (spec 8a): a searchable dropdown of installed families (type
// to filter, Enter takes the first match), plus Browse… (a file) and Reset.
// Selecting a family writes its name; Browse writes the file's path.
// `empty_label` is what "" means ("Built-in (IBM Plex Sans)", "Automatic", ...):
// shown in the closed dropdown and as the list's first entry. `mono` lists
// monospace families only, unless "Show all" is ticked.
void font_family_field(const char* label, const char* key, std::string& value, const char* empty_label,
                       const FontIndex& index, bool mono, void* hwnd, float scale, ConfigWrites& writes) {
    struct FieldState {
        std::array<char, 128> filter{};
        bool show_all = false;
    };
    static std::map<std::string, FieldState> states;  // one per field
    FieldState& fs_ = states[key];

    ImGui::PushID(key);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(110 * scale);
    ImGui::SetNextItemWidth(std::max(80.0f * scale, ImGui::GetContentRegionAvail().x - 120 * scale));

    // The closed dropdown shows the family name, a path's file name, or the default.
    std::string preview = value.empty() ? std::string(empty_label)
                          : is_font_family_name(value) ? value
                                                       : to_utf8(fs::path(std::u8string(value.begin(), value.end())).filename());
    const ImGuiStyle& s = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, s.Colors[ImGuiCol_Button]);  // drawn like a button, as the theme picker
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, s.Colors[ImGuiCol_ButtonHovered]);
    const bool open = ImGui::BeginCombo("##family", preview.c_str(), ImGuiComboFlags_HeightLarge);
    ImGui::PopStyleColor(2);
    if (!value.empty() && !is_font_family_name(value)) {
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", value.c_str());  // the full path
    }
    if (open) {
        std::string picked;  // a family chosen this frame ("" = none)
        bool pick_builtin = false;
        const bool appearing = ImGui::IsWindowAppearing();
        if (appearing) {
            fs_.filter[0] = 0;
            ImGui::SetKeyboardFocusHere();  // type to filter right away
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter = ImGui::InputTextWithHint("##filter", "Type to filter\xE2\x80\xA6", fs_.filter.data(),
                                                    fs_.filter.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        if (mono) ImGui::Checkbox("Show all", &fs_.show_all);

        if (!index.ready()) {
            ImGui::TextDisabled("Scanning installed fonts\xE2\x80\xA6");
        } else {
            // The families to list.
            std::vector<const FontFamily*> shown;
            for (const FontFamily& f : index.families())
                if ((!mono || fs_.show_all || f.mono) && matches_filter(f.name, fs_.filter.data())) shown.push_back(&f);
            if (enter && !shown.empty()) picked = shown.front()->name;  // Enter = the first match

            const float line = ImGui::GetTextLineHeightWithSpacing();
            if (ImGui::BeginChild("##list", ImVec2(0, line * 14), ImGuiChildFlags_None)) {
                if (!fs_.filter[0] && ImGui::Selectable(empty_label, value.empty()))
                    pick_builtin = true;
                int selected_row = -1;
                for (size_t i = 0; i < shown.size(); i++)
                    if (same_family(value, shown[i]->name)) selected_row = int(i);
                ImGuiListClipper clip;  // only the visible rows are laid out (hundreds of families)
                clip.Begin(int(shown.size()));
                if (appearing && selected_row >= 0) clip.IncludeItemByIndex(selected_row);  // to scroll it into view
                while (clip.Step())
                    for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                        const std::string& name = shown[size_t(i)]->name;
                        if (ImGui::Selectable(name.c_str(), i == selected_row)) picked = name;
                        if (i == selected_row && appearing) ImGui::SetScrollHereY(0.3f);
                    }
                if (shown.empty()) ImGui::TextDisabled("No matching fonts");
            }
            ImGui::EndChild();
        }
        if (!picked.empty() || pick_builtin) {
            const std::string new_value = pick_builtin ? std::string() : picked;
            if (new_value != value) {
                value = new_value;
                writes.emplace_back(key, toml_string(value));
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button("Browse\xE2\x80\xA6")) {  // "Browse…"
        if (auto p = platform::open_font_dialog(hwnd)) {
            value = to_utf8(*p);
            writes.emplace_back(key, toml_string(value));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset") && !value.empty()) {
        value.clear();
        writes.emplace_back(key, toml_string(value));
    }
    ImGui::PopID();
}

void checkbox(const char* label, const char* key, bool& value, ConfigWrites& writes) {
    if (ImGui::Checkbox(label, &value)) writes.emplace_back(key, toml_bool(value));
}

}  // namespace

ConfigWrites theme_combo(const char* id, Config& cfg, const ThemeSet& themes, float width, float scale, ThemePicker* picker) {
    ConfigWrites writes;
    if (theme_picker(id, cfg.theme, themes, true, width, scale, picker)) writes.emplace_back("theme", toml_string(cfg.theme));
    return writes;
}

ConfigWrites draw_settings(bool& open, Config& cfg, const ThemeSet& themes, const FontIndex& fonts, void* hwnd,
                           float scale) {
    ConfigWrites writes;
    if (!open) return writes;
    ImGui::SetNextWindowSize(ImVec2(520 * scale, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * scale, 10 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale, 6 * scale));
    if (ImGui::Begin("Settings", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        const float label_w = 110 * scale;
        auto row = [&](const char* label) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine(label_w);
        };

        // --- theme ---------------------------------------------------------------
        row("Theme");
        if (theme_picker("##theme", cfg.theme, themes, true, 220 * scale, scale)) writes.emplace_back("theme", toml_string(cfg.theme));
        if (cfg.theme == "auto") {
            row("  when dark");
            if (theme_picker("##dark", cfg.dark_theme, themes, false, 220 * scale, scale))
                writes.emplace_back("dark_theme", toml_string(cfg.dark_theme));
            row("  when light");
            if (theme_picker("##light", cfg.light_theme, themes, false, 220 * scale, scale))
                writes.emplace_back("light_theme", toml_string(cfg.light_theme));
        }
        ImGui::Separator();

        // --- sizes ----------------------------------------------------------------
        row("Text size");
        ImGui::SetNextItemWidth(220 * scale);
        if (ImGui::SliderInt("##body", &cfg.body_size, kBodySizeMin, kBodySizeMax, "%d pt"))
            writes.emplace_back("body_size", toml_int(cfg.body_size));
        row("Column width");
        ImGui::SetNextItemWidth(220 * scale);
        if (ImGui::SliderInt("##maxw", &cfg.max_width, kMaxWidthMin, kMaxWidthMax, "%d pt")) {
            cfg.max_width = std::clamp((cfg.max_width + 10) / 20 * 20, kMaxWidthMin, kMaxWidthMax);  // steps of 20, like egui's slider
            writes.emplace_back("max_width", toml_int(cfg.max_width));
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The widest the text gets (max_width)");
        row("Zoom");
        ImGui::SetNextItemWidth(220 * scale);
        float zoom = float(cfg.zoom);
        if (ImGui::SliderFloat("##zoom", &zoom, nav::kZoomMin, nav::kZoomMax, "%.1f")) {
            cfg.zoom = nav::clamp_zoom(zoom);  // whole steps of 0.1
            writes.emplace_back("zoom", toml_float(cfg.zoom));
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset##zoom") && cfg.zoom != 1.0) {
            cfg.zoom = 1.0;
            writes.emplace_back("zoom", toml_float(cfg.zoom));
        }
        ImGui::Separator();

        // --- fonts ------------------------------------------------------------------
        font_family_field("Prose font", "fonts.prose", cfg.fonts.prose, "Built-in (IBM Plex Sans)", fonts, false, hwnd,
                          scale, writes);
        font_family_field("Code font", "fonts.mono", cfg.fonts.mono, "Built-in (Cascadia Code)", fonts, true, hwnd, scale, writes);
        // The optional style overrides: the same dropdown as the prose row, where
        // "" = Automatic (as in the Rust port).
        if (ImGui::TreeNode("More")) {
            ImGui::TextDisabled("Automatic = the text font's own style (a family's bold face, ...),\n"
                                "the text font file itself, or the built-in style.");
            font_family_field("Bold", "fonts.prose_bold", cfg.fonts.prose_bold, "Automatic", fonts, false, hwnd, scale,
                              writes);
            font_family_field("Italic", "fonts.prose_italic", cfg.fonts.prose_italic, "Automatic", fonts, false, hwnd,
                              scale, writes);
            font_family_field("Bold italic", "fonts.prose_bold_italic", cfg.fonts.prose_bold_italic, "Automatic", fonts,
                              false, hwnd, scale, writes);
            ImGui::TreePop();
        }
        ImGui::Separator();

        // --- toggles ------------------------------------------------------------------
        checkbox("Vim keys", "vim_keys", cfg.vim_keys, writes);
        checkbox("Use system fonts for missing characters", "system_fallback", cfg.system_fallback, writes);
        checkbox("Show outline at start-up", "show_outline", cfg.show_outline, writes);
        checkbox("Remember window size and position", "remember_window", cfg.remember_window, writes);
        checkbox("Code line numbers", "code.line_numbers", cfg.code_line_numbers, writes);
        checkbox("Show the cursor marker in editor previews", "editor_marker", cfg.editor_marker, writes);
        ImGui::Separator();

        // --- files ----------------------------------------------------------------------
        if (ImGui::Button("Open config file")) {
            const fs::path path = config_path();
            std::error_code ec;
            if (!fs::exists(path, ec)) platform::write_file_atomic(path, config_file_text(cfg));
            platform::open_path(path);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open themes folder")) {
            const fs::path dir = config_dir() / "themes";
            std::error_code ec;
            fs::create_directories(dir, ec);
            platform::open_path(dir);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    return writes;
}
