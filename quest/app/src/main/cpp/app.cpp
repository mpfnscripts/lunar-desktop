#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <fstream>
#include <sstream>
#include "platform.h"

static const char *kVersion = "1.0.0";

// ------------------------------------------------------------------------------------------------
// Prefs
// ------------------------------------------------------------------------------------------------
void Prefs::load(const std::string &path) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "deviceId") deviceId = v;
        else if (k == "lastHost") lastHost = v;
        else if (k == "theme") theme = v;
        else if (k == "cAccent") cAccent = v;
        else if (k == "cAccent2") cAccent2 = v;
        else if (k == "cBg") cBg = v;
        else if (k == "cInk") cInk = v;
        else if (k == "clunk") clunk = v == "1";
        else if (k == "screenWidth") screenWidth = (float)atof(v.c_str());
        else if (k == "screenDist") screenDist = (float)atof(v.c_str());
        else if (k == "curve") curve = (float)atof(v.c_str());
        else if (k == "ground") ground = v == "1";
        else if (k == "stars") stars = v == "1";
        else if (k == "bigMoon") bigMoon = v == "1";
        else if (k == "autoConnect") autoConnect = v == "1";
    }
    if (screenWidth < 1.0f || screenWidth > 6.f) screenWidth = 2.6f;
    if (screenDist < 1.0f || screenDist > 6.f) screenDist = 2.3f;
}

void Prefs::save(const std::string &path) const {
    std::ofstream f(path, std::ios::trunc);
    f << "deviceId=" << deviceId << "\nlastHost=" << lastHost << "\ntheme=" << theme << "\ncAccent=" << cAccent << "\ncAccent2=" << cAccent2
      << "\ncBg=" << cBg << "\ncInk=" << cInk << "\nclunk=" << (clunk ? 1 : 0) << "\nscreenWidth=" << screenWidth << "\nscreenDist=" << screenDist
      << "\ncurve=" << curve << "\nground=" << (ground ? 1 : 0) << "\nstars=" << (stars ? 1 : 0) << "\nbigMoon=" << (bigMoon ? 1 : 0)
      << "\nautoConnect=" << (autoConnect ? 1 : 0) << "\n";
}

bool Panel::create(int logicalW, int logicalH, float widthM, float scale) {
    lw = logicalW;
    lh = logicalH;
    wM = widthM;
    hM = widthM * logicalH / logicalW;
    return rt.create((int)(logicalW * scale), (int)(logicalH * scale), false, true);
}

static LQuat YawQuat(float yaw) { return LQuat{0, sinf(yaw / 2), 0, cosf(yaw / 2)}; }

// ------------------------------------------------------------------------------------------------
// Init
// ------------------------------------------------------------------------------------------------
bool App::init(android_app *app) {
    m_app = app;
    platform::Init(app);
    m_prefsPath = platform::FilesDir() + "/lunar_prefs.txt";
    m_prefs.load(m_prefsPath);
    if (m_prefs.deviceId.empty()) {
        char buf[40];
        srand((unsigned)NowUs());
        snprintf(buf, sizeof(buf), "%08x%08x%08x", rand(), rand(), rand());
        m_prefs.deviceId = buf;
        savePrefs();
    }
    m_theme = MakeTheme(m_prefs.theme, m_prefs.cAccent.c_str(), m_prefs.cAccent2.c_str(), m_prefs.cBg.c_str(), m_prefs.cInk.c_str());
    m_theme.clunk = m_prefs.clunk;

    if (!m_xr.init(app)) {
        LOGE("OpenXR init failed");
        return false;
    }
    AAssetManager *am = app->activity->assetManager;
    if (!m_fonts.load(am)) return false;
    m_ui.init(&m_fonts);
    m_scene.init(am);
    m_scene.setTheme(m_theme);

    m_main.create(1300, 860, 1.3f, 1.4f);
    m_dock.create(1100, 150, 1.0f, 1.6f);
    m_settings.create(1000, 780, 1.0f, 1.4f);
    m_keyboard.create(1000, 360, 0.95f, 1.5f);

    platform::AcquireWifiLocks();
    if (!platform::HasPermission("android.permission.RECORD_AUDIO")) platform::RequestPermission("android.permission.RECORD_AUDIO");

    LunarHello &h = m_net.hello;
    memset(&h, 0, sizeof(h));
    lunarCopyStr(h.deviceId, m_prefs.deviceId.c_str());
    lunarCopyStr(h.deviceName, platform::DeviceModel().c_str());
    h.recommendedEyeWidth = m_xr.eyeW;
    h.recommendedEyeHeight = m_xr.eyeH;
    for (size_t i = 0; i < m_xr.refreshRates.size() && i < 8; i++) h.refreshRates[i] = m_xr.refreshRates[i];
    h.currentRefreshRate = m_xr.currentRefresh;
    h.supportsHevc = 1;

    StreamClient::Callbacks cb;
    cb.onStreamConfig = [this](const LunarStreamConfig &c) { onStreamConfig(c); };
    cb.onFrame = [this](const uint8_t *d, size_t n, const FrameMeta &m, bool k) { onFrame(d, n, m, k); };
    cb.onAudio = [this](const int16_t *s, int frames) { m_audio.push(s, frames); };
    cb.onMessage = [this](uint16_t t, const uint8_t *d, uint32_t n) { onMessage(t, d, n); };
    m_net.start(cb);
    m_start = NowSec();
    LOGI("Lunar Desktop %s ready (%s)", kVersion, h.deviceName);
    return true;
}

void App::shutdown() {
    m_mic.stop();
    m_audio.stop();
    m_net.stop();
    m_decoder.shutdown();
    m_decoder.destroyGl();
    platform::ReleaseWifiLocks();
    m_xr.shutdown();
}

void App::savePrefs() { m_prefs.save(m_prefsPath); }

// ------------------------------------------------------------------------------------------------
// Network callbacks (network threads)
// ------------------------------------------------------------------------------------------------
void App::onStreamConfig(const LunarStreamConfig &c) {
    LOGI("stream %u: %s %ux%u %s %.0f fps", c.streamId, c.mode == LUNAR_MODE_VR ? "VR" : "desktop", c.width, c.height, c.codec ? "HEVC" : "H.264", c.fps);
    m_decoder.configure(c.codec, (int)c.width, (int)c.height);
    {
        std::lock_guard<std::mutex> lk(m_lock);
        m_cfg = c;
    }
    m_haveCfg = true;
    m_net.requestIdr();
}

void App::onFrame(const uint8_t *d, size_t n, const FrameMeta &m, bool key) {
    {
        std::lock_guard<std::mutex> lk(m_lock);
        m_meta[m.frameIndex % 64] = m;
    }
    m_decoder.submit(d, n, m.frameIndex, key);
    if (m_decoder.waitingForKeyframe()) m_net.requestIdr();
}

void App::onMessage(uint16_t type, const uint8_t *d, uint32_t n) {
    switch (type) {
        case LMSG_WELCOME:
            m_welcomed = true;
            m_viewSent = false;
            break;
        case LMSG_HOST_STATE:
            if (n >= sizeof(LunarHostState)) {
                std::lock_guard<std::mutex> lk(m_lock);
                memcpy(&m_host, d, sizeof(m_host));
                m_host.theme[sizeof(m_host.theme) - 1] = 0;
                m_haveHostState = true;
                m_themeDirty = true;
            }
            break;
        case LMSG_HOST_STATS:
            if (n >= sizeof(LunarHostStats)) {
                std::lock_guard<std::mutex> lk(m_lock);
                memcpy(&m_hostStats, d, sizeof(m_hostStats));
            }
            break;
        case LMSG_HAPTIC:
            if (n >= sizeof(LunarHaptic)) {
                std::lock_guard<std::mutex> lk(m_lock);
                LunarHaptic h;
                memcpy(&h, d, sizeof(h));
                m_haptics.push_back(h);
            }
            break;
        case LMSG_CURSOR:
            if (n >= sizeof(LunarCursor)) {
                std::lock_guard<std::mutex> lk(m_lock);
                memcpy(&m_cursor, d, sizeof(m_cursor));
            }
            break;
        default:
            break;
    }
}

