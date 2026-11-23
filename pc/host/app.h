// The Lunar Desktop host: owns the headset connection and decides what to stream
// (the PC desktop, or SteamVR when a VR game is running).
#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include "audio.h"
#include "capture.h"
#include "net.h"
#include "settings.h"
#include "../common/lunar_ipc.h"
#include "../third_party/json/json.hpp"

class App {
public:
    bool Start();
    void Stop();
    nlohmann::json StateJson();
    // Returns an error message or "" on success.
    std::string Action(const std::string &name, const nlohmann::json &args);
    void OnSettingsChanged();

    // UI hooks (set by main)
    void (*notify)(const std::string &title, const std::string &text) = nullptr;
    void (*openUi)() = nullptr;

private:
    void OnConnected();
    void OnDisconnected();
    void OnMessage(uint16_t type, const uint8_t *data, uint32_t size);
    void OnTracking(const LunarTracking &t);
    void VideoLoop();
    void ServiceLoop();
    void SendHostState();
    void SendWelcome();
    void ComputeEyeSize();
    void WriteDisplayConfig();
    bool VrActive();
    int ChooseCodec(uint64_t luid);

    Net m_net;
    LunarIpc *m_ipc = nullptr;
    HANDLE m_ipcMapping = nullptr;
    AudioCapture m_audio;
    MicOutput m_mic;
    std::thread m_videoThread, m_serviceThread;
    std::atomic<bool> m_run{false};

    std::mutex m_lock;
    // connected headset
    bool m_gotHello = false;
    bool m_trusted = false;
    bool m_haveView = false;
    LunarHello m_hello = {};
    LunarViewConfig m_view = {};
    LunarClientStats m_clientStats = {};
    std::string m_pendingId, m_pendingName;
    std::atomic<bool> m_streamAllowed{false};

    // stream state
    std::atomic<int> m_mode{LUNAR_MODE_IDLE};
    std::atomic<bool> m_restart{false}, m_forceIdr{false};
    std::atomic<uint32_t> m_bitrateKbps{100000};
    uint32_t m_streamId = 0;
    std::string m_encoderName, m_sourceName;
    uint32_t m_encW = 0, m_encH = 0;
    int m_codec = 0;
    RECT m_monitorRect = {};
    std::atomic<int> m_monitorCount{1};

    // stats
    std::atomic<uint32_t> m_framesSent{0};
    float m_encodeMsAvg = 0;
    float m_fps = 0, m_mbps = 0, m_vrFps = 0;
    float m_rttMs = 0;

    // VR detection
    uint32_t m_lastVrCount = 0;
    int64_t m_lastVrChangeUs = 0;
    bool m_steamvrRunning = false;
    bool m_chaperoneDone = false;
    int64_t m_driverSeenUs = 0;
    uint32_t m_hapticRead = 0;
};

extern App g_app;
