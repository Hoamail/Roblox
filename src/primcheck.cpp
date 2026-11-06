// ext_primcheck: read-only proof of the entity-liveness rule used by
// ovl_cache.cpp. For every Player it resolves the character Model, the
// HumanoidRootPart / Head parts and their Primitive, then checks
//     Instance.Parent(part) == Model
// plus Primitive.Owner(prim) -> BasePart.Primitive back-pointer.
// A live character must print PART-OK for root and head; anything else means
// the cache rule would false-positive (and re-resolve every 250 ms).
#include "offsets.h"
#include "sdk.h"
#include <cstdio>
#include <string>

// Primitive.Owner is in the dump (536) but Offsets does not expose it yet:
// keeping the probe off the parser keeps the 58-key fixture intact.
static const int kPrimOwner = 536;

static void probePart(Memory& m, const Offsets& o, SDK& sdk, uint64_t part,
                      uint64_t model, const char* label) {
    if (!part) {
        printf("      %-16s part=none\n", label);
        return;
    }
    uint64_t prim = 0, par = 0, owner = 0, back = 0;
    bool okPrim = o.basePartPrimitive &&
                  m.read(part + (uint64_t)o.basePartPrimitive, prim) && prim;
    bool okPar = o.instanceParent && m.read(part + (uint64_t)o.instanceParent, par);
    bool okOwner = okPrim && kPrimOwner &&
                   m.read(prim + (uint64_t)kPrimOwner, owner) && owner;
    bool okBack = okOwner && o.basePartPrimitive &&
                  m.read(owner + (uint64_t)o.basePartPrimitive, back);
    printf("      %-16s part=%p prim=%p parent=%p %s%s%s\n", label,
           (void*)part, (void*)prim, (void*)par,
           (!okPar || par == model) ? "PART-OK" : "PART-BAD",
           okBack ? (back == prim ? " owner-ok" : " OWNER-MISMATCH")
                  : " owner-n/a",
           sdk.readName(part).empty() ? " (name-unreadable)" : "");
}

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
    uint64_t dm = 0, players = 0;
    if (!sdk.dataModel(dm) || !sdk.findChildByClass(dm, "Players", players)) {
        printf("FAIL dm/players\n");
        return 1;
    }
    uint64_t local = 0;
    m.read(players + (uint64_t)o.playersLocalPlayer, local);
    int nPlayer = 0, nBadRoot = 0, nBadHead = 0, nOwner = 0, nBack = 0;

    sdk.walkChildren(players, [&](uint64_t p) {
        if (sdk.readClass(p) != "Player") return true;
        uint64_t model = 0, hum = 0, root = 0, head = 0;
        if (!m.read(p + (uint64_t)o.playerModelInstance, model) || !model)
            return true;
        sdk.walkChildren(model, [&](uint64_t c) {
            if (!hum && sdk.readClass(c) == "Humanoid") hum = c;
            std::string nm = sdk.readName(c);
            if (nm == "HumanoidRootPart" && !root) root = c;
            if (nm == "Head" && !head) head = c;
            return true;
        });
        ++nPlayer;
        float hp = -1.0f;
        if (hum && o.humanoidHealth) m.read(hum + (uint64_t)o.humanoidHealth, hp);
        printf("%s player=%p model=%p hum=%p hp=%.0f\n",
               (p == local) ? "[SELF]" : "      ", (void*)p, (void*)model,
               (void*)hum, hp);
        probePart(m, o, sdk, root, model, "HumanoidRootPart");
        probePart(m, o, sdk, head, model, "Head");
        // count verdicts
        auto verdict = [&](uint64_t part, int& bad) {
            if (!part) return;
            uint64_t par = 0;
            if (o.instanceParent && m.read(part + (uint64_t)o.instanceParent, par) &&
                par != model)
                ++bad;
            uint64_t prim = 0, owner = 0, back = 0;
            if (!o.basePartPrimitive ||
                !m.read(part + (uint64_t)o.basePartPrimitive, prim) || !prim)
                return;
            if (!kPrimOwner ||
                !m.read(prim + (uint64_t)kPrimOwner, owner) || !owner)
                return;
            ++nOwner;
            if (!m.read(owner + (uint64_t)o.basePartPrimitive, back)) return;
            if (back == prim) ++nBack;
        };
        verdict(root, nBadRoot);
        verdict(head, nBadHead);
        return true;
    });
    printf("\nplayers=%d  parent-bad(root)=%d parent-bad(head)=%d  "
           "owner-resolved=%d owner-back-match=%d\n",
           nPlayer, nBadRoot, nBadHead, nOwner, nBack);
    printf("PRIMCHECK %s\n", (nBadRoot == 0 && nBack == nOwner) ? "PASS" : "FAIL");
    return (nBadRoot == 0 && nBack == nOwner) ? 0 : 2;
}
