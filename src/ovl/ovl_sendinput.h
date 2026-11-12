#pragma once
// Synthetic input for the autoclicker: one DOWN/UP pair per press, either as a
// mouse LEFT click or as an arbitrary virtual key. Shared with ext_actest so the
// exact production path is what gets exercised.
#include <windows.h>

inline void acSendMouse(bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(in));
}

// Scan-code delivery works for both raw-input consumers and window-message
// consumers; wVk alone is dropped by some games. Extended keys (arrows, nav
// cluster, numpad operators, right ctrl/alt) need KEYEVENTF_EXTENDEDKEY or they
// arrive as their numpad twins.
inline void acSendKey(int vk, bool down) {
    if (!vk) return;
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    WORD scan = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    in.ki.wScan = scan;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    if (scan) {
        in.ki.dwFlags |= KEYEVENTF_SCANCODE;
    } else {
        in.ki.wVk = (WORD)vk;
    }
    bool ext = (vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT ||
               vk == VK_DELETE || vk == VK_HOME || vk == VK_END ||
               vk == VK_DIVIDE || vk == VK_NUMLOCK || vk == VK_RCONTROL ||
               vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN ||
               vk == VK_APPS || vk == VK_SNAPSHOT;
    if (ext) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &in, sizeof(in));
}