// ------------------------------------------------------------------------------------------------
// Connection housekeeping (render thread)
// ------------------------------------------------------------------------------------------------
void App::updateConnection() {
    double now = NowSec();
    StreamClient::State st = m_net.state();
    bool connected = st == StreamClient::CONNECTED;

    if (!connected && m_haveCfg) {
        m_haveCfg = false;
        m_welcomed = false;
        m_decoder.configure(0, 0, 0);
        m_audio.stop();
        m_audioOn = false;
        m_mic.stop();
        m_overlayOpen = false;
    }
    if ((st == StreamClient::IDLE || st == StreamClient::FAILED) && m_prefs.autoConnect && !m_prefs.lastHost.empty() &&
        now - m_lastConnectTry > 3.0 && now - m_start > 1.5) {
        m_lastConnectTry = now;
        m_net.connect(m_prefs.lastHost);
    }
    if (connected && m_welcomed) {
        if (!m_viewSent || now - m_lastViewCheck > 2.0) {
            m_lastViewCheck = now;
            sendViewConfig();
        }
        if (!m_audioOn) m_audioOn = m_audio.start();
        if (now - m_lastPing > 1.0) {
            m_lastPing = now;
            LunarPing p = {NowUs(), 0};
            m_net.send(LMSG_PING, p);
        }
        bool hostMic;
        {
            std::lock_guard<std::mutex> lk(m_lock);
            hostMic = m_haveHostState && m_host.micActive;
        }
        if (hostMic && !m_mic.running() && platform::HasPermission("android.permission.RECORD_AUDIO")) {
            m_mic.start([this](const int16_t *s, int frames) { m_net.sendMic(s, frames); });
        } else if (!hostMic && m_mic.running()) {
            m_mic.stop();
        }
    }
    if (now - m_lastStats > 1.0) {
        m_lastStats = now;
        m_battery = platform::BatteryPercent();
        if (connected) {
            LunarClientStats cs = {};
            cs.decodeMs = m_decoder.decodeMs();
            cs.fps = m_displayFps;
            cs.lossPercent = m_net.lossPercent();
            cs.networkMs = m_latencyMs > 0 ? m_latencyMs : m_net.rttMs();
            cs.framesDropped = m_net.framesDropped();
            m_net.send(LMSG_CLIENT_STATS, cs);
        }
    }
}

void App::sendViewConfig() {
    LunarViewConfig v = {};
    for (int i = 0; i < 2; i++) {
        const XrFovf &f = m_xr.views[i].fov;
        v.fov[i] = {f.angleLeft, f.angleRight, f.angleUp, f.angleDown};
        v.eyeInHead[i] = m_xr.eyeInHead[i];
    }
    v.refreshRate = m_xr.currentRefresh;
    if (v.fov[0].angleRight - v.fov[0].angleLeft < 0.1f) return;  // views not located yet
    bool changed = !m_viewSent || fabsf(v.refreshRate - m_sentView.refreshRate) > 0.5f ||
                   fabsf((v.eyeInHead[1].position.x - v.eyeInHead[0].position.x) - (m_sentView.eyeInHead[1].position.x - m_sentView.eyeInHead[0].position.x)) > 0.001f;
    if (!changed) return;
    if (m_net.send(LMSG_VIEW_CONFIG, v)) {
        m_sentView = v;
        m_viewSent = true;
        LOGI("view config sent: %.0f Hz, ipd %.1f mm", v.refreshRate, (v.eyeInHead[1].position.x - v.eyeInHead[0].position.x) * 1000);
    }
}

void App::applyTheme() {
    if (!m_themeDirty) return;
    m_themeDirty = false;
    LunarHostState h;
    bool have;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        h = m_host;
        have = m_haveHostState;
    }
    if (have) {
        m_prefs.theme = h.theme[0] ? h.theme : "lunar";
        auto s = [](const char *c, size_t n) { return std::string(c, strnlen(c, n)); };
        m_prefs.cAccent = s(h.customAccent, 8);
        m_prefs.cAccent2 = s(h.customAccent2, 8);
        m_prefs.cBg = s(h.customBg, 8);
        m_prefs.cInk = s(h.customInk, 8);
        m_prefs.clunk = h.clunkMode != 0;
        savePrefs();
        if (h.refreshPref) {
            for (float r : m_xr.refreshRates)
                if (fabsf(r - h.refreshPref) < 0.5f && fabsf(m_xr.currentRefresh - r) > 0.5f) m_xr.requestRefresh(r);
        }
    }
    m_theme = MakeTheme(m_prefs.theme, m_prefs.cAccent.c_str(), m_prefs.cAccent2.c_str(), m_prefs.cBg.c_str(), m_prefs.cInk.c_str());
    m_theme.clunk = m_prefs.clunk;
    m_scene.setTheme(m_theme);
}

void App::setSetting(const char *key, const std::string &value) {
    LunarSetting s = {};
    lunarCopyStr(s.key, key);
    lunarCopyStr(s.value, value.c_str());
    m_net.send(LMSG_SET_SETTING, s);
}

void App::action(uint32_t a) {
    LunarAction act = {};
    act.action = a;
    m_net.send(LMSG_ACTION, act);
}

void App::sendKey(uint16_t vk, bool down) {
    LunarKey k = {vk, (uint8_t)(down ? 1 : 0), 0};
    m_net.send(LMSG_KEY, k);
}

void App::sendText(const std::string &s) { m_net.send(LMSG_TEXT, s.data(), (uint32_t)s.size()); }

// ------------------------------------------------------------------------------------------------
// Placement
// ------------------------------------------------------------------------------------------------
void App::recenter() {
    Vec3 f(lqRotate(m_xr.head.orientation, LVec3{0, 0, -1}));
    float yaw = atan2f(-f.x, -f.z);
    m_anchor.orientation = YawQuat(yaw);
    m_anchor.position = {m_xr.head.position.x, 0, m_xr.head.position.z};
    m_eyeHeight = m_xr.head.position.y;
    if (!m_xr.hasStage) m_anchor.position.y = m_eyeHeight - 1.6f;  // LOCAL space: origin is the head
    m_eyeHeight = m_xr.head.position.y - m_anchor.position.y;
    if (m_eyeHeight < 0.8f || m_eyeHeight > 2.2f) m_eyeHeight = 1.6f;
    m_anchored = true;
    LOGI("recentered (eye height %.2f m)", m_eyeHeight);
}

void App::layoutPanels() {
    Mat4 A = Mat4::FromPose(m_anchor);
    float aspect = 16.f / 9.f;
    if (m_haveCfg && m_cfg.contentHeight) aspect = (float)m_cfg.contentWidth / m_cfg.contentHeight;
    float w = m_prefs.screenWidth, h = w / aspect;
    float radius = m_prefs.curve < 0.02f ? 0.f : m_prefs.screenDist / m_prefs.curve;
    if (fabsf(w - m_meshW) > 1e-4f || fabsf(h - m_meshH) > 1e-4f || fabsf(radius - m_meshR) > 1e-4f) {
        m_screenMesh.destroy();
        m_screenMesh = MakeCurvedQuad(w, h, radius, radius > 0 ? 48 : 1);
        m_meshW = w;
        m_meshH = h;
        m_meshR = radius;
    }
    m_screenH = h;
    float sy = m_eyeHeight - 0.05f;
    if (sy - h / 2 < 0.25f) sy = 0.25f + h / 2;
    m_screenPose = A * Mat4::Translate(Vec3(0, sy, -m_prefs.screenDist));

    m_main.pose = A * Mat4::Translate(Vec3(0, m_eyeHeight - 0.1f, -1.5f));
    m_dock.pose = A * Mat4::Translate(Vec3(0, m_eyeHeight - 0.55f, -0.95f)) * Mat4::RotateX(-0.52f);
    m_settings.pose = A * Mat4::Translate(Vec3(0.78f, m_eyeHeight - 0.12f, -1.0f)) * Mat4::RotateY(-0.6f);
    m_keyboard.pose = A * Mat4::Translate(Vec3(0, m_eyeHeight - 0.82f, -0.62f)) * Mat4::RotateX(-0.78f);
}

bool App::rayPanel(const Panel &p, const Vec3 &o, const Vec3 &d, float *t, float *px, float *py) {
    if (!p.visible) return false;
    Mat4 inv = p.pose.rigidInverse();
    Vec3 lo = inv.transformPoint(o);
    Vec3 ld = inv.transformPoint(o + d) - lo;
    if (fabsf(ld.z) < 1e-5f) return false;
    float tt = -lo.z / ld.z;
    if (tt <= 0) return false;
    Vec3 hit = lo + ld * tt;
    float u = hit.x / p.wM + 0.5f, v = 0.5f - hit.y / p.hM;
    if (u < 0 || u > 1 || v < 0 || v > 1) return false;
    *t = tt;
    *px = u * p.lw;
    *py = v * p.lh;
    return true;
}

