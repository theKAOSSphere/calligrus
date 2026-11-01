// The window layer of the Android build: see window_android.h.
//
// One full-screen window: the EGL context lives as long as the process (so the GL textures of pictures,
// formulas and the font atlas survive the app going to the background); only the EGL surface comes and
// goes with the Android window. While there is no window, the event loop sleeps.
//
// Fingers: the first finger decides where a touch belongs. Starting on the document: a gesture tracker
// (touch.h) turns it into scrolling, a fling and zoom for main.cpp, and taps into mouse clicks. Starting
// anywhere else (toolbar, outline, a popup): a second tracker, whose taps click ImGui's widgets and whose
// drags turn the mouse wheel (lists scroll with the finger).
#include "window_android.h"

#include <EGL/egl.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <game-activity/GameActivity.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include <utility>

#include <unistd.h>

#include <imgui_internal.h>  // (the windows: which list is under a finger, its scroll offset)

#include "../touch.h"
#include "shell.h"

#define LOG_TAG "calligrus"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

struct Window {};  // GLFWwindow is opaque: the one window is this object
Window g_window;

struct Egl {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLConfig config = nullptr;
    EGLSurface surface = EGL_NO_SURFACE;
} g_egl;

ALooper* g_looper = nullptr;
std::atomic<bool> g_wake{false};
std::function<void()> g_background_hook;  // flushes the config and the state file (set by main.cpp)
bool g_woken = false;                     // the last wait ended for a reason other than its timer
bool g_legacy_power = false;              // dev switch: file `power_legacy` in the app home = the pre-pass-5 loop (16 ms polls, 0.5 s idle frames)
bool g_power_log = false;                 // dev switch: file `power_log` = one log line per wait, frame and skipped wake
unsigned g_polls = 0, g_waits = 0, g_frames = 0, g_skipped = 0;
int g_last_w = 1, g_last_h = 1;
std::string g_drop_path;

// The callbacks main.cpp installs; they only note that input arrived.
GLFWerrorfun g_cb_error = nullptr;
GLFWwindowsizefun g_cb_size = nullptr;
GLFWdropfun g_cb_drop = nullptr;
GLFWcursorposfun g_cb_cursor = nullptr;
GLFWmousebuttonfun g_cb_mouse = nullptr;
GLFWscrollfun g_cb_scroll = nullptr;
GLFWkeyfun g_cb_key = nullptr;
GLFWcharfun g_cb_char = nullptr;
GLFWwindowfocusfun g_cb_focus = nullptr;

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
const double g_start = now_s();

float density() {
    android_app* app = shell::app();
    if (!app || !app->config) return 1.0f;
    const int32_t dpi = AConfiguration_getDensity(app->config);
    if (dpi <= 0 || dpi == ACONFIGURATION_DENSITY_DEFAULT || dpi == ACONFIGURATION_DENSITY_NONE || dpi == ACONFIGURATION_DENSITY_ANY) return 1.0f;
    return float(dpi) / 160.0f;
}

// ---------------------------------------------------------------------------
// EGL
// ---------------------------------------------------------------------------

bool egl_create_context() {
    g_egl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_egl.display == EGL_NO_DISPLAY || !eglInitialize(g_egl.display, nullptr, nullptr)) {
        LOGE("eglInitialize failed (0x%x)", eglGetError());
        return false;
    }
    // RGBA8 window surface for OpenGL ES 3; no depth or stencil: ImGui draws flat triangles in order.
    const EGLint attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                              EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLint count = 0;
    if (!eglChooseConfig(g_egl.display, attribs, &g_egl.config, 1, &count) || count < 1) {
        LOGE("no OpenGL ES 3 EGL config (0x%x)", eglGetError());
        return false;
    }
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g_egl.context = eglCreateContext(g_egl.display, g_egl.config, EGL_NO_CONTEXT, ctx_attribs);
    if (g_egl.context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed (0x%x)", eglGetError());
        return false;
    }
    return true;
}

void query_size() {
    if (g_egl.surface == EGL_NO_SURFACE) return;
    EGLint w = 0, h = 0;
    eglQuerySurface(g_egl.display, g_egl.surface, EGL_WIDTH, &w);
    eglQuerySurface(g_egl.display, g_egl.surface, EGL_HEIGHT, &h);
    if (w > 0 && h > 0) g_last_w = w, g_last_h = h;
}

void notify_size() {
    query_size();
    if (g_cb_size) g_cb_size(reinterpret_cast<GLFWwindow*>(&g_window), g_last_w, g_last_h);
}

