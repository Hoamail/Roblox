// ext_diag: asks the OS which window is under the cursor + foreground state.
// Usage: run, then CLICK INSIDE THE GAME repeatedly for 60s. Read-only.
#include <windows.h>
#include <cstdio>

int main() {
    SetConsoleOutputCP(CP_UTF8);
    FILE* f = fopen("diag.log", "w");
    if (!f) return 1;
    HWND ovl = FindWindowW(L"ext_ovl", nullptr);
    fprintf(f, "overlay hwnd=%p\n", ovl);
    if (ovl) {
        LONG ex = GetWindowLongW(ovl, GWL_EXSTYLE);
        RECT r{};
        GetWindowRect(ovl, &r);
        fprintf(f, "exstyle=0x%08lX visible=%d rect=%ld,%ld-%ld,%ld\n",
                ex, IsWindowVisible(ovl), r.left, r.top, r.right, r.bottom);
        fprintf(f, "TOPMOST=%d TRANSPARENT=%d NOACTIVATE=%d LAYERED=%d\n",
                !!(ex & WS_EX_TOPMOST), !!(ex & WS_EX_TRANSPARENT),
                !!(ex & WS_EX_NOACTIVATE), !!(ex & WS_EX_LAYERED));
    }
    for (int i = 0; i < 120; ++i) {
        POINT p{};
        GetCursorPos(&p);
        HWND hit = WindowFromPoint(p);
        DWORD hitPid = 0, fgPid = 0;
        char cls[64]{}, txt[128]{};
        if (hit) {
            GetWindowThreadProcessId(hit, &hitPid);
            GetClassNameA(hit, cls, sizeof cls);
            GetWindowTextA(hit, txt, sizeof txt);
        }
        HWND fg = GetForegroundWindow();
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        fprintf(f, "[%02ds] cursor=%ld,%ld hit=%p pid=%lu class=%s title=%.40s fgPid=%lu%s\n",
                i / 2, p.x, p.y, hit, hitPid, cls, txt, fgPid,
                (hit == ovl) ? "  <-- OVERLAY EATS IT" : "");
        fflush(f);
        Sleep(500);
    }
    fclose(f);
    printf("done -> diag.log\n");
    return 0;
}