bool App::rayScreen(const Vec3 &o, const Vec3 &d, float *t, float *u, float *v) {
    Mat4 inv = m_screenPose.rigidInverse();
    Vec3 lo = inv.transformPoint(o);
    Vec3 ld = (inv.transformPoint(o + d) - lo).norm();
    float R = m_meshR;
    float x, y, tt;
    if (R <= 0) {
        if (fabsf(ld.z) < 1e-5f) return false;
        tt = -lo.z / ld.z;
        if (tt <= 0) return false;
        x = lo.x + ld.x * tt;
        y = lo.y + ld.y * tt;
        *u = x / m_meshW + 0.5f;
    } else {
        // cylinder around the vertical axis through (0, R) in local XZ
        float ox = lo.x, oz = lo.z - R, dx = ld.x, dz = ld.z;
        float a = dx * dx + dz * dz, b = 2 * (ox * dx + oz * dz), c = ox * ox + oz * oz - R * R;
        float disc = b * b - 4 * a * c;
        if (disc < 0 || a < 1e-8f) return false;
        float s = sqrtf(disc);
        float t0 = (-b - s) / (2 * a), t1 = (-b + s) / (2 * a);
        tt = -1;
        for (float cand : {t0, t1}) {
            if (cand <= 0) continue;
            float hz = lo.z + ld.z * cand;
            if (hz < R && (tt < 0 || cand < tt)) tt = cand;  // the half facing the viewer
        }
        if (tt < 0) return false;
        x = lo.x + ld.x * tt;
        y = lo.y + ld.y * tt;
        float hz = lo.z + ld.z * tt;
        float ang = atan2f(x, R - hz);
        *u = ang / (m_meshW / R) + 0.5f;
    }
    *v = 0.5f - y / m_meshH;
    *t = tt;
    return *u >= 0 && *u <= 1 && *v >= 0 && *v <= 1;
}

// ------------------------------------------------------------------------------------------------
// Input
// ------------------------------------------------------------------------------------------------
void App::handleInput() {
    double now = NowSec();
    bool vrMode = m_haveCfg && m_cfg.mode == LUNAR_MODE_VR;
    bool connected = m_net.state() == StreamClient::CONNECTED;
    XrHand *H = m_xr.hands;

    for (int i = 0; i < 2; i++)
        if (H[i].active && H[i].trigger > 0.6f && m_prevTrigger[i] <= 0.6f) m_activeHand = i;

    // Left menu button: short press = game menu (VR) / settings (desktop), long press = Lunar overlay / recenter
    bool menu = (H[0].buttons & LBTN_MENU) != 0;
    bool menuPrev = (m_prevButtons[0] & LBTN_MENU) != 0;
    if (menu && !menuPrev) {
        m_menuDownAt = now;
        m_menuLongFired = false;
    }
    if (menu && !m_menuLongFired && m_menuDownAt > 0 && now - m_menuDownAt > 0.7) {
        m_menuLongFired = true;
        m_xr.haptic(0, 0.5f, 0.04f, 0);
        if (vrMode) {
            m_overlayOpen = !m_overlayOpen;
            if (m_overlayOpen) {
                Vec3 f(lqRotate(m_xr.head.orientation, LVec3{0, 0, -1}));
                float yaw = atan2f(-f.x, -f.z);
                m_overlayPose.orientation = YawQuat(yaw);
                Vec3 p = Vec3(m_xr.head.position) + Vec3(-sinf(yaw), 0, -cosf(yaw)) * 1.1f;
                m_overlayPose.position = {p.x, m_xr.head.position.y - 0.1f, p.z};
            }
        } else {
            recenter();
        }
    }
    if (!menu && menuPrev && !m_menuLongFired) {
        if (vrMode) m_menuPulse = 8;
        else if (connected) m_settingsOpen = !m_settingsOpen;
    }

    // Panel visibility
    bool desktop = connected && m_haveCfg && m_cfg.mode == LUNAR_MODE_DESKTOP;
    m_main.visible = !vrMode && !desktop;
    m_dock.visible = desktop;
    m_settings.visible = (desktop || (connected && !vrMode && m_haveCfg)) && m_settingsOpen;
    m_keyboard.visible = desktop && m_keyboardOpen;
    if (m_main.visible && m_settingsOpen && connected) m_settings.visible = true;

    // keyboard toggle on B/Y in desktop mode
    for (int i = 0; i < 2; i++) {
        bool b = (H[i].buttons & LBTN_B) != 0, bp = (m_prevButtons[i] & LBTN_B) != 0;
        if (desktop && b && !bp) m_keyboardOpen = !m_keyboardOpen;
    }

    // Rays
    Panel *panels[] = {&m_main, &m_dock, &m_settings, &m_keyboard};
    for (Panel *p : panels) p->ptr = UiPointer();
    m_overlayPtr = UiPointer();
    m_screenHover = false;
    for (int i = 0; i < 2; i++) {
        m_laserOn[i] = false;
        if (!H[i].active) continue;
        Vec3 o(H[i].aim.position);
        Vec3 d(lqRotate(H[i].aim.orientation, LVec3{0, 0, -1}));
        float best = 1e9f;
        Panel *hitPanel = nullptr;
        float hx = 0, hy = 0;
        bool hitOverlay = false;
        for (Panel *p : panels) {
            float t, px, py;
            if (rayPanel(*p, o, d, &t, &px, &py) && t < best) {
                best = t;
                hitPanel = p;
                hx = px;
                hy = py;
            }
        }
        if (vrMode && m_overlayOpen) {
            Panel tmp;
            tmp.visible = true;
            tmp.lw = 1000;
            tmp.lh = 625;
            tmp.wM = 1.0f;
            tmp.hM = 0.625f;
            tmp.pose = Mat4::FromPose(m_overlayPose);
            float t, px, py;
            if (rayPanel(tmp, o, d, &t, &px, &py) && t < best) {
                best = t;
                hitPanel = nullptr;
                hitOverlay = true;
                hx = px;
                hy = py;
            }
        }
        float st = 0, su = 0, sv = 0;
        bool screenHit = desktop && rayScreen(o, d, &st, &su, &sv) && st < best;
        if (screenHit) best = st;

        bool interactive = vrMode ? m_overlayOpen : true;
        if (!interactive) continue;
        m_laserOn[i] = true;
        m_laserEnd[i] = o + d * (best < 1e8f ? best : 3.0f);
        if (i != m_activeHand) continue;

        UiPointer ptr;
        ptr.valid = true;
        ptr.x = hx;
        ptr.y = hy;
        ptr.down = H[i].trigger > 0.6f;
        ptr.pressed = ptr.down && m_prevTrigger[i] <= 0.6f;
        ptr.released = !ptr.down && m_prevTrigger[i] > 0.6f;
        ptr.scroll = H[i].stickY;
        if (hitPanel) hitPanel->ptr = ptr;
        else if (hitOverlay) m_overlayPtr = ptr;
        else if (screenHit) {
            m_screenHover = true;
            if (fabsf(su - m_screenU) > 0.0003f || fabsf(sv - m_screenV) > 0.0003f) {
                m_screenU = su;
                m_screenV = sv;
                LunarPointer lp = {LPTR_MOVE, 0, 0, su, sv, 0, 0};
                m_net.send(LMSG_POINTER, lp);
            }
            bool trig = H[i].trigger > 0.6f, trigPrev = m_prevTrigger[i] > 0.6f;
            if (trig != trigPrev) {
                LunarPointer lp = {(uint8_t)(trig ? LPTR_DOWN : LPTR_UP), LBTN_LEFT, 0, su, sv, 0, 0};
                m_net.send(LMSG_POINTER, lp);
                m_mouseDown[0] = trig;
                m_xr.haptic(i, trig ? 0.35f : 0.15f, 0.015f, 0);
            }
            bool sq = H[i].squeeze > 0.6f, sqPrev = m_prevSqueeze[i] > 0.6f;
            if (sq != sqPrev) {
                LunarPointer lp = {(uint8_t)(sq ? LPTR_DOWN : LPTR_UP), LBTN_RIGHT, 0, su, sv, 0, 0};
                m_net.send(LMSG_POINTER, lp);
                m_mouseDown[1] = sq;
                m_xr.haptic(i, 0.3f, 0.015f, 0);
            }
            if (fabsf(H[i].stickY) > 0.25f && now - m_lastScroll > 0.05) {
                m_lastScroll = now;
                LunarPointer lp = {LPTR_SCROLL, 0, 0, su, sv, 0, H[i].stickY * 0.6f};
                m_net.send(LMSG_POINTER, lp);
            } else if (fabsf(H[i].stickX) > 0.4f && now - m_lastScroll > 0.05) {
                m_lastScroll = now;
                LunarPointer lp = {LPTR_SCROLL, 0, 0, su, sv, H[i].stickX * 0.6f, 0};
                m_net.send(LMSG_POINTER, lp);
            }
        }
        // release buttons that were pressed on the screen even if the pointer left it
        if (!screenHit && desktop) {
            if (m_mouseDown[0] && H[i].trigger <= 0.6f) {
                LunarPointer lp = {LPTR_UP, LBTN_LEFT, 0, m_screenU, m_screenV, 0, 0};
                m_net.send(LMSG_POINTER, lp);
                m_mouseDown[0] = false;
            }
            if (m_mouseDown[1] && H[i].squeeze <= 0.6f) {
                LunarPointer lp = {LPTR_UP, LBTN_RIGHT, 0, m_screenU, m_screenV, 0, 0};
                m_net.send(LMSG_POINTER, lp);
                m_mouseDown[1] = false;
            }
        }
    }

    for (int i = 0; i < 2; i++) {
        m_prevTrigger[i] = H[i].trigger;
        m_prevSqueeze[i] = H[i].squeeze;
        m_prevButtons[i] = H[i].buttons;
    }
}

