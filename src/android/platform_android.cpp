// Small OS helpers (Android). Counterpart of platform.cpp (Windows) and platform_linux.cpp; same interface,
// see platform.h. Also the calls into the Java activity and the documents that come from it (shell.h).
#include "../platform.h"

#include <android/configuration.h>
#include <android/log.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <jni.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>

#include "shell.h"

#define LOG_TAG "calligrus"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace fs = std::filesystem;

namespace shell {

namespace {

android_app* g_app = nullptr;
std::mutex g_mutex;
std::deque<std::string> g_opened;  // paths of documents that arrived, not yet taken

// A JNIEnv for this thread (attaching it if it is not a Java thread yet) and the activity object.
class Env {
public:
    Env() {
        if (!g_app || !g_app->activity || !g_app->activity->vm || !g_app->activity->javaGameActivity) return;
        vm_ = g_app->activity->vm;
        if (vm_->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) == JNI_EDETACHED) {
            if (vm_->AttachCurrentThread(&env_, nullptr) == JNI_OK) attached_ = true;
            else env_ = nullptr;
        }
    }
    ~Env() {
        if (attached_) vm_->DetachCurrentThread();
    }
    Env(const Env&) = delete;
    Env& operator=(const Env&) = delete;
    JNIEnv* get() const { return env_; }
    jobject activity() const { return g_app->activity->javaGameActivity; }
    // True (the exception is cleared) if the last Java call threw.
    bool failed(const char* what) const {
        if (!env_->ExceptionCheck()) return false;
        env_->ExceptionClear();
        LOGE("%s threw a Java exception", what);
        return true;
    }

private:
    JavaVM* vm_ = nullptr;
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};

// activity.<name>(<args>) for the few void methods MainActivity has.
void call(const char* name, const char* sig, const std::string& str_arg, bool bool_arg) {
    Env e;
    JNIEnv* env = e.get();
    if (!env) return;
    jclass cls = env->GetObjectClass(e.activity());
    jmethodID m = env->GetMethodID(cls, name, sig);
    env->DeleteLocalRef(cls);
    if (!m) {
        e.failed(name);
        return;
    }
    if (sig[1] == ')') {
        env->CallVoidMethod(e.activity(), m);
    } else if (sig[1] == 'Z') {
        env->CallVoidMethod(e.activity(), m, jboolean(bool_arg));
    } else {
        jstring s = env->NewStringUTF(str_arg.c_str());
        env->CallVoidMethod(e.activity(), m, s);
        env->DeleteLocalRef(s);
    }
    e.failed(name);
}

// A file name that is safe inside our own folder: no folder parts, no control characters; ".md" if it
// has no extension at all.
std::string safe_name(const std::string& name) {
    std::string base = name.substr(name.find_last_of("/\\") == std::string::npos ? 0 : name.find_last_of("/\\") + 1);
    for (char& c : base)
        if (static_cast<unsigned char>(c) < 0x20 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    if (base.empty() || base == "." || base == "..") base = "document.md";
    if (base.find('.') == std::string::npos) base += ".md";
    return base;
}

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

}  // namespace

android_app* app() { return g_app; }
void set_app(android_app* a) { g_app = a; }

void show_open_picker() { call("showOpenPicker", "()V", "", false); }
void open_external_url(const std::string& url) { call("openUrl", "(Ljava/lang/String;)V", url, false); }
void set_light_bars(bool light) { call("setLightBars", "(Z)V", "", light); }

std::string files_dir() {
    return g_app && g_app->activity && g_app->activity->internalDataPath ? g_app->activity->internalDataPath : "";
}
std::string cache_dir() {
    const fs::path files = files_dir();
    return files.empty() ? "" : (files.parent_path() / "cache").string();
}

bool prefers_dark() {
    if (!g_app || !g_app->config) return true;
    return AConfiguration_getUiModeNight(g_app->config) != ACONFIGURATION_UI_MODE_NIGHT_NO;
}

void post_open(const std::string& name, const std::string& bytes) {
    // <files>/opened/<hash of the bytes>/<name>: the same document is the same path, a new one a new path.
    char hash[24];
    std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(fnv1a(bytes)));
    const fs::path root = fs::path(files_dir()) / "opened";
    const fs::path dir = root / hash;
    const std::string leaf = safe_name(name);
    const fs::path file = dir / fs::path(std::u8string(leaf.begin(), leaf.end()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || !platform::write_file_atomic(file, bytes)) {
        LOGE("cannot store %s", file.string().c_str());
        return;
    }
    // Keep the 40 newest documents.
    std::vector<std::pair<fs::file_time_type, fs::path>> dirs;
    for (const auto& d : fs::directory_iterator(root, ec))
        if (d.is_directory(ec)) dirs.push_back({d.last_write_time(ec), d.path()});
    if (dirs.size() > 40) {
        std::sort(dirs.begin(), dirs.end());
        for (size_t i = 0; i + 40 < dirs.size(); i++)
            if (dirs[i].second != dir) fs::remove_all(dirs[i].second, ec);
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_opened.push_back(file.string());
    }
    LOGI("opened %s (%zu bytes)", file.string().c_str(), bytes.size());
    wake();
}

std::optional<std::string> take_opened_path() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_opened.empty()) return std::nullopt;
    std::string p = std::move(g_opened.front());
    g_opened.pop_front();
    return p;
}

}  // namespace shell

