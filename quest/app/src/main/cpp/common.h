// Shared helpers for the Quest client: logging, time, vector/matrix math, Lunar theme colours.
#pragma once
#include <android/log.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <string>
#include "lunar_math.h"
#include "lunar_protocol.h"

#define LOG_TAG "LunarDesktop"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static inline uint64_t NowUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}
static inline double NowSec() { return NowUs() / 1e6; }

// ------------------------------------------------------------------------------------------------
struct Vec2 { float x = 0, y = 0; };
struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() {}
    Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    Vec3(const LVec3 &v) : x(v.x), y(v.y), z(v.z) {}
    Vec3 operator+(const Vec3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float dot(const Vec3 &o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3 &o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    float len() const { return sqrtf(dot(*this)); }
    Vec3 norm() const { float l = len(); return l > 1e-8f ? *this * (1.f / l) : Vec3(0, 0, 0); }
    LVec3 l() const { LVec3 r = {x, y, z}; return r; }
};
struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };

// column-major 4x4 (OpenGL convention)
struct Mat4 {
    float m[16];
    static Mat4 Identity() {
        Mat4 r;
        memset(r.m, 0, sizeof(r.m));
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
        return r;
    }
    Mat4 operator*(const Mat4 &b) const {
        Mat4 r;
        for (int c = 0; c < 4; c++)
            for (int rr = 0; rr < 4; rr++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += m[k * 4 + rr] * b.m[c * 4 + k];
                r.m[c * 4 + rr] = s;
            }
        return r;
    }
    Vec3 transformPoint(const Vec3 &p) const {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
    }
    static Mat4 Translate(const Vec3 &t) {
        Mat4 r = Identity();
        r.m[12] = t.x;
        r.m[13] = t.y;
        r.m[14] = t.z;
        return r;
    }
    static Mat4 Scale(const Vec3 &s) {
        Mat4 r = Identity();
        r.m[0] = s.x;
        r.m[5] = s.y;
        r.m[10] = s.z;
        return r;
    }
    static Mat4 FromQuat(const LQuat &q) {
        float r3[3][3];
        lqToMat3(q, r3);
        Mat4 r = Identity();
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) r.m[j * 4 + i] = r3[i][j];
        return r;
    }
    static Mat4 FromPose(const LPose &p) { return Translate(Vec3(p.position)) * FromQuat(p.orientation); }
    static Mat4 RotateY(float a) {
        Mat4 r = Identity();
        r.m[0] = cosf(a);
        r.m[8] = sinf(a);
        r.m[2] = -sinf(a);
        r.m[10] = cosf(a);
        return r;
    }
    static Mat4 RotateX(float a) {
        Mat4 r = Identity();
        r.m[5] = cosf(a);
        r.m[9] = -sinf(a);
        r.m[6] = sinf(a);
        r.m[10] = cosf(a);
        return r;
    }
    // Asymmetric projection from OpenXR fov
    static Mat4 Projection(const LFov &f, float nearZ, float farZ) {
        float l = tanf(f.angleLeft), r = tanf(f.angleRight), u = tanf(f.angleUp), d = tanf(f.angleDown);
        float w = r - l, h = u - d;
        Mat4 p;
        memset(p.m, 0, sizeof(p.m));
        p.m[0] = 2 / w;
        p.m[8] = (r + l) / w;
        p.m[5] = 2 / h;
        p.m[9] = (u + d) / h;
        p.m[10] = -(farZ + nearZ) / (farZ - nearZ);
        p.m[14] = -2 * farZ * nearZ / (farZ - nearZ);
        p.m[11] = -1;
        return p;
    }
    // inverse of a rigid transform
    Mat4 rigidInverse() const {
        Mat4 r = Identity();
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) r.m[j * 4 + i] = m[i * 4 + j];
        Vec3 t(m[12], m[13], m[14]);
        r.m[12] = -(r.m[0] * t.x + r.m[4] * t.y + r.m[8] * t.z);
        r.m[13] = -(r.m[1] * t.x + r.m[5] * t.y + r.m[9] * t.z);
        r.m[14] = -(r.m[2] * t.x + r.m[6] * t.y + r.m[10] * t.z);
        return r;
    }
};

