// Lunar Desktop for Quest: ties together OpenXR, the Lunar scene, the glass UI and the PC stream.
#pragma once
#include <android_native_app_glue.h>
#include <atomic>
#include <mutex>
#include <string>
#include "audio.h"
#include "decoder.h"
#include "font.h"
#include "gfx.h"
#include "scene.h"
#include "stream.h"
#include "ui.h"
#include "xr.h"

struct Prefs {
    std::string deviceId;
    std::string lastHost;
    std::string theme = "lunar", cAccent, cAccent2, cBg, cInk;
    bool clunk = false;
    float screenWidth = 2.6f;   // metres
    float screenDist = 2.3f;
    float curve = 0.5f;         // 0 flat .. 1 wraps around you
    bool ground = true, stars = true, bigMoon = true;
    bool autoConnect = true;
    void load(const std::string &path);
    void save(const std::string &path) const;
};

struct Panel {
    RenderTarget rt;
    int lw = 0, lh = 0;      // logical UI size
    float wM = 0, hM = 0;    // metres
    Mat4 pose = Mat4::Identity();
    bool visible = false;
    UiPointer ptr;
    bool create(int logicalW, int logicalH, float widthM, float scale);
    Mat4 model() const { return pose * Mat4::Scale(Vec3(wM, hM, 1)); }
};

class App {
public:
    bool init(android_app *app);
    void shutdown();
    bool pollXr() { return m_xr.pollEvents(); }
    bool running() const { return m_xr.sessionRunning(); }
    void frame();

private:
    enum Tab { TAB_STREAM, TAB_SCREEN, TAB_LOOK, TAB_INFO };

    // network callbacks
    void onStreamConfig(const LunarStreamConfig &c);
    void onFrame(const uint8_t *d, size_t n, const FrameMeta &m, bool key);
    void onMessage(uint16_t type, const uint8_t *d, uint32_t n);

    void updateConnection();
    void sendViewConfig();
    void sendTracking();
    void handleInput();
    void recenter();
    void layoutPanels();
    void applyTheme();
    void savePrefs();

    // UI builders
    void drawConnectUi(Panel &p);
    void drawDockUi(Panel &p);
    void drawSettingsUi(Panel &p);
    void drawKeyboardUi(Panel &p);
    void drawOverlayUi(int w, int h);
    void renderPanel(Panel &p, void (App::*fn)(Panel &));

    // pointers
    bool rayPanel(const Panel &p, const Vec3 &o, const Vec3 &d, float *t, float *px, float *py);
    bool rayScreen(const Vec3 &o, const Vec3 &d, float *t, float *u, float *v);
    void setSetting(const char *key, const std::string &value);
    void sendKey(uint16_t vk, bool down);
    void sendText(const std::string &s);
    void action(uint32_t a);

    android_app *m_app = nullptr;
    XrApp m_xr;
    Scene m_scene;
    FontAtlas m_fonts;
    Ui m_ui;
    StreamClient m_net;
    Decoder m_decoder;
    AudioPlayer m_audio;
    MicCapture m_mic;
    Prefs m_prefs;
    std::string m_prefsPath;
    Theme m_theme;

    // host state (from the control thread)
    std::mutex m_lock;
    LunarHostState m_host = {};
    bool m_haveHostState = false;
    LunarHostStats m_hostStats = {};
    LunarStreamConfig m_cfg = {};
    std::atomic<bool> m_haveCfg{false};
    std::atomic<bool> m_welcomed{false};
    LunarCursor m_cursor = {};
    std::vector<LunarHaptic> m_haptics;
    FrameMeta m_meta[64];
    bool m_themeDirty = true;

    // view config we told the PC
    LunarViewConfig m_sentView = {};
    bool m_viewSent = false;
    double m_lastViewCheck = 0;

    // placement
    LPose m_anchor = lpIdentity();  // floor point + yaw in front of which everything is placed
    bool m_anchored = false;
    float m_eyeHeight = 1.6f;
    Mesh m_screenMesh;
    float m_meshW = 0, m_meshH = 0, m_meshR = -1;
    Mat4 m_screenPose = Mat4::Identity();
    float m_screenH = 1.4f;

    Panel m_main, m_dock, m_settings, m_keyboard;
    bool m_settingsOpen = false, m_keyboardOpen = false, m_overlayOpen = false;
    Tab m_tab = TAB_STREAM;
    LPose m_overlayPose = lpIdentity();
    UiPointer m_overlayPtr;

    // input state
    int m_activeHand = 1;
    float m_prevTrigger[2] = {0, 0}, m_prevSqueeze[2] = {0, 0};
    uint32_t m_prevButtons[2] = {0, 0};
    double m_menuDownAt = -1;
    bool m_menuLongFired = false;
    int m_menuPulse = 0, m_systemPulse = 0;
    bool m_screenHover = false;
    float m_screenU = 0.5f, m_screenV = 0.5f;
    bool m_mouseDown[2] = {false, false};
    double m_lastScroll = 0;
    Vec3 m_laserEnd[2];
    bool m_laserOn[2] = {false, false};
    float m_bitrateDrag = -1;
    bool m_shift = false, m_ctrl = false, m_alt = false, m_win = false;
    std::string m_manualIp;
    bool m_manualEntry = false;

    // timing / stats
    double m_start = 0, m_lastStats = 0, m_lastPing = 0, m_lastConnectTry = 0;
    uint32_t m_framesShown = 0, m_lastShownIndex = 0;
    float m_displayFps = 0, m_latencyMs = 0;
    double m_fpsWindowStart = 0;
    uint32_t m_fpsFrames = 0;
    bool m_micAsked = false;
    bool m_audioOn = false;
    int m_battery = -1;
};
