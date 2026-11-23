#include "log.h"
#include <windows.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <deque>
#include <mutex>

static std::mutex g_logLock;
static FILE *g_logFile = nullptr;
static std::deque<std::string> g_tail;

void LogInit(const std::wstring &path) {
    g_logFile = _wfopen(path.c_str(), L"w");
}

void Log(const char *fmt, ...) {
    char buf[2048];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2200];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    std::lock_guard<std::mutex> lk(g_logLock);
    if (g_logFile) {
        fprintf(g_logFile, "%s\n", line);
        fflush(g_logFile);
    }
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
    g_tail.push_back(line);
    while (g_tail.size() > 300) g_tail.pop_front();
}

std::string LogTail(size_t maxLines) {
    std::lock_guard<std::mutex> lk(g_logLock);
    std::string out;
    size_t start = g_tail.size() > maxLines ? g_tail.size() - maxLines : 0;
    for (size_t i = start; i < g_tail.size(); i++) {
        out += g_tail[i];
        out += "\n";
    }
    return out;
}

std::string Narrow(const std::wstring &w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring AppDataDir() {
    wchar_t *p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) {
        dir = p;
        CoTaskMemFree(p);
    } else {
        dir = ExeDir();
    }
    dir += L"\\LunarDesktop";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s = buf;
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? s : s.substr(0, p);
}
