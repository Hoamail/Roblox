// ext_jumpchk: verifies the jump hack.
//  - samples Humanoid.JumpPower (0x1A0), JumpHeight (0x19C), WalkSpeed (0x1CC),
//    WalkSpeedShadow (0x3A4) at high rate -> catches a revert sawtooth
//  - samples the self Primitive's AssemblyLinearVelocity.y and reports every
//    take-off peak (v.y jump from ~0 to the launch speed), which is exactly
//    what the engine derived from JumpPower / sqrt(2*g*JumpHeight).
//    Baseline for JumpHeight 7.2 / gravity 196.2 -> v0 = 53.2.
// Read-only.
// Usage: ext_jumpchk.exe [--sec 30]
#include "memory.h"
#include "offsets.h"
#include "sdk.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    double secs = 30.0;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--sec" && i + 1 < argc) secs = atof(argv[++i]);

    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe") || !m.open()) { printf("FAIL: game\n"); return 1; }
    Offsets o;
    std::string log, cache = "offsets_cache.json";
    if (!OffsetManager::ensure(m.exeVersion(), cache, o, log)) { printf("FAIL: offsets\n"); return 1; }
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) { printf("FAIL: base\n"); return 1; }
    uint64_t dm = 0, players = 0, local = 0, hum = 0;
    if (!sdk.dataModel(dm) || !sdk.findChildByClass(dm, "Players", players) ||
        !m.read(players + (uint64_t)o.playersLocalPlayer, local)) { printf("FAIL: dm\n"); return 1; }
    sdk.walkChildren(players, [&](uint64_t p) {
        if (p != local) return true;
        uint64_t model = 0;
        if (!m.read(p + (uint64_t)o.playerModelInstance, model) || !model) return false;
        sdk.walkChildren(model, [&](uint64_t c) {
            if (sdk.readClass(c) == "Humanoid") hum = c;
            return true;
        });
        return false;
    });
    if (!hum) { printf("FAIL: humanoid\n"); return 1; }
    printf("humanoid=0x%llx  sampling %.0fs  (JumpPower=0x%x JumpHeight=0x%x)\n",
           (unsigned long long)hum, secs, o.humanoidJumpPower, o.humanoidJumpHeight);

    std::map<uint32_t, long> jhCount, jpCount, wsCount, chkCount;
    long mismatch = 0;
    double maxY = -1e9, minY = 1e9;
    std::vector<std::pair<double, double>> peaks; // t, peak v.y
    bool inAir = false;
    double peak = 0, peakT = 0;
    uint64_t n = 0, primMiss = 0;
    auto resolve = [&]() -> bool {
        uint64_t dm2 = 0, pl = 0, lp = 0, h = 0;
        if (!sdk.dataModel(dm2) || !sdk.findChildByClass(dm2, "Players", pl) ||
            !m.read(pl + (uint64_t)o.playersLocalPlayer, lp))
            return false;
        sdk.walkChildren(pl, [&](uint64_t p) {
            if (p != lp) return true;
            uint64_t model = 0;
            if (!m.read(p + (uint64_t)o.playerModelInstance, model) || !model)
                return false;
            sdk.walkChildren(model, [&](uint64_t c) {
                if (sdk.readClass(c) == "Humanoid") h = c;
                return true;
            });
            return false;
        });
        if (!h) return false;
        hum = h;
        return true;
    };
    DWORD t0 = GetTickCount();
    DWORD lastResolve = 0;
    while ((GetTickCount() - t0) / 1000.0 < secs) {
        Sleep(1);
        DWORD now = GetTickCount();
        if (now - lastResolve > 900) { // respawns swap the Humanoid object
            resolve();
            lastResolve = now;
        }
        uint32_t jpb = 0, jhb = 0, wsb = 0, chb = 0;
        bool ok = m.read(hum + (uint64_t)o.humanoidJumpPower, jpb);
        m.read(hum + (uint64_t)o.humanoidJumpHeight, jhb);
        if (!ok) { primMiss++; n++; continue; }
        jpCount[jpb]++;
        jhCount[jhb]++;
        if (m.read(hum + (uint64_t)o.humanoidWalkSpeed, wsb) &&
            m.read(hum + (uint64_t)o.humanoidWalkSpeedShadow, chb)) {
            wsCount[wsb]++;
            chkCount[chb]++;
            if (wsb != chb) mismatch++;
        }
        uint64_t prim = 0;
        if (m.read(hum + (uint64_t)o.humanoidHRP, prim) && prim) {
            float vy = 0.0f;
            Vec3 v{};
            if (m.read(prim + (uint64_t)o.primitiveVelocity, v)) {
                vy = v.y;
                double t = (GetTickCount() - t0) / 1000.0;
                if (vy > maxY) maxY = vy;
                if (vy < minY) minY = vy;
                if (vy > 20.0f) { // airborne / rising
                    if (!inAir) { inAir = true; peak = vy; peakT = t; }
                    if (vy > peak) peak = vy;
                } else if (inAir && vy < 5.0f) {
                    inAir = false;
                    peaks.push_back({peakT, peak});
                }
            } else primMiss++;
        } else primMiss++;
        n++;
    }
    double dur = (GetTickCount() - t0) / 1000.0;
    printf("samples=%llu (%.0f Hz)  primMiss=%llu\n", (unsigned long long)n,
           n / dur, (unsigned long long)primMiss);
    printf("velocity.y range: min=%.2f max=%.2f\n", minY, maxY);

    auto dump = [&](const char* nm, std::map<uint32_t, long>& mm) {
        printf("%s distinct values (%zu):\n", nm, mm.size());
        for (auto& kv : mm) {
            float f = *(float*)&kv.first;
            printf("   %.6f (0x%08x) x%ld\n", f, kv.first, kv.second);
        }
    };
    dump("JumpPower(0x1A0)", jpCount);
    dump("JumpHeight(0x19C)", jhCount);
    dump("WalkSpeed(0x1CC)", wsCount);
    dump("WalkSpeedShadow(0x3A4)", chkCount);
    printf("WalkSpeed != Shadow samples: %ld\n", mismatch);

    printf("take-off peaks detected: %zu\n", peaks.size());
    for (size_t i = 0; i < peaks.size() && i < 40; ++i)
        printf("   [%5.2fs] v0 = %.2f  (implied JumpHeight = %.2f)\n", peaks[i].first,
               peaks[i].second, (peaks[i].second * peaks[i].second) / (2 * 196.2));
    printf("\nJUMPCHK DONE\n");
    return 0;
}
