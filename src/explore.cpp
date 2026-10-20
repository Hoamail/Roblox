// ext_explore: dump DataModel neighborhoods to reverse children/name layout.
// Read-only. Prints hex + ascii for interpretation.
#include "offsets.h"
#include "sdk.h"
#include <cstdio>
#include <string>
#include <vector>

static Memory* g_m;

static void dumpQ(const char* label, uint64_t addr, int n) {
    printf("--- %s @ 0x%llx ---\n", label, (unsigned long long)addr);
    for (int i = 0; i < n; ++i) {
        uint64_t v = 0;
        bool ok = g_m->read(addr + (uint64_t)i * 8, v);
        char asc[9];
        for (int k = 0; k < 8; ++k) {
            unsigned char c = (v >> (k * 8)) & 0xFF;
            asc[k] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        asc[8] = 0;
        printf("+%3d 0x%016llx %s %s\n", i * 8, (unsigned long long)v,
               ok ? "ok " : "BAD", asc);
    }
}

static bool heapPtr(uint64_t v) {
    return v > 0x100000000ULL && (v & 7) == 0;
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
    printf("%s", log.c_str());
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) {
        printf("FAIL base\n");
        return 1;
    }
    uint64_t dm = 0;
    if (!sdk.dataModel(dm)) {
        printf("FAIL dm\n");
        return 1;
    }
    printf("DataModel=0x%llx\n", (unsigned long long)dm);

    // 1) Instance header area of DataModel itself.
    dumpQ("DM+0..152", dm, 20);

    // 2) Follow NameContainer(+112) and ClassDescriptor(+24) one level.
    uint64_t cont = 0, desc = 0;
    if (m.read(dm + 112, cont) && heapPtr(cont)) dumpQ("NameContainer?", cont, 6);
    if (m.read(dm + 24, desc) && heapPtr(desc)) dumpQ("ClassDescriptor?", desc, 8);

    // 3) Children hypotheses.
    uint64_t cs = 0, ce = 0;
    m.read(dm + 120, cs);
    m.read(dm + 8, ce);
    printf("CS_raw=0x%llx CE_raw=0x%llx\n", (unsigned long long)cs, (unsigned long long)ce);
    // H1: cs -> {begin,end} struct, end at +8
    if (heapPtr(cs)) {
        uint64_t b = 0, e = 0;
        m.read(cs, b);
        m.read(cs + 8, e);
        printf("H1: begin=0x%llx end=0x%llx count16=%lld\n", (unsigned long long)b,
               (unsigned long long)e, (long long)(e > b ? (e - b) / 16 : -1));
        if (heapPtr(b)) dumpQ("H1.array[0..3]", b, 4);
    }
    // H2: inline vector at dm+120: begin,end = next qwords
    {
        uint64_t b = 0, e = 0;
        m.read(dm + 120, b);
        m.read(dm + 128, e);
        printf("H2: begin=0x%llx end=0x%llx count16=%lld\n", (unsigned long long)b,
               (unsigned long long)e, (long long)(e > b ? (e - b) / 16 : -1));
        if (heapPtr(b) && e > b && (e - b) < 0x10000) dumpQ("H2.array[0..3]", b, 4);
    }
    printf("EXPLORE DONE\n");
    return 0;
}
