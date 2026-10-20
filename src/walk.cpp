// ext_walk: iterate DataModel children (H1 layout), print class+name each.
// Read-only. Validates vtable-in-image + descriptor chain per entry.
#include "offsets.h"
#include "sdk.h"
#include <cstdio>
#include <string>

static Memory* g_m;
static uint64_t g_base = 0, g_imgSize = 0;

static bool inImage(uint64_t v) {
    return v >= g_base && v < g_base + g_imgSize;
}

// Print up to 48 ascii chars starting at addr (raw, for eyeball parsing).
static void asciiAt(uint64_t addr, char* out, int n) {
    for (int i = 0; i < n; ++i) {
        unsigned char c = 0;
        if (!g_m->read(addr + (uint64_t)i, c) || c < 32 || c >= 127) {
            out[i] = 0;
            return;
        }
        out[i] = (char)c;
    }
    out[n] = 0;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    Memory m;
    g_m = &m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe") || !m.open()) {
        printf("FAIL: game\n");
        return 1;
    }
    Offsets o;
    std::string log, cache = "offsets_cache.json";
    if (!OffsetManager::ensure(m.exeVersion(), cache, o, log)) {
        printf("FAIL offsets\n");
        return 1;
    }
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) {
        printf("FAIL base\n");
        return 1;
    }
    g_base = sdk.base;
    // image size via module snapshot
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, m.pid);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                if (!wcscmp(me.szModule, L"RobloxPlayerBeta.exe")) {
                    g_imgSize = me.modBaseSize;
                    break;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    printf("base=0x%llx size=0x%llx\n", (unsigned long long)g_base,
           (unsigned long long)g_imgSize);

    uint64_t dm = 0;
    if (!sdk.dataModel(dm)) {
        printf("FAIL dm\n");
        return 1;
    }
    uint64_t vec = 0;
    if (!m.read(dm + 120, vec)) {
        printf("FAIL vec\n");
        return 1;
    }
    uint64_t b = 0, e = 0;
    m.read(vec, b);
    m.read(vec + 8, e);
    long long cnt = (e > b && e - b < 0x100000) ? (long long)((e - b) / 16) : -1;
    printf("children vec=0x%llx count16=%lld\n", (unsigned long long)vec, cnt);
    if (cnt < 0 || cnt > 2000) {
        printf("FAIL count\n");
        return 1;
    }
    for (long long i = 0; i < cnt; ++i) {
        uint64_t obj = 0;
        if (!m.read(b + (uint64_t)i * 16, obj) || !obj) continue;
        uint64_t vt = 0, desc = 0, dstr = 0, cont = 0, cstr = 0;
        m.read(obj, vt);
        m.read(obj + 24, desc);
        if (inImage(desc)) m.read(desc + 8, dstr);
        m.read(obj + 112, cont);
        // container+8: pointer or inline?
        uint64_t c8 = 0;
        m.read(cont + 8, c8);
        if (c8 > 0x100000000ULL && !(c8 & 7)) cstr = c8;
        char cls[48]{}, nm[48]{};
        if (dstr) asciiAt(dstr, cls, 40);
        // class strings may be length-prefixed; also try +16
        if (!cls[0] && dstr) asciiAt(dstr + 16, cls, 40);
        if (cstr)
            asciiAt(cstr, nm, 40);
        else if (c8) {
            // inline guess: ditch, mark raw
            snprintf(nm, sizeof nm, "raw8=0x%llx", (unsigned long long)c8);
        }
        printf("[%3lld] obj=0x%llx vtImg=%d class=%s name=%s\n", i,
               (unsigned long long)obj, inImage(vt), cls, nm);
    }
    printf("WALK DONE\n");
    return 0;
}
