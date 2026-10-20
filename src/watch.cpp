// watch: tight-loop (no sleep) sampler for the local Humanoid.
//   - internal sampling at max rate (tens of kHz): catches transient writes
//     and any 0x1CC/0x3A4 mismatch window
//   - CSV throttled to ~500 Hz
//   - per-250ms bins: distinct WalkSpeed values, |AssemblyLinearVelocity|,
//     and MEASURED ground speed from position deltas (20ms window)
// Read-only.
//
// Usage: ext_watch.exe [--sec 24] [--csv watch.csv]
#include "offsets.h"
#include "sdk.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static uint64_t qpcNow() { LARGE_INTEGER l; QueryPerformanceCounter(&l); return (uint64_t)l.QuadPart; }
static uint64_t qpcFreq() { LARGE_INTEGER l; QueryPerformanceFrequency(&l); return (uint64_t)l.QuadPart; }

struct Bin {
    std::map<uint32_t, long long> ws;
    double maxMeas = 0, sumMeas = 0, maxV = 0, sumV = 0;
    long long nMeas = 0, n = 0;
};

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    double sec = 24.0;
    std::string csv = "watch.csv";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--sec" && i + 1 < argc) sec = atof(argv[++i]);
        if (a == "--csv" && i + 1 < argc) csv = argv[++i];
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
    printf("self hum=0x%llx prim=0x%llx  logging %.0fs -> %s\n",
           (unsigned long long)hum, (unsigned long long)prim, sec, csv.c_str());

    FILE* f = fopen(csv.c_str(), "w");
    if (!f) { printf("FAIL: csv\n"); return 1; }
    fprintf(f, "t_ms,ws,chk,jp,jh,hp,vx,vy,vz,vmag,meas_speed,px,py,pz\n");

    uint64_t freq = qpcFreq(), t0 = qpcNow();
    long long iters = 0, mism = 0, csvRows = 0;
    double firstMismatchT = -1;
    uint64_t lastCsv = 0, lastPrint = 0;
    double prevX = 0, prevY = 0, prevZ = 0, prevPosT = -1;
    std::vector<Bin> bins;
    std::map<uint32_t, long long> wsHistAll;
    uint64_t prevKey = ~0ull;
    long long transitions = 0;

    while (true) {
        uint64_t now = qpcNow();
        double t = (double)(now - t0) * 1000.0 / (double)freq;
        if (t > sec * 1000.0) break;
        uint32_t ws = 0, chk = 0, jp = 0, jh = 0;
        float hp = 0;
        Vec3 v{0, 0, 0}, pos{0, 0, 0};
        if (!m.read(hum + 0x1CC, ws)) break;
        if (!m.read(hum + 0x3A4, chk)) break;
        m.read(hum + 0x1A0, jp);
        m.read(hum + 0x19C, jh);
        m.read(hum + 0x18C, hp);
        if (prim && SDK::plausiblePtr(prim)) {
            m.read(prim + (uint64_t)o.primitiveVelocity, v);
            m.read(prim + (uint64_t)o.primitivePosition, pos);
        }
        double vmag = std::sqrt((double)v.x * v.x + (double)v.y * v.y + (double)v.z * v.z);
        double meas = 0;
        if (prevPosT >= 0 && (t - prevPosT) >= 20.0) {
            double dt = (t - prevPosT) / 1000.0;
            double dx = pos.x - prevX, dy = pos.y - prevY, dz = pos.z - prevZ;
            meas = std::sqrt(dx * dx + dy * dy + dz * dz) / dt;
            prevX = pos.x; prevY = pos.y; prevZ = pos.z; prevPosT = t;
        } else if (prevPosT < 0) {
            prevX = pos.x; prevY = pos.y; prevZ = pos.z; prevPosT = t;
        }
        uint64_t key = ((uint64_t)ws << 32) | chk;
        if (key != prevKey) { transitions++; prevKey = key; }
        if (ws != chk) {
            mism++;
            if (firstMismatchT < 0) firstMismatchT = t;
        }
        wsHistAll[ws]++;
        size_t bi = (size_t)(t / 250.0);
        if (bi >= bins.size()) bins.resize(bi + 1);
        Bin& b = bins[bi];
        b.n++;
        b.ws[ws]++;
        if (meas < 300) { b.sumMeas += meas; b.nMeas++; if (meas > b.maxMeas) b.maxMeas = meas; }
        if (vmag > b.maxV) b.maxV = vmag;
        b.sumV += vmag;
        if ((now - lastCsv) * 1000ULL >= freq / 500) {
            lastCsv = now;
            fprintf(f, "%.1f,%.6f,%.6f,%.6f,%.6f,%.1f,%.4f,%.4f,%.4f,%.3f,%.3f,%.2f,%.2f,%.2f\n",
                    t, *(float*)&ws, *(float*)&chk, *(float*)&jp, *(float*)&jh, hp,
                    v.x, v.y, v.z, vmag, meas, pos.x, pos.y, pos.z);
            csvRows++;
        }
        if ((now - lastPrint) * 1000ULL >= freq) {
            lastPrint = now;
            printf("  t=%4.0fs ws=%.3f chk=%.3f meas=%.2f |v|=%.2f hp=%.0f\n",
                   t / 1000.0, *(float*)&ws, *(float*)&chk, meas, vmag, hp);
            fflush(stdout);
        }
        iters++;
    }
    fclose(f);
    double dur = (double)(qpcNow() - t0) / freq;
    printf("\n--- summary ---\niterations=%lld (%.0f Hz internal)  csvRows=%lld  transitions=%lld  mismatches(ws!=chk)=%lld",
           iters, iters / dur, csvRows, transitions, mism);
    if (firstMismatchT >= 0) printf(" (first at t=%.1fms)", firstMismatchT);
    printf("\n\ndistinct WalkSpeed values overall:\n");
    for (auto& kv : wsHistAll)
        printf("  %-14.6f (0x%08x)  x%lld\n", *(float*)&kv.first, kv.first, kv.second);
    printf("\nper-250ms bins: t(ms)  n  ws-values  maxMeas  max|v|\n");
    for (size_t i = 0; i < bins.size(); ++i) {
        Bin& b = bins[i];
        if (b.n == 0) continue;
        printf("  %6.0f  %6lld  ", (double)i * 250.0, b.n);
        int k = 0;
        for (auto& kv : b.ws) {
            printf("%.2f", *(float*)&kv.first);
            if (kv.second != b.n) printf("(x%lld)", kv.second);
            if (++k < (int)b.ws.size()) printf("/");
        }
        printf("  meas=%.2f  |v|=%.2f\n", b.maxMeas, b.maxV);
    }
    printf("\ncsv=%s\nWATCH DONE\n", csv.c_str());
    return 0;
}
