// Entry point of the Android build. GameActivity loads libcalligrus.so and calls android_main on a thread of
// its own; we point HOME and the XDG folders at the app's private storage (so the config, the state file
// and the themes folder work as on Linux) and then run the same app as the desktop: calligrus_main()
// (main.cpp), with window_android.cpp standing in for GLFW.
#include <android/log.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <stdlib.h>

#include <filesystem>

#include "shell.h"

extern "C" void android_main(android_app* app) {
    shell::set_app(app);
    namespace fs = std::filesystem;
    const fs::path files = shell::files_dir(), cache = shell::cache_dir();
    std::error_code ec;
    for (const char* d : {"config", "state", "data"}) fs::create_directories(files / d, ec);
    fs::create_directories(cache, ec);
    setenv("HOME", files.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (files / "config").c_str(), 1);
    setenv("XDG_STATE_HOME", (files / "state").c_str(), 1);
    setenv("XDG_DATA_HOME", (files / "data").c_str(), 1);
    setenv("TMPDIR", cache.c_str(), 1);
    __android_log_print(ANDROID_LOG_INFO, "calligrus", "starting, files dir %s", files.c_str());
    const int code = calligrus_main();
    __android_log_print(ANDROID_LOG_INFO, "calligrus", "main returned %d", code);
}
