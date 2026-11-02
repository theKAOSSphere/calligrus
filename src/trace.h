// Timing and debug lines. On Android they go to the system log (tag "calligrus"); elsewhere to stderr, and only when
// the environment variable CALLIGRUS_TRACE is set: to a file path (contains a slash or backslash) to append them to that
// file instead, which a Windows GUI launch needs (it has no stderr). Used for the system-font stages (spec 30) and the
// display-scale changes.
#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace trace {

inline bool enabled() {
#ifdef __ANDROID__
    return true;
#else
    static const bool on = [] {
#pragma warning(suppress : 4996)  // MSVC: getenv is fine here (read once)
        const char* v = std::getenv("CALLIGRUS_TRACE");
        return v && *v;
    }();
    return on;
#endif
}

// Milliseconds on a steady clock since the first call (any call from any thread; the first one is made at start-up).
inline double now_ms() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline void log(const char* fmt, ...) {
    if (!enabled()) return;
    char line[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "calligrus", "%s", line);
#else
    static FILE* const out = [] {
#pragma warning(suppress : 4996)  // (read once, like enabled())
        const char* v = std::getenv("CALLIGRUS_TRACE");
        const bool path = v && (std::strchr(v, '/') || std::strchr(v, '\\'));
#pragma warning(suppress : 4996)
        FILE* f = path ? std::fopen(v, "a") : nullptr;
        return f ? f : stderr;
    }();
    std::fprintf(out, "[calligrus %9.1f ms] %s\n", now_ms(), line);
    std::fflush(out);
#endif
}

// Stopwatch: ms() since construction.
struct Stopwatch {
    double start = now_ms();
    double ms() const { return now_ms() - start; }
};

}  // namespace trace