// The Java side hands over a document it read (picker, "Open with", share): name and bytes.
extern "C" JNIEXPORT void JNICALL Java_io_github_thekaossphere_calligrus_MainActivity_nativeOpenDocument(JNIEnv* env, jclass, jstring name, jbyteArray data) {
    if (!name || !data) return;
    const char* n = env->GetStringUTFChars(name, nullptr);
    const std::string display = n ? n : "";
    if (n) env->ReleaseStringUTFChars(name, n);
    const jsize len = env->GetArrayLength(data);
    std::string bytes(static_cast<size_t>(len), '\0');
    env->GetByteArrayRegion(data, 0, len, reinterpret_cast<jbyte*>(bytes.data()));
    shell::post_open(display, bytes);
}

namespace platform {

std::vector<fs::path> args() { return {}; }  // a document arrives through shell::post_open instead

fs::path exe_path() { return fs::path(shell::files_dir()) / "calligrus"; }

void print_cli(const std::string& line) { __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "%s", line.c_str()); }

fs::path exe_dir() { return fs::path(shell::files_dir()); }

// The picker is Java's: it answers through shell::post_open, so nothing comes back here.
std::optional<fs::path> open_file_dialog(void*) {
    shell::show_open_picker();
    return std::nullopt;
}

std::optional<fs::path> open_font_dialog(void*) { return std::nullopt; }
std::optional<fs::path> save_pdf_dialog(void*, const fs::path&) { return std::nullopt; }
int run_and_capture(const std::vector<fs::path>&, std::string&) { return -1; }

bool write_file_atomic(const fs::path& path, const std::string& bytes) {
    std::error_code ec;
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes.data(), std::streamsize(bytes.size()));
        out.flush();
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);  // atomic replace on POSIX
    if (ec) fs::remove(tmp, ec);
    return !ec;
}

void open_path(const fs::path&) {}

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    if (s.starts_with("\xEF\xBB\xBF")) s.erase(0, 3);  // a UTF-8 byte-order mark
    return s;
}

void open_url(const std::string& url) {
    // Only real web / mail links go to the system, never arbitrary text.
    if (!(url.starts_with("http://") || url.starts_with("https://") || url.starts_with("mailto:"))) return;
    shell::open_external_url(url);
}

void allow_editor_focus() {}
bool system_prefers_dark() { return shell::prefers_dark(); }
void set_dark_title_bar(void*, bool) {}

}  // namespace platform
