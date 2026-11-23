#include "compositor.h"
#include <d3dcompiler.h>
#include <stdio.h>
#include "../../shared/lunar_math.h"

void DriverLog(const char *fmt, ...);

template <typename T>
static void SafeRelease(T *&p) {
    if (p) { p->Release(); p = nullptr; }
}

static const char *kShader = R"(
cbuffer CB : register(b0) { float4 bounds; float linearIn; float opaque; float2 pad; };
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vsmain(uint id : SV_VertexID) {
    VSOut o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = lerp(bounds.xy, bounds.zw, t);
    return o;
}
float3 toSrgb(float3 c) {
    c = saturate(c);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return lerp(lo, hi, step(0.0031308, c));
}
float4 psmain(VSOut i) : SV_Target {
    float4 c = tex.Sample(smp, i.uv);
    if (linearIn > 0.5) c.rgb = toSrgb(c.rgb);
    if (opaque > 0.5) c.a = 1.0;
    return c;
}
)";

struct ShaderCB {
    float bounds[4];
    float linearIn;
    float opaque;
    float pad[2];
};

typedef HRESULT(WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static bool IsSrgb(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}
static bool IsFloat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT || f == DXGI_FORMAT_R11G11B10_FLOAT ||
           f == DXGI_FORMAT_R16G16B16A16_TYPELESS;
}
static DXGI_FORMAT ViewFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return f;
    }
}

bool LunarCompositor::Init(LunarIpc *ipc) {
    m_ipc = ipc;

    // Pick the adapter with the most dedicated video memory (the gaming GPU).
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory))) return false;
    IDXGIAdapter1 *best = nullptr;
    SIZE_T bestMem = 0;
    for (UINT i = 0;; i++) {
        IDXGIAdapter1 *a = nullptr;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory >= bestMem) {
            SafeRelease(best);
            best = a;
            bestMem = d.DedicatedVideoMemory;
        } else {
            a->Release();
        }
    }
    factory->Release();
    if (!best) return false;
    DXGI_ADAPTER_DESC1 desc;
    best->GetDesc1(&desc);
    m_luid = ((uint64_t)(uint32_t)desc.AdapterLuid.HighPart << 32) | desc.AdapterLuid.LowPart;
    DriverLog("Lunar compositor using adapter: %ls", desc.Description);

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device *dev = nullptr;
    HRESULT hr = D3D11CreateDevice(best, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                   D3D11_SDK_VERSION, &dev, nullptr, &m_ctx);
    best->Release();
    if (FAILED(hr)) {
        DriverLog("D3D11CreateDevice failed 0x%08x", (unsigned)hr);
        return false;
    }
    hr = dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&m_dev);
    dev->Release();
    if (FAILED(hr)) return false;

    if (!CreateShaders()) return false;
    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, LUNAR_IPC_FRAME_EVENT);
    m_ipc->adapterLuid = m_luid;
    return true;
}

bool LunarCompositor::CreateShaders() {
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!dll) {
        DriverLog("d3dcompiler_47.dll not found");
        return false;
    }
    auto compile = (PFN_D3DCompile)GetProcAddress(dll, "D3DCompile");
    if (!compile) return false;
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(compile(kShader, strlen(kShader), "lunar", nullptr, nullptr, "vsmain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err)) ||
        FAILED(compile(kShader, strlen(kShader), "lunar", nullptr, nullptr, "psmain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err))) {
        if (err) DriverLog("shader error: %s", (const char *)err->GetBufferPointer());
        SafeRelease(err);
        SafeRelease(vsb);
        return false;
    }
    m_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_vs);
    m_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_ps);
    vsb->Release();
    psb->Release();

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(ShaderCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_dev->CreateBuffer(&bd, nullptr, &m_cb);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    m_dev->CreateSamplerState(&sd, &m_sampler);

    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_dev->CreateBlendState(&bl, &m_blendOff);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    m_dev->CreateBlendState(&bl, &m_blendOn);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    m_dev->CreateRasterizerState(&rd, &m_raster);
    return m_vs && m_ps && m_cb && m_sampler && m_blendOff && m_blendOn && m_raster;
}

void LunarCompositor::ReleaseOutput() {
    for (int i = 0; i < LUNAR_IPC_FRAME_SLOTS; i++) {
        SafeRelease(m_outRtv[i]);
        SafeRelease(m_outMutex[i]);
        SafeRelease(m_out[i]);
        if (m_outHandle[i]) CloseHandle(m_outHandle[i]);
        m_outHandle[i] = nullptr;
    }
    m_outW = m_outH = 0;
}

bool LunarCompositor::EnsureOutput(uint32_t w, uint32_t h) {
    if (m_out[0] && w == m_outW && h == m_outH) return true;
    ReleaseOutput();
    uint32_t gen = m_ipc->frameTexGeneration + 1;
    for (int i = 0; i < LUNAR_IPC_FRAME_SLOTS; i++) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &m_out[i]))) {
            DriverLog("failed to create output texture %ux%u", w, h);
            ReleaseOutput();
            return false;
        }
        m_dev->CreateRenderTargetView(m_out[i], nullptr, &m_outRtv[i]);
        m_out[i]->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&m_outMutex[i]);
        IDXGIResource1 *res = nullptr;
        m_out[i]->QueryInterface(__uuidof(IDXGIResource1), (void **)&res);
        wchar_t name[96];
        lunarFrameTexName(name, 96, gen, i);
        HRESULT hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, name, &m_outHandle[i]);
        res->Release();
        if (FAILED(hr)) {
            DriverLog("CreateSharedHandle failed 0x%08x", (unsigned)hr);
            ReleaseOutput();
            return false;
        }
    }
    m_outW = w;
    m_outH = h;
    m_ipc->frameTexWidth = w;
    m_ipc->frameTexHeight = h;
    m_ipc->frameTexFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    MemoryBarrier();
    m_ipc->frameTexGeneration = gen;
    DriverLog("Lunar output textures %ux%u (generation %u)", w, h, gen);
    return true;
}