void App::sendTracking() {
    if (m_net.state() != StreamClient::CONNECTED || !m_xr.headValid) return;
    bool vrMode = m_haveCfg && m_cfg.mode == LUNAR_MODE_VR;
    LunarTracking t = {};
    t.clientTimeUs = NowUs();
    t.targetTimeUs = t.clientTimeUs;
    t.head = m_xr.head;
    t.headLinearVelocity = m_xr.headLinVel;
    t.headAngularVelocity = m_xr.headAngVel;
    for (int i = 0; i < 2; i++) {
        const XrHand &h = m_xr.hands[i];
        LunarController &c = t.controllers[i];
        c.active = h.active;
        c.gripPose = h.grip;
        c.aimPose = h.aim;
        c.linearVelocity = h.linVel;
        c.angularVelocity = h.angVel;
        c.buttons = h.buttons;
        c.trigger = h.trigger;
        c.grip = h.squeeze;
        c.stickX = h.stickX;
        c.stickY = h.stickY;
    }
    // The menu button is held back until we know whether it's a short (game) or long (Lunar) press.
    t.controllers[0].buttons &= ~LBTN_MENU;
    if (m_menuPulse > 0) {
        t.controllers[0].buttons |= LBTN_MENU;
        m_menuPulse--;
    }
    if (m_systemPulse > 0) {
        t.controllers[0].buttons |= LBTN_SYSTEM;
        m_systemPulse--;
    }
    t.inputToHost = (vrMode && !m_overlayOpen) ? 1 : 0;
    if (m_menuPulse > 0 || m_systemPulse > 0) t.inputToHost = 1;
    m_net.sendTracking(t);
}

