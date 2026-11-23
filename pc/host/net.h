// Networking: discovery, TCP control channel, UDP media.
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../../shared/lunar_protocol.h"

class Net {
public:
    struct Callbacks {
        std::function<void()> onConnected;  // TCP accepted
        std::function<void()> onDisconnected;
        std::function<void(uint16_t type, const uint8_t *data, uint32_t size)> onMessage;
        std::function<void(const LunarTracking &)> onTracking;
        std::function<void(const int16_t *samples, int frames, int channels)> onMic;
    };

    bool Start(const Callbacks &cb);
    void Stop();

    bool Connected() const { return m_client != INVALID_SOCKET; }
    std::string ClientIp() const;
    void Disconnect();

    bool Send(uint16_t type, const void *payload, uint32_t size);
    template <typename T>
    bool Send(uint16_t type, const T &v) { return Send(type, &v, sizeof(T)); }

    void SetClientUdpPort(uint16_t port) { m_clientUdpPort = port; }
    // Sends one encoded frame split into UDP chunks (paced to avoid Wi-Fi bursts).
    void SendVideo(uint32_t streamId, uint32_t frameIndex, const uint8_t *data, size_t size, bool keyframe, bool hasPose,
                   const LPose &pose, uint64_t trackingTimeUs, uint32_t bitrateKbps);
    void SendAudio(const int16_t *samples, int frames, int channels);

    uint64_t BytesSent() const { return m_bytesSent; }
    void SetHostName(const std::string &n) { m_hostName = n; }
    void SetSteamVrReady(bool r) { m_steamvrReady = r; }

private:
    void DiscoveryLoop();
    void AcceptLoop();
    void UdpLoop();
    bool RecvAll(SOCKET s, void *buf, int len);
    void SendUdp(const void *data, int len);

    Callbacks m_cb;
    std::atomic<bool> m_run{false};
    SOCKET m_disc = INVALID_SOCKET, m_listen = INVALID_SOCKET, m_udp = INVALID_SOCKET;
    std::atomic<SOCKET> m_client{INVALID_SOCKET};
    sockaddr_in m_clientAddr = {};
    std::atomic<uint16_t> m_clientUdpPort{LUNAR_PORT_CLIENT};
    std::mutex m_sendLock, m_udpLock;
    std::thread m_discThread, m_acceptThread, m_udpThread;
    std::atomic<uint64_t> m_bytesSent{0};
    std::string m_hostName;
    std::atomic<bool> m_steamvrReady{false};
    uint32_t m_audioSeq = 0;
};
