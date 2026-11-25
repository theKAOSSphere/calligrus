// See gl_funcs.h. Only desktop Linux has anything to define here.
#if !defined(_WIN32) && !defined(__ANDROID__) && !defined(__APPLE__)

#define CALLIGRUS_GL_FUNCS_IMPL  // (keeps the header from turning the names below into macros)
#include "gl_funcs.h"

#include <GLFW/glfw3.h>

#include <cstdio>

#include "trace.h"

#define CALLIGRUS_GL_DEFINE(name) decltype(&name) calligrus_##name = nullptr;
CALLIGRUS_GL_FUNCS(CALLIGRUS_GL_DEFINE)
#undef CALLIGRUS_GL_DEFINE

bool gl_funcs_load() {
    bool ok = true;
#define CALLIGRUS_GL_LOAD(name)                                                                         \
    calligrus_##name = reinterpret_cast<decltype(&name)>(glfwGetProcAddress(#name));                    \
    if (!calligrus_##name) {                                                                            \
        std::fprintf(stderr, "calligrus: the OpenGL context has no %s\n", #name);                      \
        trace::log("the OpenGL context has no %s", #name);                                              \
        ok = false;                                                                                     \
    }
    CALLIGRUS_GL_FUNCS(CALLIGRUS_GL_LOAD)
#undef CALLIGRUS_GL_LOAD
    return ok;
}

#endif
