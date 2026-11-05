// The window layer of the Android build. main.cpp is written against the small part of GLFW it uses
// (window, GL context, event loop, clock); on the desktop that is GLFW itself, on Android this header
// declares the same names, implemented in window_android.cpp over GameActivity and EGL / OpenGL ES 3.
// Things a phone or tablet has no use for (monitors, window rectangles, a second window) are stubs.
//
// Besides the GLFW subset there is `android_shell`: what only the Android build needs (screen insets,
// where touches belong to the document, what the fingers did this frame).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <GLES3/gl3.h>
#include <imgui.h>

struct GLFWwindow;
struct GLFWmonitor;
struct GLFWvidmode {
    int width, height, redBits, greenBits, blueBits, refreshRate;
};
struct GLFWimage {
    int width, height;
    unsigned char* pixels;
};

#define GLFW_TRUE 1
#define GLFW_FALSE 0
#define GLFW_DONT_CARE (-1)
#define GLFW_VISIBLE 0x00020004
#define GLFW_MAXIMIZED 0x00020008
#define GLFW_ICONIFIED 0x00020002
#define GLFW_AUTO_ICONIFY 0x00020006
#define GLFW_CONTEXT_VERSION_MAJOR 0x00022002
#define GLFW_CONTEXT_VERSION_MINOR 0x00022003
#define GLFW_OPENGL_PROFILE 0x00022008
#define GLFW_OPENGL_CORE_PROFILE 0x00032001

using GLFWerrorfun = void (*)(int, const char*);
using GLFWcursorposfun = void (*)(GLFWwindow*, double, double);
using GLFWmousebuttonfun = void (*)(GLFWwindow*, int, int, int);
using GLFWscrollfun = void (*)(GLFWwindow*, double, double);
using GLFWkeyfun = void (*)(GLFWwindow*, int, int, int, int);
using GLFWcharfun = void (*)(GLFWwindow*, unsigned);
using GLFWwindowsizefun = void (*)(GLFWwindow*, int, int);
using GLFWwindowfocusfun = void (*)(GLFWwindow*, int);
using GLFWdropfun = void (*)(GLFWwindow*, int, const char**);
using GLFWcursorenterfun = void (*)(GLFWwindow*, int);
using GLFWwindowrefreshfun = void (*)(GLFWwindow*);
using GLFWglproc = void (*)();

// --- the part that does work ---
int glfwInit();
void glfwTerminate();
GLFWwindow* glfwCreateWindow(int width, int height, const char* title, GLFWmonitor*, GLFWwindow*);
void glfwDestroyWindow(GLFWwindow*);
void glfwMakeContextCurrent(GLFWwindow*);
void glfwSwapInterval(int);
void glfwSwapBuffers(GLFWwindow*);
void glfwGetFramebufferSize(GLFWwindow*, int* w, int* h);
void glfwGetWindowSize(GLFWwindow*, int* w, int* h);
void glfwGetWindowContentScale(GLFWwindow*, float* x, float* y);
int glfwWindowShouldClose(GLFWwindow*);
void glfwSetWindowShouldClose(GLFWwindow*, int);
void glfwPollEvents();
void glfwWaitEventsTimeout(double seconds);
void glfwPostEmptyEvent();
double glfwGetTime();
GLFWglproc glfwGetProcAddress(const char* name);
GLFWerrorfun glfwSetErrorCallback(GLFWerrorfun);
GLFWwindowsizefun glfwSetWindowSizeCallback(GLFWwindow*, GLFWwindowsizefun);
GLFWdropfun glfwSetDropCallback(GLFWwindow*, GLFWdropfun);
GLFWcursorposfun glfwSetCursorPosCallback(GLFWwindow*, GLFWcursorposfun);
GLFWmousebuttonfun glfwSetMouseButtonCallback(GLFWwindow*, GLFWmousebuttonfun);
GLFWscrollfun glfwSetScrollCallback(GLFWwindow*, GLFWscrollfun);
GLFWkeyfun glfwSetKeyCallback(GLFWwindow*, GLFWkeyfun);
GLFWcharfun glfwSetCharCallback(GLFWwindow*, GLFWcharfun);
GLFWwindowfocusfun glfwSetWindowFocusCallback(GLFWwindow*, GLFWwindowfocusfun);

