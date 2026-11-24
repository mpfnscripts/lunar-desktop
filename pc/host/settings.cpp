#include "settings.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "log.h"

using nlohmann::json;
Settings g_settings;

static std::wstring SettingsPath() { return AppDataDir() + L"\\settings.json"; }

static json ToJ(const HostSettings &s) {
    json j;
    j["bitrateMbps"] = s.bitrateMbps;
    j["quality"] = s.quality;
    j["codec"] = s.codec;
    j["refresh"] = s.refresh;
    j["desktopFps"] = s.desktopFps;
    j["desktopMaxWidth"] = s.desktopMaxWidth;
    j["monitor"] = s.monitor;
    j["audio"] = s.audio;
    j["mic"] = s.mic;
    j["micDevice"] = s.micDevice;
    j["theme"] = s.theme;
    j["customAccent"] = s.customAccent;
    j["customAccent2"] = s.customAccent2;
    j["customBg"] = s.customBg;
    j["customInk"] = s.customInk;
    j["stars"] = s.stars;
    j["bigMoon"] = s.bigMoon;
    j["clunk"] = s.clunk;
    j["blur"] = s.blur;
    j["tint"] = s.tint;
    j["corners"] = s.corners;
    j["controllerPoseMode"] = s.controllerPoseMode;
    j["controllerOffsetPos"] = {s.controllerOffsetPos[0], s.controllerOffsetPos[1], s.controllerOffsetPos[2]};
    j["controllerOffsetRot"] = {s.controllerOffsetRot[0], s.controllerOffsetRot[1], s.controllerOffsetRot[2]};
    j["autoChaperone"] = s.autoChaperone;
    j["openWindowOnStart"] = s.openWindowOnStart;
    j["startWithWindows"] = s.startWithWindows;
    j["headsetIp"] = s.headsetIp;
    j["firewallAsked"] = s.firewallAsked;
    json t = json::array();
    for (auto &d : s.trusted) t.push_back({{"id", d.id}, {"name", d.name}});
    j["trusted"] = t;
    return j;
}

template <typename T>
static bool Take(const json &j, const char *k, T &dst) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return false;
    try {
        T v = it->get<T>();
        if (v == dst) return false;
        dst = v;
        return true;
    } catch (...) {
        return false;
    }
}
static bool Take3(const json &j, const char *k, float *dst) {
    auto it = j.find(k);
    if (it == j.end() || !it->is_array() || it->size() != 3) return false;
    bool ch = false;
    for (int i = 0; i < 3; i++) {
        float v = (*it)[i].is_number() ? (*it)[i].get<float>() : 0.f;
        if (v != dst[i]) { dst[i] = v; ch = true; }
    }
    return ch;
}

static bool ApplyTo(HostSettings &s, const json &j) {
    bool ch = false;
    ch |= Take(j, "bitrateMbps", s.bitrateMbps);
    ch |= Take(j, "quality", s.quality);
    ch |= Take(j, "codec", s.codec);
    ch |= Take(j, "refresh", s.refresh);
    ch |= Take(j, "desktopFps", s.desktopFps);
    ch |= Take(j, "desktopMaxWidth", s.desktopMaxWidth);
    ch |= Take(j, "monitor", s.monitor);
    ch |= Take(j, "audio", s.audio);
    ch |= Take(j, "mic", s.mic);
    ch |= Take(j, "micDevice", s.micDevice);
    ch |= Take(j, "theme", s.theme);
    ch |= Take(j, "customAccent", s.customAccent);
    ch |= Take(j, "customAccent2", s.customAccent2);
    ch |= Take(j, "customBg", s.customBg);
    ch |= Take(j, "customInk", s.customInk);
    ch |= Take(j, "stars", s.stars);
    ch |= Take(j, "bigMoon", s.bigMoon);
    ch |= Take(j, "clunk", s.clunk);
    ch |= Take(j, "blur", s.blur);
    ch |= Take(j, "tint", s.tint);
    ch |= Take(j, "corners", s.corners);
    ch |= Take(j, "controllerPoseMode", s.controllerPoseMode);
    ch |= Take3(j, "controllerOffsetPos", s.controllerOffsetPos);
    ch |= Take3(j, "controllerOffsetRot", s.controllerOffsetRot);
    ch |= Take(j, "autoChaperone", s.autoChaperone);
    ch |= Take(j, "openWindowOnStart", s.openWindowOnStart);
    ch |= Take(j, "startWithWindows", s.startWithWindows);
    ch |= Take(j, "headsetIp", s.headsetIp);
    ch |= Take(j, "firewallAsked", s.firewallAsked);
    auto t = j.find("trusted");
    if (t != j.end() && t->is_array()) {
        s.trusted.clear();
        for (auto &d : *t) s.trusted.push_back({d.value("id", ""), d.value("name", "")});
        ch = true;
    }
    // clamp
    if (s.bitrateMbps < 5) s.bitrateMbps = 5;
    if (s.bitrateMbps > 300) s.bitrateMbps = 300;
    if (s.quality < 0) s.quality = 0;
    if (s.quality > 3) s.quality = 3;
    if (s.desktopFps < 20) s.desktopFps = 20;
    if (s.desktopFps > 120) s.desktopFps = 120;
    if (s.monitor < 0) s.monitor = 0;
    return ch;
}

void Settings::Load() {
    std::ifstream f{std::filesystem::path(SettingsPath())};
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        json j = json::parse(ss.str());
        std::lock_guard<std::mutex> lk(m_lock);
        ApplyTo(m_s, j);
    } catch (...) {
        Log("settings.json is invalid, using defaults");
    }
}

void Settings::Save() {
    json j;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        j = ToJ(m_s);
    }
    std::wstring tmp = SettingsPath() + L".tmp";
    {
        std::ofstream f(std::filesystem::path(tmp), std::ios::trunc);
        f << j.dump(2);
    }
    MoveFileExW(tmp.c_str(), SettingsPath().c_str(), MOVEFILE_REPLACE_EXISTING);
}

HostSettings Settings::Get() {
    std::lock_guard<std::mutex> lk(m_lock);
    return m_s;
}
void Settings::Set(const HostSettings &s) {
    {
        std::lock_guard<std::mutex> lk(m_lock);
        m_s = s;
    }
    Save();
}
json Settings::ToJson() {
    std::lock_guard<std::mutex> lk(m_lock);
    return ToJ(m_s);
}
bool Settings::Apply(const json &j) {
    bool ch;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        ch = ApplyTo(m_s, j);
    }
    if (ch) Save();
    return ch;
}
bool Settings::IsTrusted(const std::string &id) {
    std::lock_guard<std::mutex> lk(m_lock);
    for (auto &d : m_s.trusted)
        if (d.id == id) return true;
    return false;
}
void Settings::Trust(const std::string &id, const std::string &name) {
    {
        std::lock_guard<std::mutex> lk(m_lock);
        for (auto &d : m_s.trusted)
            if (d.id == id) { d.name = name; goto done; }
        m_s.trusted.push_back({id, name});
    }
done:
    Save();
}
void Settings::Forget(const std::string &id) {
    {
        std::lock_guard<std::mutex> lk(m_lock);
        for (size_t i = 0; i < m_s.trusted.size(); i++)
            if (m_s.trusted[i].id == id) { m_s.trusted.erase(m_s.trusted.begin() + i); break; }
    }
    Save();
}