// ------------------------------------------------------------------------------------------------
// UI
// ------------------------------------------------------------------------------------------------
void App::renderPanel(Panel &p, void (App::*fn)(Panel &)) {
    glBindFramebuffer(GL_FRAMEBUFFER, p.rt.fbo);
    glViewport(0, 0, p.rt.w, p.rt.h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    m_ui.begin(p.lw, p.lh, m_theme, p.ptr, (float)(NowSec() - m_start), false);
    (this->*fn)(p);
    if (p.ptr.valid) {
        // pointer dot
        m_ui.circle(p.ptr.x, p.ptr.y, 10, m_theme.accent.withA(0.5f), m_theme.accent.withA(0.5f), Color(0, 0, 0, 0), 0, 8);
        m_ui.circle(p.ptr.x, p.ptr.y, p.ptr.down ? 5 : 7, Color(1, 1, 1), Color(1, 1, 1));
    }
    m_ui.end();
    glBindTexture(GL_TEXTURE_2D, p.rt.tex);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void RingViz(Ui &ui, float cx, float cy, float r0, float len, float t, float energy) {
    const Theme &th = ui.theme();
    if (th.clunk) return;
    Color bar = Color::Mix(th.accent, Color(1, 1, 1), 0.4f);
    for (int i = 0; i < 72; i++) {
        int half = i < 36 ? i : 71 - i;
        float lvl = 0.05f + 0.03f * sinf(t * 1.1f + i * 0.5f);
        lvl += energy * (0.5f + 0.5f * sinf(t * 3.8f + half * 0.45f)) * (0.6f + 0.4f * sinf(t * 1.1f + i));
        float a = i / 72.f * 6.2831853f - 1.5707963f;
        float l = 2 + lvl * len;
        ui.line(cx + cosf(a) * r0, cy + sinf(a) * r0, cx + cosf(a) * (r0 + l), cy + sinf(a) * (r0 + l), 3.5f, bar.withA(0.22f + lvl * 0.78f));
    }
}

void App::drawConnectUi(Panel &p) {
    Ui &ui = m_ui;
    const Theme &t = m_theme;
    float time = ui.time();
    ui.glassPanel(Rect(0, 0, (float)p.lw, (float)p.lh), 40);
    ui.brand(50, 70, 30);
    StreamClient::State st = m_net.state();
    bool searching = st == StreamClient::IDLE || st == StreamClient::FAILED;
    Color dot = st == StreamClient::CONNECTED ? Color::Hex("#9ff0c0") : (st == StreamClient::WAITING_APPROVAL || st == StreamClient::CONNECTING ? Color::Hex("#ffd9a8") : t.inkFaint);
    std::string pillText = st == StreamClient::CONNECTED ? "Connected" : (st == StreamClient::WAITING_APPROVAL ? "Waiting for PC" : (st == StreamClient::CONNECTING ? "Connecting" : "Searching"));
    float pw = m_fonts.measure(FONT_REGULAR, pillText.c_str(), 18, 0.1f) + 80;
    ui.pill(Rect(p.lw - 50 - pw, 46, pw, 48), pillText.c_str(), dot);

    // the moon with its corona
    float mcx = 290, mcy = 470;
    RingViz(ui, mcx, mcy, 170, 70, time, searching ? 0.25f : 0.6f);
    ui.moon(mcx, mcy, 150, st == StreamClient::CONNECTED ? 0.75f : 0.5f);

    float x = 560, w = p.lw - x - 60;
    if (st == StreamClient::WAITING_APPROVAL) {
        ui.text(x, 230, "Allow this headset", 46, t.ink, FONT_SEMIBOLD, ALIGN_LEFT, 0.01f, t.accent.withA(0.45f));
        ui.text(x, 290, ("On " + m_net.hostName() + ", click Allow in the Lunar Desktop window.").c_str(), 24, t.inkSoft, FONT_REGULAR, ALIGN_LEFT, 0.02f, Color(0, 0, 0, 0), w);
        ui.text(x, 330, "You only need to do this once.", 22, t.inkFaint, FONT_REGULAR);
        if (ui.button(10, Rect(x, 400, 220, 64), "Cancel")) {
            m_prefs.autoConnect = false;
            m_net.disconnect();
        }
        return;
    }
    if (st == StreamClient::CONNECTING || (st == StreamClient::CONNECTED && !m_haveCfg)) {
        ui.text(x, 230, st == StreamClient::CONNECTED ? "Starting the stream" : "Connecting", 46, t.ink, FONT_SEMIBOLD, ALIGN_LEFT, 0.01f, t.accent.withA(0.45f));
        ui.text(x, 290, m_net.stateText().c_str(), 24, t.inkSoft, FONT_REGULAR, ALIGN_LEFT, 0.02f, Color(0, 0, 0, 0), w);
        if (ui.button(11, Rect(x, 360, 220, 64), "Cancel")) {
            m_prefs.autoConnect = false;
            m_net.disconnect();
        }
        return;
    }

    if (m_manualEntry) {
        ui.text(x, 200, "Enter your PC's IP address", 34, t.ink, FONT_SEMIBOLD);
        Rect field(x, 240, w, 70);
        ui.rect(field, 35, Color(5 / 255.f, 8 / 255.f, 22 / 255.f, 0.5f), Color(5 / 255.f, 8 / 255.f, 22 / 255.f, 0.5f), Color(240 / 255.f, 244 / 255.f, 1, 0.38f), 1.5f);
        ui.text(field.x + 30, field.cy(), m_manualIp.empty() ? "192.168.1.20" : m_manualIp.c_str(), 30, m_manualIp.empty() ? t.inkFaint : t.ink, FONT_REGULAR);
        const char *keys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", "\xE2\x86\x90"};
        float kw = 150, kh = 66, gx = x, gy = 335;
        for (int i = 0; i < 12; i++) {
            Rect r(gx + (i % 3) * (kw + 12), gy + (i / 3) * (kh + 12), kw, kh);
            if (ui.button(100 + i, r, keys[i])) {
                if (i == 11) { if (!m_manualIp.empty()) m_manualIp.pop_back(); }
                else if (m_manualIp.size() < 15) m_manualIp += keys[i];
            }
        }
        float bx = gx + 3 * (kw + 12) + 10;
        if (ui.button(120, Rect(bx, gy, w - (bx - x), kh), "Connect", true) && !m_manualIp.empty()) {
            m_prefs.lastHost = m_manualIp;
            m_prefs.autoConnect = true;
            savePrefs();
            m_manualEntry = false;
            m_net.connect(m_manualIp);
        }
        if (ui.button(121, Rect(bx, gy + kh + 12, w - (bx - x), kh), "Back")) m_manualEntry = false;
        return;
    }

    ui.text(x, 200, "Connect to your PC", 46, t.ink, FONT_SEMIBOLD, ALIGN_LEFT, 0.01f, t.accent.withA(0.45f));
    std::string sub = st == StreamClient::FAILED ? m_net.stateText() : "Pick your computer to start streaming.";
    ui.text(x, 252, sub.c_str(), 23, t.inkSoft, FONT_REGULAR, ALIGN_LEFT, 0.02f, Color(0, 0, 0, 0), w);
    ui.sectionHeader(x, 320, "Computers on your network");

    auto hosts = m_net.hosts();
    float y = 350;
    if (hosts.empty()) {
        Rect box(x, y, w, 200);
        ui.rect(box, 26, Color(5 / 255.f, 8 / 255.f, 22 / 255.f, 0.35f), Color(5 / 255.f, 8 / 255.f, 22 / 255.f, 0.35f), Color(225 / 255.f, 232 / 255.f, 1, 0.16f), 1.5f);
        int dots = (int)(time * 2) % 4;
        std::string s = "Searching for Lunar Desktop" + std::string(dots, '.');
        ui.text(box.x + 30, box.y + 50, s.c_str(), 26, t.ink, FONT_REGULAR);
        ui.text(box.x + 30, box.y + 100, "Run LunarDesktop.exe on your PC.", 22, t.inkSoft, FONT_REGULAR, ALIGN_LEFT, 0, Color(0, 0, 0, 0), box.w - 60);
        ui.text(box.x + 30, box.y + 136, "Both must be on the same Wi-Fi / network.", 22, t.inkSoft, FONT_REGULAR, ALIGN_LEFT, 0, Color(0, 0, 0, 0), box.w - 60);
        y += 220;
    } else {
        int i = 0;
        for (auto &h : hosts) {
            if (i >= 4) break;
            Rect r(x, y, w, 96);
            bool sel = h.ip == m_prefs.lastHost;
            if (ui.row(200 + i, r, sel)) {
                m_prefs.lastHost = h.ip;
                m_prefs.autoConnect = true;
                savePrefs();
                m_net.connect(h.ip);
            }
            ui.moon(r.x + 52, r.cy(), 30, 0.25f);
            ui.text(r.x + 104, r.cy() - 16, h.name.c_str(), 28, sel ? Color(1, 1, 1) : t.ink, FONT_SEMIBOLD, ALIGN_LEFT, 0, Color(0, 0, 0, 0), r.w - 360);
            ui.text(r.x + 104, r.cy() + 20, h.ip.c_str(), 20, t.inkSoft, FONT_REGULAR);
            if (h.steamvrReady) ui.pill(Rect(r.x + r.w - 250, r.cy() - 22, 220, 44), "SteamVR ready", Color::Hex("#9ff0c0"));
            y += 106;
            i++;
        }
        y += 10;
    }
    if (ui.button(20, Rect(x, p.lh - 130.f, 300, 70), "Enter IP address", false, false, ICON_WIFI)) {
        m_manualEntry = true;
        m_manualIp.clear();
    }
    if (ui.button(21, Rect(x + 320, p.lh - 130.f, 240, 70), "Settings", false, m_settingsOpen, ICON_SETTINGS)) {
        m_settingsOpen = !m_settingsOpen;
        m_tab = TAB_LOOK;
    }
    char ver[64];
    snprintf(ver, sizeof(ver), "Lunar Desktop %s", kVersion);
    ui.text(p.lw - 50.f, p.lh - 40.f, ver, 18, t.inkFaint, FONT_REGULAR, ALIGN_RIGHT, 0.04f);
}

void App::drawDockUi(Panel &p) {
    Ui &ui = m_ui;
    const Theme &t = m_theme;
    ui.glassPanel(Rect(0, 0, (float)p.lw, (float)p.lh), p.lh / 2.f);
    LunarHostState hs;
    LunarHostStats stats;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        hs = m_host;
        stats = m_hostStats;
    }
    float cy = p.lh / 2.f;
    ui.moon(80, cy, 40, 0.5f);
    ui.text(140, cy - 20, m_net.hostName().c_str(), 30, t.ink, FONT_SEMIBOLD, ALIGN_LEFT, 0, t.accent.withA(0.3f), 300);
    char buf[96];
    snprintf(buf, sizeof(buf), "%.0f fps  \xC2\xB7  %.0f Mbps  \xC2\xB7  %.0f ms", m_displayFps, m_net.mbps(), m_latencyMs > 0 ? m_latencyMs : m_net.rttMs());
    ui.text(140, cy + 22, buf, 20, t.inkSoft, FONT_REGULAR);

    float x = 520, gap = 92, r = 36;
    if (ui.roundButton(1, x, cy, r, ICON_LEFT, false, false, "Monitor")) action(LACTION_PREV_MONITOR);
    x += gap;
    snprintf(buf, sizeof(buf), "%d/%d", hs.monitorIndex + 1, hs.monitorCount ? hs.monitorCount : 1);
    ui.text(x - gap / 2, cy, buf, 18, t.inkSoft, FONT_REGULAR, ALIGN_CENTER);
    if (ui.roundButton(2, x, cy, r, ICON_RIGHT, false, false, "Monitor")) action(LACTION_NEXT_MONITOR);
    x += gap;
    if (ui.roundButton(3, x, cy, r, ICON_KEYBOARD, false, m_keyboardOpen, "Keyboard")) m_keyboardOpen = !m_keyboardOpen;
    x += gap;
    if (ui.roundButton(4, x, cy, r + 6, ICON_VR, true, false, hs.steamvrRunning ? "SteamVR" : "Launch SteamVR"))
        action(LACTION_LAUNCH_STEAMVR);
    x += gap + 6;
    if (ui.roundButton(5, x, cy, r, ICON_RECENTER, false, false, "Recenter")) recenter();
    x += gap;
    if (ui.roundButton(6, x, cy, r, ICON_SETTINGS, false, m_settingsOpen, "Settings")) m_settingsOpen = !m_settingsOpen;
    x += gap;
    if (ui.roundButton(7, x, cy, r, ICON_POWER, false, false, "Disconnect")) {
        m_prefs.autoConnect = false;
        m_net.disconnect();
    }
}

void App::drawSettingsUi(Panel &p) {
    Ui &ui = m_ui;
    const Theme &t = m_theme;
    ui.glassPanel(Rect(0, 0, (float)p.lw, (float)p.lh), 36);
    const char *tabs[] = {"Stream", "Screen", "Look", "Info"};
    float tw = (p.lw - 80.f - 30) / 4;
    for (int i = 0; i < 4; i++)
        if (ui.tab(1 + i, Rect(40 + i * (tw + 10), 30, tw, 52), tabs[i], m_tab == i)) m_tab = (Tab)i;

    LunarHostState hs;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        hs = m_host;
    }
    bool connected = m_net.state() == StreamClient::CONNECTED;
    float x = 50, y = 130, w = p.lw - 100.f;
    auto label = [&](const char *title, const char *sub) {
        ui.text(x, y, title, 26, t.ink, FONT_SEMIBOLD);
        if (sub) ui.text(x, y + 32, sub, 19, t.inkFaint, FONT_REGULAR, ALIGN_LEFT, 0, Color(0, 0, 0, 0), w * 0.5f);
    };
    char buf[128];
    if (m_tab == TAB_STREAM) {
        if (!connected) {
            ui.text(x, y, "Connect to your PC to change stream settings.", 24, t.inkSoft);
            return;
        }
        label("VR quality", "Sharper needs a faster GPU and Wi-Fi");
        const char *q[] = {"Low", "Medium", "High", "Ultra"};
        int s = ui.segmented(20, Rect(x + w * 0.45f, y - 28, w * 0.55f, 60), q, 4, hs.quality);
        if (s >= 0) setSetting("quality", std::to_string(s));
        y += 110;
        label("Bitrate", "100-150 Mbps suits Wi-Fi 6");
        float br = m_bitrateDrag >= 0 ? m_bitrateDrag : hs.bitrateKbps / 1000.f;
        if (ui.slider(21, Rect(x + w * 0.45f, y - 10, w * 0.42f, 20), &br, 10, 250)) m_bitrateDrag = br;
        snprintf(buf, sizeof(buf), "%.0f Mbps", br);
        ui.text(x + w, y, buf, 22, t.inkSoft, FONT_REGULAR, ALIGN_RIGHT);
        if (m_bitrateDrag >= 0 && !p.ptr.down) {
            setSetting("bitrateMbps", std::to_string((int)(m_bitrateDrag / 5 + 0.5f) * 5));
            m_bitrateDrag = -1;
        }
        y += 110;
        label("Codec", "HEVC looks cleaner at the same bitrate");
        const char *c[] = {"Auto", "H.264", "HEVC"};
        s = ui.segmented(22, Rect(x + w * 0.45f, y - 28, w * 0.55f, 60), c, 3, hs.codecPref);
        if (s >= 0) setSetting("codec", s == 0 ? "auto" : (s == 1 ? "h264" : "hevc"));
        y += 110;
        label("Refresh rate", "Higher is smoother in games");
        std::vector<std::string> labels = {"Default"};
        std::vector<int> vals = {0};
        for (float r : m_xr.refreshRates) {
            snprintf(buf, sizeof(buf), "%.0f", r);
            labels.push_back(buf);
            vals.push_back((int)(r + 0.5f));
            if (labels.size() >= 5) break;
        }
        std::vector<const char *> lp;
        for (auto &l : labels) lp.push_back(l.c_str());
        int sel = 0;
        for (size_t i = 0; i < vals.size(); i++)
            if (vals[i] == hs.refreshPref) sel = (int)i;
        s = ui.segmented(23, Rect(x + w * 0.45f, y - 28, w * 0.55f, 60), lp.data(), (int)lp.size(), sel);
        if (s >= 0) {
            setSetting("refresh", std::to_string(vals[s]));
            if (vals[s]) m_xr.requestRefresh((float)vals[s]);
        }
        y += 110;
        label("PC sound in headset", nullptr);
        if (ui.toggle(24, x + w - 72, y - 20, hs.audioStreaming)) setSetting("audio", hs.audioStreaming ? "false" : "true");
        y += 80;
        label("Headset microphone", hs.micActive ? "Sent to the PC" : "Set up on the PC (Audio tab)");
        if (ui.toggle(25, x + w - 72, y - 20, hs.micActive)) setSetting("mic", hs.micActive ? "false" : "true");
    } else if (m_tab == TAB_SCREEN) {
        float v;
        label("Screen size", nullptr);
        v = m_prefs.screenWidth;
        if (ui.slider(30, Rect(x + w * 0.4f, y - 10, w * 0.45f, 20), &v, 1.2f, 5.0f)) m_prefs.screenWidth = v;
        snprintf(buf, sizeof(buf), "%.0f\"", m_prefs.screenWidth / 0.0254f * 1.147f);
        ui.text(x + w, y, buf, 22, t.inkSoft, FONT_REGULAR, ALIGN_RIGHT);
        y += 100;
        label("Distance", nullptr);
        v = m_prefs.screenDist;
        if (ui.slider(31, Rect(x + w * 0.4f, y - 10, w * 0.45f, 20), &v, 1.2f, 5.0f)) m_prefs.screenDist = v;
        snprintf(buf, sizeof(buf), "%.1f m", m_prefs.screenDist);
        ui.text(x + w, y, buf, 22, t.inkSoft, FONT_REGULAR, ALIGN_RIGHT);
        y += 100;
        label("Curve", nullptr);
        v = m_prefs.curve;
        if (ui.slider(32, Rect(x + w * 0.4f, y - 10, w * 0.45f, 20), &v, 0.f, 1.f)) m_prefs.curve = v < 0.03f ? 0 : v;
        snprintf(buf, sizeof(buf), "%.0f%%", m_prefs.curve * 100);
        ui.text(x + w, y, buf, 22, t.inkSoft, FONT_REGULAR, ALIGN_RIGHT);
        y += 100;
        if (connected) {
            label("Monitor", nullptr);
            snprintf(buf, sizeof(buf), "%d of %d", hs.monitorIndex + 1, hs.monitorCount ? hs.monitorCount : 1);
            if (ui.button(33, Rect(x + w * 0.4f, y - 30, 70, 60), "", false, false, ICON_LEFT)) action(LACTION_PREV_MONITOR);
            ui.text(x + w * 0.4f + 150, y, buf, 22, t.ink, FONT_REGULAR, ALIGN_CENTER);
            if (ui.button(34, Rect(x + w * 0.4f + 230, y - 30, 70, 60), "", false, false, ICON_RIGHT)) action(LACTION_NEXT_MONITOR);
            y += 100;
        }
        if (ui.button(35, Rect(x, y, 300, 64), "Recenter view", false, false, ICON_RECENTER)) recenter();
        if (!p.ptr.down) savePrefs();
    } else if (m_tab == TAB_LOOK) {
        ui.sectionHeader(x, y - 10, "Theme");
        float sw = (w - 5 * 16) / 6, sh = 120;
        for (int i = 0; i < kThemeCount; i++) {
            Rect r(x + (i % 6) * (sw + 16), y + 20 + (i / 6) * (sh + 16), sw, sh);
            bool on = m_prefs.theme == kThemes[i].key && m_prefs.cAccent.empty();
            if (ui.row(40 + i, r, on)) {
                if (connected) {
                    setSetting("theme", kThemes[i].key);
                    setSetting("customAccent", "");
                    setSetting("customAccent2", "");
                    setSetting("customBg", "");
                    setSetting("customInk", "");
                }
                m_prefs.theme = kThemes[i].key;
                m_prefs.cAccent = m_prefs.cAccent2 = m_prefs.cBg = m_prefs.cInk = "";
                m_themeDirty = !connected;
                if (!connected) {
                    std::lock_guard<std::mutex> lk(m_lock);
                    m_haveHostState = false;
                }
                savePrefs();
            }
            Theme th = MakeTheme(kThemes[i].key);
            ui.circle(r.cx(), r.y + 42, 30, th.bg, th.bg);
            ui.circle(r.cx(), r.y + 42, 22, Color::Mix(Color(1, 1, 1), th.accent, 0.5f), th.accent, Color(0, 0, 0, 0), 0, 0);
            ui.circle(r.cx(), r.y + 42, 22, th.accent.withA(0.4f), th.accent.withA(0.4f), Color(0, 0, 0, 0), 0, 10);
            ui.text(r.cx(), r.y + 94, kThemes[i].name, 18, on ? Color(1, 1, 1) : t.inkSoft, FONT_REGULAR, ALIGN_CENTER, 0, Color(0, 0, 0, 0), sw - 8);
        }
        y += 20 + 2 * (sh + 16) + 40;
        auto toggleRow = [&](int id, const char *name, bool *val) {
            ui.text(x, y, name, 24, t.ink, FONT_SEMIBOLD);
            if (ui.toggle(id, x + w - 72, y - 20, *val)) {
                *val = !*val;
                savePrefs();
                return true;
            }
            y += 64;
            return false;
        };
        toggleRow(60, "Stars", &m_prefs.stars);
        toggleRow(61, "Big moon", &m_prefs.bigMoon);
        toggleRow(62, "Lunar ground", &m_prefs.ground);
        bool clunk = m_prefs.clunk;
        if (toggleRow(63, "Clunk Mode", &clunk)) {
            if (connected) setSetting("clunk", clunk ? "true" : "false");
            m_prefs.clunk = clunk;
            m_theme.clunk = clunk;
        }
    } else {
        LunarHostStats hst;
        {
            std::lock_guard<std::mutex> lk(m_lock);
            hst = m_hostStats;
        }
        auto kv = [&](const char *k, const std::string &v) {
            ui.text(x, y, k, 22, t.inkFaint, FONT_REGULAR);
            ui.text(x + 260, y, v.c_str(), 22, t.ink, FONT_SEMIBOLD, ALIGN_LEFT, 0, Color(0, 0, 0, 0), w - 260);
            y += 46;
        };
        kv("PC", connected ? m_net.hostName() + "  (" + m_net.hostIp() + ")" : "Not connected");
        snprintf(buf, sizeof(buf), "%s  %u\xC3\x97%u  %.0f fps", m_cfg.codec ? "HEVC" : "H.264", m_cfg.width, m_cfg.height, m_cfg.fps);
        kv("Stream", m_haveCfg ? buf : "-");
        snprintf(buf, sizeof(buf), "%.0f fps shown, %.1f Mbps", m_displayFps, m_net.mbps());
        kv("Picture", buf);
        snprintf(buf, sizeof(buf), "encode %.1f ms, decode %.1f ms", hst.encodeMs, m_decoder.decodeMs());
        kv("Timing", buf);
        snprintf(buf, sizeof(buf), "%.0f ms  (Wi-Fi round trip %.0f ms)", m_latencyMs, m_net.rttMs());
        kv("Latency", buf);
        snprintf(buf, sizeof(buf), "%.1f %%  (%u frames dropped)", m_net.lossPercent(), m_net.framesDropped());
        kv("Packet loss", buf);
        snprintf(buf, sizeof(buf), "%.0f Hz, eye %dx%d", m_xr.currentRefresh, m_xr.eyeW, m_xr.eyeH);
        kv("Headset", buf);
        if (m_battery >= 0) {
            snprintf(buf, sizeof(buf), "%d %%", m_battery);
            kv("Battery", buf);
        }
        kv("Version", kVersion);
        y += 10;
        if (connected && ui.button(70, Rect(x, y, 260, 64), "Disconnect", false, false, ICON_POWER)) {
            m_prefs.autoConnect = false;
            m_net.disconnect();
        }
        if (ui.button(71, Rect(x + 280, y, 260, 64), "Forget this PC")) {
            m_prefs.lastHost.clear();
            m_prefs.autoConnect = false;
            savePrefs();
            m_net.disconnect();
        }
    }
}