void LunarCompositor::Shutdown() {
    std::lock_guard<std::mutex> lk(m_lock);
    for (auto *s : m_sets) {
        for (int i = 0; i < 3; i++) {
            SafeRelease(s->srv[i]);
            SafeRelease(s->tex[i]);
        }
        delete s;
    }
    m_sets.clear();
    m_handleMap.clear();
    ReleaseOutput();
    for (int i = 0; i < 2; i++) {
        SafeRelease(m_resolveSrv[i]);
        SafeRelease(m_resolveTex[i]);
    }
    SafeRelease(m_vs);
    SafeRelease(m_ps);
    SafeRelease(m_cb);
    SafeRelease(m_sampler);
    SafeRelease(m_blendOff);
    SafeRelease(m_blendOn);
    SafeRelease(m_raster);
    SafeRelease(m_ctx);
    SafeRelease(m_dev);
    if (m_frameEvent) CloseHandle(m_frameEvent);
    m_frameEvent = nullptr;
}

void LunarCompositor::CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t *pDesc, SwapTextureSet_t *pOut) {
    std::lock_guard<std::mutex> lk(m_lock);
    auto *set = new SwapSet();
    set->pid = unPid;
    set->format = (DXGI_FORMAT)pDesc->nFormat;
    set->samples = pDesc->nSampleCount ? pDesc->nSampleCount : 1;
    set->linear = IsSrgb(set->format) || IsFloat(set->format);
    for (int i = 0; i < 3; i++) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = pDesc->nWidth;
        td.Height = pDesc->nHeight;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = set->format;
        td.SampleDesc.Count = set->samples;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        HRESULT hr = m_dev->CreateTexture2D(&td, nullptr, &set->tex[i]);
        if (FAILED(hr)) {
            DriverLog("CreateSwapTextureSet: CreateTexture2D failed 0x%08x fmt %u", (unsigned)hr, pDesc->nFormat);
            continue;
        }
        IDXGIResource *res = nullptr;
        set->tex[i]->QueryInterface(__uuidof(IDXGIResource), (void **)&res);
        res->GetSharedHandle(&set->handles[i]);
        res->Release();
        if (set->samples == 1) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format = ViewFormat(set->format);
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            m_dev->CreateShaderResourceView(set->tex[i], &sv, &set->srv[i]);
        }
        m_handleMap[set->handles[i]] = {set, i};
        pOut->rSharedTextureHandles[i] = (vr::SharedTextureHandle_t)set->handles[i];
    }
    pOut->unTextureFlags = 0;
    m_sets.push_back(set);
}

void LunarCompositor::DestroySwapTextureSet(vr::SharedTextureHandle_t h) {
    std::lock_guard<std::mutex> lk(m_lock);
    auto it = m_handleMap.find((HANDLE)h);
    if (it == m_handleMap.end()) return;
    SwapSet *set = it->second.first;
    for (int i = 0; i < 3; i++) {
        m_handleMap.erase(set->handles[i]);
        SafeRelease(set->srv[i]);
        SafeRelease(set->tex[i]);
    }
    for (size_t i = 0; i < m_sets.size(); i++)
        if (m_sets[i] == set) { m_sets.erase(m_sets.begin() + i); break; }
    delete set;
}

