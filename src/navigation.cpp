#include "navigation.h"

#include <algorithm>
#include <cmath>

namespace nav {

// --- zoom ---------------------------------------------------------------------

float clamp_zoom(float zoom) {
    if (!std::isfinite(zoom)) return 1.0f;
    // Whole steps of 0.1: avoids 1.2000001 creeping in after many presses.
    zoom = std::round(zoom / kZoomStep) * kZoomStep;
    return std::clamp(zoom, kZoomMin, kZoomMax);
}

float step_zoom(float zoom, int steps) { return clamp_zoom(zoom + steps * kZoomStep); }

// --- vim keys -------------------------------------------------------------------

void VimKeys::reset() {
    count_ = 0;
    prefix_ = 0;
}

std::string VimKeys::pending() const {
    std::string s = count_ > 0 ? std::to_string(count_) : "";
    if (prefix_) s += char(prefix_);
    return s;
}

std::optional<Command> VimKeys::feed(const KeyInput& key, double now, bool vim_enabled) {
    // A pending count / prefix expires after 1.5 s.
    if ((count_ || prefix_) && now - last_ > kTimeout) reset();

    // Esc with keys pending only cancels them (spec 6b: one thing per Esc).
    if (key.named == Named::Escape && (count_ || prefix_)) {
        reset();
        return std::nullopt;
    }
    const int count = std::max(count_, 1);
    auto done = [&](Action a, int n = 1) {
        reset();
        return Command{a, n};
    };

    // --- standard keys: always on --------------------------------------------
    if (key.named != Named::None) {
        // A named key never continues a `g` / `[` / `]` prefix (it cancels it),
        // but a typed count still applies to motions (5<Down>).
        prefix_ = 0;
        switch (key.named) {
            case Named::Down: return done(Action::LineDown, count);
            case Named::Up: return done(Action::LineUp, count);
            case Named::PageDown: return done(Action::PageDown, count);
            case Named::PageUp: return done(Action::PageUp, count);
            case Named::Space: return done(key.shift ? Action::PageUp : Action::PageDown, count);
            case Named::Home: return done(Action::Top);
            case Named::End: return done(Action::Bottom);
            case Named::Escape: return done(Action::Escape);
            case Named::F5: return done(Action::Reload);
            case Named::None: break;
        }
        return std::nullopt;
    }
    if (key.ctrl) {
        switch (key.ch) {
            case 'f': case 'F': return done(Action::OpenFind);
            case 'b': case 'B': return done(Action::ToggleOutline);
            case 'd': case 'D': if (vim_enabled) return done(Action::HalfPageDown, count); break;
            case 'u': case 'U': if (vim_enabled) return done(Action::HalfPageUp, count); break;
            case 'e': case 'E': if (vim_enabled) return done(Action::OneLineDown, count); break;
            case 'y': case 'Y': if (vim_enabled) return done(Action::OneLineUp, count); break;
            default: break;
        }
        reset();  // any other key cancels what was pending
        return std::nullopt;
    }
    if (!vim_enabled) return std::nullopt;

    // --- vim keys -------------------------------------------------------------
    const char32_t c = key.ch;

    // Second key of a two-key sequence.
    if (prefix_) {
        const char32_t p = prefix_;
        prefix_ = 0;
        if (p == 'g' && c == 'g') return done(Action::Top);
        if (p == 'g' && c == 't') return done(Action::NextTab, std::max(count_, 1));  // spec 34
        if (p == 'g' && c == 'T') return done(Action::PrevTab, std::max(count_, 1));
        if (p == ']' && c == ']') return done(Action::NextHeading, count);
        if (p == '[' && c == '[') return done(Action::PrevHeading, count);
        // Marks (spec 28): any letter names one.
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (p == 'm' && letter) return reset(), Command{Action::SetMark, 1, c};
        if (p == '\'' && c == '\'') return done(Action::HistoryBack);  // `''`: back to before the jump
        if (p == '\'' && letter) return reset(), Command{Action::GoMark, 1, c};
        // Anything else: the sequence is dropped (count too) and the key is
        // handled on its own below, so `g j` still scrolls.
        count_ = 0;
    }

    // Counts: 1-9 start one, 0 only continues one (a lone 0 does nothing).
    if (c >= '0' && c <= '9' && (c != '0' || count_ > 0)) {
        count_ = std::min(count_ * 10 + int(c - '0'), kMaxCount);
        last_ = now;
        return std::nullopt;
    }

    switch (c) {
        case 'j': return done(Action::LineDown, std::max(count_, 1));
        case 'k': return done(Action::LineUp, std::max(count_, 1));
        case 'd': return done(Action::HalfPageDown, std::max(count_, 1));
        case 'u': return done(Action::HalfPageUp, std::max(count_, 1));
        case 'f': return done(Action::PageDown, std::max(count_, 1));
        case 'b': return done(Action::PageUp, std::max(count_, 1));
        case 'G': return done(Action::Bottom);
        case 'n': return done(Action::NextMatch, std::max(count_, 1));
        case 'N': return done(Action::PrevMatch, std::max(count_, 1));
        case '/': return done(Action::OpenFind);
        case 'o': return done(Action::ToggleOutline);
        case 'r': return done(Action::Reload);
        case 'q': return done(Action::Quit);
        case 'H': return done(Action::HistoryBack, std::max(count_, 1));
        case 'L': return done(Action::HistoryForward, std::max(count_, 1));
        case '*': return done(Action::FindWordNext);
        case '#': return done(Action::FindWordPrev);
        case 't': return done(Action::ThemePicker);  // (after a pending `g` it was handled above: next tab)
        case 'g':
        case '[':
        case ']':
        case 'm':
        case '\'':
            prefix_ = c;  // wait for the second key (the count stays)
            last_ = now;
            return std::nullopt;
        default:
            reset();  // any other key cancels a pending count
            return std::nullopt;
    }
}

// --- heading jumps -----------------------------------------------------------------

std::optional<float> heading_target(const std::vector<float>& tops, float current, int count) {
    if (tops.empty() || count == 0) return std::nullopt;
    const float eps = 1.0f;  // "at the top" tolerance, in px
    if (count > 0) {
        // First heading strictly below the current position, then count-1 more.
        auto it = std::upper_bound(tops.begin(), tops.end(), current + eps);
        if (it == tops.end()) return std::nullopt;
        size_t i = size_t(it - tops.begin()) + size_t(count - 1);
        return tops[std::min(i, tops.size() - 1)];
    }
    // Last heading strictly above, then |count|-1 more.
    auto it = std::lower_bound(tops.begin(), tops.end(), current - eps);
    if (it == tops.begin()) return std::nullopt;
    std::ptrdiff_t i = (it - tops.begin()) - 1 - std::ptrdiff_t(-count - 1);
    return tops[size_t(std::max<std::ptrdiff_t>(i, 0))];
}

// --- autoscroll ----------------------------------------------------------------------

float autoscroll_velocity(float dy) {
    const float dead = 10.0f, cap = 6000.0f;
    const float beyond = std::abs(dy) - dead;  // distance outside the dead zone
    if (beyond <= 0) return 0.0f;
    return std::copysign(std::min(std::pow(beyond, 1.5f) * 0.6f, cap), dy);
}

void Autoscroll::middle_pressed(float x, float y, double now) {
    if (active_) {  // a press while toggled on just turns it off
        active_ = toggle_ = false;
        return;
    }
    active_ = true;
    toggle_ = false;
    ax_ = x;
    ay_ = y;
    moved_ = 0;
    pressed_at_ = now;
}

void Autoscroll::middle_released(double now) {
    if (!active_ || toggle_) return;
    if (now - pressed_at_ < kClickTime && moved_ < kClickMove) toggle_ = true;  // a click: stay on
    else active_ = false;                                                      // hold: done
}

void Autoscroll::mouse_moved(float x, float y) {
    if (active_ && !toggle_) moved_ = std::max(moved_, std::hypot(x - ax_, y - ay_));
}

void Autoscroll::other_button_pressed() {
    if (toggle_) active_ = toggle_ = false;
}

void Autoscroll::wheel() {
    if (toggle_) active_ = toggle_ = false;
}

void Autoscroll::escape() { active_ = toggle_ = false; }

ImVec2 lightbox_size(float nat_w, float nat_h, float win_w, float win_h, bool caption, float max_scale) {
    const float room_w = win_w - 2 * (kLightboxMargin + kLightboxPad);
    const float room_h = win_h - 2 * (kLightboxMargin + kLightboxPad) - (caption ? kLightboxCaption : 0.0f);
    const float s = std::min({room_w / std::max(nat_w, 1.0f), room_h / std::max(nat_h, 1.0f), max_scale});
    return ImVec2(std::max(nat_w * s, 1.0f), std::max(nat_h * s, 1.0f));
}

float Autoscroll::scroll_delta(float mouse_y, float dt) const {
    return active_ ? autoscroll_velocity(mouse_y - ay_) * dt : 0.0f;
}

}  // namespace nav