struct KeyDef {
    const char *label, *shifted;
    uint16_t vk;  // 0 = printable text
    float width;
};

void App::drawKeyboardUi(Panel &p) {
    Ui &ui = m_ui;
    ui.glassPanel(Rect(0, 0, (float)p.lw, (float)p.lh), 30);
    static const KeyDef rows[5][14] = {
        {{"Esc", 0, 0x1B, 1}, {"1", "!", 0, 1}, {"2", "@", 0, 1}, {"3", "#", 0, 1}, {"4", "$", 0, 1}, {"5", "%", 0, 1}, {"6", "^", 0, 1}, {"7", "&", 0, 1}, {"8", "*", 0, 1}, {"9", "(", 0, 1}, {"0", ")", 0, 1}, {"-", "_", 0, 1}, {"=", "+", 0, 1}, {"\xE2\x86\x90", 0, 0x08, 1.5f}},
        {{"Tab", 0, 0x09, 1.3f}, {"q", "Q", 0, 1}, {"w", "W", 0, 1}, {"e", "E", 0, 1}, {"r", "R", 0, 1}, {"t", "T", 0, 1}, {"y", "Y", 0, 1}, {"u", "U", 0, 1}, {"i", "I", 0, 1}, {"o", "O", 0, 1}, {"p", "P", 0, 1}, {"[", "{", 0, 1}, {"]", "}", 0, 1}, {"\\", "|", 0, 1.2f}},
        {{"Caps", 0, 0x14, 1.5f}, {"a", "A", 0, 1}, {"s", "S", 0, 1}, {"d", "D", 0, 1}, {"f", "F", 0, 1}, {"g", "G", 0, 1}, {"h", "H", 0, 1}, {"j", "J", 0, 1}, {"k", "K", 0, 1}, {"l", "L", 0, 1}, {";", ":", 0, 1}, {"'", "\"", 0, 1}, {"Enter", 0, 0x0D, 2.0f}, {0, 0, 0, 0}},
        {{"Shift", 0, 0x10, 2.0f}, {"z", "Z", 0, 1}, {"x", "X", 0, 1}, {"c", "C", 0, 1}, {"v", "V", 0, 1}, {"b", "B", 0, 1}, {"n", "N", 0, 1}, {"m", "M", 0, 1}, {",", "<", 0, 1}, {".", ">", 0, 1}, {"/", "?", 0, 1}, {"\xE2\x86\x91", 0, 0x26, 1}, {"Del", 0, 0x2E, 1.4f}, {0, 0, 0, 0}},
        {{"Ctrl", 0, 0x11, 1.4f}, {"Win", 0, 0x5B, 1.2f}, {"Alt", 0, 0x12, 1.2f}, {"", " ", 0x20, 5.6f}, {"\xE2\x86\x90", 0, 0x25, 1}, {"\xE2\x86\x93", 0, 0x28, 1}, {"\xE2\x86\x92", 0, 0x27, 1}, {"Close", 0, 0xFFFF, 1.7f}, {0, 0, 0, 0}},
    };
    float pad = 22, gap = 8;
    float kh = (p.lh - pad * 2 - gap * 4) / 5;
    int id = 1;
    for (int r = 0; r < 5; r++) {
        float total = 0;
        int n = 0;
        for (int k = 0; k < 14 && rows[r][k].label; k++) {
            total += rows[r][k].width;
            n++;
        }
        float unit = (p.lw - pad * 2 - gap * (n - 1)) / total;
        float x = pad, y = pad + r * (kh + gap);
        for (int k = 0; k < n; k++) {
            const KeyDef &kd = rows[r][k];
            float kw = unit * kd.width;
            bool mod = (kd.vk == 0x10 && m_shift) || (kd.vk == 0x11 && m_ctrl) || (kd.vk == 0x12 && m_alt) || (kd.vk == 0x5B && m_win);
            const char *lab = (m_shift && kd.shifted && kd.vk == 0) ? kd.shifted : kd.label;
            if (ui.button(id++, Rect(x, y, kw, kh), lab, false, mod)) {
                if (kd.vk == 0xFFFF) {
                    m_keyboardOpen = false;
                } else if (kd.vk == 0x10) {
                    m_shift = !m_shift;
                } else if (kd.vk == 0x11 || kd.vk == 0x12 || kd.vk == 0x5B) {
                    bool &flag = kd.vk == 0x11 ? m_ctrl : (kd.vk == 0x12 ? m_alt : m_win);
                    flag = !flag;
                    sendKey(kd.vk, flag);
                } else {
                    bool anyMod = m_ctrl || m_alt || m_win;
                    if (kd.vk == 0 && !anyMod) {
                        sendText(m_shift && kd.shifted ? kd.shifted : kd.label);
                    } else {
                        uint16_t vk = kd.vk;
                        if (!vk) {
                            char c = kd.label[0];
                            if (c >= 'a' && c <= 'z') vk = (uint16_t)(c - 'a' + 'A');
                            else if (c >= '0' && c <= '9') vk = (uint16_t)c;
                        }
                        if (vk) {
                            if (m_shift) sendKey(0x10, true);
                            sendKey(vk, true);
                            sendKey(vk, false);
                            if (m_shift) sendKey(0x10, false);
                        }
                    }
                    // modifiers are one-shot
                    if (m_ctrl) sendKey(0x11, false);
                    if (m_alt) sendKey(0x12, false);
                    if (m_win) sendKey(0x5B, false);
                    m_ctrl = m_alt = m_win = false;
                    if (kd.vk == 0) m_shift = false;
                }
            }
            x += kw + gap;
        }
    }
}

