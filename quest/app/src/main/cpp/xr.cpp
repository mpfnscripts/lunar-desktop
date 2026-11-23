#include "xr.h"
#include "gfx.h"

#define XR_CHECK(x)                                                    \
    do {                                                               \
        XrResult _r = (x);                                             \
        if (XR_FAILED(_r)) {                                           \
            LOGE("%s failed: %d (%s:%d)", #x, _r, __FILE__, __LINE__); \
            return false;                                              \
        }                                                              \
    } while (0)

static bool HasExt(const std::vector<XrExtensionProperties> &exts, const char *name) {
    for (auto &e : exts)
        if (!strcmp(e.extensionName, name)) return true;
    return false;
}

bool XrApp::init(android_app *app) {
    m_app = app;
    PFN_xrInitializeLoaderKHR initLoader = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&initLoader);
    if (initLoader) {
        XrLoaderInitInfoAndroidKHR li = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        li.applicationVM = app->activity->vm;
        li.applicationContext = app->activity->clazz;
        initLoader((XrLoaderInitInfoBaseHeaderKHR *)&li);
    }

    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
    std::vector<const char *> enable = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
    m_refreshExt = HasExt(exts, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (m_refreshExt) enable.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);

    XrInstanceCreateInfoAndroidKHR ai = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    ai.applicationVM = app->activity->vm;
    ai.applicationActivity = app->activity->clazz;
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &ai;
    strcpy(ci.applicationInfo.applicationName, "Lunar Desktop");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "Lunar");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ci.enabledExtensionCount = (uint32_t)enable.size();
    ci.enabledExtensionNames = enable.data();
    XR_CHECK(xrCreateInstance(&ci, &m_instance));

    XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(m_instance, &sgi, &m_system));

    PFN_xrGetOpenGLESGraphicsRequirementsKHR getReq = nullptr;
    xrGetInstanceProcAddr(m_instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getReq);
    XrGraphicsRequirementsOpenGLESKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (getReq) getReq(m_instance, m_system, &req);

    // EGL context
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, nullptr, nullptr);
    EGLint cfgAttr[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0,
                        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLint nc = 0;
    eglChooseConfig(display, cfgAttr, &m_config, 1, &nc);
    EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context = eglCreateContext(display, m_config, EGL_NO_CONTEXT, ctxAttr);
    EGLint pbAttr[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    m_surface = eglCreatePbufferSurface(display, m_config, pbAttr);
    if (!eglMakeCurrent(display, m_surface, m_surface, context)) {
        LOGE("eglMakeCurrent failed");
        return false;
    }
    LoadGlExtensions();

    XrGraphicsBindingOpenGLESAndroidKHR gb = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    gb.display = display;
    gb.config = m_config;
    gb.context = context;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = m_system;
    XR_CHECK(xrCreateSession(m_instance, &sci, &m_session));

    // Reference spaces: STAGE (floor level, matches the Quest boundary) if available
    uint32_t sc = 0;
    xrEnumerateReferenceSpaces(m_session, 0, &sc, nullptr);
    std::vector<XrReferenceSpaceType> spaces(sc);
    xrEnumerateReferenceSpaces(m_session, sc, &sc, spaces.data());
    for (auto s : spaces)
        if (s == XR_REFERENCE_SPACE_TYPE_STAGE) hasStage = true;
    XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = hasStage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
    XR_CHECK(xrCreateReferenceSpace(m_session, &rs, &m_stage));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(m_session, &rs, &m_viewSpace));

    // Swapchains
    uint32_t vc = 0;
    xrEnumerateViewConfigurationViews(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &vc, nullptr);
    std::vector<XrViewConfigurationView> vcv(vc, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, vc, &vc, vcv.data());
    eyeW = (int)vcv[0].recommendedImageRectWidth;
    eyeH = (int)vcv[0].recommendedImageRectHeight;
    for (int i = 0; i < 2; i++)
        if (!createSwap(eye[i], eyeW, eyeH, 4)) return false;
    if (!createSwap(quadSwap, 1600, 1000, 1)) return false;
    LOGI("eye buffers %dx%d, stage %d", eyeW, eyeH, hasStage);

    if (m_refreshExt) {
        xrGetInstanceProcAddr(m_instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&m_enumRates);
        xrGetInstanceProcAddr(m_instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&m_requestRate);
        xrGetInstanceProcAddr(m_instance, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction *)&m_getRate);
        uint32_t rc = 0;
        if (m_enumRates && m_enumRates(m_session, 0, &rc, nullptr) == XR_SUCCESS && rc) {
            refreshRates.resize(rc);
            m_enumRates(m_session, rc, &rc, refreshRates.data());
        }
        if (m_getRate) m_getRate(m_session, &currentRefresh);
    }
    if (refreshRates.empty()) refreshRates.push_back(72.f);

    for (int i = 0; i < 2; i++) {
        views[i] = {XR_TYPE_VIEW};
        eyeInHead[i] = lpIdentity();
        eyeInHead[i].position.x = i == 0 ? -0.032f : 0.032f;
    }
    return createActions();
}

