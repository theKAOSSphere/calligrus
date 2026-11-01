// calligrus entry point: window + GPU setup (GLFW + OpenGL 3), the toolbar,
// the find bar, the outline panel, keyboard/mouse navigation, the config file,
// and the frame loop. The interesting drawing code lives in render.cpp /
// inline_layout.cpp; parsing in model.cpp; input logic in navigation.cpp.
//
// The look deliberately copies the Rust/egui version: same toolbar controls,
// egui-like rounding and spacing, colours from the active theme (theme.h).
#include <cstdint>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <deque>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>  // ImGuiInputTextState (the find field's selection)
#include <imgui_impl_opengl3.h>

#ifdef __ANDROID__
// Android: the window layer (src/android/window_android.h) declares the part of GLFW used here.
#include <android/log.h>
#include "android/shell.h"
#include "android/window_android.h"
#ifndef APIENTRY
#define APIENTRY
#endif
constexpr const char* kGlslVersion = "#version 300 es";
#else
#include <imgui_impl_glfw.h>

#include <GLFW/glfw3.h>
constexpr const char* kGlslVersion = "#version 330";
#endif
#ifdef _WIN32
// glfw3native.h pulls in <windows.h>; NOMINMAX stops it defining min/max
// macros that would break std::min / std::max.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>  // glfwGetWin32Window
// The native window handle platform:: wants (an HWND; nothing on other systems).
static void* native_window(GLFWwindow* w) { return glfwGetWin32Window(w); }
#else
#include <unistd.h>
#include <cerrno>
static void* native_window(GLFWwindow*) { return nullptr; }
#endif

#include "config.h"
#include "editor.h"
#include "find.h"
#include "font_index.h"
#include "trace.h"
#include "sysfont.h"
#include "fonts.h"
#include "highlight.h"
#include "images.h"
#include "marks.h"
#include "inline_layout.h"
#include "math.h"
#include "model.h"
#include "org.h"
#include "pages.h"
#include "pdf.h"
#include "text_capture.h"
#include "slides.h"
#include "navigation.h"
#include "platform.h"
#include "render.h"
#include "resources.h"
#include "script.h"
#include "session.h"
#include "userdir.h"
#include "settings.h"
#include "theme.h"

#include <stb_image.h>        // the window icon (implementation in images.cpp)
#include <stb_image_write.h>  // implementation in debug_capture.cpp

// Debugging aid: with CALLIGRUS_CAPTURE=<file.png> set, save the frame the app
// itself rendered (straight from OpenGL, before Windows composites it) a few
// frames after start-up, plus a <file.png>.txt note on sizes and how many
// vertices sit off the pixel grid, then quit. Used to chase text blur.
static void debug_capture(int fb_w, int fb_h) {
#pragma warning(suppress : 4996)  // MSVC: getenv/fopen are fine for a debug switch
    static const char* path = std::getenv("CALLIGRUS_CAPTURE");
    static int frame = 0;
    if (!path || ++frame < 30) return;

    std::vector<unsigned char> px(size_t(fb_w) * fb_h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fb_w, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    stbi_flip_vertically_on_write(1);  // OpenGL rows start at the bottom
    stbi_write_png(path, fb_w, fb_h, 4, px.data(), fb_w * 4);

    // Textured vertices (glyphs, images) at fractional positions get resampled.
    const ImDrawData* dd = ImGui::GetDrawData();
    // Only textured vertices (uv away from the atlas's white pixel = glyphs);
    // plain shapes have fractional anti-aliasing fringes on purpose.
    const ImVec2 white = ImGui::GetIO().Fonts->TexUvWhitePixel;
    // Thick lines are drawn with a textured strip from the atlas (TexUvLines);
    // those have fractional positions on purpose too, so they're counted apart.
    const ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    auto is_line_texture = [&](ImVec2 uv) {
        for (const ImVec4& r : atlas->TexUvLines)
            if (uv.x >= std::min(r.x, r.z) - 1e-6f && uv.x <= std::max(r.x, r.z) + 1e-6f && uv.y >= std::min(r.y, r.w) - 1e-6f &&
                uv.y <= std::max(r.y, r.w) + 1e-6f)
                return true;
        return false;
    };
    int verts = 0, frac_x = 0, frac_y = 0, line_frac = 0;
    std::string samples;
    for (const ImDrawList* dl : dd->CmdLists)
        for (const ImDrawVert& v : dl->VtxBuffer) {
            if (std::fabs(v.uv.x - white.x) < 1e-6f && std::fabs(v.uv.y - white.y) < 1e-6f) continue;
            const bool fx = std::fabs(v.pos.x - std::round(v.pos.x)) > 0.01f;
            const bool fy = std::fabs(v.pos.y - std::round(v.pos.y)) > 0.01f;
            if (is_line_texture(v.uv)) {
                line_frac += fx || fy;
                continue;
            }
            verts++;
            frac_x += fx;
            frac_y += fy;
            if ((fx || fy) && samples.size() < 600) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "  off-grid vertex at (%.2f, %.2f) uv (%.4f, %.4f)\n", v.pos.x, v.pos.y, v.uv.x, v.uv.y);
                samples += buf;
            }
        }
#pragma warning(suppress : 4996)
    if (FILE* f = std::fopen((std::string(path) + ".txt").c_str(), "w")) {
        std::fprintf(f, "framebuffer %dx%d, DisplaySize %.2fx%.2f, DisplayPos %.2f,%.2f, FramebufferScale %.3f,%.3f\n",
                     fb_w, fb_h, dd->DisplaySize.x, dd->DisplaySize.y, dd->DisplayPos.x, dd->DisplayPos.y,
                     dd->FramebufferScale.x, dd->FramebufferScale.y);
        std::fprintf(f, "vertices %d, fractional x %d, fractional y %d (anti-aliased line strips off the grid: %d, expected)\n%s",
                     verts, frac_x, frac_y, line_frac, samples.c_str());
        std::fclose(f);
    }
    std::exit(0);
}

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// At start-up, how long to wait (seconds, window still hidden) for the font
// index when config.toml names a font family, before showing the built-in
// faces and applying the named one when the scan finishes.
constexpr double kFontIndexStartupWait = 1.0;

static const char* kSample = R"(# calligrus

A tiny **native** Markdown viewer written in *C++20* with Dear ImGui.

Open a file with **Ctrl+O**, drop one onto the window, or pass a path on the command line.
)";

// ImGui wants UTF-8 `char` strings; std::filesystem gives char8_t on C++20.
static std::string to_utf8(const fs::path& p) {
    std::u8string u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

// A path as shown to the user (toolbar, recent files, tooltips, messages). Normally the path itself.
// Test-only: with CALLIGRUS_DISPLAY_ROOT=<dir> set, paths under <dir> are shown relative to it
// (forward slashes), so screenshots made on different machines are identical (spec 20).
static std::string display_path(const fs::path& p) {
#pragma warning(suppress : 4996)  // MSVC: getenv is fine for a test switch
    static const char* root_env = std::getenv("CALLIGRUS_DISPLAY_ROOT");
    if (root_env && *root_env && !p.empty()) {
        std::error_code ec;
        const fs::path root = fs::absolute(fs::path(std::u8string(root_env, root_env + std::strlen(root_env))), ec).lexically_normal();
        const fs::path abs = fs::absolute(p, ec).lexically_normal();
        const fs::path rel = abs.lexically_relative(root);
        if (!ec && !rel.empty() && *rel.begin() != "..") {
            const std::u8string u8 = rel.generic_u8string();
            return std::string(u8.begin(), u8.end());
        }
    }
    return to_utf8(p);
}

// ---------------------------------------------------------------------------
// Headless script mode (spec 14): no state/config writes, no dialogs, no
// external launches; output goes to the parent console and <script>.log.
// ---------------------------------------------------------------------------

static bool g_script_mode = false;
static bool g_editor = false;  // --editor: live preview driven by an editor on stdin (spec 15)
static fs::path g_script_log;

// One line to stdout (or stderr) and the log file. Never a message box.
static void script_log(const std::string& line, bool is_err = false) {
#ifdef _WIN32
    static bool attached = false;
    if (!attached) {
        attached = true;
        // Only when a stream was not redirected (to a file or a pipe, e.g. by an editor or a test
        // harness): then the parent's console is the place for it, if there is one.
        auto missing = [](DWORD which) {
            const HANDLE h = GetStdHandle(which);
            return h == nullptr || h == INVALID_HANDLE_VALUE;
        };
        const bool out_missing = missing(STD_OUTPUT_HANDLE), err_missing = missing(STD_ERROR_HANDLE);
        if ((out_missing || err_missing) && AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* o = nullptr;
            if (out_missing) freopen_s(&o, "CONOUT$", "w", stdout);
            if (err_missing) freopen_s(&o, "CONOUT$", "w", stderr);
        }
    }
#endif
    std::fprintf(is_err ? stderr : stdout, "%s\n", line.c_str());
    std::fflush(is_err ? stderr : stdout);
    if (!g_script_log.empty()) {
        std::ofstream f(g_script_log, std::ios::binary | std::ios::app);
        f << line << "\n";
    }
}

// Messages to the editor (spec 23): one JSON line each, nothing else is ever written to stdout in
// editor mode. Script mode: appended to <out>/editor-out.jsonl (and kept for `expect-editor`).
static fs::path g_editor_out;
static std::vector<std::string> g_editor_lines;
#ifdef _WIN32
// Our own stdout pipe, taken before anything (script_log's AttachConsole + freopen) can replace the
// CRT's stdout. Both exes are GUI-subsystem: there is a handle only if the editor redirected it.
static const HANDLE g_stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
#endif
static void editor_emit(const std::string& json) {
    const std::string line = json + "\n";
    if (g_script_mode) {
        g_editor_lines.push_back(json);
        std::ofstream f(g_editor_out, std::ios::binary | std::ios::app);
        f << line;
        return;
    }
#ifdef _WIN32
    if (g_stdout_handle == nullptr || g_stdout_handle == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(g_stdout_handle, line.data(), DWORD(line.size()), &written, nullptr);  // one write: one whole line
#else
    for (size_t off = 0; off < line.size();) {
        const ssize_t n = write(1, line.data() + off, line.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        off += size_t(n);
    }
#endif
}

// The state file is not written in script mode.
static void save_state_guarded(const fs::path& file, const session::State& state, bool remember_window) {
    if (!g_script_mode) session::save_state(file, state, remember_window);
}

// Offscreen render target (FBO) for script mode: works for a hidden window and
// a locked workstation. FBO entry points come from glfwGetProcAddress.
struct Offscreen {
    using GenFn = void(APIENTRY*)(GLsizei, GLuint*);
    using BindFn = void(APIENTRY*)(GLenum, GLuint);
    using StorageFn = void(APIENTRY*)(GLenum, GLenum, GLsizei, GLsizei);
    using AttachFn = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint);
    using StatusFn = GLenum(APIENTRY*)(GLenum);
    static constexpr GLenum kFramebuffer = 0x8D40, kRenderbuffer = 0x8D41, kColor0 = 0x8CE0, kRgba8 = 0x8058, kComplete = 0x8CD5;
    GenFn gen_fb = nullptr, gen_rb = nullptr;
    BindFn bind_fb = nullptr, bind_rb = nullptr;
    StorageFn storage = nullptr;
    AttachFn attach = nullptr;
    StatusFn status = nullptr;
    GLuint fbo = 0, rb = 0;
    int w = 0, h = 0;

    bool load() {
        gen_fb = GenFn(glfwGetProcAddress("glGenFramebuffers"));
        gen_rb = GenFn(glfwGetProcAddress("glGenRenderbuffers"));
        bind_fb = BindFn(glfwGetProcAddress("glBindFramebuffer"));
        bind_rb = BindFn(glfwGetProcAddress("glBindRenderbuffer"));
        storage = StorageFn(glfwGetProcAddress("glRenderbufferStorage"));
        attach = AttachFn(glfwGetProcAddress("glFramebufferRenderbuffer"));
        status = StatusFn(glfwGetProcAddress("glCheckFramebufferStatus"));
        return gen_fb && gen_rb && bind_fb && bind_rb && storage && attach && status;
    }
    // Bind a target of this size (re-allocated when the size changes).
    bool use(int nw, int nh) {
        if (!fbo) {
            gen_fb(1, &fbo);
            gen_rb(1, &rb);
        }
        bind_fb(kFramebuffer, fbo);
        if (nw != w || nh != h) {
            bind_rb(kRenderbuffer, rb);
            storage(kRenderbuffer, kRgba8, nw, nh);
            attach(kFramebuffer, kColor0, kRenderbuffer, rb);
            w = nw;
            h = nh;
        }
        return status(kFramebuffer) == kComplete;
    }
};

// A script `keys` token -> ImGui key (+ whether shift is implied by the character).
static ImGuiKey imgui_key_for(const script::KeyToken& k, bool& implied_shift) {
    implied_shift = false;
    if (!k.named.empty()) {
        static const std::pair<const char*, ImGuiKey> named[] = {
            {"esc", ImGuiKey_Escape}, {"enter", ImGuiKey_Enter}, {"tab", ImGuiKey_Tab}, {"space", ImGuiKey_Space},
            {"up", ImGuiKey_UpArrow}, {"down", ImGuiKey_DownArrow}, {"left", ImGuiKey_LeftArrow},
            {"right", ImGuiKey_RightArrow}, {"pageup", ImGuiKey_PageUp}, {"pagedown", ImGuiKey_PageDown},
            {"home", ImGuiKey_Home}, {"end", ImGuiKey_End}, {"f5", ImGuiKey_F5}, {"f11", ImGuiKey_F11},
            {"backspace", ImGuiKey_Backspace}};
        for (auto& [n, key] : named)
            if (k.named == n) return key;
        return ImGuiKey_None;
    }
    const char32_t c = k.ch;
    if (c >= U'a' && c <= U'z') return ImGuiKey(ImGuiKey_A + int(c - U'a'));
    if (c >= U'A' && c <= U'Z') return implied_shift = true, ImGuiKey(ImGuiKey_A + int(c - U'A'));
    if (c >= U'0' && c <= U'9') return ImGuiKey(ImGuiKey_0 + int(c - U'0'));
    struct Sym {
        char32_t c;
        ImGuiKey key;
        bool shift;
    };
    static const Sym syms[] = {
        {U'/', ImGuiKey_Slash, false},        {U'?', ImGuiKey_Slash, true},        {U'[', ImGuiKey_LeftBracket, false},
        {U'{', ImGuiKey_LeftBracket, true},   {U']', ImGuiKey_RightBracket, false}, {U'}', ImGuiKey_RightBracket, true},
        {U'-', ImGuiKey_Minus, false},        {U'_', ImGuiKey_Minus, true},        {U'=', ImGuiKey_Equal, false},
        {U'+', ImGuiKey_Equal, true},         {U'.', ImGuiKey_Period, false},      {U'>', ImGuiKey_Period, true},
        {U',', ImGuiKey_Comma, false},        {U'<', ImGuiKey_Comma, true},        {U';', ImGuiKey_Semicolon, false},
        {U':', ImGuiKey_Semicolon, true},     {U'\'', ImGuiKey_Apostrophe, false}, {U'"', ImGuiKey_Apostrophe, true},
        {U'\\', ImGuiKey_Backslash, false},   {U'|', ImGuiKey_Backslash, true},    {U'`', ImGuiKey_GraveAccent, false},
        {U'~', ImGuiKey_GraveAccent, true}};
    for (const Sym& sy : syms)
        if (sy.c == c) return implied_shift = sy.shift, sy.key;
    return ImGuiKey_None;
}

// ---------------------------------------------------------------------------
// Document state
// ---------------------------------------------------------------------------

struct DocState {
    fs::path path;  // empty => the built-in demo (or an editor document: see `live`)
    std::string binary;  // not empty: the file is not text (what it looks like, see md::looks_binary): the view shows a note
    md::Document doc;
    std::vector<int> pauses;  // slides: source lines of `<!-- pause -->` / `# pause` (spec 26)
    std::vector<int> ends;    // slides: source lines of `<!-- end_slide -->` (spec 26)
    std::vector<std::pair<int, bool>> incremental;  // slides: `incremental_lists: true/false` lines (spec 26)
    std::vector<slides::Directive> layouts;  // slides: column directives (spec 26)
    std::vector<slides::Placement> columns;  // slides mode only: where each block goes (empty: one column)
    std::vector<slides::TextDirective> texts;  // slides: new_lines / alignment / font_size lines (spec 26)
    std::vector<slides::TextStyle> styles;     // slides mode only: each block's text style (empty: all plain)
    std::vector<char> backgrounds;             // slides mode only: which blocks are background pictures (empty: none)
    std::vector<std::pair<int, std::string>> notes;  // slides: speaker notes (spec 26)
    int line_count = 0;  // source lines (the last slide's lines run to here)
    size_t version = 0;  // bumped on every (re)load; invalidates the render cache
    fs::file_time_type mtime{};
    std::string error;
    RenderCache cache;

    // Smooth scrolling: we animate `cur` towards `target` instead of jumping.
    float scroll_target = 0, scroll_cur = 0, scroll_last_set = -1;
    float bar_prev_y = 0;     // last frame's scroll offset (to notice scrolling)
    double bar_until = 0;     // the scrollbar stays visible until this time (spec 21)
    double bar_wake = 0;      // when the idle loop must wake to hide the bar (0: no wake needed)
    float scroll_to = -1;     // set by the outline panel / find / heading jumps
    float scroll_now = 0;     // autoscroll: px to move this frame, without easing
    bool animating = false;

    // The document view as of last frame (for zoom anchoring and find scrolling).
    float view_w = 0, view_h = 0;
    float view_y = 0;     // the view's top edge on the screen (the pinch gesture needs it)
    float content_h = 0;  // scrollable height (explicit content size for ImGui)
    // Zoom / text size changed: keep this block (index, fraction of its height) at the top.
    // (Also how a mark is gone to, spec 28.)
    std::optional<std::pair<size_t, float>> zoom_anchor;
    float zoom_anchor_scale = 0;  // (if not 0: wait for the layout at this scale before putting the block back)
    float zoom_anchor_off = 0;  // ...at this many px below the top of the view (a pinch keeps the block under the fingers)
    // A pinch in progress (Android, spec 35): the laid-out document is drawn scaled by `pinch_s` about
    // `pinch_from`, moved to `pinch_to`; no layout happens until the fingers lift.
    float pinch_s = 1.0f;
    ImVec2 pinch_from{0, 0}, pinch_to{0, 0};

    // Marks (spec 28): each top-level block's first words (marks find their block by them), and
    // the margin's letters (label, block, fraction), worked out again when `marks_dirty`.
    std::vector<std::string> keys;
    struct MarkLabel {
        std::string label;
        size_t block;
        float frac;
    };
    std::vector<MarkLabel> mark_labels;
    bool marks_dirty = true;

    // Text selection (see select.h): this frame's painted runs, last frame's
    // (for hit-testing the mouse), and the drag/click state.
    Selection sel;
    Collector runs;
    size_t sel_key = 0;        // layout the selection belongs to (doc, width, text size)
    bool selecting = false;    // a drag-selection is in progress
    std::string press_link;    // link under the mouse when the button went down
    std::optional<LinkHit> press_zoom;  // picture / diagram under the mouse when it went down (spec 27)
    std::optional<LinkHit> lightbox;    // the picture / diagram shown zoomed (spec 27)
    std::string menu_math;     // the formula under the mouse when the right-click menu opened ("" = none)
    std::string toast;         // a short note at the bottom of the view ("Copied $x^2$")...
    double toast_since = 0;    // ...and when it appeared
    bool goto_press = false;   // a Ctrl+click in an editor preview is under way (spec 23): no selection, no link
    ImVec2 goto_from;          // ...where the button went down
    std::optional<ImVec2> goto_click;  // a finished Ctrl+click (screen coords), to be sent to the editor
    bool copy_requested = false;

    // Navigation requests, handled by the frame loop after drawing (it owns the history and state file).
    std::string link_request;       // a link was clicked
    bool link_new_tab = false;      // ...with Ctrl held (or the middle button): a link to a file opens a new tab (spec 34)
    bool press_new_tab = false;     // Ctrl was held when the button went down on `press_link`
    bool over_link = false;         // the pointer was over a link last frame (a middle-click then follows it instead of autoscrolling)
    fs::path open_request;          // the toolbar's Open button picked a file
    bool open_new_tab = false;      // ...to be opened in a new tab (Ctrl+T, Ctrl+Enter in the recent popup)
    std::string pending_anchor;     // heading slug to scroll to once the outline is known
    float restore_y = -1;           // scroll offset (px) to restore for a newly opened file; -1 = none

    // Editor live preview (spec 15): the shown document comes from the editor, not the disk.
    // `path` stays empty then, so nothing watches / remembers / reloads a file.
    bool live = false;
    bool live_open = false;         // the editor still sends updates (false after `close`: text stays, no " (live)")
    std::string live_id, live_text;
    std::string live_format;        // "org" or "markdown": fixed at `open` (spec 25)
    fs::path live_path;             // the buffer's file, "" if it has none (only for links, images and the title)
    // Scroll sync: the last `cursor` line (1-based), whether to act on it, and where in the view
    // (px from the top) the line sat after the last sync.
    int sync_line = 0;
    bool sync_pending = false;      // a `cursor` arrived: scroll (smoothly) if the line is off-centre
    bool sync_reapply = false;      // the text changed: put the last line back at sync_view_y, no animation
    float sync_view_y = -1;

    // Show editor text (a new document, or new text of the one already shown).
    // `format`: "org" / "markdown" / "" (by the file's extension); only the `open` call passes it.
    void show_live(const std::string& id, const fs::path& file, const std::string& text, const std::string& format = "") {
        const bool same = live && live_id == id;
        if (!same || !format.empty()) {
            live_format = format.empty() ? (md::is_org_path(to_utf8(file)) ? "org" : "markdown") : format;
        }
        live = live_open = true;
        live_id = id;
        live_path = file;
        live_text = text;
        path.clear();
        binary.clear();
        error.clear();
        set_doc(md::parse_format(text, live_format));
        read_directives(text, live_format == "org");
        if (same) {
            sync_reapply = sync_line > 0;
        } else {
            sync_line = 0, sync_view_y = -1, sync_pending = sync_reapply = false;
            restore_y = 0;
        }
    }

    // The slide directives of the source text (spec 26).
    void read_directives(std::string_view text, bool org) {
        pauses = slides::pause_lines(text, org);
        ends = slides::end_slide_lines(text, org);
        incremental = slides::incremental_lines(text, org);
        layouts = slides::layout_lines(text, org);
        texts = slides::text_lines(text, org);
        notes = slides::note_lines(text, org);
        line_count = int(std::count(text.begin(), text.end(), '\n')) + (!text.empty() && text.back() != '\n' ? 1 : 0);
    }

    // The slides of `doc`, and where their steps start.
    std::vector<slides::Range> split() const { return slides::split_with_ends(doc, ends); }
    std::vector<std::vector<slides::Step>> steps(const std::vector<slides::Range>& ranges) const {
        return slides::steps(ranges, doc, pauses, incremental);
    }

    void set_doc(md::Document d) {
        doc = std::move(d);
        keys.clear();
        for (const md::Block& b : doc) keys.push_back(marks::block_key(b));
        marks_dirty = true;
        pauses.clear();
        ends.clear();
        incremental.clear();
        texts.clear();
        styles.clear();
        backgrounds.clear();
        notes.clear();
        layouts.clear();
        columns.clear();  // (slides_prepare places the new blocks)
        sel.clear();
        version++;
    }

    bool load(const fs::path& p) {
        auto text = platform::read_file(p);
        if (!text) {
            error = "Could not read " + display_path(p);
            return false;
        }
        path = p;
        live = live_open = false;
        sync_line = 0, sync_pending = sync_reapply = false;
        const char* not_text = md::looks_binary(*text);
        binary = not_text ? not_text : "";
        if (not_text) *text = "";  // nothing to show but the note
        const bool org = md::is_org_path(to_utf8(p));
        set_doc(md::parse_format(*text, org ? "org" : "markdown"));
        read_directives(*text, org);
        std::error_code ec;
        mtime = fs::last_write_time(p, ec);
        error.clear();
        return true;
    }

    void reload() {
        if (!path.empty()) load(path);
        else if (live) show_live(live_id, live_path, live_text);  // the last text the editor sent
    }

    // Live reload: re-parse if the file changed on disk.
    void poll() {
        if (path.empty()) return;
        std::error_code ec;
        auto t = fs::last_write_time(path, ec);
        if (!ec && t != mtime) load(path);
    }
};

// Find in page (spec 7): the bar's text, and the matches for it.
struct FindState {
    bool open = false;
    bool focus = false;          // give the text field keyboard focus next frame
    char buf[256] = "";          // the text field
    std::string query;           // what `matches` were computed for
    size_t layout_key = 0;       // ...and for which layout
    std::vector<TextRange> matches;
    std::vector<std::pair<float, float>> match_y;  // top / bottom of each match (document y)
    int current = -1;
    bool scroll_to_current = false;
    // `*` / `#` (spec 28): the current match is the first one from this block on (+1) or the last
    // one up to it (-1), instead of the first one from the top of the view.
    std::optional<std::pair<uint32_t, int>> word_from;

    // How the bar was opened (spec 7). `/` (vim): Enter runs the search and gives the keys back to the document, the
    // query and highlights stay; the search starts at the view position where `/` was pressed. Ctrl+F: browser style
    // (Enter = next match, the field keeps focus).
    bool vim = false;
    float origin = 0;            // vim: the view's scroll position when `/` was pressed
    bool repick = false;         // vim: the next Enter picks the first match from `origin` again, even for an unchanged query
    bool select_all = false;     // select the field's text when it gets focus (`/` brings the last query back selected)
    bool enter_search = false;   // Enter was pressed in the field: search now (no waiting for typing to stop)
    std::string last_query;      // what Esc cleared: `/` puts it back in the field
    // Typing is debounced (spec 7): the search runs kFindDebounce s after the last edit of the field.
    std::string typed;           // the field's text as of the last frame (an edit shows as a difference)
    double typed_at = -1e9;      // ImGui time of the last edit
    double due = 0;              // > 0: a search is waiting, to run at this time

    // The document's searchable text, built once per layout (not per keystroke).
    FindIndex index;
    bool index_ok = false;
    size_t index_key = 0;

    void close() {
        if (buf[0]) last_query = buf;
        open = false;
        buf[0] = 0;  // closing clears the query, and so the highlights
        typed.clear();
        due = 0;
        repick = enter_search = false;
    }

    // The editor's search highlight (spec 17): all matches, no current one. The
    // find bar, while open, takes over; closing it shows these again.
    std::string ed_text;       // the editor's query ("" = none)
    bool ed_case = false;
    std::string ed_done;       // what ed_matches were computed for...
    bool ed_done_case = false;
    size_t ed_key = 0;         // ...and for which layout
    std::vector<TextRange> ed_matches;
};

// ---------------------------------------------------------------------------
// The config file: reading, watching, debounced in-place writing (spec 5)
// ---------------------------------------------------------------------------

struct ConfigFile {
    Config cfg;
    fs::path path;                               // the file in use (or where it would be created)
    fs::file_time_type mtime{};                  // of the file as we last read or wrote it
    std::map<std::string, std::string> pending;  // key -> TOML value, waiting to be written
    double due = 0;                              // when to write them (300 ms after the last change)
    bool readonly = false;                       // script mode: changes stay in memory

    // The file's exact bytes (no BOM stripping: the writer keeps a BOM).
    static std::optional<std::string> read_raw(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return std::nullopt;
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // (Re)read the file. Returns warnings (bad values...) for the error bar.
    std::vector<std::string> load() {
        std::vector<std::string> warnings;
        path = config_path();  // looked up again: the lookup order may now find another file
        std::error_code ec;
        mtime = fs::last_write_time(path, ec);
        if (ec) mtime = {};
        auto text = read_raw(path);
        cfg = text ? parse_config(*text, &warnings) : Config{};  // missing file: all defaults
        return warnings;
    }

    // Changed on disk by someone else (not by our own write)? Also true when
    // the lookup now finds a different file (one was created in a place that
    // comes earlier in the order, or the one in use was deleted).
    bool changed_on_disk() const {
        if (config_path() != path) return true;
        std::error_code ec;
        auto t = fs::last_write_time(path, ec);
        if (ec) t = {};
        return t != mtime;
    }

    void queue(const ConfigWrites& writes, double now) {
        if (readonly) return;
        for (const auto& [key, value] : writes) pending[key] = value;
        if (!writes.empty()) due = now + 0.3;
    }

    // Write what's pending once the debounce time has passed.
    void flush(double now, bool force = false) {
        if (readonly || pending.empty() || (!force && now < due)) return;
        const fs::path p = path.empty() ? config_path() : path;  // the file in use, or where to create it
        std::string text = read_raw(p).value_or("");
        const std::string before = text;
        for (const auto& [key, value] : pending) text = config_set(text, key, value);
        pending.clear();
        if (text == before && fs::exists(p)) return;
        if (platform::write_file_atomic(p, text)) {
            std::error_code ec;
            mtime = fs::last_write_time(p, ec);  // so the watcher ignores our own write
        }
    }
};

// ---------------------------------------------------------------------------
// Theme -> ImGui style
// ---------------------------------------------------------------------------

static ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

static void apply_style(const Theme& t, float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    // egui spacing: item_spacing (8,3), button_padding (4,1), corner radius 2.
    s.ItemSpacing = ImVec2(8, 3);
    s.ItemInnerSpacing = ImVec2(4, 3);
    s.FramePadding = ImVec2(4, 1);
#ifdef __ANDROID__
    s.FramePadding = ImVec2(10, 9);  // finger-sized buttons (the toolbar grows with them)
    s.ItemSpacing = ImVec2(10, 6);
    s.ScrollbarSize = 14;
#endif
    // Tooltips look like egui's popup frame: 6px margin, 6px corners, thin border.
    // (The main full-window panel pushes zero padding for itself.)
    s.WindowPadding = ImVec2(6, 6);
    s.FrameRounding = 2;
    s.GrabRounding = 2;
    s.PopupRounding = 6;
    s.PopupBorderSize = 1;
    s.WindowRounding = 6;
    s.ChildRounding = 0;
    s.WindowBorderSize = 1;
    s.ChildBorderSize = 0;
    s.ScrollbarSize = 10;
    s.ScrollbarRounding = 5;
    s.ScaleAllSizes(scale);

    // Everything not set below stays transparent.
    ImVec4* c = s.Colors;
    for (int i = 0; i < ImGuiCol_COUNT; i++) c[i] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_WindowBg] = c[ImGuiCol_ChildBg] = c[ImGuiCol_PopupBg] = col(t.ui.panel);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = col(t.ui.panel);
    c[ImGuiCol_Border] = c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = col(t.ui.border);
    c[ImGuiCol_Text] = col(t.ui.text);          // egui button text
    c[ImGuiCol_TextDisabled] = col(t.ui.weak);  // egui weak text
    c[ImGuiCol_Button] = col(t.ui.widget);
    c[ImGuiCol_ButtonHovered] = col(t.ui.widget_hover);
    c[ImGuiCol_ButtonActive] = col(t.ui.widget_active);
    c[ImGuiCol_FrameBg] = c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_FrameBgActive] = col(t.ui.field);
    c[ImGuiCol_SliderGrab] = col(t.ui.widget_hover);
    c[ImGuiCol_SliderGrabActive] = col(t.ui.accent);
    c[ImGuiCol_Header] = c[ImGuiCol_HeaderHovered] = c[ImGuiCol_HeaderActive] = col(t.ui.selection);
    c[ImGuiCol_ScrollbarGrab] = col(t.ui.border);
    c[ImGuiCol_ScrollbarGrabHovered] = col(t.ui.widget_hover);
    c[ImGuiCol_ScrollbarGrabActive] = col(t.ui.weak);
    c[ImGuiCol_CheckMark] = col(t.ui.text);
    c[ImGuiCol_TextSelectedBg] = col(t.ui.selection);
    c[ImGuiCol_NavCursor] = col(t.ui.accent);
    c[ImGuiCol_InputTextCursor] = col(t.ui.text);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.35f);
}

// ---------------------------------------------------------------------------
// Toolbar widgets (egui look-alikes)
// ---------------------------------------------------------------------------

// Developer switch: a file `font_delay_ms` in the home folder (or CALLIGRUS_FONT_DELAY_MS) holding a number makes the
// background choice of system fonts take that long more, to see how the app looks on a slow device (spec 30).
static int debug_font_delay_ms() {
    int ms = 0;
    if (const char* v = std::getenv("CALLIGRUS_FONT_DELAY_MS")) ms = std::atoi(v);
    else if (const char* home = std::getenv("HOME")) {
        std::ifstream in(fs::path(home) / "font_delay_ms");
        in >> ms;
    }
    return std::max(ms, 0);
}

// Tooltips are for a mouse (spec 35): on Android a finger arrives as mouse input too, with the touch-screen
// source, and a tap must not leave a tooltip behind. (GLFW's mouse is always the mouse.)
static bool tooltips_allowed() {
#ifdef __ANDROID__
    return ImGui::GetIO().MouseSource == ImGuiMouseSource_Mouse;
#else
    return true;
#endif
}

static void tooltip(const char* text) {
    if (tooltips_allowed() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

// egui "selectable label": no background unless hovered or selected.
static bool selectable_button(const char* label, bool selected, const Theme& t) {
    const ImGuiStyle& s = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? col(t.ui.selection) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? col(t.ui.selection) : s.Colors[ImGuiCol_ButtonHovered]);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, selected ? col(t.ui.selection) : s.Colors[ImGuiCol_ButtonActive]);
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? col(t.ui.selection_text) : s.Colors[ImGuiCol_Text]);
    bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    return clicked;
}

