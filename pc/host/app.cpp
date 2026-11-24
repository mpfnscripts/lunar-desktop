#include "app.h"
#include <mmsystem.h>
#include <shellapi.h>
#include <dxgi.h>
#include <math.h>
#include <mfapi.h>
#include "encoder.h"
#include "input.h"
#include "log.h"
#include "steamvr.h"

using nlohmann::json;
App g_app;
std::atomic<int> g_firewallOk{-1};

static bool RunHiddenWait(const std::wstring &cmdline, DWORD *exitCode) {
    std::wstring cmd = cmdline;
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 10000);
    GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// True if our inbound allow rule exists (netsh returns 0 when a rule matches).
bool FirewallRuleExists() {
    DWORD code = 1;
    if (!RunHiddenWait(L"netsh.exe advfirewall firewall show rule name=\"Lunar Desktop\"", &code)) return true;  // can't tell: don't nag
    return code == 0;
}

// Removes any rules Windows made for this exe (including "Block" rules created when its
// firewall popup was cancelled) and adds one allow rule for every network type.
// Asks for admin rights through the normal Windows prompt.
bool FixFirewall() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring e = exe;
    std::wstring args = L"/c netsh advfirewall firewall delete rule name=all program=\"" + e +
                        L"\" & netsh advfirewall firewall add rule name=\"Lunar Desktop\" dir=in action=allow enable=yes profile=any program=\"" + e + L"\"";
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        Log("Firewall fix declined");
        return false;
    }
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 15000);
        CloseHandle(sei.hProcess);
    }
    bool ok = FirewallRuleExists();
    Log("Firewall rule %s", ok ? "added" : "still missing");
    return ok;
}

static const char *kVersion = "1.0.0";

static std::string ComputerName() {
    wchar_t buf[256];
    DWORD n = 256;
    if (GetComputerNameW(buf, &n)) return Narrow(buf);
    return "PC";
}

bool App::Start() {
    m_ipc = lunarIpcOpen(&m_ipcMapping);
    if (!m_ipc) {
        Log("Could not create shared memory for the SteamVR driver");
        return false;
    }
    m_ipc->hostHeartbeatUs = lunarNowUs();
    m_net.SetHostName(ComputerName());
    m_net.SetSteamVrReady(steamvr::IsDriverRegistered());

    Net::Callbacks cb;
    cb.onConnected = [this] { OnConnected(); };
    cb.onDisconnected = [this] { OnDisconnected(); };
    cb.onMessage = [this](uint16_t t, const uint8_t *d, uint32_t s) { OnMessage(t, d, s); };
    cb.onTracking = [this](const LunarTracking &t) { OnTracking(t); };
    cb.onMic = [this](const int16_t *s, int frames, int ch) {
        if (ch == 1) {
            m_mic.Push(s, frames);
        } else {
            std::vector<int16_t> mono(frames);
            for (int i = 0; i < frames; i++) mono[i] = s[i * ch];
            m_mic.Push(mono.data(), frames);
        }
    };
    if (!m_net.Start(cb)) return false;

    m_run = true;
    OnSettingsChanged();
    m_videoThread = std::thread([this] { VideoLoop(); });
    m_serviceThread = std::thread([this] { ServiceLoop(); });
    return true;
}

void App::Stop() {
    m_run = false;
    m_net.Stop();
    if (m_videoThread.joinable()) m_videoThread.join();
    if (m_serviceThread.joinable()) m_serviceThread.join();
    m_audio.Stop();
    m_mic.Stop();
    if (m_ipc) {
        m_ipc->headsetConnected = 0;
        m_ipc->hostHeartbeatUs = 0;
        UnmapViewOfFile(m_ipc);
    }
    if (m_ipcMapping) CloseHandle(m_ipcMapping);
    m_ipc = nullptr;
}

