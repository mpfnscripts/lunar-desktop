#include "encoder.h"
#include <codecapi.h>
#include <d3d10.h>
#include <mferror.h>
#include <math.h>
#include "log.h"

template <typename T>
static void SafeRelease(T *&p) {
    if (p) { p->Release(); p = nullptr; }
}

// Not declared by every SDK: MFTEnum2 (Windows 10 1703+) and the adapter LUID filter attribute.
static const GUID kMFT_ENUM_ADAPTER_LUID = {0x1d39518c, 0xe220, 0x4da8, {0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1}};
typedef HRESULT(WINAPI *PFN_MFTEnum2)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *, const MFT_REGISTER_TYPE_INFO *, IMFAttributes *, IMFActivate ***, UINT32 *);

static HRESULT EnumEncoders(uint64_t luid, const GUID &subtype, bool hardware, IMFActivate ***out, UINT32 *count) {
    MFT_REGISTER_TYPE_INFO inInfo = {MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outInfo = {MFMediaType_Video, subtype};
    UINT32 flags = (hardware ? MFT_ENUM_FLAG_HARDWARE : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT)) | MFT_ENUM_FLAG_SORTANDFILTER;
    static PFN_MFTEnum2 enum2 = (PFN_MFTEnum2)GetProcAddress(LoadLibraryW(L"mfplat.dll"), "MFTEnum2");
    if (hardware && luid && enum2) {
        IMFAttributes *attr = nullptr;
        MFCreateAttributes(&attr, 1);
        LUID l;
        l.LowPart = (DWORD)(luid & 0xffffffff);
        l.HighPart = (LONG)(luid >> 32);
        attr->SetBlob(kMFT_ENUM_ADAPTER_LUID, (const UINT8 *)&l, sizeof(l));
        HRESULT hr = enum2(MFT_CATEGORY_VIDEO_ENCODER, flags, &inInfo, &outInfo, attr, out, count);
        attr->Release();
        if (SUCCEEDED(hr) && *count > 0) return hr;
    }
    return MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &inInfo, &outInfo, out, count);
}

bool VideoEncoder::SupportsHevc(uint64_t luid) {
    IMFActivate **acts = nullptr;
    UINT32 n = 0;
    if (FAILED(EnumEncoders(luid, MFVideoFormat_HEVC, true, &acts, &n))) return false;
    for (UINT32 i = 0; i < n; i++) acts[i]->Release();
    CoTaskMemFree(acts);
    return n > 0;
}

bool VideoEncoder::CreateMft(uint64_t luid, const GUID &subtype, bool hardware) {
    IMFActivate **acts = nullptr;
    UINT32 n = 0;
    if (FAILED(EnumEncoders(luid, subtype, hardware, &acts, &n)) || n == 0) return false;
    bool ok = false;
    for (UINT32 i = 0; i < n && !ok; i++) {
        IMFTransform *t = nullptr;
        if (SUCCEEDED(acts[i]->ActivateObject(__uuidof(IMFTransform), (void **)&t))) {
            wchar_t *name = nullptr;
            UINT32 len = 0;
            if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len))) {
                m_name = Narrow(name);
                CoTaskMemFree(name);
            }
            m_mft = t;
            ok = true;
        }
    }
    for (UINT32 i = 0; i < n; i++) acts[i]->Release();
    CoTaskMemFree(acts);
    return ok;
}

static void SetU32(ICodecAPI *api, const GUID &g, UINT32 v) {
    if (!api) return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    api->SetValue(&g, &var);
}
static void SetBool(ICodecAPI *api, const GUID &g, bool v) {
    if (!api) return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    api->SetValue(&g, &var);
}