// egui's `ui.separator()` inside a horizontal layout: a thin vertical line.
static void vertical_separator() {
    // egui allocates a 6 px wide slot and draws the line 3 px into it (measured on the Rust port).
    const float scale = ImGui::GetStyle().ItemInnerSpacing.x / 4.0f;  // (4 px at scale 1, whatever the padding)
    ImGui::SameLine();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    const float x = std::floor(p.x + 3.0f * scale);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x, p.y), ImVec2(x + 1.0f, p.y + h), ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(6.0f * scale, h));
    ImGui::SameLine();
}

struct UiState {
    bool show_outline = true;
    bool settings_open = false;
    ThemePicker theme_picker;  // `t` (spec 6): the toolbar's theme dropdown, driven from the keyboard
    const std::vector<std::string>* recent = nullptr;  // the state file's recent list (newest first)
    bool open_recent = false;    // Ctrl+Shift+O or the ▾ button: open the popup this frame
    bool clear_recent = false;   // "Clear recent" was clicked; the frame loop empties the list
    int recent_sel = 0;          // keyboard selection in the popup
    bool slides_request = false; // the Slides button was clicked (spec 26)
    bool pdf_current_theme = false;  // the PDF menu's "Use current theme" box (this session only)
    int pdf_paper = 0;           // the PDF menu's paper for Pages (this session only): 0 = A4, 1 = Letter
    int export_request = 0;      // "Slides..." (1) or "Pages..." (2) was picked in the PDF menu, or `e` typed in slides mode (spec 27, 29)
    bool can_export = false;     // ...possible: a file is shown and no export is running
    float tab_scroll = 0;        // the tab bar's horizontal scroll (px; spec 34)
    size_t tab_seen = SIZE_MAX;  // the active tab / the number of tabs the bar last showed: when they change, keep the active tab visible
    size_t tab_seen_count = 0;
};

static void draw_recent_popup(DocState& st, UiState& ui, const Theme& theme, float scale);

// The toolbar's buttons, in drawing order. When the window is too narrow they move, the ones that stay
// least long first, into the overflow menu behind the "more" button at the right end (spec 35: made for
// phones; a desktop window gets it below kDesktopOverflowBelow px, so the usual window sizes are untouched).
enum TbItem { kTbOpen, kTbRecent, kTbReload, kTbOutline, kTbSlides, kTbPdf, kTbSizes, kTbTheme, kTbSettings, kTbCount };
// How long an item stays in the bar as the width shrinks (0 = last to go): Open, Outline, A- / A+, theme,
// settings, theme, reload, recent files, then the desktop-only slides and PDF.
static const int kTbRank[kTbCount] = {0, 6, 5, 1, 7, 8, 2, 4, 3};
// The groups the separators divide: {Open .. PDF}, {A-, A+}, {theme, settings}.
static int tb_group(int item) { return item <= kTbPdf ? 0 : item == kTbSizes ? 1 : 2; }
static const float kDesktopOverflowBelow = 400.0f;

static float theme_combo_width(const Config& cfg, const ThemeSet& themes, float scale) {
    const Theme* cur = themes.find(cfg.theme);
    const std::string preview = cfg.theme == "auto" ? std::string("Auto (system)") : cur ? cur->display_name : cfg.theme;
    return std::max(100.0f * scale, ImGui::CalcTextSize(preview.c_str()).x + 26.0f * scale);  // (as theme_picker sizes it)
}

// `text` shortened in the middle ("JW-MWM_2…10-29.md") until `width_of` says it fits in `room`; empty when fewer than
// 3 characters would remain (the label is then not shown at all). The ends are kept: a file name's end tells the type.
static std::string elide_middle(const std::string& text, float room, const std::function<float(const std::string&)>& width_of) {
    std::vector<size_t> at;  // byte offset of every character, and the end
    for (size_t i = 0; i < text.size(); i++)
        if ((uint8_t(text[i]) & 0xC0) != 0x80) at.push_back(i);
    const size_t n = at.size();
    at.push_back(text.size());
    const char* ellipsis = "\xE2\x80\xA6";
    // `keep` = characters kept besides the ellipsis (the front gets the extra one); the largest that fits, by bisection.
    auto with = [&](size_t keep) {
        const size_t head = (keep + 1) / 2, tail = keep - head;
        return text.substr(0, at[head]) + ellipsis + text.substr(at[n - tail]);
    };
    if (n < 4 || width_of(with(2)) > room) return {};
    size_t lo = 2, hi = n - 1;  // `lo` fits
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (width_of(with(mid)) <= room) lo = mid;
        else hi = mid - 1;
    }
    return with(lo);
}

static float tb_button_w(const char* label) { return ImGui::CalcTextSize(label, nullptr, true).x + 2 * ImGui::GetStyle().FramePadding.x; }

static void draw_toolbar(DocState& st, UiState& ui, ConfigFile& conf, const ThemeSet& themes, const Theme& theme,
                         const Fonts& fonts, GLFWwindow* window, float scale, double now) {
    Config& cfg = conf.cfg;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float gap = style.ItemSpacing.x;
    const float sep_w = 2 * gap + 6.0f * (style.ItemInnerSpacing.x / 4.0f);  // vertical_separator's slot
    const float pad_right = std::round(8.0f * scale);
    const float right = ImGui::GetWindowWidth() - pad_right;  // (window coordinates) the toolbar's right end
    const float more_w = tb_button_w("    ##more");

    // Which buttons fit.
    const float widths[kTbCount] = {tb_button_w("\xF0\x9F\x93\x82 Open\xE2\x80\xA6"), tb_button_w("   ##recent"), tb_button_w("\xE2\x9F\xB3"),
                                    tb_button_w("\xE2\x98\xB0 Outline"), tb_button_w("Slides"), tb_button_w("PDF"),
                                    tb_button_w("A\xE2\x88\x92") + gap + tb_button_w("A+"), theme_combo_width(cfg, themes, scale),
                                    tb_button_w("\xE2\x9A\x99")};
    bool present[kTbCount];
    for (bool& p : present) p = true;
#ifdef __ANDROID__  // (slides and PDF export are desktop-only for now: spec 35)
    present[kTbSlides] = present[kTbPdf] = false;
#endif
    auto needed = [&](const bool* on) {
        float w = 0;
        int prev_group = -1, count_in_group = 0;
        for (int i = 0; i < kTbCount; i++) {
            if (!on[i]) continue;
            const int g = tb_group(i);
            if (prev_group >= 0 && g != prev_group) w += sep_w, count_in_group = 0;
            if (count_in_group > 0) w += gap;
            w += widths[i];
            prev_group = g, count_in_group++;
        }
        return w;
    };
    bool shown[kTbCount];
    for (int i = 0; i < kTbCount; i++) shown[i] = present[i];
    const float avail = right - ImGui::GetCursorPosX();
#ifdef __ANDROID__
    const bool may_overflow = true;
#else
    const bool may_overflow = avail < kDesktopOverflowBelow;
#endif
    bool overflow = false;
    if (may_overflow && needed(shown) > avail) {
        overflow = true;
        for (;;) {  // drop the item that stays least long, until the rest and the "more" button fit
            int worst = -1;
            for (int i = 0; i < kTbCount; i++)
                if (shown[i] && (worst < 0 || kTbRank[i] > kTbRank[worst])) worst = i;
            if (worst < 0 || needed(shown) <= avail - more_w - gap) break;
            shown[worst] = false;
        }
    }
    const float more_x = right - more_w;  // the "more" button (window coordinates)
    const float limit = overflow ? more_x - gap : right;

    bool first = true, after_sep = false;
    int prev_group = -1;
    auto start = [&](int item) {  // the separator between groups, and the gap to the previous item
        const int g = tb_group(item);
        if (!first && g != prev_group) vertical_separator(), after_sep = true;
        if (!first && !after_sep) ImGui::SameLine();
        first = false, after_sep = false, prev_group = g;
    };
    auto do_open = [&] {
        if (g_script_mode) script_log("open dialog suppressed in script mode");
        else if (auto p = platform::open_file_dialog(native_window(window))) st.open_request = *p;
    };
    auto smaller = [&] {
        if (cfg.body_size > kBodySizeMin) {
            cfg.body_size--;
            conf.queue({{"body_size", toml_int(cfg.body_size)}}, now);
        }
    };
    auto larger = [&] {
        if (cfg.body_size < kBodySizeMax) {
            cfg.body_size++;
            conf.queue({{"body_size", toml_int(cfg.body_size)}}, now);
        }
    };
    // The menu under the PDF button: "Slides..." or "Pages..." (each then the save dialog). Drawn right after
    // the PDF button, or after the "more" button when PDF went into its menu.
    auto pdf_popup = [&] {
#ifndef __ANDROID__
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y), ImGuiCond_Appearing);
        if (ImGui::BeginPopup("##pdfmenu")) {
            if (ImGui::Selectable("Slides\xE2\x80\xA6")) ui.export_request = 1;
            if (ImGui::Selectable("Pages\xE2\x80\xA6")) ui.export_request = 2;
            ImGui::Checkbox("Use current theme", &ui.pdf_current_theme);
            tooltip("Pages: the theme shown now instead of light_theme");
            // Paper for Pages: two radio items side by side.
            ImGui::RadioButton("A4", &ui.pdf_paper, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Letter", &ui.pdf_paper, 1);
            ImGui::EndPopup();
        }
#endif
    };

    if (shown[kTbOpen]) {
        start(kTbOpen);
        if (ImGui::Button("\xF0\x9F\x93\x82 Open\xE2\x80\xA6")) do_open();  // "📂 Open…"
        tooltip("Ctrl+O");
    }
    if (shown[kTbRecent]) {
        start(kTbRecent);
        // The UI fonts have no ▾ glyph: paint the arrow on a blank button (the
        // same small triangle as the Rust port).
        if (ImGui::Button("   ##recent")) ui.open_recent = true;
        {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            const float r = 3.5f * scale;
            ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.5f), ImVec2(c.x + r, c.y - r * 0.5f),
                                                          ImVec2(c.x, c.y + r * 0.6f), ImGui::GetColorU32(ImGuiCol_Text));
        }
        tooltip("Recent files (Ctrl+Shift+O)");
        draw_recent_popup(st, ui, theme, scale);
    }
    if (shown[kTbReload]) {
        start(kTbReload);
        if (ImGui::Button("\xE2\x9F\xB3")) st.reload();  // "⟳"
        tooltip("Reload (R / Ctrl+R)");
    }
    if (shown[kTbOutline]) {
        start(kTbOutline);
        if (selectable_button("\xE2\x98\xB0 Outline", ui.show_outline, theme)) ui.show_outline = !ui.show_outline;  // "☰"
        tooltip("Ctrl+B");
    }
#ifndef __ANDROID__
    if (shown[kTbSlides]) {
        start(kTbSlides);
        if (ImGui::Button("Slides")) ui.slides_request = true;
        tooltip("Presentation (F5)");
    }
    if (shown[kTbPdf]) {
        start(kTbPdf);
        ImGui::BeginDisabled(!ui.can_export);
        if (ImGui::Button("PDF")) ImGui::OpenPopup("##pdfmenu");
        ImGui::EndDisabled();
        tooltip("Export the slides or the pages as a PDF (e in slides mode: slides)");
        pdf_popup();
    }
#endif
    if (shown[kTbSizes]) {
        start(kTbSizes);
        if (ImGui::Button("A\xE2\x88\x92")) smaller();  // "A−"
        tooltip("Smaller text");
        ImGui::SameLine();
        if (ImGui::Button("A+")) larger();
        tooltip("Larger text");
    }
    if (shown[kTbTheme]) {
        start(kTbTheme);
        conf.queue(theme_combo("##theme", cfg, themes, 0, scale, &ui.theme_picker), now);
        tooltip("Theme");
    }
    if (shown[kTbSettings]) {
        start(kTbSettings);
        if (ImGui::Button("\xE2\x9A\x99")) ui.settings_open = !ui.settings_open;  // a plain button in the Rust port too (no "on" look)  // "⚙"
        tooltip("Settings");
    }

    // The file name, with whatever room is left (all of it on a window wide enough). With the "more" button (a phone) the
    // bar has no name: it is the first line of the menu instead (spec 35).
    std::string label = st.path.empty() ? "built-in demo â open a file with Ctrl+O or drop one here" : display_path(st.path);
#ifdef __ANDROID__  // (the app's own copy of the document sits in a long private path: show the name)
    label = st.path.empty() ? "built-in demo" : to_utf8(st.path.filename());
#endif
    if (st.live) label = (st.live_path.empty() ? st.live_id : display_path(st.live_path)) + (st.live_open ? " (live)" : "");
    // The path can be long, so draw it with the exact advances of the document font (like egui),
    // not ImGui's whole-pixel ones that make it ~3 % narrower.
    const float em = 13.0f * scale;
    const float size = fonts.px(fonts.regular, em), adv = fonts.advance_scale(fonts.regular, em);
    auto label_width = [&](const std::string& s) { return text_width(fonts.regular, size, adv, s); };
    if (!overflow) {
        const float w = label_width(label);
        // The name gets the room left before the right edge and is shortened in the middle with an ellipsis when it does not
        // fit; with room for fewer than 3 characters it is left out (spec 35).
        const float room = limit - ((ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x) + sep_w);  // (the cursor is back at the line start after the last item)
#ifdef __ANDROID__
        const bool fit_label = true;
#else
        const bool fit_label = false;  // (a desktop window keeps drawing the whole name: the window edge cuts it, as the references show)
#endif
        const std::string shown_label = !fit_label || w <= room ? label : elide_middle(label, room, label_width);
        if (!shown_label.empty()) {
            vertical_separator();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            draw_text(ImGui::GetWindowDrawList(), fonts.regular, size, adv, ImVec2(at.x, at.y + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), shown_label);
            ImGui::Dummy(ImVec2(std::min(w, room), ImGui::GetFrameHeight()));
        }
    }

    if (!overflow) return;

    // The "more" button and its menu.
    // The theme list opens from the menu (or `t`) while its button is hidden: a combo one pixel wide at the "more"
    // button holds it (no arrow is drawn at that width; the button is drawn over it, and the list is its popup).
    if (!shown[kTbTheme] && (ui.theme_picker.open_request || ui.theme_picker.active)) {
        ImGui::SameLine(more_x);
        conf.queue(theme_combo("##theme", cfg, themes, 1.0f, scale, &ui.theme_picker), now);
    }
    ImGui::SameLine(more_x);
    if (ImGui::Button("    ##more")) ImGui::OpenPopup("##overflow");
    const ImVec2 mb_min = ImGui::GetItemRectMin(), mb_max = ImGui::GetItemRectMax();
    {
        const ImVec2 c((mb_min.x + mb_max.x) * 0.5f, (mb_min.y + mb_max.y) * 0.5f);
        const float r = 1.6f * scale, step = 5.0f * scale;
        for (int k = -1; k <= 1; k++) ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(c.x, c.y + step * float(k)), r, ImGui::GetColorU32(ImGuiCol_Text));
    }
    tooltip("More");
    if (!shown[kTbRecent]) draw_recent_popup(st, ui, theme, scale);  // (opens under this button when picked from the menu)
    bool open_pdf = false;
    ImGui::SetNextWindowPos(ImVec2(mb_max.x, mb_max.y), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
    if (ImGui::BeginPopup("##overflow")) {
        const ImVec2 row(0, ImGui::GetFrameHeight());
        {  // the document's name, greyed (not an item), shortened in the middle when long
            const float room = 260.0f * scale;
            const std::string name = label_width(label) <= room ? label : elide_middle(label, room, label_width);
            ImGui::BeginDisabled();
            ImGui::Selectable((name + "##name").c_str(), false, ImGuiSelectableFlags_NoAutoClosePopups, row);
            ImGui::EndDisabled();
            ImGui::Separator();
        }
        auto item = [&](int id, const char* label, bool selected = false) {
            return !shown[id] && present[id] && ImGui::Selectable(label, selected, 0, row);
        };
        if (item(kTbOpen, "Open\xE2\x80\xA6")) do_open();
        if (item(kTbOutline, "Outline", ui.show_outline)) ui.show_outline = !ui.show_outline;
        if (!shown[kTbSizes]) {  // (these stay open, so a few taps make a bigger change)
            if (ImGui::Selectable("Smaller text", false, ImGuiSelectableFlags_NoAutoClosePopups, row)) smaller();
            if (ImGui::Selectable("Larger text", false, ImGuiSelectableFlags_NoAutoClosePopups, row)) larger();
        }
        if (item(kTbTheme, "Theme\xE2\x80\xA6")) ui.theme_picker.open_request = true;
        if (item(kTbReload, "Reload")) st.reload();
        if (item(kTbRecent, "Recent files\xE2\x80\xA6")) ui.open_recent = true;
        if (item(kTbSettings, "Settings", ui.settings_open)) ui.settings_open = !ui.settings_open;
        if (item(kTbSlides, "Slides")) ui.slides_request = true;
        if (!shown[kTbPdf] && present[kTbPdf]) {
            ImGui::BeginDisabled(!ui.can_export);
            if (ImGui::Selectable("PDF\xE2\x80\xA6", false, 0, row)) open_pdf = true;
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }
    if (open_pdf) ImGui::OpenPopup("##pdfmenu");
    if (!shown[kTbPdf] && present[kTbPdf]) pdf_popup();
}

// The find bar (spec 7), a row under the toolbar.
static void draw_find_bar(FindState& find, const Theme& theme, float pad_x, float scale) {
    ImGui::SetCursorPosX(pad_x);
    if (find.focus) {
        ImGui::SetKeyboardFocusHere();
        find.focus = false;
    }
    ImGui::SetNextItemWidth(240 * scale);
    // egui's TextEdit: 2 px of vertical margin (21 px tall in all) and a 1 px outline, in the accent colour while focused.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4 * scale, 2 * scale));
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | (find.select_all ? ImGuiInputTextFlags_AutoSelectAll : 0);
    const bool entered = ImGui::InputTextWithHint("##find", "Find in page", find.buf, sizeof(find.buf), flags);
    find.select_all = false;
    ImGui::PopStyleVar();
    if (find.typed != find.buf) find.typed = find.buf, find.typed_at = ImGui::GetTime();  // an edit: the search waits for the typing to stop
    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                        ImGui::IsItemActive() ? theme.ui.selection_text : theme.ui.border, 2.0f * scale);
    if (entered && find.vim) {
        // Opened with `/`: Enter runs the search (now, whatever the typing wait) and the keys go back to the document.
        find.enter_search = true;
        ImGui::ClearActiveID();
        ImGui::GetCurrentContext()->InputTextReactivateId = 0;  // (ConfigInputTextEnterKeepActive would take the field back next frame)
    } else if (entered) {
        // ConfigInputTextEnterKeepActive selects the whole text after Enter; egui leaves the cursor at the end.
        if (ImGuiInputTextState* ts = ImGui::GetInputTextState(ImGui::GetItemID())) ts->ClearSelection();
        // Enter = next match, Shift+Enter = previous (the field keeps focus). Typing still waiting to be searched: search it
        // first (it then starts at the first match from the view).
        const int n = int(find.matches.size());
        if (find.due > 0) find.enter_search = true;
        else if (n > 0) {
            find.current = (find.current + (ImGui::GetIO().KeyShift ? n - 1 : 1)) % n;
            find.scroll_to_current = true;
        }
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (find.buf[0] == 0) ImGui::TextDisabled(" ");
    else if (find.due > 0) ImGui::TextDisabled("\xE2\x80\xA6");  // "…" (the search waits for the typing to stop)
    else if (find.matches.empty()) ImGui::TextDisabled("no matches");
    else ImGui::TextDisabled("%d/%d", find.current + 1, int(find.matches.size()));
    ImGui::SameLine();
    const int n = int(find.matches.size());
    if (ImGui::Button("\xE2\x86\x91") && n > 0) {  // "↑"
        find.current = (find.current + n - 1) % n;
        find.scroll_to_current = true;
    }
    tooltip("Previous match (Shift+Enter, N)");
    ImGui::SameLine();
    if (ImGui::Button("\xE2\x86\x93") && n > 0) {  // "↓"
        find.current = (find.current + 1) % n;
        find.scroll_to_current = true;
    }
    tooltip("Next match (Enter, n)");
    ImGui::SameLine();
    if (ImGui::Button("\xC3\x97")) find.close();  // "×"
    tooltip("Close (Esc)");
}

// ---------------------------------------------------------------------------
// Tab bar (spec 34)
// ---------------------------------------------------------------------------

// A tab's name for the bar: the file name, at most 24 characters (longer ones: the first 23 and "…").
static std::string tab_name(const std::string& file_name) {
    size_t chars = 0, cut = file_name.size();
    for (size_t i = 0; i < file_name.size(); i++) {
        if ((static_cast<unsigned char>(file_name[i]) & 0xC0) == 0x80) continue;  // (inside a UTF-8 character)
        if (chars == 23) cut = i;
        chars++;
    }
    return chars <= 24 ? file_name : file_name.substr(0, cut) + "\xE2\x80\xA6";
}

struct TabLabel {
    std::string name;  // as shown
    std::string tip;   // the full path
};

// What the bar asked for this frame: switch to a tab, close a tab (-1: nothing).
struct TabBarResult {
    int switch_to = -1;
    int close = -1;
};

// The row under the toolbar, drawn at `top` (screen y) and `row_h` tall: one egui-like selectable
// label per tab (no background unless hovered or active) with an "×" on the active one and on the one
// under the pointer. A row wider than the window scrolls with the wheel, and keeps the active tab in view.
static TabBarResult draw_tab_bar(const std::vector<TabLabel>& tabs, size_t active, UiState& ui, const Theme& theme, const Fonts& fonts, float top,
                                 float row_h, float pad_x, float pad_y, float scale) {
    TabBarResult res;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight(), gap = 2.0f * scale, between = 4.0f * scale;
    const char* cross = "\xC3\x97";  // "×"
    // The labels are measured and drawn with the document font's exact advances, like egui (the UI font's
    // are whole pixels: a name came out 1 to 2 px wider or narrower, so every tab after it sat off).
    const float em = 13.0f * scale, size = fonts.px(fonts.regular, em), adv = fonts.advance_scale(fonts.regular, em);
    // (a label with its kerning, as egui's text is: "tabs-b.md" is 0.4 px narrower than its plain advances)
    struct Label {
        std::vector<prose_shaping::Piece> pieces;
        float width = 0;
    };
    const auto label_of = [&](std::string_view s) {
        Label l;
        if (!prose_shaping::kerned_pieces(fonts.regular, s, size * adv, l.pieces, l.width)) {
            l.pieces = {{std::string(s), 0.0f}};
            l.width = text_width(fonts.regular, size, adv, s);
        }
        return l;
    };
    const auto draw_label = [&](const Label& l, ImVec2 at, ImU32 col) {
        for (const prose_shaping::Piece& p : l.pieces) draw_text(dl, fonts.regular, size, adv, ImVec2(at.x + p.x, at.y), col, p.text);
    };
    const Label cross_label = label_of(cross);
    const float cross_w = cross_label.width;
    std::vector<Label> labels;
    for (const TabLabel& t : tabs) labels.push_back(label_of(t.name));
    // Where each tab sits along the row (before scrolling), and the total.
    std::vector<float> x0, w;
    float total = 0;
    for (const Label& label : labels) {
        x0.push_back(total);
        // (a whole number of points, as egui sizes the tab: ceil of padding + name + gap + x + padding)
        w.push_back(std::ceil((style.FramePadding.x + label.width + between + cross_w + style.FramePadding.x) / scale) * scale);
        total += w.back() + gap;
    }
    total -= gap;
    const float view_x = vp->WorkPos.x + pad_x, view_w = vp->WorkSize.x - 2 * pad_x;
    const float max_scroll = std::max(0.0f, total - view_w);
    const ImVec2 row_min(vp->WorkPos.x, top), row_max(vp->WorkPos.x + vp->WorkSize.x, top + row_h);
    if (ImGui::IsMouseHoveringRect(row_min, row_max) && ImGui::GetIO().MouseWheel != 0 && !ImGui::GetIO().KeyCtrl)
        ui.tab_scroll -= ImGui::GetIO().MouseWheel * 48.0f * scale;
    if (ui.tab_seen != active || ui.tab_seen_count != tabs.size()) {  // another tab is active: show it whole
        ui.tab_seen = active, ui.tab_seen_count = tabs.size();
        if (x0[active] < ui.tab_scroll) ui.tab_scroll = x0[active];
        if (x0[active] + w[active] > ui.tab_scroll + view_w) ui.tab_scroll = x0[active] + w[active] - view_w;
    }
    ui.tab_scroll = std::clamp(ui.tab_scroll, 0.0f, max_scroll);

    ImGui::PushClipRect(ImVec2(view_x, top), ImVec2(view_x + view_w, top + row_h), true);
    const float y = top + pad_y;
    for (size_t i = 0; i < tabs.size(); i++) {
        const float x = std::round(view_x + x0[i] - ui.tab_scroll);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(int(i));
        ImGui::InvisibleButton("##tab", ImVec2(w[i], h));
        const bool hovered = ImGui::IsItemHovered();
        const bool is_active = i == active;
        const ImVec2 a(x, y), b(x + w[i], y + h);
        if (is_active || hovered) dl->AddRectFilled(a, b, is_active ? theme.ui.selection : theme.ui.widget_hover, style.FrameRounding);
        const ImU32 text_col = is_active ? theme.ui.selection_text : theme.ui.text;
        draw_label(labels[i], ImVec2(x + style.FramePadding.x, y + style.FramePadding.y), text_col);
        // The ×, on the active tab and the one under the pointer; on its own square while the pointer is on it.
        const ImVec2 ca(b.x - style.FramePadding.x - cross_w - 2.0f * scale, y), cb(b.x - 1.0f * scale, y + h);
        bool on_cross = false;
        if (is_active || hovered) {
            on_cross = hovered && ImGui::IsMouseHoveringRect(ca, cb);
            if (on_cross) dl->AddRectFilled(ca, cb, theme.ui.widget_hover, style.FrameRounding);
            draw_label(cross_label, ImVec2(ca.x + 2.0f * scale, y + style.FramePadding.y),
                       on_cross ? theme.ui.text : (is_active ? theme.ui.selection_text : theme.weak));
        }
        if (hovered && tooltips_allowed() && !tabs[i].tip.empty()) ImGui::SetTooltip("%s", tabs[i].tip.c_str());
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) (on_cross ? res.close : res.switch_to) = int(i);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) res.close = int(i);
        ImGui::PopID();
    }
    ImGui::PopClipRect();
    return res;
}

// ---------------------------------------------------------------------------
// Outline panel
// ---------------------------------------------------------------------------

// Shorten `text` with "…" so it fits in `width` pixels (egui's Label::truncate).
static std::string ellipsize(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    const char* dots = "\xE2\x80\xA6";
    std::string s = text;
    while (!s.empty()) {
        s.pop_back();
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();  // whole UTF-8 chars
        if (ImGui::CalcTextSize((s + dots).c_str()).x <= width) break;
    }
    return s + dots;
}

// Does `b` (or anything nested in it) hold a span linking to `link`?
static bool block_has_link(const md::Block& b, const std::string& link) {
    auto in = [&](const md::Spans& spans) {
        return std::any_of(spans.begin(), spans.end(), [&](const md::Span& s) { return s.link == link; });
    };
    if (in(b.spans)) return true;
    for (const md::Spans& h : b.header)
        if (in(h)) return true;
    for (const auto& row : b.rows)
        for (const md::Spans& cell : row)
            if (in(cell)) return true;
    for (const md::Block& c : b.children)
        if (block_has_link(c, link)) return true;
    for (const auto& item : b.items)
        for (const md::Block& c : item)
            if (block_has_link(c, link)) return true;
    return false;
}

// Footnote anchors: "fn-3" is the block holding footnote 3's text, "fnref-3"
// the (top-level) block of its first reference. -1 = no such anchor.
static int footnote_block(const md::Document& doc, const std::string& slug) {
    const bool ref = slug.rfind("fnref-", 0) == 0;
    if (!ref && slug.rfind("fn-", 0) != 0) return -1;
    const int n = std::atoi(slug.c_str() + (ref ? 6 : 3));
    if (n <= 0) return -1;
    for (size_t i = 0; i < doc.size(); i++)
        if (ref ? block_has_link(doc[i], "#fn-" + std::to_string(n)) : doc[i].footnote == n) return int(i);
    return -1;
}

// Middle-elide `text` (a folder path) to fit in `width` pixels.
static std::string ellipsize_middle(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    for (size_t keep = text.size(); keep > 2; keep--) {
        const size_t head = (keep + 1) / 2, tail = keep / 2;
        if ((text[head] & 0xC0) == 0x80 || (text[text.size() - tail] & 0xC0) == 0x80) continue;  // inside a UTF-8 char
        std::string s = text.substr(0, head) + "\xE2\x80\xA6" + text.substr(text.size() - tail);
        if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
    }
    return "\xE2\x80\xA6";
}

// The recent-files popup under the toolbar's ▾ button (see ui.open_recent):
// file name + weak folder per row, newest first; Up/Down/j/k + Enter work too.
static void draw_recent_popup(DocState& st, UiState& ui, const Theme& theme, float scale) {
    const ImVec2 under(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y);  // below the ▾ button
    if (ui.open_recent) {
        ui.open_recent = false;
        ui.recent_sel = 0;
        ImGui::OpenPopup("##recent");
    }
    ImGui::SetNextWindowPos(under, ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("##recent")) return;
    const std::vector<std::string>& recent = *ui.recent;
    const int n = int(recent.size());
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_J)) ui.recent_sel = std::min(ui.recent_sel + 1, n - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_K)) ui.recent_sel = std::max(ui.recent_sel - 1, 0);
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    if (n == 0) {
        ImGui::BeginDisabled();
        ImGui::Selectable("No recent files");
        ImGui::EndDisabled();
    }
    for (int i = 0; i < n; i++) {
        const fs::path p(std::u8string(recent[size_t(i)].begin(), recent[size_t(i)].end()));
        std::error_code ec;
        const bool exists = fs::is_regular_file(p, ec);
        const std::u8string name = p.filename().u8string();
        const std::string dir = display_path(p.parent_path());
        std::string label(name.begin(), name.end());
        if (!exists) label += " (missing)";
        ImGui::PushID(i);
        ImGui::BeginDisabled(!exists);
        const bool clicked = ImGui::Selectable(label.c_str(), i == ui.recent_sel, 0, ImVec2(0, 0));
        if (exists && ImGui::IsItemHovered() && ImGui::GetIO().MouseDelta.x + ImGui::GetIO().MouseDelta.y != 0) ui.recent_sel = i;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.weak);
        ImGui::TextUnformatted(ellipsize_middle(dir, 280 * scale).c_str());
        ImGui::PopStyleColor();
        ImGui::PopID();
        if (exists && (clicked || (enter && i == ui.recent_sel))) {
            st.open_request = p;
            st.open_new_tab = ImGui::GetIO().KeyCtrl;  // Ctrl+Enter / Ctrl+click: a new tab (spec 34)
            ImGui::CloseCurrentPopup();
            break;
        }
    }
    ImGui::Separator();
    if (ImGui::Selectable("Clear recent")) {
        ui.clear_recent = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Where clicking a heading in the outline scrolls to.
static float top_pad(float scale);
// the Rust port: scroll_to_rect(rect.expand(8), TOP) inside the central panel's 8 pt margin, so the
// heading's text box starts 18 px below the top of the C++ document child (measured: the
// headings/next-heading shot; the last 2 px are egui's own rounding). `e.y` counts from the
// document's top, which sits top_pad above the first content.
static float heading_scroll(const OutlineEntry& e, float scale) { return std::max(0.0f, top_pad(scale) + e.y - 18.0f * scale); }

static void draw_outline(DocState& st, const std::vector<OutlineEntry>& outline, const Theme& t, float scale) {
    // Sub-headings: egui's non-interactive label colour (not in the theme): ui.text
    // mixed towards the panel, which gives egui's grey 140 / 80 for the built-ins.
    const float mix = t.dark ? 113.0f / 153.0f : 168.0f / 188.0f;
    const ImVec4 p = col(t.ui.panel), q = col(t.ui.text);
    const ImU32 normal = ImGui::ColorConvertFloat4ToU32(ImVec4(p.x + (q.x - p.x) * mix, p.y + (q.y - p.y) * mix, p.z + (q.z - p.z) * mix, 1));
    const ImU32 strong = t.heading;     // the title and top-level headings
    ImGui::Dummy(ImVec2(0, 1 * scale));  // egui: add_space(6); ImGui adds the 4 px row spacing after a Dummy itself (+1: font ascent rounding)
    ImGui::PushStyleColor(ImGuiCol_Text, strong);
    ImGui::TextUnformatted("Outline");
    ImGui::PopStyleColor();
    // (Spacing around the rule matches egui's measured pixel rows: 10 px under the title, 11 px above the first entry.)
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2 * scale);
    ImGui::Separator();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2 * scale);

    int min_level = 6;
    for (const auto& e : outline) min_level = std::min(min_level, e.level);
    for (size_t i = 0; i < outline.size(); i++) {
        const auto& e = outline[i];
        float indent = 12.0f * scale * (e.level - min_level);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
        std::string text = ellipsize(e.title, ImGui::GetContentRegionAvail().x);
        ImGui::PushStyleColor(ImGuiCol_Text, e.level == min_level ? strong : normal);
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextUnformatted(text.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) st.scroll_to = heading_scroll(e, scale);
        ImGui::PopID();
        ImGui::PopStyleColor();
    }
}

// ---------------------------------------------------------------------------
// Document view
// ---------------------------------------------------------------------------

// The reading column (like render.rs): up to `max_width` pt wide (config, default 860), 24 pt side
// margins, inside egui's CentralPanel margin of 8 pt on every side (Frame::central_panel).
static float g_max_width = 860.0f;
static float panel_margin(float scale) { return 8.0f * scale; }
// Scrollable height = laid-out height + this. egui: 64 pt of space after the last block inside
// the central panel's 8 pt margin, less what top_pad() already counts (measured on mermaid/bottom).
static float bottom_pad(float scale) { return std::round(32.0f * scale); }
static float top_pad(float scale) { return std::round(8.0f * scale + 16.0f * scale); }  // document y=0 -> content y
static float column_width(float view_w, float scale) {
    float avail = view_w - 2 * panel_margin(scale);
    return std::clamp(avail - 48.0f * scale, 200.0f * scale, std::max(g_max_width, 200.0f) * scale);
}

