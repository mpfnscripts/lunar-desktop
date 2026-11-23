// Lunar Desktop SteamVR driver: presents the Quest (streamed by LunarDesktop.exe) to SteamVR as an
// HMD with two Touch controllers. Tracking and input come from the host through shared memory,
// rendered frames go back through shared D3D11 textures (see compositor.cpp).
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <thread>
#include "openvr_driver.h"
#include "compositor.h"
#include "../common/lunar_ipc.h"
#include "../../shared/lunar_math.h"

#if defined(_WIN32)
#define HMD_DLL_EXPORT extern "C" __declspec(dllexport)
#else
#define HMD_DLL_EXPORT extern "C" __attribute__((visibility("default")))
#endif

void DriverLog(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    va_end(args);
    size_t n = strlen(buf);
    buf[n] = '\n';
    buf[n + 1] = 0;
    if (vr::VRDriverLog()) vr::VRDriverLog()->Log(buf);
}

static LunarIpc *g_ipc = nullptr;
static HANDLE g_ipcMapping = nullptr;

static const float kDefaultFov = 0.87f;  // ~50 degrees half-angle

static bool HostAlive() {
    return g_ipc && g_ipc->hostHeartbeatUs && (lunarNowUs() - g_ipc->hostHeartbeatUs) < 3000000;
}

static vr::HmdMatrix34_t MatFromPose(const LPose &p) {
    vr::HmdMatrix34_t m;
    float r[3][3];
    lqToMat3(p.orientation, r);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m.m[i][j] = r[i][j];
    }
    m.m[0][3] = p.position.x;
    m.m[1][3] = p.position.y;
    m.m[2][3] = p.position.z;
    return m;
}

static vr::DriverPose_t MakePose(const LPose &p, const LVec3 &linVel, const LVec3 &angVelWorld, bool valid) {
    vr::DriverPose_t pose = {};
    pose.poseTimeOffset = 0;
    pose.qWorldFromDriverRotation.w = 1;
    pose.qDriverFromHeadRotation.w = 1;
    pose.qRotation.w = p.orientation.w;
    pose.qRotation.x = p.orientation.x;
    pose.qRotation.y = p.orientation.y;
    pose.qRotation.z = p.orientation.z;
    pose.vecPosition[0] = p.position.x;
    pose.vecPosition[1] = p.position.y;
    pose.vecPosition[2] = p.position.z;
    pose.vecVelocity[0] = linVel.x;
    pose.vecVelocity[1] = linVel.y;
    pose.vecVelocity[2] = linVel.z;
    // OpenVR wants angular velocity in the device's local space; OpenXR gives it in world space.
    LVec3 local = lqRotate(lqConj(p.orientation), angVelWorld);
    pose.vecAngularVelocity[0] = local.x;
    pose.vecAngularVelocity[1] = local.y;
    pose.vecAngularVelocity[2] = local.z;
    pose.poseIsValid = valid;
    pose.result = valid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
    pose.deviceIsConnected = true;
    pose.willDriftInYaw = false;
    pose.shouldApplyHeadModel = false;
    return pose;
}

// ---------------------------------------------------------------------------------------------
// HMD
// ---------------------------------------------------------------------------------------------
class LunarHmd : public vr::ITrackedDeviceServerDriver, public vr::IVRDisplayComponent {
public:
    uint32_t m_id = vr::k_unTrackedDeviceIndexInvalid;
    vr::PropertyContainerHandle_t m_props = 0;
    LunarCompositor m_compositor;
    bool m_compositorOk = false;
    uint32_t m_displayGen = 0;
    vr::DriverPose_t m_pose = {};

    virtual ~LunarHmd() = default;
    LunarHmd() {
        m_pose.qRotation.w = 1;
        m_pose.qWorldFromDriverRotation.w = 1;
        m_pose.qDriverFromHeadRotation.w = 1;
        m_pose.deviceIsConnected = true;
        m_pose.result = vr::TrackingResult_Running_OutOfRange;
    }

