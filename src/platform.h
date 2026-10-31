// Small OS helpers (Windows: platform.cpp, Linux: platform_linux.cpp). Kept separate so the rest of the code never
// includes <windows.h> (which defines macros like min/max that clash with C++).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace platform {

// Command-line arguments as paths. Uses the wide (UTF-16) command line so
// non-ASCII file names survive, unlike `char** argv`.
std::vector<std::filesystem::path> args();

// Full path of the running .exe.
std::filesystem::path exe_path();

// One line of command-line output from a GUI-subsystem exe: printed to the
// parent console if there is one, else shown in a message box.
void print_cli(const std::string& line);

// Folder containing the running .exe (assets are copied next to it).
std::filesystem::path exe_dir();

// Native "Open file" dialog. `owner` is the parent HWND (may be nullptr).
std::optional<std::filesystem::path> open_file_dialog(void* owner);

// Native "Open" dialog for a font file (*.ttf;*.otf).
std::optional<std::filesystem::path> open_font_dialog(void* owner);

// Native "Save as" dialog for a PDF, starting at `suggested` (its folder and file name); asks
// before replacing a file.
std::optional<std::filesystem::path> save_pdf_dialog(void* owner, const std::filesystem::path& suggested);

// Run a program (`argv[0]` = its path) and wait for it: its exit code, or -1 if it could not be
// started. `out` gets what it printed on stdout and stderr. No console window, stdin empty.
int run_and_capture(const std::vector<std::filesystem::path>& argv, std::string& out);

// Replace `path` with `bytes` atomically: write a temporary file next to it,
// then rename it over the old one (so a crash never leaves half a file).
// Creates the folder if needed. Returns false on failure.
bool write_file_atomic(const std::filesystem::path& path, const std::string& bytes);

// Open a file in its default app, or a folder in Explorer.
void open_path(const std::filesystem::path& path);

// Whole file as bytes (UTF-8 for Markdown), or nullopt if it can't be read.
std::optional<std::string> read_file(const std::filesystem::path& path);

// Open a URL in the default browser.
void open_url(const std::string& url);

// After a Ctrl+click we hold the focus; let the editor take it back (spec 23).
// Windows needs our permission for that; elsewhere a no-op.
void allow_editor_focus();

// Does Windows want dark-mode apps ("Choose your app mode: Dark")?
bool system_prefers_dark();

// Make the window's title bar dark or light to match the app theme.
void set_dark_title_bar(void* hwnd, bool dark);

}  // namespace platform
