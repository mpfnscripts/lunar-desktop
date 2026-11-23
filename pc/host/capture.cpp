#include "capture.h"
#include <stdio.h>
#include "log.h"

template <typename T>
static void SafeRelease(T *&p) {
    if (p) { p->Release(); p = nullptr; }
}

static uint64_t LuidOf(const LUID &l) { return ((uint64_t)(uint32_t)l.HighPart << 32) | l.LowPart; }

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> list;
    IDXGIFactory1 *f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) return list;
    for (UINT a = 0;; a++) {
        IDXGIAdapter1 *ad = nullptr;
        if (f->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 ades;
        ad->GetDesc1(&ades);
        for (UINT o = 0;; o++) {
            IDXGIOutput *out = nullptr;
            if (ad->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC d;
            out->GetDesc(&d);
            if (d.AttachedToDesktop) {
                MonitorInfo m;
                m.rect = d.DesktopCoordinates;
                m.adapterLuid = LuidOf(ades.AdapterLuid);
                m.adapterIndex = a;
                m.outputIndex = o;
                char buf[128];
                snprintf(buf, sizeof(buf), "Display %d  (%ldx%ld)", (int)list.size() + 1, m.rect.right - m.rect.left, m.rect.bottom - m.rect.top);
                m.name = buf;
                list.push_back(m);
            }
            out->Release();
        }
        ad->Release();
    }
    f->Release();
    // Primary monitor (origin 0,0) first
    for (size_t i = 1; i < list.size(); i++) {
        if (list[i].rect.left == 0 && list[i].rect.top == 0) {
            std::swap(list[0], list[i]);
            break;
        }
    }
    return list;
}

ID3D11Device *CreateDeviceForLuid(uint64_t luid, uint64_t *actual) {
    IDXGIFactory1 *f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) return nullptr;
    IDXGIAdapter1 *chosen = nullptr;
    SIZE_T bestMem = 0;
    for (UINT a = 0;; a++) {
        IDXGIAdapter1 *ad = nullptr;
        if (f->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d;
        ad->GetDesc1(&d);
        if (luid && LuidOf(d.AdapterLuid) == luid) {
            SafeRelease(chosen);
            chosen = ad;
            break;
        }
        if (!luid && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory >= bestMem) {
            SafeRelease(chosen);
            chosen = ad;
            bestMem = d.DedicatedVideoMemory;
            continue;
        }
        ad->Release();
    }
    f->Release();
    if (!chosen) return nullptr;
    DXGI_ADAPTER_DESC1 d;
    chosen->GetDesc1(&d);
    if (actual) *actual = LuidOf(d.AdapterLuid);
    ID3D11Device *dev = nullptr;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                   levels, 2, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
    }
    if (SUCCEEDED(hr)) Log("D3D11 device on %s", Narrow(d.Description).c_str());
    chosen->Release();
    return SUCCEEDED(hr) ? dev : nullptr;
}

// ------------------------------------------------------------------------------------------------
bool DesktopCapture::Init(int monitorIndex) {
    Shutdown();
    auto mons = EnumerateMonitors();
    if (mons.empty()) return false;
    if (monitorIndex < 0 || monitorIndex >= (int)mons.size()) monitorIndex = 0;
    m_mon = mons[monitorIndex];

    m_dev = CreateDeviceForLuid(m_mon.adapterLuid, &m_luid);
    if (!m_dev) return false;
    m_dev->GetImmediateContext(&m_ctx);

    IDXGIDevice *dxgiDev = nullptr;
    IDXGIAdapter *adapter = nullptr;
    IDXGIOutput *output = nullptr;
    IDXGIOutput1 *output1 = nullptr;
    bool ok = SUCCEEDED(m_dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter)) &&
              SUCCEEDED(adapter->EnumOutputs(m_mon.outputIndex, &output)) &&
              SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), (void **)&output1));
    HRESULT hr = E_FAIL;
    if (ok) hr = output1->DuplicateOutput(m_dev, &m_dup);
    SafeRelease(output1);
    SafeRelease(output);
    SafeRelease(adapter);
    SafeRelease(dxgiDev);
    if (FAILED(hr)) {
        Log("DuplicateOutput failed 0x%08x", (unsigned)hr);
        Shutdown();
        return false;
    }
    DXGI_OUTDUPL_DESC dd;
    m_dup->GetDesc(&dd);
    m_w = dd.ModeDesc.Width;
    m_h = dd.ModeDesc.Height;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = m_w;
    td.Height = m_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &m_copy))) {
        Shutdown();
        return false;
    }
    m_hasFrame = false;
    Log("Capturing %s", m_mon.name.c_str());
    return true;
}

