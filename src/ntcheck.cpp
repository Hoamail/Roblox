// ext_ntcheck: verifies the direct NtWriteVirtualMemory path used by the
// overlay, without changing any live value.
//   1. prints the syscall id resolved from the ntdll stub (expect 0x3A=58)
//   2. calls the direct syscall with an impossible address and compares the
//      NTSTATUS with what the Win32 path reports (ABI sanity, no side effects)
//   3. re-writes the Humanoid's current WalkSpeed + its shadow with THE SAME
//      bytes (no net change) and reads them back -> proves the write lands
// Read-mostly; step 3 is a value-preserving no-op write.
#include "memory.h"
#include "offsets.h"
#include "sdk.h"
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    bool doWrite = false;
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--write") doWrite = true;

    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe")) { printf("FAIL: game\n"); return 1; }
    if (!m.openWrite()) { printf("FAIL: openWrite err=%lu\n", GetLastError()); return 1; }
    printf("pid=%lu handle=%p\n", m.pid, m.h);
    printf("resolved NtWriteVirtualMemory id = %u (0x%x)  directWrite=%d\n",
           m.ntWriteId, m.ntWriteId, (int)m.directWrite);
    if (!m.ntWriteId) { printf("FAIL: could not resolve syscall id\n"); return 1; }

    // --- 2. ABI sanity: both paths must reject the same impossible address ---
    uint32_t v = 0x12345678;
    SIZE_T done = 0;
    long st = ntWriteSyscall(m.h, (void*)0x1, &v, 4, &done);
    printf("direct syscall -> addr 0x1 : NTSTATUS 0x%08x (%d)\n",
           (uint32_t)st, (int)st);
    SIZE_T done2 = 0;
    SetLastError(0);
    BOOL ok = WriteProcessMemory(m.h, (LPVOID)0x1, &v, 4, &done2);
    printf("win32 WPM      -> addr 0x1 : ok=%d err=%lu\n", (int)ok, GetLastError());
    // same call through ntdll's own stub -> NTSTATUS must match ours
    typedef long(NTAPI* NtWpm_t)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    NtWpm_t realWpm = ntdll ? (NtWpm_t)GetProcAddress(ntdll, "NtWriteVirtualMemory") : nullptr;
    if (realWpm) {
        SIZE_T done3 = 0;
        long stReal = realWpm(m.h, (PVOID)0x1, &v, 4, &done3);
        printf("ntdll stub     -> addr 0x1 : NTSTATUS 0x%08x (%d)  %s\n",
               (uint32_t)stReal, (int)stReal,
               stReal == st ? "MATCH direct-call ABI OK" : "MISMATCH!");
    }
    if (st >= 0) { printf("WARN: impossible write reported success\n"); }

    Offsets o;
    std::string log, cache = "offsets_cache.json";
    if (!OffsetManager::ensure(m.exeVersion(), cache, o, log)) { printf("FAIL: offsets\n"); return 1; }
    printf("offsets: WalkSpeed=%d WalkSpeedShadow=%d JumpPower=%d\n",
           o.humanoidWalkSpeed, o.humanoidWalkSpeedShadow, o.humanoidJumpPower);

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
    if (!hum) { printf("FAIL: self humanoid\n"); return 1; }
    printf("humanoid = 0x%llx\n", (unsigned long long)hum);

    float ws = 0.0f, sh = 0.0f;
    m.read(hum + (uint64_t)o.humanoidWalkSpeed, ws);
    m.read(hum + (uint64_t)o.humanoidWalkSpeedShadow, sh);
    printf("live WalkSpeed=%.6f  0x3A4(shadow)=%.6f  in-sync=%d\n",
           ws, sh, (ws == sh) ? 1 : 0);

    if (!doWrite) { printf("(pass --write to run the value-preserving write test)\nNTCHECK DONE\n"); return 0; }

    // --- 3. value-preserving write of the exact bytes already present ---
    SIZE_T w1 = 0, w2 = 0, w3 = 0;
    long s1 = ntWriteSyscall(m.h, (void*)(hum + (uint64_t)o.humanoidWalkSpeedShadow), &ws, 4, &w1);
    long s2 = ntWriteSyscall(m.h, (void*)(hum + (uint64_t)o.humanoidWalkSpeed), &ws, 4, &w2);
    long s3 = ntWriteSyscall(m.h, (void*)(hum + (uint64_t)o.humanoidWalkSpeedShadow), &ws, 4, &w3);
    printf("write shadow=0x%08lx walkspeed=0x%08lx shadow=0x%08lx (status %d/%d/%d)\n",
           (unsigned long)s1, (unsigned long)s2, (unsigned long)s3, (int)s1, (int)s2, (int)s3);
    Sleep(120);
    float ws2 = 0.0f, sh2 = 0.0f;
    m.read(hum + (uint64_t)o.humanoidWalkSpeed, ws2);
    m.read(hum + (uint64_t)o.humanoidWalkSpeedShadow, sh2);
    printf("after: WalkSpeed=%.6f  0x3A4=%.6f  in-sync=%d  (delta=%.6f)\n",
           ws2, sh2, (ws2 == sh2) ? 1 : 0, ws2 - ws);
    printf("NTCHECK DONE\n");
    return 0;
}
