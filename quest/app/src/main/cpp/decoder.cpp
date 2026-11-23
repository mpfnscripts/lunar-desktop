#include "decoder.h"
#include <android/hardware_buffer.h>
#include <unistd.h>
#include <functional>
#include "common.h"
#include "gfx.h"

static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC eglGetNativeClientBufferANDROID_ = nullptr;
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;
static PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR_ = nullptr;
static PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR_ = nullptr;
static PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID_ = nullptr;

static void LoadEglExt() {
    if (eglCreateImageKHR_) return;
    eglGetNativeClientBufferANDROID_ = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    eglCreateImageKHR_ = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR_ = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    eglCreateSyncKHR_ = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    eglDestroySyncKHR_ = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    eglDupNativeFenceFDANDROID_ = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
}

// Splits Annex-B data into NAL units (including their start codes).
static void ForEachNal(const uint8_t *d, size_t n, const std::function<void(const uint8_t *, size_t, int)> &fn, int codec) {
    std::vector<size_t> starts;
    for (size_t i = 0; i + 3 <= n; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (i + 3 < n && d[i + 2] == 0 && d[i + 3] == 1))) {
            starts.push_back(i);
            i += d[i + 2] == 1 ? 2 : 3;
        }
    }
    for (size_t k = 0; k < starts.size(); k++) {
        size_t s = starts[k], e = k + 1 < starts.size() ? starts[k + 1] : n;
        size_t hdr = d[s + 2] == 1 ? 3 : 4;
        if (s + hdr >= e) continue;
        uint8_t b = d[s + hdr];
        int type = codec == 1 ? (b >> 1) & 0x3f : b & 0x1f;
        fn(d + s, e - s, type);
    }
}

void Decoder::configure(int codec, int w, int h) {
    std::lock_guard<std::mutex> lk(m_lock);
    stopCodec();
    m_codecType = codec;
    m_w = w;
    m_h = h;
    m_waitKey = true;
    std::lock_guard<std::mutex> tl(m_timesLock);
    m_submitTimes.clear();
}

void Decoder::stopCodec() {
    // m_lock held
    m_outRun = false;
    if (m_outThread.joinable()) m_outThread.join();
    if (m_codec) {
        AMediaCodec_stop(m_codec);
        AMediaCodec_delete(m_codec);
        m_codec = nullptr;
    }
    if (m_reader) {
        m_retired.push_back(m_reader);  // the render thread deletes it once its images are released
        m_reader = nullptr;
    }
}

void Decoder::shutdown() {
    std::lock_guard<std::mutex> lk(m_lock);
    stopCodec();
}

bool Decoder::createCodec(const uint8_t *data, size_t size) {
    // m_lock held
    std::vector<uint8_t> csd0, csd1;
    ForEachNal(data, size, [&](const uint8_t *p, size_t n, int type) {
        if (m_codecType == 1) {
            if (type == 32 || type == 33 || type == 34) csd0.insert(csd0.end(), p, p + n);
        } else {
            if (type == 7) csd0.insert(csd0.end(), p, p + n);
            if (type == 8) csd1.insert(csd1.end(), p, p + n);
        }
    }, m_codecType);
    if (csd0.empty()) {
        LOGW("keyframe without parameter sets, waiting");
        return false;
    }

    if (AImageReader_newWithUsage(m_w, m_h, AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 5, &m_reader) != AMEDIA_OK) {
        LOGE("AImageReader failed");
        m_reader = nullptr;
        return false;
    }
    ANativeWindow *win = nullptr;
    AImageReader_getWindow(m_reader, &win);

    const char *mime = m_codecType == 1 ? "video/hevc" : "video/avc";
    m_codec = AMediaCodec_createDecoderByType(mime);
    if (!m_codec) {
        LOGE("no decoder for %s", mime);
        return false;
    }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, m_w);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, m_h);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, 8 * 1024 * 1024);
    AMediaFormat_setInt32(f, "low-latency", 1);
    AMediaFormat_setInt32(f, "priority", 0);
    AMediaFormat_setInt32(f, "operating-rate", 32767);
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-picture-order.enable", 1);
    AMediaFormat_setBuffer(f, "csd-0", csd0.data(), csd0.size());
    if (!csd1.empty()) AMediaFormat_setBuffer(f, "csd-1", csd1.data(), csd1.size());
    media_status_t st = AMediaCodec_configure(m_codec, f, win, nullptr, 0);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK || AMediaCodec_start(m_codec) != AMEDIA_OK) {
        LOGE("decoder configure/start failed %d", st);
        AMediaCodec_delete(m_codec);
        m_codec = nullptr;
        return false;
    }
    m_outRun = true;
    m_outThread = std::thread([this] { outputLoop(); });
    LOGI("decoder started: %s %dx%d", mime, m_w, m_h);
    return true;
}

