#include "stream.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

void StreamClient::start(const Callbacks &cb) {
    m_cb = cb;
    m_run = true;
    int yes = 1;

    m_discSock = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(m_discSock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(m_discSock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(LUNAR_PORT_DISCOVERY);
    if (bind(m_discSock, (sockaddr *)&a, sizeof(a)) != 0) LOGW("discovery bind failed");
    timeval tv = {0, 300000};
    setsockopt(m_discSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    m_mediaSock = socket(AF_INET, SOCK_DGRAM, 0);
    int big = 16 * 1024 * 1024;
    setsockopt(m_mediaSock, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
    setsockopt(m_mediaSock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    a.sin_port = htons(LUNAR_PORT_CLIENT);
    if (bind(m_mediaSock, (sockaddr *)&a, sizeof(a)) != 0) LOGW("media bind failed");
    tv = {0, 200000};
    setsockopt(m_mediaSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int tos = 0xB8;
    setsockopt(m_mediaSock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    m_discThread = std::thread([this] { discoveryLoop(); });
    m_mediaThread = std::thread([this] { mediaLoop(); });
}

void StreamClient::stop() {
    m_run = false;
    disconnect();
    if (m_discThread.joinable()) m_discThread.join();
    if (m_mediaThread.joinable()) m_mediaThread.join();
    close(m_discSock);
    close(m_mediaSock);
}

std::vector<HostInfo> StreamClient::hosts() {
    std::lock_guard<std::mutex> lk(m_hostsLock);
    std::vector<HostInfo> out;
    double now = NowSec();
    for (auto &h : m_hosts)
        if (now - h.lastSeen < 6.0) out.push_back(h);
    return out;
}

std::string StreamClient::stateText() {
    std::lock_guard<std::mutex> lk(m_stateLock);
    return m_stateText;
}
std::string StreamClient::hostName() {
    std::lock_guard<std::mutex> lk(m_stateLock);
    return m_hostName;
}

void StreamClient::discoveryLoop() {
    double lastProbe = 0;
    while (m_run) {
        double now = NowSec();
        if (now - lastProbe > 1.0) {
            lastProbe = now;
            if (m_state != CONNECTED) {
                LunarDiscovery d = {};
                d.magic = LUNAR_MAGIC;
                d.version = LUNAR_PROTOCOL_VERSION;
                d.type = LUNAR_DISC_PROBE;
                lunarCopyStr(d.name, hello.deviceName);
                sockaddr_in bc = {};
                bc.sin_family = AF_INET;
                bc.sin_port = htons(LUNAR_PORT_DISCOVERY);
                bc.sin_addr.s_addr = INADDR_BROADCAST;
                sendto(m_discSock, &d, sizeof(d), 0, (sockaddr *)&bc, sizeof(bc));
            }
            // keep the host's view of our media port fresh
            if (m_haveHostMedia) {
                LunarPacketHeader h;
                lunarInitPacket(h, LPKT_HELLO);
                sendto(m_mediaSock, &h, sizeof(h), 0, (sockaddr *)&m_hostMedia, sizeof(m_hostMedia));
            }
        }
        LunarDiscovery d;
        sockaddr_in from = {};
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(m_discSock, &d, sizeof(d), 0, (sockaddr *)&from, &fl);
        if (n == (ssize_t)sizeof(d) && d.magic == LUNAR_MAGIC && d.type == LUNAR_DISC_ANNOUNCE) {
            char ip[32];
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            d.name[sizeof(d.name) - 1] = 0;
            std::lock_guard<std::mutex> lk(m_hostsLock);
            bool found = false;
            for (auto &h : m_hosts) {
                if (h.ip == ip) {
                    h.name = d.name;
                    h.steamvrReady = d.steamvrReady;
                    h.lastSeen = NowSec();
                    found = true;
                }
            }
            if (!found) m_hosts.push_back({d.name, ip, d.steamvrReady != 0, NowSec()});
        }
    }
}

void StreamClient::connect(const std::string &ip) {
    disconnect();
    {
        std::lock_guard<std::mutex> lk(m_stateLock);
        m_hostIp = ip;
        m_hostName = ip;
        for (auto &h : hosts())
            if (h.ip == ip) m_hostName = h.name;
        m_stateText = "Connecting to " + m_hostName + (m_hostName != ip ? " (" + ip + ")" : "") + "…";
    }
    m_state = CONNECTING;
    m_ctrlThread = std::thread([this, ip] { controlLoop(ip); });
}

void StreamClient::disconnect() {
    int s = m_ctrlSock.exchange(-1);
    if (s >= 0) {
        shutdown(s, SHUT_RDWR);
        close(s);
    }
    if (m_ctrlThread.joinable()) m_ctrlThread.join();
    m_haveHostMedia = false;
    m_state = IDLE;
}

static bool RecvAll(int s, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    while (len) {
        ssize_t n = recv(s, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

void StreamClient::controlLoop(std::string ip) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(LUNAR_PORT_CONTROL);
    inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    ::connect(s, (sockaddr *)&a, sizeof(a));
    pollfd pfd = {s, POLLOUT, 0};
    int err = 1;
    socklen_t el = sizeof(err);
    if (poll(&pfd, 1, 4000) == 1) getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err != 0) {
        close(s);
        std::lock_guard<std::mutex> lk(m_stateLock);
        m_stateText = "Couldn't reach " + m_hostName + " at " + ip + ".";
        m_state = FAILED;
        return;
    }
    fcntl(s, F_SETFL, flags);
    int yes = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    m_ctrlSock = s;

    m_hostMedia = {};
    m_hostMedia.sin_family = AF_INET;
    m_hostMedia.sin_port = htons(LUNAR_PORT_STREAM);
    m_hostMedia.sin_addr = a.sin_addr;
    m_haveHostMedia = true;

    hello.protocolVersion = LUNAR_PROTOCOL_VERSION;
    hello.udpPort = LUNAR_PORT_CLIENT;
    send(LMSG_HELLO, hello);
    {
        std::lock_guard<std::mutex> lk(m_stateLock);
        m_stateText = "Connecting to " + m_hostName + "…";
    }
    LOGI("connected to %s", ip.c_str());

    std::vector<uint8_t> buf;
    while (m_run && m_ctrlSock == s) {
        LunarMsgHeader h;
        if (!RecvAll(s, &h, sizeof(h)) || h.size > (1 << 24)) break;
        buf.resize(h.size);
        if (h.size && !RecvAll(s, buf.data(), h.size)) break;
        switch (h.type) {
            case LMSG_WELCOME:
                if (h.size >= sizeof(LunarWelcome)) {
                    auto *w = (LunarWelcome *)buf.data();
                    w->hostName[sizeof(w->hostName) - 1] = 0;
                    std::lock_guard<std::mutex> lk(m_stateLock);
                    m_hostName = w->hostName;
                    m_stateText = "Connected to " + m_hostName;
                }
                m_state = CONNECTED;
                if (m_cb.onMessage) m_cb.onMessage(h.type, buf.data(), h.size);
                break;
            case LMSG_DENIED: {
                std::lock_guard<std::mutex> lk(m_stateLock);
                m_stateText = "On your PC, click Allow in Lunar Desktop";
                m_state = WAITING_APPROVAL;
                break;
            }
            case LMSG_STREAM_CONFIG:
                if (h.size >= sizeof(LunarStreamConfig)) {
                    auto *c = (LunarStreamConfig *)buf.data();
                    m_streamId = c->streamId;
                    if (m_cb.onStreamConfig) m_cb.onStreamConfig(*c);
                }
                break;
            case LMSG_PONG:
                if (h.size >= sizeof(LunarPing)) {
                    auto *p = (LunarPing *)buf.data();
                    float rtt = (NowUs() - p->clientTimeUs) / 1000.f;
                    m_rtt = m_rtt == 0 ? rtt : m_rtt * 0.8f + rtt * 0.2f;
                }
                break;
            default:
                if (m_cb.onMessage) m_cb.onMessage(h.type, buf.data(), h.size);
                break;
        }
    }
    int expected = s;
    if (m_ctrlSock.compare_exchange_strong(expected, -1)) close(s);
    m_haveHostMedia = false;
    if (m_state != IDLE) {
        std::lock_guard<std::mutex> lk(m_stateLock);
        m_stateText = "Lost connection to " + m_hostName;
        m_state = FAILED;
    }
    LOGI("control connection closed");
}

bool StreamClient::send(uint16_t type, const void *payload, uint32_t size) {
    int s = m_ctrlSock;
    if (s < 0) return false;
    std::vector<uint8_t> buf(sizeof(LunarMsgHeader) + size);
    LunarMsgHeader h = {size, type, 0};
    memcpy(buf.data(), &h, sizeof(h));
    if (size) memcpy(buf.data() + sizeof(h), payload, size);
    std::lock_guard<std::mutex> lk(m_sendLock);
    size_t off = 0;
    while (off < buf.size()) {
        ssize_t n = ::send(s, buf.data() + off, buf.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += n;
    }
    return true;
}

void StreamClient::sendTracking(const LunarTracking &t) {
    if (!m_haveHostMedia) return;
    LunarTracking c = t;
    lunarInitPacket(c.h, LPKT_TRACKING);
    c.seq = m_trackSeq++;
    sendto(m_mediaSock, &c, sizeof(c), 0, (sockaddr *)&m_hostMedia, sizeof(m_hostMedia));
}

void StreamClient::sendMic(const int16_t *samples, int frames) {
    if (!m_haveHostMedia) return;
    uint8_t pkt[LUNAR_MAX_UDP_PAYLOAD];
    auto *m = (LunarMicPacket *)pkt;
    lunarInitPacket(m->h, LPKT_MIC);
    m->seq = m_micSeq++;
    m->channels = 1;
    m->frames = (uint16_t)frames;
    memcpy(pkt + sizeof(LunarMicPacket), samples, frames * 2);
    sendto(m_mediaSock, pkt, sizeof(LunarMicPacket) + frames * 2, 0, (sockaddr *)&m_hostMedia, sizeof(m_hostMedia));
}

void StreamClient::requestIdr() {
    uint64_t now = NowUs();
    if (now - m_lastIdrRequest < 150000) return;
    m_lastIdrRequest = now;
    send(LMSG_REQUEST_IDR, nullptr, 0);
}

void StreamClient::mediaLoop() {
    std::vector<uint8_t> buf(65536);
    uint32_t curStream = 0;
    while (m_run) {
        sockaddr_in from = {};
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(m_mediaSock, buf.data(), buf.size(), 0, (sockaddr *)&from, &fl);
        uint64_t now = NowUs();
        if (now - m_windowStart > 1000000) {
            double secs = (now - m_windowStart) / 1e6;
            if (m_windowStart) {
                m_mbps = (float)(m_bytesWindow * 8 / 1e6 / secs);
                uint32_t total = m_framesWindow + m_lostWindow;
                m_loss = total ? 100.f * m_lostWindow / total : 0.f;
            }
            m_bytesWindow = 0;
            m_framesWindow = m_lostWindow = 0;
            m_windowStart = now;
        }
        if (n < (ssize_t)sizeof(LunarPacketHeader) || !m_haveHostMedia) continue;
        if (from.sin_addr.s_addr != m_hostMedia.sin_addr.s_addr) continue;
        auto *h = (LunarPacketHeader *)buf.data();
        if (h->magic != LUNAR_MAGIC) continue;
        m_bytesWindow += n;
        if (h->type == LPKT_VIDEO && n >= (ssize_t)sizeof(LunarVideoChunk)) {
            auto *c = (LunarVideoChunk *)buf.data();
            if (c->streamId != m_streamId) continue;
            if (c->streamId != curStream) {
                curStream = c->streamId;
                for (auto &s : m_slots) s.used = false;
                m_lastDelivered = 0;
                m_needKey = true;
            }
            onVideoChunk(c, buf.data() + sizeof(LunarVideoChunk), n - sizeof(LunarVideoChunk));
        } else if (h->type == LPKT_AUDIO && n >= (ssize_t)sizeof(LunarAudioPacket)) {
            auto *a = (LunarAudioPacket *)buf.data();
            if (a->channels == 2 && n >= (ssize_t)(sizeof(LunarAudioPacket) + a->frames * 4) && m_cb.onAudio)
                m_cb.onAudio((const int16_t *)(buf.data() + sizeof(LunarAudioPacket)), a->frames);
        }
    }
}

void StreamClient::onVideoChunk(const LunarVideoChunk *c, const uint8_t *payload, size_t n) {
    if (m_lastDelivered && c->frameIndex <= m_lastDelivered && m_lastDelivered - c->frameIndex < 100000) return;  // late
    if (c->chunkCount == 0 || c->chunkIndex >= c->chunkCount || c->frameSize > 16 * 1024 * 1024) return;
    size_t slot = c->frameIndex % 8;
    Assembly &a = m_slots[slot];
    if (!a.used || a.frameIndex != c->frameIndex) {
        if (a.used && a.got < a.count) {
            m_lostWindow++;
            m_dropped++;
        }
        a.used = true;
        a.streamId = c->streamId;
        a.frameIndex = c->frameIndex;
        a.size = c->frameSize;
        a.count = c->chunkCount;
        a.got = 0;
        a.key = (c->flags & LVIDEO_FLAG_KEYFRAME) != 0;
        a.data.resize(a.size);
        a.have.assign(a.count, 0);
        a.firstUs = NowUs();
        a.meta = FrameMeta();
        a.meta.streamId = c->streamId;
        a.meta.frameIndex = c->frameIndex;
        a.meta.hasPose = (c->flags & LVIDEO_FLAG_HAS_POSE) != 0;
        a.meta.pose = c->headPose;
        a.meta.trackingTimeUs = c->trackingTimeUs;
    }
    if (a.have[c->chunkIndex]) return;
    size_t off = (size_t)c->chunkIndex * LUNAR_VIDEO_CHUNK_PAYLOAD;
    if (off + n > a.size) return;
    memcpy(a.data.data() + off, payload, n);
    a.have[c->chunkIndex] = 1;
    a.got++;
    if (a.got == a.count) deliverFrame(slot);
}

void StreamClient::deliverFrame(size_t slot) {
    Assembly &a = m_slots[slot];
    a.meta.receivedUs = NowUs();
    // anything older that is still incomplete is lost
    for (auto &s : m_slots) {
        if (s.used && &s != &a && s.frameIndex < a.frameIndex) {
            if (s.got < s.count) {
                m_lostWindow++;
                m_dropped++;
            }
            s.used = false;
        }
    }
    bool gap = m_lastDelivered != 0 && a.frameIndex != m_lastDelivered + 1;
    if (gap && !a.key) m_needKey = true;
    if (a.key) m_needKey = false;
    m_lastDelivered = a.frameIndex;
    a.used = false;
    m_framesWindow++;
    if (m_needKey) {
        requestIdr();
        return;
    }
    if (m_cb.onFrame) m_cb.onFrame(a.data.data(), a.size, a.meta, a.key);
}
