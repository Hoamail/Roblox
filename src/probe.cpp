// winprobe: proves whether clicks pass through the overlay.
// Creates a normal window, SendInputs clicks at its center, logs what arrives.
// The fullscreen overlay (if visible) sits ABOVE it. Run while overlay is up.
#include <windows.h>
#include <cstdio>

static FILE* g_log = nullptr;
static int g_down = 0, g_up = 0;

static LRESULT WINAPI Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_LBUTTONDOWN) {
        ++g_down;
        fprintf(g_log, "LBUTTONDOWN #%d\n", g_down);
        fflush(g_log);
    } else if (m == WM_LBUTTONUP) {
        ++g_up;
        fprintf(g_log, "LBUTTONUP #%d\n", g_up);
        fflush(g_log);
    } else if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void clickAt(int x, int y) {
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    INPUT in[2]{};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dx = (LONG)(x * 65535.0 / (sx - 1));
    in[0].mi.dy = (LONG)(y * 65535.0 / (sy - 1));
    in[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dx = in[0].mi.dx;
    in[1].mi.dy = in[0].mi.dy;
    in[1].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

int main() {
    g_log = fopen("probe.log", "w");
    if (!g_log) return 1;
    WNDCLASSA wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "winprobe";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&wc);
    HWND h = CreateWindowA("winprobe", "probe", WS_OVERLAPPEDWINDOW,
                           400, 200, 400, 300, nullptr, nullptr,
                           wc.hInstance, nullptr);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    RECT r{};
    GetWindowRect(h, &r);
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    fprintf(g_log, "probe center=%d,%d\n", cx, cy);
    fflush(g_log);
    // Let overlay settle on top, then click 3x at probe center.
    Sleep(2500);
    for (int i = 0; i < 3; ++i) {
        clickAt(cx, cy);
        Sleep(800);
    }
    // Keep pumping 8s so late messages arrive.
    DWORD t0 = GetTickCount();
    MSG msg{};
    while (GetTickCount() - t0 < 8000) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            if (msg.message == WM_QUIT) break;
        }
        Sleep(20);
    }
    fprintf(g_log, "SUMMARY down=%d up=%d (expect 3/3)\n", g_down, g_up);
    fclose(g_log);
    printf("down=%d up=%d\n", g_down, g_up);
    DestroyWindow(h);
    return 0;
}