void Decoder::submit(const uint8_t *data, size_t size, uint32_t frameIndex, bool keyframe) {
    std::lock_guard<std::mutex> lk(m_lock);
    if (!m_w) return;
    if (m_waitKey && !keyframe) return;
    if (!m_codec) {
        if (!keyframe || !createCodec(data, size)) return;
    }
    ssize_t idx = AMediaCodec_dequeueInputBuffer(m_codec, 8000);
    if (idx < 0) {
        LOGW("decoder input full, dropping frame %u", frameIndex);
        m_waitKey = true;  // a dropped reference frame would corrupt the picture
        return;
    }
    size_t cap = 0;
    uint8_t *buf = AMediaCodec_getInputBuffer(m_codec, idx, &cap);
    if (!buf || cap < size) {
        AMediaCodec_queueInputBuffer(m_codec, idx, 0, 0, 0, 0);
        m_waitKey = true;
        return;
    }
    memcpy(buf, data, size);
    {
        std::lock_guard<std::mutex> tl(m_timesLock);
        m_submitTimes[frameIndex] = NowUs();
        if (m_submitTimes.size() > 64) m_submitTimes.erase(m_submitTimes.begin());
    }
    AMediaCodec_queueInputBuffer(m_codec, idx, 0, size, (uint64_t)frameIndex, 0);
    if (keyframe) m_waitKey = false;
}

void Decoder::outputLoop() {
    while (m_outRun) {
        AMediaCodecBufferInfo info;
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(m_codec, &info, 20000);
        if (idx >= 0) {
            uint32_t fi = (uint32_t)info.presentationTimeUs;
            {
                std::lock_guard<std::mutex> lk(m_timesLock);
                auto it = m_submitTimes.find(fi);
                if (it != m_submitTimes.end()) {
                    float ms = (NowUs() - it->second) / 1000.f;
                    m_decodeMs = m_decodeMs * 0.9f + ms * 0.1f;
                }
            }
            AMediaCodec_releaseOutputBuffer(m_codec, idx, info.size != 0);
            m_decoded++;
        } else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *f = AMediaCodec_getOutputFormat(m_codec);
            LOGI("decoder output format: %s", AMediaFormat_toString(f));
            AMediaFormat_delete(f);
        }
    }
}

bool Decoder::latest(GLuint *tex, uint32_t *frameIndex) {
    LoadEglExt();
    EGLDisplay dpy = eglGetCurrentDisplay();
    AImageReader *reader;
    std::vector<AImageReader *> retired;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        reader = m_reader;
        retired.swap(m_retired);
    }
    if (!retired.empty() || reader != m_glReader) {
        // stream changed: drop everything that belongs to old readers
        if (m_previous) AImage_delete(m_previous);
        if (m_current) AImage_delete(m_current);
        m_previous = m_current = nullptr;
        for (auto &e : m_eglImages) eglDestroyImageKHR_(dpy, e.second);
        m_eglImages.clear();
        for (auto *r : retired) AImageReader_delete(r);
        m_glReader = reader;
    }
    if (!m_tex) {
        glGenTextures(1, &m_tex);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_tex);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (m_glReader) {
        AImage *img = nullptr;
        if (AImageReader_acquireLatestImage(m_glReader, &img) == AMEDIA_OK && img) {
            AHardwareBuffer *hb = nullptr;
            AImage_getHardwareBuffer(img, &hb);
            EGLImageKHR eimg = EGL_NO_IMAGE_KHR;
            auto it = m_eglImages.find(hb);
            if (it != m_eglImages.end()) {
                eimg = it->second;
            } else if (hb && eglGetNativeClientBufferANDROID_) {
                EGLClientBuffer cb = eglGetNativeClientBufferANDROID_(hb);
                EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
                eimg = eglCreateImageKHR_(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attrs);
                if (eimg != EGL_NO_IMAGE_KHR) m_eglImages[hb] = eimg;
                if (m_eglImages.size() > 16) LOGW("many decoder buffers (%zu)", m_eglImages.size());
            }
            if (eimg != EGL_NO_IMAGE_KHR) {
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_tex);
                glEGLImageTargetTexture2DOES_(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)eimg);
                int64_t ts = 0;
                AImage_getTimestamp(img, &ts);
                m_currentIndex = (uint32_t)(ts / 1000);
                // keep the previous image until the GPU is done with it (see frameDone)
                if (m_previous) AImage_delete(m_previous);
                m_previous = m_current;
                m_current = img;
            } else {
                AImage_delete(img);
            }
        }
    }
    *tex = m_tex;
    *frameIndex = m_currentIndex;
    return m_current != nullptr;
}

void Decoder::frameDone() {
    // Release the previous image with a GPU fence so the decoder can reuse it once rendering finished.
    if (!m_previous) return;
    EGLDisplay dpy = eglGetCurrentDisplay();
    int fd = -1;
    if (eglCreateSyncKHR_ && eglDupNativeFenceFDANDROID_) {
        EGLint attrs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE};
        EGLSyncKHR sync = eglCreateSyncKHR_(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
        if (sync != EGL_NO_SYNC_KHR) {
            glFlush();
            fd = eglDupNativeFenceFDANDROID_(dpy, sync);
            eglDestroySyncKHR_(dpy, sync);
        }
    }
    if (fd >= 0) AImage_deleteAsync(m_previous, fd);
    else AImage_delete(m_previous);
    m_previous = nullptr;
}

void Decoder::destroyGl() {
    EGLDisplay dpy = eglGetCurrentDisplay();
    if (m_previous) AImage_delete(m_previous);
    if (m_current) AImage_delete(m_current);
    m_previous = m_current = nullptr;
    for (auto &e : m_eglImages) eglDestroyImageKHR_(dpy, e.second);
    m_eglImages.clear();
    if (m_tex) glDeleteTextures(1, &m_tex);
    m_tex = 0;
}
