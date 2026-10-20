// dumpdata.cpp -- READ-ONLY runtime dump of velext_v1.exe .data / .rdata.
//
// Purpose (P1a of PLAN_speed_jump.md):
//   velext_v1.exe keeps its Roblox offsets in a .data table initialised to
//   0xDEADBEEF and filled at runtime. Static analysis can't see those values,
//   so while velext is running (speed/jump on) we read them live and map
//   slot -> value. Nothing is written to the target process.
//
// Usage: ext_dumpdata.exe [procName] [outPrefix]
//   default procName = velext_v1.exe, outPrefix = velext_dump

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static HANDLE g_h = nullptr;

static bool rd(uint64_t addr, void* buf, size_t n) {
    SIZE_T r = 0;
    if (!ReadProcessMemory(g_h, (LPCVOID)addr, buf, n, &r)) return false;
    return r == n;
}
static bool rd8(uint64_t addr, uint64_t& v)      { return rd(addr, &v, 8); }
static bool rd32(uint64_t addr, uint32_t& v)     { return rd(addr, &v, 4); }

// Read a NUL-terminated ASCII string but never more than maxlen.
static std::string rdstr(uint64_t addr, size_t maxlen = 96) {
    char buf[128];
    size_t n = maxlen < sizeof(buf) - 1 ? maxlen : sizeof(buf) - 1;
    if (!rd(addr, buf, n)) return "";
    buf[n] = 0;
    for (size_t i = 0; i < n; i++)
        if (buf[i] == 0) { n = i; break; }
    for (size_t i = 0; i < n; i++)
        if (buf[i] < 0x20 || buf[i] > 0x7e) return "";   // not printable
    return std::string(buf, n);
}

static void enableDebugPriv() {
    HANDLE t = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t))
        return;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(t, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    CloseHandle(t);
}

static DWORD findPid(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(name, pe.szExeFile) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static uint64_t mainModuleBase(HANDLE h, const wchar_t* name) {
    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(h, mods, sizeof(mods), &needed, LIST_MODULES_ALL))
        return 0;
    DWORD cnt = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < cnt; i++) {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameExW(h, mods[i], path, MAX_PATH)) continue;
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        if (_wcsicmp(base, name) == 0) return (uint64_t)mods[i];
    }
    return cnt ? (uint64_t)mods[0] : 0;
}

struct Sec { const char* nm; uint64_t rva; size_t sz; };