// Relative image paths resolve against the document's folder (the exe's
// folder for the built-in demo).
static fs::path g_export_base_dir;  // `--base-dir` of an export child (spec 29): wins over the document's folder
static fs::path base_dir(const DocState& st) {
    if (!g_export_base_dir.empty()) return g_export_base_dir;
    if (st.live) return st.live_path.empty() ? fs::current_path() : st.live_path.parent_path();
    return st.path.empty() ? platform::exe_dir() : st.path.parent_path();
}

// Slides mode's column layouts for render_document (null: one column, normal mode).
static const std::vector<slides::Placement>* columns_of(const DocState& st) {
    return st.columns.empty() ? nullptr : &st.columns;
}

// ...and its text styles (null: all plain).
static const std::vector<slides::TextStyle>* styles_of(const DocState& st) { return st.styles.empty() ? nullptr : &st.styles; }

// ...and which blocks are background pictures (null: none).
static const std::vector<char>* backgrounds_of(const DocState& st) { return st.backgrounds.empty() ? nullptr : &st.backgrounds; }

// The layout a selection / find match belongs to (which spaces a wrap swallowed).
// The same as render_document's height cache key.
static size_t layout_key(const DocState& st, float col_w, const Fonts& fonts) {
    return std::hash<size_t>{}(st.version) ^ (std::hash<float>{}(col_w) << 1) ^ (std::hash<float>{}(fonts.body) << 2) ^
           (st.columns.empty() ? 0 : kColumnsKey) ^ (st.styles.empty() ? 0 : kStylesKey) ^ (st.backgrounds.empty() ? 0 : kBackgroundsKey);
}

// Lay out the whole document without showing it (into a scratch draw list).
// Fills the height cache (cache.top / cache.height) and returns the runs of
// every block, with y measured from the top of the document.
static std::vector<Run> layout_all(DocState& st, const Fonts& fonts, const Theme& theme, float col_w, float* height = nullptr) {
    static ImDrawList* scratch = nullptr;
    if (!scratch) scratch = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
    scratch->_ResetForNewFrame();
    scratch->PushTexture(ImGui::GetIO().Fonts->TexRef);
    scratch->PushClipRectFullScreen();
    Collector col;
    RenderOptions opts;
    opts.columns = columns_of(st);
    opts.styles = styles_of(st);
    opts.backgrounds = backgrounds_of(st);
    RenderResult r = render_document(scratch, st.doc, fonts, theme, ImVec2(0, 0), col_w, st.cache, st.version, -1e9f, 1e9f,
                                     base_dir(st), col, std::nullopt, 0, opts);
    if (height) *height = r.height;
    return std::move(col.runs);
}

// How long after the last keystroke in the find field the search runs (spec 7).
constexpr double kFindDebounce = 0.15;

// A developer switch for measuring: a file `find_legacy` in the home folder (or CALLIGRUS_FIND_LEGACY) makes every
// search lay the document out and prepare its text again, as the first version did.
static bool find_legacy() {
    static int legacy = -1;
    if (legacy < 0) {
#pragma warning(suppress : 4996)
        const char* home = std::getenv("HOME");
        std::error_code ec;
#pragma warning(suppress : 4996)
        legacy = std::getenv("CALLIGRUS_FIND_LEGACY") || (home && fs::exists(fs::path(home) / "find_legacy", ec)) ? 1 : 0;
    }
    return legacy == 1;
}

// The document's searchable text for the layout on screen: built the first time it is asked for (after a layout
// change), then kept.
static const FindIndex& find_index(DocState& st, FindState& find, const Fonts& fonts, const Theme& theme, float col_w) {
    const size_t key = layout_key(st, col_w, fonts);
    if (!find.index_ok || find.index_key != key || find_legacy()) {
        trace::Stopwatch sw;
        const std::vector<Run> runs = layout_all(st, fonts, theme, col_w);
        const double layout_ms = sw.ms();
        find.index = build_find_index(runs);
        find.index_key = key;
        find.index_ok = true;
        trace::log("find: text prepared: layout %.1f ms + index %.1f ms (%zu runs, %zu groups)", layout_ms, sw.ms() - layout_ms, runs.size(),
                   find.index.groups.size());
    }
    return find.index;
}

// Recompute find matches when the query or the layout changed.
static void update_find(DocState& st, FindState& find, const Fonts& fonts, const Theme& theme, float col_w) {
    const std::string query = find.open ? find.buf : "";
    const size_t key = layout_key(st, col_w, fonts);
    const bool forced = find.enter_search;  // (Enter: run now, and for vim `/` start from where it was pressed)
    find.enter_search = false;
    const bool again = forced && find.vim && find.repick;  // the same query, searched again from the origin
    if (query == find.query && key == find.layout_key && !again) {
        find.due = 0;
        if (forced && find.current >= 0) find.scroll_to_current = true;
        return;
    }
    // Typing: wait until the keys stop (a cleared query and a layout change run at once; script mode never waits).
    if (query != find.query && !query.empty() && !forced && !g_script_mode) {
        const double due = find.typed_at + kFindDebounce;
        if (ImGui::GetTime() < due) {
            find.due = due;
            return;
        }
    }
    find.due = 0;
    find.repick = false;
    const bool new_query = query != find.query || again;
    find.query = query;
    find.layout_key = key;
    find.matches.clear();
    find.match_y.clear();
    if (query.empty()) {
        find.current = -1;
        if (find.ed_text.empty()) find.index = FindIndex(), find.index_ok = false;  // (nothing needs the text now)
        return;
    }
    // Search the whole document's text, not just what is on screen.
    const FindIndex& index = find_index(st, find, fonts, theme, col_w);
    trace::Stopwatch sw;
    find.matches = find_matches(index, query);
    for (const TextRange& m : find.matches) {
        auto it = std::lower_bound(index.rows.begin(), index.rows.end(), TextPos{m.first.block, m.first.tok, 0},
                                   [](const FindIndex::Row& r, const TextPos& p) { return r.pos < p; });
        find.match_y.emplace_back(it != index.rows.end() ? it->min_y : 0.0f, it != index.rows.end() ? it->max_y : 0.0f);
    }
    trace::log("find: \"%s\": %zu matches in %.2f ms", query.c_str(), find.matches.size(), sw.ms());
    const int n = int(find.matches.size());
    if (new_query && find.word_from) {
        const auto [block, dir] = *find.word_from;
        find.word_from.reset();
        find.current = n > 0 ? (dir > 0 ? 0 : n - 1) : -1;
        if (dir > 0) {
            for (int i = 0; i < n; i++)
                if (find.matches[size_t(i)].first.block >= block) {
                    find.current = i;
                    break;
                }
        } else {
            for (int i = n - 1; i >= 0; i--)
                if (find.matches[size_t(i)].first.block <= block) {
                    find.current = i;
                    break;
                }
        }
        find.scroll_to_current = n > 0;
    } else if (new_query && find.vim) {
        // `/`: the first match at or after the view position where the key was pressed (wrapping to the first one).
        const float from = find.origin - top_pad(fonts.scale);
        find.current = n > 0 ? 0 : -1;
        for (int i = 0; i < n; i++)
            if (find.match_y[size_t(i)].second > from + 1.0f) {
                find.current = i;
                break;
            }
        find.scroll_to_current = n > 0;
    } else if (new_query) {
        // Start at the first match in or below the block at the top of the view.
        const float view_doc_top = st.scroll_cur - top_pad(fonts.scale);
        const auto& tops = st.cache.top;
        size_t top_block = size_t(std::upper_bound(tops.begin(), tops.end(), view_doc_top) - tops.begin());
        top_block = top_block > 0 ? top_block - 1 : 0;
        find.current = n > 0 ? 0 : -1;
        for (int i = 0; i < n; i++)
            if (find.matches[size_t(i)].first.block >= top_block) {
                find.current = i;
                break;
            }
        find.scroll_to_current = n > 0;
    } else {
        find.current = n > 0 ? std::clamp(find.current, 0, n - 1) : -1;
    }
    if (forced && find.current >= 0) find.scroll_to_current = true;
}

// Recompute the editor-search matches when its query or the layout changed (same
// matching as find, but `case` instead of smartcase).
static void update_editor_search(DocState& st, FindState& find, const Fonts& fonts, const Theme& theme, float col_w) {
    const size_t key = layout_key(st, col_w, fonts);
    if (find.ed_text == find.ed_done && find.ed_case == find.ed_done_case && key == find.ed_key) return;
    find.ed_done = find.ed_text;
    find.ed_done_case = find.ed_case;
    find.ed_key = key;
    find.ed_matches.clear();
    if (find.ed_text.empty()) {
        if (find.query.empty()) find.index = FindIndex(), find.index_ok = false;  // (nothing needs the text now)
        return;
    }
    find.ed_matches = find_matches(find_index(st, find, fonts, theme, col_w), find.ed_text, find.ed_case ? 1 : 0);
}

// How far one mouse-wheel notch scrolls the document, in points (times
// fonts.scale = DPI x zoom for pixels), the same as the Rust port: Windows reports
// a notch as one "line" (winit LineDelta 1.0; GLFW/ImGui io.MouseWheel 1.0),
// and egui turns lines into points with InputOptions::line_scroll_speed = 40.0
// on native (egui 0.36.2, src/input_state/mod.rs, `impl Default for
// InputOptions`; applied in src/input_state/wheel_state.rs). egui's points
// are zoomed points, so the distance grows with zoom exactly like ours.
constexpr float kWheelNotchPoints = 40.0f;

// Animate the scroll position towards its target (egui-style smooth scrolling).
static void smooth_scroll(DocState& st, const Fonts& fonts) {
    ImGuiIO& io = ImGui::GetIO();
    float now = ImGui::GetScrollY();
    // Something else moved the view (scrollbar drag): follow it.
    if (st.scroll_last_set >= 0 && std::abs(now - st.scroll_last_set) > 0.5f) st.scroll_target = st.scroll_cur = now;
    if (st.scroll_to >= 0) {
        st.scroll_target = st.scroll_to;
        st.scroll_to = -1;
    }
    // The wheel scrolls, except with Ctrl (zoom) or Shift (code blocks scroll sideways).
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0 && !io.KeyCtrl && !io.KeyShift)
        st.scroll_target -= io.MouseWheel * kWheelNotchPoints * fonts.scale;
    const float max_scroll = ImGui::GetScrollMaxY();
    if (st.scroll_now != 0) {  // autoscroll: move right away, no easing
        st.scroll_target = std::clamp(st.scroll_target + st.scroll_now, 0.0f, max_scroll);
        st.scroll_cur = st.scroll_target;
        st.scroll_now = 0;
    }
    st.scroll_target = std::clamp(st.scroll_target, 0.0f, max_scroll);

    // Exponential ease. Clamp dt: after sleeping in glfwWaitEvents the first
    // frame's DeltaTime can be seconds, which would make the first notch jump.
    float dt = std::min(io.DeltaTime, 1.0f / 30.0f);
    st.scroll_cur += (st.scroll_target - st.scroll_cur) * (1.0f - std::exp(-dt * 18.0f));
    if (std::abs(st.scroll_target - st.scroll_cur) < 0.5f) st.scroll_cur = st.scroll_target;
    st.animating = st.scroll_cur != st.scroll_target;
    ImGui::SetScrollY(st.scroll_cur);
    st.scroll_last_set = st.scroll_cur;
}

// `s` cut to at most `n` characters (code points), with "…" when it was longer (and on one line).
static std::string shorten(const std::string& s, size_t n) {
    std::string flat;
    for (size_t i = 0; i < s.size();) {
        size_t j = i;
        while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) j++;
        if (j > i) flat += (flat.empty() ? "" : " ") + s.substr(i, j - i);
        while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) j++;
        i = j;
    }
    size_t chars = 0, cut = flat.size();
    for (size_t i = 0; i < flat.size(); i++)
        if ((static_cast<unsigned char>(flat[i]) & 0xC0) != 0x80 && chars++ == n - 1) cut = i;
    return chars <= n ? flat : flat.substr(0, cut) + "\xE2\x80\xA6";
}

// The toast (spec 27, render.rs `paint_toast`): 14 pt UI text on a panel-coloured pill, 24 pt above
// the bottom of the view (`view_min`..`view_max`, screen; plus the 8 pt margin egui's panel puts
// around the Rust view), centred, for 1.6 s.
static void draw_toast(DocState& st, const Fonts& fonts, const Theme& theme, ImVec2 view_min, ImVec2 view_max) {
    if (st.toast.empty()) return;
    const double left = 1.6 - (ImGui::GetTime() - st.toast_since);
    if (left <= 0) {
        st.toast.clear();
        return;
    }
    const float scale = fonts.scale;
    const float size = fonts.px(fonts.regular, 14.0f * scale), adv = fonts.advance_scale(fonts.regular, 14.0f * scale);
    float asc = 0, desc = 0;
    font_metrics(fonts, fonts.regular, 14.0f * scale, asc, desc);
    const float tw = text_width(fonts.regular, size, adv, st.toast);
    const ImVec2 box(std::round(tw + 20 * scale), std::round(asc + desc + 12 * scale));
    const ImVec2 a(std::round((view_min.x + view_max.x) / 2 - box.x / 2), std::round(view_max.y - (24 + 8) * scale - box.y));
    const ImVec2 b(a.x + box.x, a.y + box.y);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(a, b, theme.ui.panel, 6 * scale);
    dl->AddRect(ImVec2(a.x + 0.5f, a.y + 0.5f), ImVec2(b.x - 0.5f, b.y - 0.5f), theme.ui.border, 6 * scale);
    draw_text(dl, fonts.regular, size, adv, ImVec2(a.x + 10 * scale, a.y + 6 * scale), theme.ui.text, st.toast);
    // No `animate`: the idle loop wakes once when it is due to go (as for the scrollbar), and a
    // script's `shot` does not wait 1.6 s for it to disappear.
}

// The zoomed picture or diagram (spec 27, render.rs `paint_lightbox`), over the whole window: the
// window dimmed (black at 80%), the picture as big as fits (nav::lightbox_size) on a card of the
// page's colour, the alt text under it. A click anywhere closes it (Esc too, in the key handling).
// A window of its own, so the document under it gets no hover or clicks.
static void draw_lightbox(DocState& st, const Fonts& fonts, const Theme& theme, float scale) {
    if (!st.lightbox) return;
    const LinkHit& lb = *st.lightbox;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##lightbox", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                                            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(3);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = vp->Pos, sz = vp->Size;
    dl->AddRectFilled(o, ImVec2(o.x + sz.x, o.y + sz.y), IM_COL32(0, 0, 0, 204));
    const bool close = ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    // The picture's natural size (points) and texture.
    const std::string caption = lb.zoom == 1 ? lb.zoom_alt : "";
    float nat_w = 0, nat_h = 0;
    const LoadedImage* img = nullptr;
    const MathTex* nat_d = nullptr;
    if (lb.zoom == 1) {
        const std::u8string u8(lb.zoom_src.begin(), lb.zoom_src.end());
        if ((img = image_get(fs::path(u8)))) nat_w = img->w, nat_h = img->h;
    } else if ((nat_d = diagram_get(lb.zoom_src, 1.05f * scale))) {
        nat_w = nat_d->px_w / scale, nat_h = nat_d->px_h / scale;
    }
    if (nat_w > 0) {
        const float max = lb.zoom == 1 ? nav::kLightboxMaxPicture : nav::kLightboxMaxDiagram;
        const ImVec2 s = nav::lightbox_size(nat_w, nat_h, sz.x / scale, sz.y / scale, !caption.empty(), max);
        const float w = std::round(s.x * scale), h = std::round(s.y * scale), pad = nav::kLightboxPad * scale;
        const float block_h = h + 2 * pad + (caption.empty() ? 0.0f : nav::kLightboxCaption * scale);
        const ImVec2 card(std::round(o.x + sz.x / 2 - w / 2 - pad), std::round(o.y + sz.y / 2 - block_h / 2));
        const ImVec2 card_max(card.x + w + 2 * pad, card.y + h + 2 * pad);
        dl->AddRectFilled(card, card_max, theme.bg, 6 * scale);
        const ImVec2 p(card.x + pad, card.y + pad), q(p.x + w, p.y + h);
        if (img) {
            dl->AddImage(img->tex, p, q);
        } else {  // drawn again at the zoomed size (half steps), so it stays sharp
            const float k = std::max(1.0f, std::ceil(s.x / nat_w * 2) / 2);
            const MathTex* d = diagram_get(lb.zoom_src, 1.05f * scale * k);
            if (!d) d = nat_d;
            dl->AddImage(d->tex, p, q, ImVec2(0, 0), ImVec2(1, 1), theme.text);
        }
        if (!caption.empty()) {
            const float em = 15.0f * scale;
            const float size = fonts.px(fonts.regular, em), adv = fonts.advance_scale(fonts.regular, em);
            float asc = 0, desc = 0;
            font_metrics(fonts, fonts.regular, em, asc, desc);
            const float tw = text_width(fonts.regular, size, adv, caption);
            draw_text(dl, fonts.regular, size, adv,
                      ImVec2(std::round(o.x + sz.x / 2 - tw / 2), std::round(card_max.y + (nav::kLightboxCaption * scale - asc - desc) / 2 + 4 * scale)),
                      IM_COL32(225, 225, 225, 255), caption);
        }
    }
    ImGui::End();
    if (close) st.lightbox.reset();
}

// Mouse and keyboard in the document view: link hover/clicks and text selection
// (hit-tested against the runs painted this frame).
static void handle_doc_input(DocState& st, const RenderResult& r, float view_top, float view_bottom, bool block_press) {
    const std::vector<Run>& runs = st.runs.runs;
    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsWindowHovered();
    const ImVec2 m = ImGui::GetMousePos();

    // A copy asked for last frame: this frame laid out every selected block.
    if (st.copy_requested) {
        if (auto range = st.sel.range()) {
            std::string text = selected_text(runs, *range);
            if (!text.empty()) ImGui::SetClipboardText(text.c_str());
        }
        st.copy_requested = false;
    }

    // Hover: tooltip + hand over links/pictures, I-beam over text.
    const LinkHit* link = nullptr;
    if (hovered && !r.over_code_control)
        for (const auto& l : r.links)
            if (m.x >= l.min.x && m.x < l.max.x && m.y >= l.min.y && m.y < l.max.y) {
                link = &l;
                break;
            }
    st.over_link = link && !link->url.empty();
    // Middle-click on a link (spec 34): follow it in a new tab (a link to a file; others as usual).
    if (st.over_link && !block_press && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) st.link_request = link->url, st.link_new_tab = true;
    if (link) {
        const std::string& tip = link->tip.empty() ? link->url : link->tip;
        if (!tip.empty() && tooltips_allowed()) ImGui::SetTooltip("%s", tip.c_str());
        if (!link->url.empty() || link->zoom) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    } else if (hovered && !r.over_code_control &&
               std::any_of(runs.begin(), runs.end(), [&](const Run& run) { return run.contains(m); })) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }

    // Press: 1 click starts a selection (or extends it with Shift), 2 = word, 3 = paragraph.
    if (hovered && !block_press && !r.mouse_captured && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        st.press_link = link ? link->url : "";
        st.press_new_tab = io.KeyCtrl;
        st.press_zoom.reset();
        if (link && link->url.empty() && link->zoom) st.press_zoom = *link;
        auto p = hit(runs, m);
        int clicks = io.MouseClickedCount[ImGuiMouseButton_Left];
        if (st.live && st.live_open && io.KeyCtrl) {
            // Ctrl+click in an editor preview (spec 23): only asks the editor to move. No selection
            // change, and a link is not followed (the release sends the message).
            st.goto_press = true;
            st.goto_from = m;
            st.press_link.clear();
        } else if (p && clicks == 2) {
            if (auto w = word_at(runs, *p)) st.sel.set(w->first, w->second);
        } else if (p && clicks >= 3) {
            if (auto g = group_at(runs, *p)) st.sel.set(g->first, g->second);
        } else if (p) {
            if (!(io.KeyShift && st.sel.anchor)) st.sel.anchor = p;
            st.sel.focus = p;
            st.selecting = true;
        }
    }
    // Drag: move the focus; dragging past the top/bottom edge scrolls.
    if (st.selecting && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        st.press_link.clear();  // a drag that started on a link selects instead of following it
        st.press_zoom.reset();
        if (auto p = hit(runs, m)) st.sel.focus = p;
        const float edge = 24.0f;
        if (m.y < view_top + edge) st.scroll_target -= (view_top + edge - m.y) * 0.5f;
        if (m.y > view_bottom - edge) st.scroll_target += (m.y - (view_bottom - edge)) * 0.5f;
    }
    // Release: a click (no drag) on a link follows it.
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        // A Ctrl+click is a click, not a drag: the pointer stayed within a few pixels.
        if (st.goto_press && std::abs(m.x - st.goto_from.x) <= 6 && std::abs(m.y - st.goto_from.y) <= 6) st.goto_click = m;
        st.goto_press = false;
        if (!st.press_link.empty()) st.link_request = st.press_link, st.link_new_tab = st.press_new_tab;
        st.press_link.clear();
        if (st.press_zoom) st.lightbox = std::move(st.press_zoom), st.sel.clear();
        st.press_zoom.reset();
        st.selecting = false;
    }

    // Keyboard: Ctrl+A, Ctrl+C (not while typing in a text field). Copy happens next frame, see above.
    if (!io.WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) st.sel.set(TextPos::start(), TextPos::end());
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) st.copy_requested = true;
    }

    // Right-click menu (on a formula it offers its TeX source, spec 27).
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        st.menu_math.clear();
        for (const Collector::Math& f : st.runs.maths)
            if (m.x >= f.min.x && m.x < f.max.x && m.y >= f.min.y && m.y < f.max.y) st.menu_math = f.src;
    }
    if (ImGui::BeginPopupContextWindow("doc-menu")) {
        if (!st.menu_math.empty() && ImGui::MenuItem("Copy LaTeX")) {
            ImGui::SetClipboardText(st.menu_math.c_str());
            st.toast = "Copied " + shorten(st.menu_math, 60);
            st.toast_since = ImGui::GetTime();
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, st.sel.range().has_value())) st.copy_requested = true;
        if (ImGui::MenuItem("Select all", "Ctrl+A")) st.sel.set(TextPos::start(), TextPos::end());
        ImGui::EndPopup();
    }
}

// Editor scroll sync (spec 15), cursor marker and sync-dump (spec 16, 18): where source line
// `line` sits. The target is the block containing the line (else the last block starting
// before it) and, inside a list / quote / callout, one level further: the item / child
// (editor::select_target). Inside the target the position is proportional to the line's place
// in the target's line range. Needs a valid layout cache. All values are pixels from the top
// of the document content (document y = 0, without top_pad).
struct SyncTarget {
    int first = 0, last = 0;  // the target's source lines
    bool tick = false;        // the marker gets a tick (see editor::SyncNode::plain)
    float top = 0, height = 0;
    float y = 0;              // the line's interpolated y
};
// The nodes select_target works on: top-level blocks plus, for lists and quotes, their items /
// children (only if the renderer measured one rect for each).
static std::vector<editor::SyncNode> sync_nodes(const DocState& st) {
    using K = md::Block::Kind;
    auto plain_kind = [](const md::Block& b) {
        return b.kind == K::Heading || b.kind == K::Paragraph || b.kind == K::Images || b.kind == K::Rule;
    };
    // How a click inside the block becomes a line (spec 23).
    auto node_kind = [](const md::Block& b) {
        switch (b.kind) {
            case K::Code: return b.lang == "mermaid" ? editor::NodeKind::Atom : editor::NodeKind::Code;
            case K::Table: return editor::NodeKind::Table;
            case K::Math: case K::Images: case K::Rule: return editor::NodeKind::Atom;
            default: return editor::NodeKind::Text;
        }
    };
    std::vector<editor::SyncNode> nodes;
    nodes.reserve(st.doc.size());
    for (size_t i = 0; i < st.doc.size(); i++) {
        const md::Block& b = st.doc[i];
        editor::SyncNode n;
        n.lines = {b.line_first, b.line_last};
        n.plain = plain_kind(b);
        n.kind = node_kind(b);
        std::vector<editor::SyncNode> subs;
        if (b.kind == K::List && b.item_lines.size() == b.items.size()) {
            for (size_t k = 0; k < b.items.size(); k++) {
                editor::SyncNode s;
                s.lines = {b.item_lines[k].first, b.item_lines[k].last};
                s.plain = b.items[k].empty() || (b.items[k].size() == 1 && plain_kind(b.items[k][0]));  // one paragraph: no tick
                subs.push_back(std::move(s));
            }
        } else if (b.kind == K::Quote) {
            if (!b.callout.empty()) subs.push_back({{b.title_line, b.title_line}, true, {}});
            for (const md::Block& c : b.children) subs.push_back({{c.line_first, c.line_last}, plain_kind(c), {}, node_kind(c)});
        }
        if (i < st.cache.sub.size() && st.cache.sub[i].size() == subs.size()) n.subs = std::move(subs);
        nodes.push_back(std::move(n));
    }
    return nodes;
}
static std::optional<SyncTarget> sync_target(const DocState& st, const std::vector<editor::SyncNode>& nodes, int line) {
    const auto& tops = st.cache.top;
    const auto& heights = st.cache.height;
    if (st.doc.empty() || tops.size() < st.doc.size() || heights.size() < st.doc.size()) return std::nullopt;
    const editor::SyncTarget sel = editor::select_target(nodes, line);
    SyncTarget t;
    t.first = sel.lines.first, t.last = sel.lines.last, t.tick = sel.tick;
    if (sel.sub >= 0) {
        const SubRect& r = st.cache.sub[sel.block][size_t(sel.sub)];
        t.top = r.top, t.height = r.height;
    } else {
        t.top = tops[sel.block];
        t.height = std::isnan(heights[sel.block]) ? 0.0f : heights[sel.block];
    }
    t.y = t.top + editor::line_fraction(sel.lines, line) * t.height;
    return t;
}
static std::optional<SyncTarget> sync_target(const DocState& st, int line) {
    return sync_target(st, sync_nodes(st), line);
}
// The same, in scroll offset units (what the scroll sync works in).
static std::optional<float> sync_target_y(const DocState& st, int line, float scale) {
    if (auto t = sync_target(st, line)) return top_pad(scale) + t->y;
    return std::nullopt;
}

// Ctrl+click (spec 23): the source line under document height `y` (pixels from the top of the first
// block), the exact inverse of sync_target. 0 when nothing is measured yet. `x` (from the
// document's left edge) only matters in slides' column layouts: blocks of other columns don't count.
static int sync_line_at(const DocState& st, float x, float y) {
    const auto& tops = st.cache.top;
    const auto& heights = st.cache.height;
    if (st.doc.empty() || tops.size() < st.doc.size() || heights.size() < st.doc.size()) return 0;
    const std::vector<editor::SyncNode> nodes = sync_nodes(st);
    auto span = [&](size_t block, int sub) -> std::optional<std::pair<float, float>> {
        if (block >= st.doc.size()) return std::nullopt;
        if (block < st.cache.hit_x.size() && (x < st.cache.hit_x[block].first || x >= st.cache.hit_x[block].second)) return std::nullopt;
        if (sub >= 0) {
            if (block >= st.cache.sub.size() || size_t(sub) >= st.cache.sub[block].size()) return std::nullopt;
            const SubRect& r = st.cache.sub[block][size_t(sub)];
            return std::make_pair(r.top, r.height);
        }
        if (std::isnan(heights[block])) return std::nullopt;
        return std::make_pair(tops[block], heights[block]);
    };
    auto rows = [&](size_t block, int sub) {
        std::vector<editor::Band> out;
        const std::vector<RowBand>* src = nullptr;
        if (sub >= 0) {
            if (block < st.cache.sub.size() && size_t(sub) < st.cache.sub[block].size()) src = &st.cache.sub[block][size_t(sub)].rows;
        } else if (block < st.cache.rows.size()) {
            src = &st.cache.rows[block];
        }
        if (src)
            for (const RowBand& b : *src) out.push_back({b.top, b.height});
        return out;
    };
    return editor::line_at(nodes, y, span, rows);
}

// The cursor marker (spec 16, 18): a thin bar in the left margin beside the target, plus a tick
// at the interpolated line when the target is several source lines of code, math, a table...
static void draw_cursor_marker(ImDrawList* dl, const DocState& st, const Theme& theme, ImVec2 origin, float scale) {
    if (!st.live || st.sync_line <= 0) return;
    const auto t = sync_target(st, st.sync_line);
    if (!t) return;
    const float right = origin.x - 8.0f * scale, w = 2.0f * scale;
    const float y0 = origin.y + t->top;
    const ImU32 rgb = theme.ui.accent & ~IM_COL32_A_MASK;
    dl->AddRectFilled(ImVec2(right - w, y0), ImVec2(right, y0 + t->height), rgb | (ImU32(0.35f * 255) << IM_COL32_A_SHIFT));
    if (t->tick) {
        const float cx = right - w / 2, ty = origin.y + t->y;
        dl->AddRectFilled(ImVec2(cx - 4.0f * scale, ty - 1.0f * scale), ImVec2(cx + 4.0f * scale, ty + 1.0f * scale), rgb | (ImU32(0.8f * 255) << IM_COL32_A_SHIFT));
    }
}

// `sync-dump` y values count from the top of the scrolling area (the Rust port: egui's ScrollArea
// content), where the first block starts 16 pt down; our t->y counts from the first block.
constexpr float kSyncDumpOriginPt = 16.0f;

// `sync-dump` (spec 16): one TSV row per source line of the shown document text.
static std::string sync_dump(DocState& st, const Fonts& fonts, const Theme& theme) {
    std::string out = "line\tfirst\tlast\ty\n";
    if (st.view_w <= 0) return out;
    layout_all(st, fonts, theme, column_width(st.view_w, fonts.scale));
    // Normal documents: read the text back from the disk (the shown one).
    std::string text = st.live ? st.live_text : "";
    if (!st.live && !st.path.empty()) text = platform::read_file(st.path).value_or("");
    const int n = editor::count_lines(text);
    const std::vector<editor::SyncNode> nodes = sync_nodes(st);
    for (int line = 1; line <= n; line++) {
        const auto t = sync_target(st, nodes, line);
        char buf[96];
        std::snprintf(buf, sizeof buf, "%d\t%d\t%d\t%.1f\n", line, t ? t->first : 0, t ? t->last : 0, t ? t->y / fonts.scale + kSyncDumpOriginPt : 0.0f);
        out += buf;
    }
    return out;
}

