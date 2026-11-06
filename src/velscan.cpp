// velscan: read-only inspection of the velext process from the outside.
// Requires elevation (velext runs elevated; a lower-integrity handle can't read it).
//
// Modes (combine freely):
//   -mods            module inventory of velext (base/size/path) + PE sections
//   -unpack <file>   reconstruct the *running* velext image to a PE on disk
//                    (packed trucks are resolved: memory bytes, not file bytes)
//   -scan            scan velext's writable memory for pointers into Roblox
//                    (base/dm/workspace/camera/players/local/model/humanoid/prim)
//                    and report their RVAs -> velext's own global map
//   -watch <secs>    sample Roblox Camera.Rotation + VisualEngine.ViewMatrix at
//                    ~1 kHz, log every rotation change (dt in us) -> write
//                    cadence of whatever aims, and whether the engine's render
//                    matrix follows those writes
//
// Default: -mods -scan -watch 10
#include "offsets.h"
#include "sdk.h"
#include <psapi.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

static bool readBytesMem(Memory& vm, uint64_t addr, void* buf, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(vm.h, (LPCVOID)addr, buf, n, &got) && got == n;
}

struct ModInfo {
    uint64_t base = 0;
    uint64_t size = 0;
    std::wstring name;
    std::wstring path;
};

static bool findVelext(DWORD& pidOut, std::wstring& nameOut) {
    const wchar_t* prefer[] = { L"velext_v1.0.2.exe", L"velext_v1.2.exe",
                                 L"velext_v1.exe", L"velext.exe" };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool ok = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            for (auto p : prefer) {
                if (_wcsicmp(pe.szExeFile, p) == 0) {
                    pidOut = pe.th32ProcessID;
                    nameOut = pe.szExeFile;
                    ok = true;
                    break;
                }
            }
            if (ok) break;
            if (wcsstr(pe.szExeFile, L"velext") || wcsstr(pe.szExeFile, L"velext_")) {
                pidOut = pe.th32ProcessID;
                nameOut = pe.szExeFile;
                ok = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return ok;
}

static std::vector<ModInfo> listModules(HANDLE h) {
    std::vector<ModInfo> out;
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModulesEx(h, mods, sizeof(mods), &need, LIST_MODULES_ALL)) return out;
    int n = (int)(need / sizeof(HMODULE));
    if (n > 1024) n = 1024;
    for (int i = 0; i < n; ++i) {
        MODULEINFO mi{};
        if (!GetModuleInformation(h, mods[i], &mi, sizeof(mi))) continue;
        ModInfo m;
        m.base = (uint64_t)mi.lpBaseOfDll;
        m.size = (uint64_t)mi.SizeOfImage;
        wchar_t buf[MAX_PATH]{};
        if (GetModuleFileNameExW(h, mods[i], buf, MAX_PATH)) m.path = buf;
        wchar_t nb[MAX_PATH]{};
        if (GetModuleBaseNameW(h, mods[i], nb, MAX_PATH)) m.name = nb;
        out.push_back(m);
    }
    return out;
}

static const ModInfo* findMod(const std::vector<ModInfo>& mods, uint64_t addr) {
    for (auto& m : mods)
        if (addr >= m.base && addr < m.base + m.size) return &m;
    return nullptr;
}