// A surface for the Android window (if there is one and we have none): the context draws into it.
void attach_surface() {
    android_app* app = shell::app();
    if (!app || !app->window || g_egl.context == EGL_NO_CONTEXT || g_egl.surface != EGL_NO_SURFACE) return;
    g_egl.surface = eglCreateWindowSurface(g_egl.display, g_egl.config, app->window, nullptr);
    if (g_egl.surface == EGL_NO_SURFACE || !eglMakeCurrent(g_egl.display, g_egl.surface, g_egl.surface, g_egl.context)) {
        LOGE("EGL surface setup failed (0x%x)", eglGetError());
        return;
    }
    eglSwapInterval(g_egl.display, 1);
    notify_size();
}

// The window goes away: keep the context (surfaceless), drop the surface.
void release_surface() {
    if (g_egl.surface == EGL_NO_SURFACE) return;
    eglMakeCurrent(g_egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, g_egl.context);
    eglDestroySurface(g_egl.display, g_egl.surface);
    g_egl.surface = EGL_NO_SURFACE;
}

void on_cmd(android_app*, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW: attach_surface(); break;
        case APP_CMD_TERM_WINDOW:
            if (g_background_hook) g_background_hook();  // (the surface is gone: the app is leaving the screen)
            release_surface();
            break;
        case APP_CMD_PAUSE:
        case APP_CMD_STOP:
        case APP_CMD_SAVE_STATE:
            // The process may be killed from here on without another word: nothing may stay unwritten (spec 35).
            if (g_background_hook) g_background_hook();
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONTENT_RECT_CHANGED:
        case APP_CMD_CONFIG_CHANGED: notify_size(); break;
        case APP_CMD_GAINED_FOCUS:
            if (g_cb_focus) g_cb_focus(reinterpret_cast<GLFWwindow*>(&g_window), 1);
            break;
        default: break;
    }
}

