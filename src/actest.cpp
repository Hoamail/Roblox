// ext_actest: verifies the autoclicker's synthetic-input path with no game and
// no memory access. Installs low-level hooks, fires the exact production
// helpers (src/ovl/ovl_sendinput.h), and checks what the OS actually delivered:
// scan code present, KEYUP on release, EXTENDED on nav keys, and LL*HF_INJECTED
// proving the event came from SendInput rather than real hardware. Real user
// input (typing, mouse clicks) is ignored everywhere — only injected events are
// asserted on, so the test stays deterministic on a live desktop.
#include "ovl/ovl_sendinput.h"
#include <cstdio>
#include <vector>

struct KbEv {
    DWORD vk, scan, flags;
};
static std::vector<KbEv> g_kb;
static int g_injDown = 0, g_injUp = 0;

static LRESULT CALLBACK llKb(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        auto* k = (KBDLLHOOKSTRUCT*)l;
        if (w == WM_KEYDOWN || w == WM_SYSKEYDOWN || w == WM_KEYUP ||
            w == WM_SYSKEYUP)
            g_kb.push_back({k->vkCode, k->scanCode, k->flags});
    }
    return CallNextHookEx(nullptr, code, w, l);
}

static LRESULT CALLBACK llMouse(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        auto* m = (MSLLHOOKSTRUCT*)l;
        if (!(m->flags & LLMHF_INJECTED)) return CallNextHookEx(nullptr, code, w, l);
        if (w == WM_LBUTTONDOWN) g_injDown++;
        else if (w == WM_LBUTTONUP) g_injUp++;
    }
    return CallNextHookEx(nullptr, code, w, l);
}

// LL hooks are dispatched through the installing thread's message queue, so
// SendInput only becomes observable once we pump.
static void pump(int ms) {
    DWORD end = GetTickCount() + (DWORD)ms;
    MSG msg;
    while ((int)(end - GetTickCount()) > 0) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(2);
    }
}

// First injected keyboard event matching vk (or any injected event when vk==0).
static const KbEv* findInj(DWORD vk, size_t from = 0) {
    for (size_t i = from; i < g_kb.size(); i++)
        if ((g_kb[i].flags & LLKHF_INJECTED) && (!vk || g_kb[i].vk == vk))
            return &g_kb[i];
    return nullptr;
}

static int fails = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

int main() {
    HHOOK hk = SetWindowsHookExW(WH_KEYBOARD_LL, llKb, nullptr, 0);
    HHOOK hm = SetWindowsHookExW(WH_MOUSE_LL, llMouse, nullptr, 0);
    if (!hk || !hm) {
        printf("FAIL hook install kb=%p mouse=%p err=%lu\n", (void*)hk,
               (void*)hm, GetLastError());
        return 1;
    }

    // 1. plain key: space down, scancode 0x39, injected, no KEYUP flag.
    acSendKey(VK_SPACE, true);
    pump(150);
    const KbEv* dn = findInj(VK_SPACE);
    bool downOk = dn && dn->scan == 0x39 && !(dn->flags & LLKHF_UP);
    check(downOk, "space down: vk=0x20 scan=0x39 injected, no KEYUP");

    // 2. release carries KEYUP with the same scan code.
    acSendKey(VK_SPACE, false);
    pump(150);
    bool upOk = false;
    for (size_t i = 0; i < g_kb.size(); i++)
        if ((g_kb[i].flags & LLKHF_INJECTED) && g_kb[i].vk == VK_SPACE &&
            (g_kb[i].flags & LLKHF_UP) && g_kb[i].scan == 0x39)
            upOk = true;
    check(upOk, "space up: KEYUP set, scan=0x39");

    // 3. extended key: Left arrow must carry EXTENDED so it is not numpad-4.
    g_kb.clear();
    acSendKey(VK_LEFT, true);
    pump(150);
    const KbEv* lf = findInj(VK_LEFT);
    bool extOk = lf && lf->scan == 0x4B && (lf->flags & LLKHF_EXTENDED) &&
                 !(lf->flags & LLKHF_UP);
    check(extOk, "left arrow: scan=0x4B EXTENDED");
    acSendKey(VK_LEFT, false);
    pump(150);

    // 4. unbound key (0) must inject nothing at all.
    g_kb.clear();
    acSendKey(0, true);
    acSendKey(0, false);
    pump(200);
    check(findInj(0) == nullptr, "vk=0 injects nothing");

    // 5. mouse mode: LEFT down then up, both injected.
    acSendMouse(true);
    pump(150);
    acSendMouse(false);
    pump(150);
    check(g_injDown == 1 && g_injUp == 1, "mouse LMB down/up injected");

    UnhookWindowsHookEx(hk);
    UnhookWindowsHookEx(hm);
    printf("=== %s (%d fails) ===\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
