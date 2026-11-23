// Shared memory layout between LunarDesktop.exe (host) and driver_lunar.dll (inside SteamVR's vrserver.exe).
//
//  host  -> driver : headset display config, latest tracking sample, headset connected flag
//  driver -> host  : rendered VR frames (shared D3D11 textures with keyed mutexes), haptics, heartbeat
#pragma once
#include <windows.h>
#include <stdint.h>
#include "../../shared/lunar_protocol.h"

#define LUNAR_IPC_NAME L"Local\\LunarDesktopIPC_v1"
#define LUNAR_IPC_FRAME_EVENT L"Local\\LunarDesktopFrameReady_v1"
#define LUNAR_IPC_MAGIC 0x4C495043u  // "LIPC"
#define LUNAR_IPC_VERSION 1
#define LUNAR_IPC_FRAME_SLOTS 3
#define LUNAR_IPC_HAPTIC_RING 64

// Keyed mutex protocol for each frame slot:
//   driver AcquireSync(0) -> render -> ReleaseSync(1)
//   host   AcquireSync(1) -> read   -> ReleaseSync(0)
#define LUNAR_KEY_DRIVER 0
#define LUNAR_KEY_HOST 1

struct LunarIpcFrameInfo {
    uint32_t slot;
    uint32_t reserved;
    uint64_t frameIndex;
    uint64_t trackingTimeUs;  // client time of the newest tracking sample when the frame was presented
    int64_t presentTimeUs;    // host clock (QPC) when the driver presented
    LPose headPose;           // pose the game rendered with
};

struct LunarIpc {
    uint32_t magic;
    uint32_t version;

    // ---- written by host ----
    volatile int64_t hostHeartbeatUs;
    volatile uint32_t headsetConnected;
    volatile uint32_t displayGeneration;  // bump after changing the display block
    uint32_t eyeWidth, eyeHeight;         // render size per eye requested from SteamVR
    float refreshRate;
    float reservedF;
    LFov fov[2];
    LPose eyeInHead[2];

    volatile uint32_t trackingSeq;  // seqlock (odd while writing)
    uint32_t reservedT;
    LunarTracking tracking;
    int64_t trackingReceivedUs;  // host clock when received

    volatile uint32_t recenterRequest;
    uint32_t controllerPoseMode;  // 0 = grip pose, 1 = aim pose
    float controllerOffsetPos[3]; // metres, applied in controller space
    float controllerOffsetRotDeg[3];
    char headsetName[64];
    float secondsToPhotons;       // latency estimate used for SteamVR pose prediction

    // ---- written by driver ----
    volatile int64_t driverHeartbeatUs;
    uint64_t adapterLuid;            // adapter the driver renders on
    volatile uint32_t frameTexGeneration;  // bumps when shared textures are recreated
    uint32_t frameTexWidth, frameTexHeight;
    uint32_t frameTexFormat;         // DXGI_FORMAT
    volatile uint32_t frameSeq;      // seqlock for latestFrame
    uint32_t reservedF2;
    LunarIpcFrameInfo latestFrame;
    volatile uint32_t hapticWrite;   // ring write index (monotonic)
    uint32_t reservedH;
    LunarHaptic haptics[LUNAR_IPC_HAPTIC_RING];
    char appName[64];
    volatile uint32_t vrFrameCount;  // total presents (for fps)
};

// Name of the shared texture for slot i of a given generation.
static inline void lunarFrameTexName(wchar_t *out, size_t n, uint32_t gen, uint32_t slot) {
    _snwprintf(out, n, L"Local\\LunarDesktopFrame_%u_%u", gen, slot);
    out[n - 1] = 0;
}

static inline int64_t lunarNowUs() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1000000.0 / (double)freq.QuadPart);
}

// Opens (or creates) the shared block. Returns nullptr on failure.
static inline LunarIpc *lunarIpcOpen(HANDLE *outMapping) {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LunarIpc), LUNAR_IPC_NAME);
    if (!h) return nullptr;
    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    auto *p = (LunarIpc *)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LunarIpc));
    if (!p) { CloseHandle(h); return nullptr; }
    if (!existed || p->magic != LUNAR_IPC_MAGIC || p->version != LUNAR_IPC_VERSION) {
        memset(p, 0, sizeof(LunarIpc));
        p->version = LUNAR_IPC_VERSION;
        MemoryBarrier();
        p->magic = LUNAR_IPC_MAGIC;
    }
    *outMapping = h;
    return p;
}

template <typename T>
static inline void lunarSeqWrite(volatile uint32_t *seq, T *dst, const T &src) {
    InterlockedIncrement((volatile LONG *)seq);
    MemoryBarrier();
    memcpy((void *)dst, &src, sizeof(T));
    MemoryBarrier();
    InterlockedIncrement((volatile LONG *)seq);
}

template <typename T>
static inline bool lunarSeqRead(volatile uint32_t *seq, const T *src, T &out, uint32_t *outSeq = nullptr) {
    for (int tries = 0; tries < 64; tries++) {
        uint32_t s1 = *seq;
        if (s1 & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        memcpy(&out, (const void *)src, sizeof(T));
        MemoryBarrier();
        if (*seq == s1) {
            if (outSeq) *outSeq = s1;
            return true;
        }
    }
    return false;
}
