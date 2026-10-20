// winlist: list top-level windows (class/title/pid/exstyle/rect/visible).
// Usage: ext_winlist.exe [filter-substring]
#include <windows.h>
#include <cstdio>
#include <string>

static const char* g_filter = nullptr;

static BOOL CALLBACK Cb(HWND h, LPARAM) {
    if (!IsWindowVisible(h)) return TRUE;
    char cls[128]{}, txt[256]{};
    GetClassNameA(h, cls, sizeof cls);
    GetWindowTextA(h, txt, sizeof txt);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    std::string s = std::string(cls) + "|" + txt;
    if (g_filter && !strstr(cls, g_filter) && !strstr(txt, g_filter)) {
        // also allow pid match
        char pb[32];
        snprintf(pb, sizeof pb, "%lu", pid);
        if (!strstr(pb, g_filter)) return TRUE;
    }
    LONG ex = GetWindowLongA(h, GWL_EXSTYLE);
    RECT r{};
    GetWindowRect(h, &r);
    printf("hwnd=%p pid=%lu ex=0x%08lX rect=%ld,%ld-%ld,%ld cls=%s title=%.50s\n",
           h, pid, ex, r.left, r.top, r.right, r.bottom, cls, txt);
    return TRUE;
}

int main(int argc, char** argv) {
    if (argc > 1) g_filter = argv[1];
    EnumWindows(Cb, 0);
    return 0;
}