void LunarCompositor::DestroyAllSwapTextureSets(uint32_t unPid) {
    std::vector<vr::SharedTextureHandle_t> victims;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        for (auto *s : m_sets)
            if (s->pid == unPid) victims.push_back((vr::SharedTextureHandle_t)s->handles[0]);
    }
    for (auto h : victims) DestroySwapTextureSet(h);
}

void LunarCompositor::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*pIndices)[2]) {
    (*pIndices)[0] = ((*pIndices)[0] + 1) % 3;
    (*pIndices)[1] = ((*pIndices)[1] + 1) % 3;
}

void LunarCompositor::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_layers.size() >= 8) return;
    Layer l;
    l.eye[0] = perEye[0];
    l.eye[1] = perEye[1];
    m_layers.push_back(l);
}

ID3D11ShaderResourceView *LunarCompositor::ResolveSrv(vr::SharedTextureHandle_t h, bool *linear, ID3D11Texture2D **tex, uint32_t *samples) {
    auto it = m_handleMap.find((HANDLE)h);
    if (it == m_handleMap.end()) return nullptr;
    SwapSet *s = it->second.first;
    int i = it->second.second;
    *linear = s->linear;
    *tex = s->tex[i];
    *samples = s->samples;
    return s->srv[i];
}

static LPose PoseFromMatrix(const vr::HmdMatrix34_t &m) {
    float r[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[i][j] = m.m[i][j];
    LPose p;
    p.orientation = lqFromMat3(r);
    p.position.x = m.m[0][3];
    p.position.y = m.m[1][3];
    p.position.z = m.m[2][3];
    return p;
}

void LunarCompositor::Present(vr::SharedTextureHandle_t syncTexture) {
    std::vector<Layer> layers;
    std::lock_guard<std::mutex> lk(m_lock);
    layers.swap(m_layers);
    if (layers.empty() || !m_dev) return;

    uint32_t eyeW = m_ipc->eyeWidth ? m_ipc->eyeWidth : 1440;
    uint32_t eyeH = m_ipc->eyeHeight ? m_ipc->eyeHeight : 1584;
    if (!EnsureOutput(eyeW * 2, eyeH)) return;

    // The sync texture tells us when the compositor finished writing the layers.
    ID3D11Texture2D *syncTex = nullptr;
    IDXGIKeyedMutex *syncMutex = nullptr;
    if (syncTexture && SUCCEEDED(m_dev->OpenSharedResource((HANDLE)syncTexture, __uuidof(ID3D11Texture2D), (void **)&syncTex))) {
        if (SUCCEEDED(syncTex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&syncMutex))) {
            if (syncMutex->AcquireSync(0, 10) != S_OK) {
                syncMutex->Release();
                syncTex->Release();
                return;
            }
        }
    }

    int slot = -1;
    for (int i = 0; i < LUNAR_IPC_FRAME_SLOTS; i++) {
        int s = (m_nextSlot + i) % LUNAR_IPC_FRAME_SLOTS;
        HRESULT hr = m_outMutex[s]->AcquireSync(LUNAR_KEY_DRIVER, 0);
        if (hr == S_OK || hr == (HRESULT)WAIT_ABANDONED) {
            slot = s;
            break;
        }
    }

    if (slot >= 0) {
        m_nextSlot = (slot + 1) % LUNAR_IPC_FRAME_SLOTS;
        ID3D11RenderTargetView *rtv = m_outRtv[slot];
        float clear[4] = {0, 0, 0, 1};
        m_ctx->ClearRenderTargetView(rtv, clear);
        m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->IASetInputLayout(nullptr);
        m_ctx->VSSetShader(m_vs, nullptr, 0);
        m_ctx->PSSetShader(m_ps, nullptr, 0);
        m_ctx->PSSetSamplers(0, 1, &m_sampler);
        m_ctx->VSSetConstantBuffers(0, 1, &m_cb);
        m_ctx->PSSetConstantBuffers(0, 1, &m_cb);
        m_ctx->RSSetState(m_raster);

        for (size_t li = 0; li < layers.size(); li++) {
            float bf[4] = {0, 0, 0, 0};
            m_ctx->OMSetBlendState(li == 0 ? m_blendOff : m_blendOn, bf, 0xffffffff);
            for (int eye = 0; eye < 2; eye++) {
                const auto &e = layers[li].eye[eye];
                bool linear = false;
                ID3D11Texture2D *tex = nullptr;
                uint32_t samples = 1;
                ID3D11ShaderResourceView *srv = ResolveSrv(e.hTexture, &linear, &tex, &samples);
                if (!tex) continue;
                if (samples > 1) {
                    D3D11_TEXTURE2D_DESC td;
                    tex->GetDesc(&td);
                    D3D11_TEXTURE2D_DESC rd = {};
                    if (m_resolveTex[eye]) m_resolveTex[eye]->GetDesc(&rd);
                    if (!m_resolveTex[eye] || rd.Width != td.Width || rd.Height != td.Height || rd.Format != ViewFormat(td.Format)) {
                        SafeRelease(m_resolveSrv[eye]);
                        SafeRelease(m_resolveTex[eye]);
                        td.SampleDesc.Count = 1;
                        td.SampleDesc.Quality = 0;
                        td.Format = ViewFormat(td.Format);
                        td.MiscFlags = 0;
                        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                        m_dev->CreateTexture2D(&td, nullptr, &m_resolveTex[eye]);
                        if (m_resolveTex[eye]) m_dev->CreateShaderResourceView(m_resolveTex[eye], nullptr, &m_resolveSrv[eye]);
                    }
                    if (!m_resolveTex[eye]) continue;
                    m_ctx->ResolveSubresource(m_resolveTex[eye], 0, tex, 0, ViewFormat(td.Format));
                    srv = m_resolveSrv[eye];
                }
                if (!srv) continue;

                D3D11_MAPPED_SUBRESOURCE map;
                if (SUCCEEDED(m_ctx->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                    ShaderCB cb = {};
                    cb.bounds[0] = e.bounds.uMin;
                    cb.bounds[1] = e.bounds.vMin;
                    cb.bounds[2] = e.bounds.uMax;
                    cb.bounds[3] = e.bounds.vMax;
                    cb.linearIn = linear ? 1.f : 0.f;
                    cb.opaque = li == 0 ? 1.f : 0.f;
                    memcpy(map.pData, &cb, sizeof(cb));
                    m_ctx->Unmap(m_cb, 0);
                }
                D3D11_VIEWPORT vp = {(float)(eye * eyeW), 0.f, (float)eyeW, (float)eyeH, 0.f, 1.f};
                m_ctx->RSSetViewports(1, &vp);
                m_ctx->PSSetShaderResources(0, 1, &srv);
                m_ctx->Draw(3, 0);
            }
        }
        ID3D11ShaderResourceView *nullSrv = nullptr;
        m_ctx->PSSetShaderResources(0, 1, &nullSrv);
        m_ctx->Flush();
        m_outMutex[slot]->ReleaseSync(LUNAR_KEY_HOST);

        LunarIpcFrameInfo info = {};
        info.slot = slot;
        info.frameIndex = ++m_frameIndex;
        info.presentTimeUs = lunarNowUs();
        info.trackingTimeUs = m_ipc->tracking.clientTimeUs;
        info.headPose = PoseFromMatrix(layers[0].eye[0].mHmdPose);
        lunarSeqWrite(&m_ipc->frameSeq, &m_ipc->latestFrame, info);
        InterlockedIncrement((volatile LONG *)&m_ipc->vrFrameCount);
        if (m_frameEvent) SetEvent(m_frameEvent);
    }

    if (syncMutex) {
        syncMutex->ReleaseSync(0);
        syncMutex->Release();
    }
    SafeRelease(syncTex);
}

void LunarCompositor::PostPresent(const Throttling_t *) {
    // Pace the compositor to the headset refresh rate and signal vsync.
    float hz = m_ipc->refreshRate > 30.f ? m_ipc->refreshRate : 72.f;
    int64_t period = (int64_t)(1000000.0 / hz);
    int64_t now = lunarNowUs();
    if (m_nextVsyncUs == 0 || now - m_nextVsyncUs > period * 2) m_nextVsyncUs = now;
    m_nextVsyncUs += period;
    int64_t wait = m_nextVsyncUs - now;
    if (wait > 2000) Sleep((DWORD)((wait - 1500) / 1000));
    while (lunarNowUs() < m_nextVsyncUs) YieldProcessor();
    vr::VRServerDriverHost()->VsyncEvent(0);
}

void LunarCompositor::GetFrameTiming(vr::DriverDirectMode_FrameTiming *t) {
    t->m_nReprojectionFlags = 0;
}
