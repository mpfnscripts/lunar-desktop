// Lunar Desktop for Windows: streams your PC (desktop and SteamVR games) to a Meta Quest over Wi-Fi.
#include <winsock2.h>
#include <windows.h>
#include <mfapi.h>
#include <shellapi.h>
#include <string>
#include "app.h"
#include "log.h"
#include "resource.h"
#include "settings.h"
#include "steamvr.h"
#include "ui_server.h"

#define WM_TRAY (WM_APP + 1)
#define WM_NOTIFY_USER (WM_APP + 2)
#define WM_OPEN_UI (WM_APP + 3)
enum { ID_OPEN = 1, ID_STEAMVR, ID_STARTUP, ID_QUIT };

static HWND g_wnd = nullptr;
static NOTIFYICONDATAW g_nid = {};
static HICON g_icon = nullptr;

struct PendingNote {
    std::wstring title, text;
};

static void Notify(const std::string &title, const std::string &text) {
    auto *n = new PendingNote{Widen(title), Widen(text)};
    PostMessageW(g_wnd, WM_NOTIFY_USER, 0, (LPARAM)n);
}
static void RequestOpenUi() { PostMessageW(g_wnd, WM_OPEN_UI, 0, 0); }

static void ShowMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    HostSettings s = g_settings.Get();
    AppendMenuW(m, MF_STRING | MF_DEFAULT, ID_OPEN, L"Open Lunar Desktop");
    AppendMenuW(m, MF_STRING, ID_STEAMVR, steamvr::IsRunning() ? L"Stop SteamVR" : L"Launch SteamVR");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (s.startWithWindows ? MF_CHECKED : 0), ID_STARTUP, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case ID_OPEN: OpenUiWindow(); break;
        case ID_STEAMVR:
            if (steamvr::IsRunning()) g_app.Action("stop_steamvr", nlohmann::json::object());
            else {
                std::string err = g_app.Action("launch_steamvr", nlohmann::json::object());
                if (!err.empty()) Notify("Lunar Desktop", err);
            }
            break;
        case ID_STARTUP: {
            bool on = !s.startWithWindows;
            HKEY k;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
                if (on) {
                    wchar_t exe[MAX_PATH];
                    GetModuleFileNameW(nullptr, exe, MAX_PATH);
                    std::wstring v = std::wstring(L"\"") + exe + L"\" --background";
                    RegSetValueExW(k, L"LunarDesktop", 0, REG_SZ, (const BYTE *)v.c_str(), (DWORD)((v.size() + 1) * 2));
                } else {
                    RegDeleteValueW(k, L"LunarDesktop");
                }
                RegCloseKey(k);
            }
            g_settings.Apply(nlohmann::json{{"startWithWindows", on}});
            break;
        }
        case ID_QUIT: PostQuitMessage(0); break;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreated) {
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }
    switch (msg) {
        case WM_TRAY:
            if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == NIN_BALLOONUSERCLICK) OpenUiWindow();
            else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowMenu(hwnd);
            return 0;
        case WM_NOTIFY_USER: {
            auto *n = (PendingNote *)lp;
            NOTIFYICONDATAW nid = g_nid;
            nid.uFlags = NIF_INFO;
            wcsncpy(nid.szInfoTitle, n->title.c_str(), 63);
            wcsncpy(nid.szInfo, n->text.c_str(), 255);
            nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
            nid.hBalloonIcon = g_icon;
            Shell_NotifyIconW(NIM_MODIFY, &nid);
            delete n;
            return 0;
        }
        case WM_OPEN_UI:
            OpenUiWindow();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    bool background = cmdLine && wcsstr(cmdLine, L"--background");
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\LunarDesktopHost");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        OpenUiWindow();
        return 0;
    }

    LogInit(AppDataDir() + L"\\host.log");
    Log("Lunar Desktop 1.0.0 starting");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    g_settings.Load();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"LunarDesktopTray";
    RegisterClassW(&wc);
    g_wnd = CreateWindowW(wc.lpszClassName, L"Lunar Desktop", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);

    g_icon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icon;
    wcscpy(g_nid.szTip, L"Lunar Desktop");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // Register the SteamVR driver on first run so games see the headset.
    if (steamvr::IsInstalled() && !steamvr::IsDriverRegistered() && !steamvr::IsRunning()) {
        std::string err;
        if (!steamvr::RegisterDriver(&err)) Log("Driver registration: %s", err.c_str());
    }

    g_app.notify = Notify;
    g_app.openUi = RequestOpenUi;
    if (!g_app.Start()) {
        MessageBoxW(nullptr, L"Lunar Desktop could not start its network service. Another copy may already be running, or a firewall is blocking it.",
                    L"Lunar Desktop", MB_ICONERROR);
        return 1;
    }
    UiServerStart();
    if (!background && g_settings.Get().openWindowOnStart) OpenUiWindow();
    Notify("Lunar Desktop is running", "Put on your Quest and open Lunar Desktop to connect.");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    UiServerStop();
    g_app.Stop();
    MFShutdown();
    CoUninitialize();
    CloseHandle(single);
    return 0;
}
