// Small JNI helpers for things the NDK can't do on its own.
#pragma once
#include <android_native_app_glue.h>
#include <string>

namespace platform {
void Init(android_app *app);
bool HasPermission(const char *perm);
void RequestPermission(const char *perm);
void AcquireWifiLocks();   // low-latency Wi-Fi + multicast (for discovery broadcasts)
void ReleaseWifiLocks();
std::string DeviceModel(); // e.g. "Quest 3"
std::string FilesDir();
int BatteryPercent();      // -1 if unknown
}  // namespace platform
