#pragma once
#include <mutex>
#include <string>
#include <vector>
#include "../third_party/json/json.hpp"

struct TrustedDevice {
    std::string id;
    std::string name;
};

struct HostSettings {
    // Streaming
    int bitrateMbps = 100;        // 10..250
    int quality = 2;              // 0 low, 1 medium, 2 high, 3 ultra (VR resolution scale)
    std::string codec = "auto";   // auto | h264 | hevc
    int refresh = 0;              // 0 = headset default, else 72/80/90/120
    int desktopFps = 60;
    int desktopMaxWidth = 2560;   // 0 = native
    int monitor = 0;
    bool audio = true;
    bool mic = true;
    std::string micDevice = "CABLE Input";  // substring of a playback device name (VB-Audio Virtual Cable)
    // Look
    std::string theme = "lunar";
    std::string customAccent, customAccent2, customBg, customInk;
    bool stars = true;
    bool bigMoon = true;
    bool clunk = false;
    int blur = 26, tint = 100, corners = 26;
    // VR
    int controllerPoseMode = 0;   // 0 grip, 1 aim
    float controllerOffsetPos[3] = {0, 0, 0};
    float controllerOffsetRot[3] = {0, 0, 0};
    bool autoChaperone = true;
    // App
    bool openWindowOnStart = true;
    bool startWithWindows = false;
    std::vector<TrustedDevice> trusted;
};

class Settings {
public:
    void Load();
    void Save();
    HostSettings Get();
    void Set(const HostSettings &s);
    nlohmann::json ToJson();
    // Applies the keys present in j. Returns true if anything changed.
    bool Apply(const nlohmann::json &j);
    bool IsTrusted(const std::string &id);
    void Trust(const std::string &id, const std::string &name);
    void Forget(const std::string &id);

private:
    std::mutex m_lock;
    HostSettings m_s;
};

extern Settings g_settings;