void DesktopCapture::Shutdown() {
    SafeRelease(m_dup);
    SafeRelease(m_copy);
    SafeRelease(m_ctx);
    SafeRelease(m_dev);
    m_hasFrame = false;
}

int DesktopCapture::Acquire(int timeoutMs) {
    if (!m_dup) return -1;
    DXGI_OUTDUPL_FRAME_INFO info;
    IDXGIResource *res = nullptr;
    HRESULT hr = m_dup->AcquireNextFrame(timeoutMs, &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;
    if (FAILED(hr)) return -1;
    int result = 0;
    if (info.LastPresentTime.QuadPart != 0 || !m_hasFrame) {
        ID3D11Texture2D *tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex))) {
            D3D11_TEXTURE2D_DESC td;
            tex->GetDesc(&td);
            if (td.Width == m_w && td.Height == m_h && (td.Format == DXGI_FORMAT_B8G8R8A8_UNORM || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) {
                m_ctx->CopyResource(m_copy, tex);
                m_hasFrame = true;
                result = 1;
            } else {
                result = -1;
            }
            tex->Release();
        }
    }
    res->Release();
    m_dup->ReleaseFrame();
    return result;
}

// ------------------------------------------------------------------------------------------------
bool VrFrameSource::Init(LunarIpc *ipc) {
    Shutdown();
    m_ipc = ipc;
    m_event = CreateEventW(nullptr, FALSE, FALSE, LUNAR_IPC_FRAME_EVENT);
    ID3D11Device *dev = CreateDeviceForLuid(ipc->adapterLuid, &m_luid);
    if (!dev) return false;
    HRESULT hr = dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&m_dev);
    dev->Release();
    if (FAILED(hr)) return false;
    m_lastSeq = ipc->frameSeq;
    return OpenTextures();
}

bool VrFrameSource::OpenTextures() {
    for (int i = 0; i < LUNAR_IPC_FRAME_SLOTS; i++) {
        SafeRelease(m_mutex[i]);
        SafeRelease(m_tex[i]);
    }
    m_gen = m_ipc->frameTexGeneration;
    if (!m_gen) return true;  // driver has not created them yet
    for (int i = 0; i < LUNAR_IPC_FRAME_SLOTS; i++) {
        wchar_t name[96];
        lunarFrameTexName(name, 96, m_gen, i);
        HRESULT hr = m_dev->OpenSharedResourceByName(name, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, __uuidof(ID3D11Texture2D), (void **)&m_tex[i]);
        if (FAILED(hr)) {
            Log("Could not open VR frame texture %d (0x%08x)", i, (unsigned)hr);
            return false;
        }
        m_tex[i]->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&m_mutex[i]);
    }
    D3D11_TEXTURE2D_DESC td;
    m_tex[0]->GetDesc(&td);
    m_w = td.Width;
    m_h = td.Height;
    Log("Opened VR frame textures %ux%u (generation %u)", m_w, m_h, m_gen);
    return true;
}

void VrFrameSource::Shutdown() {
    Release();
    for (int i = 0; i < LUNAR_IPC_FRAME_SLOTS; i++) {
        SafeRelease(m_mutex[i]);
        SafeRelease(m_tex[i]);
    }
    SafeRelease(m_dev);
    if (m_event) CloseHandle(m_event);
    m_event = nullptr;
    m_gen = 0;
}

bool VrFrameSource::Acquire(int timeoutMs, ID3D11Texture2D **tex, LunarIpcFrameInfo *info) {
    if (!m_dev) return false;
    if (m_ipc->frameSeq == m_lastSeq) {
        if (WaitForSingleObject(m_event, timeoutMs) != WAIT_OBJECT_0 && m_ipc->frameSeq == m_lastSeq) return false;
    }
    if (m_ipc->frameTexGeneration != m_gen && !OpenTextures()) return false;
    if (!m_tex[0]) return false;
    uint32_t seq = 0;
    if (!lunarSeqRead(&m_ipc->frameSeq, &m_ipc->latestFrame, *info, &seq)) return false;
    m_lastSeq = seq;
    int slot = (int)info->slot;
    if (slot < 0 || slot >= LUNAR_IPC_FRAME_SLOTS || !m_mutex[slot]) return false;
    HRESULT hr = m_mutex[slot]->AcquireSync(LUNAR_KEY_HOST, 20);
    if (hr != S_OK && hr != (HRESULT)WAIT_ABANDONED) return false;
    m_locked = slot;
    *tex = m_tex[slot];
    return true;
}

void VrFrameSource::Release() {
    if (m_locked >= 0 && m_mutex[m_locked]) m_mutex[m_locked]->ReleaseSync(LUNAR_KEY_DRIVER);
    m_locked = -1;
}