// --- stubs: one full-screen window, no monitors, no title bar ---
inline void glfwWindowHint(int, int) {}
inline void glfwSetWindowIcon(GLFWwindow*, int, const GLFWimage*) {}
inline void glfwShowWindow(GLFWwindow*) {}
inline void glfwMaximizeWindow(GLFWwindow*) {}
inline void glfwSetWindowTitle(GLFWwindow*, const char*) {}
inline void glfwSetWindowAttrib(GLFWwindow*, int, int) {}
inline int glfwGetWindowAttrib(GLFWwindow*, int attrib) { return attrib == GLFW_MAXIMIZED ? 1 : 0; }
inline void glfwGetWindowPos(GLFWwindow*, int* x, int* y) { *x = *y = 0; }
inline void glfwSetWindowPos(GLFWwindow*, int, int) {}
inline void glfwSetWindowSize(GLFWwindow*, int, int) {}
inline void glfwGetWindowFrameSize(GLFWwindow*, int* l, int* t, int* r, int* b) { *l = *t = *r = *b = 0; }
inline GLFWmonitor** glfwGetMonitors(int* count) { return *count = 0, nullptr; }
inline GLFWmonitor* glfwGetPrimaryMonitor() { return nullptr; }
inline void glfwGetMonitorWorkarea(GLFWmonitor*, int* x, int* y, int* w, int* h) { *x = *y = *w = *h = 0; }
inline const GLFWvidmode* glfwGetVideoMode(GLFWmonitor*) { return nullptr; }
inline void glfwSetWindowMonitor(GLFWwindow*, GLFWmonitor*, int, int, int, int, int) {}
inline GLFWcursorenterfun glfwSetCursorEnterCallback(GLFWwindow*, GLFWcursorenterfun) { return nullptr; }
inline GLFWwindowrefreshfun glfwSetWindowRefreshCallback(GLFWwindow*, GLFWwindowrefreshfun) { return nullptr; }

// ImGui's platform backend (imgui_impl_glfw's names): this layer feeds ImGui's input itself.
bool ImGui_ImplGlfw_InitForOpenGL(GLFWwindow*, bool install_callbacks);
void ImGui_ImplGlfw_NewFrame();
void ImGui_ImplGlfw_Shutdown();
inline void ImGui_ImplGlfw_KeyCallback(GLFWwindow*, int, int, int, int) {}
inline void ImGui_ImplGlfw_CharCallback(GLFWwindow*, unsigned) {}
inline void ImGui_ImplGlfw_MouseButtonCallback(GLFWwindow*, int, int, int) {}
inline void ImGui_ImplGlfw_CursorPosCallback(GLFWwindow*, double, double) {}
inline void ImGui_ImplGlfw_CursorEnterCallback(GLFWwindow*, int) {}

// --- what only the Android build has ---
namespace android_shell {

struct Insets {
    float left = 0, top = 0, right = 0, bottom = 0;  // px covered by system bars / the camera cut-out
};
Insets insets();

// What the fingers that started on the document did, as of the last ImGui_ImplGlfw_NewFrame.
struct DocTouch {
    float scroll_dy = 0;  // px the content moves with the fingers (+ = down)
    float zoom = 1.0f;    // pinch: factor for the zoom (this frame's step)
    bool pinching = false;                // two fingers are down
    float focus_x = 0, focus_y = 0;       // the pinch centre now
    bool fling_active = false;
    float fling_dy = 0;                   // the fling's share of scroll_dy
    float fling_v = 0, fling_total = 0;   // the running fling: lift velocity (px/s) and the distance it will cover (px)
    const void* h_key = nullptr;          // the sideways-scrolling block the finger is dragging (or flinging)
    float h_dx = 0;                       // px the block content moves right with the finger this frame (fling included)
};
DocTouch doc_touch();
// A block of the document that scrolls sideways (code block, wide formula), as drawn this frame. A finger that goes
// down on one can drag it sideways (direction locked once the slop is crossed); `key` names it for the caller.
struct HScrollBlock {
    float x0, y0, x1, y1;
    const void* key;
};
void set_hscroll_blocks(const std::vector<HScrollBlock>& blocks);
// The tab strip's scrolling part (spec 35; empty rectangle: none). A finger that goes down on it also scrolls it sideways:
// `h_key == tab_strip_key()` then, with `h_dx` as for a block; the finger's tap still clicks a tab.
void set_tab_strip(float x0, float y0, float x1, float y1);
const void* tab_strip_key();
// The fling of a sideways scroll ran into an end of its block.
void doc_touch_hit_end_h();
// The fling ran into an end of the document.
void doc_touch_hit_end();
// Fingers that start inside this rectangle belong to the document (gestures); elsewhere they act
// as taps and drags on ImGui's widgets. `ui_only`: a popup is open, every finger belongs to ImGui.
void set_doc_region(float x0, float y0, float x1, float y1, bool ui_only);
// Settings window open: fingers outside the document act as a plain mouse (press, drag, release), so
// sliders can be dragged; otherwise they tap and scroll lists.
void set_raw_touch(bool raw);
// A finger is down, a fling or a tap is still being delivered: keep drawing frames.
bool touch_busy();
// Light status bar icons for a light theme, dark for a dark one.
void set_light_bars(bool light);

// Power (spec 35). Called on the loop's thread when the app is going to the background (pause, stop, save state,
// the window going away): write whatever is pending (config, state file).
void set_background_hook(std::function<void()> hook);
// A document read again from its source while a tab shows its older copy: (old path, new path), UTF-8. Main thread.
std::optional<std::pair<std::string, std::string>> take_replacement();
// Dev switch (file `power_legacy` in the app home): the loop as before pass 5 (16 ms polls, a frame every 0.5 s).
bool legacy_power();
// True once after a wait that ended for input, a job, a document or a window event (false: only its timer ran out).
bool consume_woken();
// Counters for the debug log (file `power_log` in the app home).
void count_frame();
void count_skipped();

}  // namespace android_shell
