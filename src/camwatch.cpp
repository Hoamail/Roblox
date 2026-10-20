// CamWatch: live monitor for manual verification (no game interaction).
// Usage: ext_camwatch.exe [seconds, default 90]
// Walk in game / rotate camera and watch CamPos change.
#include "offsets.h"
#include "sdk.h"
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    int secs = argc > 1 ? atoi(argv[1]) : 90;
    if (secs <= 0 || secs > 600) secs = 90;

    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe") || !m.open()) {
        printf("FAIL: cannot open game process\n");
        return 1;
    }
    std::string running = m.exeVersion();
    printf("running build: %s\n", running.c_str());
    Offsets o;
    std::string log, cache = "offsets_cache.json";
    if (!OffsetManager::ensure(running, cache, o, log)) {
        printf("%sFAIL (offsets)\n", log.c_str());
        return 1;
    }
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) {
        printf("FAIL: module base\n");
        return 1;
    }
    printf("Move in game / rotate camera. Numbers must change. Ctrl+C to stop.\n");
    for (int i = 0; i < secs; ++i) {
        uint64_t dm = 0, ws = 0, cam = 0, place = 0;
        Vec3 v;
        bool ok = sdk.dataModel(dm) && sdk.placeId(dm, place) &&
                  sdk.workspace(dm, ws) && sdk.camera(ws, cam) &&
                  sdk.cameraPos(cam, v);
        if (ok)
            printf("[%02ds] PlaceId=%llu  CamPos=%8.1f %8.1f %8.1f\n",
                   i, (unsigned long long)place, v.x, v.y, v.z);
        else
            printf("[%02ds] READ FAIL (game loading / left?)\n", i);
        fflush(stdout);
        Sleep(1000);
    }
    return 0;
}
