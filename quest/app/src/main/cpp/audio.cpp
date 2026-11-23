#include "audio.h"
#include "common.h"

static const size_t kRing = 48000 * 2;  // 1 s stereo samples... (frames * 2)
static const size_t kTargetFrames = 48000 * 40 / 1000;  // 40 ms jitter buffer
static const size_t kMaxFrames = 48000 * 150 / 1000;

bool AudioPlayer::start() {
    stop();
    {
        std::lock_guard<std::mutex> lk(m_lock);
        m_ring.assign(kRing * 2, 0);
        m_read = m_write = 0;
        m_priming = true;
    }
    AAudioStreamBuilder *b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(b, Callback, this);
    AAudioStreamBuilder_setErrorCallback(b, ErrorCallback, this);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &m_stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOGE("audio open failed %d", r);
        m_stream = nullptr;
        return false;
    }
    AAudioStream_requestStart(m_stream);
    LOGI("audio playback started (%d Hz)", AAudioStream_getSampleRate(m_stream));
    return true;
}

void AudioPlayer::stop() {
    if (m_stream) {
        AAudioStream_requestStop(m_stream);
        AAudioStream_close(m_stream);
        m_stream = nullptr;
    }
}

void AudioPlayer::ErrorCallback(AAudioStream *, void *user, aaudio_result_t err) {
    LOGW("audio stream error %d (device changed?)", err);
    ((AudioPlayer *)user)->m_restart = true;
}

void AudioPlayer::push(const int16_t *s, int frames) {
    if (m_restart.exchange(false)) start();
    std::lock_guard<std::mutex> lk(m_lock);
    size_t cap = m_ring.size() / 2;
    for (int i = 0; i < frames; i++) {
        size_t w = (m_write % cap) * 2;
        m_ring[w] = s[i * 2];
        m_ring[w + 1] = s[i * 2 + 1];
        m_write++;
    }
    // too much latency built up (Wi-Fi burst): skip ahead
    if (m_write - m_read > kMaxFrames) m_read = m_write - kTargetFrames;
}

float AudioPlayer::bufferedMs() const {
    std::lock_guard<std::mutex> lk(m_lock);
    return (m_write - m_read) / 48.f;
}

aaudio_data_callback_result_t AudioPlayer::Callback(AAudioStream *, void *user, void *data, int32_t frames) {
    auto *self = (AudioPlayer *)user;
    int16_t *out = (int16_t *)data;
    std::lock_guard<std::mutex> lk(self->m_lock);
    size_t avail = self->m_write - self->m_read;
    if (self->m_priming) {
        if (avail >= kTargetFrames) self->m_priming = false;
        else {
            memset(out, 0, frames * 4);
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
    }
    size_t cap = self->m_ring.size() / 2;
    for (int i = 0; i < frames; i++) {
        if (self->m_write - self->m_read == 0) {
            // underrun: fill silence and prime again
            memset(out + i * 2, 0, (frames - i) * 4);
            self->m_priming = true;
            break;
        }
        size_t r = (self->m_read % cap) * 2;
        out[i * 2] = self->m_ring[r];
        out[i * 2 + 1] = self->m_ring[r + 1];
        self->m_read++;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// ------------------------------------------------------------------------------------------------
bool MicCapture::start(std::function<void(const int16_t *, int)> cb) {
    stop();
    m_cb = cb;
    m_acc.clear();
    AAudioStreamBuilder *b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
    AAudioStreamBuilder_setDataCallback(b, Callback, this);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &m_stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOGE("mic open failed %d", r);
        m_stream = nullptr;
        return false;
    }
    AAudioStream_requestStart(m_stream);
    LOGI("microphone started");
    return true;
}

void MicCapture::stop() {
    if (m_stream) {
        AAudioStream_requestStop(m_stream);
        AAudioStream_close(m_stream);
        m_stream = nullptr;
    }
}

aaudio_data_callback_result_t MicCapture::Callback(AAudioStream *, void *user, void *data, int32_t frames) {
    auto *self = (MicCapture *)user;
    const int16_t *in = (const int16_t *)data;
    self->m_acc.insert(self->m_acc.end(), in, in + frames);
    while (self->m_acc.size() >= LUNAR_MIC_FRAMES_PER_PACKET) {
        if (self->m_cb) self->m_cb(self->m_acc.data(), LUNAR_MIC_FRAMES_PER_PACKET);
        self->m_acc.erase(self->m_acc.begin(), self->m_acc.begin() + LUNAR_MIC_FRAMES_PER_PACKET);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