bool VideoEncoder::Init(ID3D11Device *dev, uint64_t luid, int codec, uint32_t w, uint32_t h, float fps, uint32_t kbps) {
    Shutdown();
    m_dev = dev;
    m_dev->AddRef();
    m_dev->GetImmediateContext(&m_ctx);
    m_codec = codec;
    m_w = w;
    m_h = h;
    m_fps = fps > 1 ? fps : 60;
    m_kbps = kbps;

    ID3D10Multithread *mt = nullptr;
    if (SUCCEEDED(m_dev->QueryInterface(__uuidof(ID3D10Multithread), (void **)&mt))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    const GUID &subtype = codec == 1 ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    bool hw = CreateMft(luid, subtype, true);
    if (!hw && codec == 0) CreateMft(0, subtype, false);
    if (!m_mft) {
        Log("No %s encoder available", codec == 1 ? "HEVC" : "H.264");
        Shutdown();
        return false;
    }

    IMFAttributes *attr = nullptr;
    if (SUCCEEDED(m_mft->GetAttributes(&attr))) {
        UINT32 v = 0;
        if (SUCCEEDED(attr->GetUINT32(MF_TRANSFORM_ASYNC, &v)) && v) {
            m_async = true;
            attr->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        }
        v = 0;
        if (SUCCEEDED(attr->GetUINT32(MF_SA_D3D11_AWARE, &v)) && v) m_d3dAware = true;
        attr->SetUINT32(MF_LOW_LATENCY, TRUE);
        attr->Release();
    }
    if (m_async) m_mft->QueryInterface(__uuidof(IMFMediaEventGenerator), (void **)&m_events);

    if (m_d3dAware) {
        UINT token = 0;
        if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &m_devMgr))) {
            m_devMgr->ResetDevice(m_dev, token);
            if (FAILED(m_mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)m_devMgr))) {
                Log("Encoder refused D3D manager, using system memory");
                m_d3dAware = false;
            }
        }
    }
    m_mft->QueryInterface(IID_ICodecAPI, (void **)&m_codecApi);

    if (!Configure()) {
        Log("Encoder configuration failed (%s %ux%u)", m_name.c_str(), w, h);
        Shutdown();
        return false;
    }
    Log("Encoder ready: %s, %s %ux%u @ %.0f fps, %u kbps%s", m_name.c_str(), codec == 1 ? "HEVC" : "H.264", w, h, m_fps, kbps,
        m_async ? " (async)" : "");
    return true;
}

bool VideoEncoder::Configure() {
    SetBool(m_codecApi, CODECAPI_AVLowLatencyMode, true);
    SetU32(m_codecApi, CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    SetU32(m_codecApi, CODECAPI_AVEncCommonMeanBitRate, m_kbps * 1000);
    SetU32(m_codecApi, CODECAPI_AVEncCommonBufferSize, (UINT32)(m_kbps * 1000.0 / m_fps * 1.5));
    SetU32(m_codecApi, CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    SetU32(m_codecApi, CODECAPI_AVEncMPVGOPSize, (UINT32)(m_fps * 30));
    SetU32(m_codecApi, CODECAPI_AVEncCommonQualityVsSpeed, 30);

    IMFMediaType *out = nullptr;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, m_codec == 1 ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, m_kbps * 1000);
    MFSetAttributeSize(out, MF_MT_FRAME_SIZE, m_w, m_h);
    MFSetAttributeRatio(out, MF_MT_FRAME_RATE, (UINT32)(m_fps * 1000), 1000);
    MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, m_codec == 1 ? (UINT32)eAVEncH265VProfile_Main_420_8 : (UINT32)eAVEncH264VProfile_High);
    out->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    out->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    out->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    HRESULT hr = m_mft->SetOutputType(0, out, 0);
    out->Release();
    if (FAILED(hr) && m_codec == 0) {
        // Some encoders don't like High profile: retry with Main.
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, m_kbps * 1000);
        MFSetAttributeSize(out, MF_MT_FRAME_SIZE, m_w, m_h);
        MFSetAttributeRatio(out, MF_MT_FRAME_RATE, (UINT32)(m_fps * 1000), 1000);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        hr = m_mft->SetOutputType(0, out, 0);
        out->Release();
    }
    if (FAILED(hr)) {
        Log("SetOutputType failed 0x%08x", (unsigned)hr);
        return false;
    }

    IMFMediaType *in = nullptr;
    for (DWORD i = 0;; i++) {
        IMFMediaType *t = nullptr;
        if (FAILED(m_mft->GetInputAvailableType(0, i, &t))) break;
        GUID sub;
        if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) {
            in = t;
            break;
        }
        t->Release();
    }
    if (!in) {
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    }
    MFSetAttributeSize(in, MF_MT_FRAME_SIZE, m_w, m_h);
    MFSetAttributeRatio(in, MF_MT_FRAME_RATE, (UINT32)(m_fps * 1000), 1000);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = m_mft->SetInputType(0, in, 0);
    in->Release();
    if (FAILED(hr)) {
        Log("SetInputType failed 0x%08x", (unsigned)hr);
        return false;
    }

    MFT_OUTPUT_STREAM_INFO info = {};
    m_mft->GetOutputStreamInfo(0, &info);
    m_providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    m_outBufSize = info.cbSize ? info.cbSize : m_w * m_h * 2;

    IMFMediaType *cur = nullptr;
    if (SUCCEEDED(m_mft->GetOutputCurrentType(0, &cur))) {
        UINT32 sz = 0;
        if (SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sz)) && sz) {
            m_seqHeader.resize(sz);
            cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, m_seqHeader.data(), sz, nullptr);
        }
        cur->Release();
    }

    m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    m_pendingNeedInput = 0;
    return true;
}