    uint32_t EyeW() const { return g_ipc->eyeWidth ? g_ipc->eyeWidth : 1440; }
    uint32_t EyeH() const { return g_ipc->eyeHeight ? g_ipc->eyeHeight : 1584; }
    float Hz() const { return g_ipc->refreshRate > 30.f ? g_ipc->refreshRate : 72.f; }
    LFov Fov(int eye) const {
        LFov f = g_ipc->fov[eye];
        if (f.angleRight - f.angleLeft < 0.1f) {
            f.angleLeft = -kDefaultFov;
            f.angleRight = kDefaultFov;
            f.angleUp = kDefaultFov;
            f.angleDown = -kDefaultFov;
        }
        return f;
    }
    LPose EyeInHead(int eye) const {
        LPose p = g_ipc->eyeInHead[eye];
        if (fabsf(p.position.x) < 0.001f) {
            p = lpIdentity();
            p.position.x = eye == 0 ? -0.0315f : 0.0315f;
        }
        if (p.orientation.w == 0 && p.orientation.x == 0 && p.orientation.y == 0 && p.orientation.z == 0) p.orientation = lqIdentity();
        return p;
    }

    vr::EVRInitError Activate(uint32_t id) override {
        m_id = id;
        m_props = vr::VRProperties()->TrackedDeviceToPropertyContainer(id);
        auto *P = vr::VRProperties();
        std::string model = g_ipc->headsetName[0] ? std::string("Lunar Desktop - ") + g_ipc->headsetName : "Lunar Desktop - Quest";
        P->SetStringProperty(m_props, vr::Prop_TrackingSystemName_String, "oculus");
        P->SetStringProperty(m_props, vr::Prop_ModelNumber_String, model.c_str());
        P->SetStringProperty(m_props, vr::Prop_ManufacturerName_String, "Oculus");
        P->SetStringProperty(m_props, vr::Prop_RenderModelName_String, "generic_hmd");
        P->SetStringProperty(m_props, vr::Prop_RegisteredDeviceType_String, "oculus/LUNAR-HMD");
        P->SetStringProperty(m_props, vr::Prop_DriverVersion_String, "1.0.0");
        P->SetUint64Property(m_props, vr::Prop_CurrentUniverseId_Uint64, 47800);
        P->SetFloatProperty(m_props, vr::Prop_UserIpdMeters_Float, EyeInHead(1).position.x - EyeInHead(0).position.x);
        P->SetFloatProperty(m_props, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.f);
        P->SetFloatProperty(m_props, vr::Prop_DisplayFrequency_Float, Hz());
        float toPhotons = g_ipc->secondsToPhotons > 0.f ? g_ipc->secondsToPhotons : 0.035f;
        P->SetFloatProperty(m_props, vr::Prop_SecondsFromVsyncToPhotons_Float, toPhotons);
        P->SetBoolProperty(m_props, vr::Prop_IsOnDesktop_Bool, false);
        P->SetBoolProperty(m_props, vr::Prop_DisplayDebugMode_Bool, false);
        P->SetBoolProperty(m_props, vr::Prop_HasDriverDirectModeComponent_Bool, true);
        P->SetBoolProperty(m_props, vr::Prop_DriverDirectModeSendsVsyncEvents_Bool, true);
        P->SetBoolProperty(m_props, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
        P->SetStringProperty(m_props, vr::Prop_NamedIconPathDeviceOff_String, "{lunar}/icons/hmd_off.png");
        P->SetStringProperty(m_props, vr::Prop_NamedIconPathDeviceSearching_String, "{lunar}/icons/hmd_searching.png");
        P->SetStringProperty(m_props, vr::Prop_NamedIconPathDeviceSearchingAlert_String, "{lunar}/icons/hmd_searching.png");
        P->SetStringProperty(m_props, vr::Prop_NamedIconPathDeviceReady_String, "{lunar}/icons/hmd_ready.png");
        P->SetStringProperty(m_props, vr::Prop_NamedIconPathDeviceReadyAlert_String, "{lunar}/icons/hmd_ready.png");
        P->SetStringProperty(m_props, vr::Prop_NamedIconPathDeviceStandby_String, "{lunar}/icons/hmd_ready.png");

        m_compositorOk = m_compositor.Init(g_ipc);
        if (m_compositorOk) P->SetUint64Property(m_props, vr::Prop_GraphicsAdapterLuid_Uint64, m_compositor.AdapterLuid());
        else DriverLog("Lunar: compositor init failed, no video will be streamed");

        ApplyDisplayConfig();
        return vr::VRInitError_None;
    }

    void ApplyDisplayConfig() {
        m_displayGen = g_ipc->displayGeneration;
        auto *H = vr::VRServerDriverHost();
        vr::HmdRect2_t l, r;
        LFov fl = Fov(0), fr = Fov(1);
        // OpenVR raw projection: left/right are tangents, top is negative (y down)
        l.vTopLeft.v[0] = tanf(fl.angleLeft);
        l.vBottomRight.v[0] = tanf(fl.angleRight);
        l.vTopLeft.v[1] = -tanf(fl.angleUp);
        l.vBottomRight.v[1] = -tanf(fl.angleDown);
        r.vTopLeft.v[0] = tanf(fr.angleLeft);
        r.vBottomRight.v[0] = tanf(fr.angleRight);
        r.vTopLeft.v[1] = -tanf(fr.angleUp);
        r.vBottomRight.v[1] = -tanf(fr.angleDown);
        if (m_id != vr::k_unTrackedDeviceIndexInvalid) {
            H->SetDisplayProjectionRaw(m_id, l, r);
            H->SetDisplayEyeToHead(m_id, MatFromPose(EyeInHead(0)), MatFromPose(EyeInHead(1)));
            H->SetRecommendedRenderTargetSize(m_id, EyeW(), EyeH());
            vr::VRProperties()->SetFloatProperty(m_props, vr::Prop_DisplayFrequency_Float, Hz());
            vr::VRProperties()->SetFloatProperty(m_props, vr::Prop_UserIpdMeters_Float, EyeInHead(1).position.x - EyeInHead(0).position.x);
        }
    }

    void Deactivate() override {
        m_compositor.Shutdown();
        m_id = vr::k_unTrackedDeviceIndexInvalid;
    }
    void EnterStandby() override {}
    void *GetComponent(const char *name) override {
        if (!strcmp(name, vr::IVRDisplayComponent_Version)) return (vr::IVRDisplayComponent *)this;
        if (!strcmp(name, vr::IVRDriverDirectModeComponent_Version) && m_compositorOk) return (vr::IVRDriverDirectModeComponent *)&m_compositor;
        return nullptr;
    }
    void DebugRequest(const char *, char *resp, uint32_t n) override {
        if (n) resp[0] = 0;
    }
    vr::DriverPose_t GetPose() override { return m_pose; }

    // IVRDisplayComponent
    void GetWindowBounds(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) override {
        *x = 0;
        *y = 0;
        *w = EyeW() * 2;
        *h = EyeH();
    }
    bool IsDisplayOnDesktop() override { return false; }
    bool IsDisplayRealDisplay() override { return false; }
    void GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h) override {
        *w = EyeW();
        *h = EyeH();
    }
    void GetEyeOutputViewport(vr::EVREye eye, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) override {
        *x = eye == vr::Eye_Left ? 0 : EyeW();
        *y = 0;
        *w = EyeW();
        *h = EyeH();
    }
    void GetProjectionRaw(vr::EVREye eye, float *left, float *right, float *top, float *bottom) override {
        LFov f = Fov(eye == vr::Eye_Left ? 0 : 1);
        *left = tanf(f.angleLeft);
        *right = tanf(f.angleRight);
        *top = -tanf(f.angleUp);
        *bottom = -tanf(f.angleDown);
    }
    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye, float u, float v) override {
        vr::DistortionCoordinates_t c;
        c.rfRed[0] = c.rfGreen[0] = c.rfBlue[0] = u;
        c.rfRed[1] = c.rfGreen[1] = c.rfBlue[1] = v;
        return c;
    }
    bool ComputeInverseDistortion(vr::HmdVector2_t *, vr::EVREye, uint32_t, float, float) override { return false; }

    void Update(const LunarTracking &t, bool valid) {
        m_pose = MakePose(t.head, t.headLinearVelocity, t.headAngularVelocity, valid);
        if (m_id != vr::k_unTrackedDeviceIndexInvalid)
            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_id, m_pose, sizeof(m_pose));
    }
};