void App::drawOverlayUi(int w, int h) {
    Ui &ui = m_ui;
    const Theme &t = m_theme;
    ui.glassPanel(Rect(0, 0, (float)w, (float)h), 34, 0.35f);
    ui.brand(40, 58, 24);
    LunarHostState hs;
    LunarHostStats st;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        hs = m_host;
        st = m_hostStats;
    }
    ui.pill(Rect(w - 290.f, 34, 250, 46), "Playing SteamVR", Color::Hex("#9ff0c0"));

    float time = ui.time();
    RingViz(ui, 190, 300, 112, 46, time, 0.5f);
    ui.moon(190, 300, 98, 0.6f);

    float x = 360;
    ui.text(x, 150, m_net.hostName().c_str(), 38, t.ink, FONT_SEMIBOLD, ALIGN_LEFT, 0.01f, t.accent.withA(0.45f), w - x - 40);
    char buf[128];
    snprintf(buf, sizeof(buf), "%.0f fps  \xC2\xB7  %.0f Mbps  \xC2\xB7  %.0f ms  \xC2\xB7  game %.0f fps", m_displayFps, m_net.mbps(), m_latencyMs, st.vrRenderFps);
    ui.text(x, 196, buf, 20, t.inkSoft, FONT_REGULAR);

    ui.text(x, 260, "Bitrate", 22, t.ink, FONT_SEMIBOLD);
    float br = m_bitrateDrag >= 0 ? m_bitrateDrag : hs.bitrateKbps / 1000.f;
    if (ui.slider(10, Rect(x + 120, 252, 330, 18), &br, 10, 250)) m_bitrateDrag = br;
    if (m_bitrateDrag >= 0 && !m_overlayPtr.down) {
        setSetting("bitrateMbps", std::to_string((int)(m_bitrateDrag / 5 + 0.5f) * 5));
        m_bitrateDrag = -1;
    }
    snprintf(buf, sizeof(buf), "%.0f Mbps", br);
    ui.text(w - 40.f, 260, buf, 20, t.inkSoft, FONT_REGULAR, ALIGN_RIGHT);
    ui.text(x, 330, "Quality", 22, t.ink, FONT_SEMIBOLD);
    const char *q[] = {"Low", "Med", "High", "Ultra"};
    int s = ui.segmented(11, Rect(x + 120, 304, w - x - 160.f, 52), q, 4, hs.quality);
    if (s >= 0) setSetting("quality", std::to_string(s));

    float by = h - 150.f, bw = (w - 80 - 3 * 16) / 4.f;
    if (ui.button(20, Rect(40, by, bw, 66), "Back to game", true)) m_overlayOpen = false;
    if (ui.button(21, Rect(40 + (bw + 16), by, bw, 66), "SteamVR menu")) {
        m_overlayOpen = false;
        m_systemPulse = 8;
    }
    if (ui.button(22, Rect(40 + 2 * (bw + 16), by, bw, 66), "Exit SteamVR")) {
        action(LACTION_STOP_STEAMVR);
        m_overlayOpen = false;
    }
    if (ui.button(23, Rect(40 + 3 * (bw + 16), by, bw, 66), "Disconnect")) {
        m_prefs.autoConnect = false;
        m_net.disconnect();
    }
    ui.text(w / 2.f, h - 44.f, "Hold the menu button to close", 18, t.inkFaint, FONT_REGULAR, ALIGN_CENTER, 0.05f);
    if (m_overlayPtr.valid) {
        ui.circle(m_overlayPtr.x, m_overlayPtr.y, 9, t.accent.withA(0.5f), t.accent.withA(0.5f), Color(0, 0, 0, 0), 0, 8);
        ui.circle(m_overlayPtr.x, m_overlayPtr.y, m_overlayPtr.down ? 4 : 6, Color(1, 1, 1), Color(1, 1, 1));
    }
}