// ------------------------------------------------------------------------------------------------
// Settings
// ------------------------------------------------------------------------------------------------
void App::OnSettingsChanged() {
    HostSettings s = g_settings.Get();
    m_bitrateKbps = (uint32_t)s.bitrateMbps * 1000;
    m_net.SetManualHeadsetIp(s.headsetIp);
    if (m_ipc) {
        m_ipc->controllerPoseMode = s.controllerPoseMode;
        for (int i = 0; i < 3; i++) {
            m_ipc->controllerOffsetPos[i] = s.controllerOffsetPos[i];
            m_ipc->controllerOffsetRotDeg[i] = s.controllerOffsetRot[i];
        }
    }
    if (s.audio && m_streamAllowed) {
        m_audio.Start([this](const int16_t *samples, int frames) {
            if (m_streamAllowed) m_net.SendAudio(samples, frames, 2);
        });
    } else {
        m_audio.Stop();
    }
    if (s.mic && m_streamAllowed) {
        if (!m_mic.Active() || m_mic.DeviceName().find(s.micDevice) == std::string::npos) m_mic.Start(s.micDevice);
    } else {
        m_mic.Stop();
    }
    {
        std::lock_guard<std::mutex> lk(m_lock);
        if (m_haveView) {
            ComputeEyeSize();
            WriteDisplayConfig();
        }
    }
    // Only restart the video stream when something that affects it changed.
    char key[160];
    snprintf(key, sizeof(key), "%d|%s|%d|%d|%d|%d", s.quality, s.codec.c_str(), s.refresh, s.desktopFps, s.desktopMaxWidth, s.monitor);
    if (m_streamKey != key) {
        m_streamKey = key;
        m_restart = true;
    }
    SendHostState();
}

// ------------------------------------------------------------------------------------------------
// Headset connection
// ------------------------------------------------------------------------------------------------
void App::OnConnected() {
    std::lock_guard<std::mutex> lk(m_lock);
    m_gotHello = m_trusted = m_haveView = false;
    m_streamAllowed = false;
}

void App::OnDisconnected() {
    {
        std::lock_guard<std::mutex> lk(m_lock);
        m_gotHello = m_trusted = m_haveView = false;
        m_pendingId.clear();
    }
    m_streamAllowed = false;
    if (m_ipc) m_ipc->headsetConnected = 0;
    m_audio.Stop();
    m_mic.Stop();
}

void App::SendWelcome() {
    LunarWelcome w = {};
    lunarCopyStr(w.hostName, ComputerName().c_str());
    lunarCopyStr(w.hostVersion, kVersion);
    m_net.Send(LMSG_WELCOME, w);
}

static int Round32(double v) { return ((int)(v + 16) / 32) * 32; }

void App::ComputeEyeSize() {
    // m_lock held
    HostSettings s = g_settings.Get();
    static const double scale[4] = {0.6, 0.75, 0.9, 1.0};
    uint32_t rw = m_hello.recommendedEyeWidth ? m_hello.recommendedEyeWidth : 1832;
    uint32_t rh = m_hello.recommendedEyeHeight ? m_hello.recommendedEyeHeight : 1920;
    int w = Round32(rw * scale[s.quality]);
    int h = Round32(rh * scale[s.quality]);
    if (w < 640) w = 640;
    if (h < 640) h = 640;
    m_ipc->eyeWidth = w;
    m_ipc->eyeHeight = h;
}

void App::WriteDisplayConfig() {
    // m_lock held
    m_ipc->refreshRate = m_view.refreshRate > 30 ? m_view.refreshRate : 72.f;
    for (int i = 0; i < 2; i++) {
        m_ipc->fov[i] = m_view.fov[i];
        m_ipc->eyeInHead[i] = m_view.eyeInHead[i];
    }
    lunarCopyStr(m_ipc->headsetName, m_hello.deviceName);
    m_ipc->secondsToPhotons = 0.035f;
    MemoryBarrier();
    m_ipc->displayGeneration++;
}

