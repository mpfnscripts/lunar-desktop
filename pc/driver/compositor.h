// Direct-mode compositor: receives SteamVR's eye textures, composites layers side by side into a
// shared texture that LunarDesktop.exe encodes and streams.
#pragma once
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <map>
#include <mutex>
#include <vector>
#include "openvr_driver.h"
#include "../common/lunar_ipc.h"

class LunarCompositor : public vr::IVRDriverDirectModeComponent {
public:
    bool Init(LunarIpc *ipc);
    void Shutdown();
    uint64_t AdapterLuid() const { return m_luid; }

    // IVRDriverDirectModeComponent
    void CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t *pDesc, SwapTextureSet_t *pOut) override;
    void DestroySwapTextureSet(vr::SharedTextureHandle_t h) override;
    void DestroyAllSwapTextureSets(uint32_t unPid) override;
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*pIndices)[2]) override;
    void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override;
    void Present(vr::SharedTextureHandle_t syncTexture) override;
    void PostPresent(const Throttling_t *pThrottling) override;
    void GetFrameTiming(vr::DriverDirectMode_FrameTiming *pFrameTiming) override;

private:
    struct SwapSet {
        uint32_t pid;
        ID3D11Texture2D *tex[3] = {};
        ID3D11ShaderResourceView *srv[3] = {};
        HANDLE handles[3] = {};
        DXGI_FORMAT format;
        uint32_t samples;
        bool linear;  // values are linear (sRGB view or float) and need encoding to sRGB
    };
    struct Layer {
        SubmitLayerPerEye_t eye[2];
    };

    bool CreateShaders();
    bool EnsureOutput(uint32_t eyeW, uint32_t eyeH);
    void ReleaseOutput();
    ID3D11ShaderResourceView *ResolveSrv(vr::SharedTextureHandle_t h, bool *linear, ID3D11Texture2D **tex, uint32_t *samples);

    LunarIpc *m_ipc = nullptr;
    ID3D11Device1 *m_dev = nullptr;
    ID3D11DeviceContext *m_ctx = nullptr;
    uint64_t m_luid = 0;

    ID3D11VertexShader *m_vs = nullptr;
    ID3D11PixelShader *m_ps = nullptr;
    ID3D11Buffer *m_cb = nullptr;
    ID3D11SamplerState *m_sampler = nullptr;
    ID3D11BlendState *m_blendOff = nullptr, *m_blendOn = nullptr;
    ID3D11RasterizerState *m_raster = nullptr;

    // output slots (shared with host)
    ID3D11Texture2D *m_out[LUNAR_IPC_FRAME_SLOTS] = {};
    ID3D11RenderTargetView *m_outRtv[LUNAR_IPC_FRAME_SLOTS] = {};
    IDXGIKeyedMutex *m_outMutex[LUNAR_IPC_FRAME_SLOTS] = {};
    HANDLE m_outHandle[LUNAR_IPC_FRAME_SLOTS] = {};
    uint32_t m_outW = 0, m_outH = 0, m_nextSlot = 0;
    HANDLE m_frameEvent = nullptr;

    std::mutex m_lock;
    std::vector<SwapSet *> m_sets;
    std::map<HANDLE, std::pair<SwapSet *, int>> m_handleMap;
    std::vector<Layer> m_layers;
    uint64_t m_frameIndex = 0;

    // resolve target for MSAA layers
    ID3D11Texture2D *m_resolveTex[2] = {};
    ID3D11ShaderResourceView *m_resolveSrv[2] = {};

    int64_t m_nextVsyncUs = 0;
};
