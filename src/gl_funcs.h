// The OpenGL 1.1 entry points Calligrus itself calls (textures, pixel storage, glReadPixels, viewport, clear, glGetString...).
//
// Windows links opengl32 and Android links GLESv3, so there these are the ordinary functions and this header only brings in the
// GL declarations. On desktop Linux the program does not link any GL library (libOpenGL.so / libGL.so): GLFW opens the system's
// GLX / EGL itself when the window is created, and the calls below go through function pointers that gl_funcs_load() fetches
// from the current context with glfwGetProcAddress (the way ImGui's own backend loads what it uses). The point: one binary
// runs against whatever the machine has (glvnd, Mesa, a vendor driver) and, under the OpenGL ES 2.0 fallback (spec 36), the
// calls reach the ES library that made the context instead of the desktop one.
//
// Call sites do not change: `glGenTextures(...)` is a macro for the pointer. Include this header instead of <GL/gl.h>, and after
// <GLFW/glfw3.h> (which includes <GL/gl.h> itself): a later <GL/gl.h> would declare the macro'd names as functions.
#pragma once

#ifdef __ANDROID__
// (the window layer, src/android/window_android.h, includes <GLES3/gl3.h>)
inline bool gl_funcs_load() { return true; }
#else
#include <GL/gl.h>

#if defined(_WIN32) || defined(__APPLE__)
inline bool gl_funcs_load() { return true; }
#else

// One line per function. (decltype(&name) takes the type from <GL/gl.h>'s prototype, so nothing is typed twice.)
#define CALLIGRUS_GL_FUNCS(X) \
    X(glBindTexture)          \
    X(glClear)                \
    X(glClearColor)           \
    X(glDeleteTextures)       \
    X(glGenTextures)          \
    X(glGetError)             \
    X(glGetIntegerv)          \
    X(glGetString)            \
    X(glPixelStorei)          \
    X(glReadPixels)           \
    X(glTexImage2D)           \
    X(glTexParameteri)        \
    X(glTexSubImage2D)        \
    X(glViewport)

#define CALLIGRUS_GL_DECLARE(name) extern decltype(&name) calligrus_##name;
CALLIGRUS_GL_FUNCS(CALLIGRUS_GL_DECLARE)
#undef CALLIGRUS_GL_DECLARE

// Fetches every pointer from the context that is current on this thread (call it right after glfwMakeContextCurrent, again
// when a different kind of context was made). false (and the missing names on stderr / in the trace) if any is absent.
bool gl_funcs_load();

#ifndef CALLIGRUS_GL_FUNCS_IMPL
#define glBindTexture calligrus_glBindTexture
#define glClear calligrus_glClear
#define glClearColor calligrus_glClearColor
#define glDeleteTextures calligrus_glDeleteTextures
#define glGenTextures calligrus_glGenTextures
#define glGetError calligrus_glGetError
#define glGetIntegerv calligrus_glGetIntegerv
#define glGetString calligrus_glGetString
#define glPixelStorei calligrus_glPixelStorei
#define glReadPixels calligrus_glReadPixels
#define glTexImage2D calligrus_glTexImage2D
#define glTexParameteri calligrus_glTexParameteri
#define glTexSubImage2D calligrus_glTexSubImage2D
#define glViewport calligrus_glViewport
#endif

#endif  // Linux
#endif  // not Android