void App::OnMessage(uint16_t type, const uint8_t *data, uint32_t size) {
    switch (type) {
        case LMSG_HELLO: {
            if (size < sizeof(LunarHello)) return;
            LunarHello h;
            memcpy(&h, data, sizeof(h));
            h.deviceId[sizeof(h.deviceId) - 1] = 0;
            h.deviceName[sizeof(h.deviceName) - 1] = 0;
            bool trusted = g_settings.IsTrusted(h.deviceId);
            {
                std::lock_guard<std::mutex> lk(m_lock);
                m_hello = h;
                m_gotHello = true;
                m_trusted = trusted;
                if (!trusted) {
                    m_pendingId = h.deviceId;
                    m_pendingName = h.deviceName;
                }
            }
            m_net.SetClientUdpPort(h.udpPort ? h.udpPort : LUNAR_PORT_CLIENT);
            Log("Hello from %s (%s), eye %ux%u @ %.0f Hz, hevc %d", h.deviceName, trusted ? "trusted" : "new", h.recommendedEyeWidth,
                h.recommendedEyeHeight, h.currentRefreshRate, h.supportsHevc);
            if (trusted) {
                SendWelcome();
                SendHostState();
            } else {
                char reason[128] = "Approve this headset in Lunar Desktop on your PC";
                m_net.Send(LMSG_DENIED, reason, sizeof(reason));
                if (notify) notify("Headset wants to connect", std::string(h.deviceName) + " is asking to connect. Open Lunar Desktop to allow it.");
                if (openUi) openUi();
            }
            break;
        }
        case LMSG_VIEW_CONFIG: {
            if (size < sizeof(LunarViewConfig)) return;
            std::lock_guard<std::mutex> lk(m_lock);
            if (!m_trusted) return;
            memcpy(&m_view, data, sizeof(m_view));
            m_haveView = true;
            ComputeEyeSize();
            WriteDisplayConfig();
            if (!m_streamAllowed) {
                m_streamAllowed = true;
                m_ipc->headsetConnected = 1;
                Log("Streaming to %s", m_hello.deviceName);
                // start audio/mic outside the lock
                std::thread([this] { OnSettingsChanged(); }).detach();
            } else {
                m_restart = true;
            }
            break;
        }
        case LMSG_REQUEST_IDR:
            m_forceIdr = true;
            break;
        case LMSG_POINTER:
            if (size >= sizeof(LunarPointer) && m_streamAllowed && m_mode == LUNAR_MODE_DESKTOP) {
                LunarPointer p;
                memcpy(&p, data, sizeof(p));
                RECT r;
                {
                    std::lock_guard<std::mutex> lk(m_lock);
                    r = m_monitorRect;
                }
                InjectPointer(p, r);
            }
            break;
        case LMSG_KEY:
            if (size >= sizeof(LunarKey) && m_streamAllowed) {
                LunarKey k;
                memcpy(&k, data, sizeof(k));
                InjectKey(k.vk, k.down != 0);
            }
            break;
        case LMSG_TEXT:
            if (m_streamAllowed) InjectText(std::string((const char *)data, size));
            break;
        case LMSG_SET_SETTING: {
            if (size < sizeof(LunarSetting) || !m_streamAllowed) return;
            LunarSetting s;
            memcpy(&s, data, sizeof(s));
            s.key[sizeof(s.key) - 1] = 0;
            s.value[sizeof(s.value) - 1] = 0;
            json j;
            std::string v = s.value;
            // numbers and booleans arrive as text
            if (v == "true" || v == "false") j[s.key] = v == "true";
            else if (!v.empty() && (isdigit((unsigned char)v[0]) || v[0] == '-') && v.find_first_not_of("-0123456789.") == std::string::npos)
                j[s.key] = v.find('.') != std::string::npos ? json(atof(v.c_str())) : json(atoi(v.c_str()));
            else j[s.key] = v;
            Log("Headset changed setting %s = %s", s.key, s.value);
            if (g_settings.Apply(j)) OnSettingsChanged();
            break;
        }
        case LMSG_ACTION: {
            if (size < sizeof(LunarAction) || !m_streamAllowed) return;
            LunarAction a;
            memcpy(&a, data, sizeof(a));
            a.arg[sizeof(a.arg) - 1] = 0;
            switch (a.action) {
                case LACTION_LAUNCH_STEAMVR: Action("launch_steamvr", json::object()); break;
                case LACTION_STOP_STEAMVR: Action("stop_steamvr", json::object()); break;
                case LACTION_NEXT_MONITOR:
                case LACTION_PREV_MONITOR: {
                    HostSettings s = g_settings.Get();
                    int n = m_monitorCount > 0 ? m_monitorCount.load() : 1;
                    int m = (s.monitor + (a.action == LACTION_NEXT_MONITOR ? 1 : n - 1)) % n;
                    if (g_settings.Apply(json{{"monitor", m}})) OnSettingsChanged();
                    break;
                }
                default: break;
            }
            break;
        }
        case LMSG_CLIENT_STATS:
            if (size >= sizeof(LunarClientStats)) {
                std::lock_guard<std::mutex> lk(m_lock);
                memcpy(&m_clientStats, data, sizeof(m_clientStats));
            }
            break;
        case LMSG_PING:
            if (size >= sizeof(LunarPing)) {
                LunarPing p;
                memcpy(&p, data, sizeof(p));
                p.hostTimeUs = (uint64_t)lunarNowUs();
                m_net.Send(LMSG_PONG, p);
            }
            break;
        default:
            break;
    }
}

