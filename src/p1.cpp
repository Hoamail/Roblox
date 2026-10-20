// P1 verify: walk DataModel->Workspace->Camera + PlaceId from live process.
// Flow: open game -> detect its exact build -> pin offsets to that build.
// Read-only. If the handle is denied, each step reports cleanly.
#include "offsets.h"
#include "sdk.h"
#include <cstdio>
#include <string>

int main() {
    SetConsoleOutputCP(CP_UTF8);
    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe")) {
        printf("P1 FAIL: game not running\n");
        return 1;
    }
    printf("pid=%lu\n", m.pid);
    if (!m.open()) {
        printf("P1 BLOCKED: OpenProcess denied (err=%lu).\n", GetLastError());
        return 2;
    }
    std::string running = m.exeVersion();
    printf("running build: %s\n", running.c_str());

    Offsets o;
    std::string log, cache = "offsets_cache.json";
    if (!OffsetManager::ensure(running, cache, o, log)) {
        printf("%sP1 FAIL (offsets)\n", log.c_str());
        return 1;
    }
    printf("%s", log.c_str());

    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) {
        printf("P1 FAIL: module snapshot denied\n");
        return 1;
    }
    printf("base=0x%llx\n", (unsigned long long)sdk.base);
    uint64_t dm = 0, ws = 0, cam = 0;
    if (!sdk.dataModel(dm)) { printf("P1 FAIL: DataModel chain\n"); return 1; }
    printf("DataModel=0x%llx\n", (unsigned long long)dm);
    uint64_t place = 0, game = 0;
    if (sdk.placeId(dm, place)) printf("PlaceId=%llu\n", (unsigned long long)place);
    if (sdk.gameId(dm, game)) printf("GameId =%llu\n", (unsigned long long)game);
    if (!sdk.workspace(dm, ws)) { printf("P1 FAIL: Workspace\n"); return 1; }
    printf("Workspace=0x%llx\n", (unsigned long long)ws);
    if (!sdk.camera(ws, cam)) { printf("P1 FAIL: Camera\n"); return 1; }
    printf("Camera=0x%llx\n", (unsigned long long)cam);
    Vec3 v;
    if (sdk.cameraPos(cam, v)) printf("CamPos=%.1f %.1f %.1f\n", v.x, v.y, v.z);
    printf("P1 OK\n");
    return 0;
}