static void printSections(Memory& vm, const ModInfo& m) {
    IMAGE_DOS_HEADER dos{};
    if (!vm.read(m.base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        printf("  (no PE header readable; packed or header wiped)\n");
        return;
    }
    IMAGE_NT_HEADERS64 nt{};
    if (!vm.read(m.base + (uint64_t)dos.e_lfanew, nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE) {
        printf("  (no NT header at +0x%x)\n", (unsigned)dos.e_lfanew);
        return;
    }
    printf("  image: ImageBase=0x%llx Entry=+0x%x SizeOfImage=0x%x sections=%d\n",
           (unsigned long long)nt.OptionalHeader.ImageBase,
           (unsigned)nt.OptionalHeader.AddressOfEntryPoint,
           (unsigned)nt.OptionalHeader.SizeOfImage,
           (int)nt.FileHeader.NumberOfSections);
    printf("  hdr: e_lfanew=0x%x SizeOfOptionalHeader=0x%x SizeOfHeaders=0x%x\n",
           (unsigned)dos.e_lfanew, (unsigned)nt.FileHeader.SizeOfOptionalHeader,
           (unsigned)nt.OptionalHeader.SizeOfHeaders);
    uint64_t secAt = m.base + (uint64_t)dos.e_lfanew + 4 +
                     sizeof(IMAGE_FILE_HEADER) +
                     (uint64_t)nt.FileHeader.SizeOfOptionalHeader;
    for (int i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER sh{};
        if (!vm.read(secAt + (uint64_t)i * sizeof(sh), sh)) {
            printf("  sec[%d] unreadable\n", i);
            break;
        }
        char nm[9]{};
        memcpy(nm, sh.Name, 8);
        uint64_t va = m.base + sh.VirtualAddress;
        double constRatio = sh.SizeOfRawData
            ? (double)sh.Misc.VirtualSize / (double)sh.SizeOfRawData : 0.0;
        printf("  %-8s va=0x%llx vsize=0x%-7x rawsize=0x%-7x vs/raw=%.2f "
               "flags=0x%x\n",
               nm, (unsigned long long)va, (unsigned)sh.Misc.VirtualSize,
               (unsigned)sh.SizeOfRawData, constRatio,
               (unsigned)sh.Characteristics);
    }
}

// hex dump of live velext memory (addr is absolute)
static void hexDump(Memory& vm, uint64_t addr, uint32_t len) {
    std::vector<BYTE> buf(len);
    if (!readBytesMem(vm, addr, buf.data(), len)) {
        printf("  hex: read failed at 0x%llx len=0x%x\n",
               (unsigned long long)addr, len);
        return;
    }
    for (uint32_t i = 0; i < len; i += 16) {
        printf("  %012llx  ", (unsigned long long)(addr + i));
        for (uint32_t j = 0; j < 16; ++j) {
            if (i + j < len) printf("%02x ", buf[i + j]);
            else printf("   ");
        }
        printf(" |");
        for (uint32_t j = 0; j < 16 && i + j < len; ++j) {
            BYTE c = buf[i + j];
            putchar(c >= 32 && c < 127 ? c : '.');
        }
        printf("|\n");
    }
}

static bool unpackImage(Memory& vm, const ModInfo& m, const char* outPath) {
    IMAGE_DOS_HEADER dos{};
    if (!vm.read(m.base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS64 nt{};
    if (!vm.read(m.base + (uint64_t)dos.e_lfanew, nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE) return false;
    DWORD hdrSize = nt.OptionalHeader.SizeOfHeaders;
    std::vector<BYTE> img(nt.OptionalHeader.SizeOfImage, 0);
    if (hdrSize > img.size()) return false;
    if (!readBytesMem(vm,m.base, img.data(), hdrSize)) return false;
    uint64_t secAt = m.base + (uint64_t)dos.e_lfanew + 4 +
                     sizeof(IMAGE_FILE_HEADER) +
                     (uint64_t)nt.FileHeader.SizeOfOptionalHeader;
    uint64_t written = hdrSize;
    for (int i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER sh{};
        if (!vm.read(secAt + (uint64_t)i * sizeof(sh), sh)) break;
        if (!sh.PointerToRawData || !sh.SizeOfRawData) continue;
        uint64_t end = (uint64_t)sh.PointerToRawData + sh.SizeOfRawData;
        if (end > img.size()) continue;
        if (!readBytesMem(vm,m.base + sh.VirtualAddress,
                          img.data() + sh.PointerToRawData, sh.SizeOfRawData))
            printf("  [warn] section %d read failed\n", i);
        written = std::max<uint64_t>(written, end);
    }
    FILE* f = fopen(outPath, "wb");
    if (!f) return false;
    size_t n = fwrite(img.data(), 1, (size_t)written, f);
    fclose(f);
    printf("  unpacked %llu bytes -> %s\n", (unsigned long long)n, outPath);
    return n == written;
}

// ---- target discovery in Roblox -------------------------------------------
struct Targets {
    uint64_t base = 0, dm = 0, ws = 0, cam = 0, ve = 0;
    uint64_t players = 0, local = 0, model = 0, humanoid = 0, hrp = 0, prim = 0;
};

static void resolveTargets(Memory& m, SDK& sdk, const Offsets& o, Targets& t) {
    t.base = sdk.base;
    if (!sdk.dataModel(t.dm)) return;
    sdk.workspace(t.dm, t.ws);
    if (t.ws) sdk.camera(t.ws, t.cam);
    if (o.visualEnginePtr) m.read(t.base + o.visualEnginePtr, t.ve);
    if (!t.players) sdk.findChildByClass(t.dm, "Players", t.players);
    if (t.players) m.read(t.players + (uint64_t)o.playersLocalPlayer, t.local);
    if (t.local) m.read(t.local + (uint64_t)o.playerModelInstance, t.model);
    if (t.model) {
        sdk.walkChildren(t.model, [&](uint64_t c) {
            if (sdk.readClass(c) == "Humanoid") { t.humanoid = c; return false; }
            return true;
        });
        sdk.walkChildren(t.model, [&](uint64_t c) {
            if (sdk.readName(c) == "HumanoidRootPart") { t.hrp = c; return false; }
            return true;
        });
    }
    if (t.hrp && o.basePartPrimitive)
        m.read(t.hrp + (uint64_t)o.basePartPrimitive, t.prim);
}

struct Hit {
    uint64_t addr = 0;
    uint64_t regionBase = 0;
    uint64_t regionSize = 0;
    DWORD prot = 0;
};

static void scanValue(HANDLE h, const char* label, uint64_t value,
                      const std::vector<ModInfo>& mods, int maxHits) {
    if (!value) { printf("  %-10s = 0 (skipped)\n", label); return; }
    printf("  %-10s = 0x%llx\n", label, (unsigned long long)value);
    int hits = 0;
    uint64_t addr = 0;
    const uint64_t kMax = 0x7FFFFFFF0000ull;
    std::vector<BYTE> buf;
    while (addr < kMax) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi))) break;
        uint64_t rbase = (uint64_t)mbi.BaseAddress;
        uint64_t rsize = (uint64_t)mbi.RegionSize;
        bool writable = (mbi.State == MEM_COMMIT) &&
                        ((mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                                         PAGE_EXECUTE_READWRITE)) != 0);
        if (writable && rsize >= 8) {
            if (buf.size() < rsize) buf.resize((size_t)rsize);
            SIZE_T got = 0;
            if (ReadProcessMemory(h, (LPCVOID)rbase, buf.data(),
                                  (SIZE_T)rsize, &got) && got >= 8) {
                for (SIZE_T i = 0; i + 8 <= got; i += 8) {
                    uint64_t v = 0;
                    memcpy(&v, buf.data() + i, 8);
                    if (v != value) continue;
                    uint64_t at = rbase + i;
                    const ModInfo* mm = findMod(mods, at);
                    printf("    hit+%03d 0x%llx", hits, (unsigned long long)at);
                    if (mm) {
                        char nm[64]{};
                        WideCharToMultiByte(CP_UTF8, 0, mm->name.c_str(), -1, nm,
                                            sizeof(nm), nullptr, nullptr);
                        printf("  %s+0x%llx", nm, (unsigned long long)(at - mm->base));
                    } else {
                        printf("  heap(region=0x%llx+0x%llx prot=0x%x)",
                               (unsigned long long)rbase,
                               (unsigned long long)rsize, mbi.Protect);
                    }
                    printf("\n");
                    if (++hits >= maxHits) {
                        printf("    (hit cap %d reached)\n", maxHits);
                        return;
                    }
                }
            }
        }
        if (!rsize) break;
        addr = rbase + rsize;
    }
    if (!hits) printf("    (no hits)\n");
}

