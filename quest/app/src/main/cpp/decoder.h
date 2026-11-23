// Hardware video decoding (MediaCodec) straight into GPU textures (AImageReader + EGLImage).
#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

class Decoder {
public:
    ~Decoder() { shutdown(); }
    // Called when the host announces a new stream (any thread).
    void configure(int codec, int width, int height);
    void shutdown();
    // Network thread: one complete encoded frame (Annex-B).
    void submit(const uint8_t *data, size_t size, uint32_t frameIndex, bool keyframe);
    // Render thread: newest decoded picture as an external texture. Returns false if nothing decoded yet.
    bool latest(GLuint *tex, uint32_t *frameIndex);
    // Render thread: call after drawing with the texture so the image can be recycled safely.
    void frameDone();
    void destroyGl();

    bool waitingForKeyframe() const { return m_waitKey; }
    float decodeMs() const { return m_decodeMs; }
    uint32_t decoded() const { return m_decoded; }
    int width() const { return m_w; }
    int height() const { return m_h; }

private:
    bool createCodec(const uint8_t *data, size_t size);
    void outputLoop();
    void stopCodec();

    std::mutex m_lock, m_timesLock;
    int m_codecType = 0, m_w = 0, m_h = 0;
    AImageReader *m_reader = nullptr;
    AMediaCodec *m_codec = nullptr;
    std::vector<AImageReader *> m_retired;
    std::thread m_outThread;
    std::atomic<bool> m_outRun{false};
    std::atomic<bool> m_waitKey{true};
    std::map<uint32_t, uint64_t> m_submitTimes;
    std::atomic<float> m_decodeMs{0};
    std::atomic<uint32_t> m_decoded{0};

    // GL side (render thread only)
    GLuint m_tex = 0;
    AImage *m_current = nullptr;
    AImage *m_previous = nullptr;
    uint32_t m_currentIndex = 0;
    std::map<AHardwareBuffer *, EGLImageKHR> m_eglImages;
    AImageReader *m_glReader = nullptr;
};