// Handles lifecycle events (and wakes). `timeout_ms` -1 waits for the first one. Returns how many events it handled.
// Touches, keys and text of GameActivity's glue each write a command to the looper's pipe (APP_CMD_TOUCH_EVENT, ...),
// so a wait here is ended by input: no polling is needed.
int pump(int timeout_ms) {
    android_app* app = shell::app();
    android_poll_source* source = nullptr;
    int events = 0, handled = 0;
    while (ALooper_pollOnce(timeout_ms, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
        if (source) source->process(app, source), handled++;
        if (app->destroyRequested) break;
        timeout_ms = 0;
    }
    attach_surface();  // (a window that came without a command we saw)
    return handled;
}

// ---------------------------------------------------------------------------
// Fingers
// ---------------------------------------------------------------------------

enum Target { kNone, kDocument, kWidgets };

touch::Tracker* g_doc = nullptr;  // created with the window (it needs the screen density)
touch::Tracker* g_ui = nullptr;
// Sideways scrolling of a block under the finger: g_h is a tracker fed with x as its "y" (same slop, velocity and fling).
// Both g_doc and g_h see the gesture until one of them crosses the slop first: that axis is kept, the other is cancelled.
touch::Tracker* g_h = nullptr;
std::vector<android_shell::HScrollBlock> g_hblocks;
bool g_hcand = false;       // the finger went down on a block that scrolls sideways
int g_hlock = 0;            // 0 undecided, 1 vertical (the page), 2 horizontal (the block)
const void* g_hkey = nullptr;
Target g_target = kNone;
float g_region[4] = {0, 0, 0, 0};
bool g_ui_only = false;
android_shell::DocTouch g_doc_touch;
std::deque<std::pair<float, float>> g_clicks;  // taps waiting to be clicked into ImGui
int g_press_phase = 0;                         // 1: a resting finger holds the button down (see ImGui_ImplGlfw_NewFrame)
int g_click_phase = 0;                         // 1: the button is down (release it next frame), 2: move the pointer away
bool g_hover_dirty = false;                    // the pointer sits on a widget after a drag: move it away
bool g_raw = false;                            // the settings window is open: a sideways drag is a mouse drag (sliders)
bool g_rawdrag = false;                        // ...and one is under way: the finger is the mouse until it lifts
ImGuiID g_scroll_id = 0;                       // the window a finger on the widgets scrolls (0: none), found at the touch-down

touch::Tracker* tracker() { return g_target == kDocument ? g_doc : g_target == kWidgets ? g_ui : nullptr; }

bool in_region(float x, float y) { return !g_ui_only && x >= g_region[0] && x < g_region[2] && y >= g_region[1] && y < g_region[3]; }

// An event's time on our clock. It is CLOCK_MONOTONIC like std::chrono::steady_clock, but GameActivity has
// given it in nanoseconds in some versions and in milliseconds in others (and 0 for some injected events):
// take the reading that is close to now, else now.
double event_time(int64_t raw) {
    const double now = now_s();
    for (const double unit : {1e-9, 1e-3})
        if (std::fabs(double(raw) * unit - now) < 5.0) return double(raw) * unit;
    return now;
}

// A mouse or trackpad (not a finger, not a pen)? Its events go to ImGui as the mouse, not into the touch trackers.
bool is_mouse(const GameActivityMotionEvent& e) {
    if ((e.source & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE) return true;
    return e.pointerCount > 0 && GameActivityPointerAxes_getToolType(&e.pointers[0]) == AMOTION_EVENT_TOOL_TYPE_MOUSE;
}

// The source ImGui should see for the input that follows: tooltips only show for the mouse (spec 35).
void source_touch(ImGuiIO& io) { io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen); }

// A real mouse (or trackpad): hover, buttons and the wheel, as on the desktop.
void handle_mouse(const GameActivityMotionEvent& e) {
    if (!ImGui::GetCurrentContext() || e.pointerCount == 0) return;
    ImGuiIO& io = ImGui::GetIO();
    const int action = e.action & AMOTION_EVENT_ACTION_MASK;
    const float x = GameActivityPointerAxes_getX(&e.pointers[0]), y = GameActivityPointerAxes_getY(&e.pointers[0]);
    io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
    if (action == AMOTION_EVENT_ACTION_HOVER_EXIT) {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        return;
    }
    io.AddMousePosEvent(x, y);
    const int b = e.buttonState;
    io.AddMouseButtonEvent(0, (b & AMOTION_EVENT_BUTTON_PRIMARY) != 0);
    io.AddMouseButtonEvent(1, (b & AMOTION_EVENT_BUTTON_SECONDARY) != 0);
    io.AddMouseButtonEvent(2, (b & AMOTION_EVENT_BUTTON_TERTIARY) != 0);
    if (action == AMOTION_EVENT_ACTION_SCROLL) {  // (Android and ImGui both count a notch up as positive)
        const float v = GameActivityPointerAxes_getAxisValue(&e.pointers[0], AMOTION_EVENT_AXIS_VSCROLL);
        const float h = GameActivityPointerAxes_getAxisValue(&e.pointers[0], AMOTION_EVENT_AXIS_HSCROLL);
        if (v != 0 || h != 0) io.AddMouseWheelEvent(h, v);
    }
}

// Touch debug log: a file `touch_log` in the app's home folder switches it on (checked at every press), so a
// session can record what the fingers of a real device really send and the lift velocity worked out from it.
//   adb shell run-as <package> touch files/touch_log     then   adb logcat -s calligrus:I | grep touch
bool g_touch_log = false;

void check_touch_log() {
    const char* home = getenv("HOME");
    g_touch_log = home && access((std::string(home) + "/touch_log").c_str(), F_OK) == 0;
}

void log_sample(const char* what, int id, float x, float y, double t, bool historical, int64_t raw) {
    LOGI("touch %s id=%d x=%.1f y=%.1f t=%.3f hist=%d raw=%lld", what, id, x, y, t * 1000.0, historical ? 1 : 0, static_cast<long long>(raw));
}

void log_lift(const touch::Tracker& tr) {
    const touch::Lift& l = tr.last_lift();
    if (!l.valid) return;
    LOGI("touch lift: samples=%d lsq2=%.0f impulse=%.0f clamped=%.0f stale=%d fling=%d px/s", l.samples, l.lsq2, l.impulse, l.clamped, l.stale ? 1 : 0, l.flung ? 1 : 0);
}

// The window a drag at (x, y) scrolls: the front-most window there, or the nearest parent of it (a child window) that has
// something to scroll. Popups do not climb to the window that opened them. 0 if none (the document is not one: it has its own).
ImGuiID scroll_window_at(float x, float y) {
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g) return 0;
    for (int i = g->Windows.Size - 1; i >= 0; i--) {
        ImGuiWindow* w = g->Windows[i];
        if (!w->WasActive || w->Hidden || (w->Flags & ImGuiWindowFlags_NoInputs) || !w->Rect().Contains(ImVec2(x, y))) continue;
        for (; w; w = (w->Flags & ImGuiWindowFlags_ChildWindow) ? w->ParentWindow : nullptr)
            if (w->ScrollMax.y > 0 && !(w->Flags & ImGuiWindowFlags_NoScrollWithMouse)) return w->ID;
        return 0;
    }
    return 0;
}

// The content of the list under the finger moved `dy` px with it (touch::scroll_list): written to the window's offset,
// which ImGui clamps again in Begin. A fling stops at an end.
void scroll_list_window(float dy) {
    ImGuiWindow* w = g_scroll_id ? ImGui::FindWindowByID(g_scroll_id) : nullptr;
    if (!w || !w->WasActive) {
        g_ui->stop_fling();
        return;
    }
    bool end = false;
    w->Scroll.y = touch::scroll_list(w->Scroll.y, dy, w->ScrollMax.y, &end);
    if (end) g_ui->stop_fling();
}