// ---- camera sampler -------------------------------------------------------
struct Basis {
    float right[3]{}, up[3]{}, back[3]{};
};

static Basis basisFromRot(const float* m9) {
    Basis b;
    b.right[0] = m9[0]; b.right[1] = m9[3]; b.right[2] = m9[6];
    b.up[0] = m9[1]; b.up[1] = m9[4]; b.up[2] = m9[7];
    b.back[0] = m9[2]; b.back[1] = m9[5]; b.back[2] = m9[8];
    return b;
}

static float dot3(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static double qpcSec2() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

// High-rate sampler on Roblox Camera.Rotation (+ VisualEngine.ViewMatrix).
// Answers: how often does something write the camera basis, at what cadence,
// and does the engine's render matrix follow those bytes. Read-only.
static void watchCamera(Memory& m, const Offsets& o, const Targets& t, double secs) {
    if (!t.cam || !o.cameraRotation) { printf("  watch: no camera/rotation\n"); return; }
    uint64_t rotAddr = t.cam + (uint64_t)o.cameraRotation;
    uint64_t vmAddr = (o.visualEngineViewMatrix && t.ve)
                          ? t.ve + (uint64_t)o.visualEngineViewMatrix : 0;
    uint64_t posAddr = o.cameraPosition ? t.cam + (uint64_t)o.cameraPosition : 0;
    printf("  watch %.1fs  rot=0x%llx vm=0x%llx pos=0x%llx\n", secs,
           (unsigned long long)rotAddr, (unsigned long long)vmAddr,
           (unsigned long long)posAddr);

    double t0 = qpcSec2(), endT = t0 + secs;
    float prev[9]{};
    bool havePrev = false;
    uint64_t samples = 0, events = 0, vmChanges = 0;
    double lastEvT = 0;
    std::vector<uint32_t> dts;
    double alignSum = 0, alignMax = 0;
    int alignN = 0, alignBad = 0;
    int logged = 0;
    float prevVmFwd[3]{};
    bool haveVmPrev = false;
    double nextReport = t0 + 1.0;
    uint64_t rotEventsThisSecond = 0;

    while (true) {
        double now = qpcSec2();
        if (now >= endT) break;
        float cur[9]{};
        if (!m.read(rotAddr, cur)) { printf("  watch: read lost\n"); break; }
        ++samples;
        bool changed = false;
        if (havePrev)
            for (int i = 0; i < 9; ++i)
                if (cur[i] != prev[i]) { changed = true; break; }
        if (changed) {
            double dtMs = lastEvT > 0 ? (now - lastEvT) * 1000.0 : 0.0;
            if (lastEvT > 0 && dtMs < 10000.0)
                dts.push_back((uint32_t)std::lround(dtMs * 1000.0));
            lastEvT = now;
            ++events;
            ++rotEventsThisSecond;
            Basis b = basisFromRot(cur);
            float vm[16]{};
            bool haveVm = vmAddr && m.read(vmAddr, vm);
            double errDeg = NAN;
            if (haveVm) {
                float fwd[3] = { vm[8], vm[9], vm[10] };
                float ln = sqrtf(dot3(fwd, fwd));
                if (ln > 1e-6f)
                    for (auto& v : fwd) v /= ln;
                float al = fabsf(dot3(fwd, b.back));
                if (al > 1.0f) al = 1.0f;
                errDeg = acosf(al) * 57.2957795f;
                alignSum += errDeg;
                ++alignN;
                if (errDeg > alignMax) alignMax = errDeg;
                if (errDeg > 2.0) ++alignBad;
            }
            if (logged < 200) {
                ++logged;
                float pos[3]{};
                bool hp = posAddr && m.read(posAddr, pos);
                printf("  ev %5llu t=%7.3fms dt=%6lluus err=%s%.2fdeg "
                       "back=(%+.4f %+.4f %+.4f) pos=(%.2f %.2f %.2f)\n",
                       (unsigned long long)events, (now - t0) * 1000.0,
                       (unsigned long long)std::lround(dtMs * 1000.0),
                       haveVm ? "" : "na/", haveVm ? errDeg : 0.0f, b.back[0],
                       b.back[1], b.back[2], pos[0], pos[1], pos[2]);
            }
        }
        memcpy(prev, cur, sizeof(prev));
        havePrev = true;

        // engine render matrix change counter (cheap enough at this rate)
        if (vmAddr) {
            float vm[16]{};
            if (m.read(vmAddr, vm)) {
                float fwd[3] = { vm[8], vm[9], vm[10] };
                if (haveVmPrev) {
                    float d = fabsf(fwd[0] - prevVmFwd[0]) +
                              fabsf(fwd[1] - prevVmFwd[1]) +
                              fabsf(fwd[2] - prevVmFwd[2]);
                    if (d > 1e-7f) ++vmChanges;
                }
                memcpy(prevVmFwd, fwd, sizeof(fwd));
                haveVmPrev = true;
            }
        }

        if (now >= nextReport) {
            nextReport += 1.0;
            printf("  [%5.2fs] rot_events=%llu (%.1f/s) vm_changes=%llu "
                   "samples/s=%.0f\n",
                   now - t0, (unsigned long long)events,
                   (double)rotEventsThisSecond, (unsigned long long)vmChanges,
                   (double)samples / (now - t0 + 1e-9));
            rotEventsThisSecond = 0;
            fflush(stdout);
        }
    }
    double dur = qpcSec2() - t0;
    printf("  watch summary over %.2fs: samples=%llu (%.0f/s) rot_events=%llu "
           "(%.1f/s) vm_changes=%llu\n",
           dur, (unsigned long long)samples, (double)samples / dur,
           (unsigned long long)events, (double)events / dur,
           (unsigned long long)vmChanges);
    if (!dts.empty()) {
        std::sort(dts.begin(), dts.end());
        double sum = 0;
        for (auto d : dts) sum += d;
        printf("  dt: n=%zu mean=%.0fus p10=%u p50=%u p90=%u p99=%u max=%u\n",
               dts.size(), sum / (double)dts.size(),
               dts[(size_t)(dts.size() * 0.10)], dts[dts.size() / 2],
               dts[(size_t)(dts.size() * 0.90)],
               dts[(size_t)(dts.size() * 0.99)], dts.back());
    } else {
        printf("  dt: no consecutive rotation changes\n");
    }
    if (alignN)
        printf("  vm-follow: n=%d mean_err=%.3fdeg max_err=%.3fdeg "
               "over2deg=%d (%.1f%%)\n",
               alignN, alignSum / alignN, alignMax, alignBad,
               100.0 * alignBad / alignN);
    else
        printf("  vm-follow: no samples (view matrix unreadable)\n");
}

// ---- matrix scanner -------------------------------------------------------
// Find every 16-float window inside VisualEngine that (a) looks like a
// row-vector view*projection matrix and (b) actually changes while the game is
// live. Settles whether offsets_cache.json's ViewMatrix=432 is the live one.
static bool looksLikeViewProj(const float* m) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i]) || fabsf(m[i]) > 1e7f) return false;
    if (fabsf(m[15] - 1.0f) > 2e-2f) return false;
    if (fabsf(m[3]) > 2e-2f || fabsf(m[7]) > 2e-2f || fabsf(m[11]) > 2e-2f)
        return false;
    float r = dot3(m + 0, m + 0), u = dot3(m + 4, m + 4), f = dot3(m + 8, m + 8);
    if (r < 1e-6f || u < 1e-6f || f < 1e-6f) return false;
    return true;
}