// ---------------------------------------------------------------------------------------------
// Touch controller
// ---------------------------------------------------------------------------------------------
class LunarTouch : public vr::ITrackedDeviceServerDriver {
public:
    int m_hand;  // 0 left, 1 right
    uint32_t m_id = vr::k_unTrackedDeviceIndexInvalid;
    vr::PropertyContainerHandle_t m_props = 0;
    vr::DriverPose_t m_pose = {};
    enum {
        C_SYSTEM, C_APPMENU, C_A_CLICK, C_A_TOUCH, C_B_CLICK, C_B_TOUCH,
        C_STICK_CLICK, C_STICK_TOUCH, C_TRIGGER_CLICK, C_TRIGGER_TOUCH, C_GRIP_CLICK, C_GRIP_TOUCH, C_THUMBREST,
        C_BOOL_COUNT
    };
    enum { S_TRIGGER, S_GRIP, S_STICK_X, S_STICK_Y, S_GRIP_FORCE, S_COUNT };
    vr::VRInputComponentHandle_t m_bool[C_BOOL_COUNT] = {};
    vr::VRInputComponentHandle_t m_scalar[S_COUNT] = {};
    vr::VRInputComponentHandle_t m_haptic = 0;

    virtual ~LunarTouch() = default;
    explicit LunarTouch(int hand) : m_hand(hand) {
        m_pose.qRotation.w = 1;
        m_pose.qWorldFromDriverRotation.w = 1;
        m_pose.qDriverFromHeadRotation.w = 1;
        m_pose.deviceIsConnected = true;
        m_pose.result = vr::TrackingResult_Running_OutOfRange;
    }
    const char *Serial() const { return m_hand == 0 ? "LUNAR-TOUCH-LEFT" : "LUNAR-TOUCH-RIGHT"; }