int wmain(int argc, wchar_t** argv) {
    const wchar_t* proc = argc > 1 ? argv[1] : L"velext_v1.exe";
    std::string prefix = "velext_dump";
    if (argc > 2) {
        char tmp[256]{}; size_t k = 0;
        for (const wchar_t* p = argv[2]; *p && k < sizeof(tmp) - 1; p++) tmp[k++] = (char)*p;
        prefix = tmp;
    }

    enableDebugPriv();
    DWORD pid = findPid(proc);
    if (!pid) { printf("[!] process not found: %ls\n", proc); return 1; }
    printf("pid = %lu\n", pid);

    g_h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
                      FALSE, pid);
    if (!g_h) {
        DWORD e = GetLastError();
        g_h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        printf("[!] OpenProcess(VMR|QI) failed (%lu); limited handle: %s\n", e,
               g_h ? "ok" : "FAILED");
    }
    if (!g_h) { printf("[!] cannot open target, error %lu\n", GetLastError()); return 2; }

    uint64_t base = mainModuleBase(g_h, proc);
    printf("image base = 0x%llx\n", (unsigned long long)base);
    if (!base) base = 0x140000000ULL;   // static base fallback

    // --- syscall-id cache globals (see velext_P0_findings.md) -----------------
    const uint64_t g_rpm = 0x140332a50, g_wpm = 0x140332a20;
    uint32_t rpm_id = 0, wpm_id = 0;
    rd32(base + (g_rpm - 0x140000000), rpm_id);
    rd32(base + (g_wpm - 0x140000000), wpm_id);
    printf("syscall id  a50(NtReadVirtualMemory)  = %d (0x%x)%s\n", (int)rpm_id, rpm_id,
           rpm_id == 0xffffffff ? " [unresolved]" : "");
    printf("syscall id  a20(NtWriteVirtualMemory) = %d (0x%x)%s\n", (int)wpm_id, wpm_id,
           wpm_id == 0xffffffff ? " [unresolved]" : "");

    // --- dump sections -------------------------------------------------------
    Sec secs[] = {
        {"text",  0x1000,   0x2576B3},
        {"rdata", 0x259000, 0xD3656},
        {"data",  0x32D000, 0x12816},
    };
    for (auto& s : secs) {
        std::vector<uint8_t> buf(s.sz);
        if (!rd(base + s.rva, buf.data(), s.sz)) {
            printf("[!] read .%s FAILED (err %lu)\n", s.nm, GetLastError());
            continue;
        }
        std::string fn = prefix + "_" + s.nm + ".bin";
        FILE* f = fopen(fn.c_str(), "wb");
        if (f) { fwrite(buf.data(), 1, buf.size(), f); fclose(f); }
        printf("[+] .%-5s base+rva 0x%llx size 0x%zx -> %s\n", s.nm,
               (unsigned long long)(base + s.rva), s.sz, fn.c_str());
    }

    // --- analyse the offset table in .data ----------------------------------
    // .data occupies RVA 0x32D000..0x33F816. Entries start as 0xDEADBEEF|0.
    const uint64_t dataRva = 0x32D000, dataEnd = 0x33F816;
    printf("\n=== .data 8-byte slots that are NOT 0xDEADBEEF/0 (0x14032D000-0x14032E800) ===\n");
    int shown = 0;
    for (uint64_t rva = 0x32D000; rva + 8 <= 0x32E800; rva += 8) {
        uint64_t v = 0;
        if (!rd8(base + rva, v)) continue;
        uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
        if (lo == 0xDEADBEEF && hi == 0) continue;
        if (lo == 0 && hi == 0) continue;
        char extra[160] = "";
        if (lo >= 0x140000000 && lo < 0x140600000) {
            std::string s = rdstr(base + (lo - 0x140000000) + 0x0);  // lo is a VA
            if (!s.empty()) snprintf(extra, sizeof(extra), "  -> \"%s\" (ptr+0x0)", s.c_str());
            else snprintf(extra, sizeof(extra), "  -> va 0x%x", lo);
        } else if (lo > 0 && lo < 0x4000) {
            snprintf(extra, sizeof(extra), "  (u32=%u / f=%.6g)", lo, *(float*)&lo);
        }
        printf("  DAT_%llx: 0x%08x 0x%08x%s\n", (unsigned long long)(0x140000000 + rva), lo, hi, extra);
        if (++shown > 400) { printf("  ... (truncated)\n"); break; }
    }
    printf("  (%d populated slots)\n", shown);

    // --- hunt specific Humanoid/BasePart offsets everywhere in the dump ------
    struct Want { const char* nm; uint32_t v; };
    Want wants[] = {
        {"Humanoid.Walkspeed",     0x1CC}, {"Humanoid.WalkspeedCheck", 0x3A4},
        {"Humanoid.JumpPower",     0x1A0}, {"Humanoid.JumpHeight",     0x19C},
        {"Humanoid.UseJumpPower",  0x1DD}, {"Humanoid.Health",         0x18C},
        {"Humanoid.MoveDirection", 0x13C}, {"Humanoid.HumanoidRootPart",0x468},
        {"Humanoid.State",         0x8B0}, {"BasePart.Primitive",      0x180},
        {"Primitive.Position",     0xD4},  {"Primitive.Velocity",      0xE0},
        {"Primitive.Size",         0x1C4}, {"Player.LocalPlayer",      0x120},
        {"Player.ModelInstance",   0x298}, {"Workspace.CurrentCamera", 0x4A8},
    };
    printf("\n=== 4-byte hits of known Roblox offsets inside .data/.rdata dump ===\n");
    for (auto& w : wants) {
        printf("  %-26s (0x%x):", w.nm, w.v);
        int hits = 0;
        for (uint64_t rva = 0x32D000; rva + 8 <= dataEnd; rva += 8) {
            uint64_t v = 0;
            if (!rd8(base + rva, v)) continue;
            if ((uint32_t)v == w.v || (uint32_t)(v >> 32) == w.v) {
                printf(" @DAT_%llx", (unsigned long long)(0x140000000 + rva));
                if (++hits >= 6) { printf(" ..."); break; }
            }
        }
        if (!hits) printf(" none");
        printf("\n");
    }

    printf("\ndone.\n");
    return 0;
}