// ------------------------------------------------------------------------------------------------
// Frame
// ------------------------------------------------------------------------------------------------
void App::frame() {
    updateConnection();
    if (!m_xr.waitFrame()) return;
    m_xr.beginFrame();
    m_xr.locate();
    if (!m_anchored && m_xr.headValid) recenter();
    applyTheme();
    handleInput();
    sendTracking();

    std::vector<LunarHaptic> haptics;
    LunarCursor cursor;
    LunarStreamConfig cfg;
    {
        std::lock_guard<std::mutex> lk(m_lock);
        haptics.swap(m_haptics);
        cursor = m_cursor;
        cfg = m_cfg;
    }
    for (auto &h : haptics) m_xr.haptic(h.hand, h.amplitude, h.duration, h.frequency);
    if (m_ui.hapticRequest) {
        m_xr.haptic(m_activeHand, m_ui.hapticRequest == 2 ? 0.45f : 0.12f, m_ui.hapticRequest == 2 ? 0.02f : 0.008f, 0);
        m_ui.hapticRequest = 0;
    }

    bool connected = m_net.state() == StreamClient::CONNECTED;
    bool vrMode = connected && m_haveCfg && cfg.mode == LUNAR_MODE_VR;
    bool desktopMode = connected && m_haveCfg && cfg.mode == LUNAR_MODE_DESKTOP;

    GLuint videoTex = 0;
    uint32_t videoIndex = 0;
    bool haveVideo = m_haveCfg && m_decoder.latest(&videoTex, &videoIndex);
    FrameMeta meta;
    if (haveVideo) {
        std::lock_guard<std::mutex> lk(m_lock);
        meta = m_meta[videoIndex % 64];
    }
    double now = NowSec();
    if (haveVideo && videoIndex != m_lastShownIndex) {
        m_lastShownIndex = videoIndex;
        m_fpsFrames++;
        if (meta.frameIndex == videoIndex && meta.trackingTimeUs) {
            float lat = (NowUs() - meta.trackingTimeUs) / 1000.f;
            if (lat > 0 && lat < 1000) m_latencyMs = m_latencyMs == 0 ? lat : m_latencyMs * 0.9f + lat * 0.1f;
        }
    }
    if (now - m_fpsWindowStart > 1.0) {
        m_displayFps = (float)(m_fpsFrames / (now - m_fpsWindowStart));
        m_fpsFrames = 0;
        m_fpsWindowStart = now;
    }

    layoutPanels();
    if (!m_xr.shouldRender) {
        XrPosef p[2];
        XrFovf f[2];
        for (int i = 0; i < 2; i++) {
            p[i] = m_xr.views[i].pose;
            f[i] = m_xr.views[i].fov;
        }
        m_xr.endFrame(false, p, f, false, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, 1, 1);
        return;
    }

    // UI panels
    if (!vrMode) {
        if (m_main.visible) renderPanel(m_main, &App::drawConnectUi);
        if (m_dock.visible) renderPanel(m_dock, &App::drawDockUi);
        if (m_settings.visible) renderPanel(m_settings, &App::drawSettingsUi);
        if (m_keyboard.visible) renderPanel(m_keyboard, &App::drawKeyboardUi);
    }
    bool quad = vrMode && m_overlayOpen;
    if (quad) {
        m_xr.acquireQuad();
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        m_ui.begin(1000, 625, m_theme, m_overlayPtr, (float)(now - m_start), true);
        drawOverlayUi(1000, 625);
        m_ui.end();
        m_xr.releaseQuad();
    }

    // Eye poses: in VR mode, use the pose the game rendered with so the headset can timewarp correctly.
    XrPosef eyePose[2];
    XrFovf eyeFov[2];
    bool streamPose = vrMode && haveVideo && meta.hasPose && meta.frameIndex == videoIndex && m_viewSent;
    for (int i = 0; i < 2; i++) {
        if (streamPose) {
            eyePose[i] = ToXr(lpMul(meta.pose, m_sentView.eyeInHead[i]));
            const LFov &f = m_sentView.fov[i];
            eyeFov[i] = {f.angleLeft, f.angleRight, f.angleUp, f.angleDown};
        } else {
            eyePose[i] = m_xr.views[i].pose;
            eyeFov[i] = m_xr.views[i].fov;
        }
    }

    float time = (float)(now - m_start);
    for (int e = 0; e < 2; e++) {
        m_xr.acquireEye(e);
        LPose ep = ToL(eyePose[e]);
        LFov lf = {eyeFov[e].angleLeft, eyeFov[e].angleRight, eyeFov[e].angleUp, eyeFov[e].angleDown};
        Mat4 view = Mat4::FromPose(ep).rigidInverse();
        Mat4 proj = Mat4::Projection(lf, 0.05f, 200.f);
        m_scene.beginEye(view, proj, Vec3(ep.position));
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);

        if (streamPose) {
            m_scene.drawStreamEye(videoTex, e);
        } else {
            m_scene.clear(m_theme.bg);
            bool clunkSky = m_theme.clunk;
            m_scene.drawEnvironment(time, m_prefs.ground && !clunkSky, m_prefs.bigMoon && !clunkSky, m_prefs.stars && !clunkSky);
            if (desktopMode && haveVideo) {
                if (!m_theme.clunk) m_scene.drawScreenGlow(m_screenPose, m_meshW, m_meshH);
                float aspect = m_meshW / m_meshH;
                m_scene.drawScreen(videoTex, m_screenMesh, m_screenPose, cursor.visible != 0, cursor.x, cursor.y, aspect);
            }
            // panels, far to near
            if (m_main.visible) m_scene.drawPanel(m_main.rt.tex, m_main.model());
            if (m_settings.visible) m_scene.drawPanel(m_settings.rt.tex, m_settings.model());
            if (m_dock.visible) m_scene.drawPanel(m_dock.rt.tex, m_dock.model());
            if (m_keyboard.visible) m_scene.drawPanel(m_keyboard.rt.tex, m_keyboard.model());
        }
        // controllers and lasers
        for (int i = 0; i < 2; i++) {
            const XrHand &h = m_xr.hands[i];
            if (!h.active) continue;
            if (!vrMode) m_scene.drawOrb(h.grip, h.trigger);
            if (m_laserOn[i]) {
                Vec3 o(h.aim.position);
                m_scene.drawLaser(o, m_laserEnd[i], i == m_activeHand ? 0.9f : 0.3f);
                if (i == m_activeHand) m_scene.drawDot(m_laserEnd[i], 0.03f, 0.8f);
            }
        }
        m_xr.releaseEye(e);
    }
    m_decoder.frameDone();

    XrPosef qp = ToXr(m_overlayPose);
    m_xr.endFrame(true, eyePose, eyeFov, quad, qp, 1.0f, 0.625f);
}