void handle_motion(const GameActivityMotionEvent& e) {
    if (is_mouse(e)) {
        handle_mouse(e);
        return;
    }
    const int action = e.action & AMOTION_EVENT_ACTION_MASK;
    const int index = (e.action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    const double t = event_time(e.eventTime);
    auto xy = [&](int i, float& x, float& y) {
        x = GameActivityPointerAxes_getX(&e.pointers[i]);
        y = GameActivityPointerAxes_getY(&e.pointers[i]);
    };
    float x = 0, y = 0;
    if (action == AMOTION_EVENT_ACTION_DOWN) check_touch_log();
    switch (action) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            if (index >= int(e.pointerCount)) return;
            xy(index, x, y);
            if (action == AMOTION_EVENT_ACTION_DOWN || g_target == kNone) g_target = in_region(x, y) ? kDocument : kWidgets;
            if (g_touch_log) log_sample(action == AMOTION_EVENT_ACTION_DOWN ? "down" : "pointer-down", e.pointers[index].id, x, y, t, false, e.eventTime);
            if (action == AMOTION_EVENT_ACTION_POINTER_DOWN && g_hlock == 2) return;  // (a second finger on a block being dragged: ignored)
            if (action == AMOTION_EVENT_ACTION_DOWN) {  // a touch stops a sideways fling too; on a wide block it may start a sideways drag
                g_h->cancel();
                g_hcand = false, g_hlock = 0, g_hkey = nullptr;
                if (g_target == kDocument)
                    for (auto it = g_hblocks.rbegin(); it != g_hblocks.rend(); ++it)
                        if (x >= it->x0 && x < it->x1 && y >= it->y0 && y < it->y1) {
                            g_hcand = true, g_hkey = it->key;
                            g_h->down(e.pointers[index].id, y, x, t);
                            break;
                        }
            } else if (g_hcand) {  // two fingers: a pinch, not a block scroll
                g_h->cancel();
                g_hcand = false;
            }
            if (g_target == kWidgets && action == AMOTION_EVENT_ACTION_DOWN) {
                g_scroll_id = scroll_window_at(x, y);
                g_rawdrag = false;
            }
            if (auto* tr = tracker()) tr->down(e.pointers[index].id, x, y, t);
            break;
        case AMOTION_EVENT_ACTION_MOVE:
            if (g_rawdrag && ImGui::GetCurrentContext()) {  // a slider drag in the settings window
                xy(0, x, y);
                source_touch(ImGui::GetIO());
                ImGui::GetIO().AddMousePosEvent(x, y);
            } else if (auto* tr = tracker()) {
                // A batch holds the samples since the last event (oldest first, each with its own time), then the
                // current one: all of them go to the velocity tracker, as Android's own views do with a MotionEvent's
                // history. (GameActivity gives every time in milliseconds.)
                for (int h = 0; h < e.historySize; h++) {
                    const int64_t raw = e.historicalEventTimesNanos ? e.historicalEventTimesNanos[h] : e.historicalEventTimesMillis ? e.historicalEventTimesMillis[h] * 1000000 : 0;
                    const double th = raw ? event_time(raw) : t;
                    for (uint32_t i = 0; i < e.pointerCount; i++) {
                        const float hx = GameActivityMotionEvent_getHistoricalX(&e, int(i), h), hy = GameActivityMotionEvent_getHistoricalY(&e, int(i), h);
                        if (g_touch_log) log_sample("move", e.pointers[i].id, hx, hy, th, true, raw);
                        if (g_hcand && g_hlock != 1) g_h->move(e.pointers[i].id, hy, hx, th);
                        if (g_hlock != 2) tr->move(e.pointers[i].id, hx, hy, th);
                    }
                }
                for (uint32_t i = 0; i < e.pointerCount; i++) {
                    xy(int(i), x, y);
                    if (g_touch_log) log_sample("move", e.pointers[i].id, x, y, t, false, e.eventTime);
                    if (g_hcand && g_hlock != 1) g_h->move(e.pointers[i].id, y, x, t);
                    if (g_hlock != 2) tr->move(e.pointers[i].id, x, y, t);
                }
                if (g_hcand && g_hlock == 0 && g_target == kDocument) {  // direction lock, as in Android views: the first axis past the slop
                    const bool vd = g_doc->dragging(), hd = g_h->dragging();
                    xy(0, x, y);
                    if (hd && (!vd || std::fabs(x - g_doc->start_x()) > std::fabs(y - g_doc->start_y()))) {
                        g_hlock = 2;
                        g_doc->cancel();
                    } else if (vd) {
                        g_hlock = 1;
                        g_h->cancel();
                    }
                }
                if (g_target == kWidgets && g_raw && tr->horizontal() && ImGui::GetCurrentContext()) {
                    // The settings window has sliders: a sideways drag is the mouse from where the finger went down.
                    ImGuiIO& io = ImGui::GetIO();
                    source_touch(io);
                    if (g_press_phase != 1) {
                        io.AddMousePosEvent(tr->start_x(), tr->start_y());
                        io.AddMouseButtonEvent(0, true);
                        g_press_phase = 1;
                    }
                    xy(0, x, y);
                    io.AddMousePosEvent(x, y);
                    g_rawdrag = true;
                    tr->cancel();
                }
            }
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
            if (index >= int(e.pointerCount)) return;
            xy(index, x, y);
            if (g_touch_log) log_sample(action == AMOTION_EVENT_ACTION_UP ? "up" : "pointer-up", e.pointers[index].id, x, y, t, false, e.eventTime);
            if (g_rawdrag && ImGui::GetCurrentContext()) {
                source_touch(ImGui::GetIO());
                ImGui::GetIO().AddMousePosEvent(x, y);
                ImGui::GetIO().AddMouseButtonEvent(0, false);
                g_press_phase = 0;
                g_rawdrag = false;
                g_hover_dirty = true;
            } else if (g_hlock == 2) {
                g_h->up(e.pointers[index].id, y, x, t);
                if (g_touch_log && action == AMOTION_EVENT_ACTION_UP) log_lift(*g_h);
            } else if (auto* tr = tracker()) {
                tr->up(e.pointers[index].id, x, y, t);
                if (g_touch_log && action == AMOTION_EVENT_ACTION_UP) log_lift(*tr);
                if (g_hcand) g_h->cancel();  // (a tap on the block is the document tap)
            }
            if (action == AMOTION_EVENT_ACTION_UP || e.pointerCount <= 1) g_target = kNone;
            break;
        case AMOTION_EVENT_ACTION_CANCEL:
            if (g_doc) g_doc->cancel();
            if (g_ui) g_ui->cancel();
            if (g_h) g_h->cancel();
            g_hcand = false, g_hlock = 0;
            if (g_rawdrag && ImGui::GetCurrentContext()) {
                ImGui::GetIO().AddMouseButtonEvent(0, false);
                g_press_phase = 0;
                g_hover_dirty = true;
            }
            g_rawdrag = false;
            g_target = kNone;
            break;
        default: break;
    }
}

