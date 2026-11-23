// Hardware H.264 / HEVC encoding through Media Foundation (works on NVIDIA, AMD and Intel GPUs),
// plus a D3D11 video processor that converts/scales RGBA textures to NV12 on the GPU.
#pragma once
#include <d3d11_1.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <codecapi.h>
#include <stdint.h>
#include <string>
#include <vector>

struct EncodedFrame {
    uint64_t id;
    bool keyframe;
    std::vector<uint8_t> data;  // Annex-B
};

class VideoEncoder {
public:
    ~VideoEncoder() { Shutdown(); }
    // codec: 0 = H.264, 1 = HEVC
    bool Init(ID3D11Device *dev, uint64_t adapterLuid, int codec, uint32_t width, uint32_t height, float fps, uint32_t bitrateKbps);
    void Shutdown();
    bool Ok() const { return m_mft != nullptr; }
    int Codec() const { return m_codec; }
    uint32_t Width() const { return m_w; }
    uint32_t Height() const { return m_h; }
    const std::string &Name() const { return m_name; }

    // Encodes an NV12 texture (created on the device given to Init). Finished frames are appended to out.
    bool Encode(ID3D11Texture2D *nv12, uint64_t id, bool forceKeyframe, std::vector<EncodedFrame> &out);
    void SetBitrate(uint32_t kbps);

    static bool SupportsHevc(uint64_t adapterLuid);

private:
    bool CreateMft(uint64_t luid, const GUID &subtype, bool hardware);
    bool Configure();
    bool PumpEvents(bool waitForOutput, std::vector<EncodedFrame> &out, int timeoutMs);
    bool DrainOutput(std::vector<EncodedFrame> &out);
    bool FeedSoftware(ID3D11Texture2D *nv12, IMFSample **outSample);

    ID3D11Device *m_dev = nullptr;
    ID3D11DeviceContext *m_ctx = nullptr;
    IMFTransform *m_mft = nullptr;
    IMFMediaEventGenerator *m_events = nullptr;
    IMFDXGIDeviceManager *m_devMgr = nullptr;
    ICodecAPI *m_codecApi = nullptr;
    ID3D11Texture2D *m_staging = nullptr;
    bool m_async = false;
    bool m_d3dAware = false;
    bool m_providesSamples = false;
    DWORD m_outBufSize = 0;
    int m_pendingNeedInput = 0;
    int m_codec = 0;
    uint32_t m_w = 0, m_h = 0, m_kbps = 0;
    float m_fps = 60;
    std::vector<uint8_t> m_seqHeader;
    std::string m_name;
};

// Converts and scales an RGB(A) texture into an NV12 texture.
class Nv12Converter {
public:
    ~Nv12Converter() { Shutdown(); }
    bool Init(ID3D11Device *dev, uint32_t inW, uint32_t inH, DXGI_FORMAT inFormat, uint32_t outW, uint32_t outH);
    void Shutdown();
    // srcRect: area of the input to use (in pixels). Output always fills the NV12 texture.
    bool Convert(ID3D11Texture2D *input, const RECT &srcRect);
    ID3D11Texture2D *Output() { return m_nv12; }
    void ClearInputViews();
    bool Matches(uint32_t inW, uint32_t inH, DXGI_FORMAT f, uint32_t outW, uint32_t outH) const {
        return m_vp && inW == m_inW && inH == m_inH && f == m_inFmt && outW == m_outW && outH == m_outH;
    }

private:
    ID3D11Device *m_dev = nullptr;
    ID3D11VideoDevice *m_vdev = nullptr;
    ID3D11VideoContext *m_vctx = nullptr;
    ID3D11VideoProcessorEnumerator *m_enum = nullptr;
    ID3D11VideoProcessor *m_vp = nullptr;
    ID3D11Texture2D *m_nv12 = nullptr;
    ID3D11VideoProcessorOutputView *m_outView = nullptr;
    // cache of input views keyed by texture pointer
    std::vector<std::pair<ID3D11Texture2D *, ID3D11VideoProcessorInputView *>> m_inViews;
    uint32_t m_inW = 0, m_inH = 0, m_outW = 0, m_outH = 0;
    DXGI_FORMAT m_inFmt = DXGI_FORMAT_UNKNOWN;
};
