#include "platform.h"
#include <jni.h>
#include "common.h"

namespace platform {

static android_app *g_app = nullptr;
static jobject g_wifiLock = nullptr, g_mcLock = nullptr;

struct Env {
    JNIEnv *env = nullptr;
    bool attached = false;
    Env() {
        JavaVM *vm = g_app->activity->vm;
        if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) {
            vm->AttachCurrentThread(&env, nullptr);
            attached = true;
        }
    }
    ~Env() {
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        if (attached) g_app->activity->vm->DetachCurrentThread();
    }
    JNIEnv *operator->() { return env; }
};

void Init(android_app *app) { g_app = app; }

bool HasPermission(const char *perm) {
    Env e;
    jobject act = g_app->activity->clazz;
    jclass cls = e->GetObjectClass(act);
    jmethodID m = e->GetMethodID(cls, "checkSelfPermission", "(Ljava/lang/String;)I");
    jstring p = e->NewStringUTF(perm);
    jint r = e->CallIntMethod(act, m, p);
    e->DeleteLocalRef(p);
    e->DeleteLocalRef(cls);
    return r == 0;
}

void RequestPermission(const char *perm) {
    Env e;
    jobject act = g_app->activity->clazz;
    jclass cls = e->GetObjectClass(act);
    jmethodID m = e->GetMethodID(cls, "requestPermissions", "([Ljava/lang/String;I)V");
    jclass strCls = e->FindClass("java/lang/String");
    jobjectArray arr = e->NewObjectArray(1, strCls, nullptr);
    jstring p = e->NewStringUTF(perm);
    e->SetObjectArrayElement(arr, 0, p);
    e->CallVoidMethod(act, m, arr, 1);
    e->DeleteLocalRef(p);
    e->DeleteLocalRef(arr);
    e->DeleteLocalRef(strCls);
    e->DeleteLocalRef(cls);
}

void AcquireWifiLocks() {
    if (g_wifiLock) return;
    Env e;
    jobject act = g_app->activity->clazz;
    jclass ctxCls = e->FindClass("android/content/Context");
    jmethodID getSvc = e->GetMethodID(ctxCls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jmethodID getAppCtx = e->GetMethodID(ctxCls, "getApplicationContext", "()Landroid/content/Context;");
    jobject appCtx = e->CallObjectMethod(act, getAppCtx);
    jstring wifiName = e->NewStringUTF("wifi");
    jobject wm = e->CallObjectMethod(appCtx, getSvc, wifiName);
    if (!wm) return;
    jclass wmCls = e->GetObjectClass(wm);
    jstring tag = e->NewStringUTF("LunarDesktop");
    // WIFI_MODE_FULL_LOW_LATENCY = 4 (API 29), fall back to WIFI_MODE_FULL_HIGH_PERF = 3
    jmethodID create = e->GetMethodID(wmCls, "createWifiLock", "(ILjava/lang/String;)Landroid/net/wifi/WifiManager$WifiLock;");
    jobject lock = e->CallObjectMethod(wm, create, 4, tag);
    if (e->ExceptionCheck()) {
        e->ExceptionClear();
        lock = e->CallObjectMethod(wm, create, 3, tag);
    }
    if (lock) {
        jclass lockCls = e->GetObjectClass(lock);
        e->CallVoidMethod(lock, e->GetMethodID(lockCls, "acquire", "()V"));
        g_wifiLock = e->NewGlobalRef(lock);
    }
    jmethodID createMc = e->GetMethodID(wmCls, "createMulticastLock", "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;");
    jobject mc = e->CallObjectMethod(wm, createMc, tag);
    if (mc) {
        jclass mcCls = e->GetObjectClass(mc);
        e->CallVoidMethod(mc, e->GetMethodID(mcCls, "acquire", "()V"));
        g_mcLock = e->NewGlobalRef(mc);
    }
    LOGI("Wi-Fi locks acquired");
}

void ReleaseWifiLocks() {
    Env e;
    for (jobject *l : {&g_wifiLock, &g_mcLock}) {
        if (!*l) continue;
        jclass c = e->GetObjectClass(*l);
        e->CallVoidMethod(*l, e->GetMethodID(c, "release", "()V"));
        e->DeleteGlobalRef(*l);
        *l = nullptr;
    }
}

std::string DeviceModel() {
    Env e;
    jclass b = e->FindClass("android/os/Build");
    jfieldID f = e->GetStaticFieldID(b, "MODEL", "Ljava/lang/String;");
    jstring s = (jstring)e->GetStaticObjectField(b, f);
    std::string out = "Quest";
    if (s) {
        const char *c = e->GetStringUTFChars(s, nullptr);
        out = c;
        e->ReleaseStringUTFChars(s, c);
    }
    return out;
}

std::string FilesDir() { return g_app->activity->internalDataPath ? g_app->activity->internalDataPath : "/data/local/tmp"; }

int BatteryPercent() {
    Env e;
    jobject act = g_app->activity->clazz;
    jclass ctxCls = e->FindClass("android/content/Context");
    jmethodID getSvc = e->GetMethodID(ctxCls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring name = e->NewStringUTF("batterymanager");
    jobject bm = e->CallObjectMethod(act, getSvc, name);
    if (!bm) return -1;
    jclass bmCls = e->GetObjectClass(bm);
    jmethodID getInt = e->GetMethodID(bmCls, "getIntProperty", "(I)I");
    int v = e->CallIntMethod(bm, getInt, 4);  // BATTERY_PROPERTY_CAPACITY
    return v;
}

}  // namespace platform