void App::OnTracking(const LunarTracking &t) {
    if (!m_streamAllowed || !m_ipc) return;
    lunarSeqWrite(&m_ipc->trackingSeq, &m_ipc->tracking, t);
    m_ipc->trackingReceivedUs = lunarNowUs();
}

void App::SendHostState() {
    if (!m_streamAllowed && !m_trusted) return;
    HostSettings s = g_settings.Get();
    LunarHostState st = {};
    st.steamvrRunning = m_steamvrRunning;
    st.driverConnected = m_ipc && m_ipc->driverHeartbeatUs && lunarNowUs() - m_ipc->driverHeartbeatUs < 2000000;
    st.driverRegistered = steamvr::IsDriverRegistered();
    st.vrFramesActive = m_mode == LUNAR_MODE_VR;
    st.monitorIndex = (uint8_t)s.monitor;
    st.monitorCount = (uint8_t)m_monitorCount;
    st.audioStreaming = m_audio.Running();
    st.micActive = m_mic.Active();
    lunarCopyStr(st.theme, s.theme.c_str());
    lunarCopyStr(st.customAccent, s.customAccent.c_str());
    lunarCopyStr(st.customAccent2, s.customAccent2.c_str());
    lunarCopyStr(st.customBg, s.customBg.c_str());
    lunarCopyStr(st.customInk, s.customInk.c_str());
    st.bitrateKbps = s.bitrateMbps * 1000;
    st.quality = (uint8_t)s.quality;
    st.codecPref = s.codec == "h264" ? 1 : (s.codec == "hevc" ? 2 : 0);
    st.refreshPref = (uint8_t)s.refresh;
    st.clunkMode = s.clunk;
    lunarCopyStr(st.micDevice, m_mic.Active() ? m_mic.DeviceName().c_str() : "");
    m_net.Send(LMSG_HOST_STATE, st);
}

// ------------------------------------------------------------------------------------------------
// Video
// ------------------------------------------------------------------------------------------------
bool App::VrActive() {
    if (!m_ipc || !m_ipc->driverHeartbeatUs) return false;
    int64_t now = lunarNowUs();
    if (now - m_ipc->driverHeartbeatUs > 1500000) return false;
    return m_lastVrChangeUs && now - m_lastVrChangeUs < 2000000;
}

int App::ChooseCodec(uint64_t luid) {
    HostSettings s = g_settings.Get();
    bool clientHevc;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        clientHevc = m_hello.supportsHevc != 0;
    }
    if (s.codec == "h264") return LUNAR_CODEC_H264;
    bool hostHevc = VideoEncoder::SupportsHevc(luid);
    if (clientHevc && hostHevc) return LUNAR_CODEC_HEVC;
    return LUNAR_CODEC_H264;
}

struct FrameMeta {
    uint64_t id = 0;
    bool hasPose = false;
    LPose pose = {};
    uint64_t trackingTimeUs = 0;
};