void handle_key(const GameActivityKeyEvent& e, ImGuiIO& io) {
    const bool down = e.action == AKEY_EVENT_ACTION_DOWN;
    ImGuiKey key = ImGuiKey_None;
    switch (e.keyCode) {
        case AKEYCODE_DEL: key = ImGuiKey_Backspace; break;
        case AKEYCODE_FORWARD_DEL: key = ImGuiKey_Delete; break;
        case AKEYCODE_ENTER: key = ImGuiKey_Enter; break;
        case AKEYCODE_TAB: key = ImGuiKey_Tab; break;
        case AKEYCODE_SPACE: key = ImGuiKey_Space; break;
        case AKEYCODE_DPAD_LEFT: key = ImGuiKey_LeftArrow; break;
        case AKEYCODE_DPAD_RIGHT: key = ImGuiKey_RightArrow; break;
        case AKEYCODE_DPAD_UP: key = ImGuiKey_UpArrow; break;
        case AKEYCODE_DPAD_DOWN: key = ImGuiKey_DownArrow; break;
        case AKEYCODE_PAGE_UP: key = ImGuiKey_PageUp; break;
        case AKEYCODE_PAGE_DOWN: key = ImGuiKey_PageDown; break;
        case AKEYCODE_MOVE_HOME: key = ImGuiKey_Home; break;
        case AKEYCODE_MOVE_END: key = ImGuiKey_End; break;
        case AKEYCODE_ESCAPE: key = ImGuiKey_Escape; break;
        default: break;
    }
    if (e.keyCode >= AKEYCODE_A && e.keyCode <= AKEYCODE_Z) key = ImGuiKey(ImGuiKey_A + (e.keyCode - AKEYCODE_A));  // for Ctrl+F etc.
    if (e.keyCode >= AKEYCODE_0 && e.keyCode <= AKEYCODE_9) key = ImGuiKey(ImGuiKey_0 + (e.keyCode - AKEYCODE_0));
    io.AddKeyEvent(ImGuiMod_Ctrl, (e.metaState & AMETA_CTRL_ON) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (e.metaState & AMETA_SHIFT_ON) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (e.metaState & AMETA_ALT_ON) != 0);
    if (key != ImGuiKey_None) io.AddKeyEvent(key, down);
    if (down && e.unicodeChar >= 0x20 && !(e.metaState & AMETA_CTRL_ON)) io.AddInputCharacter(unsigned(e.unicodeChar));
}

