// ext_wpmtest: proves WriteProcessMemory rights WITHOUT changing gameplay.
// Reads Camera.FieldOfView, writes back the IDENTICAL value, reads again.
// Verdict tells whether memory-aim (mode 1) is even possible on this setup.
#include "offsets.h"
#include "sdk.h"
#include <cstdio>
#include <string>

int main() {
    SetConsoleOutputCP(CP_UTF8);
    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe")) {
        printf("FAIL: game not running\n");
        return 1;
    }
    if (!m.openWrite()) {
        printf("WPM DENIED (err=%lu): memory-aim impossible, mouse mode only.\n",
               GetLastError());
        return 2;
    }
    printf("write handle: OK\n");
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
    uint64_t dm = 0, ws = 0, cam = 0;
    if (!sdk.dataModel(dm) || !sdk.workspace(dm, ws) || !sdk.camera(ws, cam)) {
        printf("FAIL chain\n");
        return 1;
    }
    // Camera.FieldOfView: write back the identical value (no-op semantically).
    float fov0 = 0, fov1 = 0;
    if (!m.read(cam + (uint64_t)o.cameraFOV, fov0)) {
        printf("FAIL: cannot read FOV\n");
        return 1;
    }
    printf("FOV before: %.2f\n", fov0);
    if (!m.write(cam + (uint64_t)o.cameraFOV, fov0)) {
        printf("WPM WRITE FAILED (err=%lu): reads OK but writes blocked.\n",
               GetLastError());
        return 2;
    }
    m.read(cam + (uint64_t)o.cameraFOV, fov1);
    printf("FOV after : %.2f\n", fov1);
    printf("WPM VERDICT: %s (wrote identical value, zero gameplay effect)\n",
           (fov0 == fov1) ? "WRITE WORKS" : "MISMATCH");
    return 0;
}
