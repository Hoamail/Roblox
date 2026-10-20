// hbcheck: recon for hitbox expander. Lists character children per player,
// and for BaseParts resolves Primitive -> reads Size + CanCollide bit.
// Read-only. Usage: hbcheck.exe [--full]
#include "offsets.h"
#include "sdk.h"
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    bool full = false, writetest = false, raw = false;
    int rawN = 3;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--full") full = true;
        if (a == "--writetest") writetest = true;
        if (a == "--raw") {
            raw = true;
            if (i + 1 < argc) rawN = atoi(argv[++i]);
        }
    }
    SetConsoleOutputCP(CP_UTF8);
    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe")) {
        printf("FAIL: game\n");
        return 1;
    }
    if (writetest) {
        if (!m.openWrite()) {
            printf("FAIL: OpenProcess(write) denied err=%lu\n", GetLastError());
            return 2;
        }
    } else if (!m.open()) {
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
    uint64_t dm = 0, players = 0, local = 0;
    if (!sdk.dataModel(dm) ||
        !sdk.findChildByClass(dm, "Players", players) ||
        !m.read(players + (uint64_t)o.playersLocalPlayer, local)) {
        printf("FAIL dm/players\n");
        return 1;
    }
    uint64_t place = 0;
    sdk.placeId(dm, place);
    printf("PlaceId=%llu primSizeOff=%d primFlagsOff=%d ccBit=%d\n",
           (unsigned long long)place, o.primitiveSize, o.primitiveFlags,
           o.primitiveCanCollideBit);
    if (writetest) {
        // SELF-ONLY write test: set own Hitbox size, verify, restore.
        uint64_t model = 0;
        if (!m.read(local + (uint64_t)o.playerModelInstance, model) || !model) {
            printf("FAIL: no self model\n");
            return 1;
        }
        uint64_t hbPrim = 0;
        sdk.walkChildren(model, [&](uint64_t ch) {
            if (sdk.readName(ch) != "Hitbox") return true;
            m.read(ch + (uint64_t)o.basePartPrimitive, hbPrim);
            return false;
        });
        if (!hbPrim || !SDK::plausiblePtr(hbPrim)) {
            printf("FAIL: self Hitbox prim not found\n");
            return 1;
        }
        Vec3 orig{};
        if (!m.read(hbPrim + (uint64_t)o.primitiveSize, orig)) {
            printf("FAIL: read orig size\n");
            return 1;
        }
        printf("self Hitbox orig size=%.2f %.2f %.2f\n", orig.x, orig.y, orig.z);
        Vec3 big{10.0f, 10.0f, 10.0f};
        if (!m.write(hbPrim + (uint64_t)o.primitiveSize, big)) {
            printf("WRITE DENIED err=%lu -> expander NOT possible\n", GetLastError());
            return 2;
        }
        Vec3 back{};
        m.read(hbPrim + (uint64_t)o.primitiveSize, back);
        printf("after write: %.2f %.2f %.2f %s\n", back.x, back.y, back.z,
               (back.x == 10.0f) ? "(STUCK)" : "(REVERTED)");
        Sleep(2000);
        Vec3 later{};
        m.read(hbPrim + (uint64_t)o.primitiveSize, later);
        printf("after 2s: %.2f %.2f %.2f %s\n", later.x, later.y, later.z,
               (later.x == 10.0f) ? "(PERSISTS)" : "(SERVER REVERTED)");
        m.write(hbPrim + (uint64_t)o.primitiveSize, orig);
        Vec3 rest{};
        m.read(hbPrim + (uint64_t)o.primitiveSize, rest);
        printf("restored: %.2f %.2f %.2f\n", rest.x, rest.y, rest.z);
        printf("HBWRITE DONE\n");
        return 0;
    }
    int shown = 0;
    sdk.walkChildren(players, [&](uint64_t p) {
        if (sdk.readClass(p) != "Player") return true;
        std::string nm = sdk.readName(p);
        uint64_t model = 0;
        if (!m.read(p + (uint64_t)o.playerModelInstance, model) || !model)
            return true;
        bool self = (p == local);
        if (raw) {
            // RAW: one line per Hitbox — inst, prim, position, size, flags.
            sdk.walkChildren(model, [&](uint64_t ch) {
                if (sdk.readName(ch) != "Hitbox") return true;
                uint64_t prim = 0;
                m.read(ch + (uint64_t)o.basePartPrimitive, prim);
                Vec3 pos{8888, 8888, 8888}, sz{9999, 9999, 9999};
                unsigned flags = 0xdead;
                if (prim && SDK::plausiblePtr(prim)) {
                    m.read(prim + (uint64_t)o.primitivePosition, pos);
                    if (o.primitiveSize)
                        m.read(prim + (uint64_t)o.primitiveSize, sz);
                    if (o.primitiveFlags)
                        m.read(prim + (uint64_t)o.primitiveFlags, flags);
                }
                uint64_t hrp = 0;
                sdk.walkChildren(model, [&](uint64_t part) {
                    if (sdk.readName(part) != "HumanoidRootPart") return true;
                    m.read(part + (uint64_t)o.basePartPrimitive, hrp);
                    return false;
                });
                Vec3 hrpPos{8888, 8888, 8888};
                if (hrp && SDK::plausiblePtr(hrp))
                    m.read(hrp + (uint64_t)o.primitivePosition, hrpPos);
                printf("RAW player=%s self=%d\n", nm.c_str(), self ? 1 : 0);
                printf("  hitbox_inst=0x%llx prim=0x%llx\n",
                       (unsigned long long)ch, (unsigned long long)prim);
                printf("  hitbox_pos=%.3f %.3f %.3f\n", pos.x, pos.y, pos.z);
                printf("  hitbox_size=%.3f %.3f %.3f flags=0x%x\n", sz.x, sz.y,
                       sz.z, flags);
                printf("  hrp_pos=%.3f %.3f %.3f\n", hrpPos.x, hrpPos.y,
                       hrpPos.z);
                return false;
            });
            if (++shown >= rawN) return false;
            return true;
        }
        if (!full && !self) return true; // default: self only (fast)
        printf("== %s %s model=0x%llx\n", self ? "[SELF]" : "      ",
               nm.c_str(), (unsigned long long)model);
        sdk.walkChildren(model, [&](uint64_t ch) {
            std::string cls = sdk.readClass(ch);
            std::string cnm = sdk.readName(ch);
            if (cls != "Part" && cls != "MeshPart" && cls != "BasePart" &&
                cnm != "HumanoidRootPart" && cnm != "Head" &&
                cnm.find("HeadHB") == std::string::npos &&
                cnm.find("Hitbox") == std::string::npos &&
                cnm.find("HB") == std::string::npos)
                return true; // only interesting parts
            uint64_t prim = 0;
            m.read(ch + (uint64_t)o.basePartPrimitive, prim);
            char extra[160] = "";
            if (prim && SDK::plausiblePtr(prim) && o.primitiveSize) {
                Vec3 sz{};
                unsigned flags = 0;
                bool okSz = m.read(prim + (uint64_t)o.primitiveSize, sz);
                bool okFl = o.primitiveFlags
                    ? m.read(prim + (uint64_t)o.primitiveFlags, flags)
                    : false;
                if (okSz)
                    snprintf(extra, sizeof extra,
                             " prim=0x%llx size=%.2f %.2f %.2f%s%s",
                             (unsigned long long)prim, sz.x, sz.y, sz.z,
                             okFl ? " flags=0x" : "",
                             "");
                if (okFl) {
                    char fb[24];
                    snprintf(fb, sizeof fb, "%x", flags);
                    strncat(extra, fb, sizeof(extra) - strlen(extra) - 1);
                    if (o.primitiveCanCollideBit) {
                        char cb[64];
                        snprintf(cb, sizeof cb, " canCollide=%d",
                                 (flags & (1u << o.primitiveCanCollideBit)) != 0);
                        strncat(extra, cb, sizeof(extra) - strlen(extra) - 1);
                    }
                }
            }
            printf("  class=%-10s name=%-22s inst=0x%llx%s\n", cls.c_str(),
                   cnm.c_str(), (unsigned long long)ch, extra);
            return true;
        });
        if (!full && self) { shown = 1; return false; }
        return true;
    });
    (void)shown;
    printf("HBCHECK DONE\n");
    return 0;
}
