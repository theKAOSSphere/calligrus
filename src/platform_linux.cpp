// Small OS helpers (Linux). Counterpart of platform.cpp (Windows); same interface, see platform.h.
#include "platform.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

extern char** environ;

namespace platform {

namespace {

// Runs `argv[0]` (searched in PATH) with stdout captured (and stderr, if `with_stderr`; else it
// goes to /dev/null, as stdin comes from it). Returns the exit code, or -1 if it could not be
// started; `out` gets what it printed.
int run_capture(const std::vector<std::string>& argv, std::string& out, bool with_stderr = false) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (with_stderr) posix_spawn_file_actions_adddup2(&fa, 1, 2);
    else posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return -1;
    }
    char buf[4096];
    for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR);)
        if (n > 0) out.append(buf, size_t(n));
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Starts a program and forgets it (a reaper thread collects the exit status).
void spawn_detached(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return;
    std::thread([pid] {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }).detach();
}

// A file picker: zenity (GTK) or kdialog (KDE), whichever is installed. With neither,
// say so on stderr (the user can still pass a path or drop a file on the window).
std::optional<std::filesystem::path> pick_file(const char* title, const char* label, const char* patterns) {
    std::string out;
    int rc = run_capture({"zenity", "--file-selection", std::string("--title=") + title,
                          std::string("--file-filter=") + label + " | " + patterns, "--file-filter=All files | *"},
                         out);
    if (rc == -1 || rc == 127) {
        out.clear();
        rc = run_capture({"kdialog", "--title", title, "--getopenfilename", ".", std::string(patterns) + "|" + label}, out);
    }
    if (rc == -1 || rc == 127) {
        std::fprintf(stderr, "calligrus: no file dialog available (install zenity or kdialog); pass a path or drop a file on the window\n");
        return std::nullopt;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (rc != 0 || out.empty()) return std::nullopt;  // cancelled
    return std::filesystem::path(std::u8string(out.begin(), out.end()));
}

}  // namespace

std::vector<std::filesystem::path> args() {
    // /proc/self/cmdline: NUL-separated raw bytes (UTF-8 file names survive).
    std::vector<std::filesystem::path> out;
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string all = ss.str();
    bool first = true;
    for (size_t at = 0; at < all.size();) {
        size_t nul = all.find('\0', at);
        if (nul == std::string::npos) nul = all.size();
        if (!first) out.emplace_back(std::u8string(all.begin() + std::ptrdiff_t(at), all.begin() + std::ptrdiff_t(nul)));
        first = false;  // skip argv[0] (the exe)
        at = nul + 1;
    }
    return out;
}

std::filesystem::path exe_path() {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : p;
}

void print_cli(const std::string& line) {
    std::fprintf(stdout, "%s\n", line.c_str());
    std::fflush(stdout);
}

std::filesystem::path exe_dir() { return exe_path().parent_path(); }

std::optional<std::filesystem::path> open_file_dialog(void*) {
    return pick_file("Open", "Markdown / Org", "*.md *.markdown *.org *.txt");
}

std::optional<std::filesystem::path> open_font_dialog(void*) {
    return pick_file("Choose a font", "Fonts", "*.ttf *.otf");
}

std::optional<std::filesystem::path> save_pdf_dialog(void*, const std::filesystem::path& suggested) {
    std::string out;
    const std::string start = suggested.string();
    int rc = run_capture({"zenity", "--file-selection", "--save", "--confirm-overwrite", "--title=Export the slides as a PDF",
                          "--filename=" + start, "--file-filter=PDF | *.pdf", "--file-filter=All files | *"},
                         out);
    if (rc == -1 || rc == 127) {
        out.clear();
        rc = run_capture({"kdialog", "--title", "Export the slides as a PDF", "--getsavefilename", start, "*.pdf|PDF"}, out);
    }
    if (rc == -1 || rc == 127) {
        std::fprintf(stderr, "calligrus: no file dialog available (install zenity or kdialog); use calligrus --export\n");
        return std::nullopt;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (rc != 0 || out.empty()) return std::nullopt;  // cancelled
    return std::filesystem::path(std::u8string(out.begin(), out.end()));
}

int run_and_capture(const std::vector<std::filesystem::path>& argv, std::string& out) {
    std::vector<std::string> args;
    for (const std::filesystem::path& a : argv) args.push_back(a.string());
    return run_capture(args, out, true);
}

bool write_file_atomic(const std::filesystem::path& path, const std::string& bytes) {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes.data(), std::streamsize(bytes.size()));
        out.flush();
        if (!out) return false;
    }
    std::filesystem::rename(tmp, path, ec);  // atomic replace on POSIX
    if (ec) std::filesystem::remove(tmp, ec);
    return !ec;
}

void open_path(const std::filesystem::path& path) { spawn_detached({"xdg-open", path.string()}); }

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    // Strip a UTF-8 byte-order mark if present.
    if (s.starts_with("\xEF\xBB\xBF")) s.erase(0, 3);
    return s;
}

void open_url(const std::string& url) {
    // Only hand real web/mail links to the desktop, never arbitrary commands/paths.
    if (!(url.starts_with("http://") || url.starts_with("https://") || url.starts_with("mailto:"))) return;
    spawn_detached({"xdg-open", url});
}

void allow_editor_focus() {}

static bool query_prefers_dark();

bool system_prefers_dark() {
    // Asked once a second by the caller; spawning gsettings that often is wasteful, so keep the answer for a while.
    static bool cached = true;
    static std::chrono::steady_clock::time_point asked{};
    const auto now = std::chrono::steady_clock::now();
    if (asked != std::chrono::steady_clock::time_point{} && now - asked < std::chrono::seconds(5)) return cached;
    asked = now;
    cached = query_prefers_dark();
    return cached;
}

// Same order and fallbacks as the Rust port (they must agree, spec 6/19):
// color-scheme prefer-dark/prefer-light, else the GTK theme name, else (no gsettings) dark.
static bool query_prefers_dark() {
    // GNOME (and most others through the XDG settings portal) keep the choice in GSettings.
    std::string out;
    if (run_capture({"gsettings", "get", "org.gnome.desktop.interface", "color-scheme"}, out) == 0) {
        if (out.find("prefer-dark") != std::string::npos) return true;
        if (out.find("prefer-light") != std::string::npos) return false;
        // 'default': look at the GTK theme name instead.
        std::string theme;
        if (run_capture({"gsettings", "get", "org.gnome.desktop.interface", "gtk-theme"}, theme) == 0)
            return theme.find("dark") != std::string::npos || theme.find("Dark") != std::string::npos;
        return false;
    }
    return true;  // unknown: dark, like the Windows version when it can't tell
}

void set_dark_title_bar(void*, bool) {}  // Windows only: the window manager draws the title bar here

}  // namespace platform
