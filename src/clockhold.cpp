// ext_clockhold: hold a candidate default ClockTime value in Lighting+184 for
// a few seconds and watch three independent reaction signals:
//  1) does the slot itself start changing on its own (engine advancing a live
//     clock) between our re-writes?
//  2) do SunPosition/LightDirection dwords move (lighting pipeline re-derived
//     from the clock)?
//  3) does any other lighting dword move?
// Restores the original dword at the end no matter what.
// Usage: ext_clockhold.exe [--val 6.0] [--sec 5] [--ms 20]
#include "memory.h"
#include "offsets.h"
#include "sdk.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    float val = 6.0f;
    double secs = 5.0;
    int ms = 20;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--val" && i + 1 < argc) val = (float)atof(argv[++i]);
        if (a == "--sec" && i + 1 < argc) secs = atof(argv[++i]);
        if (a == "--ms" && i + 1 < argc) ms = atoi(argv[++i]);
    }
    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe") || !m.openWrite()) { printf("FAIL: game\n"); return 1; }
    Offsets o;
    std::string log, cache = "offsets_cache.json";
    if (!OffsetManager::ensure(m.exeVersion(), cache, o, log)) { printf("FAIL: offsets\n"); return 1; }
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) { printf("FAIL: base\n"); return 1; }
    uint64_t dm = 0, light = 0;
    if (!sdk.dataModel(dm) || !sdk.findChildByClass(dm, "Lighting", light)) {
        printf("FAIL: lighting\n");
        return 1;
    }
    uint64_t clk = light + (uint64_t)o.lightingClockTime;
    uint32_t orig = 0;
    if (!m.read(clk, orig)) { printf("FAIL: read clock\n"); return 1; }
    float of = 0;
    memcpy(&of, &orig, 4);
    printf("clock slot +%d @0x%llx orig=0x%08x (f=%.6g)\n",
           o.lightingClockTime, (unsigned long long)clk, orig, of);

    auto sun = [&]() {
        std::vector<uint32_t> v;
        for (int off = 344; off < 384; off += 4) {
            uint32_t q = 0;
            m.read(light + (uint64_t)off, q);
            v.push_back(q);
        }
        return v;
    };
    auto sunStr = [](const std::vector<uint32_t>& v) {
        char b[256];
        snprintf(b, sizeof b, "dir=%.4f,%.4f,%.4f sun=%.4f,%.4f,%.4f",
                 [&] { float x; memcpy(&x, &v[0], 4); return x; }(),
                 [&] { float x; memcpy(&x, &v[1], 4); return x; }(),
                 [&] { float x; memcpy(&x, &v[2], 4); return x; }(),
                 [&] { float x; memcpy(&x, &v[4], 4); return x; }(),
                 [&] { float x; memcpy(&x, &v[5], 4); return x; }(),
                 [&] { float x; memcpy(&x, &v[6], 4); return x; }());
        return std::string(b);
    };

    auto base = sun();
    printf("baseline: %s\n", sunStr(base).c_str());

    uint32_t want = 0;
    memcpy(&want, &val, 4);
    DWORD t0 = GetTickCount();
    int engineWrites = 0, sunChanges = 0;
    uint32_t lastSeen = want, lastSun0 = base[4];
    DWORD firstEngineMs = 0, firstSunMs = 0;
    printf("holding %.3f for %.1fs, sampling every %d ms...\n", val, secs, ms);
    while ((GetTickCount() - t0) / 1000.0 < secs) {
        m.write(clk, val);
        Sleep((DWORD)ms - 3 > 0 ? ms - 3 : 1);
        uint32_t cur = 0;
        m.read(clk, cur);
        if (cur != want && cur != lastSeen) {
            float cf = 0;
            memcpy(&cf, &cur, 4);
            if (!engineWrites) firstEngineMs = GetTickCount() - t0;
            engineWrites++;
            printf("  [%5lu ms] engine rewrote clock: 0x%08x f=%.7g\n",
                   (unsigned long)(GetTickCount() - t0), cur, cf);
        }
        lastSeen = cur;
        auto s = sun();
        if (s[4] != lastSun0) {
            if (!sunChanges) firstSunMs = GetTickCount() - t0;
            sunChanges++;
            printf("  [%5lu ms] sun moved: %s\n",
                   (unsigned long)(GetTickCount() - t0), sunStr(s).c_str());
        }
        lastSun0 = s[4];
    }
    // restore
    m.writeBytes(clk, &orig, 4);
    printf("result: engineWrites=%d (first @%lu ms)  sunChanges=%d (first @%lu ms)\n",
           engineWrites, (unsigned long)firstEngineMs, sunChanges,
           (unsigned long)firstSunMs);
    uint32_t now = 0;
    m.read(clk, now);
    float nf = 0;
    memcpy(&nf, &now, 4);
    auto s = sun();
    printf("restored: clock=0x%08x (f=%.6g)  sun %s %s\n", now, nf,
           sunStr(s).c_str(), s == base ? "(== baseline)" : "(!= baseline)");
    return 0;
}
