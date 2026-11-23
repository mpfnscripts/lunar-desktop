// OpenXR session, swapchains and Touch controller input.
#pragma once
#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <android_native_app_glue.h>
#include <jni.h>
#ifndef XR_USE_PLATFORM_ANDROID
#define XR_USE_PLATFORM_ANDROID
#endif
#ifndef XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_GRAPHICS_API_OPENGL_ES
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <vector>
#include "common.h"

struct XrHand {
    bool active = false;
    LPose grip = {}, aim = {};
    LVec3 linVel = {}, angVel = {};
    uint32_t buttons = 0;
    float trigger = 0, squeeze = 0, stickX = 0, stickY = 0;
};

struct XrSwap {
    XrSwapchain handle = XR_NULL_HANDLE;
    int w = 0, h = 0;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    std::vector<GLuint> fbos;
    GLuint depth = 0;  // shared multisampled depth
    uint32_t acquired = 0;
};

class XrApp {
public:
    bool init(android_app *app);
    void shutdown();
    // Returns false when the app should quit.
    bool pollEvents();
    bool sessionRunning() const { return m_running; }
    bool focused() const { return m_state == XR_SESSION_STATE_FOCUSED; }

    // Frame
    bool waitFrame();       // fills predictedTime, shouldRender
    void beginFrame();
    void locate();          // views, head, controllers (call after beginFrame)
    GLuint acquireEye(int eye);   // binds and returns the FBO for this eye
    void releaseEye(int eye);
    GLuint acquireQuad();
    void releaseQuad();
    void endFrame(bool projection, const XrPosef eyePose[2], const XrFovf eyeFov[2], bool quad, const XrPosef &quadPose,
                  float quadW, float quadH);
    void haptic(int hand, float amplitude, float seconds, float frequency);

    // Refresh rate (XR_FB_display_refresh_rate)
    std::vector<float> refreshRates;
    float currentRefresh = 72.f;
    void requestRefresh(float hz);

    XrTime predictedTime = 0;
    bool shouldRender = false;
    XrView views[2];
    LPose head = lpIdentity();
    LVec3 headLinVel = {}, headAngVel = {};
    bool headValid = false;
    LPose eyeInHead[2];
    XrHand hands[2];
    int eyeW = 0, eyeH = 0;
    XrSwap eye[2];
    XrSwap quadSwap;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    bool hasStage = false;

private:
    bool createActions();
    bool createSwap(XrSwap &s, int w, int h, int samples);
    void syncActions();

    android_app *m_app = nullptr;
    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_system = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_stage = XR_NULL_HANDLE, m_viewSpace = XR_NULL_HANDLE;
    XrSessionState m_state = XR_SESSION_STATE_UNKNOWN;
    bool m_running = false;
    EGLConfig m_config = nullptr;
    EGLSurface m_surface = EGL_NO_SURFACE;
    bool m_refreshExt = false;
    PFN_xrEnumerateDisplayRefreshRatesFB m_enumRates = nullptr;
    PFN_xrRequestDisplayRefreshRateFB m_requestRate = nullptr;
    PFN_xrGetDisplayRefreshRateFB m_getRate = nullptr;

    XrActionSet m_set = XR_NULL_HANDLE;
    XrPath m_handPath[2];
    XrAction m_gripPose, m_aimPose, m_trigger, m_squeeze, m_stick, m_stickClick, m_stickTouch, m_btnA, m_btnB, m_touchA, m_touchB,
        m_menu, m_triggerTouch, m_thumbrest, m_haptic;
    XrSpace m_gripSpace[2], m_aimSpace[2];
};

static inline LPose ToL(const XrPosef &p) {
    LPose r;
    r.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    r.position = {p.position.x, p.position.y, p.position.z};
    return r;
}
static inline XrPosef ToXr(const LPose &p) {
    XrPosef r;
    r.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    r.position = {p.position.x, p.position.y, p.position.z};
    return r;
}