static void dumpFloats(Memory& m, uint64_t addr, int n) {
    std::vector<float> f(n);
    if (!readBytesMem(m, addr, f.data(), (size_t)n * 4)) {
        printf("  dumpf: read fail at 0x%llx\n", (unsigned long long)addr);
        return;
    }
    for (int i = 0; i < n; i += 4) {
        printf("   +0x%03x:", i * 4);
        for (int k = 0; k < 4 && i + k < n; ++k) printf(" %+12.5f", f[i + k]);
        printf("\n");
    }
}

// Which 16-float windows inside VisualEngine actually track the camera?
// Self-validating: waits until Camera.Rotation moves, then samples N times and
// counts, per offset, how many samples differ from the baseline. A live
// view*projection matrix must show up as a high-hit window.
static void scanMatrices(Memory& m, const Offsets& o, const Targets& t, int ms) {
    if (!t.ve) { printf("  mats: no VisualEngine\n"); return; }
    const uint32_t SPAN = 0x2400;
    std::vector<BYTE> base(SPAN);
    if (!readBytesMem(m, t.ve, base.data(), SPAN)) { printf("  mats: read failed\n"); return; }

    printf("  mats: ve=0x%llx span=0x%x\n", (unsigned long long)t.ve, SPAN);
    printf("  mats: dump says ViewMatrix=%d\n", o.visualEngineViewMatrix);
    if (o.visualEngineViewMatrix > 0 && (uint32_t)o.visualEngineViewMatrix + 64 <= SPAN) {
        printf("  mats: dump-offset values:\n");
        dumpFloats(m, t.ve + (uint64_t)o.visualEngineViewMatrix, 16);
    }
    if (o.visualEngineFakeDataModel > 0 && (uint32_t)o.visualEngineFakeDataModel + 8 <= SPAN) {
        uint64_t fake = 0;
        m.read(t.ve + (uint64_t)o.visualEngineFakeDataModel, fake);
        printf("  mats: ve+FakeDataModel(0x%x)=0x%llx\n",
               o.visualEngineFakeDataModel, (unsigned long long)fake);
        if (o.visualEngineDimensions)
            printf("  mats: ve+Dimensions(0x%x) ints=(%d %d)\n",
                   o.visualEngineDimensions,
                   *(int*)(base.data() + (o.visualEngineDimensions & (SPAN - 1))),
                   *(int*)(base.data() + ((o.visualEngineDimensions + 4) & (SPAN - 1))));
    }

    // candidates: aligned, all-finite, plausible magnitude
    std::vector<uint32_t> cand;
    for (uint32_t off = 0; off + 64 <= SPAN; off += 4) {
        const float* f = (const float*)(base.data() + off);
        bool ok = true;
        for (int i = 0; i < 16; ++i)
            if (!std::isfinite(f[i]) || fabsf(f[i]) > 1e6f) { ok = false; break; }
        if (ok) cand.push_back(off);
    }

    // wait for the camera to actually move (max 3 s)
    float r0[9]{}, r1[9]{};
    bool camMoved = false;
    uint64_t rotAddr = t.cam + (uint64_t)o.cameraRotation;
    m.read(rotAddr, r0);
    for (int w = 0; w < 30 && !camMoved; ++w) {
        Sleep(100);
        if (!m.read(rotAddr, r1)) break;
        for (int i = 0; i < 9; ++i)
            if (r0[i] != r1[i]) { camMoved = true; break; }
    }
    printf("  mats: camera rotated during window = %s\n", camMoved ? "yes" : "no");

    const int K = 12;
    std::vector<int> hits(cand.size(), 0);
    std::vector<BYTE> buf(SPAN);
    int samples = 0;
    for (int s = 0; s < K; ++s) {
        Sleep((DWORD)(ms > 0 ? ms : 20));
        if (!readBytesMem(m, t.ve, buf.data(), SPAN)) break;
        ++samples;
        for (size_t ci = 0; ci < cand.size(); ++ci) {
            const float* a = (const float*)(base.data() + cand[ci]);
            const float* b = (const float*)(buf.data() + cand[ci]);
            int ch = 0;
            for (int i = 0; i < 16; ++i) if (a[i] != b[i]) ++ch;
            if (ch > 0) hits[ci] += ch;
        }
        memcpy(base.data(), buf.data(), SPAN);   // rolling baseline
    }

    struct R { uint32_t off; int hits; };
    std::vector<R> out;
    for (size_t ci = 0; ci < cand.size(); ++ci)
        if (hits[ci] > 0) out.push_back({ cand[ci], hits[ci] });
    std::sort(out.begin(), out.end(),
              [](const R& x, const R& y) { return x.hits > y.hits; });
    printf("  mats: samples=%d candidates=%zu changing=%zu\n", samples, cand.size(),
           out.size());
    int shown = 0;
    for (auto& r : out) {
        if (shown++ >= 24) break;
        const float* f = (const float*)(base.data() + r.off);
        printf("    off=0x%-5x hits=%3d  r0=(%+8.4f %+8.4f %+8.4f %+9.3f) "
               "r2=(%+8.4f %+8.4f %+8.4f %+9.3f) m15=%+.4f\n",
               r.off, r.hits, f[0], f[1], f[2], f[3], f[8], f[9], f[10], f[11],
               f[15]);
    }
}

