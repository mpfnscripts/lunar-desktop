// Lunar Desktop for Meta Quest - entry point.
#include <android_native_app_glue.h>
#include "app.h"

static bool g_resumed = false;

static void OnAppCmd(android_app *app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_RESUME: g_resumed = true; break;
        case APP_CMD_PAUSE: g_resumed = false; break;
        default: break;
    }
}

void android_main(android_app *app) {
    app->onAppCmd = OnAppCmd;
    App *a = new App();
    bool quit = false;
    if (!a->init(app)) {
        LOGE("Lunar Desktop failed to start");
        ANativeActivity_finish(app->activity);
        quit = true;
    }
    while (!app->destroyRequested) {
        int events;
        android_poll_source *source;
        int timeout = (a->running() || g_resumed) && !quit ? 0 : 100;
        while (ALooper_pollOnce(timeout, nullptr, &events, (void **)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) break;
            timeout = 0;
        }
        if (app->destroyRequested) break;
        if (!quit && !a->pollXr()) {
            quit = true;
            ANativeActivity_finish(app->activity);
        }
        if (!quit && a->running()) a->frame();
    }
    a->shutdown();
    delete a;
}
