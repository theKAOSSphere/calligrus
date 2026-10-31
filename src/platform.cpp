#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace platform {

std::vector<std::filesystem::path> args() {
    std::vector<std::filesystem::path> out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) out.emplace_back(argv[i]);  // skip argv[0] (the exe)
    LocalFree(argv);
    return out;
}

std::filesystem::path exe_path() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n));
}

void print_cli(const std::string& line) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* out = nullptr;
        if (freopen_s(&out, "CONOUT$", "w", stdout) == 0) {
            std::fprintf(stdout, "%s\n", line.c_str());
            std::fflush(stdout);
            return;
        }
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, w.data(), n);
    MessageBoxW(nullptr, w.c_str(), L"Calligrus", MB_OK);
}

std::filesystem::path exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

std::optional<std::filesystem::path> open_file_dialog(void* owner) {
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(owner);
    // Filter pairs: "label\0pattern\0", terminated by an extra \0.
    ofn.lpstrFilter = L"Markdown / Org (*.md;*.markdown;*.org;*.txt)\0*.md;*.markdown;*.org;*.txt\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;  // cancelled
    return std::filesystem::path(file);
}

std::optional<std::filesystem::path> open_font_dialog(void* owner) {
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(owner);
    ofn.lpstrFilter = L"Fonts (*.ttf;*.otf)\0*.ttf;*.otf\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;  // cancelled
    return std::filesystem::path(file);
}

std::optional<std::filesystem::path> save_pdf_dialog(void* owner, const std::filesystem::path& suggested) {
    wchar_t file[MAX_PATH * 4] = L"";
    wcsncpy_s(file, suggested.filename().wstring().c_str(), _TRUNCATE);
    const std::wstring dir = suggested.parent_path().wstring();
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(owner);
    ofn.lpstrFilter = L"PDF (*.pdf)\0*.pdf\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrDefExt = L"pdf";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return std::nullopt;  // cancelled
    return std::filesystem::path(file);
}

// One argument quoted for CreateProcess's command line (the CRT's parsing rules: backslashes
// are literal unless they come before a quote).
static std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            slashes++;
            continue;
        }
        q.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
}

int run_and_capture(const std::vector<std::filesystem::path>& argv, std::string& out) {
    std::wstring cmd;
    for (const std::filesystem::path& a : argv) cmd += (cmd.empty() ? L"" : L" ") + quote_arg(a.wstring());
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);  // (only the child's end is inherited)
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        CloseHandle(rd);
        return -1;
    }
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0) out.append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return int(code);
}

bool write_file_atomic(const std::filesystem::path& path, const std::string& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes.data(), std::streamsize(bytes.size()));
        if (!out) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void open_path(const std::filesystem::path& path) {
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    // Strip a UTF-8 byte-order mark if present (Notepad likes to add one).
    if (s.starts_with("\xEF\xBB\xBF")) s.erase(0, 3);
    return s;
}

void open_url(const std::string& url) {
    // Only hand real web/mail links to the shell, never arbitrary commands/paths.
    if (!(url.starts_with("http://") || url.starts_with("https://") || url.starts_with("mailto:"))) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, w.data(), n);
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void allow_editor_focus() { AllowSetForegroundWindow(ASFW_ANY); }

bool system_prefers_dark() {
    // Windows stores the "app mode" choice in the registry: 0 = dark, 1 = light.
    DWORD light = 0, size = sizeof(light);
    LSTATUS rc = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                              L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return rc != ERROR_SUCCESS || light == 0;
}

void set_dark_title_bar(void* hwnd, bool dark) {
    BOOL value = dark ? TRUE : FALSE;
    const DWORD kUseImmersiveDarkMode = 20;  // DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 2004+)
    DwmSetWindowAttribute(static_cast<HWND>(hwnd), kUseImmersiveDarkMode, &value, sizeof(value));
    // The title bar only picks the change up when the frame is redrawn.
    SetWindowPos(static_cast<HWND>(hwnd), nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace platform