// Takes what arrived since last time: fingers into the trackers, keys into ImGui. The only place that
// swaps GameActivity's input buffers (swapping twice would throw events away). True if there was any.
bool process_input() {
    android_app* app = shell::app();
    android_input_buffer* in = android_app_swap_input_buffers(app);
    if (!in) return false;
    const bool any = in->motionEventsCount > 0 || in->keyEventsCount > 0;
    for (uint64_t i = 0; i < in->motionEventsCount; i++) handle_motion(in->motionEvents[i]);
    if (in->keyEventsCount > 0 && ImGui::GetCurrentContext()) {
        ImGuiIO& io = ImGui::GetIO();
        for (uint64_t i = 0; i < in->keyEventsCount; i++) handle_key(in->keyEvents[i], io);
    }
    android_app_clear_motion_events(in);
    android_app_clear_key_events(in);
    if (any) {
        auto* w = reinterpret_cast<GLFWwindow*>(&g_window);
        if (g_cb_mouse) g_cb_mouse(w, 0, 0, 0);
        if (in->keyEventsCount > 0 && g_cb_key) g_cb_key(w, 0, 0, 0, 0);
    }
    return any;
}

// Documents that came from Java (picker, "Open with", share): open like a dropped file.
bool deliver_opened() {
    bool any = false;
    while (auto p = shell::take_opened_path()) {
        g_drop_path = *p;
        const char* paths[1] = {g_drop_path.c_str()};
        if (g_cb_drop) g_cb_drop(reinterpret_cast<GLFWwindow*>(&g_window), 1, paths);
        any = true;
    }
    return any;
}

bool should_close() { return shell::app()->destroyRequested != 0; }

// No window (the app is in the background): sleep until there is one, or the app is being closed.
void wait_for_window() {
    while (g_egl.surface == EGL_NO_SURFACE && !should_close()) pump(-1);
}

}  // namespace

// ---------------------------------------------------------------------------
// The GLFW subset
// ---------------------------------------------------------------------------

int glfwInit() {
    g_looper = ALooper_forThread();
    if (const char* home = getenv("HOME")) {
        g_legacy_power = access((std::string(home) + "/power_legacy").c_str(), F_OK) == 0;
        g_power_log = access((std::string(home) + "/power_log").c_str(), F_OK) == 0;
    }
    shell::app()->onAppCmd = on_cmd;
    android_app_set_motion_event_filter(shell::app(), nullptr);  // (the default lets only the touch screen through: a mouse needs the rest)
    // GameActivity copies only the X and Y axes of a pointer unless asked: the wheel's two are read in handle_mouse.
    GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_VSCROLL);
    GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_HSCROLL);
    return 1;
}

void glfwTerminate() {
    release_surface();
    if (g_egl.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_egl.context != EGL_NO_CONTEXT) eglDestroyContext(g_egl.display, g_egl.context);
        eglTerminate(g_egl.display);
    }
    g_egl = Egl();
    delete g_doc, delete g_ui;
    g_doc = g_ui = nullptr;
}

GLFWwindow* glfwCreateWindow(int, int, const char*, GLFWmonitor*, GLFWwindow*) {
    if (!egl_create_context()) return nullptr;
    const touch::Params p = touch::Params::for_density(density());  // (ViewConfiguration's dp limits, spec 35)
    g_doc = new touch::Tracker(p);
    g_ui = new touch::Tracker(p);
    g_h = new touch::Tracker(p);
    // The first frame needs a window: wait for Android to give us one.
    while (!shell::app()->window && !should_close()) pump(-1);
    attach_surface();
    return reinterpret_cast<GLFWwindow*>(&g_window);
}

void glfwDestroyWindow(GLFWwindow*) {}
void glfwMakeContextCurrent(GLFWwindow*) {}
void glfwSwapInterval(int) {}

void glfwSwapBuffers(GLFWwindow*) {
    if (g_egl.surface != EGL_NO_SURFACE && !eglSwapBuffers(g_egl.display, g_egl.surface))
        LOGE("eglSwapBuffers failed (0x%x)", eglGetError());  // the surface was taken away: APP_CMD_TERM_WINDOW follows
}

void glfwGetFramebufferSize(GLFWwindow*, int* w, int* h) {
    query_size();
    *w = g_last_w, *h = g_last_h;
}
void glfwGetWindowSize(GLFWwindow* win, int* w, int* h) { glfwGetFramebufferSize(win, w, h); }
void glfwGetWindowContentScale(GLFWwindow*, float* x, float* y) {
    const float d = density();
    if (x) *x = d;
    if (y) *y = d;
}

int glfwWindowShouldClose(GLFWwindow*) { return should_close(); }
void glfwSetWindowShouldClose(GLFWwindow*, int) { GameActivity_finish(shell::app()->activity); }

void glfwPollEvents() {
    wait_for_window();
    pump(0);
    process_input();
    deliver_opened();
}

