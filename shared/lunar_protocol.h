// Lunar Desktop wire protocol, shared by the PC host (Windows) and the Quest client (Android).
// All values are little-endian. Structs are packed and only use fixed-size types.
#pragma once
#include <stdint.h>
#include <string.h>

#define LUNAR_PROTOCOL_VERSION 1

// Ports
#define LUNAR_PORT_DISCOVERY 47810  // UDP: client broadcast <-> host announce
#define LUNAR_PORT_CONTROL   47811  // TCP: control channel (host listens)
#define LUNAR_PORT_STREAM    47812  // UDP: host media socket (tracking/mic arrive here)
#define LUNAR_PORT_CLIENT    47813  // UDP: client media socket (video/audio arrive here)
#define LUNAR_PORT_UI        47800  // HTTP: host settings window (127.0.0.1 only)
#define LUNAR_PORT_REVERSE   47814  // TCP: headset listens; the PC connects out (works when the PC firewall blocks incoming)

#define LUNAR_MAGIC 0x524E554Cu  // "LUNR"
#define LUNAR_MAX_UDP_PAYLOAD 1400

#pragma pack(push, 1)

// ---------------------------------------------------------------------------
// Basic math types
// ---------------------------------------------------------------------------
typedef struct { float x, y, z; } LVec3;
typedef struct { float x, y, z, w; } LQuat;
typedef struct { LQuat orientation; LVec3 position; } LPose;
typedef struct { float angleLeft, angleRight, angleUp, angleDown; } LFov;  // radians, OpenXR convention

// ---------------------------------------------------------------------------
// Discovery (UDP, port LUNAR_PORT_DISCOVERY)
// ---------------------------------------------------------------------------
enum {
    LUNAR_DISC_PROBE = 1,     // client -> broadcast
    LUNAR_DISC_ANNOUNCE = 2,  // host -> client (unicast reply or broadcast)
};
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    char name[64];       // host: computer name / client: headset name
    uint16_t controlPort;
    uint8_t steamvrReady;  // host: driver registered
    uint8_t reserved;
} LunarDiscovery;

// ---------------------------------------------------------------------------
// Control channel (TCP). Each message: LunarMsgHeader + payload of `size` bytes.
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t size;  // payload size (not including header)
    uint16_t type;
    uint16_t reserved;
} LunarMsgHeader;

enum {
    // client -> host
    LMSG_HELLO = 1,
    LMSG_VIEW_CONFIG = 2,
    LMSG_REQUEST_IDR = 3,
    LMSG_POINTER = 4,
    LMSG_KEY = 5,
    LMSG_TEXT = 6,
    LMSG_SET_SETTING = 7,
    LMSG_ACTION = 8,
    LMSG_CLIENT_STATS = 9,
    LMSG_PING = 10,

    // host -> client
    LMSG_WELCOME = 100,
    LMSG_STREAM_CONFIG = 101,
    LMSG_HOST_STATE = 102,
    LMSG_HAPTIC = 103,
    LMSG_CURSOR = 104,
    LMSG_HOST_STATS = 105,
    LMSG_PONG = 106,
    LMSG_DENIED = 107,
};

typedef struct {
    uint16_t protocolVersion;
    uint16_t udpPort;           // client media port
    char deviceId[40];          // stable random id of this headset install
    char deviceName[64];        // e.g. "Quest 3"
    uint32_t recommendedEyeWidth, recommendedEyeHeight;
    float refreshRates[8];      // supported refresh rates, 0 terminated
    float currentRefreshRate;
    uint8_t supportsHevc;
    uint8_t reserved[3];
} LunarHello;

typedef struct {
    LFov fov[2];        // per eye
    LPose eyeInHead[2]; // eye pose relative to head (gives IPD)
    float refreshRate;
} LunarViewConfig;

enum { LUNAR_MODE_IDLE = 0, LUNAR_MODE_DESKTOP = 1, LUNAR_MODE_VR = 2 };
enum { LUNAR_CODEC_H264 = 0, LUNAR_CODEC_HEVC = 1 };

typedef struct {
    uint32_t streamId;   // increments each time the stream is (re)configured
    uint8_t mode;        // LUNAR_MODE_*
    uint8_t codec;       // LUNAR_CODEC_*
    uint16_t reserved;
    uint32_t width, height;  // encoded frame size (VR: both eyes side by side)
    uint32_t contentWidth, contentHeight; // valid area inside the frame
    float fps;
    uint32_t bitrateKbps;
    LFov fov[2];         // VR: fov the game renders with (echo of client's)
    char sourceName[64]; // desktop: monitor name, VR: "SteamVR"
} LunarStreamConfig;

typedef struct {
    char hostName[64];
    char hostVersion[16];
} LunarWelcome;

// Settings the client can see and change. Key/value pairs keep it forward compatible.
typedef struct {
    char key[32];
    char value[96];
} LunarSetting;

enum {
    LACTION_LAUNCH_STEAMVR = 1,
    LACTION_STOP_STEAMVR = 2,
    LACTION_NEXT_MONITOR = 3,
    LACTION_PREV_MONITOR = 4,
    LACTION_RECENTER = 5,
    LACTION_LAUNCH_APP = 6,  // payload text = app id / path
};
typedef struct {
    uint32_t action;
    char arg[128];
} LunarAction;