void App::VideoLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    DesktopCapture desk;
    VrFrameSource vr;
    VideoEncoder enc;
    Nv12Converter conv;
    int mode = LUNAR_MODE_IDLE;
    uint64_t frameIndex = 0;
    int64_t lastEncode = 0, lastFail = 0;
    bool dirty = false;
    uint32_t vrGen = 0;
    FrameMeta meta[32];
    double encodeMsAcc = 0;
    int encodeCount = 0;
    int64_t statsStart = lunarNowUs();
    uint32_t statsFrames = 0;

    auto teardown = [&] {
        enc.Shutdown();
        conv.Shutdown();
        desk.Shutdown();
        vr.Shutdown();
        mode = LUNAR_MODE_IDLE;
        m_mode = LUNAR_MODE_IDLE;
    };

    auto encodeAndSend = [&](ID3D11Texture2D *nv12, const FrameMeta &m) {
        bool idr = m_forceIdr.exchange(false);
        enc.SetBitrate(m_bitrateKbps);
        meta[m.id % 32] = m;
        std::vector<EncodedFrame> out;
        int64_t t0 = lunarNowUs();
        enc.Encode(nv12, m.id, idr, out);
        encodeMsAcc += (lunarNowUs() - t0) / 1000.0;
        encodeCount++;
        for (auto &f : out) {
            const FrameMeta &fm = meta[f.id % 32];
            bool match = fm.id == f.id;
            m_net.SendVideo(m_streamId, (uint32_t)f.id, f.data.data(), f.data.size(), f.keyframe, match && fm.hasPose, fm.pose,
                            match ? fm.trackingTimeUs : 0, m_bitrateKbps);
            m_framesSent++;
            statsFrames++;
        }
    };

    while (m_run) {
        // stats
        int64_t now = lunarNowUs();
        if (now - statsStart > 1000000) {
            double secs = (now - statsStart) / 1e6;
            m_fps = (float)(statsFrames / secs);
            m_encodeMsAvg = encodeCount ? (float)(encodeMsAcc / encodeCount) : 0.f;
            statsFrames = 0;
            encodeMsAcc = 0;
            encodeCount = 0;
            statsStart = now;
        }

        if (!m_streamAllowed || !m_net.Connected()) {
            if (mode != LUNAR_MODE_IDLE) teardown();
            Sleep(50);
            continue;
        }
        int want = VrActive() ? LUNAR_MODE_VR : LUNAR_MODE_DESKTOP;
        bool restart = m_restart.exchange(false);
        if (want != mode || restart) {
            teardown();
            if (now - lastFail < 1000000) {
                Sleep(100);
                m_restart = true;
                continue;
            }
            HostSettings s = g_settings.Get();
            float fps;
            {
                std::lock_guard<std::mutex> lk(m_lock);
                fps = m_view.refreshRate > 30 ? m_view.refreshRate : 72.f;
            }
            bool ok = false;
            uint32_t contentW = 0, contentH = 0;
            uint64_t luid = 0;
            ID3D11Device *dev = nullptr;
            if (want == LUNAR_MODE_VR) {
                ok = vr.Init(m_ipc);
                dev = vr.Device();
                luid = vr.Luid();
                m_encW = m_ipc->eyeWidth * 2;
                m_encH = m_ipc->eyeHeight;
                contentW = m_encW;
                contentH = m_encH;
                m_sourceName = "SteamVR";
            } else {
                auto mons = EnumerateMonitors();
                m_monitorCount = (int)mons.size();
                ok = desk.Init(s.monitor);
                if (ok) {
                    dev = desk.Device();
                    luid = desk.Luid();
                    contentW = desk.Width();
                    contentH = desk.Height();
                    double w = contentW, h = contentH;
                    if (s.desktopMaxWidth > 0 && w > s.desktopMaxWidth) {
                        h = h * s.desktopMaxWidth / w;
                        w = s.desktopMaxWidth;
                    }
                    m_encW = ((uint32_t)w) & ~15u;
                    m_encH = ((uint32_t)h + 1) & ~1u;
                    fps = (float)(s.desktopFps < fps ? s.desktopFps : fps);
                    std::lock_guard<std::mutex> lk(m_lock);
                    m_monitorRect = desk.Monitor().rect;
                    m_sourceName = desk.Monitor().name;
                }
            }
            if (ok) {
                m_codec = ChooseCodec(luid);
                // H.264 encoders top out at 4096 pixels wide
                if (m_codec == LUNAR_CODEC_H264 && m_encW > 4096) {
                    m_encH = ((uint32_t)(m_encH * 4096.0 / m_encW) + 1) & ~1u;
                    m_encW = 4096;
                }
                ok = enc.Init(dev, luid, m_codec, m_encW, m_encH, fps, m_bitrateKbps);
                if (!ok && m_codec == LUNAR_CODEC_HEVC) {
                    m_codec = LUNAR_CODEC_H264;
                    if (m_encW > 4096) {
                        m_encH = ((uint32_t)(m_encH * 4096.0 / m_encW) + 1) & ~1u;
                        m_encW = 4096;
                    }
                    ok = enc.Init(dev, luid, m_codec, m_encW, m_encH, fps, m_bitrateKbps);
                }
                if (ok) {
                    // Raise our GPU priority a little so encoding keeps up while a game is running
                    IDXGIDevice *dd = nullptr;
                    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd))) {
                        dd->SetGPUThreadPriority(5);
                        dd->Release();
                    }
                }
            }
            if (!ok) {
                Log("Could not start %s stream", want == LUNAR_MODE_VR ? "VR" : "desktop");
                teardown();
                lastFail = lunarNowUs();
                m_restart = true;
                continue;
            }
            mode = want;
            m_mode = want;
            m_encoderName = enc.Name();
            m_streamId++;
            vrGen = 0;
            dirty = true;

            LunarStreamConfig sc = {};
            sc.streamId = m_streamId;
            sc.mode = (uint8_t)mode;
            sc.codec = (uint8_t)m_codec;
            sc.width = m_encW;
            sc.height = m_encH;
            sc.contentWidth = contentW;
            sc.contentHeight = contentH;
            sc.fps = fps;
            sc.bitrateKbps = m_bitrateKbps;
            {
                std::lock_guard<std::mutex> lk(m_lock);
                sc.fov[0] = m_view.fov[0];
                sc.fov[1] = m_view.fov[1];
                lunarCopyStr(sc.sourceName, m_sourceName.c_str());
            }
            m_net.Send(LMSG_STREAM_CONFIG, sc);
            m_forceIdr = true;
            SendHostState();
            Log("Streaming %s (%ux%u %s)", mode == LUNAR_MODE_VR ? "SteamVR" : "desktop", m_encW, m_encH, m_codec ? "HEVC" : "H.264");
        }

        if (mode == LUNAR_MODE_VR) {
            ID3D11Texture2D *tex = nullptr;
            LunarIpcFrameInfo info;
            if (!vr.Acquire(50, &tex, &info)) continue;
            if (vr.Generation() != vrGen) {
                vrGen = vr.Generation();
                conv.ClearInputViews();
            }
            D3D11_TEXTURE2D_DESC td;
            tex->GetDesc(&td);
            if (!conv.Matches(td.Width, td.Height, td.Format, m_encW, m_encH) &&
                !conv.Init(vr.Device(), td.Width, td.Height, td.Format, m_encW, m_encH)) {
                vr.Release();
                Log("Video processor init failed");
                m_restart = true;
                lastFail = lunarNowUs();
                continue;
            }
            RECT src = {0, 0, (LONG)td.Width, (LONG)td.Height};
            bool ok = conv.Convert(tex, src);
            vr.Release();
            if (!ok) continue;
            FrameMeta m;
            m.id = ++frameIndex;
            m.hasPose = true;
            m.pose = info.headPose;
            m.trackingTimeUs = info.trackingTimeUs;
            encodeAndSend(conv.Output(), m);
        } else if (mode == LUNAR_MODE_DESKTOP) {
            HostSettings s = g_settings.Get();
            int64_t period = (int64_t)(1000000.0 / (s.desktopFps > 0 ? s.desktopFps : 60));
            int64_t due = lastEncode + period;
            int timeout = dirty ? (int)((due - lunarNowUs()) / 1000) : 50;
            if (timeout < 0) timeout = 0;
            int r = desk.Acquire(timeout);
            if (r < 0) {
                Log("Desktop capture lost, restarting");
                m_restart = true;
                Sleep(100);
                continue;
            }
            if (r == 1) dirty = true;
            now = lunarNowUs();
            bool keepalive = now - lastEncode > 250000;
            if (!((dirty && now >= due) || keepalive) || !desk.HasFrame()) continue;
            if (!conv.Matches(desk.Width(), desk.Height(), DXGI_FORMAT_B8G8R8A8_UNORM, m_encW, m_encH) &&
                !conv.Init(desk.Device(), desk.Width(), desk.Height(), DXGI_FORMAT_B8G8R8A8_UNORM, m_encW, m_encH)) {
                Log("Video processor init failed");
                m_restart = true;
                lastFail = lunarNowUs();
                continue;
            }
            RECT src = {0, 0, (LONG)desk.Width(), (LONG)desk.Height()};
            if (!conv.Convert(desk.Texture(), src)) continue;
            FrameMeta m;
            m.id = ++frameIndex;
            encodeAndSend(conv.Output(), m);
            lastEncode = now;
            dirty = false;
        }
    }
    teardown();
    timeEndPeriod(1);
    CoUninitialize();
}