// The note shown for a file that is not text (see md::looks_binary), centred near the top.
static void draw_binary_note(const DocState& st, const Theme& theme, float scale) {
    const std::string msg = md::binary_message(to_utf8(st.path.filename()), st.binary);
    const float w = std::min(ImGui::GetWindowWidth() - 40.0f * scale, 560.0f * scale);
    const float x = (ImGui::GetWindowWidth() - w) / 2;
    ImGui::SetCursorPos(ImVec2(x, 40.0f * scale));
    ImGui::PushStyleColor(ImGuiCol_Text, theme.text);
    ImGui::PushTextWrapPos(x + w);
    ImGui::TextUnformatted(msg.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::SetCursorPos(ImVec2(x, ImGui::GetCursorPosY() + 12.0f * scale));
    if (ImGui::Button("Open with default app")) platform::open_path(st.path);
}

// The scrolling document view. Returns the outline found while drawing.
#ifdef __ANDROID__
static bool g_touch_ui_only = false;  // the settings window is open: every finger belongs to its widgets
// The pinch gesture (spec 35): scale `s` since the fingers came down, about `from` (where they were), moved to `to`.
static struct PinchGesture {
    bool active = false;
    float s = 1.0f;
    ImVec2 from{0, 0}, to{0, 0};
    double z0 = 1.0;  // the zoom the document is laid out at
} g_pinch;
// Frame costs during a pinch, logged when the fingers lift (to compare ways of zooming; logcat tag "calligrus").
static struct PinchStats {
    bool on = false;
    int frames = 0;
    double work = 0, wall = 0, worst = 0, last_end = 0;
} g_pstat;
// A developer switch: a file named pinch_legacy in the home folder makes a pinch change the zoom (and lay the
// document out again) at every step, as the first Android version did. Only for measuring.
static bool pinch_legacy() {
    static int legacy = -1;
    if (legacy < 0) {
        const char* home = std::getenv("HOME");
        std::error_code ec;
        legacy = home && fs::exists(fs::path(home) / "pinch_legacy", ec) ? 1 : 0;
    }
    return legacy == 1;
}
#endif
static std::vector<OutlineEntry> draw_document(DocState& st, FindState& find, nav::Autoscroll& autoscroll,
                                               const Fonts& fonts, const Theme& theme, bool line_numbers, bool editor_marker,
                                               bool block_press, bool& animate) {
    const float scale = fonts.scale;

    // Before the child window starts, with last frame's view size: things
    // that need a whole-document layout (and may set this frame's scroll).
    if (st.view_w > 0) {
        const float col_w = column_width(st.view_w, scale);
        if (st.restore_y >= 0) {
            // A newly opened file (or a history step): put the view at the saved offset.
            float height = 0;
            layout_all(st, fonts, theme, col_w, &height);
            st.content_h = height + bottom_pad(scale);
            const float y = std::min(st.restore_y, std::max(0.0f, st.content_h - st.view_h));
            st.restore_y = -1;
            st.zoom_anchor.reset();
            ImGui::SetNextWindowScroll(ImVec2(-1.0f, y));
            st.scroll_target = st.scroll_cur = st.scroll_last_set = y;
        }
        if (st.zoom_anchor && (st.zoom_anchor_scale == 0 || std::fabs(scale - st.zoom_anchor_scale) < 0.01f)) {
            st.zoom_anchor_scale = 0;
            // Zoom / text size changed: put the block that was at the top back there.
            float height = 0;
            layout_all(st, fonts, theme, col_w, &height);
            auto [i, frac] = *st.zoom_anchor;
            st.zoom_anchor.reset();
            const float anchor_off = st.zoom_anchor_off;
            st.zoom_anchor_off = 0;
            if (i < st.cache.top.size()) {
                const float h = std::isnan(st.cache.height[i]) ? 0.0f : st.cache.height[i];
                const float y = std::max(0.0f, std::round(top_pad(scale) + st.cache.top[i] + frac * h - anchor_off));
                st.content_h = height + bottom_pad(scale);
                ImGui::SetNextWindowScroll(ImVec2(-1.0f, y));
                st.scroll_target = st.scroll_cur = st.scroll_last_set = y;
            }
        }
        if (st.live && (st.sync_pending || st.sync_reapply)) {
            // Editor cursor sync: see sync_target_y. A `cursor` scrolls only when the line is
            // outside the middle 60 % of the view (then puts it at 1/3, smoothly); after a
            // text update the line goes back to where it was in the view, at once.
            const bool instant = !st.sync_pending;
            st.sync_pending = st.sync_reapply = false;
            float height = 0;
            layout_all(st, fonts, theme, col_w, &height);
            st.content_h = height + bottom_pad(scale);
            if (auto y = sync_target_y(st, st.sync_line, scale)) {
                const float max_scroll = std::max(0.0f, st.content_h - st.view_h);
                if (instant) {
                    if (st.sync_view_y >= 0) {
                        const float to = std::clamp(std::round(*y - st.sync_view_y), 0.0f, max_scroll);
                        ImGui::SetNextWindowScroll(ImVec2(-1.0f, to));
                        st.scroll_target = st.scroll_cur = st.scroll_last_set = to;
                        st.scroll_to = -1;
                    }
                } else {
                    const float at = *y - st.scroll_target;
                    if (at < 0.2f * st.view_h || at > 0.8f * st.view_h)
                        st.scroll_to = std::clamp(std::round(*y - st.view_h / 3), 0.0f, max_scroll);
                    st.sync_view_y = *y - (st.scroll_to >= 0 ? st.scroll_to : st.scroll_target);
                }
            }
        }
        update_find(st, find, fonts, theme, col_w);
        update_editor_search(st, find, fonts, theme, col_w);
        if (find.scroll_to_current && find.current >= 0) {
            // Centre the current match if it's off screen.
            auto [top, bottom] = find.match_y[size_t(find.current)];
            const float y0 = top_pad(scale) + top, y1 = top_pad(scale) + bottom;
            if (y0 < st.scroll_cur || y1 > st.scroll_cur + st.view_h) st.scroll_to = std::max(0.0f, (y0 + y1 - st.view_h) / 2);
            find.scroll_to_current = false;
        }
    }
    // The scrollable height is known before drawing (last frame's, or the
    // zoom layout's), so a new scroll position isn't clamped to a stale size.
    if (st.content_h > 0) ImGui::SetNextWindowContentSize(ImVec2(0, st.content_h));

    // Floating scrollbar like egui's (spec 21): invisible until the view scrolls or the
    // pointer is near the right edge, then it hides again after a second.
    // The bar doesn't fade, so showing it needs no extra frames: the idle loop just
    // wakes once at `bar_wake` to hide it (redrawing every frame while it showed cost
    // a CPU core with software GL, e.g. over RDP).
    bool bar_visible;
    {
        const double now_t = ImGui::GetTime();
        if (std::abs(st.scroll_cur - st.bar_prev_y) > 0.01f) st.bar_until = now_t + 1.0;
        st.bar_prev_y = st.scroll_cur;
        const ImGuiViewport* v = ImGui::GetMainViewport();
        const ImVec2 m = ImGui::GetIO().MousePos;
        const bool near_edge = m.x > v->WorkPos.x + v->WorkSize.x - 16.0f * scale && m.x <= v->WorkPos.x + v->WorkSize.x;
        // Near the edge the bar simply stays (moving away is an input event that redraws);
        // 0.3 s after the pointer leaves, it hides.
        if (near_edge) st.bar_until = std::max(st.bar_until, now_t + 0.3);
        st.bar_wake = !near_edge && st.bar_until > now_t ? st.bar_until : 0;
        bar_visible = near_edge || now_t < st.bar_until;
    }
    if (!bar_visible)
        for (ImGuiCol c : {ImGuiCol_ScrollbarGrab, ImGuiCol_ScrollbarGrabHovered, ImGuiCol_ScrollbarGrabActive})
            ImGui::PushStyleColor(c, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.bg);  // the document's own background (toolbar/outline: ui.panel)
    ImGui::BeginChild("doc", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
#ifdef __ANDROID__  // fingers that start here scroll and zoom the document (spec 35); not under a popup or the settings window
    android_shell::set_doc_region(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                                  ImGui::GetWindowPos().y + ImGui::GetWindowHeight(),
                                  g_touch_ui_only || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
#endif
    // (the scrollbar colours stay pushed: ImGui draws the bar in EndChild)
    smooth_scroll(st, fonts);

    ImVec2 pos = ImGui::GetCursorScreenPos();  // already shifted by the scroll offset
    // Whole view width: egui's scrollbar floats over the content instead of taking space.
    st.view_w = ImGui::GetWindowWidth();
    st.view_h = ImGui::GetWindowHeight();
    st.view_y = ImGui::GetWindowPos().y;
    if (!st.binary.empty()) {  // not a text file: a note and a button instead of the document
        draw_binary_note(st, theme, scale);
        ImGui::EndChild();
        if (!bar_visible) ImGui::PopStyleColor(3);
        return {};
    }
    const float margin = panel_margin(scale);
    const float avail = st.view_w - 2 * margin;
    const float col_w = column_width(st.view_w, scale);
    ImVec2 origin(std::round(pos.x + margin + (avail - col_w) / 2), std::round(pos.y + top_pad(scale)));

    float view_top = ImGui::GetWindowPos().y;
    float view_bottom = view_top + st.view_h;
    const bool hovered = ImGui::IsWindowHovered();
    // A pinch is on: what is drawn gets scaled afterwards, so blocks are drawn for the range that maps onto the view.
    const bool pinch_view = st.pinch_s != 1.0f;
    if (pinch_view) {
        const float inv = 1.0f / st.pinch_s;
        view_top = st.pinch_from.y + (view_top - st.pinch_to.y) * inv;
        view_bottom = st.pinch_from.y + (view_bottom - st.pinch_to.y) * inv;
    }
    ImDrawList* doc_dl = ImGui::GetWindowDrawList();
    int pinch_cmd0 = 0, pinch_vtx0 = 0;
    if (pinch_view) {
        doc_dl->AddDrawCmd();  // (a command of its own, so no earlier shape shares it)
        pinch_cmd0 = doc_dl->CmdBuffer.Size - 1, pinch_vtx0 = doc_dl->VtxBuffer.Size;
    }

    // Middle-click autoscroll starts over the document.
    if (hovered && !st.over_link && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {  // (over a link it opens a tab, spec 34)
        const ImVec2 m = ImGui::GetMousePos();
        autoscroll.middle_pressed(m.x, m.y, ImGui::GetTime());
    }

    // A selection belongs to one layout: drop it when the document, column
    // width or text size changes.
    const size_t key = layout_key(st, col_w, fonts);
    if (key != st.sel_key) {
        st.sel.clear();
        st.sel_key = key;
    }
    RenderOptions opts;
    opts.line_numbers = line_numbers;
    opts.interactive = !autoscroll.active();
    opts.mouse_in_view = hovered;
    opts.find = find.open ? &find.matches : find.ed_matches.empty() ? nullptr : &find.ed_matches;
    opts.find_current = find.open ? find.current : -1;
    RenderResult r = render_document(doc_dl, st.doc, fonts, theme, origin, col_w, st.cache,
                                     st.version, view_top, view_bottom, base_dir(st), st.runs, st.sel.range(),
                                     ImGui::GetColorU32(ImGuiCol_TextSelectedBg), opts);
#ifdef __ANDROID__  // wide code blocks and formulas: a sideways finger drag on them scrolls them (spec 35)
    {
        std::vector<android_shell::HScrollBlock> hb;
        for (const HBlock& b : r.hblocks) hb.push_back({b.min.x, b.min.y, b.max.x, b.max.y, b.key});
        android_shell::set_hscroll_blocks(hb);
    }
#endif

    if (editor_marker) draw_cursor_marker(ImGui::GetWindowDrawList(), st, theme, origin, scale);
    // Marks (spec 28): their letters, faint, in the margin left of the text, at the spot that was
    // the top of the view when they were set.
    if (!st.mark_labels.empty()) {
        const float em = fonts.body * 0.75f;
        const float size = fonts.px(fonts.regular, em), adv = fonts.advance_scale(fonts.regular, em);
        for (const DocState::MarkLabel& m : st.mark_labels) {
            if (m.block >= st.cache.top.size() || m.block >= st.cache.height.size() || std::isnan(st.cache.height[m.block])) continue;
            const float y = origin.y + st.cache.top[m.block] + m.frac * st.cache.height[m.block];
            const float w = text_width(fonts.regular, size, adv, m.label);
            draw_text(ImGui::GetWindowDrawList(), fonts.regular, size, adv, ImVec2(std::round(origin.x - 10.0f * scale - w), std::round(y)),
                      theme.weak, m.label);
        }
    }

    if (pinch_view) {  // the GPU-side zoom: scale what was just drawn about the pinch point (clip rects too)
        const float k = st.pinch_s;
        const ImVec2 from = st.pinch_from, to = st.pinch_to;
        auto map = [&](float x, float y) { return ImVec2(to.x + (x - from.x) * k, to.y + (y - from.y) * k); };
        for (int i = pinch_vtx0; i < doc_dl->VtxBuffer.Size; i++) {
            ImDrawVert& v = doc_dl->VtxBuffer[i];
            v.pos = map(v.pos.x, v.pos.y);
        }
        const ImVec2 wmin = ImGui::GetWindowPos(), wmax(wmin.x + ImGui::GetWindowWidth(), wmin.y + ImGui::GetWindowHeight());
        for (int c = pinch_cmd0; c < doc_dl->CmdBuffer.Size; c++) {
            ImVec4& cr = doc_dl->CmdBuffer[c].ClipRect;
            const ImVec2 a = map(cr.x, cr.y), b = map(cr.z, cr.w);
            cr = ImVec4(std::max(a.x, wmin.x), std::max(a.y, wmin.y), std::min(b.x, wmax.x), std::min(b.y, wmax.y));
            if (cr.z < cr.x) cr.z = cr.x;
            if (cr.w < cr.y) cr.w = cr.y;
        }
    }

    // Tell ImGui how big the content is so the scrollbar works.
    st.content_h = r.height + bottom_pad(scale);
    ImGui::Dummy(ImVec2(avail, st.content_h));

    handle_doc_input(st, r, view_top, view_bottom, block_press || autoscroll.active());
    draw_toast(st, fonts, theme, ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));
    if (st.goto_click) {
        // Ctrl+click (spec 23): tell the editor which source line was clicked. No scrolling here:
        // the editor answers with a normal `cursor` message.
        const ImVec2 at = *st.goto_click;
        st.goto_click.reset();
        if (st.live && st.live_open)
            if (const int line = sync_line_at(st, at.x - origin.x, at.y - origin.y); line > 0) {
                platform::allow_editor_focus();
                editor_emit(editor::goto_message(st.live_id, line));
            }
    }
    if (r.animating) animate = true;
    ImGui::EndChild();
    if (!bar_visible) ImGui::PopStyleColor(3);
    return std::move(r.outline);
}

// ---------------------------------------------------------------------------
// Slides mode (spec 26)
// ---------------------------------------------------------------------------

// A slide is a range of top-level blocks (slides.h) drawn alone, scaled to the window. The whole
// document is laid out as usual (so find, selection and sync keep working) at a reading width of
// 700 units; only the slide's blocks are drawn, at zoom `z`.
struct SlideState {
    bool on = false;
    std::vector<slides::Range> ranges;
    size_t version = SIZE_MAX;   // document version `ranges` / `h` belong to
    std::string doc_id;          // which document (path / editor id): a different one starts at slide 1
    size_t index = 0;            // the current slide
    std::vector<std::vector<slides::Step>> steps;  // pauses, incremental lists: per slide, where each step starts
    std::vector<std::string> notes;  // speaker notes, per slide (maybe empty); shown at the bottom (`N`)?
    std::vector<std::vector<slides::Background>> bgs;  // each slide's background pictures (Marp `![bg]`)
    bool notes_shown = false;
    std::string notes_sent;          // "doc id/slide/notes" the editor got last (each slide once)
    double since = 0;                // the presenter clock (`t`): ImGui time slides mode started
    bool clock_shown = false;
    bool overview = false;           // the slide overview (`o`)...
    size_t overview_sel = 0;         // ...the slide picked in it
    size_t overview_cols = 1;        // ...and its columns (as drawn last)
    float overview_scroll = 0;       // a long deck's overview scrolls: px from its top...
    size_t overview_followed = SIZE_MAX;  // ...and the slide it was last scrolled to show (again when the pick moves)
    bool presenter = false;          // the presenter window (`s`)
    float audience_w = 1100, audience_h = 800;  // the slide view's size (px, as of the last frame): the presenter's pictures have its shape
    size_t step = 0;             // the step on screen (0..steps[index].size())
    std::vector<float> h;        // each slide's laid-out height in units (zoom 1)
    size_t measured = 0;         // hash of what `h` was measured with (0 = measure again)
    float z = 1;                 // the zoom the current slide is drawn at
    float scroll = 0;            // px scrolled within a slide that doesn't fit
    bool want_on = false;        // --slides: switch on once the first frame is up
};

// One open document (spec 34). The frame loop works on the ACTIVE tab through its own variables
// (`st`, `find`, `slides`, `history`: many lambdas hold them by reference); the other tabs wait in
// `tabs`, and switching swaps the two (see `swap_live` in main).
struct Tab {
    DocState st;
    FindState find;
    SlideState slides;
    session::History history;
};

static std::string slide_doc_id(const DocState& st) { return st.live ? "\x02" + st.live_id : to_utf8(st.path); }

// A slide's top and bottom (px from the top of the document, as laid out last). With column
// layouts the last block need not be the lowest one, so look at all of them.
static std::pair<float, float> slide_extent(const DocState& st, const slides::Range& r) {
    float top = FLT_MAX, bottom = -FLT_MAX;
    for (size_t i = r.first; i <= r.last && i < st.cache.top.size(); i++) {
        const float h = std::isnan(st.cache.height[i]) ? 0.0f : st.cache.height[i];
        // (the empty lines asked for above a block are part of the slide)
        top = std::min(top, st.cache.top[i] - (i < st.cache.space.size() ? st.cache.space[i] : 0.0f));
        bottom = std::max(bottom, st.cache.top[i] + h);
    }
    return top <= bottom ? std::make_pair(top, bottom) : std::make_pair(0.0f, 0.0f);
}

// Re-split after a (re)load and measure each slide at zoom 1 (laid out with fonts at scale
// `dpi`, so units are logical pixels); then pick this frame's zoom. `fonts` is left at scale dpi.
static void slides_prepare(SlideState& ss, DocState& st, Fonts& fonts, const Theme& theme, float dpi, int body_size, float win_w, float win_h) {
    if (ss.version != st.version) {
        ss.ranges = st.split();
        ss.steps = st.steps(ss.ranges);
        st.columns = slides::placements(ss.ranges, st.doc, st.layouts);
        st.styles = slides::text_styles(ss.ranges, st.doc, st.texts);
        if (std::all_of(st.styles.begin(), st.styles.end(), [](const slides::TextStyle& s) { return s == slides::TextStyle{}; })) st.styles.clear();
        ss.notes = slides::slide_notes(ss.ranges, st.doc, st.notes);
        ss.bgs = slides::slide_backgrounds(ss.ranges, st.doc);
        slides::place_backgrounds(st.columns, ss.ranges, ss.bgs, st.doc.size());
        st.backgrounds.clear();
        for (size_t i = 0; i < st.doc.size(); i++)
            if (!slides::block_backgrounds(st.doc[i]).empty()) {
                st.backgrounds.resize(st.doc.size(), 0);
                st.backgrounds[i] = 1;
            }
        ss.version = st.version;
        ss.measured = 0;
        const std::string id = slide_doc_id(st);
        if (id != ss.doc_id) ss.index = 0, ss.step = 0, ss.scroll = 0;
        ss.doc_id = id;
    }
    if (!ss.ranges.empty()) ss.index = std::min(ss.index, ss.ranges.size() - 1);
    ss.step = std::min(ss.step, ss.index < ss.steps.size() ? ss.steps[ss.index].size() : size_t(0));
    const size_t key = std::hash<size_t>{}(st.version) ^ (std::hash<int>{}(body_size) << 1) ^ (std::hash<float>{}(dpi) << 2) ^ 1;
    if (ss.measured != key) {
        fonts.scale = dpi;
        fonts.body = float(body_size) * dpi;
        float height = 0;
        layout_all(st, fonts, theme, 700.0f * dpi, &height);
        ss.h.clear();
        for (const slides::Range& r : ss.ranges) {
            if (r.last >= st.cache.top.size()) {
                ss.h.push_back(0);
                continue;
            }
            const auto [top, bottom] = slide_extent(st, r);
            ss.h.push_back((bottom - top) / dpi);
        }
        ss.measured = key;
    }
    const float h = ss.index < ss.h.size() ? ss.h[ss.index] : 0.0f;
    ss.z = slides::zoom(win_w / dpi, win_h / dpi, h);
}

// A slide's background picture in [a, b] (spec 26): covering it (cropped), or `contain`ed (whole,
// centred), with its filters. Nothing if remote or unreadable.
static void draw_background(ImDrawList* dl, const slides::Background& bg, const fs::path& base, ImVec2 a, ImVec2 b) {
    if (bg.src.starts_with("http://") || bg.src.starts_with("https://") || bg.src.starts_with("data:")) return;
    const LoadedImage* img = image_get(resolve(base, bg.src), bg.filters);
    if (!img) return;
    const float pic = img->w / std::max(img->h, 1.0f), room = (b.x - a.x) / std::max(b.y - a.y, 1.0f);
    ImVec2 p0 = a, p1 = b, uv0(0, 0), uv1(1, 1);
    if (bg.contain) {
        const ImVec2 size = pic > room ? ImVec2(b.x - a.x, (b.x - a.x) / pic) : ImVec2((b.y - a.y) * pic, b.y - a.y);
        const ImVec2 c((a.x + b.x) / 2, (a.y + b.y) / 2);
        p0 = ImVec2(c.x - size.x / 2, c.y - size.y / 2), p1 = ImVec2(c.x + size.x / 2, c.y + size.y / 2);
    } else if (pic > room) {
        const float w = room / pic;  // the middle part of the picture's width
        uv0.x = 0.5f - w / 2, uv1.x = 0.5f + w / 2;
    } else {
        const float h = pic / room;
        uv0.y = 0.5f - h / 2, uv1.y = 0.5f + h / 2;
    }
    dl->PushClipRect(a, b, true);
    dl->AddImage(img->tex, p0, p1, uv0, uv1);
    dl->PopClipRect();
}

// A slide's background pictures, under everything (spec 26): behind the whole view [lo, hi], or from
// the text column's gap (the slide's left edge is `slide_left`, drawn at `scale`) to the view's edge;
// several share that area (side by side, or stacked).
static void draw_backgrounds(ImDrawList* dl, const std::vector<slides::Background>& bgs, const fs::path& base, ImVec2 lo, ImVec2 hi,
                             float slide_left, float scale) {
    if (bgs.empty()) return;
    ImVec2 a = lo, b = hi;
    if (const slides::Background* strip = slides::backgrounds_strip(bgs)) {
        const float edge = slide_left + slides::background_split(*strip)->edge * scale;
        if (strip->side == slides::BgSide::Left) b.x = edge;
        else a.x = edge;
    }
    const auto cells = slides::background_cells(bgs, {a.x, a.y, b.x - a.x, b.y - a.y});
    for (size_t i = 0; i < bgs.size(); i++) {
        const auto [x, y, w, h] = cells[i];
        draw_background(dl, bgs[i], base, ImVec2(x, y), ImVec2(x + w, y + h));
    }
}

// Lines of `text` (split at newlines), each wrapped at spaces to `width` px at this size.
static std::vector<std::string> wrap_rows(const std::string& text, const Fonts& fonts, float size, float adv, float width) {
    std::vector<std::string> rows;
    size_t from = 0;
    while (from <= text.size()) {
        const size_t nl = std::min(text.find('\n', from), text.size());
        std::string line = text.substr(from, nl - from), row;
        for (size_t k = 0; k <= line.size();) {
            const size_t sp = std::min(line.find(' ', k), line.size());
            const std::string word = line.substr(k, sp - k);
            const std::string next = row.empty() ? word : row + " " + word;
            if (!row.empty() && text_width(fonts.regular, size, adv, next) > width) rows.push_back(row), row = word;
            else row = next;
            k = sp + 1;
        }
        rows.push_back(row);
        from = nl + 1;
    }
    return rows;
}

// Slide `k` as a picture in the cell at `a`, `cell_w` px wide (spec 26): drawn as draw_slide draws it in
// a `view_w` x `view_h` px window at its own zoom (fonts are at the current slide's scale), then its
// vertices and clip rects are shrunk into the cell. `step`: the steps shown (SIZE_MAX = all of it).
static void draw_slide_picture(ImDrawList* dl, DocState& st, const SlideState& ss, const Fonts& fonts, const Theme& theme, float dpi,
                               size_t k, size_t step, ImVec2 a, float cell_w, float view_w, float view_h) {
    const float scale = fonts.scale;
    const float col_w = 700.0f * scale;
    const ImVec2 b(a.x + cell_w, a.y + cell_w * view_h / std::max(view_w, 1.0f));
    dl->AddRectFilled(a, b, theme.bg);
    if (k >= ss.ranges.size()) return;
    if (st.cache.key != layout_key(st, col_w, fonts) || st.cache.height.size() != st.doc.size() ||
        std::any_of(st.cache.height.begin(), st.cache.height.end(), [](float v) { return std::isnan(v); }))
        layout_all(st, fonts, theme, col_w);
    // The canvas: the window at this slide's own zoom (a tall slide is drawn smaller).
    const float own = slides::zoom(view_w / dpi, view_h / dpi, k < ss.h.size() ? ss.h[k] : 0.0f);
    const float cw = view_w * ss.z / own, ch = view_h * ss.z / own;
    const float s = cell_w / cw;
    const int vtx0 = dl->VtxBuffer.Size;
    // (a command with nothing in it yet takes the new clip rect in place: it is ours then)
    const int cmd0 = dl->CmdBuffer.Size - (dl->CmdBuffer.Size > 0 && dl->CmdBuffer.back().ElemCount == 0 ? 1 : 0);
    dl->PushClipRect(a, ImVec2(a.x + cw, a.y + ch), false);
    const auto [top, bottom] = slide_extent(st, ss.ranges[k]);
    const float y0 = std::max((ch - (bottom - top)) / 2, 0.0f);
    const ImVec2 at(std::round(a.x + (cw - col_w) / 2), std::round(a.y + y0 - top));
    if (k < ss.bgs.size())
        draw_backgrounds(dl, ss.bgs[k], base_dir(st), a, ImVec2(a.x + cw, a.y + ch), a.x + (cw - col_w) / 2, scale);
    RenderOptions opts;
    opts.columns = columns_of(st);
    opts.styles = styles_of(st);
    opts.backgrounds = backgrounds_of(st);
    opts.margin = 0;
    if (k < ss.steps.size() && step < ss.steps[k].size()) std::tie(opts.hide_from, opts.hide_items) = ss.steps[k][step];
    Collector scratch;
    render_document(dl, st.doc, fonts, theme, at, col_w, st.cache, st.version, a.y + y0 + 0.5f, a.y + y0 + (bottom - top) - 0.5f,
                    base_dir(st), scratch, std::nullopt, 0, opts);
    dl->PopClipRect();
    auto shrink = [&](float x, float y) { return ImVec2(a.x + (x - a.x) * s, a.y + (y - a.y) * s); };
    for (int i = vtx0; i < dl->VtxBuffer.Size; i++) dl->VtxBuffer[i].pos = shrink(dl->VtxBuffer[i].pos.x, dl->VtxBuffer[i].pos.y);
    for (int i = cmd0; i < dl->CmdBuffer.Size - 1; i++) {  // (the last one is the outer clip again, after the pop)
        ImVec4& r = dl->CmdBuffer[i].ClipRect;
        const ImVec2 lo = shrink(r.x, r.y), hi = shrink(r.z, r.w);
        r = ImVec4(std::max(lo.x, a.x), std::max(lo.y, a.y), std::min(hi.x, b.x), std::min(hi.y, b.y));
    }
}

// ---------------------------------------------------------------------------
// Document export: pages (spec 29)
// ---------------------------------------------------------------------------

// A document export in progress (the Rust port: App::page_view and export_pages): the document is
// laid out as one column as wide as a page's text area, cut into pages by pages::page_ends, and
// one page at a time is drawn in a window the size of the paper.
struct PageExport {
    bool on = false;
    float paper_w = 0, paper_h = 0;  // points
    std::string theme;               // the theme's name
    std::vector<float> ends;         // where each page ends (document points), once planned
    size_t plan_key = SIZE_MAX;      // what `ends` was planned for (layout_key)
    size_t page = 0;                 // the page on screen (0-based)
    float body_pt = 0;               // the text size in points (--body-size; 0 = the config's body_size)
};

// Every top-level block as the page breaker wants it (kind, top, height, where its lines or rows
// end), in document points; `runs` are those of a layout_all of the whole document at `scale`
// pixels per point. `total` is the document's height.
static std::vector<pages::Block> page_blocks(const DocState& st, const std::vector<Run>& runs, float scale, float& total) {
    using BK = md::Block::Kind;
    std::vector<pages::Block> out;
    for (size_t i = 0; i < st.doc.size() && i < st.cache.top.size(); i++) {
        const md::Block& b = st.doc[i];
        pages::Block pb;
        pb.top = st.cache.top[i] / scale;
        pb.height = (std::isnan(st.cache.height[i]) ? 0.0f : st.cache.height[i]) / scale;
        switch (b.kind) {
            case BK::Paragraph: case BK::Quote: case BK::List: pb.kind = pages::Kind::Text; break;
            case BK::Heading: pb.kind = pages::Kind::Heading; break;
            case BK::Code: pb.kind = b.lang == "mermaid" ? pages::Kind::Atomic : pages::Kind::Code; break;
            case BK::Table: pb.kind = pages::Kind::Table; break;
            case BK::Rule: case BK::Math: case BK::Images: pb.kind = pages::Kind::Atomic; break;
        }
        if (pb.kind == pages::Kind::Code || pb.kind == pages::Kind::Table) {
            // one list: the bottoms of the code lines / table rows
            std::vector<float> bottoms;
            for (const RowBand& r : st.cache.rows[i]) bottoms.push_back((r.top + r.height) / scale);
            pb.groups.push_back(std::move(bottoms));
        } else if (pb.kind != pages::Kind::Atomic) {
            // the bottoms of the visual lines of each text group (paragraph, list item, quote child...)
            bool open = false;
            uint32_t group = 0, line = 0;
            for (auto it = std::lower_bound(runs.begin(), runs.end(), TextPos{uint32_t(i), 0, 0}, [](const Run& r, const TextPos& p) { return r.pos < p; });
                 it != runs.end() && it->pos.block == i; ++it) {
                const float bottom = it->max.y / scale;
                if (!open || it->group != group) pb.groups.push_back({bottom});
                else if (it->line != line) pb.groups.back().push_back(bottom);
                else pb.groups.back().back() = std::max(pb.groups.back().back(), bottom);  // a taller run (a formula) on the same line
                open = true, group = it->group, line = it->line;
            }
        }
        out.push_back(std::move(pb));
    }
    total = out.empty() ? 0.0f : out.back().top + out.back().height;
    return out;
}

// Decide where the pages end (again if the layout changed since).
static void plan_pages(DocState& st, PageExport& pg, const Fonts& fonts, const Theme& theme) {
    const float col_w = (pg.paper_w - 2 * pages::kMargin) * fonts.scale;
    const size_t key = layout_key(st, col_w, fonts);
    if (key == pg.plan_key && !pg.ends.empty()) return;
    const std::vector<Run> runs = layout_all(st, fonts, theme, col_w);
    float total = 0;
    const std::vector<pages::Block> blocks = page_blocks(st, runs, fonts.scale, total);
    pg.ends = pages::page_ends(blocks, pg.paper_h - 2 * pages::kMargin, total);
    pg.plan_key = key;
    pg.page = std::min(pg.page, pg.ends.size() - 1);
}

// One page: the document scrolled so that the page's start sits at the top margin, then everything
// above the text area and below the page's end painted over with the page background, and the page
// number in the bottom margin.
static void draw_page(DocState& st, PageExport& pg, const Fonts& fonts, const Theme& theme) {
    plan_pages(st, pg, fonts, theme);
    const float scale = fonts.scale, m = pages::kMargin;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float w = std::round(pg.paper_w * scale), h = std::round(pg.paper_h * scale);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), theme.bg);
    const float y0 = pg.page == 0 ? 0.0f : pg.ends[pg.page - 1], end = pg.ends[pg.page];
    const ImVec2 origin(std::round(pos.x + m * scale), std::round(pos.y + (m - y0) * scale));
    RenderOptions opts;
    opts.columns = columns_of(st);
    opts.styles = styles_of(st);
    opts.backgrounds = backgrounds_of(st);
    Collector scratch;
    render_document(dl, st.doc, fonts, theme, origin, (pg.paper_w - 2 * m) * scale, st.cache, st.version, pos.y, pos.y + h, base_dir(st), scratch,
                    std::nullopt, 0, opts);
    dl->AddRectFilled(pos, ImVec2(pos.x + w, std::round(pos.y + m * scale)), theme.bg);
    const float below = std::min(std::round(pos.y + (m + end - y0) * scale), std::round(pos.y + (pg.paper_h - m) * scale));
    dl->AddRectFilled(ImVec2(pos.x, below), ImVec2(pos.x + w, pos.y + h), theme.bg);
    // (the text of the other pages, captured for the PDF, is covered too)
    text_capture::cover(pos, ImVec2(pos.x + w, std::round(pos.y + m * scale)));
    text_capture::cover(ImVec2(pos.x, below), ImVec2(pos.x + w, pos.y + h));
    const std::string number = std::to_string(pg.page + 1) + " / " + std::to_string(pg.ends.size());
    const float em = pages::kNumberSize * scale;
    const float size = fonts.px(fonts.regular, em), adv = fonts.advance_scale(fonts.regular, em);
    const float tw = text_width(fonts.regular, size, adv, number);
    draw_text(dl, fonts.regular, size, adv, ImVec2(std::round(pos.x + w / 2 - tw / 2), std::round(pos.y + h - pages::kNumberBoxBottom * scale - size)), theme.weak, number);
}

// The time of day as "14:05" (the presenter's wall clock; wallclock.rs).
static std::string local_hm() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    if (localtime_s(&tm, &now) != 0) return "00:00";
#else
    if (!localtime_r(&now, &tm)) return "00:00";
#endif
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

// The presenter view (spec 26, `s`) in the `w` x `h` px area at `pos`: the slide on screen as the
// audience sees it, what the next key shows, the speaker notes, the slide and step numbers, the time of
// day and the time since the show started (`elapsed` seconds). Text is drawn at `ui_dpi` (the presenter window's), pictures are shrunk.
static void draw_presenter(ImDrawList* dl, DocState& st, const SlideState& ss, const Fonts& fonts, const Theme& theme, float dpi,
                           float ui_dpi, ImVec2 pos, float w, float h, double elapsed) {
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), theme.ui.panel);
    const slides::PresenterLayout lay = slides::presenter_layout(w / ui_dpi, h / ui_dpi, ss.audience_w / std::max(ss.audience_h, 1.0f));
    auto box = [&](const float r[4]) { return std::array<float, 4>{pos.x + r[0] * ui_dpi, pos.y + r[1] * ui_dpi, r[2] * ui_dpi, r[3] * ui_dpi}; };
    auto text = [&](float px, ImVec2 at, ImU32 color, const std::string& t, float align_x, float align_y) {
        const float em = px * ui_dpi;
        const float size = fonts.px(fonts.regular, em), adv = fonts.advance_scale(fonts.regular, em);
        const float tw = text_width(fonts.regular, size, adv, t);
        draw_text(dl, fonts.regular, size, adv, ImVec2(std::round(at.x - tw * align_x), std::round(at.y - size * align_y)), color, t);
    };
    // The top bar: where we are, and the clock (frozen in scripts, so their pictures don't change).
    const size_t n = std::max<size_t>(ss.ranges.size(), 1);
    const size_t steps = ss.index < ss.steps.size() ? ss.steps[ss.index].size() : 0;
    const size_t step = std::min(ss.step, steps);
    std::string status = "Slide " + std::to_string(ss.index + 1) + " / " + std::to_string(n);
    if (steps > 0) status += "   step " + std::to_string(step + 1) + " / " + std::to_string(steps + 1);
    const float mid = pos.y + lay.bar / 2 * ui_dpi;
    text(18, ImVec2(pos.x + slides::kPresenterPad * ui_dpi, mid), theme.ui.text, status, 0, 0.5f);
    const long secs = g_script_mode ? 0 : long(elapsed);
    char clock[32];
    if (secs >= 3600) std::snprintf(clock, sizeof clock, "%ld:%02ld:%02ld", secs / 3600, secs / 60 % 60, secs % 60);
    else std::snprintf(clock, sizeof clock, "%ld:%02ld", secs / 60, secs % 60);
    text(28, ImVec2(pos.x + w - slides::kPresenterPad * ui_dpi, mid), theme.ui.text, clock, 1, 0.5f);
    // The time of day, in the middle (12:00 in scripts).
    text(18, ImVec2(pos.x + w / 2, mid), theme.weak, g_script_mode ? std::string("12:00") : local_hm(), 0.5f, 0.5f);
    auto label = [&](const std::array<float, 4>& r, const char* what) { text(13, ImVec2(r[0], r[1] - 6 * ui_dpi), theme.weak, what, 0, 1); };
    auto frame = [&](const std::array<float, 4>& r) {
        dl->AddRect(ImVec2(r[0] - ui_dpi, r[1] - ui_dpi), ImVec2(r[0] + r[2] + ui_dpi, r[1] + r[3] + ui_dpi), theme.ui.border, 0, 0, ui_dpi);
    };
    // Now: the slide on screen, with the steps shown so far.
    const auto now = box(lay.now);
    label(now, "Now");
    draw_slide_picture(dl, st, ss, fonts, theme, dpi, ss.index, step, ImVec2(now[0], now[1]), now[2], ss.audience_w, ss.audience_h);
    frame(now);
    // Next: what the next key shows (the next step, or the next slide from its start).
    const auto next = box(lay.next);
    label(next, "Next");
    if (step < steps) draw_slide_picture(dl, st, ss, fonts, theme, dpi, ss.index, step + 1, ImVec2(next[0], next[1]), next[2], ss.audience_w, ss.audience_h);
    else if (ss.index + 1 < n) draw_slide_picture(dl, st, ss, fonts, theme, dpi, ss.index + 1, 0, ImVec2(next[0], next[1]), next[2], ss.audience_w, ss.audience_h);
    else {
        dl->AddRectFilled(ImVec2(next[0], next[1]), ImVec2(next[0] + next[2], next[1] + next[3]), theme.bg);
        text(16, ImVec2(next[0] + next[2] / 2, next[1] + next[3] / 2), theme.weak, "End of the slides", 0.5f, 0.5f);
    }
    frame(next);
    // The speaker notes, wrapped to their box.
    const auto nb = box(lay.notes);
    label(nb, "Notes");
    const std::string& notes = ss.index < ss.notes.size() ? ss.notes[ss.index] : std::string();
    const float em = 20.0f * ui_dpi;
    const float size = fonts.px(fonts.regular, em), adv = fonts.advance_scale(fonts.regular, em);
    const float line_h = std::round(em * 1.3f);
    dl->PushClipRect(ImVec2(nb[0], nb[1]), ImVec2(nb[0] + nb[2], nb[1] + nb[3]), true);
    const auto rows = wrap_rows(notes.empty() ? "(no notes)" : notes, fonts, size, adv, nb[2]);
    for (size_t r = 0; r < rows.size(); r++)
        draw_text(dl, fonts.regular, size, adv, ImVec2(nb[0], std::round(nb[1] + line_h * float(r))), notes.empty() ? theme.weak : theme.ui.text, rows[r]);
    dl->PopClipRect();
}