void VideoEncoder::Shutdown() {
    if (m_mft) {
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    }
    SafeRelease(m_codecApi);
    SafeRelease(m_events);
    if (m_mft) {
        IMFShutdown *sd = nullptr;
        if (SUCCEEDED(m_mft->QueryInterface(__uuidof(IMFShutdown), (void **)&sd))) {
            sd->Shutdown();
            sd->Release();
        }
    }
    SafeRelease(m_mft);
    SafeRelease(m_devMgr);
    SafeRelease(m_staging);
    SafeRelease(m_ctx);
    SafeRelease(m_dev);
    m_async = m_d3dAware = false;
    m_seqHeader.clear();
}

void VideoEncoder::SetBitrate(uint32_t kbps) {
    if (kbps == m_kbps) return;
    m_kbps = kbps;
    SetU32(m_codecApi, CODECAPI_AVEncCommonMeanBitRate, kbps * 1000);
}

static bool HasParameterSets(const std::vector<uint8_t> &d, int codec) {
    // Look at the first few NAL units for SPS (H.264 type 7) or VPS (HEVC type 32)
    for (size_t i = 0; i + 4 < d.size() && i < 256; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            uint8_t nal = d[i + 3];
            if (codec == 0 && (nal & 0x1f) == 7) return true;
            if (codec == 1 && ((nal >> 1) & 0x3f) == 32) return true;
        }
    }
    return false;
}

bool VideoEncoder::DrainOutput(std::vector<EncodedFrame> &out) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER odb = {};
        IMFSample *own = nullptr;
        if (!m_providesSamples) {
            IMFMediaBuffer *buf = nullptr;
            MFCreateSample(&own);
            MFCreateMemoryBuffer(m_outBufSize, &buf);
            own->AddBuffer(buf);
            buf->Release();
            odb.pSample = own;
        }
        DWORD status = 0;
        HRESULT hr = m_mft->ProcessOutput(0, 1, &odb, &status);
        if (odb.pEvents) odb.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            SafeRelease(own);
            return true;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            SafeRelease(own);
            IMFMediaType *t = nullptr;
            if (SUCCEEDED(m_mft->GetOutputAvailableType(0, 0, &t))) {
                m_mft->SetOutputType(0, t, 0);
                t->Release();
            }
            continue;
        }
        if (FAILED(hr)) {
            SafeRelease(own);
            return false;
        }
        IMFSample *s = odb.pSample;
        if (s) {
            EncodedFrame f;
            LONGLONG t = 0;
            s->GetSampleTime(&t);
            double dur = 1e7 / m_fps;
            f.id = (uint64_t)llround((double)t / dur);
            UINT32 clean = 0;
            f.keyframe = SUCCEEDED(s->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean;
            IMFMediaBuffer *buf = nullptr;
            if (SUCCEEDED(s->ConvertToContiguousBuffer(&buf))) {
                BYTE *p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                    f.data.assign(p, p + len);
                    buf->Unlock();
                }
                buf->Release();
            }
            if (f.keyframe && !m_seqHeader.empty() && !HasParameterSets(f.data, m_codec))
                f.data.insert(f.data.begin(), m_seqHeader.begin(), m_seqHeader.end());
            if (!f.data.empty()) out.push_back(std::move(f));
            if (s != own) s->Release();
        }
        SafeRelease(own);
        if (m_async) return true;  // async MFTs produce one sample per HaveOutput event
    }
}