bool XrApp::createSwap(XrSwap &s, int w, int h, int samples) {
    uint32_t fc = 0;
    xrEnumerateSwapchainFormats(m_session, 0, &fc, nullptr);
    std::vector<int64_t> formats(fc);
    xrEnumerateSwapchainFormats(m_session, fc, &fc, formats.data());
    int64_t fmt = formats.empty() ? GL_RGBA8 : formats[0];
    for (auto f : formats)
        if (f == GL_SRGB8_ALPHA8) fmt = f;
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(m_session, &ci, &s.handle));
    s.w = w;
    s.h = h;
    uint32_t ic = 0;
    xrEnumerateSwapchainImages(s.handle, 0, &ic, nullptr);
    s.images.assign(ic, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(s.handle, ic, &ic, (XrSwapchainImageBaseHeader *)s.images.data());
    bool msaa = samples > 1 && glFramebufferTexture2DMultisampleEXT_ && glRenderbufferStorageMultisampleEXT_;
    glGenRenderbuffers(1, &s.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, s.depth);
    if (msaa) glRenderbufferStorageMultisampleEXT_(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h);
    else glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    s.fbos.resize(ic);
    for (uint32_t i = 0; i < ic; i++) {
        glGenFramebuffers(1, &s.fbos[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, s.fbos[i]);
        if (msaa) glFramebufferTexture2DMultisampleEXT_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.images[i].image, 0, samples);
        else glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.images[i].image, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s.depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) LOGE("swapchain fbo incomplete");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

static XrPath Path(XrInstance inst, const char *s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(inst, s, &p);
    return p;
}

bool XrApp::createActions() {
    XrActionSetCreateInfo asci = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(asci.actionSetName, "lunar");
    strcpy(asci.localizedActionSetName, "Lunar Desktop");
    XR_CHECK(xrCreateActionSet(m_instance, &asci, &m_set));
    m_handPath[0] = Path(m_instance, "/user/hand/left");
    m_handPath[1] = Path(m_instance, "/user/hand/right");

    auto mk = [&](XrAction *a, const char *name, XrActionType type) {
        XrActionCreateInfo c = {XR_TYPE_ACTION_CREATE_INFO};
        c.actionType = type;
        strcpy(c.actionName, name);
        strcpy(c.localizedActionName, name);
        c.countSubactionPaths = 2;
        c.subactionPaths = m_handPath;
        xrCreateAction(m_set, &c, a);
    };
    mk(&m_gripPose, "grip_pose", XR_ACTION_TYPE_POSE_INPUT);
    mk(&m_aimPose, "aim_pose", XR_ACTION_TYPE_POSE_INPUT);
    mk(&m_trigger, "trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    mk(&m_squeeze, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT);
    mk(&m_stick, "thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    mk(&m_stickClick, "thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(&m_stickTouch, "thumbstick_touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(&m_btnA, "button_a", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(&m_btnB, "button_b", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(&m_touchA, "touch_a", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(&m_touchB, "touch_b", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(&m_menu, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(&m_triggerTouch, "trigger_touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(&m_thumbrest, "thumbrest_touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(&m_haptic, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT);

    std::vector<XrActionSuggestedBinding> b;
    auto bind = [&](XrAction a, const char *p) { b.push_back({a, Path(m_instance, p)}); };
    for (const char *h : {"/user/hand/left", "/user/hand/right"}) {
        std::string s = h;
        bind(m_gripPose, (s + "/input/grip/pose").c_str());
        bind(m_aimPose, (s + "/input/aim/pose").c_str());
        bind(m_trigger, (s + "/input/trigger/value").c_str());
        bind(m_squeeze, (s + "/input/squeeze/value").c_str());
        bind(m_stick, (s + "/input/thumbstick").c_str());
        bind(m_stickClick, (s + "/input/thumbstick/click").c_str());
        bind(m_stickTouch, (s + "/input/thumbstick/touch").c_str());
        bind(m_triggerTouch, (s + "/input/trigger/touch").c_str());
        bind(m_thumbrest, (s + "/input/thumbrest/touch").c_str());
        bind(m_haptic, (s + "/output/haptic").c_str());
    }
    bind(m_btnA, "/user/hand/left/input/x/click");
    bind(m_btnB, "/user/hand/left/input/y/click");
    bind(m_touchA, "/user/hand/left/input/x/touch");
    bind(m_touchB, "/user/hand/left/input/y/touch");
    bind(m_btnA, "/user/hand/right/input/a/click");
    bind(m_btnB, "/user/hand/right/input/b/click");
    bind(m_touchA, "/user/hand/right/input/a/touch");
    bind(m_touchB, "/user/hand/right/input/b/touch");
    bind(m_menu, "/user/hand/left/input/menu/click");
    XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = Path(m_instance, "/interaction_profiles/oculus/touch_controller");
    sb.countSuggestedBindings = (uint32_t)b.size();
    sb.suggestedBindings = b.data();
    XR_CHECK(xrSuggestInteractionProfileBindings(m_instance, &sb));

    for (int i = 0; i < 2; i++) {
        XrActionSpaceCreateInfo sc = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sc.subactionPath = m_handPath[i];
        sc.poseInActionSpace.orientation.w = 1;
        sc.action = m_gripPose;
        xrCreateActionSpace(m_session, &sc, &m_gripSpace[i]);
        sc.action = m_aimPose;
        xrCreateActionSpace(m_session, &sc, &m_aimSpace[i]);
    }
    XrSessionActionSetsAttachInfo att = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    att.countActionSets = 1;
    att.actionSets = &m_set;
    XR_CHECK(xrAttachSessionActionSets(m_session, &att));
    return true;
}

void XrApp::shutdown() {
    if (m_session) xrDestroySession(m_session);
    if (m_instance) xrDestroyInstance(m_instance);
    m_session = XR_NULL_HANDLE;
    m_instance = XR_NULL_HANDLE;
    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (m_surface != EGL_NO_SURFACE) eglDestroySurface(display, m_surface);
        eglTerminate(display);
    }
}

bool XrApp::pollEvents() {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(m_instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto *e = (XrEventDataSessionStateChanged *)&ev;
                m_state = e->state;
                LOGI("session state %d", m_state);
                if (m_state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XR_SUCCEEDED(xrBeginSession(m_session, &bi))) m_running = true;
                } else if (m_state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(m_session);
                    m_running = false;
                } else if (m_state == XR_SESSION_STATE_EXITING || m_state == XR_SESSION_STATE_LOSS_PENDING) {
                    return false;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                return false;
            case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
                auto *e = (XrEventDataDisplayRefreshRateChangedFB *)&ev;
                currentRefresh = e->toDisplayRefreshRate;
                LOGI("refresh rate now %.0f", currentRefresh);
                break;
            }
            default:
                break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return true;
}

void XrApp::requestRefresh(float hz) {
    if (m_requestRate && hz > 0) m_requestRate(m_session, hz);
}

bool XrApp::waitFrame() {
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
    if (XR_FAILED(xrWaitFrame(m_session, &wi, &fs))) return false;
    predictedTime = fs.predictedDisplayTime;
    shouldRender = fs.shouldRender;
    return true;
}

void XrApp::beginFrame() {
    XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
    xrBeginFrame(m_session, &bi);
}

void XrApp::syncActions() {
    XrActiveActionSet a = {m_set, XR_NULL_PATH};
    XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &a;
    xrSyncActions(m_session, &si);
}

void XrApp::locate() {
    XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = predictedTime;
    vli.space = m_stage;
    XrViewState vs = {XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    views[0] = views[1] = {XR_TYPE_VIEW};
    xrLocateViews(m_session, &vli, &vs, 2, &n, views);

    // eye poses relative to the head (gives IPD)
    XrView hv[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    vli.space = m_viewSpace;
    XrViewState vs2 = {XR_TYPE_VIEW_STATE};
    if (XR_SUCCEEDED(xrLocateViews(m_session, &vli, &vs2, 2, &n, hv)) && (vs2.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
        eyeInHead[0] = ToL(hv[0].pose);
        eyeInHead[1] = ToL(hv[1].pose);
    }

    XrSpaceVelocity vel = {XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION, &vel};
    if (XR_SUCCEEDED(xrLocateSpace(m_viewSpace, m_stage, predictedTime, &loc))) {
        headValid = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        head = ToL(loc.pose);
        headLinVel = (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) ? LVec3{vel.linearVelocity.x, vel.linearVelocity.y, vel.linearVelocity.z} : LVec3{0, 0, 0};
        headAngVel = (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) ? LVec3{vel.angularVelocity.x, vel.angularVelocity.y, vel.angularVelocity.z} : LVec3{0, 0, 0};
    }

    syncActions();
    for (int i = 0; i < 2; i++) {
        XrHand &h = hands[i];
        XrSpaceVelocity gv = {XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation gl = {XR_TYPE_SPACE_LOCATION, &gv};
        XrSpaceLocation al = {XR_TYPE_SPACE_LOCATION};
        bool okG = XR_SUCCEEDED(xrLocateSpace(m_gripSpace[i], m_stage, predictedTime, &gl));
        bool okA = XR_SUCCEEDED(xrLocateSpace(m_aimSpace[i], m_stage, predictedTime, &al));
        h.active = okG && okA && (gl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (gl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (h.active) {
            h.grip = ToL(gl.pose);
            h.aim = ToL(al.pose);
            h.linVel = (gv.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) ? LVec3{gv.linearVelocity.x, gv.linearVelocity.y, gv.linearVelocity.z} : LVec3{0, 0, 0};
            h.angVel = (gv.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) ? LVec3{gv.angularVelocity.x, gv.angularVelocity.y, gv.angularVelocity.z} : LVec3{0, 0, 0};
        }
        auto getF = [&](XrAction a) {
            XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = a;
            gi.subactionPath = m_handPath[i];
            XrActionStateFloat st = {XR_TYPE_ACTION_STATE_FLOAT};
            xrGetActionStateFloat(m_session, &gi, &st);
            return st.isActive ? st.currentState : 0.f;
        };
        auto getB = [&](XrAction a) {
            XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = a;
            gi.subactionPath = m_handPath[i];
            XrActionStateBoolean st = {XR_TYPE_ACTION_STATE_BOOLEAN};
            xrGetActionStateBoolean(m_session, &gi, &st);
            return st.isActive && st.currentState;
        };
        h.trigger = getF(m_trigger);
        h.squeeze = getF(m_squeeze);
        XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = m_stick;
        gi.subactionPath = m_handPath[i];
        XrActionStateVector2f sv = {XR_TYPE_ACTION_STATE_VECTOR2F};
        xrGetActionStateVector2f(m_session, &gi, &sv);
        h.stickX = sv.isActive ? sv.currentState.x : 0;
        h.stickY = sv.isActive ? sv.currentState.y : 0;
        uint32_t b = 0;
        if (getB(m_btnA)) b |= LBTN_A;
        if (getB(m_btnB)) b |= LBTN_B;
        if (getB(m_stickClick)) b |= LBTN_STICK;
        if (i == 0 && getB(m_menu)) b |= LBTN_MENU;
        if (getB(m_touchA)) b |= LTOUCH_A;
        if (getB(m_touchB)) b |= LTOUCH_B;
        if (getB(m_stickTouch)) b |= LTOUCH_STICK;
        if (getB(m_triggerTouch)) b |= LTOUCH_TRIGGER;
        if (getB(m_thumbrest)) b |= LTOUCH_THUMBREST;
        h.buttons = b;
    }
}

GLuint XrApp::acquireEye(int e) {
    XrSwap &s = eye[e];
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xrAcquireSwapchainImage(s.handle, &ai, &s.acquired);
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(s.handle, &wi);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbos[s.acquired]);
    glViewport(0, 0, s.w, s.h);
    return s.fbos[s.acquired];
}

void XrApp::releaseEye(int e) {
    // Tell the driver it can drop the multisampled depth contents (saves bandwidth on tile GPUs)
    GLenum inv[] = {GL_DEPTH_STENCIL_ATTACHMENT};
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, inv);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(eye[e].handle, &ri);
}

GLuint XrApp::acquireQuad() {
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xrAcquireSwapchainImage(quadSwap.handle, &ai, &quadSwap.acquired);
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(quadSwap.handle, &wi);
    glBindFramebuffer(GL_FRAMEBUFFER, quadSwap.fbos[quadSwap.acquired]);
    glViewport(0, 0, quadSwap.w, quadSwap.h);
    return quadSwap.fbos[quadSwap.acquired];
}

void XrApp::releaseQuad() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(quadSwap.handle, &ri);
}

void XrApp::endFrame(bool projection, const XrPosef eyePose[2], const XrFovf eyeFov[2], bool quad, const XrPosef &quadPose, float qw, float qh) {
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad ql = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    std::vector<XrCompositionLayerBaseHeader *> layers;
    if (projection) {
        for (int i = 0; i < 2; i++) {
            pv[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            pv[i].pose = eyePose[i];
            pv[i].fov = eyeFov[i];
            pv[i].subImage.swapchain = eye[i].handle;
            pv[i].subImage.imageRect = {{0, 0}, {eye[i].w, eye[i].h}};
        }
        proj.space = m_stage;
        proj.viewCount = 2;
        proj.views = pv;
        layers.push_back((XrCompositionLayerBaseHeader *)&proj);
    }
    if (quad) {
        ql.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        ql.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // our UI is premultiplied
        ql.space = m_stage;
        ql.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        ql.subImage.swapchain = quadSwap.handle;
        ql.subImage.imageRect = {{0, 0}, {quadSwap.w, quadSwap.h}};
        ql.pose = quadPose;
        ql.size = {qw, qh};
        layers.push_back((XrCompositionLayerBaseHeader *)&ql);
    }
    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
    ei.displayTime = predictedTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = shouldRender ? (uint32_t)layers.size() : 0;
    ei.layers = layers.data();
    xrEndFrame(m_session, &ei);
}

void XrApp::haptic(int hand, float amplitude, float seconds, float frequency) {
    XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = amplitude < 0 ? 0 : (amplitude > 1 ? 1 : amplitude);
    v.duration = seconds > 0 ? (XrDuration)(seconds * 1e9) : XR_MIN_HAPTIC_DURATION;
    v.frequency = frequency > 0 ? frequency : XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = m_haptic;
    hi.subactionPath = m_handPath[hand & 1];
    xrApplyHapticFeedback(m_session, &hi, (XrHapticBaseHeader *)&v);
}
