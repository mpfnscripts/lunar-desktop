#include "audio.h"
#include <windows.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <math.h>
#include "log.h"

template <typename T>
static void SafeRelease(T *&p) {
    if (p) { p->Release(); p = nullptr; }
}

static const PROPERTYKEY kPKEY_FriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
static const CLSID kCLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
static const IID kIID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);

static std::string DeviceFriendlyName(IMMDevice *d) {
    IPropertyStore *ps = nullptr;
    std::string name;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kPKEY_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = Narrow(v.pwszVal);
        PropVariantClear(&v);
        ps->Release();
    }
    return name;
}

std::vector<std::string> ListPlaybackDevices() {
    std::vector<std::string> out;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void **)&en))) return out;
    IMMDeviceCollection *col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            IMMDevice *d = nullptr;
            if (SUCCEEDED(col->Item(i, &d))) {
                out.push_back(DeviceFriendlyName(d));
                d->Release();
            }
        }
        col->Release();
    }
    en->Release();
    return out;
}

// Reads one sample from a WASAPI buffer as float
static inline float ReadSample(const BYTE *p, const WAVEFORMATEX *wf, bool isFloat) {
    if (isFloat) return *(const float *)p;
    if (wf->wBitsPerSample == 16) return *(const int16_t *)p / 32768.f;
    if (wf->wBitsPerSample == 32) return *(const int32_t *)p / 2147483648.f;
    if (wf->wBitsPerSample == 24) {
        int32_t v = (p[0] << 8) | (p[1] << 16) | (p[2] << 24);
        return v / 2147483648.f;
    }
    return 0.f;
}

static bool IsFloatFormat(const WAVEFORMATEX *wf) {
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto *ex = (const WAVEFORMATEXTENSIBLE *)wf;
        static const GUID kFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        return IsEqualGUID(ex->SubFormat, kFloat) != 0;
    }
    return false;
}

static inline int16_t ToS16(float v) {
    v *= 32767.f;
    if (v > 32767.f) v = 32767.f;
    if (v < -32768.f) v = -32768.f;
    return (int16_t)lrintf(v);
}

// ------------------------------------------------------------------------------------------------
void AudioCapture::Start(std::function<void(const int16_t *, int)> cb) {
    if (m_run) return;
    m_cb = cb;
    m_run = true;
    m_thread = std::thread([this] { Loop(); });
}

void AudioCapture::Stop() {
    m_run = false;
    if (m_thread.joinable()) m_thread.join();
}

void AudioCapture::Loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (m_run) {
        IMMDeviceEnumerator *en = nullptr;
        IMMDevice *dev = nullptr;
        IAudioClient *client = nullptr;
        IAudioCaptureClient *cap = nullptr;
        WAVEFORMATEX *wf = nullptr;
        bool ok = SUCCEEDED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void **)&en)) &&
                  SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
                  SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client)) &&
                  SUCCEEDED(client->GetMixFormat(&wf)) &&
                  SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, wf, nullptr)) &&
                  SUCCEEDED(client->GetService(__uuidof(IAudioCaptureClient), (void **)&cap)) && SUCCEEDED(client->Start());
        if (ok) {
            Log("Audio capture: %s, %lu Hz, %u ch", DeviceFriendlyName(dev).c_str(), wf->nSamplesPerSec, wf->nChannels);
            m_running = true;
            bool isFloat = IsFloatFormat(wf);
            int ch = wf->nChannels;
            int bps = wf->wBitsPerSample / 8;
            double step = (double)wf->nSamplesPerSec / 48000.0;
            double pos = 0;
            float prev[2] = {0, 0};
            std::vector<int16_t> out;
            int64_t lastCheck = GetTickCount64();
            while (m_run) {
                Sleep(4);
                UINT32 pkt = 0;
                HRESULT hr;
                while (SUCCEEDED(hr = cap->GetNextPacketSize(&pkt)) && pkt > 0) {
                    BYTE *data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                    out.clear();
                    bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    // linear resample to 48 kHz stereo
                    for (UINT32 i = 0; i < frames; i++) {
                        float cur[2];
                        for (int c = 0; c < 2; c++) {
                            int sc = c < ch ? c : 0;
                            cur[c] = silent ? 0.f : ReadSample(data + (i * ch + sc) * bps, wf, isFloat);
                        }
                        while (pos < 1.0) {
                            out.push_back(ToS16(prev[0] + (cur[0] - prev[0]) * (float)pos));
                            out.push_back(ToS16(prev[1] + (cur[1] - prev[1]) * (float)pos));
                            pos += step;
                        }
                        pos -= 1.0;
                        prev[0] = cur[0];
                        prev[1] = cur[1];
                    }
                    cap->ReleaseBuffer(frames);
                    if (!out.empty() && m_cb) m_cb(out.data(), (int)out.size() / 2);
                }
                if (FAILED(hr)) break;  // device changed / removed
                // Follow the default device if the user switches it
                if (GetTickCount64() - lastCheck > 2000) {
                    lastCheck = GetTickCount64();
                    IMMDevice *cur = nullptr;
                    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &cur))) {
                        LPWSTR a = nullptr, b = nullptr;
                        cur->GetId(&a);
                        dev->GetId(&b);
                        bool changed = a && b && wcscmp(a, b) != 0;
                        CoTaskMemFree(a);
                        CoTaskMemFree(b);
                        cur->Release();
                        if (changed) break;
                    }
                }
            }
            client->Stop();
            m_running = false;
        } else {
            Sleep(1000);
        }
        if (wf) CoTaskMemFree(wf);
        SafeRelease(cap);
        SafeRelease(client);
        SafeRelease(dev);
        SafeRelease(en);
    }
    CoUninitialize();
}