// ------------------------------------------------------------------------------------------------
// Colours (sRGB, 0..1)
struct Color {
    float r = 1, g = 1, b = 1, a = 1;
    Color() {}
    Color(float R, float G, float B, float A = 1) : r(R), g(G), b(B), a(A) {}
    static Color Hex(const char *h, float a = 1) {
        if (!h || !*h) return Color(1, 1, 1, a);
        if (*h == '#') h++;
        unsigned v = 0;
        for (int i = 0; i < 6 && h[i]; i++) {
            char c = h[i];
            v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
        }
        return Color(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, a);
    }
    Color withA(float A) const { return Color(r, g, b, A); }
    static Color Mix(const Color &a, const Color &b, float t) {  // t of a
        return Color(a.r * t + b.r * (1 - t), a.g * t + b.g * (1 - t), a.b * t + b.b * (1 - t), a.a * t + b.a * (1 - t));
    }
};

struct Theme {
    std::string key = "lunar";
    Color accent, accent2, bg, ink;
    // derived (DESIGN.md §1)
    Color inkSoft, inkFaint, moonLight, moonDark, skyTop, skyBottom;
    bool clunk = false;
    void derive() {
        inkSoft = Color::Mix(ink, bg, 0.68f);
        inkFaint = Color::Mix(ink, bg, 0.44f);
        moonLight = Color::Mix(accent, Color(1, 1, 1), 0.40f);
        moonDark = Color::Mix(accent, Color::Hex("#4c5478"), 0.62f);
        skyTop = Color::Mix(bg, Color(0, 0, 0), 0.72f);
        skyBottom = Color::Mix(bg, Color(0, 0, 0), 0.42f);
    }
};

struct ThemeDef { const char *key, *name, *accent, *accent2, *bg, *ink; };
static const ThemeDef kThemes[] = {
    {"lunar", "Lunar", "#b8c6ff", "#6054be", "#0a0f26", "#eef1fb"},
    {"eclipse", "Eclipse", "#ffc98a", "#a8431f", "#140d0a", "#fbf3ea"},
    {"aurora", "Aurora", "#8ff0d0", "#2b7fa8", "#06171b", "#eafbf6"},
    {"blood", "Blood Moon", "#ff9a8f", "#8a1f3a", "#1a080b", "#fdeeee"},
    {"nebula", "Nebula", "#e3a8ff", "#c2417f", "#150a27", "#f7eefd"},
    {"frost", "Frost", "#d8f1ff", "#4a7fa0", "#0b1721", "#f1f8fc"},
    {"harvest", "Harvest Moon", "#ffd27a", "#b5651d", "#17110a", "#fff6e6"},
    {"sunset", "Sunset", "#ff9e7a", "#b0368f", "#1c0d1a", "#fff0ea"},
    {"ocean", "Ocean", "#7fd0ff", "#1f4fb5", "#061324", "#eaf6ff"},
    {"forest", "Forest", "#a8e6a1", "#2e7d4f", "#0a160f", "#effaf0"},
    {"sakura", "Sakura", "#ffb7d5", "#a04a8a", "#1b0f1a", "#fff0f6"},
    {"cyber", "Cyber", "#7dfcff", "#ff2fb3", "#0a0a14", "#f2fbff"},
};
static const int kThemeCount = sizeof(kThemes) / sizeof(kThemes[0]);

static inline Theme MakeTheme(const std::string &key, const char *cAccent = nullptr, const char *cAccent2 = nullptr,
                              const char *cBg = nullptr, const char *cInk = nullptr) {
    const ThemeDef *d = &kThemes[0];
    for (int i = 0; i < kThemeCount; i++)
        if (key == kThemes[i].key) d = &kThemes[i];
    Theme t;
    t.key = d->key;
    t.accent = Color::Hex(cAccent && *cAccent ? cAccent : d->accent);
    t.accent2 = Color::Hex(cAccent2 && *cAccent2 ? cAccent2 : d->accent2);
    t.bg = Color::Hex(cBg && *cBg ? cBg : d->bg);
    t.ink = Color::Hex(cInk && *cInk ? cInk : d->ink);
    t.derive();
    return t;
}