// The overview grid (spec 26) in the `w` x `h` box at `pos`: every slide as a small picture, the picked
// one outlined. Each slide is drawn as draw_slide draws it, in a canvas the audience window's size at its
// own zoom; then those vertices (and clip rects) are shrunk into its cell. Slides are drawn at `dpi`, the
// rest at `ui_dpi` (the window's own). `click`: where the mouse was clicked (if it was). Returns the
// slide clicked, or -1.
static int draw_overview_grid(ImDrawList* dl, DocState& st, SlideState& ss, const Fonts& fonts, const Theme& theme, float dpi, float ui_dpi,
                              ImVec2 pos, float w, float h, std::optional<ImVec2> click) {
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), theme.bg);
    int picked = -1;
    const size_t n = std::max<size_t>(ss.ranges.size(), 1);
    const float gap = 24.0f * ui_dpi, label = 22.0f * ui_dpi;
    const slides::Grid g =
        slides::overview_grid_min(n, w, h, ss.audience_w / std::max(ss.audience_h, 1.0f), gap, label, slides::kOverviewMinCell * ui_dpi);
    ss.overview_cols = g.cols;
    ss.overview_sel = std::min(ss.overview_sel, n - 1);
    const size_t rows = (n + g.cols - 1) / g.cols;
    const float grid_w = float(g.cols) * g.cell_w + float(g.cols - 1) * gap;
    const float grid_h = float(rows) * (g.cell_h + label) + float(rows - 1) * gap;
    // A grid taller than the view scrolls (the wheel, 40 px a notch; a moved pick is scrolled into view).
    const float max_scroll = std::max(slides::overview_height(n, g.cols, g.cell_h, gap, label) - h, 0.0f);
    ImVec2 origin(pos.x + (w - grid_w) / 2, pos.y + (h - grid_h) / 2);
    if (max_scroll > 0) {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsWindowHovered() && !io.KeyCtrl) ss.overview_scroll -= io.MouseWheel * 40.0f * ui_dpi;
        if (ss.overview_followed != ss.overview_sel) {
            const float top = gap + float(ss.overview_sel / g.cols) * (g.cell_h + label + gap);
            ss.overview_scroll = slides::overview_follow(ss.overview_scroll, top, top + g.cell_h + label, h, gap);
            ss.overview_followed = ss.overview_sel;
        }
        ss.overview_scroll = std::clamp(ss.overview_scroll, 0.0f, max_scroll);
        origin.y = pos.y + gap - ss.overview_scroll;
    } else {
        ss.overview_scroll = 0;
    }
    for (size_t k = 0; k < ss.ranges.size(); k++) {
        const ImVec2 a(origin.x + float(k % g.cols) * (g.cell_w + gap), origin.y + float(k / g.cols) * (g.cell_h + label + gap));
        const ImVec2 b(a.x + g.cell_w, a.y + g.cell_h);
        if (b.y + label < pos.y || a.y > pos.y + h) continue;  // scrolled out of sight
        draw_slide_picture(dl, st, ss, fonts, theme, dpi, k, SIZE_MAX, a, g.cell_w, ss.audience_w, ss.audience_h);
        const bool sel = k == ss.overview_sel;
        const float lw = (sel ? 3.0f : 1.0f) * ui_dpi;
        dl->AddRect(ImVec2(a.x - lw / 2, a.y - lw / 2), ImVec2(b.x + lw / 2, b.y + lw / 2), sel ? theme.ui.accent : theme.ui.border, 0, 0, lw);
        const std::string num = std::to_string(k + 1);
        const float em = 13.0f * ui_dpi;
        const float size = fonts.px(fonts.regular, em), adv = fonts.advance_scale(fonts.regular, em);
        draw_text(dl, fonts.regular, size, adv, ImVec2(std::round((a.x + b.x) / 2 - text_width(fonts.regular, size, adv, num) / 2), std::round(b.y + 5.0f * ui_dpi)),
                  theme.weak, num);
        if (click && click->x >= a.x && click->x < b.x && click->y >= a.y && click->y < b.y) picked = int(k);
    }
    return picked;
}

// The overview in the main window (when the presenter window isn't open: then it is there).
static int draw_overview(DocState& st, SlideState& ss, const Fonts& fonts, const Theme& theme, float dpi) {
    ImGui::BeginChild("overview", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    st.view_w = ImGui::GetWindowWidth();
    st.view_h = ImGui::GetWindowHeight();
    ss.audience_w = st.view_w, ss.audience_h = st.view_h;  // (this is the audience's window)
    std::optional<ImVec2> click;
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) click = ImGui::GetIO().MousePos;
    const int picked = draw_overview_grid(ImGui::GetWindowDrawList(), st, ss, fonts, theme, dpi, dpi, ImGui::GetCursorScreenPos(), st.view_w, st.view_h, click);
    ImGui::EndChild();
    return picked;
}

// Draw the current slide (fonts are at scale dpi * z). Returns the outline like draw_document.
static std::vector<OutlineEntry> draw_slide(DocState& st, SlideState& ss, FindState& find, nav::Autoscroll& autoscroll,
                                            const Fonts& fonts, const Theme& theme, bool line_numbers, bool block_press,
                                            bool& animate, float dpi) {
    const float scale = fonts.scale;
    const float col_w = 700.0f * scale;
    update_find(st, find, fonts, theme, col_w);
    update_editor_search(st, find, fonts, theme, col_w);
    if (find.scroll_to_current && find.current >= 0) {  // jump to the slide of the current match
        if (!ss.ranges.empty()) {
            const size_t block = find.matches[size_t(find.current)].first.block;
            const size_t idx = slides::slide_of_block(ss.ranges, block);
            if (idx != ss.index) ss.index = idx, ss.step = 0, ss.scroll = 0;
            if (idx < ss.steps.size()) ss.step = std::max(ss.step, slides::step_of_block(ss.steps[idx], block));  // reveal it
        }
        find.scroll_to_current = false;
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.bg);
    ImGui::BeginChild("slide", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    st.view_w = ImGui::GetWindowWidth();
    st.view_h = ImGui::GetWindowHeight();
    ss.audience_w = st.view_w, ss.audience_h = st.view_h;
    const bool hovered = ImGui::IsWindowHovered();

    const size_t key = layout_key(st, col_w, fonts);
    if (key != st.sel_key) {
        st.sel.clear();
        st.sel_key = key;
    }
    std::vector<OutlineEntry> outline;
    if (!ss.ranges.empty()) {
        const slides::Range r = ss.ranges[ss.index];
        // Every block must be measured at this zoom, or the others would be drawn too.
        // (Hidden steps are skipped by index, which needs their heights too.)
        if (st.cache.key != key || st.cache.height.size() != st.doc.size() ||
            std::any_of(st.cache.height.begin(), st.cache.height.end(), [](float v) { return std::isnan(v); }))
            layout_all(st, fonts, theme, col_w);
        const auto [top, bottom] = slide_extent(st, r);
        const float hs = bottom - top;
        float y0 = (st.view_h - hs) / 2;
        if (hs > st.view_h) {  // doesn't fit even at the smallest zoom: top-aligned, the wheel scrolls
            ImGuiIO& io = ImGui::GetIO();
            if (hovered && io.MouseWheel != 0 && !io.KeyCtrl && !io.KeyShift) ss.scroll -= io.MouseWheel * kWheelNotchPoints * scale;
            ss.scroll = std::clamp(ss.scroll, 0.0f, hs - st.view_h);
            y0 = -ss.scroll;
        } else {
            ss.scroll = 0;
        }
        const ImVec2 origin(std::round(pos.x + (st.view_w - col_w) / 2), std::round(pos.y + y0 - top));
        if (ss.index < ss.bgs.size())
            draw_backgrounds(ImGui::GetWindowDrawList(), ss.bgs[ss.index], base_dir(st), pos, ImVec2(pos.x + st.view_w, pos.y + st.view_h),
                            pos.x + (st.view_w - col_w) / 2, scale);
        const float view_top = pos.y + y0 + 0.5f;
        const float view_bottom = pos.y + y0 + hs - 0.5f;
        RenderOptions opts;
        opts.columns = columns_of(st);
        opts.styles = styles_of(st);
        opts.backgrounds = backgrounds_of(st);
        // A pause: the blocks from the next step on keep their space but are not drawn (by index,
        // not by y: in a column layout a later block can sit higher than an earlier one).
        if (ss.index < ss.steps.size() && ss.step < ss.steps[ss.index].size())
            std::tie(opts.hide_from, opts.hide_items) = ss.steps[ss.index][ss.step];
        opts.line_numbers = line_numbers;
        opts.interactive = !autoscroll.active();
        opts.mouse_in_view = hovered;
        opts.margin = 0;  // only this slide's blocks
        opts.find = find.open ? &find.matches : find.ed_matches.empty() ? nullptr : &find.ed_matches;
        opts.find_current = find.open ? find.current : -1;
        RenderResult res = render_document(ImGui::GetWindowDrawList(), st.doc, fonts, theme, origin, col_w, st.cache, st.version,
                                           view_top, view_bottom, base_dir(st), st.runs, st.sel.range(),
                                           ImGui::GetColorU32(ImGuiCol_TextSelectedBg), opts);
        handle_doc_input(st, res, view_top, view_bottom, block_press || autoscroll.active());
        draw_toast(st, fonts, theme, ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));
        if (st.goto_click) {  // Ctrl+click in an editor preview (spec 23)
            const ImVec2 at = *st.goto_click;
            st.goto_click.reset();
            if (st.live && st.live_open)
                if (const int line = sync_line_at(st, at.x - origin.x, at.y - origin.y); line > 0) {
                    platform::allow_editor_focus();
                    editor_emit(editor::goto_message(st.live_id, line));
                }
        }
        if (res.animating) animate = true;
        outline = std::move(res.outline);
    }
    // The counter, bottom right: 14 px scaled by the window (not the zoom), 16 px from the edges.
    {
        const std::string text = std::to_string(ss.index + 1) + " / " + std::to_string(std::max<size_t>(ss.ranges.size(), 1));
        const float em = 14.0f * dpi;
        const float size = fonts.px(fonts.regular, em), adv = fonts.advance_scale(fonts.regular, em);
        const float w = text_width(fonts.regular, size, adv, text);
        draw_text(ImGui::GetWindowDrawList(), fonts.regular, size, adv,
                  ImVec2(std::round(pos.x + st.view_w - 16.0f * dpi - w), std::round(pos.y + st.view_h - 16.0f * dpi - size)), theme.weak, text);
        // The progress bar: 3 px along the bottom edge, as far as this slide is through the deck.
        const float done = float(ss.index + 1) / float(std::max<size_t>(ss.ranges.size(), 1));
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x, std::round(pos.y + st.view_h - 3.0f * dpi)),
                                                  ImVec2(std::round(pos.x + st.view_w * done), pos.y + st.view_h), theme.ui.accent);
        // The presenter clock (`t`): time since slides mode started, bottom left.
        if (ss.clock_shown) {
            const long secs = long(ImGui::GetTime() - ss.since);
            char buf[32];
            if (secs >= 3600) std::snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", secs / 3600, secs / 60 % 60, secs % 60);
            else std::snprintf(buf, sizeof buf, "%ld:%02ld", secs / 60, secs % 60);
            draw_text(ImGui::GetWindowDrawList(), fonts.regular, size, adv,
                      ImVec2(std::round(pos.x + 16.0f * dpi), std::round(pos.y + st.view_h - 16.0f * dpi - size)), theme.weak, buf);
            // (the idle loop wakes at least every 0.5 s, so the seconds keep ticking)
        }
        // Speaker notes (`N`): on a panel along the bottom, above the counter.
        const std::string& notes = ss.index < ss.notes.size() ? ss.notes[ss.index] : std::string();
        if (ss.notes_shown && !notes.empty()) {
            const float nem = 15.0f * dpi, pad = 12.0f * dpi;
            const float nsize = fonts.px(fonts.regular, nem), nadv = fonts.advance_scale(fonts.regular, nem);
            const float line_h = std::round(nem * 1.3f);
            const std::vector<std::string> rows = wrap_rows(notes, fonts, nsize, nadv, st.view_w - 4.0f * pad);  // to the panel's width
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float bottom = std::round(pos.y + st.view_h - 40.0f * dpi);
            const ImVec2 a(std::round(pos.x + pad), std::round(bottom - line_h * float(rows.size()) - 2.0f * pad));
            const ImVec2 b(std::round(pos.x + st.view_w - pad), bottom);
            dl->AddRectFilled(a, b, theme.ui.panel, 6.0f * dpi);
            dl->AddRect(a, b, theme.ui.border, 6.0f * dpi, 0, 1.0f * dpi);
            for (size_t r = 0; r < rows.size(); r++)
                draw_text(dl, fonts.regular, nsize, nadv, ImVec2(a.x + pad, std::round(a.y + pad + line_h * float(r))), theme.ui.text, rows[r]);
        }
        // Tell the editor the slide on screen and its notes, once per slide (editor documents only).
        // (Not before the notes are known; again when the slide's notes change.)
        if (st.live && st.live_open && ss.notes.size() == ss.ranges.size()) {
            const std::string sent = st.live_id + "/" + std::to_string(ss.index) + "/" + notes;
            if (ss.notes_sent != sent) {
                const auto lines = slides::slide_lines(ss.ranges, st.doc, ss.index, st.line_count);
                editor_emit(editor::notes_message(st.live_id, ss.index + 1, std::max<size_t>(ss.ranges.size(), 1), notes, lines));
                ss.notes_sent = sent;
            }
        }
    }
    ImGui::EndChild();
    return outline;
}

// The autoscroll anchor (spec 3): a 26 px circle with up/down arrowheads.
static void draw_autoscroll_marker(const nav::Autoscroll& a, const Theme& t, float scale) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 c(std::round(a.anchor_x()), std::round(a.anchor_y()));
    const float r = 13.0f * scale;
    ImU32 fill = (t.bg & ~IM_COL32_A_MASK) | (ImU32(0.85f * 255) << IM_COL32_A_SHIFT);
    dl->AddCircleFilled(c, r, fill, 32);
    dl->AddCircle(c, r, t.weak, 32, 1.0f);
    const float s = 3.5f * scale, gap = 4.0f * scale;
    dl->AddTriangleFilled(ImVec2(c.x, c.y - gap - s), ImVec2(c.x - s, c.y - gap), ImVec2(c.x + s, c.y - gap), t.weak);
    dl->AddTriangleFilled(ImVec2(c.x, c.y + gap + s), ImVec2(c.x + s, c.y + gap), ImVec2(c.x - s, c.y + gap), t.weak);
    dl->AddCircleFilled(c, 1.5f * scale, t.weak);
}

// ---------------------------------------------------------------------------
// Input bookkeeping for the frame loop
// ---------------------------------------------------------------------------

// Set by GLFW callbacks whenever input arrives; the loop then renders a few
// extra frames so ImGui can settle (it often needs a frame or two after input).
static bool g_input = false;

// The presenter window (spec 26, `s`): a second native window with its own ImGui context (sharing the
// main one's font atlas) and GL context (sharing its textures). Its keys go to the main window's ImGui
// context, so they drive the slides exactly as they do there.
static GLFWwindow* g_main_window = nullptr;
struct PresenterWindow {
    GLFWwindow* win = nullptr;
    ImGuiContext* ctx = nullptr;
};
static std::string g_dropped;  // UTF-8 path of a file dropped on the window

static void install_input_callbacks(GLFWwindow* w) {
    // Installed BEFORE ImGui's backend, which chains to these.
    glfwSetCursorPosCallback(w, [](GLFWwindow*, double, double) { g_input = true; });
    glfwSetMouseButtonCallback(w, [](GLFWwindow*, int, int, int) { g_input = true; });
    glfwSetScrollCallback(w, [](GLFWwindow*, double, double) { g_input = true; });
    glfwSetKeyCallback(w, [](GLFWwindow*, int, int, int, int) { g_input = true; });
    glfwSetCharCallback(w, [](GLFWwindow*, unsigned) { g_input = true; });
    glfwSetWindowSizeCallback(w, [](GLFWwindow*, int, int) { g_input = true; });
    glfwSetWindowFocusCallback(w, [](GLFWwindow*, int) { g_input = true; });
    glfwSetDropCallback(w, [](GLFWwindow*, int n, const char** paths) {
        if (n > 0) g_dropped = paths[0];
        g_input = true;
    });
}

// This frame's key presses, in the form the vim key machine wants.
static std::vector<nav::KeyInput> collect_keys() {
    using nav::KeyInput;
    using nav::Named;
    ImGuiIO& io = ImGui::GetIO();
    std::vector<KeyInput> keys;
    const std::pair<ImGuiKey, Named> named[] = {
        {ImGuiKey_DownArrow, Named::Down}, {ImGuiKey_UpArrow, Named::Up},   {ImGuiKey_PageDown, Named::PageDown},
        {ImGuiKey_PageUp, Named::PageUp},  {ImGuiKey_Space, Named::Space},  {ImGuiKey_Home, Named::Home},
        {ImGuiKey_End, Named::End},        {ImGuiKey_Escape, Named::Escape},
    };
    if (!io.KeyCtrl && !io.KeyAlt)
        for (auto [key, n] : named)
            if (ImGui::IsKeyPressed(key)) keys.push_back(KeyInput{.named = n, .shift = io.KeyShift});
    // Ctrl chords the machine knows (Ctrl+F, Ctrl+B, Ctrl+D, Ctrl+U, Ctrl+E, Ctrl+Y).
    for (auto [key, c] : {std::pair{ImGuiKey_F, U'f'}, {ImGuiKey_B, U'b'}, {ImGuiKey_D, U'd'}, {ImGuiKey_U, U'u'},
                                         {ImGuiKey_E, U'e'}, {ImGuiKey_Y, U'y'}})
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | key)) keys.push_back(KeyInput{.ch = c, .ctrl = true});
    // Typed characters (layout-aware: 'G', '[', '/'); space comes as a named key.
    if (!io.KeyCtrl && !io.KeyAlt)
        for (ImWchar c : io.InputQueueCharacters)
            if (c != ' ') keys.push_back(KeyInput{.ch = char32_t(c)});
    return keys;
}

// `--register` / `--unregister`: "Open with" entries in HKCU, no window.
static int run_registration(bool add) {
    const fs::path exe = platform::exe_path();
    const std::string err = add ? session::register_app(L"Software\\Classes", exe, L"Calligrus")
                                : session::unregister_app(L"Software\\Classes", exe);
    const std::string exe_u8 = to_utf8(exe);
    if (err.empty()) platform::print_cli((add ? "Registered " : "Unregistered ") + exe_u8 + " for .md/.markdown/.org");
    else platform::print_cli("Error: " + err);
    return err.empty() ? 0 : 1;
}

// Frame pacing (spec 24), like the Rust port pacing.rs: with working vsync every swap
// waits for the screen and this never sleeps. Where vsync is ignored (software GL
// over RDP, Xvfb) a busy app drew 500+ frames a second, eating ~10 cores; so after
// a run of frames under 4 ms apart, keep to 60 frames a second from then on.
struct FramePacer {
    using clock = std::chrono::steady_clock;
    clock::time_point last{};
    int fast_run = 0;
    bool no_vsync = false;  // sticky once seen
    void pace() {
        const auto now = clock::now();
        if (last != clock::time_point{}) {
            const auto gap = now - last;
            if (!no_vsync) {
                fast_run = gap < std::chrono::milliseconds(4) ? fast_run + 1 : 0;
                no_vsync = fast_run >= 10;
            }
            constexpr auto target = std::chrono::microseconds(16667);
            if (no_vsync && gap < target) std::this_thread::sleep_for(target - gap);
        }
        last = clock::now();
    }
};

// `calligrus --help`: the command line in one screen.
static const char* kHelp =
    "Calligrus " CALLIGRUS_VERSION " - a native Markdown and Org viewer\n"
    "\n"
    "Usage:\n"
    "  calligrus [FILE...]                 open files (several open in tabs)\n"
    "  calligrus --slides FILE             start in slides mode\n"
    "  calligrus --export OUT.pdf FILE [OPTIONS]\n"
    "                                      write a PDF without opening a window\n"
    "                                      (the slides, or pages with --pages)\n"
    "  calligrus --editor                  live preview driven by an editor over stdin\n"
    "                                      (used by the Emacs and Neovim plugins)\n"
    "  calligrus --register, --unregister  add / remove \"Open with\" for .md and .org (Windows)\n"
    "  calligrus --help, --version\n"
    "\n"
    "Export options:\n"
    "  --pages               pages instead of slides\n"
    "  --paper a4|letter     paper size for --pages (default a4)\n"
    "  --theme NAME          theme (default: the config's light theme)\n"
    "  --body-size PT        body text size in points for --pages, 8 to 64\n"
    "  --base-dir DIR        resolve relative pictures and links from DIR\n"
    "  --raster              pictures only, no selectable text\n"
    "  --size WxH            slide size in pixels (default 1280x720)\n"
    "  --scale S             pixels per point / per pixel (default 2 for pages, 1.5 for slides)\n"
    "\n"
    "Settings live in config.toml in ~/.config/calligrus (or %APPDATA%\\calligrus on Windows\n"
    "without a ~/.config folder); CALLIGRUS_CONFIG points to another file. The first screen\n"
    "of the app lists the keys. More: https://github.com/theKAOSSphere/calligrus";