    vr::EVRInitError Activate(uint32_t id) override {
        m_id = id;
        m_props = vr::VRProperties()->TrackedDeviceToPropertyContainer(id);
        auto *P = vr::VRProperties();
        bool left = m_hand == 0;
        P->SetStringProperty(m_props, vr::Prop_TrackingSystemName_String, "oculus");
        P->SetStringProperty(m_props, vr::Prop_ManufacturerName_String, "Oculus");
        P->SetStringProperty(m_props, vr::Prop_ModelNumber_String, left ? "Oculus Quest2 (Left Controller)" : "Oculus Quest2 (Right Controller)");
        P->SetStringProperty(m_props, vr::Prop_RenderModelName_String, left ? "oculus_quest2_controller_left" : "oculus_quest2_controller_right");
        P->SetStringProperty(m_props, vr::Prop_RegisteredDeviceType_String, left ? "oculus/LUNAR-TOUCH-LEFT" : "oculus/LUNAR-TOUCH-RIGHT");
        P->SetStringProperty(m_props, vr::Prop_ControllerType_String, "oculus_touch");
        P->SetStringProperty(m_props, vr::Prop_InputProfilePath_String, "{oculus}/input/touch_profile.json");
        P->SetInt32Property(m_props, vr::Prop_ControllerRoleHint_Int32, left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        P->SetInt32Property(m_props, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_Controller);
        P->SetUint64Property(m_props, vr::Prop_CurrentUniverseId_Uint64, 47800);
        P->SetStringProperty(m_props, vr::Prop_DriverVersion_String, "1.0.0");
        P->SetBoolProperty(m_props, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
        P->SetStringProperty(m_props, vr::Prop_NamedIconPathDeviceOff_String, left ? "{lunar}/icons/left_off.png" : "{lunar}/icons/right_off.png");
        P->SetStringProperty(m_props, vr::Prop_NamedIconPathDeviceSearching_String, left ? "{lunar}/icons/left_off.png" : "{lunar}/icons/right_off.png");
        P->SetStringProperty(m_props, vr::Prop_NamedIconPathDeviceReady_String, left ? "{lunar}/icons/left_ready.png" : "{lunar}/icons/right_ready.png");

        auto *I = vr::VRDriverInput();
        I->CreateBooleanComponent(m_props, "/input/system/click", &m_bool[C_SYSTEM]);
        I->CreateBooleanComponent(m_props, "/input/application_menu/click", &m_bool[C_APPMENU]);
        I->CreateBooleanComponent(m_props, left ? "/input/x/click" : "/input/a/click", &m_bool[C_A_CLICK]);
        I->CreateBooleanComponent(m_props, left ? "/input/x/touch" : "/input/a/touch", &m_bool[C_A_TOUCH]);
        I->CreateBooleanComponent(m_props, left ? "/input/y/click" : "/input/b/click", &m_bool[C_B_CLICK]);
        I->CreateBooleanComponent(m_props, left ? "/input/y/touch" : "/input/b/touch", &m_bool[C_B_TOUCH]);
        I->CreateBooleanComponent(m_props, "/input/joystick/click", &m_bool[C_STICK_CLICK]);
        I->CreateBooleanComponent(m_props, "/input/joystick/touch", &m_bool[C_STICK_TOUCH]);
        I->CreateBooleanComponent(m_props, "/input/trigger/click", &m_bool[C_TRIGGER_CLICK]);
        I->CreateBooleanComponent(m_props, "/input/trigger/touch", &m_bool[C_TRIGGER_TOUCH]);
        I->CreateBooleanComponent(m_props, "/input/grip/click", &m_bool[C_GRIP_CLICK]);
        I->CreateBooleanComponent(m_props, "/input/grip/touch", &m_bool[C_GRIP_TOUCH]);
        I->CreateBooleanComponent(m_props, "/input/thumbrest/touch", &m_bool[C_THUMBREST]);
        I->CreateScalarComponent(m_props, "/input/trigger/value", &m_scalar[S_TRIGGER], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
        I->CreateScalarComponent(m_props, "/input/grip/value", &m_scalar[S_GRIP], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
        I->CreateScalarComponent(m_props, "/input/grip/force", &m_scalar[S_GRIP_FORCE], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
        I->CreateScalarComponent(m_props, "/input/joystick/x", &m_scalar[S_STICK_X], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
        I->CreateScalarComponent(m_props, "/input/joystick/y", &m_scalar[S_STICK_Y], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
        I->CreateHapticComponent(m_props, "/output/haptic", &m_haptic);
        return vr::VRInitError_None;
    }
    void Deactivate() override { m_id = vr::k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {}
    void *GetComponent(const char *) override { return nullptr; }
    void DebugRequest(const char *, char *resp, uint32_t n) override {
        if (n) resp[0] = 0;
    }
    vr::DriverPose_t GetPose() override { return m_pose; }

    void Update(const LunarController &c, bool inputEnabled, bool hostValid, bool dashboardPulse) {
        if (m_id == vr::k_unTrackedDeviceIndexInvalid) return;
        bool valid = hostValid && c.active;
        LPose p = g_ipc->controllerPoseMode == 1 ? c.aimPose : c.gripPose;
        // optional user offset (controller space)
        const float *op = (const float *)g_ipc->controllerOffsetPos;
        const float *orr = (const float *)g_ipc->controllerOffsetRotDeg;
        if (op[0] || op[1] || op[2] || orr[0] || orr[1] || orr[2]) {
            LPose off;
            float mirror = m_hand == 0 ? -1.f : 1.f;  // offsets are given for the right hand, mirrored for the left
            off.position.x = op[0] * mirror;
            off.position.y = op[1];
            off.position.z = op[2];
            off.orientation = lqFromEulerDeg(orr[0], orr[1] * mirror, orr[2] * mirror);
            p = lpMul(p, off);
        }
        m_pose = MakePose(p, c.linearVelocity, c.angularVelocity, valid);
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_id, m_pose, sizeof(m_pose));

        auto *I = vr::VRDriverInput();
        uint32_t b = inputEnabled ? c.buttons : 0;
        float trig = inputEnabled ? c.trigger : 0.f, grip = inputEnabled ? c.grip : 0.f;
        float sx = inputEnabled ? c.stickX : 0.f, sy = inputEnabled ? c.stickY : 0.f;
        I->UpdateBooleanComponent(m_bool[C_SYSTEM], dashboardPulse || (m_hand == 1 && (b & LBTN_SYSTEM)), 0);
        I->UpdateBooleanComponent(m_bool[C_APPMENU], (b & LBTN_MENU) != 0, 0);
        I->UpdateBooleanComponent(m_bool[C_A_CLICK], (b & LBTN_A) != 0, 0);
        I->UpdateBooleanComponent(m_bool[C_A_TOUCH], (b & LTOUCH_A) != 0, 0);
        I->UpdateBooleanComponent(m_bool[C_B_CLICK], (b & LBTN_B) != 0, 0);
        I->UpdateBooleanComponent(m_bool[C_B_TOUCH], (b & LTOUCH_B) != 0, 0);
        I->UpdateBooleanComponent(m_bool[C_STICK_CLICK], (b & LBTN_STICK) != 0, 0);
        I->UpdateBooleanComponent(m_bool[C_STICK_TOUCH], (b & LTOUCH_STICK) != 0, 0);
        I->UpdateBooleanComponent(m_bool[C_TRIGGER_CLICK], trig > 0.7f, 0);
        I->UpdateBooleanComponent(m_bool[C_TRIGGER_TOUCH], (b & LTOUCH_TRIGGER) != 0 || trig > 0.05f, 0);
        I->UpdateBooleanComponent(m_bool[C_GRIP_CLICK], grip > 0.7f, 0);
        I->UpdateBooleanComponent(m_bool[C_GRIP_TOUCH], grip > 0.05f, 0);
        I->UpdateBooleanComponent(m_bool[C_THUMBREST], (b & LTOUCH_THUMBREST) != 0, 0);
        I->UpdateScalarComponent(m_scalar[S_TRIGGER], trig, 0);
        I->UpdateScalarComponent(m_scalar[S_GRIP], grip, 0);
        I->UpdateScalarComponent(m_scalar[S_GRIP_FORCE], grip > 0.95f ? (grip - 0.95f) * 20.f : 0.f, 0);
        I->UpdateScalarComponent(m_scalar[S_STICK_X], sx, 0);
        I->UpdateScalarComponent(m_scalar[S_STICK_Y], sy, 0);
    }
};

// ---------------------------------------------------------------------------------------------
// Provider
// ---------------------------------------------------------------------------------------------
class LunarProvider : public vr::IServerTrackedDeviceProvider {
public:
    LunarHmd *m_hmd = nullptr;
    LunarTouch *m_ctrl[2] = {};
    std::thread m_thread;
    std::atomic<bool> m_run{false};

    vr::EVRInitError Init(vr::IVRDriverContext *ctx) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
        g_ipc = lunarIpcOpen(&g_ipcMapping);
        if (!g_ipc) {
            DriverLog("Lunar: could not open shared memory");
            return vr::VRInitError_Init_InterfaceNotFound;
        }
        DriverLog("Lunar Desktop driver starting (host %s)", HostAlive() ? "running" : "not running yet");
        g_ipc->driverHeartbeatUs = lunarNowUs();

        m_hmd = new LunarHmd();
        vr::VRServerDriverHost()->TrackedDeviceAdded("LUNAR-HMD", vr::TrackedDeviceClass_HMD, m_hmd);
        for (int i = 0; i < 2; i++) {
            m_ctrl[i] = new LunarTouch(i);
            vr::VRServerDriverHost()->TrackedDeviceAdded(m_ctrl[i]->Serial(), vr::TrackedDeviceClass_Controller, m_ctrl[i]);
        }
        m_run = true;
        m_thread = std::thread([this] { TrackingLoop(); });
        return vr::VRInitError_None;
    }

    void TrackingLoop() {
        timeBeginPeriod(1);
        uint32_t lastSeq = 0;
        int64_t lastPush = 0;
        int dashboardPulse = 0;
        uint32_t prevLeftButtons = 0;
        while (m_run) {
            int64_t now = lunarNowUs();
            g_ipc->driverHeartbeatUs = now;
            LunarTracking t;
            uint32_t seq = 0;
            bool got = lunarSeqRead(&g_ipc->trackingSeq, &g_ipc->tracking, t, &seq);
            bool fresh = got && seq != lastSeq;
            bool hostOk = HostAlive() && g_ipc->headsetConnected && (now - g_ipc->trackingReceivedUs) < 500000;
            if (fresh || now - lastPush > 20000) {
                if (got) {
                    lastSeq = seq;
                    // Long press of the left system/menu combo is handled on the headset; here the special
                    // "dashboard" flag (system bit on the left hand) gives a short system click.
                    uint32_t lb = t.controllers[0].buttons;
                    if ((lb & LBTN_SYSTEM) && !(prevLeftButtons & LBTN_SYSTEM)) dashboardPulse = 6;
                    prevLeftButtons = lb;
                    m_hmd->Update(t, hostOk);
                    for (int i = 0; i < 2; i++)
                        m_ctrl[i]->Update(t.controllers[i], t.inputToHost != 0, hostOk, i == 0 && dashboardPulse > 0);
                    if (dashboardPulse > 0) dashboardPulse--;
                }
                lastPush = now;
            }
            Sleep(1);
        }
        timeEndPeriod(1);
    }

    void Cleanup() override {
        m_run = false;
        if (m_thread.joinable()) m_thread.join();
        delete m_hmd;
        delete m_ctrl[0];
        delete m_ctrl[1];
        m_hmd = nullptr;
        m_ctrl[0] = m_ctrl[1] = nullptr;
        if (g_ipc) UnmapViewOfFile(g_ipc);
        if (g_ipcMapping) CloseHandle(g_ipcMapping);
        g_ipc = nullptr;
        g_ipcMapping = nullptr;
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }

    const char *const *GetInterfaceVersions() override { return vr::k_InterfaceVersions; }

    void RunFrame() override {
        if (m_hmd && g_ipc->displayGeneration != m_hmd->m_displayGen) m_hmd->ApplyDisplayConfig();
        vr::VREvent_t ev;
        while (vr::VRServerDriverHost()->PollNextEvent(&ev, sizeof(ev))) {
            if (ev.eventType == vr::VREvent_Input_HapticVibration) {
                const auto &h = ev.data.hapticVibration;
                for (int i = 0; i < 2; i++) {
                    if (m_ctrl[i] && m_ctrl[i]->m_props == h.containerHandle) {
                        uint32_t w = g_ipc->hapticWrite;
                        LunarHaptic &dst = g_ipc->haptics[w % LUNAR_IPC_HAPTIC_RING];
                        dst.hand = (uint8_t)i;
                        dst.amplitude = h.fAmplitude;
                        dst.frequency = h.fFrequency;
                        dst.duration = h.fDurationSeconds;
                        MemoryBarrier();
                        g_ipc->hapticWrite = w + 1;
                    }
                }
            }
        }
    }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}
};

static LunarProvider g_provider;

HMD_DLL_EXPORT void *HmdDriverFactory(const char *pInterfaceName, int *pReturnCode) {
    if (0 == strcmp(vr::IServerTrackedDeviceProvider_Version, pInterfaceName)) return &g_provider;
    if (pReturnCode) *pReturnCode = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
