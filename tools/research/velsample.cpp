// velsample.cpp — PHASE B read-only sampler (velext_v1 vs Roblox).
//
// TUYET DOI READ-ONLY: khong co API ghi nao duoc goi. Chi doc.
//   - velext : NtReadVirtualMemory qua syscall truc tiep (kieu ext_dumpdata);
//              fallback ReadProcessMemory neu khong resolve duoc syscall id.
//   - Roblox : ReadProcessMemory (giong ext_watch / ext_dumpdata).
//
// Muc dich: thu thap trace de chung minh (B) model smoothing,
// (C) bang slider that, (D) 4 an so (sensitivity / frame limiter / targetPart /
// duong actuation). Moi mau co QPC timestamp (ns).
//
// Usage:
//   velsample.exe [--sec=10] [--rate=2000] [--csv=aim_work\velsample.csv]
//                 [--log=aim_work\velsample.log] [--delay=2]
//                 [--smooth=5.0] [--aimbase=0x...] [--mouse=0x...]
//                 [--dumpblock] [--allblocks] [--svcscan[=substr]]
//                 [--nomouse] [--force-base] [--robloxproc=...] [--velproc=...]
//   --dumpblock : in hex khoi settings cua velext + vec begin/end/cap + cac block
//   --svcscan   : liet ke con truc tiep cua DataModel (name/class/addr)
//   --help      : in usage, khong cham vao process nao.
//
// Build (giong ext_watch):
//   g++ -std=c++17 -O2 -static -o velsample.exe aim_work\velsample.cpp
//       src\offsets.cpp -lwinhttp -lshell32 -lpsapi

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cmath>
#include <string>
#include <vector>
#include "offsets.h"
#include "sdk.h"

// ---------------------------------------------------------------------------
// Raw NtReadVirtualMemory syscall (giu nguyen kieu memory.h lam voi Write).
// ---------------------------------------------------------------------------
extern "C" {
__attribute__((used)) static uint32_t g_ntReadId = 0;
long ntReadSyscallRaw(void* handle, void* addr, void* buf, size_t size, size_t* read);
}
__asm__(
    ".text\n"
    ".globl ntReadSyscallRaw\n"
    ".def ntReadSyscallRaw; .scl 2; .type 32; .endef\n"
    "ntReadSyscallRaw:\n"
    "    movl g_ntReadId(%rip), %eax\n"
    "    movq %rcx, %r10\n"
    "    syscall\n"
    "    ret\n");

// ---------------------------------------------------------------------------
// Small utils
// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;
static bool g_quiet = false;
static void slog(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!g_quiet) { fputs(buf, stdout); fflush(stdout); }
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
}
// QuickEdit: neu nguoi dung click/select vao console, moi lenh ghi console se
// block vo han -> sampler dung hinh. Tat QuickEdit ngay khi khoi dong.
static void disableQuickEdit() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(hIn, &mode)) {
        mode &= ~ENABLE_QUICK_EDIT_MODE;
        mode |= ENABLE_EXTENDED_FLAGS;
        SetConsoleMode(hIn, mode);
    }
}
static void progLine(uint64_t n, double hz, uint64_t cf, uint64_t vf) {
    if (g_log) {
        fprintf(g_log, "[progress] samples=%llu hz=%.0f camfail=%llu velfail=%llu\n",
                (unsigned long long)n, hz, (unsigned long long)cf, (unsigned long long)vf);
        fflush(g_log);
    }
    if (!g_quiet) {
        printf("\r[samples=%llu hz=%.0f camfail=%llu velfail=%llu]      ",
               (unsigned long long)n, hz, (unsigned long long)cf, (unsigned long long)vf);
        fflush(stdout);
    }
}

static uint64_t qpcTicks() {
    LARGE_INTEGER l; QueryPerformanceCounter(&l); return (uint64_t)l.QuadPart;
}
static uint64_t qpcFreqVal() {
    LARGE_INTEGER l; QueryPerformanceFrequency(&l); return (uint64_t)l.QuadPart;
}
// exact ticks -> ns without 64-bit overflow
static uint64_t ticksToNs(uint64_t t, uint64_t f) {
    return (t / f) * 1000000000ull + (t % f) * 1000000000ull / f;
}

static void enableDebugPriv() {
    HANDLE t = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t))
        return;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    if (LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &tp.Privileges[0].Luid)) {
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(t, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    CloseHandle(t);
}

static bool isElevated() {
    HANDLE t = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    TOKEN_ELEVATION el{};
    DWORD sz = sizeof(el);
    bool ok = GetTokenInformation(t, TokenElevation, &el, sz, &sz) && el.TokenIsElevated;
    CloseHandle(t);
    return ok;
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
    return 0;
}

// ---------------------------------------------------------------------------
// XMem: reader for one process. tries raw syscall first, falls back to RPM.
// ---------------------------------------------------------------------------
class XMem {
public:
    DWORD pid = 0;
    HANDLE h = nullptr;
    bool directRead = false;
    bool syscallWorked = false, rpmWorked = false;
    long long failCount = 0, okCount = 0;

    static void resolveReadSyscall() {
        if (g_ntReadId) return;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return;
        const unsigned char* p =
            (const unsigned char*)GetProcAddress(ntdll, "NtReadVirtualMemory");
        if (!p) return;
        uint32_t id = 0;
        for (int i = 0; i < 64; i++) {
            const unsigned char* q = p + i;
            if (q[0] == 0x4C && q[1] == 0x8B && q[2] == 0xD1 && q[3] == 0xB8) {
                id = *(const uint32_t*)(q + 4);
                break;
            }
        }
        if (!id) {  // stub patched: any "mov eax,id; syscall"
            for (int i = 0; i < 64; i++) {
                const unsigned char* q = p + i;
                if (q[0] == 0xB8 && q[5] == 0x0F && q[6] == 0x05) {
                    id = *(const uint32_t*)(q + 1);
                    break;
                }
            }
        }
        if (id) g_ntReadId = id;
    }

    bool findAndOpen(const wchar_t* name) {
        pid = findPid(name);
        if (!pid) return false;
        h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION |
                            PROCESS_QUERY_LIMITED_INFORMATION,
                        FALSE, pid);
        if (!h)
            h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return false;
        resolveReadSyscall();
        directRead = g_ntReadId != 0;
        return true;
    }

    bool readRaw(uint64_t addr, void* buf, size_t n) {
        if (directRead) {
            SIZE_T got = 0;
            long st = ntReadSyscallRaw(h, (void*)addr, buf, n, &got);
            if (st == 0 && got == n) { syscallWorked = true; okCount++; return true; }
        }
        SIZE_T got = 0;
        if (ReadProcessMemory(h, (LPCVOID)addr, buf, n, &got) && got == n) {
            rpmWorked = true; okCount++;
            return true;
        }
        failCount++;
        return false;
    }
    template <typename T>
    bool read(uint64_t addr, T& out) {
        return readRaw(addr, &out, sizeof(T));
    }
    ~XMem() { if (h) CloseHandle(h); }
};

