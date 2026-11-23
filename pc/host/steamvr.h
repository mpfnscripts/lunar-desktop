#pragma once
#include <string>

namespace steamvr {
std::wstring RuntimePath();          // e.g. C:\Program Files (x86)\Steam\steamapps\common\SteamVR ("" if not installed)
std::wstring DriverDir();            // <exe dir>\driver\lunar
bool IsInstalled();
bool IsDriverRegistered();
bool RegisterDriver(std::string *error);
bool IsRunning();
void Launch();
void Stop();
// Sets a standing play area whose origin is the headset's floor origin (avoids SteamVR's room setup).
bool SetupChaperone();
std::string ActiveOpenXrRuntime();   // json path from the registry (empty if none)
bool OpenXrIsSteamVr();
}