bool VideoEncoder::PumpEvents(bool waitForOutput, std::vector<EncodedFrame> &out, int timeoutMs) {
    LARGE_INTEGER f, start, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&start);
    size_t before = out.size();
    for (;;) {
        IMFMediaEvent *ev = nullptr;
        HRESULT hr = m_events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            if (!waitForOutput && m_pendingNeedInput > 0) return true;
            if (waitForOutput && out.size() > before) return true;
            QueryPerformanceCounter(&now);
            if ((now.QuadPart - start.QuadPart) * 1000 / f.QuadPart > timeoutMs) return false;
            Sleep(0);
            continue;
        }
        if (FAILED(hr)) return false;
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        ev->Release();
        if (type == METransformNeedInput) {
            m_pendingNeedInput++;
        } else if (type == METransformHaveOutput) {
            DrainOutput(out);
        }
    }
}

bool VideoEncoder::FeedSoftware(ID3D11Texture2D *nv12, IMFSample **outSample) {
    if (!m_staging) {
        D3D11_TEXTURE2D_DESC td;
        nv12->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &m_staging))) return false;
    }
    m_ctx->CopyResource(m_staging, nv12);
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m_ctx->Map(m_staging, 0, D3D11_MAP_READ, 0, &map))) return false;
    DWORD size = m_w * m_h * 3 / 2;
    IMFMediaBuffer *buf = nullptr;
    MFCreateMemoryBuffer(size, &buf);
    BYTE *dst = nullptr;
    buf->Lock(&dst, nullptr, nullptr);
    const BYTE *src = (const BYTE *)map.pData;
    for (uint32_t y = 0; y < m_h; y++) memcpy(dst + y * m_w, src + y * map.RowPitch, m_w);
    const BYTE *uv = src + map.RowPitch * m_h;
    for (uint32_t y = 0; y < m_h / 2; y++) memcpy(dst + m_w * m_h + y * m_w, uv + y * map.RowPitch, m_w);
    buf->Unlock();
    buf->SetCurrentLength(size);
    m_ctx->Unmap(m_staging, 0);
    MFCreateSample(outSample);
    (*outSample)->AddBuffer(buf);
    buf->Release();
    return true;
}

bool VideoEncoder::Encode(ID3D11Texture2D *nv12, uint64_t id, bool forceKey, std::vector<EncodedFrame> &out) {
    if (!m_mft) return false;
    IMFSample *sample = nullptr;
    if (m_d3dAware) {
        IMFMediaBuffer *buf = nullptr;
        if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buf))) return false;
        IMF2DBuffer *b2 = nullptr;
        if (SUCCEEDED(buf->QueryInterface(__uuidof(IMF2DBuffer), (void **)&b2))) {
            DWORD len = 0;
            b2->GetContiguousLength(&len);
            buf->SetCurrentLength(len);
            b2->Release();
        }
        MFCreateSample(&sample);
        sample->AddBuffer(buf);
        buf->Release();
    } else if (!FeedSoftware(nv12, &sample)) {
        return false;
    }
    LONGLONG dur = (LONGLONG)(1e7 / m_fps);
    sample->SetSampleTime((LONGLONG)id * dur);
    sample->SetSampleDuration(dur);
    if (forceKey) SetU32(m_codecApi, CODECAPI_AVEncVideoForceKeyFrame, 1);

    bool ok = true;
    if (m_async) {
        if (m_pendingNeedInput <= 0 && !PumpEvents(false, out, 100)) {
            sample->Release();
            Log("Encoder did not ask for input in time");
            return false;
        }
        HRESULT hr = m_mft->ProcessInput(0, sample, 0);
        m_pendingNeedInput--;
        if (FAILED(hr)) {
            Log("ProcessInput failed 0x%08x", (unsigned)hr);
            ok = false;
        } else {
            PumpEvents(true, out, 40);
        }
    } else {
        HRESULT hr = m_mft->ProcessInput(0, sample, 0);
        if (hr == MF_E_NOTACCEPTING) {
            DrainOutput(out);
            hr = m_mft->ProcessInput(0, sample, 0);
        }
        ok = SUCCEEDED(hr) && DrainOutput(out);
    }
    sample->Release();
    return ok;
}