// raw RPM on the Roblox Memory handle (Memory::read is templated per-type)
static bool mread(Memory& m, uint64_t addr, void* dst, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(m.h, (LPCVOID)addr, dst, n, &got) && got == n;
}

static inline uint32_t u32at(const uint8_t* b, size_t o) {
    uint32_t v = 0; memcpy(&v, b + o, 4); return v;
}
static inline uint64_t u64at(const uint8_t* b, size_t o) {
    uint64_t v = 0; memcpy(&v, b + o, 8); return v;
}
static inline float f32at(const uint8_t* b, size_t o) {
    float v = 0; memcpy(&v, b + o, 4); return v;
}

// ---------------------------------------------------------------------------
// Args
// ---------------------------------------------------------------------------
struct Args {
    double sec = 10.0;
    int rate = 2000;
    double delay = 2.0;
    std::string csv = "aim_work\\velsample.csv";
    std::string logPath;
    double smoothExpected = 5.0;
    bool smoothAuto = false;  // --smooth=auto: chap nhan gia tri live neu hop ly
    uint64_t aimBaseOverride = 0;
    uint64_t mouseOverride = 0;
    int blockIdx = -1;  // --block=N (0-based) chon profile block
    bool dumpBlock = false, allBlocks = false, svcScan = false;
    std::string svcFilter;
    bool noMouse = false, forceBase = false, help = false;
    bool quiet = false;  // --quiet: khong ghi console trong luc do (tranh QuickEdit)
    std::wstring robloxProc = L"RobloxPlayerBeta.exe";
    std::wstring velProc = L"velext_v1.exe";
};
static bool hasPrefix(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }
static Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        const char* s = argv[i];
        if (!strcmp(s, "--help") || !strcmp(s, "-h")) a.help = true;
        else if (hasPrefix(s, "--sec=")) a.sec = atof(s + 6);
        else if (hasPrefix(s, "--rate=")) a.rate = atoi(s + 7);
        else if (hasPrefix(s, "--delay=")) a.delay = atof(s + 8);
        else if (hasPrefix(s, "--csv=")) a.csv = s + 6;
        else if (hasPrefix(s, "--log=")) a.logPath = s + 6;
        else if (hasPrefix(s, "--smooth=")) {
            const char* v = s + 9;
            if (!strcmp(v, "auto")) a.smoothAuto = true;
            else a.smoothExpected = atof(v);
        }
        else if (hasPrefix(s, "--aimbase=")) a.aimBaseOverride = strtoull(s + 10, nullptr, 0);
        else if (hasPrefix(s, "--block=")) a.blockIdx = atoi(s + 8);
        else if (hasPrefix(s, "--mouse=")) a.mouseOverride = strtoull(s + 8, nullptr, 0);
        else if (!strcmp(s, "--dumpblock")) a.dumpBlock = true;
        else if (!strcmp(s, "--allblocks")) a.allBlocks = true;
        else if (hasPrefix(s, "--svcscan")) {
            a.svcScan = true;
            if (s[9] == '=') a.svcFilter = s + 10;
        } else if (!strcmp(s, "--nomouse")) a.noMouse = true;
        else if (!strcmp(s, "--force-base")) a.forceBase = true;
        else if (!strcmp(s, "--quiet")) a.quiet = true;
        else if (hasPrefix(s, "--robloxproc=")) {
            std::string t = s + 13; a.robloxProc.assign(t.begin(), t.end());
        } else if (hasPrefix(s, "--velproc=")) {
            std::string t = s + 10; a.velProc.assign(t.begin(), t.end());
        } else {
            slog("[!] unknown arg: %s\n", s);
        }
    }
    if (a.rate < 100) a.rate = 100;
    if (a.rate > 20000) a.rate = 20000;
    if (a.sec <= 0 || a.sec > 3600) a.sec = 10;
    if (a.delay < 0 || a.delay > 60) a.delay = 2;
    return a;
}

static void usage() {
    slog(
        "velsample.exe — PHASE B read-only sampler (velext vs Roblox).\n"
        "  --sec=N        thoi luong mau (giay, default 10)\n"
        "  --rate=N       tan so muc tieu Hz (default 2000, >=500 theo spec)\n"
        "  --delay=N      cho N giay truoc khi lay mau (default 2)\n"
        "  --csv=PATH     file CSV (default aim_work\\velsample.csv)\n"
        "  --log=PATH     tee console log ra file\n"
        "  --smooth=5.0   gia tri aim.smoothing ky vong de verify base\n"
        "                 (--smooth=auto: doc gia tri live, chi kiem tra hop ly)\n"
        "  --aimbase=0x.. override base khoi aim (bo qua verify)\n"
        "  --block=N      chon profile block N (0-based) thay cho last-block\n"
        "  --mouse=0x..   override dia chi MouseService\n"
        "  --dumpblock    in hex khoi settings + profile blocks roi thoat\n"
        "  --allblocks    (voi --dumpblock) in het cac profile block\n"
        "  --svcscan[=S]  liet ke con DataModel (loc theo chuoi S) roi thoat\n"
        "  --nomouse      khong resolve MouseService/sensitivity\n"
        "  --force-base   van lay mau du verify base that bai\n"
        "  --quiet        khong ghi console trong luc do (chi ghi --log)\n"
        "  --help         in usage (khong cham process)\n");
}

// ---------------------------------------------------------------------------
// Velext globals: VA (image base 0x140000000) -> RVA
// ---------------------------------------------------------------------------
static const uint64_t IMG_BASE = 0x140000000ull;
#define VE_RVA(va) ((uint64_t)(va) - IMG_BASE)

static const uint64_t VA_VEC_BEGIN = 0x140334E88;  // vector begin ptr
static const uint64_t VA_VEC_END = 0x140334E90;    // vector end ptr (base = end-0x1E8)
static const uint64_t VA_VEC_CAP = 0x140334E98;

static const int BLOCK_SIZE = 0x208;   // profile block
static const int AIMOFF_BASE = 0x1E8;  // base = vecEnd - AIMOFF_BASE == block+0x20
static const int AIMBLK_LEN = 0x1E8;