// ------------------------------------------------------------------------------------------------
// Service loop: heartbeat, haptics, cursor, VR detection, host state
// ------------------------------------------------------------------------------------------------
void App::ServiceLoop() {
    timeBeginPeriod(1);
    int64_t lastSlow = 0, lastCursor = 0, lastStateSend = 0;
    uint64_t lastBytes = 0;
    LunarCursor lastCur = {};
    while (m_run) {
        int64_t now = lunarNowUs();
        m_ipc->hostHeartbeatUs = now;

        // VR frame activity
        uint32_t c = m_ipc->vrFrameCount;
        if (c != m_lastVrCount) {
            m_lastVrCount = c;
            m_lastVrChangeUs = now;
        }

        // haptics from SteamVR -> headset
        while (m_hapticRead != m_ipc->hapticWrite) {
            if (m_ipc->hapticWrite - m_hapticRead > LUNAR_IPC_HAPTIC_RING) m_hapticRead = m_ipc->hapticWrite - LUNAR_IPC_HAPTIC_RING;
            LunarHaptic h = m_ipc->haptics[m_hapticRead % LUNAR_IPC_HAPTIC_RING];
            m_hapticRead++;
            if (m_streamAllowed) m_net.Send(LMSG_HAPTIC, h);
        }

        // cursor position in desktop mode (~90 Hz)
        if (m_streamAllowed && m_mode == LUNAR_MODE_DESKTOP && now - lastCursor > 11000) {
            lastCursor = now;
            CURSORINFO ci = {sizeof(ci)};
            LunarCursor cur = {};
            RECT r;
            {
                std::lock_guard<std::mutex> lk(m_lock);
                r = m_monitorRect;
            }
            if (GetCursorInfo(&ci) && r.right > r.left) {
                cur.visible = (ci.flags & CURSOR_SHOWING) && ci.ptScreenPos.x >= r.left && ci.ptScreenPos.x < r.right &&
                              ci.ptScreenPos.y >= r.top && ci.ptScreenPos.y < r.bottom;
                cur.x = (float)(ci.ptScreenPos.x - r.left) / (r.right - r.left);
                cur.y = (float)(ci.ptScreenPos.y - r.top) / (r.bottom - r.top);
            }
            if (cur.visible != lastCur.visible || fabsf(cur.x - lastCur.x) > 1e-5f || fabsf(cur.y - lastCur.y) > 1e-5f) {
                lastCur = cur;
                m_net.Send(LMSG_CURSOR, cur);
            }
        }

        if (now - lastSlow > 1000000) {
            double secs = (now - lastSlow) / 1e6;
            lastSlow = now;
            uint64_t b = m_net.BytesSent();
            m_mbps = (float)((b - lastBytes) * 8.0 / 1e6 / (secs > 0 ? secs : 1));
            lastBytes = b;
            static uint32_t lastVrCountForFps = 0;
            m_vrFps = (float)((c - lastVrCountForFps) / (secs > 0 ? secs : 1));
            lastVrCountForFps = c;

            bool running = steamvr::IsRunning();
            if (running != m_steamvrRunning) {
                m_steamvrRunning = running;
                m_chaperoneDone = false;
                m_driverSeenUs = 0;
                SendHostState();
            }
            m_net.SetSteamVrReady(steamvr::IsDriverRegistered());
            bool driverAlive = m_ipc->driverHeartbeatUs && now - m_ipc->driverHeartbeatUs < 2000000;
            if (running && driverAlive && !m_driverSeenUs) m_driverSeenUs = now;
            if (running && driverAlive && !m_chaperoneDone && m_driverSeenUs && now - m_driverSeenUs > 4000000 && g_settings.Get().autoChaperone) {
                m_chaperoneDone = true;
                std::thread([] { steamvr::SetupChaperone(); }).detach();
            }

            if (m_streamAllowed) {
                LunarHostStats st = {};
                st.encodeMs = m_encodeMsAvg;
                st.fps = m_fps;
                st.bitrateMbps = m_mbps;
                st.vrRenderFps = m_vrFps;
                m_net.Send(LMSG_HOST_STATS, st);
                if (now - lastStateSend > 5000000) {
                    lastStateSend = now;
                    SendHostState();
                }
            }
        }
        Sleep(2);
    }
    timeEndPeriod(1);
}

