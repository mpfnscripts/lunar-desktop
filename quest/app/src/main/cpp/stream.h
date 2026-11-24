// Network client: finds Lunar Desktop PCs, connects, receives video/audio, sends tracking/input.
#pragma once
#include <netinet/in.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "common.h"

struct HostInfo {
    std::string name;
    std::string ip;
    bool steamvrReady = false;
    double lastSeen = 0;
};

struct FrameMeta {
    uint32_t streamId = 0;
    uint32_t frameIndex = 0;
    bool hasPose = false;
    LPose pose = {};
    uint64_t trackingTimeUs = 0;
    uint64_t receivedUs = 0;
};

class StreamClient {
public:
    struct Callbacks {
        std::function<void(const LunarStreamConfig &)> onStreamConfig;
        std::function<void(const uint8_t *, size_t, const FrameMeta &, bool keyframe)> onFrame;
        std::function<void(const int16_t *, int frames)> onAudio;
        std::function<void(uint16_t type, const uint8_t *, uint32_t)> onMessage;  // everything else
    };
    enum State { IDLE, CONNECTING, WAITING_APPROVAL, CONNECTED, FAILED };

    void start(const Callbacks &cb);
    void stop();

    std::vector<HostInfo> hosts();
    void connect(const std::string &ip);
    void disconnect();
    void cancel();  // user gave up: stop accepting the PC dialing in, and disconnect
    State state() const { return m_state; }
    std::string stateText();
    std::string hostName();
    std::string hostIp() const { return m_hostIp; }

    bool send(uint16_t type, const void *payload, uint32_t size);
    template <typename T>
    bool send(uint16_t type, const T &v) { return send(type, &v, sizeof(T)); }
    void sendTracking(const LunarTracking &t);
    void sendMic(const int16_t *samples, int frames);
    void requestIdr();

    // stats
    float lossPercent() const { return m_loss; }
    float mbps() const { return m_mbps; }
    float rttMs() const { return m_rtt; }
    uint32_t framesDropped() const { return m_dropped; }

    // filled by app before connecting
    LunarHello hello = {};

private:
    void discoveryLoop();
    void controlLoop(std::string ip);
    void reverseLoop();
    void runSession(int s, const sockaddr_in &peer);
    bool reverseAllowed(const std::string &peerIp);
    void mediaLoop();
    void onVideoChunk(const LunarVideoChunk *c, const uint8_t *payload, size_t n);
    void deliverFrame(size_t slot);

    Callbacks m_cb;
    std::atomic<bool> m_run{false};
    int m_discSock = -1, m_mediaSock = -1;
    std::atomic<int> m_ctrlSock{-1};
    std::thread m_discThread, m_mediaThread, m_ctrlThread, m_revThread;
    int m_revListen = -1;
    std::string m_wantIp, m_wantName;  // the PC the user picked (guarded by m_stateLock)
    std::mutex m_hostsLock, m_sendLock, m_stateLock;
    std::vector<HostInfo> m_hosts;
    std::atomic<State> m_state{IDLE};
    std::string m_stateText, m_hostName, m_hostIp;
    sockaddr_in m_hostMedia = {};
    std::atomic<bool> m_haveHostMedia{false};
    std::atomic<uint32_t> m_streamId{0};
    uint32_t m_trackSeq = 0, m_micSeq = 0;

    // reassembly (media thread only)
    struct Assembly {
        bool used = false;
        uint32_t streamId = 0, frameIndex = 0, size = 0;
        uint16_t count = 0, got = 0;
        bool key = false;
        std::vector<uint8_t> data;
        std::vector<uint8_t> have;
        FrameMeta meta;
        uint64_t firstUs = 0;
    };
    Assembly m_slots[8];
    uint32_t m_lastDelivered = 0;
    bool m_needKey = true;
    uint64_t m_lastIdrRequest = 0;

    std::atomic<float> m_loss{0}, m_mbps{0}, m_rtt{0};
    std::atomic<uint32_t> m_dropped{0};
    uint64_t m_bytesWindow = 0, m_windowStart = 0;
    uint32_t m_framesWindow = 0, m_lostWindow = 0;
};
