// ext_fovpair: are Camera+304 (theo FieldOfView) and the duplicate at +316
// coupled? Writes a marker value into one, reads both after ~400 ms, restores.
// If the engine copies +304 -> +316 the two are a property+cache pair.
// All writes transient and restored. Usage: ext_fovpair.exe
#include "memory.h"
#include "offsets.h"
#include "sdk.h"
#include <cstdio>
#include <cstring>
#include <string>

static void show(Memory& m, uint64_t cam, int base) {
    float a = 0, b = 0;
    uint32_t qa = 0, qb = 0;
    m.read(cam + (uint64_t)base, qa);
    m.read(cam + (uint64_t)base + 12, qb);
    memcpy(&a, &qa, 4);
    memcpy(&b, &qb, 4);
    printf("    +%d=0x%08x (%.6g)  +%d=0x%08x (%.6g)\n", base, qa, a, base + 12,
           qb, b);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe") || !m.openWrite()) { printf("FAIL: game\n"); return 1; }
    Offsets o;
    std::string log, cache = "offsets_cache.json";
    if (!OffsetManager::ensure(m.exeVersion(), cache, o, log)) { printf("FAIL: offsets\n"); return 1; }
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) { printf("FAIL: base\n"); return 1; }
    uint64_t dm = 0, ws = 0, cam = 0;
    if (!sdk.dataModel(dm)) { printf("FAIL: dm\n"); return 1; }
    sdk.workspace(dm, ws);
    sdk.camera(ws, cam);
    if (!cam) { printf("FAIL: camera\n"); return 1; }
    int base = o.cameraFOV;
    printf("camera=0x%llx FOV@%d\n", (unsigned long long)cam, base);
    show(m, cam, base);

    uint32_t orig304 = 0, orig316 = 0;
    m.read(cam + (uint64_t)base, orig304);
    m.read(cam + (uint64_t)base + 12, orig316);

    // A: marker into +304
    float v = 2.0f;
    m.write(cam + (uint64_t)base, v);
    printf("A: wrote 2.0 to +%d; after 400 ms:\n", base);
    Sleep(400);
    show(m, cam, base);
    m.writeBytes(cam + (uint64_t)base, &orig304, 4);
    Sleep(250);

    // B: marker into +316
    m.write(cam + (uint64_t)base + 12, v);
    printf("B: wrote 2.0 to +%d; after 400 ms:\n", base + 12);
    Sleep(400);
    show(m, cam, base);
    m.writeBytes(cam + (uint64_t)base + 12, &orig316, 4);
    Sleep(250);
    printf("restored:\n");
    show(m, cam, base);
    return 0;
}
