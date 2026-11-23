#pragma once
#include <windows.h>
#include <string>
#include "../../shared/lunar_protocol.h"

void InjectPointer(const LunarPointer &p, const RECT &monitor);
void InjectKey(uint16_t vk, bool down);
void InjectText(const std::string &utf8);
