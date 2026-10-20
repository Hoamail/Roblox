// velprobe: while velext (or any external) has speed/jump active, read the
// LOCAL player's Humanoid and show which fields actually changed vs other
// players (control group). Read-only.
//
// Answers: does the external write Humanoid.WalkSpeed/JumpPower (property
// path) or drive Primitive.AssemblyLinearVelocity (velocity path)?
//
// Usage: ext_velprobe.exe [--dump]
#include "offsets.h"
#include "sdk.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static bool readF(Memory& m, uint64_t a, float& f) { return m.read(a, f); }

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    bool dump = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--dump") dump = true;

    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe") || !m.open()) {
        printf("FAIL: game process\n");
        return 1;
    }
    Offsets o;
    std::string log, cache = "offsets_cache.json";
    if (!OffsetManager::ensure(m.exeVersion(), cache, o, log)) {
        printf("FAIL: offsets\n");
        return 1;
    }
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) { printf("FAIL: base\n"); return 1; }

    printf("version=%s\n", m.exeVersion().c_str());
    printf("offset table: WalkSpeed=%d JumpPower=%d Health=%d MaxHealth=%d "
           "HRP=%d PrimVel=%d PrimPos=%d\n",
           o.humanoidWalkSpeed, o.humanoidJumpPower, o.humanoidHealth,
           o.humanoidMaxHealth, o.humanoidHRP, o.primitiveVelocity,
           o.primitivePosition);

    uint64_t dm = 0, players = 0, local = 0;
    if (!sdk.dataModel(dm) || !sdk.findChildByClass(dm, "Players", players) ||
        !m.read(players + (uint64_t)o.playersLocalPlayer, local)) {
        printf("FAIL: dm/players\n");
        return 1;
    }

    auto probe = [&](const char* tag) {
        printf("\n===== SAMPLE %s =====\n", tag);
        int idx = 0, selfIdx = -1;
        std::vector<uint64_t> selfH{};
        sdk.walkChildren(players, [&](uint64_t p) {
            if (sdk.readClass(p) != "Player") return true;
            std::string nm = sdk.readName(p);
            uint64_t model = 0;
            if (!m.read(p + (uint64_t)o.playerModelInstance, model) || !model)
                return true;
            bool self = (p == local);
            uint64_t hum = 0;
            sdk.walkChildren(model, [&](uint64_t c) {
                if (sdk.readClass(c) == "Humanoid") { hum = c; return false; }
                return true;
            });
            if (!hum) return true;
            float ws = -1, jp = -1, jh = -1, hp = -1, mh = -1;
            if (o.humanoidWalkSpeed) readF(m, hum + (uint64_t)o.humanoidWalkSpeed, ws);
            if (o.humanoidJumpPower) readF(m, hum + (uint64_t)o.humanoidJumpPower, jp);
            readF(m, hum + 0x19C, jh);            // JumpHeight
            if (o.humanoidHealth)    readF(m, hum + (uint64_t)o.humanoidHealth, hp);
            if (o.humanoidMaxHealth) readF(m, hum + (uint64_t)o.humanoidMaxHealth, mh);
            uint8_t ujp = 0;
            m.read(hum + 0x1DD, ujp);             // UseJumpPower
            // HRP velocity
            uint64_t hrpInst = 0, prim = 0;
            sdk.walkChildren(model, [&](uint64_t c) {
                if (sdk.readName(c) == "HumanoidRootPart") { hrpInst = c; return false; }
                return true;
            });
            Vec3 vel{0, 0, 0}, pos{0, 0, 0};
            if (hrpInst && o.basePartPrimitive) {
                m.read(hrpInst + (uint64_t)o.basePartPrimitive, prim);
                if (SDK::plausiblePtr(prim)) {
                    if (o.primitiveVelocity)
                        m.read(prim + (uint64_t)o.primitiveVelocity, vel);
                    if (o.primitivePosition)
                        m.read(prim + (uint64_t)o.primitivePosition, pos);
                }
            }
            double spd = std::sqrt((double)vel.x * vel.x + (double)vel.y * vel.y +
                                   (double)vel.z * vel.z);
            if (self) { selfIdx = idx; selfH.push_back(hum); }
            printf("%-8s %-22s hum=0x%llx WalkSpeed=%-8.3f JumpPower=%-8.3f "
                   "JumpHeight=%-7.3f UseJumpPower=%d HP=%.1f/%.1f |HRP vel|=%.3f\n",
                   self ? "[SELF]" : "", nm.c_str(), (unsigned long long)hum, ws,
                   jp, jh, (int)ujp, hp, mh, spd);
            idx++;
            return true;
        });
        if (selfIdx < 0) printf("  (no self humanoid found)\n");
        return selfH.empty() ? (uint64_t)0 : selfH[0];
    };

    uint64_t hum = probe("A");
    if (!hum) { printf("FAIL: self humanoid not found\n"); return 1; }

    // Fields around the check region: WalkspeedCheck (0x3A4) and friends.
    printf("\n--- self humanoid check region (raw) ---\n");
    for (int off = 0x380; off < 0x3D0; off += 8) {
        uint64_t q = 0;
        if (!m.read(hum + (uint64_t)off, q)) break;
        float f0, f1;
        memcpy(&f0, &q, 4);
        memcpy(&f1, (char*)&q + 4, 4);
        printf("  +0x%03x: 0x%016llx  f=[%.6g, %.6g]\n", off,
               (unsigned long long)q, f0, f1);
    }
    printf("--- WalkSpeed / JumpPower / JumpHeight slot bytes ---\n");
    for (int off : {o.humanoidWalkSpeed, o.humanoidJumpPower, 0x19C}) {
        if (off <= 0) continue;
        uint32_t u = 0;
        float f = 0;
        m.read(hum + (uint64_t)off, u);
        memcpy(&f, &u, 4);
        printf("  +0x%03x: raw=0x%08x float=%.6f\n", off, u, f);
    }

    if (dump) {
        printf("\n--- self humanoid window 0x000..0x900 (16B/line: 4 floats) ---\n");
        for (int off = 0; off < 0x900; off += 16) {
            float q[4] = {0, 0, 0, 0};
            if (!m.read(hum + (uint64_t)off, q)) break;
            printf("+0x%03x: %12.4g %12.4g %12.4g %12.4g\n", off, q[0], q[1],
                   q[2], q[3]);
        }
    }

    Sleep(1500);
    probe("B (after 1.5s)");
    printf("\nVELPROBE DONE\n");
    return 0;
}