#ifdef __ANDROID__
int calligrus_main() {  // (called by android_main)
#else
int main() {
#endif
    if (auto a = platform::args(); !a.empty() && (a[0] == "--help" || a[0] == "-h" || a[0] == "--version")) {
        script_log(a[0] == "--version" ? std::string("calligrus ") + CALLIGRUS_VERSION : std::string(kHelp));
        return 0;
    }
    if (auto a = platform::args(); !a.empty() && (a[0] == "--register" || a[0] == "--unregister"))
        return run_registration(a[0] == "--register");

    // First start of this version: copy the files of the old (mdview) versions into ~/.config/calligrus.
    // Not with CALLIGRUS_CONFIG set (a test run or a deliberate override must not touch the real folders).
    if (userdir::current_env().calligrus_config.empty()) userdir::migrate_legacy(userdir::current_env());

    // Command line: [file.md] [--script FILE [--out DIR]]  (env: CALLIGRUS_SCRIPT, CALLIGRUS_SCRIPT_OUT)
    std::vector<fs::path> cli_files;  // every file argument: one tab each (spec 34)
    fs::path script_file, script_out;
    bool start_slides = false;  // --slides (spec 26)
    {
        const auto a = platform::args();
        for (size_t i = 0; i < a.size(); i++) {
            if (a[i] == "--script" && i + 1 < a.size()) script_file = a[++i];
            else if (a[i] == "--out" && i + 1 < a.size()) script_out = a[++i];
            else if (a[i] == "--editor") g_editor = true;
            else if (a[i] == "--slides") start_slides = true;
            else cli_files.push_back(a[i]);
        }
        // `--export OUT FILE [options]` (below) takes its own arguments: none of them is a file to open.
        if (std::find(a.begin(), a.end(), "--export") != a.end()) cli_files.clear();
        if (script_file.empty())
#pragma warning(suppress : 4996)
            if (const char* e = std::getenv("CALLIGRUS_SCRIPT")) script_file = fs::path(std::u8string(e, e + std::strlen(e)));
        if (script_out.empty())
#pragma warning(suppress : 4996)
            if (const char* e = std::getenv("CALLIGRUS_SCRIPT_OUT")) script_out = fs::path(std::u8string(e, e + std::strlen(e)));
    }
    // Slides export (spec 26): `--export OUT FILE [--size WxH] [--scale S]`, run as a script that shows
    // each slide (`slide N`) and shoots it; a PDF keeps the shots in memory and is written at the end.
    std::optional<script::Export> exporting;
    std::vector<pdf::Page> export_pages;
    size_t export_total = 0;  // slides to export
    {
        std::vector<std::string> args;
        for (const fs::path& a : platform::args()) args.push_back(to_utf8(a));
        std::string err;
        exporting = script::export_args(args, err);
        if (!err.empty()) return script_log("export: " + err, true), 2;
    }
    const bool export_pdf = exporting && fs::path(exporting->out).extension() == ".pdf";
    // A PDF export has real text (spec 33) unless `--raster`: the text is captured, the picture drawn without it.
    if (export_pdf && !exporting->raster) text_capture::enable(true);
    if (exporting && !exporting->base_dir.empty()) g_export_base_dir = fs::absolute(fs::path(std::u8string(exporting->base_dir.begin(), exporting->base_dir.end())));
    g_script_mode = !script_file.empty() || exporting;
    if (exporting) render_set_export(true);  // (no scrollbars, wrapped code)
    script::Parsed script_parsed;
    fs::path script_dir;
    if (exporting) {
        const fs::path input = fs::absolute(fs::path(std::u8string(exporting->input.begin(), exporting->input.end())));
        auto text = platform::read_file(input);
        if (!text) return script_log("export: cannot open " + to_utf8(input) + ": no such file", true), 1;
        if (exporting->pages) {
            // Pages (spec 29): open the document, switch the page view on, then one `page-shot` per page
            // (how many is only known once the document is laid out: each one repeats itself until the last).
            script_parsed = script::parse("scale " + std::to_string(exporting->scale) + "\nopen " + to_utf8(input) + "\n");
            script::Command begin, shot;
            begin.kind = script::Kind::PagesBegin, begin.text = exporting->paper;
            shot.kind = script::Kind::PageShot;
            script_parsed.commands.push_back(begin);
            script_parsed.commands.push_back(shot);
        } else {
        // How many slides: split the document the way slides mode will.
        const bool org = md::is_org_path(to_utf8(input));
        const md::Document doc = md::parse_format(*text, org ? "org" : "markdown");
        const size_t total = export_total = std::max<size_t>(slides::split_with_ends(doc, slides::end_slide_lines(*text, org)).size(), 1);
        std::string s = "size " + std::to_string(exporting->width) + " " + std::to_string(exporting->height) + "\nscale " +
                        std::to_string(exporting->scale) + "\nopen " + to_utf8(input) + "\n";
        for (size_t n = 1; n <= total; n++) {
            char shot[32];
            std::snprintf(shot, sizeof shot, "slide-%03zu.png", n);
            s += "slide " + std::to_string(n) + "\nshot " + shot + "\n";
        }
        script_parsed = script::parse(s);
        }
        script_dir = input.parent_path();
        script_out = fs::absolute(fs::path(std::u8string(exporting->out.begin(), exporting->out.end())));
        if (export_pdf) {
            std::error_code mk;
            if (script_out.has_parent_path()) fs::create_directories(script_out.parent_path(), mk);
        }
        g_editor_out = fs::temp_directory_path() / "calligrus-export-editor-out.jsonl";  // (nothing is sent: no editor)
    } else if (g_script_mode) {
        script_file = fs::absolute(script_file);
        script_dir = script_file.parent_path();
        if (!script_out.empty()) script_out = fs::absolute(script_out);
        g_script_log = script_file;
        g_script_log += ".log";
        g_editor_out = (script_out.empty() ? script_dir : script_out) / "editor-out.jsonl";
        {
            std::error_code mk;
            fs::create_directories(g_editor_out.parent_path(), mk);
            std::ofstream fresh(g_editor_out, std::ios::binary | std::ios::trunc);  // a fresh capture per run
        }
        std::error_code rm;
        fs::remove(g_script_log, rm);  // a fresh log per run
        auto text = platform::read_file(script_file);
        if (!text) {
            script_log("cannot read script " + to_utf8(script_file), true);
            return 1;
        }
        script_parsed = script::parse(*text);
        if (!script_parsed.error.empty()) {
            script_log(script_parsed.error, true);
            return 1;
        }
    }
    glfwSetErrorCallback([](int code, const char* msg) { std::fprintf(stderr, "GLFW %d: %s\n", code, msg); });
    if (!glfwInit()) return 1;

    // OpenGL 3.3 core profile; ImGui's GL3 backend draws everything on the GPU.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    // Start hidden: shown once the theme (incl. dark title bar) has been applied.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(1100, 800, "Calligrus", nullptr, nullptr);
    if (!window) return 1;
    // Window / taskbar icon: the 256 px PNG embedded in the exe (the exe's own icon is src/app.rc).
    if (auto png = resource("icon-256.png")) {
        int iw = 0, ih = 0, n = 0;
        unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(png->data()), int(png->size()), &iw, &ih, &n, 4);
        if (px) {
            GLFWimage img{iw, ih, px};
            glfwSetWindowIcon(window, 1, &img);
            stbi_image_free(px);
        }
    }
    glfwMakeContextCurrent(window);
    g_main_window = window;
    glfwSwapInterval(g_script_mode ? 0 : 1);  // vsync (script mode: as fast as possible)
    install_input_callbacks(window);

    float dpi = 1.0f;  // monitor DPI scale (1.5 on a 144-dpi screen)
    glfwGetWindowContentScale(window, &dpi, nullptr);
    if (g_script_mode) dpi = 1.0f;  // output must not depend on the monitor: the script's `scale` says otherwise

    // The installed-font index (spec 8a), for family names in [fonts]: scanned
    // on a background thread while the rest starts up. When it's done, wake the
    // event loop so a named font waiting for it is applied right away.
    // (g_glfw_alive: don't post to GLFW after it was shut down.)
    static std::atomic<bool> g_glfw_alive{true};
    g_glfw_alive = true;  // (Android: the process outlives the activity, and main runs again)
    FontIndex font_index;
    trace::log("start-up: font index starting");
    font_index.start([] {
        trace::log("font index ready");
        if (g_glfw_alive) glfwPostEmptyEvent();
    });

    // Editor mode: a thread reads protocol lines from stdin (a pipe from the editor; this is a
    // GUI-subsystem exe, so no C stdio) into a queue and wakes the loop; EOF = the editor is gone.
    struct EditorInput {
        std::mutex mutex;
        std::deque<std::string> lines;
        bool eof = false;
    };
    static EditorInput editor_in;
    if (g_editor && !g_script_mode) {
        std::thread([] {
#ifdef _WIN32
            HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
#endif
            // Bytes read from stdin; 0 = end of input (or an error).
            auto read_some = [&](char* buf, size_t cap) -> size_t {
#ifdef _WIN32
                DWORD got = 0;
                const bool good = in != INVALID_HANDLE_VALUE && in != nullptr && ReadFile(in, buf, DWORD(cap), &got, nullptr);
                return good ? got : 0;
#else
                for (;;) {
                    const ssize_t got = read(0, buf, cap);
                    if (got < 0 && errno == EINTR) continue;
                    return got > 0 ? size_t(got) : 0;
                }
#endif
            };
            std::string pending;
            char chunk[65536];
            for (;;) {
                const size_t n = read_some(chunk, sizeof chunk);
                const bool ok = n > 0;
                if (ok) pending.append(chunk, n);
                else if (!pending.empty()) pending += '\n';  // a last line without a newline
                {
                    std::lock_guard<std::mutex> lock(editor_in.mutex);
                    for (size_t nl; (nl = pending.find('\n')) != std::string::npos; pending.erase(0, nl + 1)) {
                        std::string line = pending.substr(0, nl);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (!line.empty()) editor_in.lines.push_back(std::move(line));
                    }
                    if (!ok) editor_in.eof = true;
                }
                if (g_glfw_alive) glfwPostEmptyEvent();
                if (!ok) break;
            }
        }).detach();
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;                   // don't litter imgui.ini next to the exe
    io.ConfigInputTextEnterKeepActive = true;   // the find field keeps focus after Enter
    // Script mode: a scripted key is one deliberate press. On a slow machine (software OpenGL in CI) one
    // frame can outlast ImGui's key-repeat delay, which turned a single press into two; so no auto-repeat.
    if (g_script_mode) io.KeyRepeatDelay = 1.0e9f;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(kGlslVersion);

    highlight_warm_up();  // parse the syntax grammars on a background thread
    const std::vector<std::string> entry_names = highlight_entry_names();

    DocState st;
    // The config file (missing = defaults) and the themes (built-in + user).
    ConfigFile conf;
    conf.readonly = g_script_mode;
    for (const std::string& w : conf.load()) st.error = w;
    // The state file (window geometry, scroll positions): separate from config.toml. Never blocks start-up.
    const fs::path state_file = session::state_path();
    session::State state = session::load_state(state_file);
    session::WindowRect normal_rect{0, 0, 1100, 800, false};  // the last non-maximized, non-fullscreen outer rect
    int normal_frame_l = 0, normal_frame_t = 0;               // its title-bar / border sizes (outer -> client)
    bool want_maximized = false;
    bool tracked = false;
    if (conf.cfg.remember_window && state.window && !g_script_mode) {
        std::vector<session::Area> areas;
        int nmon = 0;
        GLFWmonitor** mons = glfwGetMonitors(&nmon);
        for (int i = 0; i < nmon; i++) {
            session::Area a{};
            glfwGetMonitorWorkarea(mons[i], &a.x, &a.y, &a.w, &a.h);
            areas.push_back(a);
        }
        if (session::title_bar_visible(*state.window, areas)) {
            int l = 0, t = 0, r = 0, b = 0;
            glfwGetWindowFrameSize(window, &l, &t, &r, &b);
            glfwSetWindowSize(window, state.window->width, state.window->height);
            glfwSetWindowPos(window, state.window->x + l, state.window->y + t);
            want_maximized = state.window->maximized;
            glfwGetWindowContentScale(window, &dpi, nullptr);  // the saved monitor may have another DPI
        }
    }
    ThemeSet themes;
    // User themes: `themes/` next to the config file in use (or where it would be created).
    fs::path themes_dir = conf.path.parent_path() / "themes";
    themes.load(themes_dir);
    themes.bind_entries(entry_names);
    std::error_code ec;
    fs::file_time_type themes_mtime = fs::last_write_time(themes_dir, ec);

    bool system_dark = platform::system_prefers_dark();
    // [fonts] values (paths or family names) -> the faces to load. Resolved
    // again only when the values change or the font index becomes ready.
    struct FontChoice {
        Config::Fonts values;   // what was resolved
        bool with_index = false;
        bool valid = false;
        ResolvedFonts result;
    } font_choice;
    auto resolve_font_choice = [&](const Config& c) -> bool {  // true if re-resolved
        const bool with_index = font_index.ready();
        if (font_choice.valid && font_choice.values == c.fonts && font_choice.with_index == with_index) return false;
        auto expand = [](const std::string& s) { return s.empty() ? s : to_utf8(expand_path(s)); };
        font_choice = {c.fonts, with_index, true, resolve_fonts(c.fonts, with_index ? &font_index.faces() : nullptr, expand)};
        return true;
    };
    // Start-up: if a family name is configured, give the index a moment to
    // finish (the window is still hidden), so the first frame already uses the
    // named font. A slow scan doesn't hold start-up: after the wait the
    // built-in faces are used, and the named font is applied once the index is
    // ready (the loop re-resolves then).
    {
        const Config::Fonts& v = conf.cfg.fonts;
        for (const std::string* s : {&v.prose, &v.prose_bold, &v.prose_italic, &v.prose_bold_italic, &v.mono})
            if (is_font_family_name(*s)) {
                font_index.wait(kFontIndexStartupWait);
                break;
            }
    }

    Fonts fonts;            // loaded in the loop, before the first frame (the coverage curve depends on the theme)
    bool fonts_dark = false;
    FontFiles fonts_loaded;
    bool fonts_ready = false;
    // System fonts for missing characters (spec 30): the faces chosen so far this session (the first
    // 8), the document check, and the background choice of fonts for the characters found.
    struct SysJob {
        std::atomic<bool> done{false};
        std::vector<FontSource> result;
    };
    std::vector<FontSource> sys_fonts;
    std::shared_ptr<SysJob> sys_job;
    std::vector<char32_t> sys_missing;  // characters no font has, waiting for the font index
    bool sys_check = true;              // look for such characters (the document or the fonts changed)
    // The characters a font search has been started for (a later search for them would find nothing new), and
    // whether the search under way is for new ones. While one is pending the document is shown at once, with the
    // characters nothing has a glyph for as blank space (`sys_blank`, never boxes); the frame after the fonts
    // arrive has the text.
    std::set<char32_t> sys_tried;
    bool sys_job_fresh = false;
    bool sys_blank = false;
    double frame_ms = 0;       // trace: the start of this frame
    double fonts_applied = -1; // trace: when the atlas was last rebuilt for new fonts (the frame after is timed)
    double fonts_load_ms = 0;
    bool first_frame_logged = false;
    auto sys_busy = [&](const Config& c) { return c.system_fallback && (sys_check || !sys_missing.empty() || sys_job); };
    fonts.scale = dpi * float(conf.cfg.zoom);
    fonts.body = float(conf.cfg.body_size) * fonts.scale;

    if (!cli_files.empty() && st.load(cli_files[0])) {  // (the others open in tabs once the frame loop's helpers exist)
        state.add_recent(cli_files[0]);
        if (auto off = state.lookup(cli_files[0])) st.restore_y = float(*off) * fonts.scale;
    }
    // The shared demo (embedded in the exe): what an empty window shows.
    auto show_demo = [&] {
        auto demo = resource("demo.md");
        st.set_doc(md::parse(demo ? *demo : std::string_view(kSample)));
    };
    const bool restore_tabs = cli_files.empty() && !g_editor && state.tabs.size() >= 2;  // (spec 34: reopen the saved tabs)
    if (g_editor) {
        st.set_doc(md::parse("*Waiting for the editor\xE2\x80\xA6*"));
    } else if (st.path.empty() && !restore_tabs) {
        show_demo();
    }

    UiState ui;
    ui.recent = &state.recent;
    ui.show_outline = conf.cfg.show_outline;
    FindState find;
#pragma warning(suppress : 4996)  // debugging aid: CALLIGRUS_FIND=<text> opens the find bar with that query
    if (const char* q = std::getenv("CALLIGRUS_FIND")) {
        find.open = true;
        std::snprintf(find.buf, sizeof(find.buf), "%s", q);
        find.typed = find.buf;
    }
    nav::VimKeys vim;
    nav::Autoscroll autoscroll;
    std::vector<OutlineEntry> outline;
    auto last_poll = Clock::now();
    int extra_frames = 2;
#ifdef __ANDROID__
    bool android_idle = false;      // as of the last frame: nothing is moving into place (see the power notes in spec 35)
    bool render_deadline = false;   // the wait was cut short by a deadline that needs a frame (not just the config write)
    double quiet_until = 0;         // after input, frames keep coming a little while (hover delays, tooltips)
    int shown_frames = 0;
#endif
    std::string applied_style;      // theme name + scale the ImGui style was made for
    bool shown = false;
    std::string title_key = "\x01";  // which document the window title shows
    float wheel_zoom = 0;           // Ctrl+wheel: fractional notches (touchpads) add up
    session::History history;
    bool fullscreen = false;
    bool fs_was_maximized = false;

    // Slides mode (spec 26): F5 / the Slides button / --slides. Entering starts at the slide that
    // holds the top of the current view; leaving scrolls to the current slide's first block.
    SlideState slides;
    PageExport pg;  // the document export's page view (spec 29)
    slides.want_on = start_slides;

    // --- tabs (spec 34) ---
    // `st`, `find`, `slides` and `history` are always the ACTIVE tab (lambdas hold them by reference).
    // The other tabs wait in `tabs`; `tabs[active_tab]` is an empty stand-in for the live one.
    // Switching swaps the live variables with a tab's, so nothing is ever copied.
    std::vector<std::unique_ptr<Tab>> tabs;
    tabs.push_back(std::make_unique<Tab>());
    size_t active_tab = 0;
    auto swap_live = [&](Tab& t) {
        std::swap(st, t.st);
        std::swap(find, t.find);
        std::swap(slides, t.slides);
        std::swap(history, t.history);
    };
    // The document of tab `i` (the live one, or a waiting one).
    auto tab_doc = [&](size_t i) -> const DocState& { return i == active_tab ? st : tabs[i]->st; };
    // Run `f` on every waiting tab (not the active one).
    auto for_waiting = [&](const std::function<void(Tab&)>& f) {
        for (size_t i = 0; i < tabs.size(); i++)
            if (i != active_tab) f(*tabs[i]);
    };
    // The tab showing file `p` (the same file, whatever the spelling), if any.
    auto find_tab = [&](const fs::path& p) -> std::optional<size_t> {
        std::error_code ec;
        for (size_t i = 0; i < tabs.size(); i++) {
            const fs::path& open = tab_doc(i).path;
            if (!open.empty() && (open == p || fs::equivalent(open, p, ec))) return i;
        }
        return std::nullopt;
    };
    // state.toml's `tabs` / `active_tab` (written with 2 or more tabs that have a file).
    auto sync_state_tabs = [&] {
        state.tabs.clear();
        state.active_tab = 1;
        for (size_t i = 0; i < tabs.size(); i++) {
            const fs::path& p = tab_doc(i).path;
            if (p.empty()) continue;
            if (i <= active_tab) state.active_tab = state.tabs.size() + 1;  // (the last listed tab not after the active one)
            state.tabs.push_back(to_utf8(fs::absolute(p)));
        }
    };
    PresenterWindow presenter;
    auto presenter_open = [&] {
        ImGuiContext* main_ctx = ImGui::GetCurrentContext();
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
        presenter.win = glfwCreateWindow(int(1000 * dpi), int(640 * dpi), "Calligrus presenter", nullptr, window);  // (shares the GL objects)
        if (!presenter.win) return;
        if (auto png = resource("icon-256.png")) {
            int iw = 0, ih = 0, n = 0;
            unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(png->data()), int(png->size()), &iw, &ih, &n, 4);
            if (px) {
                GLFWimage img{iw, ih, px};
                glfwSetWindowIcon(presenter.win, 1, &img);
                stbi_image_free(px);
            }
        }
        // Keys and typed characters go to the main window's ImGui (whose callbacks also wake the loop).
        glfwSetKeyCallback(presenter.win, [](GLFWwindow*, int key, int scancode, int action, int mods) {
            ImGui_ImplGlfw_KeyCallback(g_main_window, key, scancode, action, mods);
        });
        glfwSetCharCallback(presenter.win, [](GLFWwindow*, unsigned c) { ImGui_ImplGlfw_CharCallback(g_main_window, c); });
        // The mouse stays its own (for clicks in the overview).
        glfwSetMouseButtonCallback(presenter.win, [](GLFWwindow* w, int button, int action, int mods) {
            ImGui_ImplGlfw_MouseButtonCallback(w, button, action, mods);
            g_input = true;
        });
        glfwSetCursorPosCallback(presenter.win, [](GLFWwindow* w, double x, double y) { ImGui_ImplGlfw_CursorPosCallback(w, x, y); });
        glfwSetCursorEnterCallback(presenter.win, [](GLFWwindow* w, int entered) { ImGui_ImplGlfw_CursorEnterCallback(w, entered); });
        glfwSetWindowSizeCallback(presenter.win, [](GLFWwindow*, int, int) { g_input = true; });
        glfwSetWindowRefreshCallback(presenter.win, [](GLFWwindow*) { g_input = true; });
        presenter.ctx = ImGui::CreateContext(ImGui::GetIO().Fonts);
        ImGui::SetCurrentContext(presenter.ctx);
        ImGui::GetIO().IniFilename = nullptr;
        glfwMakeContextCurrent(presenter.win);
        glfwSwapInterval(0);  // (the main window waits for vsync already)
        ImGui_ImplGlfw_InitForOpenGL(presenter.win, false);
        ImGui_ImplOpenGL3_Init(kGlslVersion);
        glfwMakeContextCurrent(window);
        ImGui::SetCurrentContext(main_ctx);
    };
    auto presenter_close = [&] {
        if (!presenter.win) return;
        ImGuiContext* main_ctx = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(presenter.ctx);
        glfwMakeContextCurrent(presenter.win);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext(presenter.ctx);
        glfwMakeContextCurrent(window);
        ImGui::SetCurrentContext(main_ctx);
        glfwDestroyWindow(presenter.win);
        presenter = {};
    };
    // One frame of the presenter window (after the main window's, with the same document state).
    auto presenter_frame = [&](const Fonts& f, const Theme& theme, double elapsed) {
        ImGuiContext* main_ctx = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(presenter.ctx);
        glfwMakeContextCurrent(presenter.win);
        platform::set_dark_title_bar(native_window(presenter.win), theme.dark);
        float pdpi = 1.0f;
        glfwGetWindowContentScale(presenter.win, &pdpi, nullptr);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("presenter", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar(2);
        if (slides.overview) {  // the overview is here while this window is open
            std::optional<ImVec2> click;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) click = ImGui::GetIO().MousePos;
            if (const int k = draw_overview_grid(ImGui::GetWindowDrawList(), st, slides, f, theme, dpi, pdpi, vp->WorkPos, vp->WorkSize.x, vp->WorkSize.y, click);
                k >= 0) {
                slides.overview = false;
                slides.index = size_t(k), slides.step = SIZE_MAX, slides.scroll = 0;
                g_input = true;  // (the main window shows it next frame)
            }
        } else {
            draw_presenter(ImGui::GetWindowDrawList(), st, slides, f, theme, dpi, pdpi, vp->WorkPos, vp->WorkSize.x, vp->WorkSize.y, elapsed);
        }
        ImGui::End();
        ImGui::Render();
        int fw = 0, fh = 0;
        glfwGetFramebufferSize(presenter.win, &fw, &fh);
        glViewport(0, 0, fw, fh);
        const ImVec4 bg = ImGui::ColorConvertU32ToFloat4(theme.ui.panel);
        glClearColor(bg.x, bg.y, bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(presenter.win);
        glfwMakeContextCurrent(window);
        ImGui::SetCurrentContext(main_ctx);
    };
    auto set_slides = [&](bool on) {
        if (on == slides.on) return;
        slides.on = on;
        if (on) {
            slides.ranges = st.split();
            slides.version = SIZE_MAX;  // (slides_prepare re-splits and measures)
            slides.doc_id = slide_doc_id(st);
            slides.index = 0, slides.step = 0, slides.scroll = 0;
            slides.since = ImGui::GetTime();  // the clock starts with the show
            const auto& tops = st.cache.top;
            if (!slides.ranges.empty() && !tops.empty() && tops.size() == st.doc.size()) {
                const float doc_y = st.scroll_cur - top_pad(fonts.scale);
                size_t i = size_t(std::upper_bound(tops.begin(), tops.end(), doc_y) - tops.begin());
                slides.index = slides::slide_of_block(slides.ranges, i > 0 ? i - 1 : 0);
            }
            st.view_w = 0;  // (the window size is used until the slide view reports its own)
        } else {
            if (slides.index < slides.ranges.size()) st.zoom_anchor = {slides.ranges[slides.index].first, 0.0f};
            st.cache.key = 0;
            st.columns.clear();  // normal mode: one column
            st.styles.clear();
            st.backgrounds.clear();
            slides.notes_sent.clear();
            slides.overview = false;
            slides.presenter = false;
        }
        st.sel.clear();
        extra_frames = std::max(extra_frames, 3);
    };

    // --- navigation: files, links, history, the state file ---
    auto here = [&]() { return session::Location{st.path, st.scroll_target / fonts.scale}; };
    // (A position still waiting to be restored is the real one: the view has not moved there yet.)
    auto remember_scroll_of = [&](const DocState& d, const SlideState& sl) {
        if (!d.path.empty() && !sl.on) state.remember(d.path, (d.restore_y >= 0 ? d.restore_y : d.scroll_target) / fonts.scale);
    };
    auto remember_scroll = [&]() { remember_scroll_of(st, slides); };
    // Window rect (as tracked by the frame loop) + scroll offsets -> the state file.
    auto save_state_file = [&]() {
        remember_scroll();
        for_waiting([&](Tab& t) { remember_scroll_of(t.st, t.slides); });
        sync_state_tabs();
        if (tracked) {
            normal_rect.maximized = fullscreen ? fs_was_maximized : (glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0);
            state.window = normal_rect;
        }
        save_state_guarded(state_file, state, conf.cfg.remember_window);
    };
#ifdef __ANDROID__
    // The app may be killed once it is in the background: flush the config and the state file when it goes (spec 35).
    android_shell::set_background_hook([&] {
        trace::log("background: writing %zu pending config keys and the state file", conf.pending.size());
        conf.flush(glfwGetTime(), true);
        save_state_file();
    });
#endif
    // Show `p` (a new file, or the same one with an anchor / scroll position).
    // `push`: a new navigation (history gets the old place); `restore`: scroll offset in points (history step).
    auto navigate = [&](const fs::path& p, const std::string& anchor, bool push, std::optional<float> restore) {
        std::error_code eq;
        const bool same = (p.empty() && st.path.empty()) || (!st.path.empty() && fs::equivalent(p, st.path, eq));
        const session::Location before = here();
        if (same) {
            if (push && !before.path.empty()) history.push(before);
            if (!anchor.empty()) st.pending_anchor = anchor;
            else if (restore) st.scroll_to = *restore * fonts.scale;
            return;
        }
        remember_scroll();
        if (!st.load(p)) return;
        state.add_recent(p);
        if (push && !before.path.empty()) history.push(before);
        st.pending_anchor = anchor;
        float y = 0;
        if (restore) y = *restore;
        else if (auto o = state.lookup(p)) y = float(*o);
        st.restore_y = y * fonts.scale;
        sync_state_tabs();
        save_state_guarded(state_file, state, conf.cfg.remember_window);  // (cheap) so a crash keeps the positions
    };
    auto go_history = [&](bool back, int count) {
        for (int i = 0; i < count; i++) {
            auto loc = back ? history.back(here()) : history.forward(here());
            if (!loc || loc->path.empty()) break;
            navigate(loc->path, "", false, loc->scroll);
        }
    };
    // --- tabs: switching, opening, closing (spec 34) ---
    // Bring tab `i` to the front: the live variables swap with its (the stand-in goes to the one left).
    auto switch_tab = [&](size_t i) {
        if (i >= tabs.size() || i == active_tab) return;
        remember_scroll();
        swap_live(*tabs[active_tab]);
        swap_live(*tabs[i]);
        active_tab = i;
        st.restore_y = st.scroll_target;  // the view comes back to where this tab was left
        outline.clear();                   // (the frame draws the new one's)
        vim.reset();
        sys_check = true;                  // (look for characters no font has, in this document)
        extra_frames = std::max(extra_frames, 3);
        sync_state_tabs();
        save_state_guarded(state_file, state, conf.cfg.remember_window);
    };
    auto new_tab = [&] {  // an empty tab, now the active one
        tabs.push_back(std::make_unique<Tab>());
        switch_tab(tabs.size() - 1);
    };
    auto close_tab = [&](size_t i) {
        if (g_editor || i >= tabs.size()) return;
        if (tabs.size() == 1) {  // the last one: an empty window showing the demo, as without a file
            remember_scroll();
            st = DocState();
            find = FindState();
            slides = SlideState();
            history = session::History();
            show_demo();
            sys_check = true;
        } else if (i != active_tab) {
            tabs.erase(tabs.begin() + std::ptrdiff_t(i));
            if (i < active_tab) active_tab--;
        } else {  // the tab to its right takes over (or the last one)
            remember_scroll();
            tabs.erase(tabs.begin() + std::ptrdiff_t(i));  // (the stand-in)
            const size_t j = std::min(i, tabs.size() - 1);
            swap_live(*tabs[j]);  // the closed tab's data now sits in tabs[j]...
            *tabs[j] = Tab();     // ...and goes; the stand-in takes its place
            active_tab = j;
            st.restore_y = st.scroll_target;
            outline.clear();
            vim.reset();
            sys_check = true;
        }
        extra_frames = std::max(extra_frames, 3);
        sync_state_tabs();
        save_state_guarded(state_file, state, conf.cfg.remember_window);
    };
    // Next / previous tab, `n` steps, wrapping.
    auto step_tab = [&](int n) {
        const int count = int(tabs.size());
        if (count > 1 && !g_editor) switch_tab(size_t(((int(active_tab) + n) % count + count) % count));
    };
    // Open `p` (Open, the recent list, a dropped file, a link): in the current tab, or in a new one;
    // a file that is already open in a tab shows that tab instead.
    auto open_file = [&](const fs::path& p, const std::string& anchor, bool in_new_tab) {
        if (!g_editor) {
            const auto open = find_tab(p);
            if (open && *open != active_tab) {
                switch_tab(*open);
                if (!anchor.empty()) st.pending_anchor = anchor;
                return;
            }
            std::error_code ec;
            // (A tab showing the built-in demo has no path: the file takes its place, spec 34.)
            const bool demo_tab = st.path.empty();
            if (in_new_tab && !demo_tab && !open && fs::is_regular_file(p, ec)) {
                const size_t before = active_tab;
                new_tab();
                navigate(p, anchor, false, std::nullopt);
                if (st.path.empty()) {  // it could not be read: back to where we were
                    close_tab(active_tab);
                    switch_tab(before);
                }
                return;
            }
        }
        navigate(p, anchor, true, std::nullopt);
    };
    // Start-up (spec 34): the other files of the command line, or the tabs of the last run, in tabs of their own.
    {
        std::vector<fs::path> more;
        size_t first_tab = 0;
        if (restore_tabs) {
            std::vector<fs::path> reopen;
            for (size_t i = 0; i < state.tabs.size(); i++) {
                const fs::path p(std::u8string(state.tabs[i].begin(), state.tabs[i].end()));
                std::error_code gone;
                if (!fs::is_regular_file(p, gone)) continue;  // (gone: skipped)
                if (i + 1 == state.active_tab) first_tab = reopen.size();
                reopen.push_back(p);
            }
            if (!reopen.empty() && st.load(reopen[0])) {
                if (auto off = state.lookup(reopen[0])) st.restore_y = float(*off) * fonts.scale;
                more.assign(reopen.begin() + 1, reopen.end());
            } else if (st.path.empty()) {
                show_demo();
            }
        } else if (cli_files.size() > 1) {
            more.assign(cli_files.begin() + 1, cli_files.end());
        }
        const auto recent = state.recent;  // (reopening is not opening: the list keeps its order)
        for (const fs::path& p : more) open_file(p, "", true);
        if (restore_tabs) state.recent = recent;
        switch_tab(first_tab);
    }
    // ----- marks (spec 28; as main.rs) -----
    auto toast = [&](std::string text) { st.toast = std::move(text), st.toast_since = ImGui::GetTime(); };
    // The first block reaching below the top of the view, and how far down into it (0..1) the view
    // starts (render.rs's top_anchor).
    auto top_anchor = [&]() -> std::pair<size_t, float> {
        const float doc_y = st.scroll_cur - top_pad(fonts.scale);
        const auto& tops = st.cache.top;
        const auto& hs = st.cache.height;
        for (size_t i = 0; i < tops.size() && i < hs.size(); i++) {
            const float h = std::isnan(hs[i]) ? 1.0f : std::max(hs[i], 1.0f);
            if (tops[i] + h > doc_y) return {i, std::clamp((doc_y - tops[i]) / h, 0.0f, 1.0f)};
        }
        return {tops.empty() ? 0 : tops.size() - 1, 0.0f};
    };
    auto set_mark = [&](char name) {
        if (st.path.empty()) return toast("Marks need a file (this is the built-in demo)");
        const auto [block, frac] = top_anchor();
        state.set_mark({name, to_utf8(st.path), block, frac, block < st.keys.size() ? st.keys[block] : std::string()});
        st.marks_dirty = true;
        save_state_guarded(state_file, state, conf.cfg.remember_window);
    };
    // Go to a mark (a global one may open its file); the place we leave goes into the history
    // (`''` comes back). A mark whose text is gone is dropped.
    auto go_mark = [&](char name) {
        const session::Mark* found = state.mark(name, st.path);
        if (!found) return toast(std::string("Mark ") + name + " is not set");
        const session::Mark m = *found;
        const fs::path target(std::u8string(m.path.begin(), m.path.end()));
        std::error_code ec;
        if (const auto open = find_tab(target); open && *open != active_tab) switch_tab(*open);  // (spec 34: its tab, then as below)
        if (!st.path.empty() && fs::equivalent(target, st.path, ec)) {
            history.push(here());
        } else {
            if (!fs::is_regular_file(target, ec)) return toast(std::string("Mark ") + name + ": " + m.path + " is gone");
            navigate(target, "", true, std::nullopt);
            if (st.path.empty() || !fs::equivalent(target, st.path, ec)) return;  // it could not be opened
            st.restore_y = -1;  // (the mark decides where we go)
        }
        if (const auto b = marks::resolve(m.block, m.text, st.keys)) {
            if (*b != m.block) {  // it follows its text
                session::Mark moved = m;
                moved.block = *b;
                state.set_mark(moved);
                save_state_guarded(state_file, state, conf.cfg.remember_window);
            }
            st.zoom_anchor = {*b, m.frac};
        } else {
            state.remove_mark(name, target);
            save_state_guarded(state_file, state, conf.cfg.remember_window);
            toast(std::string("Mark ") + name + " is gone: its text changed");
        }
        st.marks_dirty = true;
    };
    // The margin's marks for the shown file, where they are now.
    auto refresh_mark_margin = [&] {
        st.mark_labels.clear();
        if (st.path.empty()) return;
        std::vector<std::pair<std::pair<size_t, float>, std::vector<char>>> spots;
        for (const session::Mark* m : state.marks_in(st.path)) {
            const auto b = marks::resolve(m->block, m->text, st.keys);
            if (!b) continue;
            auto it = std::find_if(spots.begin(), spots.end(), [&](const auto& s) { return s.first == std::make_pair(*b, m->frac); });
            if (it != spots.end()) it->second.push_back(m->name);
            else spots.push_back({{*b, m->frac}, {m->name}});
        }
        for (auto& [at, names] : spots) st.mark_labels.push_back({marks::margin_label(names), at.first, at.second});
    };
    // `*` / `#`: find the selected text, or the word under the mouse (as painted last frame).
    auto find_word = [&](int dir) {
        std::string word;
        uint32_t block = 0;
        if (const auto r = st.sel.range()) {
            std::istringstream lines(selected_text(st.runs.runs, *r));
            for (std::string l; std::getline(lines, l);) {
                l.erase(0, l.find_first_not_of(" \t\r"));
                l.erase(l.find_last_not_of(" \t\r") + 1);
                if (!l.empty()) {
                    word = l;
                    break;
                }
            }
            block = r->first.block;
        } else {
            const ImVec2 p = ImGui::GetMousePos();
            for (const Run& run : st.runs.runs)
                if (run.contains(p) && !run.xs.empty() && p.x >= run.xs.front() && p.x <= run.xs.back()) {
                    // trimmed of punctuation (ASCII; letters, digits and `_` stay)
                    auto keep = [](unsigned char c) { return c >= 0x80 || std::isalnum(c) || c == '_'; };
                    size_t a = 0, z = run.text.size();
                    while (a < z && !keep(static_cast<unsigned char>(run.text[a]))) a++;
                    while (z > a && !keep(static_cast<unsigned char>(run.text[z - 1]))) z--;
                    word = run.text.substr(a, z - a);
                    block = run.pos.block;
                    break;
                }
        }
        if (word.empty()) return toast("Select some text or point at a word to search for it");
        find.open = true;
        find.focus = false;
        std::snprintf(find.buf, sizeof(find.buf), "%s", word.c_str());
        find.typed = find.buf;  // (not typing: no waiting)
        find.vim = false;
        find.query.clear();  // (searched again, even for the same word)
        find.word_from = {block, dir};
    };
    auto toggle_fullscreen = [&]() {
        if (!fullscreen) {
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) return;
            // The monitor the window mostly sits on.
            int wx, wy, ww, wh, nmon = 0;
            glfwGetWindowPos(window, &wx, &wy);
            glfwGetWindowSize(window, &ww, &wh);
            GLFWmonitor** mons = glfwGetMonitors(&nmon);
            GLFWmonitor* best = glfwGetPrimaryMonitor();
            long best_area = -1;
            for (int i = 0; i < nmon; i++) {
                int mx, my, mw, mh;
                glfwGetMonitorWorkarea(mons[i], &mx, &my, &mw, &mh);
                const long ow = std::max(0, std::min(wx + ww, mx + mw) - std::max(wx, mx));
                const long oh = std::max(0, std::min(wy + wh, my + mh) - std::max(wy, my));
                if (ow * oh > best_area) best_area = ow * oh, best = mons[i];
            }
            const GLFWvidmode* mode = best ? glfwGetVideoMode(best) : nullptr;
            if (!mode) return;
            fs_was_maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
            fullscreen = true;  // (stops the rect tracking: normal_rect keeps the windowed place)
            // GLFW minimizes full-screen windows when they lose focus; a
            // borderless reader on another monitor must stay put.
            glfwSetWindowAttrib(window, GLFW_AUTO_ICONIFY, GLFW_FALSE);
            glfwSetWindowMonitor(window, best, 0, 0, mode->width, mode->height, mode->refreshRate);
        } else {
            fullscreen = false;
            glfwSetWindowMonitor(window, nullptr, normal_rect.x + normal_frame_l, normal_rect.y + normal_frame_t,
                                 normal_rect.width, normal_rect.height, GLFW_DONT_CARE);
            if (fs_was_maximized) glfwMaximizeWindow(window);
        }
    };

    // --- editor live preview (spec 15): apply one protocol message. -------------
    struct EditorDoc {
        fs::path path;
        std::string text;
        std::string format;
    };
    std::map<std::string, EditorDoc> editor_docs;  // the buffers the editor has opened (and not closed)
    std::string editor_current;                    // the one the editor looks at now
    auto editor_apply = [&](const editor::Message& m) {
        using editor::Message;
        switch (m.type) {
            case Message::Type::Open: {
                fs::path file = m.path.empty() ? fs::path() : fs::path(std::u8string(m.path.begin(), m.path.end()));
                if (g_script_mode && !file.empty() && file.is_relative()) file = script_dir / file;
                editor_docs[m.doc] = {file, m.text, m.format};
                if (editor_current != m.doc) find.ed_text.clear();  // a different doc: its search is gone
                editor_current = m.doc;
                st.show_live(m.doc, file, m.text, m.format);
                break;
            }
            case Message::Type::Text: {
                auto it = editor_docs.find(m.doc);
                if (it == editor_docs.end()) break;  // unknown (or closed) document
                it->second.text = m.text;
                // (Also brings the editor's document back after a link led to another file.)
                if (m.doc == editor_current) st.show_live(m.doc, it->second.path, m.text, it->second.format);
                break;
            }
            case Message::Type::Cursor:
                if (st.live && st.live_open && st.live_id == m.doc && m.line > 0) {
                    st.sync_line = m.line;
                    if (slides.on) {  // the slide holding that line; no marker (spec 26)
                        size_t b = 0;
                        for (size_t i = 0; i < st.doc.size(); i++)
                            if (st.doc[i].line_first > 0 && st.doc[i].line_first <= m.line) b = i;
                        const auto ranges = st.split();
                        const auto steps = st.steps(ranges);
                        slides.index = slides::slide_of_block(ranges, b);
                        slides.step = slides.index < steps.size() ? slides::step_of_block(steps[slides.index], b) : 0;  // revealed up to the cursor
                        slides.scroll = 0;
                    } else {
                        st.sync_pending = true;
                    }
                }
                break;
            case Message::Type::Search:
                if (st.live && st.live_open && st.live_id == m.doc) {
                    find.ed_text = m.text;
                    find.ed_case = m.case_sensitive;
                }
                break;
            case Message::Type::Close:
                editor_docs.erase(m.doc);  // what is shown stays, but no more updates
                if (editor_current == m.doc) editor_current.clear();
                if (st.live && st.live_id == m.doc) st.live_open = false;
                break;
            case Message::Type::Quit: glfwSetWindowShouldClose(window, GLFW_TRUE); break;
        }
        extra_frames = std::max(extra_frames, 3);
    };

    // --- script mode: commands become "steps" (input injected into ImGui's
    // queue, then a few frames) and "settling" (frames until nothing moves). ---
    struct Step {
        std::function<void()> fn;
        int frames;  // frames to run after fn
    };
    std::deque<Step> steps;
    int script_frames = 0;  // frames still to run before the next step / command
    bool settling = false;
    int settle_n = 0;
    bool script_idle = false;  // as of the last frame: nothing animating / pending
    FramePacer pacer;          // not used in script mode (it draws as fast as it can)
    size_t script_next = 0;
    int script_w = 1200, script_h = 900;  // client size in px (the `size` command), before `scale`
    int script_exit = 0;
    Offscreen offscreen;
    std::string window_title = "Calligrus";
    auto queue = [&](std::function<void()> fn, int frames) { steps.push_back({std::move(fn), frames}); };
    auto press_key = [&](const script::KeyToken& k) {
        bool implied = false;
        const ImGuiKey key = imgui_key_for(k, implied);
        const bool shift = k.shift || implied;
        // Modifiers go down a frame before the key (ImGui's input queue would
        // otherwise hold the key back anyway), and come up a frame after it.
        const bool mods = k.ctrl || shift || k.alt;
        if (mods)
            queue(
                [&io, &extra_frames, k, shift] {
                    if (k.ctrl) io.AddKeyEvent(ImGuiKey_LeftCtrl, true), io.AddKeyEvent(ImGuiMod_Ctrl, true);
                    if (shift) io.AddKeyEvent(ImGuiKey_LeftShift, true), io.AddKeyEvent(ImGuiMod_Shift, true);
                    if (k.alt) io.AddKeyEvent(ImGuiKey_LeftAlt, true), io.AddKeyEvent(ImGuiMod_Alt, true);
                    extra_frames = 3;
                },
                2);
        queue(
            [&io, &extra_frames, k, key] {
                if (key != ImGuiKey_None) io.AddKeyEvent(key, true);
                // Like a real keyboard: a typed character too (not with Ctrl / Alt).
                if (!k.ctrl && !k.alt) {
                    if (k.named.empty()) io.AddInputCharacter(unsigned(k.ch));
                    else if (k.named == "space") io.AddInputCharacter(' ');
                }
                extra_frames = 3;
            },
            2);
        queue([&io, key] { if (key != ImGuiKey_None) io.AddKeyEvent(key, false); }, 2);
        if (mods)
            queue(
                [&io, k, shift] {
                    if (k.alt) io.AddKeyEvent(ImGuiKey_LeftAlt, false), io.AddKeyEvent(ImGuiMod_Alt, false);
                    if (shift) io.AddKeyEvent(ImGuiKey_LeftShift, false), io.AddKeyEvent(ImGuiMod_Shift, false);
                    if (k.ctrl) io.AddKeyEvent(ImGuiKey_LeftCtrl, false), io.AddKeyEvent(ImGuiMod_Ctrl, false);
                },
                2);
    };
    // `ctrl` / `shift` / `alt` words after click / move / drag coordinates: the keys go down a
    // frame before the mouse input, and come up a frame after it.
    auto mods_down = [&](const script::Command& c) {
        if (c.mod_ctrl || c.mod_shift || c.mod_alt)
            queue(
                [&io, &extra_frames, ctrl = c.mod_ctrl, shift = c.mod_shift, alt = c.mod_alt] {
                    if (ctrl) io.AddKeyEvent(ImGuiKey_LeftCtrl, true), io.AddKeyEvent(ImGuiMod_Ctrl, true);
                    if (shift) io.AddKeyEvent(ImGuiKey_LeftShift, true), io.AddKeyEvent(ImGuiMod_Shift, true);
                    if (alt) io.AddKeyEvent(ImGuiKey_LeftAlt, true), io.AddKeyEvent(ImGuiMod_Alt, true);
                    extra_frames = 3;
                },
                2);
    };
    auto mods_up = [&](const script::Command& c) {
        if (c.mod_ctrl || c.mod_shift || c.mod_alt)
            queue(
                [&io, ctrl = c.mod_ctrl, shift = c.mod_shift, alt = c.mod_alt] {
                    if (alt) io.AddKeyEvent(ImGuiKey_LeftAlt, false), io.AddKeyEvent(ImGuiMod_Alt, false);
                    if (shift) io.AddKeyEvent(ImGuiKey_LeftShift, false), io.AddKeyEvent(ImGuiMod_Shift, false);
                    if (ctrl) io.AddKeyEvent(ImGuiKey_LeftCtrl, false), io.AddKeyEvent(ImGuiMod_Ctrl, false);
                },
                2);
    };
    auto mouse_pos = [&](double x, double y) {
        queue([&io, &extra_frames, x, y] { io.AddMousePosEvent(float(x), float(y)), extra_frames = 3; }, 1);
    };
    auto mouse_button = [&](int b, bool down) {
        queue([&io, &extra_frames, b, down] { io.AddMouseButtonEvent(b, down), extra_frames = 3; }, 2);
    };
    // A slides export started from the app ("PDF" button, `e` in slides mode; spec 27): the exe's own
    // `--export` in a child process, waited for on a thread (detached: the job outlives a closed window).
    struct ExportJob {
        fs::path out;
        bool pages = false;  // a document export (spec 29), not the slides
        std::atomic<bool> done{false};
        int code = 0;        // the child's exit code (-1: it could not be started)
        std::string output;  // what it printed
    };
    std::shared_ptr<ExportJob> export_job;
    bool export_wait = false;  // a script's `export` waits for the job
    double export_wait_since = 0;
    std::string shown_theme;  // the name of the theme on screen now (the frame loop keeps it)
    auto start_export = [&](const fs::path& out, bool pages) {
        if (export_job || !(st.live || !st.path.empty())) return;
        auto job = std::make_shared<ExportJob>();
        job->out = out;
        job->pages = pages;
        // Pages follow the app: the text size on screen (body size x zoom), and the shown theme if asked.
        const std::string body_pt = [&] {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%g", ::pages::export_body_size(conf.cfg.body_size, conf.cfg.zoom));
            return std::string(buf);
        }();
        const std::string theme_arg = ui.pdf_current_theme ? shown_theme : "";
        const std::string paper_arg = ui.pdf_paper == 1 ? "letter" : "a4";
        // In editor mode: the shown text (unsaved edits included) goes to a temp file the child reads,
        // with `--base-dir` at the real document's folder; the file is deleted when the child is done.
        fs::path doc = st.path, base_dir_arg, temp;
        std::string temp_text;
        if (st.live) {
            temp = doc = script::editor_temp_file(st.live_format == "org");
            temp_text = st.live_text;
            base_dir_arg = script::editor_base_dir(st.live_path);
        }
        std::thread([job, doc, temp, temp_text, base_dir_arg, body_pt, theme_arg, paper_arg] {
            std::string text;
            if (!temp.empty() && !platform::write_file_atomic(temp, temp_text)) {
                job->output = "export: cannot write " + to_utf8(temp);
                job->code = 1;
                job->done = true;
                if (g_glfw_alive) glfwPostEmptyEvent();
                return;
            }
            std::vector<fs::path> argv = {platform::exe_path(), fs::path("--export"), job->out, doc};
            if (!base_dir_arg.empty()) argv.push_back(fs::path("--base-dir")), argv.push_back(base_dir_arg);
            if (job->pages) {
                argv.push_back(fs::path("--pages")), argv.push_back(fs::path("--body-size")), argv.push_back(fs::path(body_pt));
                if (!theme_arg.empty()) argv.push_back(fs::path("--theme")), argv.push_back(fs::path(theme_arg));
                argv.push_back(fs::path("--paper")), argv.push_back(fs::path(paper_arg));
            }
            job->code = platform::run_and_capture(argv, text);
            if (!temp.empty()) {
                std::error_code rm;
                fs::remove(temp, rm);  // success or failure
            }
            job->output = std::move(text);
            job->done = true;
            if (g_glfw_alive) glfwPostEmptyEvent();
        }).detach();
        export_job = std::move(job);
    };
    // While it runs, the toast says so; when it has finished, what came of it (as main.rs).
    auto poll_export = [&] {
        if (!export_job) return;
        const std::string name = to_utf8(export_job->out.filename());
        if (!export_job->done) {
            st.toast = "Exporting to " + name + "\xE2\x80\xA6";  // "…"
        } else {
            size_t n = 0;
            std::string err;
            if (export_job->code == -1) st.toast = "Export failed: cannot start the export";
            else if (script::export_outcome(export_job->output, n, err))
                st.toast = "Exported " + std::to_string(n) + (export_job->pages ? (n == 1 ? " page" : " pages") : (n == 1 ? " slide" : " slides")) + " to " + name;
            else st.toast = "Export failed: " + err;
            export_job.reset();
        }
        st.toast_since = ImGui::GetTime();
    };
    size_t editor_seen = 0;  // how many editor messages the last `expect-editor` has looked at
    std::function<std::string()> sync_dump_fn;  // set by the frame loop (it owns the theme and fonts)
    auto script_fail = [&](const script::Command& c, const std::string& msg) {
        script_log("line " + std::to_string(c.line) + ": " + msg, true);
        script_exit = 1;
        return false;
    };
    // Run one command. false = stop the script (quit, or an error).
    auto script_exec = [&](const script::Command& c) -> bool {
        using script::Kind;
        auto to_path = [&](const std::string& t) { return script_dir / fs::path(std::u8string(t.begin(), t.end())); };
        switch (c.kind) {
            case Kind::Size: script_w = int(c.a), script_h = int(c.b); break;
            case Kind::Scale: queue([&dpi, s = float(c.a)] { dpi = s; }, 1); break;
            case Kind::Open: {
                const fs::path file = to_path(c.text).lexically_normal();
                std::error_code e;
                if (!fs::is_regular_file(file, e)) return script_fail(c, "no such file: " + to_utf8(file));
                queue([&, file] { navigate(file, "", true, std::nullopt), extra_frames = 3; }, 1);
                break;
            }
            case Kind::Keys:
                for (const script::KeyToken& k : c.keys) press_key(k);
                break;
            case Kind::Type:
                queue([&io, &extra_frames, t = c.text] { io.AddInputCharactersUTF8(t.c_str()), extra_frames = 3; }, 2);
                break;
            case Kind::Click:
                mods_down(c);
                mouse_pos(c.a, c.b);
                mouse_button(c.button, true);
                mouse_button(c.button, false);
                mods_up(c);
                break;
            case Kind::Move:
                mods_down(c);
                mouse_pos(c.a, c.b);
                mods_up(c);
                break;
            case Kind::Drag: {
                mods_down(c);
                mouse_pos(c.a, c.b);
                mouse_button(0, true);
                for (int i = 1; i <= 6; i++) {
                    const double t = i / 6.0;
                    queue([&io, &extra_frames, x = c.a + (c.c - c.a) * t, y = c.b + (c.d - c.b) * t] {
                        io.AddMousePosEvent(float(x), float(y)), extra_frames = 3;
                    }, 1);
                }
                mouse_button(0, false);
                mods_up(c);
                break;
            }
            case Kind::Wheel: {
                const double x = c.has_pos ? c.b : script_w * dpi / 2, y = c.has_pos ? c.c : script_h * dpi / 2;
                mouse_pos(x, y);
                queue([&io, &extra_frames, dy = float(c.a)] { io.AddMouseWheelEvent(0.0f, -dy), extra_frames = 3; }, 2);
                break;
            }
            case Kind::Settle: break;
            case Kind::Frames: script_frames = int(c.a); return true;
            case Kind::Slide:
                queue([&, n = size_t(c.a)] {
                    set_slides(true);
                    slides.index = n - 1, slides.step = SIZE_MAX, slides.scroll = 0;  // (slides_prepare clamps them)
                    extra_frames = 3;
                }, 1);
                break;
            case Kind::Export:
            case Kind::ExportPages: {
                const fs::path file = (script_out.empty() ? script_dir : script_out) / fs::path(std::u8string(c.text.begin(), c.text.end()));
                queue([&, file, pages = c.kind == Kind::ExportPages] { start_export(file, pages), extra_frames = 3; }, 1);
                export_wait = true, export_wait_since = glfwGetTime();
                break;
            }
            case Kind::PagesBegin: {  // (the pages export's own: switch the page view on)
                float w = 0, h = 0;
                pages::paper_size(c.text, w, h);
                const std::string want = exporting && !exporting->theme.empty() ? exporting->theme : conf.cfg.light_theme;
                if (!themes.find(want)) {
                    script_log("export: unknown theme " + want, true);
                    script_exit = 1;
                    return false;
                }
                // real points: `--body-size`, else the config's body size (pixels) at 0.75 pt each
                const float body_pt = exporting && exporting->body_size > 0 ? exporting->body_size : pages::export_body_size(conf.cfg.body_size, 1.0);
                queue([&, w, h, want, body_pt] {
                    pg.on = true, pg.paper_w = w, pg.paper_h = h, pg.theme = want, pg.page = 0, pg.ends.clear(), pg.body_pt = body_pt;
                    extra_frames = 3;
                }, 1);
                break;
            }
            case Kind::PageShot: {  // keep the page on screen for the PDF; then the next one (this command runs again)
                if (offscreen.w <= 0 || !pg.on || pg.ends.empty()) return script_fail(c, "no page to shoot");
                const int w = offscreen.w, h = offscreen.h;
                std::vector<unsigned char> px(size_t(w) * h * 4);
                offscreen.bind_fb(Offscreen::kFramebuffer, offscreen.fbo);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                export_pages.push_back({w, h, pdf::rgb_from_gl_rgba(px, w, h), pg.paper_w, pg.paper_h});
                export_pages.back().predictor = text_capture::enabled();
                text_capture::fill(export_pages.back());
                export_total = export_pages.size();
                if (pg.page + 1 < pg.ends.size()) pg.page++, script_next--;
                break;
            }
            case Kind::Shot: {
                if (offscreen.w <= 0) return script_fail(c, "no frame rendered yet");
                if (export_pdf) {  // keep the picture for the PDF
                    const int w = offscreen.w, h = offscreen.h;
                    std::vector<unsigned char> px(size_t(w) * h * 4);
                    offscreen.bind_fb(Offscreen::kFramebuffer, offscreen.fbo);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                    export_pages.push_back({w, h, pdf::rgb_from_gl_rgba(px, w, h)});
                    export_pages.back().predictor = text_capture::enabled();
                    text_capture::fill(export_pages.back());
                    return true;
                }
                const fs::path file = (script_out.empty() ? script_dir : script_out) / fs::path(std::u8string(c.text.begin(), c.text.end()));
                std::error_code e;
                fs::create_directories(file.parent_path(), e);
                const int w = offscreen.w, h = offscreen.h;
                std::vector<unsigned char> px(size_t(w) * h * 4);
                offscreen.bind_fb(Offscreen::kFramebuffer, offscreen.fbo);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                std::ofstream out(file, std::ios::binary);
                if (!out) return script_fail(c, "cannot write " + to_utf8(file));
                stbi_flip_vertically_on_write(1);  // OpenGL rows start at the bottom
                stbi_write_png_to_func([](void* ctx, void* data, int n) { static_cast<std::ofstream*>(ctx)->write(static_cast<const char*>(data), n); },
                                       &out, w, h, 4, px.data(), w * 4);
                out.close();
                if (!exporting) script_log("shot " + c.text + " " + std::to_string(w) + "x" + std::to_string(h));
                return true;
            }
            case Kind::Echo: script_log(c.text); return true;
            case Kind::SyncDump: {
                const fs::path file = (script_out.empty() ? script_dir : script_out) / fs::path(std::u8string(c.text.begin(), c.text.end()));
                std::error_code e;
                fs::create_directories(file.parent_path(), e);
                std::ofstream out(file, std::ios::binary);
                if (!out || !sync_dump_fn) return script_fail(c, "cannot write " + to_utf8(file));
                out << sync_dump_fn();
                out.close();
                script_log("sync-dump " + c.text);
                return true;
            }
            case Kind::Editor: {
                editor::Message m;
                std::string err;
                if (!editor::parse_message(c.text, m, err)) return err.empty() ? true : script_fail(c, "editor: " + err);
                queue([&editor_apply, m] { editor_apply(m); }, 1);
                break;
            }
            case Kind::ExpectEditor: {
                // The newest message the app wrote since the last check (`none`: there must be none).
                const size_t seen = std::min(editor_seen, g_editor_lines.size());
                const bool fresh = g_editor_lines.size() > seen;
                const std::string newest = fresh ? g_editor_lines.back() : std::string();
                editor_seen = g_editor_lines.size();
                if (c.text.empty()) {
                    if (fresh) return script_fail(c, "expect-editor none: the app sent " + newest);
                } else if (!fresh) {
                    return script_fail(c, "expect-editor: the app sent nothing since the last check");
                } else if (!editor::json_objects_equal(newest, c.text)) {
                    return script_fail(c, "expect-editor: the app sent " + newest + ", expected " + c.text);
                }
                return true;
            }
            case Kind::ExpectTabs:
                if (tabs.size() != size_t(c.a) || active_tab + 1 != size_t(c.b))
                    return script_fail(c, "expect-tabs: " + std::to_string(tabs.size()) + " tabs, tab " + std::to_string(active_tab + 1) + " active; expected " +
                                              std::to_string(size_t(c.a)) + " and " + std::to_string(size_t(c.b)));
                return true;
            case Kind::ExpectFind: {
                const char* state = !find.open ? "closed" : io.WantTextInput ? "field" : "doc";
                const int n = int(find.matches.size()), cur = find.current + 1;
                if (c.text != state || (c.text != "closed" && (n != int(c.a) || cur != int(c.b))))
                    return script_fail(c, std::string("expect-find: ") + state + ", " + std::to_string(n) + " matches, current " + std::to_string(cur) + "; expected " + c.text +
                                              (c.text == "closed" ? "" : ", " + std::to_string(int(c.a)) + ", " + std::to_string(int(c.b))));
                return true;
            }
            case Kind::ExpectTitle:
                if (window_title != c.text) return script_fail(c, "expect-title: title is '" + window_title + "', expected '" + c.text + "'");
                return true;
            case Kind::Quit: return false;
        }
        settling = true;  // after every input command: frames until settled
        settle_n = 0;
        return true;
    };
    // Called at the top of every loop turn: false = the script is over.
    auto script_pump = [&]() -> bool {
        for (;;) {
            if (script_frames > 0) {
                script_frames--;
                return true;
            }
            if (!steps.empty()) {
                Step s = std::move(steps.front());
                steps.pop_front();
                s.fn();
                script_frames = s.frames - 1;
                return true;
            }
            if (export_wait) {  // `export`: frames until the child has finished (then settle)
                if (export_job && !export_job->done) {
                    if (glfwGetTime() - export_wait_since > 120) {
                        script_log("export: no result after 120 s", true);
                        script_exit = 1;
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    return true;
                }
                export_wait = false, settling = true, settle_n = 0;
            }
            if (settling) {
                if ((settle_n >= 4 && script_idle) || settle_n >= 300) settling = false;
                else {
                    settle_n++;
                    return true;
                }
            }
            if (script_next >= script_parsed.commands.size()) return false;
            if (!script_exec(script_parsed.commands[script_next++])) return false;
        }
    };
    if (g_script_mode) {
        if (!offscreen.load()) {
            script_log("OpenGL framebuffer objects are not available", true);
            return 1;
        }
        io.AddFocusEvent(true);
        settling = true;  // start-up frames (fonts, layout) before the first command
    }

    while (!glfwWindowShouldClose(window)) {
        // Sleep until input arrives (or a timeout, so live reload and the
        // config writer still run), but keep drawing while things move.
        bool busy = st.animating || autoscroll.active() || extra_frames > 0;
#ifdef __ANDROID__
        busy = busy || android_shell::touch_busy();  // (a finger down, a fling, a tap being delivered)
#endif
        if (g_script_mode) {
            if (!script_pump()) break;
            glfwPollEvents();
        } else if (busy) glfwPollEvents();
#ifdef __ANDROID__
        else if (!android_shell::legacy_power()) {
            // Spec 35 (power): sleep until input / a job / a real deadline wakes us; no timer when nothing is due.
            double timeout = 1e9;  // (the window layer waits without a timeout)
            render_deadline = false;
            const double t_now = glfwGetTime();
            auto soon = [&](double seconds, bool renders) {
                timeout = std::min(timeout, std::max(seconds, 0.0) + 0.01);
                render_deadline = render_deadline || renders;
            };
            if (!conf.pending.empty()) soon(conf.due - t_now, false);                             // the debounced config write
            if (st.bar_wake > 0) soon(st.bar_wake - ImGui::GetTime(), true);                      // the scrollbar goes
            if (!st.toast.empty()) soon(st.toast_since + 1.6 - ImGui::GetTime(), true);          // the toast goes
            if (find.due > 0) soon(find.due - ImGui::GetTime(), true);                            // a find search waiting for the typing to stop
            if (quiet_until > t_now) soon(0.2, true);                                             // hover delays, tooltips, widget fades settle
            if (io.WantTextInput) soon(0.5, true);                                                // the caret blinks
            if (!android_idle) soon(0.016, true);                                                 // something is still moving into place
            glfwWaitEventsTimeout(timeout);
        }
#endif
        else {
            double timeout = conf.pending.empty() ? 0.5 : 0.1;
            // Hide the scrollbar on time (bar_wake is on ImGui's clock, which stands at the last frame).
            if (st.bar_wake > 0) timeout = std::clamp(st.bar_wake - ImGui::GetTime() + 0.01, 0.0, timeout);
            // Likewise the toast (draw_toast).
            if (!st.toast.empty()) timeout = std::clamp(st.toast_since + 1.6 - ImGui::GetTime() + 0.01, 0.0, timeout);
            // Likewise a find search waiting for the typing to stop.
            if (find.due > 0) timeout = std::clamp(find.due - ImGui::GetTime() + 0.01, 0.0, timeout);
            glfwWaitEventsTimeout(timeout);
        }
        if (g_input) {
            extra_frames = 3;
            g_input = false;
        } else if (extra_frames > 0) {
            extra_frames--;
        }
        const double now = glfwGetTime();
        frame_ms = trace::now_ms();
#ifdef __ANDROID__
        if (!g_script_mode && !android_shell::legacy_power()) {
            const bool woken = android_shell::consume_woken();  // input, a job, a window event: not just a timer
            if (woken) quiet_until = now + 1.0;
            if (!woken && !busy && android_idle && extra_frames == 0 && !render_deadline && fonts_ready && shown_frames >= 3) {
                // Only a timer ran out (the config write): no frame, no buffer swap.
                conf.flush(now);
                android_shell::count_skipped();
                continue;
            }
        }
#endif
        if (g_editor && !g_script_mode) {
            std::deque<std::string> lines;
            bool eof = false;
            {
                std::lock_guard<std::mutex> lock(editor_in.mutex);
                lines.swap(editor_in.lines);
                eof = editor_in.eof;
            }
            std::vector<editor::Message> msgs;
            for (const std::string& l : lines) {
                editor::Message m;
                std::string err;
                if (editor::parse_message(l, m, err)) msgs.push_back(std::move(m));
                else if (!err.empty()) script_log("editor: bad message (" + err + "): " + l.substr(0, 80), true);
            }
            for (size_t i = 0; i < msgs.size(); i++) {
                bool newer = false;  // coalesce: only the newest `text` of a document is parsed
                for (size_t j = i + 1; j < msgs.size() && !newer; j++) newer = editor::supersedes(msgs[j], msgs[i]);
                if (!newer) editor_apply(msgs[i]);
            }
            if (eof) glfwSetWindowShouldClose(window, GLFW_TRUE);  // the editor is gone
        }
        if (!g_dropped.empty()) {
            open_file(fs::path(std::u8string(g_dropped.begin(), g_dropped.end())), "", false);
            g_dropped.clear();
        }
        // Remember the windowed rect (for the state file and for leaving fullscreen).
        if (shown && !fullscreen && !glfwGetWindowAttrib(window, GLFW_ICONIFIED) && !glfwGetWindowAttrib(window, GLFW_MAXIMIZED)) {
            int x, y, w, h, l, t, r, b;
            glfwGetWindowPos(window, &x, &y);
            glfwGetWindowSize(window, &w, &h);
            glfwGetWindowFrameSize(window, &l, &t, &r, &b);
            normal_rect = {x - l, y - t, w, h, false};
            normal_frame_l = l;
            normal_frame_t = t;
            tracked = true;
        }
#ifdef __ANDROID__
        // Opened files are local copies, the config and themes are ours: nothing to watch. Only the night mode can change.
        if (!android_shell::legacy_power()) system_dark = platform::system_prefers_dark();
        if (android_shell::legacy_power() && Clock::now() - last_poll > std::chrono::seconds(1)) {
#else
        if (Clock::now() - last_poll > std::chrono::seconds(1)) {
#endif
            st.poll();
            for_waiting([](Tab& t) { t.st.poll(); });  // (the other tabs' files too)
            // Pictures changed on disk: load them again and measure the document again.
            if (image_refresh()) {
                st.cache.key = 0;
                slides.version = SIZE_MAX;
                for_waiting([](Tab& t) { t.st.cache.key = 0, t.slides.version = SIZE_MAX; });
            }
            system_dark = platform::system_prefers_dark();
            // The config file or the themes folder changed on disk: reload.
            if (conf.pending.empty() && conf.changed_on_disk())
                for (const std::string& w : conf.load()) st.error = w;
            // (A different config file may be in use now: its themes folder then.)
            const fs::path want_themes_dir = conf.path.parent_path() / "themes";
            const bool themes_moved = want_themes_dir != themes_dir;
            themes_dir = want_themes_dir;
            if (auto t = fs::last_write_time(themes_dir, ec); themes_moved || (!ec && t != themes_mtime)) {
                themes_mtime = ec ? fs::file_time_type{} : t;
                themes.load(themes_dir);
                themes.bind_entries(entry_names);
            }
            last_poll = Clock::now();
        }
        conf.flush(now);
        const Config& cfg = conf.cfg;

        // --- apply the settings (they may have changed anywhere: keys, the
        // settings window, the config file) ----------------------------------
        const Theme& theme = resolve_theme(themes, pg.on ? pg.theme : cfg.theme, cfg.dark_theme, cfg.light_theme, system_dark);
        shown_theme = theme.name;
        if (g_script_mode) sync_dump_fn = [&] { return sync_dump(st, fonts, theme); };
        if (resolve_font_choice(cfg))
            for (const std::string& e : font_choice.result.errors) st.error = e;  // font "X" not found
        // Spec 30: look for characters no font has; if there are any, choose installed fonts for them
        // in the background and add them to the chains (the atlas is then rebuilt below).
        auto sys_search = [&] {
        if (cfg.system_fallback && fonts_ready) {
            if (sys_job && sys_job->done) {  // a choice came back: keep its faces, up to the session's limit
                for (const FontSource& s : sys_job->result)
                    if (sys_fonts.size() < sysfont::kMaxPerSession && std::find(sys_fonts.begin(), sys_fonts.end(), s) == sys_fonts.end())
                        sys_fonts.push_back(s);
                sys_job.reset();
            }
            static uint64_t checked_version = ~uint64_t(0);
            if (checked_version != uint64_t(st.version)) checked_version = uint64_t(st.version), sys_check = true;
            if (sys_check && !sys_job) {
                sys_check = false;
                // Latin Modern Math is the last source of every font and does not count: an installed font
                // may be picked for a character it would otherwise draw (spec 8: system fonts come first).
                auto has = [](ImFont* font, char32_t c) {
                    const ImFontGlyph* g = font->GetFontBaked(16.0f)->FindGlyphNoFallback(ImWchar(c));
                    return g && int(g->SourceIdx) != font->Sources.Size - 1;
                };
                auto has_any = [&](char32_t c) { return has(fonts.regular, c) || has(fonts.mono, c); };
                trace::Stopwatch sw;
                const std::set<char32_t> chars = sysfont::document_chars(st.doc);
                sys_missing = sysfont::missing_chars(chars, has_any);
                trace::log("check: %zu distinct characters, %zu without a glyph (%.1f ms)", chars.size(), sys_missing.size(), sw.ms());
            }
            const size_t room = std::min(sysfont::kMaxPerPick, sysfont::kMaxPerSession - std::min(sys_fonts.size(), sysfont::kMaxPerSession));
            if (!sys_missing.empty() && !sys_job && font_index.ready() && room > 0) {
                auto job = std::make_shared<SysJob>();
                sys_job_fresh = false;
                for (char32_t c : sys_missing) sys_job_fresh = sys_job_fresh || sys_tried.insert(c).second;
                std::thread([job, index = font_index, missing = std::move(sys_missing), room] {
                    try {
                        // The font file is read here too, so the UI thread only hands it to the atlas.
                        if (const int delay = debug_font_delay_ms()) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                        trace::Stopwatch sw;
                        job->result = sysfont::pick_system_fonts(index.faces(), missing, room, [&index](const FontFace& f) { return index.coverage(f); });
                        const double picked = sw.ms();
                        prefetch_font_files(job->result);
                        trace::log("pick: %zu missing characters -> %zu fonts (%.1f ms to pick, %.1f ms to read the files)", missing.size(), job->result.size(), picked, sw.ms() - picked);
                    } catch (...) {
                    }
                    job->done = true;
                    if (g_glfw_alive) glfwPostEmptyEvent();
                }).detach();
                sys_missing.clear();
                sys_job = job;
            } else if (room == 0) {
                sys_missing.clear();
            }
        }
        {  // Spec 30: while fonts for missing characters are being found, those characters are blank, not boxes
            bool pending = cfg.system_fallback && !g_script_mode && (sys_check || (sys_job && sys_job_fresh));
            for (char32_t c : sys_missing) pending = pending || (cfg.system_fallback && !g_script_mode && !sys_tried.count(c));
            sys_blank = pending;
            set_blank_missing(sys_blank);
        }
        };
        sys_search();  // (and once more right after the first atlas is built, below: the search starts before the first frame is drawn)
        FontFiles want_fonts = font_choice.result.files;
        if (cfg.system_fallback) want_fonts.system = sys_fonts;
        const bool first_build = !fonts_ready;
        if (!fonts_ready || fonts_dark != theme.dark || !(want_fonts == fonts_loaded)) {
            // (Re)build the atlas: new font files, or the dark-mode coverage boost changed.
            const float scale = fonts.scale, body = fonts.body;
            trace::Stopwatch load_sw;
            io.Fonts->Clear();
            fonts = load_fonts(want_fonts, theme.dark);
            fonts_load_ms = load_sw.ms();
            if (!want_fonts.system.empty()) fonts_applied = trace::now_ms();
            fonts.scale = scale;
            fonts.body = body;
            for (const std::string& e : fonts.errors) st.error = e;
            fonts_dark = theme.dark;
            fonts_loaded = want_fonts;
            fonts_ready = true;
            sys_check = true;  // the chains changed: what is missing may have too
            st.cache.key = 0;  // re-measure every block with the new faces
            slides.measured = 0;
            for_waiting([](Tab& t) { t.st.cache.key = 0, t.slides.measured = 0; });
        }
        if (first_build) sys_search();
        float want_scale = dpi * float(cfg.zoom);
        if (slides.on) {  // the slide's own zoom (user zoom is ignored here and kept for normal mode)
            slides_prepare(slides, st, fonts, theme, dpi, cfg.body_size, st.view_w > 0 ? st.view_w : io.DisplaySize.x,
                           st.view_h > 0 ? st.view_h : io.DisplaySize.y);
            want_scale = dpi * slides.z;
        }
        if (pg.on) want_scale = dpi;  // (a page export ignores the zoom too)
        const float want_body = (pg.on && pg.body_pt > 0 ? pg.body_pt : float(cfg.body_size)) * want_scale;
        const bool width_changed = float(cfg.max_width) != g_max_width;  // (config max_width)
        if (want_scale != fonts.scale || want_body != fonts.body || width_changed) {
            // Zoom / text size / column width changed: remember which block is at the top of
            // the view, so draw_document can put it back there.
            const float doc_y = st.scroll_cur - top_pad(fonts.scale);
            const auto& tops = st.cache.top;
            // (The very top of the page stays the very top, with its padding: no anchor then, like the Rust port.)
            if (!slides.on && !pg.on && !st.zoom_anchor && st.scroll_cur >= 1.0f && !tops.empty() && tops.size() == st.cache.height.size()) {
                size_t i = size_t(std::upper_bound(tops.begin(), tops.end(), doc_y) - tops.begin());
                i = i > 0 ? i - 1 : 0;
                const float h = st.cache.height[i];
                const float frac = std::isnan(h) || h <= 0 ? 0.0f : std::clamp((doc_y - tops[i]) / h, 0.0f, 1.0f);
                st.zoom_anchor = {i, frac};
            }
            fonts.scale = want_scale;
            fonts.body = want_body;
            g_max_width = float(cfg.max_width);
        }
        const float doc_scale = fonts.scale;  // pixels per point in the document: DPI x zoom
#ifdef __ANDROID__
        const float scale = dpi;  // the toolbar and widgets keep their size: only the document zooms (spec 35)
#else
        const float scale = doc_scale;  // (the toolbar zooms too, like egui)
#endif
        const std::string style_key = theme.name + "@" + std::to_string(scale);
        if (applied_style != style_key) {
            apply_style(theme, scale);
            platform::set_dark_title_bar(native_window(window), theme.dark);
#ifdef __ANDROID__
            android_shell::set_light_bars(!theme.dark);  // (status bar icons that show on this theme)
#endif
            applied_style = style_key;
        }
        if (!shown) {
            if (!g_script_mode) {  // (script mode: the window is never shown)
                glfwShowWindow(window);  // first frame: now show it
                if (want_maximized) glfwMaximizeWindow(window);
            }
            shown = true;
        }

        // Window title: "file.md — Calligrus".
        const std::string want_key = st.live ? "\x02" + st.live_id + (st.live_open ? "\x02" : "\x03") + to_utf8(st.live_path) : to_utf8(st.path);
        if (title_key != want_key) {
            std::string title = st.path.empty() ? "Calligrus" : to_utf8(st.path.filename()) + " \xE2\x80\x94 Calligrus";
            if (st.live)
                title = (st.live_path.empty() ? st.live_id : to_utf8(st.live_path.filename())) + (st.live_open ? " (live)" : "") + " \xE2\x80\x94 Calligrus";
            glfwSetWindowTitle(window, title.c_str());
            window_title = title;
            title_key = want_key;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        if (g_script_mode) {  // the offscreen target, not the (hidden) window, sets the size
            io.DisplaySize = ImVec2(float(std::lround(script_w * dpi)), float(std::lround(script_h * dpi)));
            if (pg.on) io.DisplaySize = ImVec2(float(std::lround(pg.paper_w * dpi)), float(std::lround(pg.paper_h * dpi)));  // the window is the paper
            io.DisplayFramebufferScale = ImVec2(1, 1);
        }
        text_capture::begin_frame();
        ImGui::NewFrame();
        // egui's widget text is 13 pt: that's the toolbar / outline size.
        ImGui::PushFont(fonts.ui, fonts.px(fonts.regular, 13.0f * scale));

        // --- keyboard ----------------------------------------------------------
        Config& cfg_w = conf.cfg;  // (writable: zoom keys)
        // A zoomed picture (spec 27) takes the keys: Esc closes it, nothing else reacts.
        const bool zoomed = st.lightbox.has_value();
        if (zoomed && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) st.lightbox.reset();
        // An open popup or menu (recent files, PDF menu, theme combo, right-click menu) owns the keys
        // too: j/k/arrows move its selection and must not also scroll the document.
        const bool popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        const bool typing = io.WantTextInput || zoomed || popup_open;  // a text field (find bar, settings) or a popup has focus
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O) && !g_script_mode)
            if (auto p = platform::open_file_dialog(native_window(window))) st.open_request = *p;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O)) ui.open_recent = true;
        // Tabs (spec 34; not in an editor preview, which has none): Ctrl+T opens a file in a new one.
        if (!g_editor && !zoomed && !pg.on) {
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_T) && !g_script_mode)
                if (auto p = platform::open_file_dialog(native_window(window))) st.open_request = *p, st.open_new_tab = true;
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Tab) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_PageDown)) step_tab(1);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Tab) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_PageUp)) step_tab(-1);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_W)) close_tab(active_tab);
            for (int n = 1; n <= 9; n++)  // Ctrl+1..8: that tab, Ctrl+9: the last one
                if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey(ImGuiKey_0 + n)))
                    switch_tab(n == 9 ? tabs.size() - 1 : size_t(n - 1));  // (no such tab: nothing)
        }
        if (!zoomed && ImGui::IsKeyPressed(ImGuiKey_F11, false)) toggle_fullscreen();
        ui.can_export = (st.live || !st.path.empty()) && !export_job;  // (an editor document too: its text is exported)
        if (ui.export_request) {  // the PDF menu / `e` (spec 27, 29): ask where, then export in the background
            const bool pages_request = ui.export_request == 2;
            ui.export_request = 0;
            if (g_script_mode) script_log("export dialog suppressed in script mode (use `export FILE`)");
            else if (ui.can_export) {
                // An editor document: its folder and name; never saved: the working folder, untitled.pdf.
                fs::path suggested = st.path.parent_path() / (st.path.stem().native() + fs::path(".pdf").native());
                if (st.live) {
                    std::error_code cwd_ec;
                    const std::string name = script::editor_pdf_name(st.live_path);
                    suggested = (st.live_path.empty() ? fs::current_path(cwd_ec) : st.live_path.parent_path()) / fs::path(std::u8string(name.begin(), name.end()));
                }
                if (auto out = platform::save_pdf_dialog(native_window(window), suggested)) start_export(*out, pages_request);
            }
        }
        if ((!zoomed && ImGui::IsKeyPressed(ImGuiKey_F5, false)) || ui.slides_request) {  // slides (spec 26)
            ui.slides_request = false;
            set_slides(!slides.on);
        }
        if (slides.want_on && fonts_ready) {
            slides.want_on = false;
            set_slides(true);
        }
        // History: Alt+Left / Alt+Right and the mouse's back / forward buttons (always on).
        if (io.KeyAlt && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) go_history(true, 1);
        if (io.KeyAlt && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) go_history(false, 1);
        if (ImGui::IsMouseClicked(3)) go_history(true, 1);
        if (ImGui::IsMouseClicked(4)) go_history(false, 1);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_R)) st.reload();

        // Zoom (spec 1): Ctrl+= / Ctrl++ / numpad +, Ctrl+-, Ctrl+0, Ctrl+wheel.
        int zoom_steps = 0;
        bool zoom_reset = false;
        if (io.KeyCtrl && !slides.on) {
            if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) zoom_steps++;
            if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) zoom_steps--;
            if (ImGui::IsKeyPressed(ImGuiKey_0, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad0, false)) zoom_reset = true;
            if (io.MouseWheel != 0 && !ImGui::IsAnyItemActive()) {
                wheel_zoom += io.MouseWheel;
                const int notches = int(wheel_zoom);  // toward zero: whole notches only
                zoom_steps += notches;
                wheel_zoom -= float(notches);
            }
        }
        if (zoom_steps != 0 || zoom_reset) {
            const double z = zoom_reset ? 1.0 : nav::step_zoom(float(cfg_w.zoom), zoom_steps);
            if (z != cfg_w.zoom) {
                cfg_w.zoom = z;
                conf.queue({{"zoom", toml_float(z)}}, now);
            }
        }