// ---- thread profiler ------------------------------------------------------
struct ThreadInfo {
    DWORD tid = 0;
    uint64_t start = 0;
    uint64_t cpu0 = 0, cpu1 = 0;
    std::vector<std::pair<uint64_t,int>> hot; // rip -> samples
};

static uint64_t fileTimeMs(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart / 10000ull; // 100ns -> ms
}

static uint64_t threadCpuMs(HANDLE th) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(th, &c, &e, &k, &u)) return 0;
    return fileTimeMs(k) + fileTimeMs(u);
}

static void addHot(ThreadInfo& t, uint64_t rip) {
    for (auto& h : t.hot)
        if (h.first == rip) { h.second++; return; }
    t.hot.push_back({ rip, 1 });
}

// Sample RIPs of every velext thread for <secs>. Suspend/GetThreadContext/Resume
// per sample; keeps the window short so the target only sees micro-stalls.
static void profileThreads(DWORD pid, const std::vector<ModInfo>& mods, double secs) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    std::vector<ThreadInfo> threads;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            ThreadInfo ti;
            ti.tid = te.th32ThreadID;
            HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, ti.tid);
            if (!th) continue;
            typedef NTSTATUS(__stdcall * NtQIT)(HANDLE, int, PVOID, ULONG, PULONG);
            HMODULE nt = GetModuleHandleW(L"ntdll.dll");
            if (nt) {
                auto f = (NtQIT)GetProcAddress(nt, "NtQueryInformationThread");
                if (f) {
                    uint64_t start = 0;
                    ULONG ret = 0;
                    if (f(th, 9, &start, sizeof(start), &ret) >= 0) ti.start = start;
                }
            }
            ti.cpu0 = threadCpuMs(th);
            threads.push_back(ti);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    if (threads.empty()) { printf("  no threads\n"); return; }
    printf("  threads=%zu window=%.1fs\n", threads.size(), secs);

    double t0 = (double)GetTickCount64();
    double endT = t0 + secs;
    int rounds = 0;
    while ((double)GetTickCount64() < endT) {
        for (auto& ti : threads) {
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                       THREAD_QUERY_INFORMATION, FALSE, ti.tid);
            if (!th) continue;
            DWORD prev = SuspendThread(th);
            if (prev != (DWORD)-1) {
                CONTEXT c{};
                c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(th, &c)) {
                    uint64_t rip = (uint64_t)c.Rip;
                    const ModInfo* mm = findMod(mods, rip);
                    addHot(ti, mm ? (rip - mm->base) : rip);
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        }
        Sleep(0);
        ++rounds;
    }
    for (auto& ti : threads) {
        HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, ti.tid);
        if (th) { ti.cpu1 = threadCpuMs(th); CloseHandle(th); }
        std::sort(ti.hot.begin(), ti.hot.end(),
                  [](auto& a, auto& b) { return a.second > b.second; });
        printf("  tid=%-6lu start=0x%llx cpu=+%llums samples=%d\n", ti.tid,
               (unsigned long long)ti.start,
               (unsigned long long)(ti.cpu1 >= ti.cpu0 ? ti.cpu1 - ti.cpu0 : 0),
               rounds);
        int shown = 0;
        for (auto& h : ti.hot) {
            if (shown++ >= 6) break;
            printf("      rip rva=0x%-8llx hits=%d\n",
                   (unsigned long long)h.first, h.second);
        }
    }
}
int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    bool doMods = false, doScan = false;
    const char* unpackPath = nullptr;
    double watchSecs = 0.0;
    bool doHex = false;
    std::vector<std::pair<uint64_t,uint32_t>> hexReqs;
    double threadSecs = 0.0;
    int matsMs = 0;
    bool any = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-mods") { doMods = true; any = true; }
        else if (a == "-scan") { doScan = true; any = true; }
        else if (a == "-unpack" && i + 1 < argc) { unpackPath = argv[++i]; any = true; }
        else if (a == "-watch" && i + 1 < argc) { watchSecs = atof(argv[++i]); any = true; }
        else if (a == "-threads" && i + 1 < argc) { threadSecs = atof(argv[++i]); any = true; }
        else if (a == "-mats" && i + 1 < argc) { matsMs = atoi(argv[++i]); any = true; }
        else if (a == "-hex" && i + 2 < argc) {
            std::string s = argv[++i];
            uint64_t at = (uint64_t)strtoull(s.c_str(), nullptr, 0);
            uint32_t len = (uint32_t)strtoul(argv[++i], nullptr, 0);
            hexReqs.push_back({ at, len ? len : 64 });
            doHex = true; any = true;
        }
    }
    if (!any) { doMods = doScan = true; unpackPath = "vel_runtime.exe"; watchSecs = 10.0; }

    // --- Roblox side (read-only) ---
    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe") || !m.open()) {
        printf("FAIL: cannot open RobloxPlayerBeta.exe (elevation?)\n");
        return 1;
    }
    Offsets o;
    std::string log;
    if (!OffsetManager::ensure(m.exeVersion(), "offsets_cache.json", o, log)) {
        printf("FAIL: offsets: %s\n", log.c_str());
        return 1;
    }
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) { printf("FAIL: base\n"); return 1; }
    Targets t;
    resolveTargets(m, sdk, o, t);
    printf("== Roblox ==\n  version=%s base=0x%llx\n  dm=0x%llx ws=0x%llx cam=0x%llx "
           "ve=0x%llx\n  players=0x%llx local=0x%llx model=0x%llx hum=0x%llx "
           "hrp=0x%llx prim=0x%llx\n",
           m.exeVersion().c_str(), (unsigned long long)t.base,
           (unsigned long long)t.dm, (unsigned long long)t.ws,
           (unsigned long long)t.cam, (unsigned long long)t.ve,
           (unsigned long long)t.players, (unsigned long long)t.local,
           (unsigned long long)t.model, (unsigned long long)t.humanoid,
           (unsigned long long)t.hrp, (unsigned long long)t.prim);
    printf("  offsets: cameraRotation=%d cameraPosition=%d subject=%d vm=%d\n",
           o.cameraRotation, o.cameraPosition, o.cameraSubject,
           o.visualEngineViewMatrix);

    // Roblox-only passes run first: they never touch velext, so they are safe
    // to use even when velext has instrumentation checks armed.
    if (watchSecs > 0.0) {
        printf("\n== camera watch ==\n");
        watchCamera(m, o, t, watchSecs);
    }
    if (matsMs > 0) {
        printf("\n== matrix scan ==\n");
        scanMatrices(m, o, t, matsMs);
    }

    // --- velext side (only when a velext mode was asked for) ---
    bool needVelext = doMods || doScan || unpackPath || doHex || threadSecs > 0.0;
    if (!needVelext) {
        printf("\nVELSCAN DONE (roblox only)\n");
        return 0;
    }
    DWORD vpid = 0;
    std::wstring vname;
    if (!findVelext(vpid, vname)) {
        printf("FAIL: velext process not found\n");
        return 1;
    }
    wprintf(L"== velext ==\n  pid=%lu name=%s\n", vpid, vname.c_str());
    HANDLE vh = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, vpid);
    if (!vh) {
        printf("FAIL: cannot open velext (error=%lu). Needs elevation.\n",
               (unsigned long)GetLastError());
        return 1;
    }
    Memory vmem;
    vmem.pid = vpid;
    vmem.h = vh;

    std::vector<ModInfo> mods = listModules(vh);
    printf("  modules=%zu\n", mods.size());
    const ModInfo* mainMod = nullptr;
    for (auto& mm : mods) {
        char nm[128]{}, path[512]{};
        WideCharToMultiByte(CP_UTF8, 0, mm.name.c_str(), -1, nm, sizeof(nm), nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, mm.path.c_str(), -1, path, sizeof(path), nullptr, nullptr);
        printf("  %-28s base=0x%llx size=0x%llx %s\n", nm,
               (unsigned long long)mm.base, (unsigned long long)mm.size, path);
        if (vname == mm.name) mainMod = &mm;
    }
    if (mainMod) {
        printf("  main module sections:\n");
        printSections(vmem, *mainMod);
        if (unpackPath) {
            printf("  runtime image:\n");
            if (!unpackImage(vmem, *mainMod, unpackPath))
                printf("  unpack FAILED\n");
        }
    }
    if (doHex && mainMod) {
        printf("\n== hex dump ==\n");
        for (auto& hr : hexReqs) {
            uint64_t at = hr.first;
            if (at < 0x1000000ull) at += mainMod->base; // rva form
            hexDump(vmem, at, hr.second);
        }
    }
    if (threadSecs > 0.0) {
        printf("\n== thread profile ==\n");
        profileThreads(vpid, mods, threadSecs);
    }
    if (doScan) {
        printf("\n== pointer scan (velext writable memory) ==\n");
        scanValue(vh, "robloxBase", t.base, mods, 20);
        scanValue(vh, "datamodel", t.dm, mods, 20);
        scanValue(vh, "workspace", t.ws, mods, 20);
        scanValue(vh, "camera", t.cam, mods, 40);
        scanValue(vh, "visualEng", t.ve, mods, 20);
        scanValue(vh, "players", t.players, mods, 20);
        scanValue(vh, "localPlr", t.local, mods, 20);
        scanValue(vh, "model", t.model, mods, 20);
        scanValue(vh, "humanoid", t.humanoid, mods, 20);
        scanValue(vh, "hrp", t.hrp, mods, 20);
        scanValue(vh, "primitive", t.prim, mods, 20);
    }

    printf("\nVELSCAN DONE\n");
    return 0;
}
