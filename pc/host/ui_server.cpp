#include "ui_server.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <string>
#include <thread>
#include "app.h"
#include "log.h"
#include "resource.h"
#include "settings.h"
#include "../../shared/lunar_protocol.h"

using nlohmann::json;

static SOCKET g_listen = INVALID_SOCKET;
static std::thread g_thread;
static std::atomic<bool> g_run{false};

static bool LoadResource(int id, const char **data, size_t *size) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return false;
    *data = (const char *)LockResource(g);
    *size = SizeofResource(nullptr, r);
    return *data != nullptr;
}

static void SendResponse(SOCKET s, int code, const char *type, const std::string &body, const char *extra = "") {
    const char *status = code == 200 ? "OK" : (code == 404 ? "Not Found" : (code == 403 ? "Forbidden" : "Bad Request"));
    char head[512];
    snprintf(head, sizeof(head),
             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n%sConnection: close\r\n\r\n",
             code, status, type, body.size(), extra);
    std::string all = head + body;
    const char *p = all.data();
    int left = (int)all.size();
    while (left > 0) {
        int n = send(s, p, left, 0);
        if (n <= 0) break;
        p += n;
        left -= n;
    }
}

static std::string Header(const std::string &req, const char *name) {
    std::string lower = req;
    for (auto &c : lower) c = (char)tolower((unsigned char)c);
    std::string key = std::string("\r\n") + name + ":";
    for (auto &c : key) c = (char)tolower((unsigned char)c);
    size_t p = lower.find(key);
    if (p == std::string::npos) return "";
    p += key.size();
    size_t e = req.find("\r\n", p);
    std::string v = req.substr(p, e - p);
    while (!v.empty() && v[0] == ' ') v.erase(0, 1);
    return v;
}

static void Handle(SOCKET s) {
    DWORD tmo = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
    std::string req;
    char buf[8192];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) { closesocket(s); return; }
        req.append(buf, n);
        headerEnd = req.find("\r\n\r\n");
        if (req.size() > 1 << 20) { closesocket(s); return; }
    }
    size_t contentLength = (size_t)atoi(Header(req, "Content-Length").c_str());
    while (req.size() < headerEnd + 4 + contentLength && contentLength < (1 << 20)) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, n);
    }
    std::string body = req.substr(headerEnd + 4);
    size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
    std::string method = req.substr(0, sp1);
    std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);

    // Only answer requests addressed to us (blocks DNS-rebinding pages)
    std::string host = Header(req, "Host");
    char ok1[64], ok2[64];
    snprintf(ok1, sizeof(ok1), "127.0.0.1:%d", LUNAR_PORT_UI);
    snprintf(ok2, sizeof(ok2), "localhost:%d", LUNAR_PORT_UI);
    if (host != ok1 && host != ok2) {
        SendResponse(s, 403, "text/plain", "forbidden");
        closesocket(s);
        return;
    }

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        const char *d;
        size_t n;
        if (LoadResource(IDR_UI_HTML, &d, &n)) SendResponse(s, 200, "text/html; charset=utf-8", std::string(d, n));
        else SendResponse(s, 404, "text/plain", "missing ui");
    } else if (method == "GET" && (path == "/logo.png" || path == "/crescent.png" || path == "/favicon.ico")) {
        int id = path == "/crescent.png" ? IDR_UI_CRESCENT : IDR_UI_LOGO;
        const char *d;
        size_t n;
        if (LoadResource(id, &d, &n)) SendResponse(s, 200, "image/png", std::string(d, n), "Cache-Control: max-age=86400\r\n");
        else SendResponse(s, 404, "text/plain", "missing");
    } else if (method == "GET" && path == "/api/state") {
        SendResponse(s, 200, "application/json", g_app.StateJson().dump());
    } else if (method == "POST" && Header(req, "X-Lunar") != "1") {
        SendResponse(s, 403, "text/plain", "forbidden");
    } else if (method == "POST" && path == "/api/settings") {
        try {
            json j = json::parse(body);
            if (j.contains("startWithWindows")) {
                HKEY k;
                if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
                    if (j["startWithWindows"].get<bool>()) {
                        wchar_t exe[MAX_PATH];
                        GetModuleFileNameW(nullptr, exe, MAX_PATH);
                        std::wstring v = std::wstring(L"\"") + exe + L"\" --background";
                        RegSetValueExW(k, L"LunarDesktop", 0, REG_SZ, (const BYTE *)v.c_str(), (DWORD)((v.size() + 1) * 2));
                    } else {
                        RegDeleteValueW(k, L"LunarDesktop");
                    }
                    RegCloseKey(k);
                }
            }
            if (g_settings.Apply(j)) g_app.OnSettingsChanged();
            SendResponse(s, 200, "application/json", "{\"ok\":true}");
        } catch (...) {
            SendResponse(s, 400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
        }
    } else if (method == "POST" && path == "/api/action") {
        try {
            json j = json::parse(body);
            std::string err = g_app.Action(j.value("action", ""), j);
            json r = {{"ok", err.empty()}, {"error", err}};
            SendResponse(s, 200, "application/json", r.dump());
        } catch (...) {
            SendResponse(s, 400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
        }
    } else {
        SendResponse(s, 404, "text/plain", "not found");
    }
    closesocket(s);
}

bool UiServerStart() {
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(LUNAR_PORT_UI);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(g_listen, (sockaddr *)&a, sizeof(a)) != 0 || listen(g_listen, 16) != 0) {
        Log("UI port %d busy", LUNAR_PORT_UI);
        return false;
    }
    g_run = true;
    g_thread = std::thread([] {
        while (g_run) {
            SOCKET c = accept(g_listen, nullptr, nullptr);
            if (c == INVALID_SOCKET) {
                if (!g_run) break;
                continue;
            }
            std::thread(Handle, c).detach();
        }
    });
    return true;
}

void UiServerStop() {
    g_run = false;
    closesocket(g_listen);
    if (g_thread.joinable()) g_thread.join();
}

void OpenUiWindow() {
    // Open in a chromeless Edge/Chrome "app" window, like Lunar Music.
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", LUNAR_PORT_UI);
    std::wstring wurl = Widen(url);
    wchar_t buf[MAX_PATH];
    std::wstring pf, pf86, local;
    if (GetEnvironmentVariableW(L"ProgramFiles", buf, MAX_PATH)) pf = buf;
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", buf, MAX_PATH)) pf86 = buf;
    if (GetEnvironmentVariableW(L"LocalAppData", buf, MAX_PATH)) local = buf;
    std::wstring candidates[] = {
        pf86 + L"\\Microsoft\\Edge\\Application\\msedge.exe",
        pf + L"\\Microsoft\\Edge\\Application\\msedge.exe",
        pf + L"\\Google\\Chrome\\Application\\chrome.exe",
        pf86 + L"\\Google\\Chrome\\Application\\chrome.exe",
        local + L"\\Google\\Chrome\\Application\\chrome.exe",
    };
    std::wstring profile = AppDataDir() + L"\\window";
    for (auto &c : candidates) {
        if (GetFileAttributesW(c.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        std::wstring args = L"--app=" + wurl + L" --window-size=1240,820 --user-data-dir=\"" + profile + L"\"";
        if ((INT_PTR)ShellExecuteW(nullptr, L"open", c.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL) > 32) return;
    }
    ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