typedef struct {
    uint8_t steamvrRunning;
    uint8_t driverConnected;
    uint8_t driverRegistered;
    uint8_t vrFramesActive;
    uint8_t monitorIndex;
    uint8_t monitorCount;
    uint8_t audioStreaming;
    uint8_t micActive;
    char theme[24];          // Lunar theme key (lunar, eclipse, ...)
    char customAccent[8], customAccent2[8], customBg[8], customInk[8]; // "#rrggbb" or empty
    uint32_t bitrateKbps;
    uint8_t quality;         // 0 low, 1 medium, 2 high, 3 ultra
    uint8_t codecPref;       // 0 auto, 1 h264, 2 hevc
    uint8_t refreshPref;     // 0 = headset default, else Hz/1 (72, 80, 90, 120)
    uint8_t clunkMode;
    char micDevice[96];
    char gameName[64];       // current VR app if known
} LunarHostState;

typedef struct {
    uint8_t hand;       // 0 left, 1 right
    uint8_t reserved[3];
    float amplitude;    // 0..1
    float frequency;    // Hz, 0 = default
    float duration;     // seconds
} LunarHaptic;

// Desktop-mode pointer events (normalized 0..1 over the streamed monitor)
enum { LPTR_MOVE = 0, LPTR_DOWN = 1, LPTR_UP = 2, LPTR_SCROLL = 3 };
enum { LBTN_LEFT = 0, LBTN_RIGHT = 1, LBTN_MIDDLE = 2 };
typedef struct {
    uint8_t kind;
    uint8_t button;
    uint16_t reserved;
    float x, y;
    float scrollX, scrollY; // notches
} LunarPointer;

typedef struct {
    uint16_t vk;      // Windows virtual-key code
    uint8_t down;
    uint8_t reserved;
} LunarKey;

typedef struct {
    uint8_t visible;
    uint8_t reserved[3];
    float x, y;  // normalized over the streamed monitor
} LunarCursor;

typedef struct {
    float encodeMs;
    float fps;
    float bitrateMbps;
    float vrRenderFps;
    float hostLatencyMs; // pose received -> frame sent
} LunarHostStats;

typedef struct {
    float decodeMs;
    float fps;
    float lossPercent;
    float networkMs;
    uint32_t framesDropped;
} LunarClientStats;

typedef struct {
    uint64_t clientTimeUs;
    uint64_t hostTimeUs;
} LunarPing;

// ---------------------------------------------------------------------------
// Media channel (UDP)
// ---------------------------------------------------------------------------
enum {
    LPKT_VIDEO = 1,     // host -> client
    LPKT_AUDIO = 2,     // host -> client
    LPKT_TRACKING = 3,  // client -> host
    LPKT_MIC = 4,       // client -> host
    LPKT_HELLO = 5,     // client -> host (opens NAT/firewall path, carries stream id)
};

typedef struct {
    uint32_t magic;
    uint8_t type;
    uint8_t reserved[3];
} LunarPacketHeader;

enum { LVIDEO_FLAG_KEYFRAME = 1, LVIDEO_FLAG_HAS_POSE = 2 };

typedef struct {
    LunarPacketHeader h;
    uint32_t streamId;
    uint32_t frameIndex;
    uint32_t frameSize;     // total bytes of the encoded frame
    uint16_t chunkIndex;
    uint16_t chunkCount;
    uint8_t flags;
    uint8_t reserved[3];
    uint64_t trackingTimeUs;  // client time of the tracking sample used for this frame (0 if none)
    LPose headPose;           // pose the frame was rendered with (VR)
} LunarVideoChunk;            // followed by payload bytes

#define LUNAR_VIDEO_CHUNK_PAYLOAD (LUNAR_MAX_UDP_PAYLOAD - (int)sizeof(LunarVideoChunk))

#define LUNAR_AUDIO_RATE 48000
#define LUNAR_AUDIO_FRAMES_PER_PACKET 240   // 5 ms
typedef struct {
    LunarPacketHeader h;
    uint32_t seq;
    uint16_t channels;
    uint16_t frames;
} LunarAudioPacket;  // followed by int16 samples (interleaved)

#define LUNAR_MIC_FRAMES_PER_PACKET 480  // 10 ms mono
typedef struct {
    LunarPacketHeader h;
    uint32_t seq;
    uint16_t channels;
    uint16_t frames;
} LunarMicPacket;    // followed by int16 samples

// Controller buttons (bitmask)
enum {
    LBTN_A = 1 << 0,  // right: A, left: X
    LBTN_B = 1 << 1,  // right: B, left: Y
    LBTN_MENU = 1 << 2,
    LBTN_STICK = 1 << 3,
    LBTN_SYSTEM = 1 << 4,
    LTOUCH_A = 1 << 8,
    LTOUCH_B = 1 << 9,
    LTOUCH_STICK = 1 << 10,
    LTOUCH_TRIGGER = 1 << 11,
    LTOUCH_THUMBREST = 1 << 12,
};

typedef struct {
    uint8_t active;    // tracked/connected
    uint8_t reserved[3];
    LPose gripPose;
    LPose aimPose;
    LVec3 linearVelocity;
    LVec3 angularVelocity;
    uint32_t buttons;
    float trigger;
    float grip;
    float stickX, stickY;
} LunarController;

typedef struct {
    LunarPacketHeader h;
    uint32_t seq;
    uint64_t clientTimeUs;     // when sampled
    uint64_t targetTimeUs;     // display time the pose was predicted for
    LPose head;
    LVec3 headLinearVelocity;
    LVec3 headAngularVelocity;
    LunarController controllers[2];
    uint8_t inputToHost;       // 0 when the Lunar overlay owns the controllers
    uint8_t reserved[3];
} LunarTracking;

#pragma pack(pop)

#ifdef __cplusplus
static inline void lunarInitPacket(LunarPacketHeader &h, uint8_t type) {
    h.magic = LUNAR_MAGIC;
    h.type = type;
    h.reserved[0] = h.reserved[1] = h.reserved[2] = 0;
}
template <size_t N>
static inline void lunarCopyStr(char (&dst)[N], const char *src) {
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= N) n = N - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}
#endif
