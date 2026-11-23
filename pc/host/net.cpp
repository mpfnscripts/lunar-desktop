#include "net.h"
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#include "log.h"
#include "../common/lunar_ipc.h"

bool Net::Start(const Callbacks &cb) {
    m_cb = cb;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // Discovery socket
    m_disc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL yes = TRUE;
    setsockopt(m_disc, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof(yes));
    setsockopt(m_disc, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(LUNAR_PORT_DISCOVERY);
    if (bind(m_disc, (sockaddr *)&a, sizeof(a)) != 0) Log("Discovery bind failed (%d)", WSAGetLastError());
    DWORD tmo = 1000;
    setsockopt(m_disc, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));

    // Control listener
    m_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    a.sin_port = htons(LUNAR_PORT_CONTROL);
    if (bind(m_listen, (sockaddr *)&a, sizeof(a)) != 0 || listen(m_listen, 4) != 0) {
        Log("Control port %d is busy (%d). Is Lunar Desktop already running?", LUNAR_PORT_CONTROL, WSAGetLastError());
        return false;
    }

    // Media socket
    m_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int big = 8 * 1024 * 1024;
    setsockopt(m_udp, SOL_SOCKET, SO_SNDBUF, (const char *)&big, sizeof(big));
    setsockopt(m_udp, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof(big));
    a.sin_port = htons(LUNAR_PORT_STREAM);
    if (bind(m_udp, (sockaddr *)&a, sizeof(a)) != 0) Log("Media bind failed (%d)", WSAGetLastError());
    // Ignore ICMP "port unreachable" resets on the UDP socket
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(m_udp, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
    setsockopt(m_udp, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
    // Mark media as interactive video for Wi-Fi QoS (best effort if not allowed)
    int tos = 0xB8;
    setsockopt(m_udp, IPPROTO_IP, IP_TOS, (const char *)&tos, sizeof(tos));

    m_run = true;
    m_discThread = std::thread([this] { DiscoveryLoop(); });
    m_acceptThread = std::thread([this] { AcceptLoop(); });
    m_udpThread = std::thread([this] { UdpLoop(); });
    Log("Listening: discovery %d, control %d, media %d", LUNAR_PORT_DISCOVERY, LUNAR_PORT_CONTROL, LUNAR_PORT_STREAM);
    return true;
}

void Net::Stop() {
    m_run = false;
    Disconnect();
    closesocket(m_disc);
    closesocket(m_listen);
    closesocket(m_udp);
    if (m_discThread.joinable()) m_discThread.join();
    if (m_acceptThread.joinable()) m_acceptThread.join();
    if (m_udpThread.joinable()) m_udpThread.join();
    WSACleanup();
}

std::string Net::ClientIp() const {
    char buf[64] = {};
    inet_ntop(AF_INET, &m_clientAddr.sin_addr, buf, sizeof(buf));
    return buf;
}

void Net::Disconnect() {
    SOCKET s = m_client.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
}

void Net::DiscoveryLoop() {
    int64_t lastAnnounce = 0;
    while (m_run) {
        LunarDiscovery d;
        sockaddr_in from = {};
        int fl = sizeof(from);
        int n = recvfrom(m_disc, (char *)&d, sizeof(d), 0, (sockaddr *)&from, &fl);
        LunarDiscovery ann = {};
        ann.magic = LUNAR_MAGIC;
        ann.version = LUNAR_PROTOCOL_VERSION;
        ann.type = LUNAR_DISC_ANNOUNCE;
        lunarCopyStr(ann.name, m_hostName.c_str());
        ann.controlPort = LUNAR_PORT_CONTROL;
        ann.steamvrReady = m_steamvrReady ? 1 : 0;
        if (n == (int)sizeof(d) && d.magic == LUNAR_MAGIC && d.type == LUNAR_DISC_PROBE) {
            sendto(m_disc, (const char *)&ann, sizeof(ann), 0, (sockaddr *)&from, sizeof(from));
        }
        int64_t now = lunarNowUs();
        if (now - lastAnnounce > 2000000 && !Connected()) {
            lastAnnounce = now;
            sockaddr_in bc = {};
            bc.sin_family = AF_INET;
            bc.sin_port = htons(LUNAR_PORT_DISCOVERY);
            bc.sin_addr.s_addr = INADDR_BROADCAST;
            sendto(m_disc, (const char *)&ann, sizeof(ann), 0, (sockaddr *)&bc, sizeof(bc));
        }
    }
}

bool Net::RecvAll(SOCKET s, void *buf, int len) {
    char *p = (char *)buf;
    while (len > 0) {
        int n = recv(s, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

void Net::AcceptLoop() {
    while (m_run) {
        sockaddr_in from = {};
        int fl = sizeof(from);
        SOCKET s = accept(m_listen, (sockaddr *)&from, &fl);
        if (s == INVALID_SOCKET) {
            if (!m_run) break;
            Sleep(100);
            continue;
        }
        if (Connected()) {
            Log("A new headset connected; dropping the previous one");
            Disconnect();
            Sleep(50);
        }
        BOOL yes = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));
        setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&yes, sizeof(yes));
        m_clientAddr = from;
        m_client = s;
        Log("Headset connected from %s", ClientIp().c_str());
        if (m_cb.onConnected) m_cb.onConnected();

        std::vector<uint8_t> buf;
        while (m_run && m_client == s) {
            LunarMsgHeader h;
            if (!RecvAll(s, &h, sizeof(h))) break;
            if (h.size > 16 * 1024 * 1024) break;
            buf.resize(h.size);
            if (h.size && !RecvAll(s, buf.data(), (int)h.size)) break;
            if (m_cb.onMessage) m_cb.onMessage(h.type, buf.data(), h.size);
        }
        SOCKET expected = s;
        if (m_client.compare_exchange_strong(expected, INVALID_SOCKET)) closesocket(s);
        Log("Headset disconnected");
        if (m_cb.onDisconnected) m_cb.onDisconnected();
    }
}

bool Net::Send(uint16_t type, const void *payload, uint32_t size) {
    SOCKET s = m_client;
    if (s == INVALID_SOCKET) return false;
    std::vector<uint8_t> buf(sizeof(LunarMsgHeader) + size);
    LunarMsgHeader h = {size, type, 0};
    memcpy(buf.data(), &h, sizeof(h));
    if (size) memcpy(buf.data() + sizeof(h), payload, size);
    std::lock_guard<std::mutex> lk(m_sendLock);
    const char *p = (const char *)buf.data();
    int left = (int)buf.size();
    while (left > 0) {
        int n = send(s, p, left, 0);
        if (n <= 0) return false;
        p += n;
        left -= n;
    }
    return true;
}

void Net::SendUdp(const void *data, int len) {
    if (!Connected()) return;
    sockaddr_in to = m_clientAddr;
    to.sin_port = htons(m_clientUdpPort);
    sendto(m_udp, (const char *)data, len, 0, (sockaddr *)&to, sizeof(to));
    m_bytesSent += len;
}

void Net::SendVideo(uint32_t streamId, uint32_t frameIndex, const uint8_t *data, size_t size, bool keyframe, bool hasPose,
                    const LPose &pose, uint64_t trackingTimeUs, uint32_t bitrateKbps) {
    const int payload = LUNAR_VIDEO_CHUNK_PAYLOAD;
    uint16_t count = (uint16_t)((size + payload - 1) / payload);
    uint8_t pkt[LUNAR_MAX_UDP_PAYLOAD];
    LunarVideoChunk *c = (LunarVideoChunk *)pkt;
    memset(c, 0, sizeof(*c));
    lunarInitPacket(c->h, LPKT_VIDEO);
    c->streamId = streamId;
    c->frameIndex = frameIndex;
    c->frameSize = (uint32_t)size;
    c->chunkCount = count;
    c->flags = (keyframe ? LVIDEO_FLAG_KEYFRAME : 0) | (hasPose ? LVIDEO_FLAG_HAS_POSE : 0);
    c->trackingTimeUs = trackingTimeUs;
    c->headPose = pose;

    // Pace at ~3x the target bitrate (min 250 Mbps) so a big keyframe doesn't overflow the Wi-Fi queue.
    double bytesPerUs = (double)(bitrateKbps * 3 > 250000 ? bitrateKbps * 3 : 250000) * 1000.0 / 8.0 / 1e6;
    int64_t start = lunarNowUs();
    size_t sent = 0;
    std::lock_guard<std::mutex> lk(m_udpLock);
    for (uint16_t i = 0; i < count; i++) {
        size_t off = (size_t)i * payload;
        size_t n = size - off < (size_t)payload ? size - off : (size_t)payload;
        c->chunkIndex = i;
        memcpy(pkt + sizeof(LunarVideoChunk), data + off, n);
        SendUdp(pkt, (int)(sizeof(LunarVideoChunk) + n));
        sent += n;
        if (sent > 64 * 1024) {
            int64_t due = start + (int64_t)((sent - 64 * 1024) / bytesPerUs);
            while (lunarNowUs() < due) YieldProcessor();
        }
    }
}

void Net::SendAudio(const int16_t *samples, int frames, int channels) {
    uint8_t pkt[LUNAR_MAX_UDP_PAYLOAD];
    LunarAudioPacket *a = (LunarAudioPacket *)pkt;
    lunarInitPacket(a->h, LPKT_AUDIO);
    int maxFrames = (LUNAR_MAX_UDP_PAYLOAD - (int)sizeof(LunarAudioPacket)) / (2 * channels);
    while (frames > 0) {
        int n = frames < maxFrames ? frames : maxFrames;
        a->seq = m_audioSeq++;
        a->channels = (uint16_t)channels;
        a->frames = (uint16_t)n;
        memcpy(pkt + sizeof(LunarAudioPacket), samples, n * channels * 2);
        std::lock_guard<std::mutex> lk(m_udpLock);
        SendUdp(pkt, (int)sizeof(LunarAudioPacket) + n * channels * 2);
        samples += n * channels;
        frames -= n;
    }
}

void Net::UdpLoop() {
    uint8_t buf[2048];
    while (m_run) {
        sockaddr_in from = {};
        int fl = sizeof(from);
        int n = recvfrom(m_udp, (char *)buf, sizeof(buf), 0, (sockaddr *)&from, &fl);
        if (n < (int)sizeof(LunarPacketHeader)) continue;
        auto *h = (LunarPacketHeader *)buf;
        if (h->magic != LUNAR_MAGIC) continue;
        if (!Connected() || from.sin_addr.s_addr != m_clientAddr.sin_addr.s_addr) continue;  // only the connected headset
        if (h->type == LPKT_TRACKING && n >= (int)sizeof(LunarTracking)) {
            if (m_cb.onTracking) m_cb.onTracking(*(LunarTracking *)buf);
        } else if (h->type == LPKT_MIC && n >= (int)sizeof(LunarMicPacket)) {
            auto *m = (LunarMicPacket *)buf;
            int need = (int)sizeof(LunarMicPacket) + m->frames * m->channels * 2;
            if (n >= need && m->channels >= 1 && m_cb.onMic) m_cb.onMic((const int16_t *)(buf + sizeof(LunarMicPacket)), m->frames, m->channels);
        } else if (h->type == LPKT_HELLO) {
            m_clientUdpPort = ntohs(from.sin_port);
        }
    }
}
