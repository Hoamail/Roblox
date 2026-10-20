// wsfast: tight-loop sampler (no sleep) on the local Humanoid to catch
// transient write values and any 0x1CC/0x3A4 mismatch window.
// Read-only.
//
// Usage: ext_wsfast.exe [--sec 3] [--max 2000000]
#include "offsets.h"
#include "sdk.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    double sec = 3.0;
    long long maxIter = 2000000;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--sec" && i + 1 < argc) sec = atof(argv[++i]);
        if (a == "--max" && i + 1 < argc) maxIter = _atoi64(argv[++i]);
    }
    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe") || !m.open()) { printf("FAIL: game\n"); return 1; }
    Offsets o;
    std::string log, cache = "offsets_cache.json";
    if (!OffsetManager::ensure(m.exeVersion(), cache, o, log)) { printf("FAIL: offsets\n"); return 1; }
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) { printf("FAIL: base\n"); return 1; }
    uint64_t dm = 0, players = 0, local = 0, hum = 0, prim = 0;
    if (!sdk.dataModel(dm) || !sdk.findChildByClass(dm, "Players", players) ||
        !m.read(players + (uint64_t)o.playersLocalPlayer, local)) { printf("FAIL: dm\n"); return 1; }
    sdk.walkChildren(players, [&](uint64_t p) {
        if (p != local) return true;
        uint64_t model = 0;
        if (!m.read(p + (uint64_t)o.playerModelInstance, model) || !model) return false;
        uint64_t hrp = 0;
        sdk.walkChildren(model, [&](uint64_t c) {
            if (sdk.readClass(c) == "Humanoid") hum = c;
            if (sdk.readName(c) == "HumanoidRootPart") hrp = c;
            return true;
        });
        if (hrp) m.read(hrp + (uint64_t)o.basePartPrimitive, prim);
        return false;
    });
    if (!hum) { printf("FAIL: self humanoid\n"); return 1; }
    printf("self hum=0x%llx prim=0x%llx  sampling %.1fs tight loop...\n",
           (unsigned long long)hum, (unsigned long long)prim, sec);

    struct Pair { uint32_t ws, chk; uint32_t jp, jh; };
    std::map<uint64_t, long long> hist;   // key = ws_bits<<32 | chk_bits
    long long iters = 0, mism = 0;
    // transition log
    struct Tr { double t; uint32_t ws, chk; };
    std::vector<Tr> trans;
    uint64_t prevKey = ~0ull;
    uint64_t t0 = GetTickCount64();
    double endT = (double)t0 + sec * 1000.0;
    uint32_t jpMin = 0xFFFFFFFF, jpMax = 0, jhMin = 0xFFFFFFFF, jhMax = 0;
    while (iters < maxIter) {
        uint64_t now = GetTickCount64();
        if ((double)now > endT) break;
        uint32_t ws = 0, chk = 0, jp = 0, jh = 0;
        if (!m.read(hum + 0x1CC, ws)) break;
        if (!m.read(hum + 0x3A4, chk)) break;
        m.read(hum + 0x1A0, jp);
        m.read(hum + 0x19C, jh);
        uint64_t key = ((uint64_t)ws << 32) | chk;
        hist[key]++;
        if (ws != chk) mism++;
        if (jp < jpMin) jpMin = jp;
        if (jp > jpMax) jpMax = jp;
        if (jh < jhMin) jhMin = jh;
        if (jh > jhMax) jhMax = jh;
        if (key != prevKey) {
            if (trans.size() < 400) trans.push_back({(double)(now - t0), ws, chk});
            prevKey = key;
        }
        iters++;
    }
    double dur = (double)(GetTickCount64() - t0);
    printf("iters=%lld in %.0f ms (%.0f reads/s)  mismatches(0x1CC!=0x3A4)=%lld\n",
           iters, dur, dur > 0 ? iters / (dur / 1000.0) : 0.0, mism);
    printf("JumpPower range=[%.6f .. %.6f]  JumpHeight range=[%.6f .. %.6f]\n",
           *(float*)&jpMin, *(float*)&jpMax, *(float*)&jhMin, *(float*)&jhMax);

    // top distinct values
    std::vector<std::pair<uint64_t, long long>> v(hist.begin(), hist.end());
    for (size_t i = 0; i + 1 < v.size(); ++i)
        for (size_t j = i + 1; j < v.size(); ++j)
            if (v[j].second > v[i].second) std::swap(v[i], v[j]);
    printf("\ndistinct (WalkSpeed,0x3A4) values observed: %zu\n", v.size());
    for (size_t i = 0; i < v.size() && i < 25; ++i) {
        uint32_t ws = (uint32_t)(v[i].first >> 32), chk = (uint32_t)v[i].first;
        printf("  %-14.6f (0x%08x)  x%-9lld  chk=%s\n", *(float*)&ws, ws,
               v[i].second, (chk == ws) ? "same" : "DIFF");
    }

    printf("\ntransitions (first 60):  t(ms)  WalkSpeed -> chk\n");
    for (size_t i = 0; i < trans.size() && i < 60; ++i)
        printf("  %8.0f  %14.6f   %14.6f%s\n", trans[i].t, *(float*)&trans[i].ws,
               *(float*)&trans[i].chk, trans[i].ws != trans[i].chk ? "  <<< MISMATCH" : "");
    if (trans.size() > 60) printf("  ... (%zu total transitions)\n", trans.size());
    printf("\nWSFAST DONE\n");
    return 0;
}
