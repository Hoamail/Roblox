// ext_calib: samples Camera+200 (Rotation mystery bytes) + CamPos over 60s.
// Usage: run, then in game SLOWLY: look left 360deg, look straight UP,
// straight DOWN, then stand still 5s. Send calib.log back.
// Read-only except zero game effect (pure reads).
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
    // Rotation offset from live offsets (fallback baked into struct).
    int rotOff = o.cameraRotation;
    FILE* f = fopen("calib.log", "w");
    if (!f) return 1;
    fprintf(f, "# t(s) camXYZ rot[16 floats as f0..f15]\n");
    for (int i = 0; i < 120; ++i) {
        uint64_t dm = 0, ws = 0, cam = 0;
        Vec3 v{};
        float r[16]{};
        bool ok = sdk.dataModel(dm) && sdk.workspace(dm, ws) &&
                  sdk.camera(ws, cam) && sdk.cameraPos(cam, v);
        if (ok) {
            for (int k = 0; k < 16; ++k) m.read(cam + (uint64_t)rotOff + (uint64_t)k * 4, r[k]);
            fprintf(f, "%d %.1f %.1f %.1f", i / 2, v.x, v.y, v.z);
            for (int k = 0; k < 16; ++k) fprintf(f, " %.4f", r[k]);
            fprintf(f, "\n");
        } else {
            fprintf(f, "%d READFAIL\n", i / 2);
        }
        fflush(f);
        Sleep(500);
    }
    fclose(f);
    printf("done -> calib.log\n");
    return 0;
}