// aim-relative offsets (per scan_aimoffs.py / RE findings)
static const int A_VISIBLE = 0x89, A_ENABLED = 0x9C, A_METHOD = 0xA0, A_KEY = 0xA4,
                 A_KEYMODE = 0xA8, A_IGTEAM = 0xAC, A_IGDOWN = 0xAD, A_FOV = 0xB0,
                 A_USEFOV = 0xB4, A_MAXD = 0xB8, A_SMOOTH = 0xBC, A_STICKY = 0xC0,
                 A_PRED = 0xC1, A_PREDSCALE = 0xC4, A_TPART = 0xC8, A_PRIO = 0xCC,
                 A_DRAWFOV = 0xD0, A_DRAWTGT = 0xD1;

// scattered velext ranges (VA, len)
struct VRange { uint64_t va; size_t len; const char* nm; };
static const VRange VRANGES[] = {
    { 0x140334240, 0x18, "g0_yaw"   },  // 0x334248 last yaw, 0x33424C, 0x334250
    { 0x140334390, 0x90, "g1_stamp" },  // dedup 0x390, pitch 0x3AC, plan 3B0/3C0/3D0,
                                        // stamp 0x408, bpstamp 0x418
    { 0x140334880, 0x20, "g2_inj"   },  // obj ptr 0x334888, roblox base 0x334890
    { 0x140332C40, 0x30, "g3_flags" },  // 0x332C5A mouse-step, 0x332C5B aim job gate
    { 0x140339F40, 0x10, "g4_fps"   },  // 0x339F48 ovl.renderFps
    { 0x14033B478, 0x18, "g5_uncap" },  // 0x33B480 ovl.renderUncap
};
static const int NRANGES = (int)(sizeof(VRANGES) / sizeof(VRANGES[0]));

// Roblox sensitivity RVAs (float pairs)
static const uint64_t RB_SENS_AB = 0x8D857D8ull;  // A = {0,4}, B = {8,12}
static const uint64_t RB_SENS_C = 0x8D85820ull;   // C = {0,4}

// ---------------------------------------------------------------------------
// CSV columns: one macro list keeps header and row format in sync.
// ---------------------------------------------------------------------------
#define ROW_FIELDS(X)                                                        \
    X(t_ns,          "%llu", (unsigned long long)tns)                        \
    X(cam_ok,        "%u",   (unsigned)camOk)                                \
    X(cam,           "%llu", (unsigned long long)cam)                        \
    X(px,            "%.4f", px) X(py, "%.4f", py) X(pz, "%.4f", pz)         \
    X(px2,           "%.4f", px2) X(py2, "%.4f", py2) X(pz2, "%.4f", pz2)    \
    X(r00, "%.7g", rot[0]) X(r01, "%.7g", rot[1]) X(r02, "%.7g", rot[2])     \
    X(r10, "%.7g", rot[3]) X(r11, "%.7g", rot[4]) X(r12, "%.7g", rot[5])     \
    X(r20, "%.7g", rot[6]) X(r21, "%.7g", rot[7]) X(r22, "%.7g", rot[8])     \
    X(v_ok,          "%u",   (unsigned)vOk)                                  \
    X(v_vecend,      "%llu", (unsigned long long)vecEnd)                     \
    X(v_active,      "%llu", (unsigned long long)activeG)                    \
    X(v_nblocks,     "%u",   (unsigned)nblocks)                              \
    X(v_vis,         "%u",   (unsigned)vis)                                  \
    X(v_en,          "%u",   (unsigned)en)                                   \
    X(v_meth,        "%u",   (unsigned)meth)                                 \
    X(v_key,         "%u",   (unsigned)key)                                  \
    X(v_keymode,     "%u",   (unsigned)keymode)                              \
    X(v_igteam,      "%u",   (unsigned)igteam)                               \
    X(v_igdown,      "%u",   (unsigned)igdown)                               \
    X(v_fovu,        "%u",   (unsigned)fovu)                                 \
    X(v_fovf,        "%.7g", fovf)                                           \
    X(v_usefov,      "%u",   (unsigned)usefov)                               \
    X(v_maxdu,       "%u",   (unsigned)maxdu)                                \
    X(v_maxdf,       "%.7g", maxdf)                                          \
    X(v_smooth,      "%.7g", smooth)                                         \
    X(v_sticky,      "%u",   (unsigned)sticky)                               \
    X(v_pred,        "%u",   (unsigned)pred)                                 \
    X(v_predscale,   "%.7g", predscale)                                      \
    X(v_tpart,       "%u",   (unsigned)tpart)                                \
    X(v_prio,        "%u",   (unsigned)prio)                                 \
    X(v_drawfov,     "%u",   (unsigned)drawfov)                              \
    X(v_drawtgt,     "%u",   (unsigned)drawtgt)                              \
    X(g_ok,          "%u",   (unsigned)gOk)                                  \
    X(v_yaw,         "%.7g", yaw)                                            \
    X(v_u24c,        "%.7g", u24c)                                           \
    X(v_u250,        "%.7g", u250)                                           \
    X(v_dedup,       "%llu", (unsigned long long)dedup)                      \
    X(v_u3a0,        "%u",   (unsigned)u3a0)                                 \
    X(v_u3a4,        "%u",   (unsigned)u3a4)                                 \
    X(v_pitch,       "%.7g", pitch)                                          \
    X(v_pa0, "%.7g", pa0) X(v_pa1, "%.7g", pa1) X(v_pa2, "%.7g", pa2)        \
    X(v_pa3, "%.7g", pa3)                                                    \
    X(v_pb0, "%.7g", pb0) X(v_pb1, "%.7g", pb1) X(v_pb2, "%.7g", pb2)        \
    X(v_pb3, "%.7g", pb3)                                                    \
    X(v_pc0, "%.7g", pc0) X(v_pc1, "%.7g", pc1) X(v_pc2, "%.7g", pc2)        \
    X(v_pc3, "%.7g", pc3)                                                    \
    X(v_stamp,       "%llu", (unsigned long long)stamp)                      \
    X(v_bpstamp,     "%llu", (unsigned long long)bpstamp)                    \
    X(v_rbase,       "%llu", (unsigned long long)rbase)                      \
    X(v_u3898,       "%.7g", u3898)                                          \
    X(v_objptr,      "%llu", (unsigned long long)objptr)                     \
    X(v_mflag,       "%u",   (unsigned)mflag)                                \
    X(v_jgate,       "%u",   (unsigned)jgate)                                \
    X(v_c5c,         "%u",   (unsigned)c5c)                                  \
    X(v_c5d,         "%u",   (unsigned)c5d)                                  \
    X(v_renderfps,   "%u",   (unsigned)renderfps)                            \
    X(v_uncap,       "%u",   (unsigned)uncap)                                \
    X(v_u38484,      "%u",   (unsigned)u38484)                               \
    X(focus,         "%u",   (unsigned)focus)                                \
    X(msvc,          "%llu", (unsigned long long)msvc)                       \
    X(msvc_21c_u,    "%u",   (unsigned)msvc21cu)                             \
    X(msvc_21c_f,    "%.7g", msvc21cf)                                       \
    X(sensA0, "%.7g", sA0) X(sensA1, "%.7g", sA1)                            \
    X(sensB0, "%.7g", sB0) X(sensB1, "%.7g", sB1)                            \
    X(sensC0, "%.7g", sC0) X(sensC1, "%.7g", sC1)                            \
    X(sens_ok,       "%u",   (unsigned)sensOk)                               \
    X(in_src,        "%u",   (unsigned)inSrc)                                \
    X(in_beg,        "%llu", (unsigned long long)inBeg)                      \
    X(in_end,        "%llu", (unsigned long long)inEnd)                      \
    X(in_cap,        "%llu", (unsigned long long)inCap)                      \
    X(in_size,       "%llu", (unsigned long long)inSize)                     \
    X(in_flag,       "%u",   (unsigned)inFlag)                               \
    X(in_u44,        "%u",   (unsigned)inU44)                                \
    X(rec0, "%.7g", rec[0]) X(rec1, "%.7g", rec[1]) X(rec2, "%.7g", rec[2])  \
    X(rec3, "%.7g", rec[3]) X(rec4, "%.7g", rec[4]) X(rec5, "%.7g", rec[5])  \
    X(rec6, "%.7g", rec[6]) X(rec7, "%.7g", rec[7])