#ifdef __ANDROID__
        {  // Fingers on the document (spec 35): a drag and a fling scroll, a pinch zooms.
            g_touch_ui_only = ui.settings_open;
            android_shell::set_raw_touch(ui.settings_open);
            const android_shell::DocTouch t = android_shell::doc_touch();
            const bool doc_view = !slides.on && !pg.on;
            // A pinch only scales the drawn document (draw_document), about the fingers; the layout at the new
            // zoom happens once, when the fingers lift (and the block under them is put back under them).
            bool pinch_ended = false;
            if (doc_view && t.pinching) {
                g_pstat.on = true;
            } else if (g_pstat.on) {
                __android_log_print(ANDROID_LOG_INFO, "calligrus", "pinch %s: %d frames, %.1f ms of work per frame (worst %.1f), %.1f ms between frames",
                                    pinch_legacy() ? "legacy relayout" : "scaled picture", g_pstat.frames,
                                    g_pstat.frames ? g_pstat.work / g_pstat.frames * 1000 : 0.0, g_pstat.worst * 1000,
                                    g_pstat.frames ? g_pstat.wall / g_pstat.frames * 1000 : 0.0);
                g_pstat = PinchStats();
            }
            if (pinch_legacy() && doc_view) {
                if (t.zoom != 1.0f) {
                    const double z = std::round(std::clamp(double(cfg_w.zoom) * double(t.zoom), double(nav::kZoomMin), double(nav::kZoomMax)) * 100.0) / 100.0;
                    if (z != cfg_w.zoom) cfg_w.zoom = z, conf.queue({{"zoom", toml_float(z)}}, now);
                }
            } else if (doc_view && t.pinching) {
                if (!g_pinch.active) g_pinch = {true, 1.0f, ImVec2(t.focus_x, t.focus_y), ImVec2(t.focus_x, t.focus_y), cfg_w.zoom};
                g_pinch.s = std::clamp(g_pinch.s * t.zoom, float(nav::kZoomMin / g_pinch.z0), float(nav::kZoomMax / g_pinch.z0));
                g_pinch.to = ImVec2(t.focus_x, t.focus_y);
            } else if (g_pinch.active) {
                pinch_ended = true;
                g_pinch.active = false;
                const double z = std::round(std::clamp(g_pinch.z0 * double(g_pinch.s), double(nav::kZoomMin), double(nav::kZoomMax)) * 100.0) / 100.0;
                const auto& tops = st.cache.top;
                if (z != cfg_w.zoom && !tops.empty() && tops.size() == st.cache.height.size()) {
                    const float doc_y = st.scroll_cur + (g_pinch.from.y - st.view_y) - top_pad(fonts.scale);
                    size_t i = size_t(std::upper_bound(tops.begin(), tops.end(), doc_y) - tops.begin());
                    i = i > 0 ? i - 1 : 0;
                    const float h = st.cache.height[i];
                    const float frac = std::isnan(h) || h <= 0 ? 0.0f : std::clamp((doc_y - tops[i]) / h, 0.0f, 1.0f);
                    st.zoom_anchor = {i, frac};
                    st.zoom_anchor_off = g_pinch.to.y - st.view_y;
                    st.zoom_anchor_scale = dpi * float(z);
                }
                if (z != cfg_w.zoom) {
                    cfg_w.zoom = z;
                    conf.queue({{"zoom", toml_float(z)}}, now);
                }
            }
            st.pinch_s = 1.0f;
            if (g_pinch.active || pinch_ended) {  // (the frame the fingers lift still draws the scaled picture: the layout comes next frame)
                st.pinch_s = g_pinch.s, st.pinch_from = g_pinch.from, st.pinch_to = g_pinch.to;
            }
            auto scroll_by = [&](float dy) {  // returns how far the content really moved
                if (dy == 0) return 0.0f;
                const float max_scroll = std::max(0.0f, st.content_h - st.view_h);
                const float to = st.scroll_target - dy;
                if (to < 0 || to > max_scroll) android_shell::doc_touch_hit_end();  // a fling stops at the ends
                const float at = std::clamp(to, 0.0f, max_scroll);
                const float moved = st.scroll_target - at;
                st.scroll_target = st.scroll_cur = at;  // 1:1 with the finger: no easing
                return moved;
            };
            if (doc_view && t.h_key && t.h_dx != 0 && !g_pinch.active) {  // a sideways drag / fling of a wide block
                bool end = false;
                render_scroll_h(st.cache, static_cast<const char*>(t.h_key), t.h_dx, &end);
                if (end) android_shell::doc_touch_hit_end_h();
            }
            static float fling_scrolled = 0, fling_v = 0, fling_total = 0;
            static bool fling_seen = false;
            if (doc_view && !g_pinch.active && !pinch_ended) {
                scroll_by(t.scroll_dy - t.fling_dy);
                fling_scrolled -= scroll_by(t.fling_dy);  // (moved is negative when the content goes up the page)
            }
            if (t.fling_active) fling_seen = true, fling_v = t.fling_v, fling_total = t.fling_total;
            else if (fling_seen) {  // the fling is over: what it was meant to cover, and what it did
                __android_log_print(ANDROID_LOG_INFO, "calligrus", "fling: lift %.0f px/s, AOSP distance %.1f px, scrolled %.1f px", fling_v, std::fabs(fling_total), std::fabs(fling_scrolled));
                fling_seen = false, fling_scrolled = 0;
            }
        }