// ------------------------------------------------------------------------------------------------
// NV12 converter
// ------------------------------------------------------------------------------------------------

bool Nv12Converter::Init(ID3D11Device *dev, uint32_t inW, uint32_t inH, DXGI_FORMAT inFmt, uint32_t outW, uint32_t outH) {
    Shutdown();
    m_dev = dev;
    m_dev->AddRef();
    if (FAILED(m_dev->QueryInterface(__uuidof(ID3D11VideoDevice), (void **)&m_vdev))) return false;
    ID3D11DeviceContext *ctx = nullptr;
    m_dev->GetImmediateContext(&ctx);
    HRESULT hr = ctx->QueryInterface(__uuidof(ID3D11VideoContext), (void **)&m_vctx);
    ctx->Release();
    if (FAILED(hr)) return false;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = inW;
    cd.InputHeight = inH;
    cd.OutputWidth = outW;
    cd.OutputHeight = outH;
    cd.InputFrameRate = {60, 1};
    cd.OutputFrameRate = {60, 1};
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(m_vdev->CreateVideoProcessorEnumerator(&cd, &m_enum))) return false;
    if (FAILED(m_vdev->CreateVideoProcessor(m_enum, 0, &m_vp))) return false;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = outW;
    td.Height = outH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &m_nv12))) return false;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov = {};
    ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(m_vdev->CreateVideoProcessorOutputView(m_nv12, m_enum, &ov, &m_outView))) return false;

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {};
    in.RGB_Range = 0;  // full range RGB
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs = {};
    outCs.YCbCr_Matrix = 1;   // BT.709
    outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    m_vctx->VideoProcessorSetStreamColorSpace(m_vp, 0, &in);
    m_vctx->VideoProcessorSetOutputColorSpace(m_vp, &outCs);
    m_vctx->VideoProcessorSetStreamFrameFormat(m_vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    m_vctx->VideoProcessorSetStreamAutoProcessingMode(m_vp, 0, FALSE);
    RECT full = {0, 0, (LONG)outW, (LONG)outH};
    m_vctx->VideoProcessorSetOutputTargetRect(m_vp, TRUE, &full);

    m_inW = inW;
    m_inH = inH;
    m_inFmt = inFmt;
    m_outW = outW;
    m_outH = outH;
    return true;
}

void Nv12Converter::ClearInputViews() {
    for (auto &p : m_inViews) {
        p.second->Release();
        p.first->Release();
    }
    m_inViews.clear();
}

void Nv12Converter::Shutdown() {
    ClearInputViews();
    SafeRelease(m_outView);
    SafeRelease(m_nv12);
    SafeRelease(m_vp);
    SafeRelease(m_enum);
    SafeRelease(m_vctx);
    SafeRelease(m_vdev);
    SafeRelease(m_dev);
}

bool Nv12Converter::Convert(ID3D11Texture2D *input, const RECT &src) {
    if (!m_vp) return false;
    ID3D11VideoProcessorInputView *view = nullptr;
    for (auto &p : m_inViews)
        if (p.first == input) view = p.second;
    if (!view) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv = {};
        iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (FAILED(m_vdev->CreateVideoProcessorInputView(input, m_enum, &iv, &view))) return false;
        input->AddRef();
        m_inViews.push_back({input, view});
        if (m_inViews.size() > 8) {
            m_inViews.front().second->Release();
            m_inViews.front().first->Release();
            m_inViews.erase(m_inViews.begin());
        }
    }
    m_vctx->VideoProcessorSetStreamSourceRect(m_vp, 0, TRUE, &src);
    RECT dst = {0, 0, (LONG)m_outW, (LONG)m_outH};
    m_vctx->VideoProcessorSetStreamDestRect(m_vp, 0, TRUE, &dst);
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = view;
    return SUCCEEDED(m_vctx->VideoProcessorBlt(m_vp, m_outView, 0, 1, &stream));
}
