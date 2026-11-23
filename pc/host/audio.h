// PC audio -> headset (WASAPI loopback) and headset microphone -> a PC playback device
// (e.g. "CABLE Input" from VB-Audio Virtual Cable, so games can pick "CABLE Output" as their mic).
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

std::vector<std::string> ListPlaybackDevices();

class AudioCapture {
public:
    // Delivers 48 kHz stereo int16 audio.
    void Start(std::function<void(const int16_t *, int frames)> cb);
    void Stop();
    bool Running() const { return m_running; }

private:
    void Loop();
    std::function<void(const int16_t *, int)> m_cb;
    std::atomic<bool> m_run{false}, m_running{false};
    std::thread m_thread;
};

class MicOutput {
public:
    void Start(const std::string &deviceSubstring);
    void Stop();
    void Push(const int16_t *mono48k, int frames);
    bool Active() const { return m_active; }
    std::string DeviceName() const;

private:
    void Loop(std::string dev);
    std::atomic<bool> m_run{false}, m_active{false};
    std::thread m_thread;
    mutable std::mutex m_lock;
    std::vector<int16_t> m_ring;
    size_t m_read = 0, m_write = 0;
    std::string m_deviceName;
};
