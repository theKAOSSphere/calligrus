// Keyboard and mouse navigation logic, kept free of ImGui and the window so it
// can be tested from the console (calligrus-nav-test): zoom steps, the vim-style
// key state machine, heading jumps and browser-style middle-click autoscroll.
// main.cpp feeds these with ImGui's input and turns the results into scrolling.
// (Spec: the Rust port sections 1-3.)
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

namespace nav {

// ---------------------------------------------------------------------------
// Zoom (spec 1)
// ---------------------------------------------------------------------------

constexpr float kZoomMin = 0.5f;
constexpr float kZoomMax = 3.0f;
constexpr float kZoomStep = 0.1f;

// Clamp to 0.5 ... 3.0 and round to a whole step (so 0.1 + 0.1 + 0.1 is exactly 0.3).
float clamp_zoom(float zoom);

// `steps` notches in (+) or out (-) from `zoom`.
float step_zoom(float zoom, int steps);

// The zoomed picture (spec 27, click a picture or diagram; zoom.rs): space around the card, the
// card's padding (it has the page's background, so diagrams drawn in the text colour show), and
// the caption line under it (the alt text). Points.
constexpr float kLightboxMargin = 40.0f, kLightboxPad = 12.0f, kLightboxCaption = 32.0f;
// How far a picture / a diagram is enlarged at most (times its natural size).
constexpr float kLightboxMaxPicture = 4.0f, kLightboxMaxDiagram = 3.0f;
// The size of a nat_w x nat_h picture zoomed in a win_w x win_h window (all in the same unit, points
// at scale 1): as big as fits inside the margins and padding (less the caption line if there is one),
// at most `max_scale` times its natural size. Never below 1.
ImVec2 lightbox_size(float nat_w, float nat_h, float win_w, float win_h, bool caption, float max_scale);

// ---------------------------------------------------------------------------
// Vim-style keys (spec 2)
// ---------------------------------------------------------------------------

// What a key (sequence) asks for. Counts are carried separately in Command.
enum class Action {
    LineDown, LineUp,          // j k, Down Up: 3 body lines
    HalfPageDown, HalfPageUp,  // d u, Ctrl+D Ctrl+U
    PageDown, PageUp,          // f b, Space Shift+Space, PageDown PageUp
    Top, Bottom,               // g g, G, Home End
    NextHeading, PrevHeading,  // ] ]  [ [
    OpenFind,                  // /, Ctrl+F
    NextMatch, PrevMatch,      // n N
    ToggleOutline,             // o, Ctrl+B
    Reload,                    // r, F5
    Escape,                    // Esc: close find bar / cancel pending keys / clear selection
    OneLineDown, OneLineUp,    // Ctrl+E Ctrl+Y: one body line
    Quit,                      // q
    HistoryBack, HistoryForward,  // H L (Alt+Left / Alt+Right and mouse buttons are handled in main.cpp); `''` = back
    SetMark, GoMark,           // m + letter, ' + letter (spec 28): the letter is Command::mark
    FindWordNext, FindWordPrev,  // * #: find the selected text, or the word under the mouse
    NextTab, PrevTab,            // g t, g T (spec 34): count = how many tabs to move (wrapping)
    ThemePicker,                 // t (not in slides mode, where it is the clock): open the toolbar's theme picker (spec 6)
};

struct Command {
    Action action;
    int count = 1;  // how many times (5j = LineDown x5)
    char32_t mark = 0;  // SetMark / GoMark: the mark's letter
};

// Keys that aren't typed characters.
enum class Named { None, Down, Up, PageDown, PageUp, Space, Home, End, Escape, F5 };

// One key press, as main.cpp sees it: either a typed character (`ch`, already
// layout-aware: 'G', '[', '/') or a named key, plus modifiers.
struct KeyInput {
    char32_t ch = 0;
    Named named = Named::None;
    bool ctrl = false;
    bool shift = false;
};

// The key state machine: counts (`5j`), two-key sequences (`gg`, `]]`, `[[`)
// and their 1.5 s timeout. Standard keys (arrows, Page keys, Space, Home/End,
// Ctrl+F, Ctrl+B, F5, Esc) work even when vim keys are off.
class VimKeys {
public:
    // Feed one key at time `now` (seconds). Returns the command it completes, if any.
    std::optional<Command> feed(const KeyInput& key, double now, bool vim_enabled);
    // Forget any pending count / prefix (e.g. when a text field takes focus).
    void reset();
    // What's pending, for display/tests ("5", "g", "3]"); empty if nothing.
    std::string pending() const;

    static constexpr double kTimeout = 1.5;  // seconds a count / prefix stays pending
    static constexpr int kMaxCount = 999;

private:
    int count_ = 0;        // 0 = no count typed
    char32_t prefix_ = 0;  // 'g', '[', ']', 'm' or '\'' waiting for its second key
    double last_ = 0;      // time of the last pending key
};

// ---------------------------------------------------------------------------
// Heading jumps (]] and [[)
// ---------------------------------------------------------------------------

// Given the scroll offsets that put each heading at the top of the view
// (ascending), and the current offset, where `count` headings down (count > 0)
// or up (count < 0) is. nullopt = no heading in that direction.
std::optional<float> heading_target(const std::vector<float>& tops, float current, int count);

// ---------------------------------------------------------------------------
// Middle-click autoscroll (spec 3)
// ---------------------------------------------------------------------------

// Scroll speed in px/s for the mouse being `dy` px below (+) / above (-) the anchor:
// sign(dy) * max(0, |dy| - 10)^1.5 * 0.6, capped at 6000 px/s.
float autoscroll_velocity(float dy);

// Browser-style autoscroll: press the middle button, then move the mouse
// away from the press point to scroll. A quick click (released within
// 250 ms, moved < 6 px) leaves it on ("toggle" mode) until the next mouse
// press, wheel or Esc; otherwise it ends when the button is released ("hold").
class Autoscroll {
public:
    bool active() const { return active_; }
    bool toggle_mode() const { return toggle_; }
    float anchor_x() const { return ax_; }
    float anchor_y() const { return ay_; }

    // Middle button pressed at (x, y) over the document.
    void middle_pressed(float x, float y, double now);
    // Middle button released.
    void middle_released(double now);
    // Mouse moved (tracks how far, to tell a click from a drag).
    void mouse_moved(float x, float y);
    // Left/right button press, wheel, Esc: end toggle mode. (Esc ends hold mode too.)
    void other_button_pressed();
    void wheel();
    void escape();
    // How far to scroll this frame (px, + = down) for the mouse at `mouse_y`.
    float scroll_delta(float mouse_y, float dt) const;

    static constexpr double kClickTime = 0.25;  // s
    static constexpr float kClickMove = 6.0f;   // px

private:
    bool active_ = false, toggle_ = false;
    float ax_ = 0, ay_ = 0;  // anchor (press point)
    float moved_ = 0;        // furthest distance from the anchor since the press
    double pressed_at_ = 0;
};

}  // namespace nav
