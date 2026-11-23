// PC sound playback (with a small jitter buffer) and microphone capture, using AAudio.
#pragma once
#include <aaudio/AAudio.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

class AudioPlayer {
public:
    bool start();
    void stop();
    void push(const int16_t *stereo, int frames);  // 48 kHz stereo
    float bufferedMs() const;

private:
    static aaudio_data_callback_result_t Callback(AAudioStream *s, void *user, void *data, int32_t frames);
    static void ErrorCallback(AAudioStream *s, void *user, aaudio_result_t err);
    AAudioStream *m_stream = nullptr;
    mutable std::mutex m_lock;
    std::vector<int16_t> m_ring;
    size_t m_read = 0, m_write = 0;
    bool m_priming = true;
    std::atomic<bool> m_restart{false};
};

class MicCapture {
public:
    bool start(std::function<void(const int16_t *, int)> cb);  // delivers 48 kHz mono, 480-frame chunks
    void stop();
    bool running() const { return m_stream != nullptr; }

private:
    static aaudio_data_callback_result_t Callback(AAudioStream *s, void *user, void *data, int32_t frames);
    AAudioStream *m_stream = nullptr;
    std::function<void(const int16_t *, int)> m_cb;
    std::vector<int16_t> m_acc;
};