#define X_NAME(n, f, a) #n ","
#define X_FMT(n, f, a)  f ","
#define X_ARG(n, f, a)  a,

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------
static void hexDumpBlock(const uint8_t* b, size_t len) {
    for (size_t off = 0; off < len; off += 16) {
        char line[256];
        int n = snprintf(line, sizeof(line), "  +%04zx:", off);
        for (size_t i = 0; i < 16; i++) {
            if (off + i < len)
                n += snprintf(line + n, sizeof(line) - n, " %02x", b[off + i]);
            else
                n += snprintf(line + n, sizeof(line) - n, "   ");
        }
        n += snprintf(line + n, sizeof(line) - n, "  ");
        for (size_t i = 0; i < 16 && off + i < len; i++) {
            uint8_t c = b[off + i];
            n += snprintf(line + n, sizeof(line) - n, "%c",
                          (c >= 32 && c < 127) ? c : '.');
        }
        slog("%s\n", line);
    }
}

// dump + report of the aim block and profile blocks. Returns validated aimBase or 0.
static uint64_t dumpAndValidate(XMem& xt, uint64_t veBase, const Args& a, bool doDump,
                                bool allBlocks) {
    uint64_t vb = 0, ve = 0, vc = 0;
    xt.read(veBase + VE_RVA(VA_VEC_BEGIN), vb);
    xt.read(veBase + VE_RVA(VA_VEC_END), ve);
    xt.read(veBase + VE_RVA(VA_VEC_CAP), vc);
    slog("[i] vec begin=0x%llx end=0x%llx cap=0x%llx\n",
         (unsigned long long)vb, (unsigned long long)ve, (unsigned long long)vc);
    uint64_t nblocks = (ve > vb && ((ve - vb) % BLOCK_SIZE) == 0) ? (ve - vb) / BLOCK_SIZE : 0;
    slog("[i] nblocks=%llu (block=0x%X)\n", (unsigned long long)nblocks, BLOCK_SIZE);
    if (!ve) {
        slog("[!] vec end rong — velext chua load profile?\n");
        return 0;
    }
    // global "active" + frame limiter: giup xac dinh profile block live
    uint64_t activeG = 0;
    xt.read(veBase + VE_RVA(0x140334E80ull), activeG);
    uint32_t liveFps = 0;
    uint8_t liveUncap = 0;
    xt.read(veBase + VE_RVA(0x140339F48ull), liveFps);
    xt.read(veBase + VE_RVA(0x14033B480ull), liveUncap);
    slog("[i] global@0x140334E80=0x%llx (u32=%u) live renderFps=%u uncap=%u\n",
         (unsigned long long)activeG, (unsigned)activeG, liveFps, (unsigned)liveUncap);
    if (nblocks && activeG <= nblocks)
        slog("[i] active neu la index: 0-based=block %llu, 1-based=block %lld\n",
             (unsigned long long)activeG, (long long)activeG - 1);

    uint64_t aimBase = 0;
    const char* aimWhy = "?";
    if (a.aimBaseOverride) {
        aimBase = a.aimBaseOverride;
        aimWhy = "--aimbase";
    } else if (a.blockIdx >= 0) {
        if (nblocks && (uint64_t)a.blockIdx < nblocks) {
            aimBase = vb + (uint64_t)a.blockIdx * BLOCK_SIZE + 0x20;
            aimWhy = "--block";
        } else {
            slog("[!] --block=%d ngoai pham vi (nblocks=%llu)\n", a.blockIdx,
                 (unsigned long long)nblocks);
        }
    } else if (nblocks && activeG >= vb && activeG < ve) {
        uint64_t off = activeG - vb;
        if (off % BLOCK_SIZE == 0x20) { aimBase = activeG; aimWhy = "active-ptr(block+0x20)"; }
        else if (off % BLOCK_SIZE == 0) { aimBase = activeG + 0x20; aimWhy = "active-ptr(block)"; }
    } else if (nblocks && activeG > 0 && activeG <= nblocks) {
        aimBase = vb + activeG * BLOCK_SIZE + 0x20;
        aimWhy = "active-index(0-based)";
    }
    if (!aimBase) {
        aimBase = ve - AIMOFF_BASE;
        aimWhy = "vecEnd-0x1E8 (last block)";
    }
    uint8_t blk[AIMBLK_LEN];
    bool bok = xt.readRaw(aimBase, blk, AIMBLK_LEN);
    slog("[i] aimBase=0x%llx (%s) read=%s\n", (unsigned long long)aimBase, aimWhy,
         bok ? "ok" : "FAIL");
    if (!bok) {
        slog("[!] khong doc duoc khoi aim. aimBase co the sai (heap da realloc).\n");
        return 0;
    }
    float sv = f32at(blk, A_SMOOTH);
    float fovf0 = f32at(blk, A_FOV), maxdf0 = f32at(blk, A_MAXD);
    slog("[i] float@+0xBC (smooth)=%.7g  float@+0xB0(fov)=%.7g  float@+0xB8(maxd)=%.7g\n",
         sv, fovf0, maxdf0);
    slog("[i] u8@+0x9C(en)=%u  u32@+0xA0(method)=%u  u32@+0xA4(key)=%u  u32@+0xA8(keyMode)=%u\n",
         (unsigned)blk[A_ENABLED], u32at(blk, A_METHOD), u32at(blk, A_KEY),
         u32at(blk, A_KEYMODE));

    bool pass;
    if (a.smoothAuto) {
        pass = std::isfinite(sv) && sv > 1e-4f && sv < 1e4f;
        slog("[i] --smooth=auto: live value %.7g -> %s\n", sv,
             pass ? "plausible (chap nhan)" : "IMPLAUSIBLE (tu choi)");
    } else {
        pass = (sv == (float)a.smoothExpected) ||
               (sv > a.smoothExpected - 1e-3f && sv < a.smoothExpected + 1e-3f);
    }
    if (a.aimBaseOverride) pass = true;  // override = nguoi dung da chot base

    if (doDump) {
        slog("=== AIM BLOCK @0x%llx (0x%X bytes) ===\n", (unsigned long long)aimBase,
             AIMBLK_LEN);
        hexDumpBlock(blk, AIMBLK_LEN);
        if (allBlocks && nblocks) {
            for (uint64_t i = 0; i < nblocks && i < 16; i++) {
                uint8_t gb[AIMBLK_LEN];
                uint64_t baddr = vb + i * BLOCK_SIZE + 0x20;
                if (!xt.readRaw(baddr, gb, AIMBLK_LEN)) {
                    slog("=== BLOCK %llu @0x%llx: READ FAIL ===\n", (unsigned long long)i,
                         (unsigned long long)baddr);
                    continue;
                }
                char mark = (baddr == aimBase) ? '*' : ' ';
                slog("=== BLOCK %llu%c @0x%llx smooth=%.7g en=%u meth=%u key=%u km=%u ===\n",
                     (unsigned long long)i, mark, (unsigned long long)baddr,
                     f32at(gb, A_SMOOTH), (unsigned)gb[A_ENABLED], u32at(gb, A_METHOD),
                     u32at(gb, A_KEY), u32at(gb, A_KEYMODE));
                hexDumpBlock(gb, AIMBLK_LEN);
            }
        }
    }

    if (pass) {
        slog("[+] BASE VERIFY OK: smooth@+0xBC == %.7g\n", a.smoothExpected);
        return aimBase;
    }

    // ---- FAIL: diagnostics + fallback scans (khong duoc tin base) ----
    slog("[!] BASE VERIFY FAIL: +0xBC = %.7g, ky vong %.7g\n", sv, a.smoothExpected);
    slog("---- scan float trong khoi aim (moi offset co the la smooth) ----\n");
    int hits = 0;
    for (int off = 0; off + 4 <= AIMBLK_LEN; off += 4)
        if (off != A_SMOOTH && fabsf(f32at(blk, off) - (float)a.smoothExpected) <= 1e-4f) {
            slog("    +0x%03X = %.7g  (u32=%u)\n", off, f32at(blk, off), u32at(blk, off));
            hits++;
        }
    if (!hits) slog("    (none)\n");
    if (nblocks) {
        slog("---- scan tung profile block (offset nao co %.7g) ----\n", a.smoothExpected);
        for (uint64_t i = 0; i < nblocks && i < 16; i++) {
            uint8_t gb[AIMBLK_LEN];
            uint64_t baddr = vb + i * BLOCK_SIZE + 0x20;
            if (!xt.readRaw(baddr, gb, AIMBLK_LEN)) continue;
            slog("  block %llu @0x%llx: ", (unsigned long long)i, (unsigned long long)baddr);
            int h = 0;
            for (int off = 0; off + 4 <= AIMBLK_LEN && h < 6; off += 4)
                if (fabsf(f32at(gb, off) - (float)a.smoothExpected) <= 1e-4f) {
                    slog("+0x%03X=%.7g ", off, f32at(gb, off));
                    h++;
                }
            if (!h) slog("(none)");
            slog(" [smooth@+0xBC=%.7g en=%u meth=%u key=%u km=%u]\n", f32at(gb, A_SMOOTH),
                 (unsigned)gb[A_ENABLED], u32at(gb, A_METHOD), u32at(gb, A_KEY),
                 u32at(gb, A_KEYMODE));
        }
    }
    slog("---- scan .data (0x32D000..0x33F816) float == %.7g (toi da 64 hit) ----\n",
         a.smoothExpected);
    {
        std::vector<uint8_t> d(0x12816);
        if (xt.readRaw(veBase + 0x32D000, d.data(), d.size())) {
            int h = 0;
            for (size_t off = 0; off + 4 <= d.size() && h < 64; off += 4) {
                float f;
                memcpy(&f, &d[off], 4);
                if (f == (float)a.smoothExpected) {
                    slog("    DAT_%llx\n",
                         (unsigned long long)(0x140000000ull + 0x32D000 + off));
                    h++;
                }
            }
            if (!h) slog("    (khong co hit trong .data)\n");
        } else {
            slog("    (khong doc duoc .data)\n");
        }
    }
    slog("[!] => KHONG dung base nay. Chay lai voi --aimbase=<addr> sau khi chot, "
         "hoac --smooth=<value> neu gia tri khac.\n");
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
static volatile LONG g_stop = 0;
static BOOL WINAPI ctrlHandler(DWORD) {
    g_stop = 1;
    return TRUE;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    disableQuickEdit();
    Args a = parseArgs(argc, argv);
    if (a.help) { usage(); return 0; }
    if (a.quiet) g_quiet = true;
    if (!a.logPath.empty()) g_log = fopen(a.logPath.c_str(), "wb");
    enableDebugPriv();

    slog("=== velsample — PHASE B read-only sampler ===\n");
    slog("[i] elevated=%s  qpc_freq=%llu\n", isElevated() ? "yes" : "NO",
         (unsigned long long)qpcFreqVal());

    // --- Roblox (RPM path, giong ext_watch) --------------------------------
    Memory m;
    if (!m.findProcess(a.robloxProc) || !m.open()) {
        slog("[!] khong mo duoc Roblox process (%ls)\n", a.robloxProc.c_str());
        return 1;
    }
    slog("[i] Roblox pid=%lu build=%s\n", (unsigned long)m.pid, m.exeVersion().c_str());
    Offsets o;
    std::string olog, cache = "offsets_cache.json";
    if (!OffsetManager::ensure(m.exeVersion(), cache, o, olog)) {
        slog("[!] offsets FAIL: %s\n", olog.c_str());
        return 1;
    }
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) {
        slog("[!] khong tim thay module base Roblox\n");
        return 1;
    }
    slog("[i] Roblox base=0x%llx  cameraPos=0x%X cameraRot=0x%X\n",
         (unsigned long long)sdk.base, (unsigned)o.cameraPosition, (unsigned)o.cameraRotation);

    int rotOff = o.cameraRotation ? o.cameraRotation : 0xC8;
    slog("[i] cameraRotation=0x%X (json cameraPosition=0x%X); sample pos tai CA 0xC4 va 0xEC\n",
         rotOff, (unsigned)o.cameraPosition);
    if (rotOff != 0xC8) slog("[!] CANH BAO: cameraRotation != 0xC8\n");

    uint64_t dm = 0, ws = 0, cam = 0;
    auto resolveCam = [&]() -> bool {
        if (!dm && !sdk.dataModel(dm)) return false;
        if (!ws && !sdk.workspace(dm, ws)) { dm = 0; return false; }
        if (!sdk.camera(ws, cam)) { ws = 0; return false; }
        return cam != 0;
    };
    if (!resolveCam()) slog("[!] chua resolve duoc Camera (se thu lai khi chay)\n");
    else slog("[i] Camera=0x%llx\n", (unsigned long long)cam);

    // MouseService (find child by name, giong FUN_1400FC490)
    uint64_t msvc = a.mouseOverride;
    auto resolveMsvc = [&]() -> bool {
        if (a.noMouse) return false;
        if (msvc) return true;
        uint64_t d2 = 0;
        if (!sdk.dataModel(d2)) return false;
        bool found = false;
        sdk.walkChildren(d2, [&](uint64_t c) {
            if (sdk.readName(c) == "MouseService") { msvc = c; found = true; return false; }
            return true;
        });
        return found;
    };
    if (!a.noMouse && !msvc) {
        if (resolveMsvc()) slog("[i] MouseService=0x%llx\n", (unsigned long long)msvc);
        else slog("[!] khong thay MouseService (se thu lai)\n");
    } else if (msvc) {
        slog("[i] MouseService (override)=0x%llx\n", (unsigned long long)msvc);
    }

    // hwnd cua Roblox (focus flag)
    WinPick wp{ m.pid, nullptr, 0 };
    EnumWindows(pickCb, (LPARAM)&wp);
    slog("[i] Roblox hwnd=0x%llx\n", (unsigned long long)(uintptr_t)wp.hwnd);

    // --- svcscan mode -------------------------------------------------------
    if (a.svcScan) {
        uint64_t dd = 0;
        if (!sdk.dataModel(dd)) { slog("[!] datamodel fail\n"); return 1; }
        int n = 0;
        sdk.walkChildren(dd, [&](uint64_t c) {
            std::string nm = sdk.readName(c), cl = sdk.readClass(c);
            if (a.svcFilter.empty() || nm.find(a.svcFilter) != std::string::npos ||
                cl.find(a.svcFilter) != std::string::npos)
                slog("  [%3d] %-28s %-24s 0x%llx\n", n, nm.c_str(), cl.c_str(),
                     (unsigned long long)c);
            n++;
            return true;
        });
        slog("[i] total children=%d\n", n);
        return 0;
    }

    // --- velext -------------------------------------------------------------
    XMem xt;
    if (!xt.findAndOpen(a.velProc.c_str())) {
        slog("[!] khong mo duoc %ls (pid=%lu err=%lu). Can chay ELEVATED (UAC).\n",
             a.velProc.c_str(), (unsigned long)xt.pid, (unsigned long)GetLastError());
        return 2;
    }
    slog("[i] velext pid=%lu direct_syscall_read=%s (id=%u)\n", (unsigned long)xt.pid,
         xt.directRead ? "yes" : "no(rpm)", g_ntReadId);
    uint64_t veBase = mainModuleBase(xt.h, a.velProc.c_str());
    if (!veBase) {
        slog("[!] khong lay duoc module base velext\n");
        return 2;
    }
    slog("[i] velext base=0x%llx\n", (unsigned long long)veBase);

    uint64_t aimBase = dumpAndValidate(xt, veBase, a, a.dumpBlock, a.allBlocks);
    if (a.dumpBlock) return aimBase ? 0 : 3;
    if (!aimBase && !a.forceBase) return 3;
    if (!aimBase && a.forceBase) {
        uint64_t ve = 0;
        xt.read(veBase + VE_RVA(VA_VEC_END), ve);
        aimBase = a.aimBaseOverride ? a.aimBaseOverride : (ve - AIMOFF_BASE);
        slog("[!] force-base: dung aimBase=0x%llx (CHUA verify)\n",
             (unsigned long long)aimBase);
    }
    // index cua block da chon (de bam theo neu vector doi)
    int chosenIdx = -1;
    {
        uint64_t vb0 = 0, ve0 = 0;
        xt.read(veBase + VE_RVA(VA_VEC_BEGIN), vb0);
        xt.read(veBase + VE_RVA(VA_VEC_END), ve0);
        if (aimBase >= vb0 && aimBase < ve0 && ((aimBase - vb0) % BLOCK_SIZE) == 0x20)
            chosenIdx = (int)((aimBase - vb0) / BLOCK_SIZE);
    }
    slog("[i] block idx dang sample = %d\n", chosenIdx);
    long long selIdx = chosenIdx;

    // --- sample loop ---------------------------------------------------------
    FILE* cf = fopen(a.csv.c_str(), "wb");
    if (!cf) {
        slog("[!] khong mo duoc CSV %s\n", a.csv.c_str());
        return 4;
    }
    {
        std::string hdr = ROW_FIELDS(X_NAME);
        if (!hdr.empty() && hdr.back() == ',') hdr.pop_back();
        fprintf(cf, "%s\n", hdr.c_str());
    }

    uint64_t freq = qpcFreqVal();
    auto nowNs = [&]() { return ticksToNs(qpcTicks(), freq); };

    if (a.delay > 0) {
        slog("[i] bat dau sau %.1f giay...\n", a.delay);
        Sleep((DWORD)(a.delay * 1000));
    }

    slog("[i] SAMPLING rate=%d Hz sec=%.1f csv=%s\n", a.rate, a.sec, a.csv.c_str());
    Beep(1200, 150); Sleep(120); Beep(1700, 300);  // hieu lenh: BAT DAU DO
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    uint64_t periodNs = 1000000000ull / (uint64_t)a.rate;
    uint64_t startNs = nowNs();
    uint64_t endNs = startNs + (uint64_t)(a.sec * 1e9);
    uint64_t nextNs = startNs;
    uint64_t nSamples = 0;
    std::string sink;
    sink.reserve(8 << 20);
    uint64_t lastProgress = startNs, lastFlush = startNs, lastResample = 0;
    uint64_t focusCheck = 0;
    unsigned focus = 0;
    uint64_t camFailRows = 0, velFailRows = 0;
    bool warnedCam = false, movedBaseWarned = false;

    while (!g_stop) {
        uint64_t tns = nowNs();
        if (tns >= endNs) break;

        // ----- periodic re-resolution (250 ms) -----
        if (tns - lastResample > 250000000ull) {
            lastResample = tns;
            resolveCam();
            if (!a.noMouse && !msvc) resolveMsvc();
        }

        // ----- Roblox camera: rot @rotOff, pos candidates 0xEC + 0xC4 -----
        unsigned camOk = 0;
        float px = 0, py = 0, pz = 0, px2 = 0, py2 = 0, pz2 = 0;
        float rot[9] = {0};
        {
            uint8_t rotb[36];
            if (mread(m, cam + (uint64_t)rotOff, rotb, 36)) {
                memcpy(rot, rotb, 36);
                camOk = 1;
            } else {
                camFailRows++;
                if (!warnedCam) { warnedCam = true; slog("[!] read camera FAIL (lan dau)\n"); }
            }
            uint8_t p12[12];
            if (mread(m, cam + 0xEC, p12, 12)) {
                memcpy(&px, &p12[0], 4); memcpy(&py, &p12[4], 4); memcpy(&pz, &p12[8], 4);
            }
            if (mread(m, cam + 0xC4, p12, 12)) {
                memcpy(&px2, &p12[0], 4); memcpy(&py2, &p12[4], 4); memcpy(&pz2, &p12[8], 4);
            }
        }

        // ----- velext settings block -----
        uint8_t sb[AIMBLK_LEN];
        bool vOk = xt.readRaw(aimBase, sb, AIMBLK_LEN);
        if (!vOk) velFailRows++;
        unsigned vis = vOk ? sb[A_VISIBLE] : 0;
        unsigned en = vOk ? sb[A_ENABLED] : 0;
        uint32_t meth = vOk ? u32at(sb, A_METHOD) : 0;
        uint32_t key = vOk ? u32at(sb, A_KEY) : 0;
        uint32_t keymode = vOk ? u32at(sb, A_KEYMODE) : 0;
        unsigned igteam = vOk ? sb[A_IGTEAM] : 0;
        unsigned igdown = vOk ? sb[A_IGDOWN] : 0;
        uint32_t fovu = vOk ? u32at(sb, A_FOV) : 0;
        float fovf = vOk ? f32at(sb, A_FOV) : 0;
        unsigned usefov = vOk ? sb[A_USEFOV] : 0;
        uint32_t maxdu = vOk ? u32at(sb, A_MAXD) : 0;
        float maxdf = vOk ? f32at(sb, A_MAXD) : 0;
        float smooth = vOk ? f32at(sb, A_SMOOTH) : 0;
        unsigned sticky = vOk ? sb[A_STICKY] : 0;
        unsigned pred = vOk ? sb[A_PRED] : 0;
        float predscale = vOk ? f32at(sb, A_PREDSCALE) : 0;
        uint32_t tpart = vOk ? u32at(sb, A_TPART) : 0;
        uint32_t prio = vOk ? u32at(sb, A_PRIO) : 0;
        unsigned drawfov = vOk ? sb[A_DRAWFOV] : 0;
        unsigned drawtgt = vOk ? sb[A_DRAWTGT] : 0;

        // ----- velext scattered ranges -----
        uint8_t gbuf[NRANGES][0x90];
        bool passive = true;
        for (int i = 0; i < NRANGES; i++) {
            if (!xt.readRaw(veBase + VE_RVA(VRANGES[i].va), gbuf[i], VRANGES[i].len)) {
                passive = false;
                memset(gbuf[i], 0, VRANGES[i].len);
            }
        }
        unsigned gOk = passive ? 1 : 0;
        const uint8_t* G0 = gbuf[0];
        const uint8_t* G1 = gbuf[1];
        const uint8_t* G2 = gbuf[2];
        const uint8_t* G3 = gbuf[3];
        const uint8_t* G4 = gbuf[4];
        const uint8_t* G5 = gbuf[5];
        float yaw = f32at(G0, 0x08), u24c = f32at(G0, 0x0C), u250 = f32at(G0, 0x10);
        uint64_t dedup = u64at(G1, 0x00);
        uint32_t u3a0 = u32at(G1, 0x10), u3a4 = u32at(G1, 0x14);
        float pitch = f32at(G1, 0x1C);
        float pa0 = f32at(G1, 0x20), pa1 = f32at(G1, 0x24), pa2 = f32at(G1, 0x28),
              pa3 = f32at(G1, 0x2C);
        float pb0 = f32at(G1, 0x30), pb1 = f32at(G1, 0x34), pb2 = f32at(G1, 0x38),
              pb3 = f32at(G1, 0x3C);
        float pc0 = f32at(G1, 0x40), pc1 = f32at(G1, 0x44), pc2 = f32at(G1, 0x48),
              pc3 = f32at(G1, 0x4C);
        uint64_t stamp = u64at(G1, 0x78), bpstamp = u64at(G1, 0x88);
        uint64_t rbase = u64at(G2, 0x10);
        float u3898 = f32at(G2, 0x18);
        uint64_t objptr = u64at(G2, 0x08);
        unsigned mflag = G3[0x1A], jgate = G3[0x1B], c5c = G3[0x1C], c5d = G3[0x1D];
        uint32_t renderfps = u32at(G4, 0x08);
        unsigned uncap = G5[0x08];
        uint32_t u38484 = u32at(G5, 0x0C);

        uint64_t vecEnd = 0, vecBegin2 = 0;
        xt.read(veBase + VE_RVA(VA_VEC_END), vecEnd);
        xt.read(veBase + VE_RVA(VA_VEC_BEGIN), vecBegin2);
        uint64_t nblocks = (vecEnd > vecBegin2 && ((vecEnd - vecBegin2) % BLOCK_SIZE) == 0)
                               ? (vecEnd - vecBegin2) / BLOCK_SIZE : 0;
        uint64_t activeG = 0;
        xt.read(veBase + VE_RVA(0x140334E80ull), activeG);
        // bam theo block live: uu tien active-index (neu --block khong set),
        // va re-pin theo vector neu heap realloc.
        if (!a.aimBaseOverride && vecEnd > vecBegin2 && vecBegin2 &&
            ((vecEnd - vecBegin2) % BLOCK_SIZE) == 0) {
            uint64_t nb = (vecEnd - vecBegin2) / BLOCK_SIZE;
            long long want = (a.blockIdx >= 0) ? (long long)a.blockIdx : (long long)activeG;
            if (want >= 0 && (uint64_t)want < nb && want != selIdx) {
                selIdx = want;
                aimBase = vecBegin2 + (uint64_t)want * BLOCK_SIZE + 0x20;
                if (!movedBaseWarned) {
                    movedBaseWarned = true;
                    slog("[i] active doi -> aimBase (block %lld) moi=0x%llx\n", want,
                         (unsigned long long)aimBase);
                }
            } else if (selIdx >= 0 && (uint64_t)selIdx < nb) {
                uint64_t nb2 = vecBegin2 + (uint64_t)selIdx * BLOCK_SIZE + 0x20;
                if (nb2 != aimBase) aimBase = nb2;
            }
        }

        // ----- MouseService +0x21C -----
        uint32_t msvc21cu = 0;
        float msvc21cf = 0;
        if (msvc) {
            uint32_t raw = 0;
            if (m.read(msvc + 0x21C, raw)) {
                msvc21cu = raw;
                memcpy(&msvc21cf, &raw, 4);
            }
        }

        // ----- sensitivity globals (Roblox base + RVA) -----
        float sA0 = 0, sA1 = 0, sB0 = 0, sB1 = 0, sC0 = 0, sC1 = 0;
        unsigned sensOk = 0;
        {
            uint8_t s16[16]{};
            if (mread(m, sdk.base + RB_SENS_AB, s16, 16)) {
                memcpy(&sA0, s16 + 0, 4); memcpy(&sA1, s16 + 4, 4);
                memcpy(&sB0, s16 + 8, 4); memcpy(&sB1, s16 + 12, 4);
                uint8_t s8[8]{};
                if (mread(m, sdk.base + RB_SENS_C, s8, 8)) {
                    memcpy(&sC0, s8 + 0, 4); memcpy(&sC1, s8 + 4, 4);
                    sensOk = 1;
                }
            }
        }

        // ----- input object (obj co the nam trong Roblox hoac velext) -----
        unsigned inSrc = 0;
        uint64_t inBeg = 0, inEnd = 0, inCap = 0, inSize = 0;
        uint32_t inFlag = 0, inU44 = 0;
        float rec[8] = {0};
        if (objptr && SDK::plausiblePtr(objptr)) {
            uint8_t ib[0x50];
            bool sane = false;
            if (mread(m, objptr, ib, sizeof(ib))) {
                uint64_t b = u64at(ib, 0x20), e = u64at(ib, 0x28), c = u64at(ib, 0x30);
                if (SDK::plausiblePtr(b) && e >= b && c >= e && (c - b) < 0x200000) {
                    inSrc = 1;
                    sane = true;
                }
            }
            if (!sane && xt.readRaw(objptr, ib, sizeof(ib))) {
                uint64_t b = u64at(ib, 0x20), e = u64at(ib, 0x28), c = u64at(ib, 0x30);
                if (SDK::plausiblePtr(b) && e >= b && c >= e && (c - b) < 0x200000)
                    inSrc = 2;
            }
            if (inSrc) {
                inBeg = u64at(ib, 0x20);
                inEnd = u64at(ib, 0x28);
                inCap = u64at(ib, 0x30);
                inSize = inEnd - inBeg;
                inFlag = u32at(ib, 0x48);
                inU44 = u32at(ib, 0x44);
                if (inBeg && inSize >= 20 && inSize <= 0x40000) {
                    uint8_t rb[32];
                    if (mread(m, inEnd - 20, rb, 32)) {
                        for (int k = 0; k < 8; k++) memcpy(&rec[k], rb + k * 4, 4);
                    }
                }
            }
        }

        // ----- focus -----
        if (tns - focusCheck > 50000000ull) {
            focusCheck = tns;
            focus = (GetForegroundWindow() == wp.hwnd) ? 1 : 0;
        }

        // ----- row -----
        {
            char row[4096];
            // X_ARG de lai dau phay cuoi; them sentinel 0 (varargs bo qua)
            snprintf(row, sizeof(row), ROW_FIELDS(X_FMT), ROW_FIELDS(X_ARG) 0);
            sink.append(row);
            sink.push_back('\n');
        }
        nSamples++;
        if (sink.size() > (4 << 20)) {
            fwrite(sink.data(), 1, sink.size(), cf);
            sink.clear();
        }
        if (tns - lastFlush > 2000000000ull) {
            lastFlush = tns;
            if (!sink.empty()) { fwrite(sink.data(), 1, sink.size(), cf); sink.clear(); }
            fflush(cf);
        }
        if (tns - lastProgress > 1000000000ull) {
            lastProgress = tns;
            double el = (tns - startNs) / 1e9;
            progLine(nSamples, nSamples / (el > 0 ? el : 1), camFailRows, velFailRows);
        }

        // ----- pacing -----
        nextNs += periodNs;
        uint64_t nowc = nowNs();
        if (nowc < nextNs) {
            uint64_t rem = nextNs - nowc;
            if (rem > 1500000ull) {
                Sleep(0);
            } else {
                while (nowNs() < nextNs) YieldProcessor();
            }
        } else if (nowc > nextNs + periodNs * 4) {
            nextNs = nowc;  // resync
        }
    }

    if (!sink.empty()) { fwrite(sink.data(), 1, sink.size(), cf); sink.clear(); }
    fflush(cf);
    fclose(cf);

    double el = (nowNs() - startNs) / 1e9;
    Beep(700, 500);  // hieu lenh: KET THUC DO
    slog("\n[done] samples=%llu duration=%.2fs avg=%.0f Hz\n", (unsigned long long)nSamples,
         el, nSamples / (el > 0 ? el : 1));
    slog("[i] read fails: velext=%lld (rpm_ok=%d syscall_ok=%d) camFailRows=%llu "
         "velFailRows=%llu\n",
         xt.failCount, (int)xt.rpmWorked, (int)xt.syscallWorked, camFailRows, velFailRows);
    slog("[i] csv=%s\n", a.csv.c_str());
    if (g_log) fclose(g_log);
    return 0;
}
