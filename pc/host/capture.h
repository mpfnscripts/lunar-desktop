// Desktop capture (DXGI Desktop Duplication) and VR frame source (shared textures from the SteamVR driver).
#pragma once
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "../common/lunar_ipc.h"

struct MonitorInfo {
    std::string name;
    RECT rect;        // virtual desktop coordinates
    uint64_t adapterLuid;
    int adapterIndex, outputIndex;
};

std::vector<MonitorInfo> EnumerateMonitors();
ID3D11Device *CreateDeviceForLuid(uint64_t luid, uint64_t *actualLuid);

class DesktopCapture {
public:
    ~DesktopCapture() { Shutdown(); }
    bool Init(int monitorIndex);
    void Shutdown();
    // 1 = new frame in Texture(), 0 = nothing new, -1 = lost (call Init again)
    int Acquire(int timeoutMs);
    ID3D11Device *Device() { return m_dev; }
    uint64_t Luid() const { return m_luid; }
    ID3D11Texture2D *Texture() { return m_copy; }
    uint32_t Width() const { return m_w; }
    uint32_t Height() const { return m_h; }
    const MonitorInfo &Monitor() const { return m_mon; }
    bool HasFrame() const { return m_hasFrame; }

private:
    ID3D11Device *m_dev = nullptr;
    ID3D11DeviceContext *m_ctx = nullptr;
    IDXGIOutputDuplication *m_dup = nullptr;
    ID3D11Texture2D *m_copy = nullptr;
    uint32_t m_w = 0, m_h = 0;
    uint64_t m_luid = 0;
    MonitorInfo m_mon = {};
    bool m_hasFrame = false;
};

class VrFrameSource {
public:
    ~VrFrameSource() { Shutdown(); }
    bool Init(LunarIpc *ipc);
    void Shutdown();
    // Waits for the next frame. On success the slot texture is locked for us and must be released with Release().
    bool Acquire(int timeoutMs, ID3D11Texture2D **tex, LunarIpcFrameInfo *info);
    void Release();
    ID3D11Device *Device() { return m_dev; }
    uint64_t Luid() const { return m_luid; }
    uint32_t Width() const { return m_w; }
    uint32_t Height() const { return m_h; }
    uint32_t Generation() const { return m_gen; }

private:
    bool OpenTextures();
    LunarIpc *m_ipc = nullptr;
    HANDLE m_event = nullptr;
    ID3D11Device1 *m_dev = nullptr;
    uint64_t m_luid = 0;
    ID3D11Texture2D *m_tex[LUNAR_IPC_FRAME_SLOTS] = {};
    IDXGIKeyedMutex *m_mutex[LUNAR_IPC_FRAME_SLOTS] = {};
    uint32_t m_gen = 0, m_w = 0, m_h = 0;
    uint32_t m_lastSeq = 0;
    int m_locked = -1;
};
