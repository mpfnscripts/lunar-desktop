#include "input.h"
#include <vector>
#include "log.h"

void InjectPointer(const LunarPointer &p, const RECT &mon) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    float x = p.x < 0 ? 0 : (p.x > 1 ? 1 : p.x);
    float y = p.y < 0 ? 0 : (p.y > 1 ? 1 : p.y);
    double px = mon.left + x * (mon.right - mon.left - 1);
    double py = mon.top + y * (mon.bottom - mon.top - 1);
    INPUT in[2] = {};
    int n = 0;
    in[n].type = INPUT_MOUSE;
    in[n].mi.dx = (LONG)((px - vx) * 65535.0 / (vw > 1 ? vw - 1 : 1));
    in[n].mi.dy = (LONG)((py - vy) * 65535.0 / (vh > 1 ? vh - 1 : 1));
    in[n].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    n++;
    if (p.kind == LPTR_DOWN || p.kind == LPTR_UP) {
        bool down = p.kind == LPTR_DOWN;
        DWORD f = 0;
        switch (p.button) {
            case LBTN_RIGHT: f = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
            case LBTN_MIDDLE: f = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
            default: f = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        }
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = f;
        n++;
    } else if (p.kind == LPTR_SCROLL) {
        if (p.scrollY != 0) {
            in[n].type = INPUT_MOUSE;
            in[n].mi.dwFlags = MOUSEEVENTF_WHEEL;
            in[n].mi.mouseData = (DWORD)(LONG)(p.scrollY * WHEEL_DELTA);
            n++;
        } else if (p.scrollX != 0) {
            in[n].type = INPUT_MOUSE;
            in[n].mi.dwFlags = MOUSEEVENTF_HWHEEL;
            in[n].mi.mouseData = (DWORD)(LONG)(p.scrollX * WHEEL_DELTA);
            n++;
        }
    }
    SendInput(n, in, sizeof(INPUT));
}

void InjectKey(uint16_t vk, bool down) {
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    switch (vk) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: case VK_INSERT: case VK_DELETE: case VK_LWIN: case VK_RWIN:
            in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            break;
    }
    SendInput(1, &in, sizeof(in));
}

void InjectText(const std::string &utf8) {
    std::wstring w = Widen(utf8);
    std::vector<INPUT> ins;
    for (wchar_t c : w) {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = c;
        in.ki.dwFlags = KEYEVENTF_UNICODE;
        ins.push_back(in);
        in.ki.dwFlags |= KEYEVENTF_KEYUP;
        ins.push_back(in);
    }
    if (!ins.empty()) SendInput((UINT)ins.size(), ins.data(), sizeof(INPUT));
}