// ------------------------------------------------------------------------------------------------
// UI
// ------------------------------------------------------------------------------------------------
json App::StateJson() {
    json j;
    int64_t now = lunarNowUs();
    j["version"] = kVersion;
    j["computer"] = ComputerName();
    {
        std::lock_guard<std::mutex> lk(m_lock);
        j["connected"] = m_net.Connected() && m_gotHello;
        j["trusted"] = m_trusted;
        j["streaming"] = m_streamAllowed.load();
        j["headset"] = m_gotHello ? m_hello.deviceName : "";
        j["headsetIp"] = m_net.Connected() ? m_net.ClientIp() : "";
        j["refresh"] = m_view.refreshRate;
        j["pending"] = m_pendingId.empty() ? json(nullptr) : json{{"id", m_pendingId}, {"name", m_pendingName}};
        j["client"] = {{"decodeMs", m_clientStats.decodeMs}, {"fps", m_clientStats.fps}, {"loss", m_clientStats.lossPercent},
                       {"networkMs", m_clientStats.networkMs}, {"dropped", m_clientStats.framesDropped}};
        j["eye"] = {m_ipc->eyeWidth, m_ipc->eyeHeight};
    }
    int mode = m_mode;
    j["mode"] = mode == LUNAR_MODE_VR ? "vr" : (mode == LUNAR_MODE_DESKTOP ? "desktop" : "idle");
    j["encoder"] = m_encoderName;
    j["codec"] = m_codec == LUNAR_CODEC_HEVC ? "HEVC" : "H.264";
    j["resolution"] = {m_encW, m_encH};
    j["source"] = m_sourceName;
    j["fps"] = m_fps;
    j["mbps"] = m_mbps;
    j["encodeMs"] = m_encodeMsAvg;
    j["vrFps"] = m_vrFps;
    j["steamvr"] = {{"installed", steamvr::IsInstalled()},
                    {"registered", steamvr::IsDriverRegistered()},
                    {"running", m_steamvrRunning},
                    {"driverConnected", m_ipc->driverHeartbeatUs && now - m_ipc->driverHeartbeatUs < 2000000},
                    {"openxrIsSteamVr", steamvr::OpenXrIsSteamVr()},
                    {"openxrRuntime", steamvr::ActiveOpenXrRuntime()},
                    {"openxrUserOverride", !steamvr::UserOpenXrOverride().empty()}};
    j["audio"] = {{"capturing", m_audio.Running()}, {"micActive", m_mic.Active()}, {"micDevice", m_mic.DeviceName()}};
    json mons = json::array();
    for (auto &m : EnumerateMonitors()) mons.push_back(m.name);
    j["monitors"] = mons;
    j["playbackDevices"] = ListPlaybackDevices();
    j["settings"] = g_settings.ToJson();
    j["firewall"] = g_firewallOk.load();
    j["log"] = LogTail(60);
    return j;
}

