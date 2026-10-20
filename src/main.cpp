// P0 demo: fetch theo's offsets (with cache), detect local Roblox build,
// print the resolved table. No process handles opened yet (P1).
#include "offsets.h"
#include "memory.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <string>
#include <algorithm>
#include <vector>

static std::string w2s(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Newest version-*/ dir under Roblox/Versions (no game interaction).
static std::string localRobloxVersion() {
    wchar_t appdata[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appdata)))
        return "";
    std::wstring base = std::wstring(appdata) + L"\\Roblox\\Versions\\version-*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(base.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::vector<std::pair<FILETIME, std::wstring>> vs;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            vs.emplace_back(fd.ftLastWriteTime, fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (vs.empty()) return "";
    std::sort(vs.begin(), vs.end(), [](auto& a, auto& b) {
        return CompareFileTime(&a.first, &b.first) > 0;
    });
    return w2s(vs.front().second);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::string running = localRobloxVersion();
    printf("local Roblox build : %s\n", running.c_str());

    Memory mem;
    printf("RobloxPlayerBeta present: %s\n",
           mem.findProcess(L"RobloxPlayerBeta.exe") ? "yes" : "no");

    Offsets o;
    std::string log;
    std::string cache = "offsets_cache.json";
    if (!OffsetManager::ensure(running, cache, o, log)) {
        printf("%s", log.c_str());
        printf("P0 FAIL\n");
        return 1;
    }
    printf("%s", log.c_str());
    printf("--- resolved offsets ---\n");
    printf("robloxVersion          %s\n", o.robloxVersion.c_str());
    printf("TaskScheduler.Pointer  %llu\n", (unsigned long long)o.taskSchedulerPtr);
    printf("DataModel.Workspace    %d\n", o.dataModelWorkspace);
    printf("Workspace.CurrentCamera %d\n", o.workspaceCurrentCamera);
    printf("Player.LocalPlayer     %d\n", o.playerLocalPlayer);
    printf("Humanoid.Health        %d\n", o.humanoidHealth);
    printf("Humanoid.MaxHealth     %d\n", o.humanoidMaxHealth);
    printf("VisualEngine.ViewMatrix %d\n", o.visualEngineViewMatrix);
    printf("Instance.ChildrenStart/End %d/%d  Name %d\n",
           o.instanceChildrenStart, o.instanceChildrenEnd, o.instanceName);
    printf("P0 OK\n");
    return 0;
}
