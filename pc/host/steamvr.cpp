#include "steamvr.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "openvr.h"
#include "log.h"
#include "../third_party/json/json.hpp"

using nlohmann::json;

namespace steamvr {

static std::wstring OpenVrPathsFile() {
    wchar_t *p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
        s = std::wstring(p) + L"\\openvr\\openvrpaths.vrpath";
        CoTaskMemFree(p);
    }
    return s;
}

static json ReadOpenVrPaths() {
    std::ifstream f{std::filesystem::path(OpenVrPathsFile())};
    if (!f) return json::object();
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        return json::parse(ss.str());
    } catch (...) {
        return json::object();
    }
}

static bool Exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring RuntimePath() {
    json j = ReadOpenVrPaths();
    if (j.contains("runtime") && j["runtime"].is_array()) {
        for (auto &r : j["runtime"]) {
            std::wstring p = Widen(r.get<std::string>());
            if (Exists(p + L"\\bin\\win64\\vrpathreg.exe")) return p;
        }
    }
    // Fall back to the Steam install folder
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH];
        DWORD sz = sizeof(buf);
        if (RegQueryValueExW(k, L"SteamPath", nullptr, nullptr, (BYTE *)buf, &sz) == ERROR_SUCCESS) {
            std::wstring p = std::wstring(buf) + L"\\steamapps\\common\\SteamVR";
            for (auto &c : p)
                if (c == L'/') c = L'\\';
            RegCloseKey(k);
            if (Exists(p + L"\\bin\\win64\\vrpathreg.exe")) return p;
            return L"";
        }
        RegCloseKey(k);
    }
    return L"";
}

std::wstring DriverDir() { return ExeDir() + L"\\driver\\lunar"; }

bool IsInstalled() { return !RuntimePath().empty(); }

static std::string NormPath(std::string s) {
    for (auto &c : s) {
        if (c == '/') c = '\\';
        c = (char)tolower((unsigned char)c);
    }
    while (!s.empty() && s.back() == '\\') s.pop_back();
    return s;
}

bool IsDriverRegistered() {
    json j = ReadOpenVrPaths();
    std::string mine = NormPath(Narrow(DriverDir()));
    if (j.contains("external_drivers") && j["external_drivers"].is_array())
        for (auto &d : j["external_drivers"])
            if (d.is_string() && NormPath(d.get<std::string>()) == mine) return true;
    return false;
}

static bool RunHidden(const std::wstring &exe, const std::wstring &args, DWORD *exitCode) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 15000);
    GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool RegisterDriver(std::string *error) {
    std::wstring rt = RuntimePath();
    if (rt.empty()) {
        if (error) *error = "SteamVR is not installed. Install it from Steam first.";
        return false;
    }
    if (!Exists(DriverDir() + L"\\driver.vrdrivermanifest")) {
        if (error) *error = "The driver folder is missing next to LunarDesktop.exe.";
        return false;
    }
    std::wstring reg = rt + L"\\bin\\win64\\vrpathreg.exe";
    // Remove registrations of other Lunar Desktop copies (e.g. an older download in another folder)
    json j = ReadOpenVrPaths();
    std::string mine = NormPath(Narrow(DriverDir()));
    if (j.contains("external_drivers") && j["external_drivers"].is_array()) {
        for (auto &d : j["external_drivers"]) {
            if (!d.is_string()) continue;
            std::string p = NormPath(d.get<std::string>());
            if (p != mine && p.size() >= 6 && p.substr(p.size() - 6) == "\\lunar") {
                DWORD code = 0;
                RunHidden(reg, L"removedriver \"" + Widen(d.get<std::string>()) + L"\"", &code);
            }
        }
    }
    DWORD code = 1;
    if (!RunHidden(reg, L"adddriver \"" + DriverDir() + L"\"", &code)) {
        if (error) *error = "Could not run vrpathreg.exe";
        return false;
    }
    bool ok = IsDriverRegistered();
    Log("SteamVR driver registration %s (%ls)", ok ? "ok" : "failed", DriverDir().c_str());
    if (!ok && error) *error = "vrpathreg did not register the driver";
    return ok;
}

static bool ProcessRunning(const wchar_t *name, bool kill = false) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, name) == 0) {
            found = true;
            if (kill) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h) {
                    TerminateProcess(h, 0);
                    CloseHandle(h);
                }
            }
        }
    }
    CloseHandle(snap);
    return found;
}

