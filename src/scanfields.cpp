// ext_scanfields: READ-ONLY. Scans the local Humanoid's memory for every 4-byte
// slot that holds one of the movement values bit-exactly, so hidden "shadow"
// copies (like Humanoid+0x3A4 vs WalkSpeed) show up before we write anything.
// Control: WalkSpeed is currently being written by the overlay, so both 0x1CC and
// its shadow must appear for that value.
//
// Usage: ext_scanfields.exe [--to 0x1800]
#include "memory.h"
#include "offsets.h"
#include "sdk.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    uint32_t to = 0x1800;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--to" && i + 1 < argc) to = (uint32_t)strtoul(argv[++i], nullptr, 0);
    }
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
    printf("humanoid = 0x%llx   scanning 0x0..0x%x\n\n",
           (unsigned long long)hum, (unsigned)to);

    float ws = 0, jp = 0, jh = 0;
    m.read(hum + (uint64_t)o.humanoidWalkSpeed, ws);
    m.read(hum + (uint64_t)o.humanoidJumpPower, jp);
    m.read(hum + (uint64_t)o.humanoidHealth + 0x10, jh); // 0x19C = 412
    uint8_t ujp = 0;
    m.read(hum + 0x1DD, ujp);
    printf("WalkSpeed(0x1CC)=%.6f (0x%08x)\n", ws, *(uint32_t*)&ws);
    printf("JumpPower(0x1A0)=%.6f (0x%08x)\n", jp, *(uint32_t*)&jp);
    printf("JumpHeight(0x19C)=%.6f (0x%08x)\n", jh, *(uint32_t*)&jh);
    printf("UseJumpPower(0x1DD)=%u\n\n", (unsigned)ujp);

    struct Q { const char* name; uint32_t bits; };
    std::vector<Q> q = {
        {"WalkSpeed value", *(uint32_t*)&ws},
        {"JumpPower value", *(uint32_t*)&jp},
        {"JumpHeight value", *(uint32_t*)&jh},
        {"0.0", 0x00000000u},
    };
    for (auto& it : q) {
        printf("=== slots holding %s (0x%08x) ===\n", it.name, it.bits);
        int n = 0;
        for (uint32_t off = 0; off + 4 <= to; off += 4) {
            uint32_t v = 0;
            if (!m.read(hum + off, v)) continue;
            if (v == it.bits) {
                printf("   +0x%04x (%u)\n", off, off);
                if (++n >= 40) { printf("   ... (capped)\n"); break; }
            }
        }
        if (!n) printf("   (none)\n");
        printf("\n");
    }

    // raw window around the known check cluster
    printf("=== Humanoid +0x140..+0x200 and the +0xE00 mirror block, side by side ===\n");
    for (uint32_t off = 0x140; off < 0x200; off += 4) {
        uint32_t a = 0, b = 0;
        m.read(hum + off, a);
        m.read(hum + off + 0xE00, b);
        printf("   +0x%03x %-12.4f (0x%08x)   +0x%03x %-12.4f (0x%08x)   %s\n",
               off, *(float*)&a, a, off + 0xE00, *(float*)&b, b,
               a == b ? "same" : "DIFF");
    }
    // Is hum+0xE00 a mirror block of the same object, or just another Humanoid
    // that happens to live right after ours? Class name + full player sweep.
    printf("=== neighbour identification ===\n");
    printf("   class(hum)          = %s\n", sdk.readClass(hum).c_str());
    printf("   class(hum+0xE00)    = %s\n", sdk.readClass(hum + 0xE00).c_str());
    printf("   class(hum+0x700)    = %s\n", sdk.readClass(hum + 0x700).c_str());
    printf("   name(hum)           = %s\n", sdk.readName(hum).c_str());
    printf("   name(hum+0xE00)     = %s\n", sdk.readName(hum + 0xE00).c_str());
    printf("   all player humanoids:\n");
    sdk.walkChildren(players, [&](uint64_t p) {
        uint64_t model = 0, h = 0;
        m.read(p + (uint64_t)o.playerModelInstance, model);
        if (model) {
            sdk.walkChildren(model, [&](uint64_t c) {
                if (sdk.readClass(c) == "Humanoid") h = c;
                return true;
            });
        }
        if (h)
            printf("     0x%llx  %-20s %s%s\n", (unsigned long long)h,
                   sdk.readName(p).c_str(), h == hum ? "<< SELF" : "",
                   h == hum + 0xE00 ? "  <= the +0xE00 neighbour" : "");
        return true;
    });

    printf("\nSCANFIELDS DONE\n");
    return 0;
}