std::string App::Action(const std::string &name, const json &args) {
    if (name == "allow") {
        std::string id, devName;
        {
            std::lock_guard<std::mutex> lk(m_lock);
            id = m_pendingId;
            devName = m_pendingName;
            if (id.empty()) return "Nothing to allow";
            m_pendingId.clear();
            m_trusted = true;
        }
        g_settings.Trust(id, devName);
        Log("Allowed headset %s", devName.c_str());
        SendWelcome();
        SendHostState();
        return "";
    }
    if (name == "deny") {
        {
            std::lock_guard<std::mutex> lk(m_lock);
            m_pendingId.clear();
        }
        m_net.Disconnect();
        return "";
    }
    if (name == "forget") {
        g_settings.Forget(args.value("id", ""));
        return "";
    }
    if (name == "disconnect") {
        m_net.Disconnect();
        return "";
    }
    if (name == "register_driver") {
        std::string err;
        if (steamvr::IsRunning()) return "Close SteamVR first, then try again.";
        if (!steamvr::RegisterDriver(&err)) return err;
        m_net.SetSteamVrReady(true);
        return "";
    }
    if (name == "launch_steamvr") {
        if (!steamvr::IsDriverRegistered()) {
            std::string err;
            if (!steamvr::IsRunning() && !steamvr::RegisterDriver(&err)) return err;
        }
        steamvr::Launch();
        return "";
    }
    if (name == "stop_steamvr") {
        std::thread([] { steamvr::Stop(); }).detach();
        return "";
    }
    if (name == "chaperone") {
        return steamvr::SetupChaperone() ? "" : "SteamVR must be running with the headset connected.";
    }
    if (name == "idr") {
        m_forceIdr = true;
        return "";
    }
    if (name == "open_logs") {
        ShellExecuteW(nullptr, L"open", AppDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return "";
    }
    if (name == "firewall") {
        bool ok = FixFirewall();
        g_firewallOk = ok ? 1 : 0;
        return ok ? "" : "Windows did not allow the change.";
    }
    if (name == "openxr_steamvr") {
        std::string err;
        return steamvr::UseSteamVrForOpenXr(&err) ? "" : err;
    }
    if (name == "openxr_undo") {
        steamvr::UndoSteamVrForOpenXr();
        return "";
    }
    if (name == "openxr_settings") {
        steamvr::Launch();
        return "";
    }
    return "Unknown action";
}
