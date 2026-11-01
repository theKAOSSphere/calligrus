// Internal to the Android build: what android_main.cpp, window_android.cpp and platform_android.cpp share.
#pragma once

#include <optional>
#include <string>

struct android_app;

namespace shell {

// The GameActivity glue's state; set by android_main for the whole run.
android_app* app();
void set_app(android_app* a);

// Calls into the Java activity (platform_android.cpp). Each is a no-op if the call fails.
void show_open_picker();                      // ACTION_OPEN_DOCUMENT; the result arrives through post_open
void open_external_url(const std::string& url);
void set_light_bars(bool light);

// A document that arrived from Java (the picker, "Open with", share): its display name and bytes. May be
// called from any thread; the bytes are stored under the app's files dir and the path queued for the
// window loop, which opens it like a dropped file.
void post_open(const std::string& name, const std::string& bytes);
std::optional<std::string> take_opened_path();  // main thread: the next queued path (UTF-8)

// The app's private folders (set up by android_main): HOME, XDG_*, TMPDIR point into them.
std::string files_dir();
std::string cache_dir();

void wake();          // wake the window loop (from any thread)
bool prefers_dark();  // the system's night mode

}  // namespace shell

// The app proper (main.cpp): the desktop `main`, on Android called from android_main.
int calligrus_main();