// Sleep until something happens or `seconds` have passed (more than a day: no timer at all). What wakes us: input
// (the glue writes a command to the looper for every touch and key), a document from Java, a job's glfwPostEmptyEvent,
// a window event. Nothing else runs in the meantime: an idle app costs no CPU (spec 35).
void glfwWaitEventsTimeout(double seconds) {
    const double t0 = now_s(), deadline = t0 + seconds;
    const char* why = "timer";
    unsigned polls = 0;
    bool woke = false;
    for (;;) {
        wait_for_window();
        const double left = deadline - now_s();
        if (left <= 0 || should_close()) break;
        int ms = seconds > 86400.0 ? -1 : std::max(1, int(std::ceil(left * 1000.0)));
        if (g_legacy_power) ms = std::min(16, ms);  // (the old loop: "fingers and keys do not wake the looper")
        polls++;
        const int handled = pump(ms);
        const bool input = process_input();
        const bool opened = deliver_opened();
        const bool wake = g_wake.exchange(false);
        if (input || opened || wake || handled > 0) {
            woke = true;
            why = input ? "input" : opened ? "document" : wake ? "job" : "window";
            break;
        }
    }
    g_woken = woke;
    g_polls += polls;
    g_waits++;
    if (g_power_log) LOGI("power: wait over after %.0f ms (%u looper polls): %s", (now_s() - t0) * 1000.0, polls, why);
}

void glfwPostEmptyEvent() {
    g_wake = true;
    if (g_looper) ALooper_wake(g_looper);
}

double glfwGetTime() { return now_s() - g_start; }
GLFWglproc glfwGetProcAddress(const char* name) { return reinterpret_cast<GLFWglproc>(eglGetProcAddress(name)); }

GLFWerrorfun glfwSetErrorCallback(GLFWerrorfun f) { return std::exchange(g_cb_error, f); }
GLFWwindowsizefun glfwSetWindowSizeCallback(GLFWwindow*, GLFWwindowsizefun f) { return std::exchange(g_cb_size, f); }
GLFWdropfun glfwSetDropCallback(GLFWwindow*, GLFWdropfun f) { return std::exchange(g_cb_drop, f); }
GLFWcursorposfun glfwSetCursorPosCallback(GLFWwindow*, GLFWcursorposfun f) { return std::exchange(g_cb_cursor, f); }
GLFWmousebuttonfun glfwSetMouseButtonCallback(GLFWwindow*, GLFWmousebuttonfun f) { return std::exchange(g_cb_mouse, f); }
GLFWscrollfun glfwSetScrollCallback(GLFWwindow*, GLFWscrollfun f) { return std::exchange(g_cb_scroll, f); }
GLFWkeyfun glfwSetKeyCallback(GLFWwindow*, GLFWkeyfun f) { return std::exchange(g_cb_key, f); }
GLFWcharfun glfwSetCharCallback(GLFWwindow*, GLFWcharfun f) { return std::exchange(g_cb_char, f); }
GLFWwindowfocusfun glfwSetWindowFocusCallback(GLFWwindow*, GLFWwindowfocusfun f) { return std::exchange(g_cb_focus, f); }

// ---------------------------------------------------------------------------
// ImGui's platform side
// ---------------------------------------------------------------------------

bool ImGui_ImplGlfw_InitForOpenGL(GLFWwindow*, bool) {
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
    return true;
}

void ImGui_ImplGlfw_Shutdown() {}