#endif

        // Navigation keys (spec 2): vim keys and the standard ones.
        const float line = 1.55f * fonts.body;
        auto run = [&](const nav::Command& cmd) {
            using nav::Action;
            const float page = std::max(line, st.view_h - 2 * line);
            switch (cmd.action) {
                case Action::LineDown: st.scroll_target += 3 * line * cmd.count; break;
                case Action::LineUp: st.scroll_target -= 3 * line * cmd.count; break;
                case Action::HalfPageDown: st.scroll_target += st.view_h / 2 * cmd.count; break;
                case Action::HalfPageUp: st.scroll_target -= st.view_h / 2 * cmd.count; break;
                case Action::PageDown: st.scroll_target += page * cmd.count; break;
                case Action::PageUp: st.scroll_target -= page * cmd.count; break;
                case Action::OneLineDown: st.scroll_target += line * cmd.count; break;
                case Action::OneLineUp: st.scroll_target -= line * cmd.count; break;
                case Action::Quit: glfwSetWindowShouldClose(window, GLFW_TRUE); break;
                case Action::NextTab: step_tab(cmd.count); break;
                case Action::ThemePicker: if (!slides.on && !pg.on) ui.theme_picker.open_request = true; break;
                case Action::PrevTab: step_tab(-cmd.count); break;
                case Action::HistoryBack: go_history(true, cmd.count); break;
                case Action::HistoryForward: go_history(false, cmd.count); break;
                case Action::SetMark: set_mark(char(cmd.mark)); break;
                case Action::GoMark: go_mark(char(cmd.mark)); break;
                case Action::FindWordNext: find_word(1); break;
                case Action::FindWordPrev: find_word(-1); break;
                case Action::Top: st.scroll_target = 0; break;
                case Action::Bottom: st.scroll_target = 1e9f; break;  // clamped to the end
                case Action::NextHeading:
                case Action::PrevHeading: {
                    std::vector<float> tops;
                    for (const OutlineEntry& e : outline) tops.push_back(heading_scroll(e, doc_scale));
                    std::sort(tops.begin(), tops.end());
                    const int n = cmd.action == Action::NextHeading ? cmd.count : -cmd.count;
                    if (auto y = nav::heading_target(tops, st.scroll_target, n)) st.scroll_target = *y;
                    break;
                }
                case Action::OpenFind:
                    // Ctrl+F: browser style. `/`: vim style (spec 7): the search starts here, and comes back with the last query selected.
                    find.vim = !io.KeyCtrl;
                    if (find.vim) {
                        auto slash = std::find(io.InputQueueCharacters.begin(), io.InputQueueCharacters.end(), ImWchar('/'));
                        if (slash != io.InputQueueCharacters.end()) io.InputQueueCharacters.erase(slash);  // (the field must not get the key as text)
                        find.origin = st.scroll_cur;
                        find.repick = true;
                        find.select_all = true;
                        if (find.buf[0] == 0 && !find.last_query.empty()) {
                            std::snprintf(find.buf, sizeof(find.buf), "%s", find.last_query.c_str());
                            find.typed = find.buf;
                        }
                    }
                    find.open = true;
                    find.focus = true;
                    break;
                case Action::NextMatch:
                case Action::PrevMatch: {
                    const int n = int(find.matches.size());
                    if (n > 0) {
                        const int step = (cmd.action == Action::NextMatch ? 1 : n - 1) * (cmd.count % n);
                        find.current = (find.current + step) % n;
                        find.scroll_to_current = true;
                    }
                    break;
                }
                case Action::ToggleOutline: ui.show_outline = !ui.show_outline; break;
                case Action::Reload: st.reload(); break;
                case Action::Escape:
                    // One thing at a time (pending keys were already cancelled):
                    // autoscroll, then the find bar, then the selection.
                    if (autoscroll.active()) autoscroll.escape();
                    else if (find.open) find.close();
                    else st.sel.clear();
                    break;
            }
        };
        if (!typing && slides.on && slides.overview) {
            // The overview's keys (spec 26): arrows / hjkl pick, Enter / Space / `o` show it, Esc goes back.
            const size_t n_slides = std::max<size_t>(slides.ranges.size(), 1);
            bool pick = false;
            auto move = [&](int dx, int dy) { slides.overview_sel = slides::overview_move(slides.overview_sel, dx, dy, slides.overview_cols, n_slides); };
            if (!io.KeyCtrl && !io.KeyAlt) {
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) move(1, 0);
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) move(-1, 0);
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move(0, 1);
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move(0, -1);
                if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) slides.overview_sel = 0;
                if (ImGui::IsKeyPressed(ImGuiKey_End, false)) slides.overview_sel = n_slides - 1;
                for (ImGuiKey k : {ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space})
                    if (ImGui::IsKeyPressed(k, false)) pick = true;
                for (ImWchar c : io.InputQueueCharacters) {
                    if (c == 'l') move(1, 0);
                    else if (c == 'h') move(-1, 0);
                    else if (c == 'j') move(0, 1);
                    else if (c == 'k') move(0, -1);
                    else if (c == 'g') slides.overview_sel = 0;
                    else if (c == 'G') slides.overview_sel = n_slides - 1;
                    else if (c == 'o') pick = true;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) slides.overview = false;
            if (pick) {
                slides.overview = false;
                slides.index = slides.overview_sel, slides.step = SIZE_MAX, slides.scroll = 0;  // (slides_prepare clamps the step)
            }
            extra_frames = std::max(extra_frames, 2);
        } else if (!typing && slides.on) {
            // Slide keys (spec 26); Ctrl chords and `/` still go to the vim machine (find).
            int step = 0;
            bool to_first = false, to_last = false;
            if (!io.KeyCtrl && !io.KeyAlt) {
                for (ImGuiKey k : {ImGuiKey_RightArrow, ImGuiKey_DownArrow, ImGuiKey_Space, ImGuiKey_PageDown, ImGuiKey_Enter, ImGuiKey_KeypadEnter})
                    if (ImGui::IsKeyPressed(k)) step++;
                for (ImGuiKey k : {ImGuiKey_LeftArrow, ImGuiKey_UpArrow, ImGuiKey_Backspace, ImGuiKey_PageUp})
                    if (ImGui::IsKeyPressed(k)) step--;
                if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) to_first = true;
                if (ImGui::IsKeyPressed(ImGuiKey_End, false)) to_last = true;
                for (ImWchar c : io.InputQueueCharacters) {
                    if (c == 'l' || c == 'j' || c == 'n') step++;
                    else if (c == 'h' || c == 'k' || c == 'p') step--;
                    else if (c == 'g') to_first = true;
                    else if (c == 'G') to_last = true;
                    else if (c == 't') slides.clock_shown = !slides.clock_shown;
                    else if (c == 'N') slides.notes_shown = !slides.notes_shown;
                    else if (c == 'o') slides.overview = true, slides.overview_sel = slides.index, slides.overview_followed = SIZE_MAX;
                    else if (c == 's') slides.presenter = !slides.presenter;
                    else if (c == 'e') ui.export_request = 1;
                }
            }
            // Next / previous: a step first (pauses), then a slide; back onto a slide shows all of it.
            const size_t n_slides = std::max<size_t>(slides.ranges.size(), 1);
            const size_t was_index = slides.index, was_step = slides.step;
            auto steps_here = [&] { return slides.index < slides.steps.size() ? slides.steps[slides.index].size() : size_t(0); };
            for (; step > 0; step--) {
                if (slides.step < steps_here()) slides.step++;
                else if (slides.index + 1 < n_slides) slides.index++, slides.step = 0;
            }
            for (; step < 0; step++) {
                if (slides.step > 0) slides.step--;
                else if (slides.index > 0) slides.index--, slides.step = steps_here();
            }
            if (to_first) slides.index = 0, slides.step = 0;
            if (to_last) slides.index = n_slides - 1, slides.step = 0;
            if (slides.index != was_index) slides.scroll = 0;
            if (slides.index != was_index || slides.step != was_step) extra_frames = std::max(extra_frames, 3);
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                if (autoscroll.active()) autoscroll.escape();
                else if (find.open) find.close();
                else set_slides(false);
            }
            for (const nav::KeyInput& k : collect_keys())
                if ((k.ctrl || k.ch == U'/') && k.named == nav::Named::None)
                    if (auto cmd = vim.feed(k, now, cfg.vim_keys)) run(*cmd);
        } else if (!typing) {
            for (const nav::KeyInput& k : collect_keys())
                if (auto cmd = vim.feed(k, now, cfg.vim_keys)) run(*cmd);
        } else {
            vim.reset();
            // In a text field only these still work.
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) find.focus = true, find.vim = false;  // (browser style from now on)
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_B)) ui.show_outline = !ui.show_outline;
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) && find.open && !ui.theme_picker.active) find.close();  // (Esc belongs to the theme picker)
        }

        // --- mouse: autoscroll (spec 3) -----------------------------------------
        bool block_press = false;  // a press that ended toggle-mode autoscroll does nothing else
        if (autoscroll.active()) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                block_press = autoscroll.toggle_mode();
                autoscroll.other_button_pressed();
            }
            if (io.MouseWheel != 0) autoscroll.wheel();
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle)) autoscroll.middle_released(ImGui::GetTime());
        autoscroll.mouse_moved(io.MousePos.x, io.MousePos.y);
        if (autoscroll.active()) {
            st.scroll_now += autoscroll.scroll_delta(io.MousePos.y, std::min(io.DeltaTime, 0.1f));
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }

        // --- the window --------------------------------------------------------
        // One window covering the whole OS window.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 work_pos = vp->WorkPos, work_size = vp->WorkSize;
#ifdef __ANDROID__  // the system bars and the camera cut-out are not ours: the window sits inside them
        {
            const android_shell::Insets in = android_shell::insets();
            work_pos.x += in.left, work_pos.y += in.top;
            work_size.x -= in.left + in.right, work_size.y -= in.top + in.bottom;
            ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(io.DisplaySize.x, work_pos.y), ImGui::GetColorU32(ImGuiCol_WindowBg));  // (under the status bar)
        }
#endif
#ifdef __ANDROID__
        {  // a narrow screen (a phone held upright, under 600 dp) starts with the outline hidden: it would take half of it
            static bool outline_decided = false;
            if (!outline_decided && work_size.x > 0) {
                outline_decided = true;
                if (work_size.x < 600.0f * dpi) ui.show_outline = false;
            }
        }
#endif
        ImGui::SetNextWindowPos(work_pos);
        ImGui::SetNextWindowSize(work_size);
        // The full-window panel: no padding, no border, square corners.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(3);
        ImDrawList* frame_dl = ImGui::GetWindowDrawList();
        const ImU32 sep = ImGui::GetColorU32(ImGuiCol_Separator);

        // Top panel: toolbar (egui panel margins: 8px sides, 2px top/bottom).
        const float pad_x = std::round(8.0f * scale), pad_y = std::round(2.0f * scale);
        // (+2: egui's panel is two pixels taller than its buttons plus margins; measured with the visual suite.)
        const bool bare = slides.on || pg.on;  // (slides and the page export have no panels)
        const float bar_h = bare ? 0.0f : ImGui::GetFrameHeight() + 2 * pad_y + std::floor(2.0f * scale);  // (slides: no toolbar)
        if (!bare) {
            ImGui::SetCursorPos(ImVec2(pad_x, pad_y));
            draw_toolbar(st, ui, conf, themes, theme, fonts, window, scale, now);
        }
        // The tab bar (spec 34): a second panel as tall as the toolbar, with 2 or more tabs.
        const bool show_tabs = !bare && !g_editor && tabs.size() > 1;
        const float tab_h = show_tabs ? bar_h : 0.0f;
        TabBarResult tab_click;
        if (show_tabs) {
            std::vector<TabLabel> labels;
            for (size_t i = 0; i < tabs.size(); i++) {
                const fs::path& p = tab_doc(i).path;
                labels.push_back(p.empty() ? TabLabel{"demo", "the built-in demo"} : TabLabel{tab_name(to_utf8(p.filename())), display_path(p)});
            }
            tab_click = draw_tab_bar(labels, active_tab, ui, theme, fonts, work_pos.y + bar_h, tab_h, pad_x, pad_y, scale);
            const float tab_line_y = std::round(work_pos.y + bar_h + tab_h) - 1.0f;
            frame_dl->AddRectFilled(ImVec2(work_pos.x, tab_line_y), ImVec2(work_pos.x + work_size.x, tab_line_y + 1.0f), sep);
        }
        ImGui::SetCursorPos(ImVec2(0, bar_h + tab_h));
        // A whole-pixel rect, not AddLine: a 1 px AA line that straddles two pixel rows comes out half as dark.
        const float line_y = std::round(work_pos.y + bar_h) - 1.0f;
        if (!bare) frame_dl->AddRectFilled(ImVec2(work_pos.x, line_y), ImVec2(work_pos.x + work_size.x, line_y + 1.0f), sep);

        // Optional error row (like the Rust version's error panel).
        if (!st.error.empty()) {
            ImGui::SetCursorPosX(pad_x);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.error);
            ImGui::TextUnformatted(st.error.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton("dismiss")) st.error.clear();
        }
        // Find bar row.
        float find_rule_y = -1;  // its bottom rule, drawn after the document child (whose background would cover it)
        if (find.open) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::round(8.0f * scale));  // the central panel's margin
            draw_find_bar(find, theme, pad_x, scale);
            // (rows measured against the Rust port: rule 5 px under the field, document 46 px below the rule)
            const float y = ImGui::GetCursorScreenPos().y + pad_y + std::round(1.0f * scale);
            find_rule_y = std::round(y) - 1.0f;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::round(1.0f * scale));
        }

        // Left panel: outline.
        if (!bare && ui.show_outline && !outline.empty()) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad_x, 2.0f * scale));
            ImGui::BeginChild("outline", ImVec2(230 * scale, 0), ImGuiChildFlags_ResizeX | ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PopStyleVar();
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale, 4 * scale));  // egui label rows
            draw_outline(st, outline, theme, scale);
            ImGui::PopStyleVar();
            ImVec2 wmin = ImGui::GetWindowPos();
            const float wx = std::round(wmin.x + ImGui::GetWindowWidth()) - 1.0f;  // whole pixel column (a 1 px AA line would be half as dark)
            ImGui::EndChild();
            frame_dl->AddRectFilled(ImVec2(wx, wmin.y), ImVec2(wx + 1.0f, work_pos.y + work_size.y), sep);
            ImGui::SameLine(0, 0);
        }
        poll_export();
        if (st.marks_dirty) {
            st.marks_dirty = false;
            refresh_mark_margin();
        }
        bool animate = false;
        if (slides.on) {
            st.restore_y = -1;  // (a newly opened file's saved scroll offset means nothing here)
            st.sync_pending = st.sync_reapply = false;
            if (slides.presenter && g_script_mode) {  // (scripts can't open windows: the view takes the main one's place)
                ImGui::BeginChild("presenter", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                if (slides.overview)
                    draw_overview_grid(ImGui::GetWindowDrawList(), st, slides, fonts, theme, dpi, dpi, ImGui::GetCursorScreenPos(), ImGui::GetWindowWidth(),
                                       ImGui::GetWindowHeight(), std::nullopt);
                else
                    draw_presenter(ImGui::GetWindowDrawList(), st, slides, fonts, theme, dpi, dpi, ImGui::GetCursorScreenPos(), ImGui::GetWindowWidth(),
                                   ImGui::GetWindowHeight(), 0);
                ImGui::EndChild();
            } else if (slides.overview && !slides.presenter) {  // (with the presenter window open it is there: the audience keeps the slide)
                if (const int k = draw_overview(st, slides, fonts, theme, dpi); k >= 0) {
                    slides.overview = false;
                    slides.index = size_t(k), slides.step = SIZE_MAX, slides.scroll = 0;
                    extra_frames = std::max(extra_frames, 3);
                }
            } else {
                outline = draw_slide(st, slides, find, autoscroll, fonts, theme, cfg.code_line_numbers, block_press, animate, dpi);
            }
        } else if (pg.on) {
            st.restore_y = -1;  // (scrolling is not ours here: forget what a newly opened file asked for)
            st.zoom_anchor.reset();
            st.scroll_to = -1;
            st.sync_pending = st.sync_reapply = false;
            st.pending_anchor.clear();
            draw_page(st, pg, fonts, theme);
        } else {
            outline = draw_document(st, find, autoscroll, fonts, theme, cfg.code_line_numbers, cfg.editor_marker, block_press, animate);
        }
        if (find_rule_y >= 0)
            ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(work_pos.x, find_rule_y), ImVec2(work_pos.x + work_size.x, find_rule_y + 1.0f), sep);
        if (animate) extra_frames = std::max(extra_frames, 2);

        // Anchor jump (the outline now belongs to the document just drawn).
        if (!st.pending_anchor.empty()) {
            std::vector<std::string> titles;
            for (const OutlineEntry& e : outline) titles.push_back(e.title);
            const auto slugs = session::unique_slugs(titles);
            auto it = std::find(slugs.begin(), slugs.end(), st.pending_anchor);
            // (slides: the slide holding that block; the heading's block is found by its y)
            auto show_block = [&](size_t block) {
                slides.index = slides::slide_of_block(st.split(), block);
                slides.scroll = 0;
            };
            if (it != slugs.end() && slides.on) {
                const auto& tops = st.cache.top;
                const float y = outline[size_t(it - slugs.begin())].y;
                const size_t b = size_t(std::upper_bound(tops.begin(), tops.end(), y + 0.5f) - tops.begin());
                show_block(b > 0 ? b - 1 : 0);
            } else if (it != slugs.end()) {
                st.scroll_to = heading_scroll(outline[size_t(it - slugs.begin())], doc_scale);
            } else if (int fb = footnote_block(st.doc, st.pending_anchor); fb >= 0 && size_t(fb) < st.cache.top.size()) {
                if (slides.on) show_block(size_t(fb));
                else st.scroll_to = std::max(0.0f, st.cache.top[size_t(fb)] - 8.0f * doc_scale);
            }
            st.pending_anchor.clear();
            extra_frames = std::max(extra_frames, 3);
        }
        // Requests made while drawing: the Open button, a clicked link.
        if (!st.open_request.empty()) {
            const fs::path req = std::move(st.open_request);
            st.open_request.clear();
            open_file(req, "", st.open_new_tab);
            st.open_new_tab = false;
        }
        // The tab bar's clicks (applied now, not mid-frame: they change what the frame is drawing).
        if (tab_click.close >= 0) close_tab(size_t(tab_click.close));
        else if (tab_click.switch_to >= 0) switch_tab(size_t(tab_click.switch_to));
        if (ui.clear_recent) {
            ui.clear_recent = false;
            state.recent.clear();
            save_state_guarded(state_file, state, conf.cfg.remember_window);
        }
        if (!st.link_request.empty()) {
            const std::string url = std::move(st.link_request);
            st.link_request.clear();
            const bool link_new_tab = st.link_new_tab && !g_editor;  // Ctrl+click / middle-click: a file opens in a new tab (spec 34)
            st.link_new_tab = false;
            const session::LinkAction act = session::resolve_link(url, base_dir(st));
            using Kind = session::LinkAction::Kind;
            switch (act.kind) {
                case Kind::Anchor: {
                    std::vector<std::string> titles;
                    for (const OutlineEntry& e : outline) titles.push_back(e.title);
                    const auto slugs = session::unique_slugs(titles);
                    if (std::find(slugs.begin(), slugs.end(), act.anchor) != slugs.end() || footnote_block(st.doc, act.anchor) >= 0) navigate(st.path, act.anchor, true, std::nullopt);
                    break;
                }
                case Kind::OpenMarkdown: open_file(act.path, act.anchor, link_new_tab); break;
                case Kind::OpenExternal:
                    if (g_script_mode) script_log("link (not launched): " + (act.url.empty() ? to_utf8(act.path) : act.url));
                    else if (act.url.empty()) platform::open_path(act.path);
                    else platform::open_url(act.url);
                    break;
                case Kind::Missing: st.error = "Link target not found: " + display_path(act.path); break;
            }
            extra_frames = std::max(extra_frames, 3);
        }

        ImGui::End();

        // Settings window (spec 9) and the autoscroll marker on top.
        conf.queue(draw_settings(ui.settings_open, conf.cfg, themes, font_index, native_window(window), scale), now);
        if (autoscroll.active()) draw_autoscroll_marker(autoscroll, theme, scale);
        draw_lightbox(st, fonts, theme, scale);
        ImGui::PopFont();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        if (g_script_mode) {
            w = int(io.DisplaySize.x), h = int(io.DisplaySize.y);
            if (!offscreen.use(w, h)) {
                script_log("offscreen framebuffer is incomplete", true);
                script_exit = 1;
                break;
            }
        }
        glViewport(0, 0, w, h);
        ImVec4 bg = ImGui::ColorConvertU32ToFloat4(theme.bg);
        glClearColor(bg.x, bg.y, bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        debug_capture(w, h);
        if (g_script_mode) {
            script_idle = !st.animating && !autoscroll.active() && extra_frames == 0 && st.restore_y < 0 && st.pending_anchor.empty() &&
                          st.scroll_to < 0 && !st.sync_pending && !st.sync_reapply && fonts_ready && !sys_busy(conf.cfg) &&
                          st.bar_wake == 0;  // `settle` waits for the scrollbar to hide again (spec 24)
            if (!script_idle) std::this_thread::sleep_for(std::chrono::milliseconds(8));  // animations use real time
        } else {
            glfwSwapBuffers(window);
#ifdef __ANDROID__
            shown_frames++;
            android_idle = !st.animating && !autoscroll.active() && st.restore_y < 0 && st.pending_anchor.empty() && st.scroll_to < 0 && !st.sync_pending &&
                           !st.sync_reapply && fonts_ready && !(conf.cfg.system_fallback && (sys_check || !sys_missing.empty())) && !st.zoom_anchor;
            android_shell::count_frame();
#endif
            if (!first_frame_logged && fonts_ready) {
                first_frame_logged = true;
                trace::log("first frame shown at %.1f ms (the frame took %.1f ms; blank characters: %s)", trace::now_ms(), trace::now_ms() - frame_ms, sys_blank ? "yes" : "no");
            }
            if (fonts_applied >= 0) {  // the frame in which new system fonts were put in the atlas: it also laid the text out again
                trace::log("fonts applied: atlas rebuilt in %.1f ms (font files, FreeType), the frame with the new fonts (relayout and draw) took %.1f ms in all",
                           fonts_load_ms, trace::now_ms() - frame_ms);
                fonts_applied = -1;
            }
#ifdef __ANDROID__
            if (g_pstat.on) {
                const double end = glfwGetTime();
                g_pstat.frames++;
                g_pstat.work += end - now;
                g_pstat.worst = std::max(g_pstat.worst, end - now);
                if (g_pstat.last_end > 0) g_pstat.wall += end - g_pstat.last_end;
                g_pstat.last_end = end;
            }
#endif
            // The presenter window: opened / closed with `s` (and slides mode), drawn after the main one.
            const bool want_presenter = slides.on && slides.presenter;
            if (want_presenter && !presenter.win) presenter_open();
            if (presenter.win && glfwWindowShouldClose(presenter.win)) slides.presenter = false;  // its close button
            if (presenter.win && !(slides.on && slides.presenter)) presenter_close();
            if (presenter.win) presenter_frame(fonts, theme, ImGui::GetTime() - slides.since);
            pacer.pace();
        }
    }
#ifdef __ANDROID__
    android_shell::set_background_hook(nullptr);
#endif
    presenter_close();
    conf.flush(glfwGetTime(), true);  // don't lose a change made just before closing
    save_state_file();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    g_glfw_alive = false;  // the font scan may still be running: it must not wake GLFW any more
    glfwTerminate();
    if (exporting && script_exit == 0) {
        if (export_pdf) {  // a page is the window's size at 96 pixels per inch: 0.75 pt per pixel, over the scale
            std::string why;
            const std::string doc = pdf::write(export_pages, 0.75f / exporting->scale, text_capture::fonts(), &why);  // (pages carry their own size in points)
            if (doc.empty() && text_capture::enabled()) {
                // The text layer could not be built (a font would not subset): the pictures have no text,
                // so draw everything again, the old way (spec 33: the automatic fallback).
                script_log("export: the PDF's text layer failed (" + why + "); exporting as pictures instead (--raster)");
                std::vector<fs::path> argv = {platform::exe_path()};
                for (const fs::path& a : platform::args()) argv.push_back(a);
                argv.push_back(fs::path("--raster"));
                std::string text;
                const int code = platform::run_and_capture(argv, text);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
                if (!text.empty()) script_log(text, code != 0);
                return code;
            }
            std::ofstream out(script_out, std::ios::binary);
            if (doc.empty() || !out.write(doc.data(), std::streamsize(doc.size()))) return script_log("export: cannot write " + to_utf8(script_out), true), 1;
        }
        script_log("exported " + std::to_string(export_total) + (exporting->pages ? " pages to " : " slides to ") + to_utf8(script_out));
    }
    return script_exit;
}
