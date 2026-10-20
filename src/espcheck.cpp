// ext_espcheck: headless P2.2 verification. Lists players with HRP positions.
// No overlay, no game interaction. User confirms names match the game.
#include "offsets.h"
#include "sdk.h"
#include <cstdio>
#include <string>

int main() {
    SetConsoleOutputCP(CP_UTF8);
    Memory m;
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
    uint64_t dm = 0;
    if (!sdk.dataModel(dm)) {
        printf("FAIL dm\n");
        return 1;
    }
    uint64_t players = 0;
    if (!sdk.findChildByClass(dm, "Players", players)) {
        printf("FAIL: Players service not found\n");
        return 1;
    }
    printf("Players=0x%llx\n", (unsigned long long)players);
    uint64_t local = 0;
    m.read(players + (uint64_t)o.playersLocalPlayer, local);
    printf("LocalPlayer=0x%llx\n", (unsigned long long)local);

    // VisualEngine matrix sanity (first row should be ~unit-scale floats).
    uint64_t ve = 0;
    float vm[16]{};
    if (m.read(sdk.base + o.visualEnginePtr, ve) && ve &&
        m.read(ve + (uint64_t)o.visualEngineViewMatrix, vm)) {
        printf("ViewMatrix row0: %.3f %.3f %.3f %.3f\n", vm[0], vm[1], vm[2], vm[3]);
    } else {
        printf("WARN: viewmatrix unreadable (VE=0x%llx)\n", (unsigned long long)ve);
    }

    int n = 0;
    sdk.walkChildren(players, [&](uint64_t p) {
        if (sdk.readClass(p) != "Player") return true;
        std::string nm = sdk.readName(p);
        std::string disp0 = sdk.readTStringDirect(p + (uint64_t)o.playerDisplayName);
        std::string disp8 = sdk.readTStringDirect(p + (uint64_t)o.playerDisplayName + 8);
        uint64_t uid = 0, model = 0;
        m.read(p + (uint64_t)o.playerUserId, uid);
        m.read(p + (uint64_t)o.playerModelInstance, model);
        Vec3 pos{};
        bool havePos = false;
        if (model) {
            // find Humanoid in character, then its HRP, then primitive pos
            sdk.walkChildren(model, [&](uint64_t h) {
                if (sdk.readClass(h) != "Humanoid") return true;
                sdk.walkChildren(model, [&](uint64_t part) {
                    if (sdk.readName(part) != "HumanoidRootPart") return true;
                    uint64_t prim = 0;
                    if (m.read(part + (uint64_t)o.basePartPrimitive, prim) && prim)
                        havePos = m.read(prim + (uint64_t)o.primitivePosition, pos);
                    return false;
                });
                return false;
            });
        }
        printf("%s Player name=%-20s disp0=%-20s disp8=%-20s uid=%llu pos=%s%.1f %.1f %.1f\n",
               (p == local) ? "[SELF]" : "      ", nm.c_str(), disp0.c_str(),
               disp8.c_str(), (unsigned long long)uid,
               havePos ? "" : "(no pos) ", pos.x, pos.y, pos.z);
        ++n;
        return true;
    });
    printf("players seen: %d\nESPCHECK DONE\n", n);
    return 0;
}