void ImGui_ImplGlfw_NewFrame() {
    ImGuiIO& io = ImGui::GetIO();
    query_size();
    io.DisplaySize = ImVec2(float(g_last_w), float(g_last_h));
    io.DisplayFramebufferScale = ImVec2(1, 1);
    static double last = 0;
    const double now = now_s();
    io.DeltaTime = last > 0 ? float(std::clamp(now - last, 0.001, 5.0)) : 1.0f / 60.0f;  // (the loop may sleep for minutes: the clock must follow)
    last = now;

    // Fingers -> what main.cpp should do (the document) and what ImGui should see (everything else).
    const touch::Step doc = g_doc->update(now);
    g_doc_touch.scroll_dy = doc.move_y;
    g_doc_touch.zoom = doc.zoom;
    g_doc_touch.pinching = doc.pinching;
    g_doc_touch.focus_x = doc.focus_x, g_doc_touch.focus_y = doc.focus_y;
    g_doc_touch.fling_active = g_doc->flinging();
    g_doc_touch.fling_dy = doc.fling_y;
    g_doc_touch.fling_v = g_doc->fling_speed(), g_doc_touch.fling_total = g_doc->fling_total();
    for (const touch::Tap& t : doc.taps) g_clicks.push_back({t.x, t.y});
    const touch::Step hs = g_h->update(now);  // (its taps and presses do not matter: the document tracker has them)
    g_doc_touch.h_key = g_hkey;
    g_doc_touch.h_dx = hs.move_y;
    const touch::Step ui = g_ui->update(now);
    for (const touch::Tap& t : ui.taps) g_clicks.push_back({t.x, t.y});
    // A drag (or the fling after it) on a list: the list under the finger scrolls 1:1, like a native one. The pointer does not
    // move: no hover over the items that pass (it stays off-screen until a press shows or a tap clicks). Two fingers: nothing.
    if (ui.move_y != 0 && !ui.pinching) scroll_list_window(ui.move_y);
    // A finger that rests on a widget for 90 ms shows the press (button down, held); a drag or a long press
    // lets go away from the widget (no click); a tap lets go on it (a click).
    // (the touch source is set only where a finger makes the mouse events: a real mouse keeps its own, spec 35)
    const bool finger_mouse = (ui.pressing && g_press_phase == 0 && g_click_phase == 0 && g_clicks.empty()) || g_press_phase == 1 || g_click_phase != 0 ||
                              !g_clicks.empty() || (g_hover_dirty && !g_ui->busy() && !g_rawdrag);
    if (finger_mouse) source_touch(io);
    if (ui.pressing && g_press_phase == 0 && g_click_phase == 0 && g_clicks.empty()) {
        io.AddMousePosEvent(ui.press_x, ui.press_y);
        io.AddMouseButtonEvent(0, true);
        g_press_phase = 1;
    } else if (g_press_phase == 1 && !ui.pressing && !g_rawdrag) {
        if (!ui.taps.empty()) {  // lifted in time: the click
            io.AddMousePosEvent(ui.taps[0].x, ui.taps[0].y);
            g_clicks.clear();
        } else {  // a drag started or the press was too long: let go elsewhere
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        }
        io.AddMouseButtonEvent(0, false);
        g_press_phase = 0;
        g_hover_dirty = true;
    } else if (g_click_phase == 1) {
        io.AddMouseButtonEvent(0, false);
        g_click_phase = 2;
    } else if (g_click_phase == 2) {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        g_click_phase = 0;
        g_hover_dirty = false;
    } else if (!g_clicks.empty()) {
        io.AddMousePosEvent(g_clicks.front().first, g_clicks.front().second);
        io.AddMouseButtonEvent(0, true);
        g_clicks.pop_front();
        g_click_phase = 1;
    } else if (g_hover_dirty && !g_ui->busy() && !g_rawdrag) {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        g_hover_dirty = false;
    }
}

// ---------------------------------------------------------------------------
// android_shell
// ---------------------------------------------------------------------------

namespace android_shell {

Insets insets() {
    Insets r;
    android_app* app = shell::app();
    if (!app || !app->activity) return r;
    auto take_max = [&r](const ARect& a) {  // per edge, the larger of the system bars and the cut-out
        r.left = std::max(r.left, float(a.left)), r.top = std::max(r.top, float(a.top));
        r.right = std::max(r.right, float(a.right)), r.bottom = std::max(r.bottom, float(a.bottom));
    };
    ARect bars{}, cutout{};
    GameActivity_getWindowInsets(app->activity, GAMECOMMON_INSETS_TYPE_SYSTEM_BARS, &bars);
    GameActivity_getWindowInsets(app->activity, GAMECOMMON_INSETS_TYPE_DISPLAY_CUTOUT, &cutout);
    take_max(bars);
    take_max(cutout);
    return r;
}

DocTouch doc_touch() { return g_doc_touch; }

void set_hscroll_blocks(const std::vector<HScrollBlock>& blocks) { g_hblocks = blocks; }
void doc_touch_hit_end_h() {
    if (g_h) g_h->stop_fling();
}

void doc_touch_hit_end() {
    if (g_doc) g_doc->stop_fling();
}

void set_doc_region(float x0, float y0, float x1, float y1, bool ui_only) {
    g_region[0] = x0, g_region[1] = y0, g_region[2] = x1, g_region[3] = y1;
    g_ui_only = ui_only;
}

void set_raw_touch(bool raw) { g_raw = raw; }

bool touch_busy() {
    return (g_doc && g_doc->busy()) || (g_ui && g_ui->busy()) || (g_h && g_h->busy()) || g_click_phase != 0 || g_press_phase != 0 || !g_clicks.empty() || g_hover_dirty || g_rawdrag;
}

void set_light_bars(bool light) { shell::set_light_bars(light); }

void set_background_hook(std::function<void()> hook) { g_background_hook = std::move(hook); }
bool legacy_power() { return g_legacy_power; }
bool consume_woken() { return std::exchange(g_woken, false); }
void count_frame() {
    g_frames++;
    if (g_power_log) LOGI("power: frame %u (waits %u, skipped wakes %u, looper polls %u)", g_frames, g_waits, g_skipped, g_polls);
}
void count_skipped() {
    g_skipped++;
    if (g_power_log) LOGI("power: wake without a frame %u", g_skipped);
}

}  // namespace android_shell

namespace shell {
void wake() { glfwPostEmptyEvent(); }
}  // namespace shell