// ------------------------------------------------------------------------------------------------
void MicOutput::Start(const std::string &dev) {
    Stop();
    {
        std::lock_guard<std::mutex> lk(m_lock);
        m_ring.assign(48000, 0);  // 1 second
        m_read = m_write = 0;
    }
    m_run = true;
    m_thread = std::thread([this, dev] { Loop(dev); });
}

void MicOutput::Stop() {
    m_run = false;
    if (m_thread.joinable()) m_thread.join();
    m_active = false;
}

std::string MicOutput::DeviceName() const {
    std::lock_guard<std::mutex> lk(m_lock);
    return m_deviceName;
}

void MicOutput::Push(const int16_t *s, int frames) {
    if (!m_active) return;
    std::lock_guard<std::mutex> lk(m_lock);
    size_t cap = m_ring.size();
    for (int i = 0; i < frames; i++) {
        m_ring[m_write % cap] = s[i];
        m_write++;
    }
    // drop old audio if we fall more than 150 ms behind
    if (m_write - m_read > 7200) m_read = m_write - 2400;
}

static std::string Lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

void MicOutput::Loop(std::string want) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (m_run) {
        IMMDeviceEnumerator *en = nullptr;
        IMMDevice *dev = nullptr;
        IAudioClient *client = nullptr;
        IAudioRenderClient *render = nullptr;
        WAVEFORMATEX *wf = nullptr;
        std::string name;
        if (SUCCEEDED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void **)&en))) {
            IMMDeviceCollection *col = nullptr;
            if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
                UINT n = 0;
                col->GetCount(&n);
                for (UINT i = 0; i < n && !dev; i++) {
                    IMMDevice *d = nullptr;
                    if (SUCCEEDED(col->Item(i, &d))) {
                        std::string fn = DeviceFriendlyName(d);
                        if (!want.empty() && Lower(fn).find(Lower(want)) != std::string::npos) {
                            dev = d;
                            name = fn;
                        } else {
                            d->Release();
                        }
                    }
                }
                col->Release();
            }
        }
        bool ok = dev && SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client)) &&
                  SUCCEEDED(client->GetMixFormat(&wf)) &&
                  SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 400000, 0, wf, nullptr)) &&
                  SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), (void **)&render)) && SUCCEEDED(client->Start());
        if (ok) {
            {
                std::lock_guard<std::mutex> lk(m_lock);
                m_deviceName = name;
            }
            Log("Headset microphone -> %s", name.c_str());
            m_active = true;
            bool isFloat = IsFloatFormat(wf);
            int ch = wf->nChannels;
            int bps = wf->wBitsPerSample / 8;
            double step = 48000.0 / wf->nSamplesPerSec;
            double pos = 0;
            UINT32 bufFrames = 0;
            client->GetBufferSize(&bufFrames);
            while (m_run) {
                Sleep(5);
                UINT32 padding = 0;
                if (FAILED(client->GetCurrentPadding(&padding))) break;
                // keep ~40 ms queued in the device
                UINT32 target = wf->nSamplesPerSec / 25;
                if (padding >= target) continue;
                UINT32 frames = target - padding;
                if (frames > bufFrames - padding) frames = bufFrames - padding;
                BYTE *data = nullptr;
                if (FAILED(render->GetBuffer(frames, &data))) break;
                std::lock_guard<std::mutex> lk(m_lock);
                size_t cap = m_ring.size();
                for (UINT32 i = 0; i < frames; i++) {
                    float v = 0.f;
                    if (m_write - m_read > 1) {
                        float a = m_ring[m_read % cap] / 32768.f, b = m_ring[(m_read + 1) % cap] / 32768.f;
                        v = a + (b - a) * (float)pos;
                        pos += step;
                        while (pos >= 1.0) {
                            pos -= 1.0;
                            m_read++;
                        }
                    }
                    for (int c = 0; c < ch; c++) {
                        BYTE *p = data + (i * ch + c) * bps;
                        if (isFloat) *(float *)p = v;
                        else if (bps == 2) *(int16_t *)p = ToS16(v);
                        else if (bps == 4) *(int32_t *)p = (int32_t)(v * 2147483647.f);
                        else memset(p, 0, bps);
                    }
                }
                render->ReleaseBuffer(frames, 0);
            }
            client->Stop();
            m_active = false;
        }
        if (wf) CoTaskMemFree(wf);
        SafeRelease(render);
        SafeRelease(client);
        SafeRelease(dev);
        SafeRelease(en);
        for (int i = 0; i < 30 && m_run; i++) Sleep(100);  // retry every 3 s (device may appear later)
    }
    CoUninitialize();
}
