// ext_worldprobe: read-only dump of the World-tab slots in the live game,
// plus an optional --try-clock write-probe.
// Read-only mode answers: what does Lighting+184 (theo "ClockTime") really
// hold, where is the clock, what are the Water/zoom values?
// --try-clock: writes 6.0 to candidate float slots (restoring each after
// ~150 ms) and watches SunPosition/LightDirection + the whole lighting block
// for an engine reaction. Only the real ClockTime makes the sun jump, so the
// reacting slot identifies itself. SunPosition := engine-derived from clock.
// Writes are transient (restored), client-local, and capped at ~8 candidates.
// Usage: ext_worldprobe.exe [--ms 2000] [--try-clock]
#include "memory.h"
#include "offsets.h"
#include "sdk.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

static bool plausF(float f) {
    return f == 0.0f || (std::isfinite(f) && fabsf(f) > 1e-4f && fabsf(f) < 1e6f);
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    int gapMs = 2000;
    bool tryClock = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--ms" && i + 1 < argc) gapMs = atoi(argv[++i]);
        if (a == "--try-clock") tryClock = true;
    }

    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe") || !m.openWrite()) { printf("FAIL: game\n"); return 1; }
    Offsets o;
    std::string log, cache = "offsets_cache.json";
    if (!OffsetManager::ensure(m.exeVersion(), cache, o, log)) { printf("FAIL: offsets\n"); return 1; }
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) { printf("FAIL: base\n"); return 1; }

    uint64_t dm = 0, ws = 0, cam = 0, light = 0, terr = 0, players = 0, local = 0;
    if (!sdk.dataModel(dm)) { printf("FAIL: dm\n"); return 1; }
    sdk.workspace(dm, ws);
    sdk.camera(ws, cam);
    sdk.findChildByClass(dm, "Lighting", light);
    sdk.findChildByClass(ws, "Terrain", terr);
    sdk.findChildByClass(dm, "Players", players);
    if (players) m.read(players + (uint64_t)o.playersLocalPlayer, local);
    printf("ver=%s dm=0x%llx light=0x%llx(%s) terr=0x%llx cam=0x%llx plr=0x%llx\n",
           m.exeVersion().c_str(), (unsigned long long)dm,
           (unsigned long long)light, light ? sdk.readClass(light).c_str() : "-",
           (unsigned long long)terr, (unsigned long long)cam, (unsigned long long)local);
    if (!light) { printf("no Lighting\n"); return 0; }

    auto sample = [&](uint64_t addr, int from, int to) {
        std::vector<uint32_t> v;
        for (int off = from; off < to; off += 4) {
            uint32_t q = 0;
            if (!m.read(addr + (uint64_t)off, q)) q = 0;
            v.push_back(q);
        }
        return v;
    };
    auto dump = [&](const char* label, uint64_t addr, int from,
                    const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
        printf("--- %s @0x%llx ---\n", label, (unsigned long long)addr);
        for (size_t i = 0; i < a.size(); ++i) {
            int off = from + (int)i * 4;
            float fa = 0, fb = 0;
            memcpy(&fa, &a[i], 4);
            memcpy(&fb, &b[i], 4);
            int ia = 0;
            memcpy(&ia, &a[i], 4);
            printf("+%4d  0x%08x %s f=%s%.6g  i=%d\n", off, a[i],
                   a[i] == b[i] ? " " : "*", plausF(fa) ? "" : "~", fa, ia);
        }
    };

    auto la = sample(light, 0, 512);
    auto tb = terr ? sample(terr, 432, 528) : std::vector<uint32_t>{};
    auto pc = local ? sample(local, 848, 912) : std::vector<uint32_t>{};
    auto ca = cam ? sample(cam, 0, 1024) : std::vector<uint32_t>{};
    Sleep((DWORD)gapMs);
    auto lb = sample(light, 0, 512);
    auto tc = terr ? sample(terr, 432, 528) : std::vector<uint32_t>{};
    auto pd = local ? sample(local, 848, 912) : std::vector<uint32_t>{};
    auto cb = cam ? sample(cam, 0, 1024) : std::vector<uint32_t>{};

    printf("gap=%d ms; '*' = dword changed on its own\n", gapMs);
    dump("Lighting[0..512)", light, 0, la, lb);
    if (terr) dump("Terrain[432..528)", terr, 432, tb, tc);
    if (local) dump("Player(local)[848..912)", local, 848, pc, pd);

    // Clock candidates in the lighting property block [0,512): any 4-byte
    // window whose float is a plausible hour, plus double windows.
    printf("--- hour-like candidates (float/double 0.2..23.5), [0..512) ---\n");
    for (int off = 0; off < 512; off += 4) {
        float f = 0;
        memcpy(&f, &la[off / 4], 4);
        if (std::isfinite(f) && f >= 0.2f && f <= 23.5f)
            printf("f32 @+%d = %.6g\n", off, f);
    }
    for (int off = 0; off + 8 <= 512; off += 4) {
        double d = 0;
        memcpy(&d, &la[off / 4], 8);
        if (std::isfinite(d) && d >= 0.2 && d <= 23.5)
            printf("f64 @+%d = %.6g\n", off, d);
    }

    // FOV: rays vs degrees.
    if (cam) {
        float f32 = 0, f30 = 0, f16 = 0;
        m.read(cam + (uint64_t)o.cameraFOV, f32);
        m.read(cam + (uint64_t)o.cameraFOV + 4, f30);
        m.read(cam + (uint64_t)o.cameraFOV + 12, f16);
        printf("cam FOV @+%d f32=%.6g (deg %.3g)  +4=%.6g  +12=%.6g\n",
               o.cameraFOV, f32, f32 * 57.29578f, f30, f16);
    }
    if (local) {
        float mn = 0, mx = 0;
        m.read(local + (uint64_t)o.playerMinZoomDistance, mn);
        m.read(local + (uint64_t)o.playerMaxZoomDistance, mx);
        printf("plr MinZoom@%d=%.3f MaxZoom@%d=%.3f\n",
               o.playerMinZoomDistance, mn, o.playerMaxZoomDistance, mx);
    }

    if (!tryClock) { printf("done (read-only)\n"); return 0; }

    // ---- write-probe: which slot is ClockTime? -------------------------------
    // Reaction signal 1: SunPosition/LightDirection dwords [344..372) change.
    // Reaction signal 2: any other lighting dword in [184..448) changes.
    auto watch = [&]() {
        std::vector<uint32_t> v;
        for (int off = 344; off < 372; off += 4) {
            uint32_t q = 0;
            m.read(light + (uint64_t)off, q);
            v.push_back(q);
        }
        return v;
    };
    std::vector<int> cands;
    for (int off = 0; off < 512; off += 4) {
        float f = 0;
        memcpy(&f, &la[off / 4], 4);
        if (!(std::isfinite(f) && f >= 0.2f && f <= 23.5f)) continue;
        // Known colour/vector slots (two colors of 3 floats, direction+sun+pos):
        bool knownCol = (off >= 192 && off < 252) || (off >= 320 && off < 344);
        bool knownVec = off >= 344 && off < 384;
        if (knownCol || knownVec) continue;
        cands.push_back(off);
    }
    printf("--- try-clock: %zu hour-like candidates (+184 explicit) ---\n", cands.size());

    auto probeWrite = [&](int off, int width, const char* what) -> bool {
        unsigned char orig[8]{};
        for (int i = 0; i < width; i += 4) {
            uint32_t q = 0;
            if (!m.read(light + (uint64_t)off + i, q)) return false;
            memcpy(orig + i, &q, 4);
        }
        unsigned char val[8]{};
        if (width == 4) {
            float f = 6.0f;
            memcpy(val, &f, 4);
        } else {
            double d = 6.0;
            memcpy(val, &d, 8);
        }
        auto before = watch();
        auto region = sample(light, 184, 448);
        if (!m.writeBytes(light + (uint64_t)off, val, width)) {
            printf("  %s @+%d: write failed\n", what, off);
            return false;
        }
        bool sunMoved = false, regionMoved = false;
        int firstRegion = -1;
        std::vector<uint32_t> afterSun;
        for (int i = 0; i < 45; ++i) {
            Sleep(3);
            auto s = watch();
            if (s != before && !sunMoved) {
                sunMoved = true;
                afterSun = s;
            }
            auto r = sample(light, 184, 448);
            for (size_t k = 0; k < r.size(); ++k) {
                if (r[k] != region[k]) {
                    int roff = 184 + (int)k * 4;
                    if (roff == off || (width == 8 && roff == off + 4)) continue; // our write
                    regionMoved = true;
                    if (firstRegion < 0) firstRegion = roff;
                }
            }
        }
        // slot state at the end of the window
        uint32_t endQ = 0;
        m.read(light + (uint64_t)off, endQ);
        uint32_t origQ = 0;
        memcpy(&origQ, orig, 4);
        char slot[64];
        if (endQ == origQ) snprintf(slot, sizeof slot, "reverted");
        else {
            float fe = 0;
            memcpy(&fe, &endQ, 4);
            snprintf(slot, sizeof slot, "holds 0x%08x f=%.4g", endQ, fe);
        }
        printf("  %s @+%d w=%d: slot=%s sun=%s region=%s%s\n", what, off, width,
               slot, sunMoved ? "JUMPED" : "still",
               regionMoved ? "changed" : "still",
               firstRegion >= 0 ? (" first@+" + std::to_string(firstRegion)).c_str() : "");
        m.writeBytes(light + (uint64_t)off, orig, width); // restore
        Sleep(60);
        if (sunMoved) {
            printf("  >>> CLOCK SLOT: +%d (write %d B) <<<\n", off, width);
            return true;
        }
        return false;
    };

    if (probeWrite(184, 4, "theo-f32")) return 0;
    if (probeWrite(184, 8, "theo-f64")) return 0;
    if (probeWrite(180, 4, "f32")) return 0;
    if (probeWrite(188, 4, "f32")) return 0;
    for (int off : cands) {
        if (probeWrite(off, 4, "cand")) return 0;
    }
    printf("done: no slot reacted -> clock not a plain float in [0,512)\n");
    return 0;
}
