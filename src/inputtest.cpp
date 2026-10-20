// ext_inputtest: proves whether game input works while overlay is VISIBLE.
// 1. waits (up to 120s) until the GAME is foreground (user clicks it once).
// 2. snapshots CamPos, sends W-walk + right-drag via SendInput, snapshots again.
// Verdict from data, not guesses.
#include "offsets.h"
#include "sdk.h"
#include <cstdio>
#include <string>

static bool fgIsGame(DWORD pid) {
    DWORD fg = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fg);
    return fg == pid;
}

static void keyDownUp(int vk, int holdMs) {
    INPUT d{}, u{};
    d.type = u.type = INPUT_KEYBOARD;
    d.ki.wVk = u.ki.wVk = (WORD)vk;
    u.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &d, sizeof(INPUT));
    Sleep(holdMs);
    SendInput(1, &u, sizeof(INPUT));
}

static void absMove(int x, int y) {
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)(x * 65535.0 / (sx - 1));
    in.mi.dy = (LONG)(y * 65535.0 / (sy - 1));
    in.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(INPUT));
}

static void rightDrag(int x0, int y0, int x1, int y1) {
    absMove(x0, y0);
    Sleep(120);
    INPUT d{};
    d.type = INPUT_MOUSE;
    d.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    SendInput(1, &d, sizeof(INPUT));
    for (int i = 1; i <= 10; ++i) {
        absMove(x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10);
        Sleep(25);
    }
    INPUT u{};
    u.type = INPUT_MOUSE;
    u.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    SendInput(1, &u, sizeof(INPUT));
}

static bool camPos(SDK& sdk, Vec3& v) {
    uint64_t dm = 0, ws = 0, cam = 0;
    return sdk.dataModel(dm) && sdk.workspace(dm, ws) &&
           sdk.camera(ws, cam) && sdk.cameraPos(cam, v);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe") || !m.open()) {
        printf("FAIL: cannot open game\n");
        return 1;
    }
    Offsets o;
    std::string log, cache = "offsets_cache.json";
    std::string running = m.exeVersion();
    if (!OffsetManager::ensure(running, cache, o, log)) {
        printf("FAIL offsets\n");
        return 1;
    }
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) {
        printf("FAIL base\n");
        return 1;
    }
    printf("waiting up to 120s for GAME foreground (click the game once)...\n");
    fflush(stdout);
    bool ready = false;
    for (int i = 0; i < 240; ++i) {
        if (fgIsGame(m.pid)) { ready = true; break; }
        Sleep(500);
    }
    if (!ready) {
        printf("TIMEOUT: game never became foreground\n");
        return 1;
    }
    printf("game is foreground. hands off mouse/keyboard for ~8s.\n");
    fflush(stdout);
    Sleep(1500);

    Vec3 a{}, b{}, c{};
    if (!camPos(sdk, a)) { printf("FAIL read0\n"); return 1; }
    keyDownUp('W', 1200);
    Sleep(400);
    if (!camPos(sdk, b)) { printf("FAIL read1\n"); return 1; }

    // game window center for the drag
    HWND gw = GetForegroundWindow();
    RECT r{};
    GetWindowRect(gw, &r);
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    rightDrag(cx, cy, cx + 300, cy);
    Sleep(400);
    if (!camPos(sdk, c)) { printf("FAIL read2\n"); return 1; }

    printf("before: %.1f %.1f %.1f\n", a.x, a.y, a.z);
    printf("afterW: %.1f %.1f %.1f  moved=%d\n", b.x, b.y, b.z,
           (a.x != b.x || a.y != b.y || a.z != b.z));
    printf("afterD: %.1f %.1f %.1f  moved=%d\n", c.x, c.y, c.z,
           (b.x != c.x || b.y != c.y || b.z != c.z));
    return 0;
}
