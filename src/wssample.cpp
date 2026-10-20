// wssample: high-rate sampler on the local Humanoid to separate an external
// writer (sawtooth / re-write) from the game's own stat system, and to check
// whether Humanoid+0x3A4 (the "WalkSpeedCheck" slot) always mirrors +0x1CC.
// Read-only.
//
// Usage: ext_wssample.exe [--n 300] [--ms 20] [--ctrl]
#include "offsets.h"
#include "sdk.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    int N = 300, MS = 20;
    bool ctrl = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--n" && i + 1 < argc) N = atoi(argv[++i]);
        if (a == "--ms" && i + 1 < argc) MS = atoi(argv[++i]);
        if (a == "--ctrl") ctrl = true;
    }
    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe") || !m.open()) { printf("FAIL: game\n"); return 1; }
    Offsets o;
    std::string log, cache = "offsets_cache.json";
    if (!OffsetManager::ensure(m.exeVersion(), cache, o, log)) { printf("FAIL: offsets\n"); return 1; }
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) { printf("FAIL: base\n"); return 1; }
    uint64_t dm = 0, players = 0, local = 0, place = 0;
    if (!sdk.dataModel(dm) || !sdk.findChildByClass(dm, "Players", players) ||
        !m.read(players + (uint64_t)o.playersLocalPlayer, local)) { printf("FAIL: dm\n"); return 1; }
    sdk.placeId(dm, place);
    printf("placeId=%llu version=%s\n", (unsigned long long)place, m.exeVersion().c_str());

    // Resolve self (and optionally one control player) Humanoid + HRP primitive.
    auto resolve = [&](bool wantSelf, uint64_t& hum, uint64_t& prim, std::string& name) -> bool {
        bool ok = false;
        sdk.walkChildren(players, [&](uint64_t p) {
            if (sdk.readClass(p) != "Player") return true;
            bool self = (p == local);
            if (self != wantSelf) return true;
            uint64_t model = 0;
            if (!m.read(p + (uint64_t)o.playerModelInstance, model) || !model) return true;
            uint64_t h = 0, hrp = 0;
            sdk.walkChildren(model, [&](uint64_t c) {
                if (sdk.readClass(c) == "Humanoid") h = c;
                if (sdk.readName(c) == "HumanoidRootPart") hrp = c;
                return true;
            });
            if (!h) return true;
            uint64_t pr = 0;
            if (hrp && o.basePartPrimitive) m.read(hrp + (uint64_t)o.basePartPrimitive, pr);
            hum = h; prim = pr; name = sdk.readName(p);
            ok = true;
            return false;
        });
        return ok;
    };
    uint64_t hum = 0, prim = 0; std::string selfName;
    if (!resolve(true, hum, prim, selfName)) { printf("FAIL: self\n"); return 1; }
    printf("self=%s hum=0x%llx prim=0x%llx\n", selfName.c_str(),
           (unsigned long long)hum, (unsigned long long)prim);
    uint64_t chum = 0, cprim = 0; std::string cname;
    if (ctrl) {
        if (resolve(false, chum, cprim, cname))
            printf("ctrl=%s hum=0x%llx prim=0x%llx\n", cname.c_str(),
                   (unsigned long long)chum, (unsigned long long)cprim);
        else ctrl = false;
    }

    struct Row { double t; float ws, chk, jp, jh, hp; double vel; uint64_t b0; };
    std::vector<Row> rows;
    auto readRow = [&](uint64_t h, uint64_t pr) {
        Row r{};
        m.read(h + 0x1CC, r.ws);
        m.read(h + 0x3A4, r.chk);
        m.read(h + 0x1A0, r.jp);
        m.read(h + 0x19C, r.jh);
        m.read(h + 0x18C, r.hp);
        m.read(h + 0x3B0, r.b0);
        Vec3 v{0, 0, 0};
        if (pr && o.primitiveVelocity) m.read(pr + (uint64_t)o.primitiveVelocity, v);
        r.vel = std::sqrt((double)v.x * v.x + (double)v.y * v.y + (double)v.z * v.z);
        return r;
    };

    printf("\n  t(ms)     WalkSpeed      +0x3A4      JumpPower JumpH  HP      |vel|   mist\n");
    uint64_t t0 = GetTickCount64();
    int mist = 0, wsChanges = 0;
    float prevWs = -1e9f;
    for (int i = 0; i < N; ++i) {
        Row r = readRow(hum, prim);
        r.t = (double)(GetTickCount64() - t0);
        bool mm = (r.chk != r.ws);
        if (mm) mist++;
        if (r.ws != prevWs) { wsChanges++; prevWs = r.ws; }
        printf("%7.0f %13.6f %13.6f %9.3f %6.3f %7.1f %7.2f  %s\n", r.t, r.ws,
               r.chk, r.jp, r.jh, r.hp, r.vel, mm ? "MISMATCH" : "");
        rows.push_back(r);
        Sleep(MS);
    }

    // summary
    std::vector<float> ws;
    for (auto& r : rows) ws.push_back(r.ws);
    std::sort(ws.begin(), ws.end());
    printf("\n--- summary (self) ---\n");
    printf("samples=%zu  ws min=%.6f med=%.6f max=%.6f  distinct-ws-changes=%d\n",
           rows.size(), ws.front(), ws[ws.size() / 2], ws.back(), wsChanges);
    printf("0x3A4 != 0x1CC in %d/%zu samples\n", mist, rows.size());
    // Is 0x3A4 tracking 0x1CC with a delay? show the last 8 rows raw relation
    printf("last rows: t,ws,chk -> ");
    for (size_t i = rows.size() > 8 ? rows.size() - 8 : 0; i < rows.size(); ++i)
        printf("[%.0f %.4f/%.4f] ", rows[i].t, rows[i].ws, rows[i].chk);
    printf("\n");

    if (ctrl) {
        printf("\n--- control (%s) 60 samples ---\n", cname.c_str());
        printf("  t(ms)     WalkSpeed      +0x3A4      HP      |vel|\n");
        for (int i = 0; i < 60; ++i) {
            Row r = readRow(chum, cprim);
            printf("%7.0f %13.6f %13.6f %7.1f %7.2f\n",
                   (double)(GetTickCount64() - t0), r.ws, r.chk, r.hp, r.vel);
            Sleep(MS);
        }
    }
    printf("\nWSSAMPLE DONE\n");
    return 0;
}
