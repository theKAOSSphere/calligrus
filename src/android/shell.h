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
// ACTION_OPEN_DOCUMENT; the result arrives through post_open. `replaces`: the private copy of a tab whose original the user
// is picking again ("Reopen"): the picked file then takes that tab's place (spec 35).
void show_open_picker(const std::string& replaces = std::string());
// A tab was closed (spec 35): the first line is its private copy, the other lines the copies of the tabs still open. Java
// lets go of the persisted read permission of the source unless another open tab shows the same one.
void release_source(const std::string& closed_then_open);
// The tabs that were restored (their private copies, UTF-8 paths, one per line): Java re-reads the originals it may
// still read and answers through post_open with `replaces` set (spec 35).
void refresh_sources(const std::string& paths);
void open_external_url(const std::string& url);
void set_light_bars(bool light);

// A document that arrived from Java (the picker, "Open with", share): its display name and bytes. May be
// called from any thread; the bytes are stored under the app's files dir and the path returned ("" if it could
// not be stored) and queued for the window loop, which opens it like a dropped file. `replaces`: the private copy
// this document is a newer read of (same source), if any; then the tab showing that copy takes the new one
// instead (spec 35). `refresh`: a re-read at start-up; nothing is queued when the bytes did not change.
std::string post_open(const std::string& name, const std::string& bytes, const std::string& replaces, bool refresh);
struct Opened {
    std::string path;      // the stored copy (UTF-8)
    std::string replaces;  // the copy it replaces, or empty
};
std::optional<Opened> take_opened();  // main thread: the next queued document

// Java's word about a restored tab's original (any thread posts, the main thread takes): `stale` = it could not be read again,
// the tab shows a possibly old copy; not stale = the user picked the source again (the note goes).
struct SourceState {
    std::string path;  // the private copy (UTF-8)
    bool stale;
};
std::optional<SourceState> take_source_state();

// Back key (the activity's back handler runs on the Java UI thread): `set_back_wanted(true)` while the tab list is open;
// then Back does not leave the app, it is queued for `take_back` (main thread).
void set_back_wanted(bool wanted);
bool take_back();
bool back_wanted();                                         // (the Java side asks)
void press_back();
void post_source_state(const std::string& path, bool stale);  // (the Java side tells)

// The app's private folders (set up by android_main): HOME, XDG_*, TMPDIR point into them.
std::string files_dir();
std::string cache_dir();

void wake();          // wake the window loop (from any thread)
bool prefers_dark();  // the system's night mode

}  // namespace shell

// The app proper (main.cpp): the desktop `main`, on Android called from android_main.
int calligrus_main();
