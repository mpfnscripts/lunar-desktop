#pragma once
#include <string>

void LogInit(const std::wstring &path);
void Log(const char *fmt, ...);
std::string LogTail(size_t maxLines);

std::string Narrow(const std::wstring &w);
std::wstring Widen(const std::string &s);
std::wstring AppDataDir();   // %AppData%\LunarDesktop
std::wstring ExeDir();