bool IsRunning() { return ProcessRunning(L"vrserver.exe"); }

void Launch() {
    if (IsRunning()) return;
    std::wstring rt = RuntimePath();
    std::wstring startup = rt + L"\\bin\\win64\\vrstartup.exe";
    if (!rt.empty() && Exists(startup)) {
        ShellExecuteW(nullptr, L"open", startup.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(nullptr, L"open", L"steam://rungameid/250820", nullptr, nullptr, SW_SHOWNORMAL);
    }
    Log("Launching SteamVR");
}

void Stop() {
    // Ask nicely through vrmonitor first, then make sure everything is gone.
    ShellExecuteW(nullptr, L"open", L"vrmonitor://quit", nullptr, nullptr, SW_HIDE);
    for (int i = 0; i < 30 && IsRunning(); i++) Sleep(200);
    const wchar_t *procs[] = {L"vrmonitor.exe", L"vrdashboard.exe", L"vrcompositor.exe", L"vrwebhelper.exe", L"vrserver.exe", L"vrstartup.exe"};
    for (auto p : procs) ProcessRunning(p, true);
    Log("SteamVR stopped");
}

typedef intptr_t(VR_CALLTYPE *PFN_InitInternal2)(vr::EVRInitError *, vr::EVRApplicationType, const char *);
typedef void(VR_CALLTYPE *PFN_ShutdownInternal)();
typedef void *(VR_CALLTYPE *PFN_GetGenericInterface)(const char *, vr::EVRInitError *);

bool SetupChaperone() {
    HMODULE dll = LoadLibraryW((ExeDir() + L"\\openvr_api.dll").c_str());
    if (!dll) return false;
    auto init = (PFN_InitInternal2)GetProcAddress(dll, "VR_InitInternal2");
    auto shut = (PFN_ShutdownInternal)GetProcAddress(dll, "VR_ShutdownInternal");
    auto get = (PFN_GetGenericInterface)GetProcAddress(dll, "VR_GetGenericInterface");
    bool ok = false;
    if (init && shut && get) {
        vr::EVRInitError err = vr::VRInitError_None;
        init(&err, vr::VRApplication_Utility, nullptr);
        if (err == vr::VRInitError_None) {
            auto *cs = (vr::IVRChaperoneSetup *)get(vr::IVRChaperoneSetup_Version, &err);
            if (cs) {
                vr::HmdMatrix34_t id = {};
                id.m[0][0] = id.m[1][1] = id.m[2][2] = 1.f;
                cs->RevertWorkingCopy();
                cs->SetWorkingPlayAreaSize(2.5f, 2.5f);
                cs->SetWorkingStandingZeroPoseToRawTrackingPose(&id);
                cs->SetWorkingSeatedZeroPoseToRawTrackingPose(&id);
                vr::HmdQuad_t walls[4];
                const float h = 1.25f, top = 2.4f;
                float c[4][2] = {{-h, -h}, {h, -h}, {h, h}, {-h, h}};
                for (int i = 0; i < 4; i++) {
                    int n = (i + 1) % 4;
                    walls[i].vCorners[0] = {{c[i][0], 0.f, c[i][1]}};
                    walls[i].vCorners[1] = {{c[i][0], top, c[i][1]}};
                    walls[i].vCorners[2] = {{c[n][0], top, c[n][1]}};
                    walls[i].vCorners[3] = {{c[n][0], 0.f, c[n][1]}};
                }
                cs->SetWorkingCollisionBoundsInfo(walls, 4);
                ok = cs->CommitWorkingCopy(vr::EChaperoneConfigFile_Live);
            }
            shut();
        } else {
            Log("Chaperone setup: VR_Init failed (%d)", (int)err);
        }
    }
    FreeLibrary(dll);
    Log("Chaperone setup %s", ok ? "done" : "failed");
    return ok;
}

std::string ActiveOpenXrRuntime() {
    HKEY k;
    std::string out;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        wchar_t buf[1024];
        DWORD sz = sizeof(buf);
        if (RegQueryValueExW(k, L"ActiveRuntime", nullptr, nullptr, (BYTE *)buf, &sz) == ERROR_SUCCESS) out = Narrow(buf);
        RegCloseKey(k);
    }
    return out;
}

bool OpenXrIsSteamVr() {
    std::string r = ActiveOpenXrRuntime();
    for (auto &c : r) c = (char)tolower((unsigned char)c);
    return r.find("steamvr") != std::string::npos;
}

}  // namespace steamvr
