// P2.1 overlay: transparent click-through fullscreen HUD (ImGui + D3D11).
// Shows live PlaceId / CamPos / FPS + crosshair. Read-only vs the game.
// Usage: ext_overlay.exe [-t seconds] (auto-quit, for testing). END key quits.
#include "offsets.h"
#include "sdk.h"
#include "ovl/ovl_types.h"
#include "ovl/ovl_ui.h"
#include "ovl/ovl_state.h"
#include "ovl/ovl_tabs.h"
#include <d3d11.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <map>
#include <set>
#include <thread>
#include <mutex>
#include <atomic>
#include <timeapi.h>
#include <shellapi.h>
#include <cctype>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND h, UINT m, WPARAM w, LPARAM l);

// Play mode: overlay never takes clicks (menu is display-only, no widgets).
// NCHITTEST always HTTRANSPARENT so the game gets every click, including
// through drawn ESP/aim pixels. -clickthru kept as a no-op alias.
//
// Menu upgrade: the panel must be clickable, so hit-testing is transparent
// EVERYWHERE except the menu's actual controls (widget rects + title bar,
// rebuilt every frame). Clicks anywhere else - HUD text, ESP boxes, aim, the
// gaps inside the panel - still go straight to the game, and WS_EX_NOACTIVATE
// keeps the game focused the whole time.
static bool g_clickthru = false;    // -clickthru forces full click-through
bool g_diagState = false;    // -diag: print arm/hotkey state to stdout



// ---- exe-relative side files ------------------------------------------------
// overlay.cfg, profiles\, logs and the offsets cache live next to the exe, NOT
// in the process CWD. The CWD is not ours to trust: "Run as administrator" from
// Explorer sets it to C:\Windows\System32, which silently made those runs load
// default settings and drop logs into an unwritable dir (observed 2026-10-19).
static std::string g_exeDir;
static void initExeDir() {
    char path[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p(path, n ? n : 0);
    size_t slash = p.find_last_of("\\/");
    g_exeDir = (slash == std::string::npos) ? "." : p.substr(0, slash);
}
static std::string exePath(const char* name) { return g_exeDir + "\\" + name; }

static int loadToggleKey() {
    FILE* f = fopen(exePath("overlay.cfg").c_str(), "r");
    if (!f) {
        f = fopen(exePath("overlay.cfg").c_str(), "w");
        if (f) {
            fputs("# toggle key for overlay hide/show.\n"
                  "# F1-F12, INSERT, DELETE, HOME, END, PGUP, PGDN,\n"
                  "# SPACE, letters/digits (e.g. G), \\ , or hex (e.g. 0x47)\n"
                  "toggle=G\n"
                  "aim.enabled=1\n"
                  "aim.key=RMB\n"
                  "aim.keymode=0\n"
                  "aim.fov=250\n"
                  "# aim.smoothing: velext model, tau = 8ms*smoothing (1..20; <=1 snaps)\n"
                  "aim.smoothing=5\n"
                  "aim.mode=0\n"
                  "aim.sticky=1\n"
                  "aim.sticktime=4\n"
                  "aim.predict=1\n"
                  "aim.pscale=1.02\n"
                  "aim.dynk=0.42\n"
                  "aim.nearest=1\n"
                  "aim.overextend=0\n"
                  "aim.height=1.5\n"
                  "aim.maxdist=2000\n"
                  "aim.deadzone=1\n"
                  "aim.ignoreteam=0\n"
                  "aim.ignoredead=1\n"
                  "aim.interval=0\n"
                  "# aim.bone: 0 Head, 1 HRP+height, 2/3 arms, 4/5 legs\n"
                  "aim.bone=0\n"
                  "aim.npcs=1\n"
                  "aim.sens=1.0\n"
                  "aim.gamesens=1.0\n"
                  "# aim.pid: 0 = velext filter (default), 1 = legacy PID/1-pole (debug)\n"
                  "aim.pid=0\n"
                  "aim.rate=500\n"
                  "aim.park=0\n"
                  "trig.enabled=0\n"
                  "trig.key=X\n"
                  "trig.keymode=1\n"
                  "trig.delay=45\n"
                  "trig.cooldown=90\n"
                  "trig.press=35\n"
                  "trig.pad=2\n"
                  "autoclick.enabled=0\n"
                  "autoclick.key=V\n"
                  "autoclick.keymode=1\n"
                  "autoclick.interval=100\n"
                  "autoclick.duration=2000\n"
                  "autoclick.press=35\n"
                  "input.chatguard=1\n"
                  "ply.mode=1\n"
                  "ply.speed=0\n"
                  "ply.speedval=32\n"
                  "ply.jump=0\n"
                  "ply.jumpval=100\n"
                  "ply.speedkey=B\n"
                  "ply.jumpkey=N\n"
                  "ply.sdnkey=0xDB\n"
                  "ply.supkey=0xDD\n"
                  "ply.jdnkey=0xBA\n"
                  "ply.jupkey=0xDE\n"
                  "fly.enabled=0\n"
                  "fly.key=F\n"
                  "fly.speed=60\n"
                  "# teleport is the menu's Teleport tab (players + TP buttons)\n"
                  "tp.npc=0\n"
                  "# clickable menu panel: the title-bar triangle collapses it,\n"
                  "# pos/size are written back when you drag/resize it\n"
                  "menu.collapsed=0\n"
                  "menu.tab=0\n"
                  "menu.unlockmouse=1\n"
                  "menu.pos=16,360\n"
                  "menu.size=340,420\n"
                  "# World tab (Đợt 2): client-local lighting/atmosphere/terrain\n"
                  "world.full=0\n"
                  "world.bright=2\n"
                  "world.fog=0\n"
                  "world.shadows=0\n"
                  "world.expo=0\n"
                  "world.expoval=0\n"
                  "world.clock=0\n"
                  "world.clockval=14\n"
                  "world.fx=0\n"
                  "world.atmo=0\n"
                  "world.water=0\n"
                  "world.grass=0\n"
                  "world.fovon=0\n"
                  "world.fov=90\n"
                  "world.zoom=0\n", f);
            fclose(f);
        }
        return 'G';
    }
    char line[128];
    int vk = 'G';
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "toggle", 6) == 0) {
            const char* eq = strchr(line, '=');
            if (eq) {
                int v = parseKey(eq + 1);
                if (v) vk = v;
            }
        }
    }
    fclose(f);
    return vk;
}

// Quit key: backslash (VK_OEM_5). No laptop is missing this one.
static const int QUIT_VK = 0xDC;

// ---- quaternion helpers (Foulz spin_cam slerp) ----
struct Quat {
    float x, y, z, w;
};

static Quat matToQuat(const float* m) {
    float r00 = m[0], r01 = m[1], r02 = m[2];
    float r10 = m[3], r11 = m[4], r12 = m[5];
    float r20 = m[6], r21 = m[7], r22 = m[8];
    float t = r00 + r11 + r22;
    Quat q;
    if (t > 0) {
        float s = sqrtf(t + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (r21 - r12) / s;
        q.y = (r02 - r20) / s;
        q.z = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        float s = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
        q.w = (r21 - r12) / s;
        q.x = 0.25f * s;
        q.y = (r01 + r10) / s;
        q.z = (r02 + r20) / s;
    } else if (r11 > r22) {
        float s = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
        q.w = (r02 - r20) / s;
        q.x = (r01 + r10) / s;
        q.y = 0.25f * s;
        q.z = (r12 + r21) / s;
    } else {
        float s = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
        q.w = (r10 - r01) / s;
        q.x = (r02 + r20) / s;
        q.y = (r12 + r21) / s;
        q.z = 0.25f * s;
    }
    return q;
}

static Quat slerpQ(Quat a, Quat b, float t) {
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0) {
        b.x = -b.x;
        b.y = -b.y;
        b.z = -b.z;
        b.w = -b.w;
        dot = -dot;
    }
    if (dot > 0.9995f) {
        Quat r{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
        float n = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
        if (n > 0.0001f) {
            r.x /= n;
            r.y /= n;
            r.z /= n;
            r.w /= n;
        }
        return r;
    }
    float th0 = acosf(dot), th = th0 * t, s0 = sinf(th0);
    float s1 = sinf(th) / s0, s2 = sinf(th0 - th) / s0;
    Quat r{ a.x * s2 + b.x * s1, a.y * s2 + b.y * s1,
            a.z * s2 + b.z * s1, a.w * s2 + b.w * s1 };
    return r;
}

static void quatToMat(Quat q, float* m) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0] = 1 - 2 * (yy + zz);
    m[1] = 2 * (xy - wz);
    m[2] = 2 * (xz + wy);
    m[3] = 2 * (xy + wz);
    m[4] = 1 - 2 * (xx + zz);
    m[5] = 2 * (yz - wx);
    m[6] = 2 * (xz - wy);
    m[7] = 2 * (yz + wx);
    m[8] = 1 - 2 * (xx + yy);
}

#ifndef MOUSEEVENTF_MOVE_NOCOALESCE
#define MOUSEEVENTF_MOVE_NOCOALESCE 0x2000
#endif

static double qpcSeconds() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

static void sendRelMouse(int mx, int my) {
    if (!mx && !my) return;
    INPUT mi{};
    mi.type = INPUT_MOUSE;
    mi.mi.dx = mx;
    mi.mi.dy = my;
    mi.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE;
    SendInput(1, &mi, sizeof(mi));
}

static float wrapPi(float a) {
    const float pi = 3.14159265f;
    while (a > pi) a -= 2.0f * pi;
    while (a < -pi) a += 2.0f * pi;
    return a;
}

static float lookYaw(const float* m9) {
    // Game CFrame layout (proven by calib.log yaw sweep):
    // columns = Right, Up, -Look. True look = -(m[2],m[5],m[8]).
    return atan2f(-m9[2], -m9[8]);
}

// ---- velext aim model (see velext_aim_findings.md) --------------------------
// Constants [P] = proven address in velext_v1.exe; [I] = inferred, tune later.
static const float kVelextTauPerUnit = 0.008f;      // [P] 0x1402B7FBC: tau = 8 ms * smoothing
static const float kVelextDtFallback = 0.004f;      // [P] 0x1402B7FB8: dt when no prior stamp
static const float kVelextDtMax = 0.1f;             // [P] 0x1402B7FF0: dt clamp hi
static const uint64_t kVelextGateNs = 4000000ULL;   // [P] 0x3D0900: >=4 ms between actuations
static const float kVelextAlphaEps = 0.002f;        // [P] 0x1402B7FAC: mouse alpha epsilon
static const float kVelextDotDeadzone = 0.99996f;   // [P] 0x1402B80F8: rotation deadzone (~0.51 deg)
static const float kVelextMouseDzPx = 1.5f;         // [P] 0x1402B8144: mouse screen deadzone (px)
static const float kVelextStepCapPxS = 14000.0f;    // [P] 0x1402B84B0 px/s: clamp(14000*dt,12,400)
static const float kVelextStepCapLo = 12.0f;        // [P] 0x1402B831C: cap lo
static const float kVelextStepCapHi = 400.0f;       // [P] 0x1402B8470: cap hi
static const float kVelextBpMoveDeg = 0.02f;        // [P] 0x1402B7FCC: back-pressure move (deg)
static const uint64_t kVelextBpTimeoutNs = 60000000ULL; // [P] 0x3938700: back-pressure timeout (60 ms)

// ---- Phase C: velext Mouse-method counts mapping (findings §5, live §14.5) --
// Ported 1:1 from fn_1400d9cc0 0x1400DAB52..0x1400DAD55 + fn_14010d580:
//   A = float2 @ base+0x8D857D8 (tracks the slider), B = @ +0x8D857E0 and
//   C = float @ +0x8D85820 (validated only), f = float @ MouseService+0x21C
//   (constant scale, 1.0 live; velext substitutes 1.0 when unreadable),
//   out1 = |f|*A.x, out2 = A.y*f.
static const uint64_t kVelextSensRvaA = 0x8D857D8ULL; // [P] rbase + float2 A
static const uint64_t kVelextSensRvaB = 0x8D857E0ULL; // [P] rbase + float2 B (validate)
static const uint64_t kVelextSensRvaC = 0x8D85820ULL; // [P] rbase + float C  (validate)
static const uint64_t kVelextMouseSvcSensRva = 0x21C; // [P] MouseService + float f
static const float kVelextSensInLo = 1e-3f;    // [P] 0x1402B7FA8: A/B/C/f input bound
static const float kVelextSensInHi = 100.0f;   // [P] 0x1402B83DC: input bound hi
static const float kVelextSensOutLo = 1e-4f;   // [P] 0x1402B7FA4: out1/out2 bound lo
static const float kVelextSensOutHi = 1000.0f; // [P] 0x1402B849C: out1/out2 bound hi
static const float kVelextSensXFac = 0.5f;     // [P] 0x1402B8074: countsX divisor
static const float kVelextSensYFac = 0.385f;   // [P] 0x1402B8050: countsY divisor
static const float kVelextMinCosP = 0.15f;     // [P] 0x1402B8008: min cos-pitch divisor
static const float kDeg2Rad = 0.0174532925f;   // [P] 0x1402B7FC8
static const float kRad2Deg = 57.2957795f;
static const int kVelextBpStaleMax = 6;        // [I] Phase C: silent releases => not locked
static const uint64_t kVelextBpProbeNs = 750000000ULL; // [I] Phase C: stale-hold probe gap

static uint64_t qpcNs() { return (uint64_t)(qpcSeconds() * 1e9); }

// Shared actuation stamp (velext 0x140334408). Only one actuation mode is
// active at a time, so the writer thread (mode 1) and the main loop (mode 0)
// can share it; a 0 value means "no previous actuation".
static std::atomic<uint64_t> g_velLastActNs{ 0 };

// alpha = 1 - exp(-dt/tau), tau = 0.008*max(1,smoothing); smoothing <= 1 snaps.
float velextAlphaFromDt(float smoothing, float dt) {
    if (dt < 0.0f) dt = 0.0f;
    if (dt > kVelextDtMax) dt = kVelextDtMax;
    float ts = smoothing > 1.0f ? smoothing : 1.0f;
    if (ts <= 1.0f) return 1.0f; // snap
    float tau = ts * kVelextTauPerUnit;
    if (tau <= 1e-4f) return 1.0f;
    float a = 1.0f - expf(-dt / tau);
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return a;
}

// 4 ms hard gate: false = too soon (no actuation, stamp untouched).
// true = stamp consumed now; dt/alpha describe the step to apply.
static bool velextActGate(uint64_t nowNs, uint64_t* lastNsOut, float* dtOut,
                          float* alphaOut, float smoothing) {
    uint64_t last = g_velLastActNs.load(std::memory_order_relaxed);
    if (last != 0 && nowNs - last < kVelextGateNs) return false;
    g_velLastActNs.store(nowNs, std::memory_order_relaxed);
    float dt = last == 0 ? kVelextDtFallback
                         : (float)((double)(nowNs - last) * 1e-9);
    if (lastNsOut) *lastNsOut = last;
    if (dtOut) *dtOut = dt;
    if (alphaOut) *alphaOut = velextAlphaFromDt(smoothing, dt);
    return true;
}

static float wrap180f(float d) {
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

// Camera yaw/pitch in degrees, velext mouse-path shape (yaw=atan2(-Lx,-Lz),
// pitch=asin(-Ly)) applied to this overlay's calib-proven look extraction
// look = -(m[2],m[5],m[8]). Only used for back-pressure deltas, so the sign
// convention does not matter.
static bool camYawPitchDeg(const float* m9, float* yawDeg, float* pitchDeg) {
    float y = m9[5];
    if (!(y >= -1.0f && y <= 1.0f)) return false; // NaN / bad read
    *yawDeg = atan2f(m9[2], m9[8]) * 57.2957795f;
    *pitchDeg = asinf(y) * 57.2957795f;
    return true;
}

// ---- Phase C: velext Mouse-method sensitivity + counts mapping ------------
// Ported from velext fn_14010d580 (sens reader) and fn_1400d9cc0
// 0x1400DAB52..0x1400DAD55 (angle -> counts). The pure math is shared with
// the -aimtest path so the shipped code and the test cannot drift apart.
struct VelextSens {
    bool ok = false;
    float out1 = 1.0f;
    float out2 = 1.0f;
};

// [P] fn_14010d580 input validation (0x1402B7FA8 lo / 0x1402B83DC hi): every
// one of A.x/A.y/B.x/B.y/C must be finite and 1e-3 <= |v| <= 100.
static bool velextSensValidIn(float v) {
    if (!std::isfinite(v)) return false;
    float a = fabsf(v);
    return a >= kVelextSensInLo && a <= kVelextSensInHi;
}

// [P] Sens reader fidelity: A = float2 at base+0x8D857D8, B at +0x8D857E0 and
// C at +0x8D85820 are hard inputs (any read/validation failure sends the
// caller back to the legacy aim.sens path). f at MouseService+0x21C is the
// constant scale and soft: velext substitutes 1.0 when it is unusable.
// out1 = |f|*A.x and out2 = A.y*f must land in [1e-4, 1000].
static VelextSens readVelextSens(Memory& mem, uint64_t base, uint64_t msvc) {
    VelextSens r;
    if (!base) return r;
    float A[2]{}, B[2]{}, C = 0.0f;
    if (!mem.read(base + kVelextSensRvaA, A) ||
        !mem.read(base + kVelextSensRvaB, B) ||
        !mem.read(base + kVelextSensRvaC, C))
        return r;
    if (!velextSensValidIn(A[0]) || !velextSensValidIn(A[1]) ||
        !velextSensValidIn(B[0]) || !velextSensValidIn(B[1]) ||
        !velextSensValidIn(C))
        return r;
    float f = 1.0f; // [P] velext default when MouseService+0x21C is unusable
    if (msvc) {
        float fv = 0.0f;
        if (mem.read(msvc + kVelextMouseSvcSensRva, fv) && velextSensValidIn(fv))
            f = fv;
    }
    float o1 = fabsf(f) * A[0];
    float o2 = A[1] * f;
    if (!(fabsf(o1) >= kVelextSensOutLo && fabsf(o1) <= kVelextSensOutHi) ||
        !(fabsf(o2) >= kVelextSensOutLo && fabsf(o2) <= kVelextSensOutHi))
        return r;
    r.ok = true;
    r.out1 = o1;
    r.out2 = o2;
    return r;
}

// [P] Angle -> mouse counts, 1:1 with 0x1400DAB52..0x1400DAD55:
//   cosp = max(0.15, sqrt(max(0, 1-p*p)))    ; p = pitch component
//   cx   = atan(eX/focal)*RAD2DEG/cosp/(0.5*out1)*alpha
//   cy   = atan(eY/focal)*RAD2DEG/(0.385*out2)*alpha
//   if alpha < 1: lim = clamp(14000*dt, 12, 400); clamp cx/cy to +/-lim.
// The cap base is dt, not alpha: register trace of the clamp site (xmm8) is
// the same value that feeds exp(-dt/tau) two blocks earlier
// (0x1400DA8DB..0x1400DA925); findings §5 hedges "xdt or xalpha" - it is dt.
static void velextMouseCounts(float eX, float eY, float focal, float p,
                              float out1, float out2, float alpha, float dt,
                              float* cxOut, float* cyOut) {
    float oneMinusP2 = 1.0f - p * p;
    if (oneMinusP2 < 0.0f) oneMinusP2 = 0.0f;
    float cosp = sqrtf(oneMinusP2);
    if (cosp < kVelextMinCosP) cosp = kVelextMinCosP;
    float cx = atanf(eX / focal) * kRad2Deg / cosp /
               (kVelextSensXFac * out1) * alpha;
    float cy = atanf(eY / focal) * kRad2Deg /
               (kVelextSensYFac * out2) * alpha;
    if (alpha < 1.0f) {
        float lim = kVelextStepCapPxS * dt;
        if (lim < kVelextStepCapLo) lim = kVelextStepCapLo;
        if (lim > kVelextStepCapHi) lim = kVelextStepCapHi;
        if (cx > lim) cx = lim;
        if (cx < -lim) cx = -lim;
        if (cy > lim) cy = lim;
        if (cy < -lim) cy = -lim;
    }
    *cxOut = cx;
    *cyOut = cy;
}

// [I] Phase C mouse-lock guard (prompt §3, live §14.4): the game hides the
// system cursor while it holds mouse lock. A visible cursor in the foreground
// game window means third-person free-look, where injected counts just fling
// the cursor around without turning the camera. GetCursorInfo reports the
// per-desktop global cursor state.
static bool mouseCursorHidden() {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return false;
    return (ci.flags & CURSOR_SHOWING) == 0;
}

bool upsertCfgKey(const char* key, const char* value) {
    FILE* f = fopen(exePath("overlay.cfg").c_str(), "r");
    std::string out;
    bool found = false;
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(key);
            if (strncmp(line, key, n) == 0 && line[n] == '=') {
                out += key;
                out += '=';
                out += value;
                out += '\n';
                found = true;
            } else {
                out += line;
            }
        }
        fclose(f);
    }
    if (!found) {
        out += key;
        out += '=';
        out += value;
        out += '\n';
    }
    f = fopen(exePath("overlay.cfg").c_str(), "w");
    if (!f) return false;
    fputs(out.c_str(), f);
    fclose(f);
    return true;
}

// ---- Đợt 1 widget helpers -------------------------------------------------
// cfg text for a vk that parseKey() understands (letters, F1-F24, names, hex).
static std::string keyCfgName(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return std::string(1, (char)vk);
    if (vk >= 0x70 && vk <= 0x87) {
        char b[8];
        snprintf(b, sizeof b, "F%d", vk - 0x6F);
        return b;
    }
    switch (vk) {
    case 0x2D: return "INSERT";
    case 0x2E: return "DELETE";
    case 0x24: return "HOME";
    case 0x23: return "END";
    case 0x21: return "PGUP";
    case 0x22: return "PGDN";
    case 0x20: return "SPACE";
    case 0x09: return "TAB";
    case 0xDC: return "\\";
    case 0x01: return "LMB";
    case 0x02: return "RMB";
    case 0x04: return "MMB";
    case 0x05: return "XBUTTON1";
    case 0x06: return "XBUTTON2";
    }
    char b[8];
    snprintf(b, sizeof b, "0x%02X", vk);
    return b;
}



// ---- aimbot config (overlay.cfg), mirrors velext profile knobs ----
bool cfgAimEnabled = true;
int cfgAimKey = 'T';
static int cfgAimKeyMode = 1;   // 0 = hold key, 1 = toggle
int cfgAimFov = 150;     // px radius around viewport center
static int cfgAimSmooth = 1;    // snap divisor (velext menu: 1.0)
static int cfgAimMode = 0;      // 0 = mouse move, 1 = memory write
static int cfgAimSticky = 0;    // menu: off (first-match is stable alone)
static float cfgAimStickTime = 4.0f; // s: max lock age, 0 = unlimited
static int cfgAimPredict = 1;   // lead moving targets by velocity
static float cfgAimPScale = 1.02f; // menu value (lead = vel * dt * scale)
static float cfgAimDynK = 0.42f;  // velext: dynamic FOV radius = max(2, k*d+2)
static int cfgAimNearest = 0;     // 0 = first-match (velext), 1 = nearest
static float cfgAimOverextend = 0.0f; // branch B: aim past head, 0.45 to mirror
static float cfgAimHeight = 1.5f; // aim point above HRP (studs; head~2.8)
static float cfgAimMaxDist = 2000.0f; // studs, 0 = unlimited
static float cfgAimDeadzone = 3.0f;   // px: don't actuate inside this
static int cfgAimIgnoreTeam = 0;  // menu: off
static int cfgAimInterval = 0;      // ms: min gap between actuations (0 = every frame)
static int cfgAimBone = 0;          // 0 = Head, 1 = HRP+height, 2-5 limbs
static int cfgAimNPCs = 1;          // also target NPC/mob Models (orange)
static float cfgAimTSmooth = 0.12f; // LEGACY (aim.pid=1): 1-pole t = 1-this
static float cfgAimSens = 1.0f;     // extra gain
static float cfgAimGameSens = 1.0f; // px per mouse count (calib F8)
static int cfgAimPid = 0;           // 0 = velext filter (default), 1 = legacy PID/AB filters
static float cfgAimMemSmooth = 0.18f; // LEGACY (aim.pid=1): rotation slerp smoothing
float cfgAimSmoothing = 5.0f;  // velext model: tau = 0.008*max(1,s) s; <=1 snaps (1..20)
static int cfgAimRate = 500; // mode-1 writer thread poll Hz (legacy path writes at this rate)
// ---- triggerbot (velext trig.* recipe: auto-click when target centered)
int cfgTrigEnabled = 0;
int cfgTrigKey = 0x06;       // XBUTTON2 like velext default
int cfgTrigKeyMode = 1;      // 0 = hold, 1 = toggle
static int cfgTrigDelay = 45;       // ms: arm delay before DOWN
static int cfgTrigCooldown = 90;    // ms: gap between shots
static int cfgTrigPress = 35;       // ms: DOWN-hold duration
static int cfgTrigPad = 2;
// ---- autoclicker: fixed-interval clicks for a set burst duration ----
int cfgACEnabled = 0;
int cfgACKey = 'V';
static int cfgACKeyMode = 1;    // 0 = hold, 1 = toggle
static int cfgACInterval = 100; // ms between click starts
static int cfgACDuration = 2000; // ms per activation, 0 = until key off
static int cfgACPress = 35;     // ms: DOWN-hold duration          // px: fire when target within this
static int cfgChatGuard = 1;      // Exunys typing check: no keys/aim in chat
// ---- player mods (memory writes, WPM proven) ----
int cfgPlyMode = 1;         // 0 = Humanoid property, 1 = velocity (keeps WalkSpeed legit)
int cfgPlySpeed = 0;
float cfgPlySpeedVal = 32.0f;
int cfgPlyJump = 0;
float cfgPlyJumpVal = 100.0f;
// How the jump hack is applied:
//   0 = property, auto (write JumpPower AND the equivalent JumpHeight, so it
//       works whether or not the game uses UseJumpPower)  <- default
//   1 = property, JumpPower only (exactly what the reference external does)
//   2 = velocity impulse on Space (no property touched at all)
int cfgPlyJumpMode = 0;
int cfgPlySpeedKey = 'B';   // toggle speed write
int cfgPlyJumpKey = 'N';    // toggle jump write
static int cfgPlySdnKey = 0xDB;     // '[' : speed -step
static int cfgPlySupKey = 0xDD;     // ']' : speed +step
static int cfgPlyJdnKey = 0xBA;     // ';' : jump -step
static int cfgPlyJupKey = 0xDE;     // '\'' : jump +step
int cfgFlyEnabled = 0;       // fly (anchored HRP + position steps)
int cfgFlyKey = 'F';         // toggle fly

// Menu checkbox = ARM, hotkey = ACTIVATE (per user's spec): tick "Fly" and F
// starts the flight; untick it and F is dead ("spam F freely, no flying").
// These runtime flags are session-only on purpose - a fresh start is never
// already flying/speed-hacked until the key is pressed.
int g_speedActive = 0;       // B key, requires cfgPlySpeed armed
int g_jumpActive = 0;        // N key, requires cfgPlyJump armed
int g_flyActive = 0;         // F key, requires cfgFlyEnabled armed
// The low bit of GetAsyncKeyState ("pressed since last read") is shared with
// every other process on the desktop and Roblox polls keys too, so an edge can
// be eaten before we look. Track the rising edge of the "down" bit ourselves.
static bool g_prevSpeedKey = false, g_prevJumpKey = false, g_prevFlyKey = false;
float cfgFlySpeed = 60.0f;   // studs/s
// Teleport is the menu's Teleport tab now: the old "H = nearest player" hotkey
// and its tp.key config key are gone (the tab lists everyone and teleports to
// the row you click).
// ---- clickable menu (overlay.cfg: menu.collapsed / menu.pos / menu.tab)
// The panel always exists while the overlay is shown: the title-bar triangle
// (or double-click on the title) collapses it to the title bar. State lives in
// menu.collapsed; K (toggle) still hides the whole overlay.
static int cfgMenuCollapsed = 0;     // 1 = collapsed to the title bar
int cfgMenuTab = 0;           // active tab (0 Body 1 Combat 2 Visual 3 TP 4 World 5 Server 6 Tools 7 Misc)
uint64_t g_tpTargetPrim = 0;  // Teleport tab: target HRP, consumed by loop
DWORD g_tpRequestTick = 0;    // when that click happened
int cfgTpNpc = 0;             // Teleport tab: also list NPCs/mobs
// Roblox pins the cursor to a 1px box in the middle of the screen while the
// player has camera control. A pinned cursor can never reach the panel, so no
// click would land. While the panel is EXPANDED we drop that clip and put the
// game's own box back when it is collapsed (menu.unlockmouse).
int cfgMenuUnlockMouse = 1;   // 1 = free the cursor while the panel is up
float cfgMenuX = 490.0f;      // panel position (ImGui/client units = the
float cfgMenuY = 80.0f;       // 96dpi space this process sees when scaled)
// ---- Đợt 1: watermark HUD, stream-proof, panic, keybind editor, profiles ----
int g_toggleVk = VK_INSERT;    // hide/show key (was a main() local)
int cfgHudOn = 1;              // watermark + status line top-left
int cfgHudFeats = 1;           // second line: which features are armed/on
int cfgWda = 0;                // 1 = hide this window from screen capture
int cfgPanicKey = VK_DELETE;   // 0 = disabled; turns everything OFF
int cfgAutoProfile = 0;        // auto-load the profile bound to a place
// The panel never takes keyboard focus (WS_EX_NOACTIVATE is what keeps the game
// focused while you click it), so ImGui text boxes can never receive WM_CHAR:
// keybind capture runs in the main loop instead, where GetAsyncKeyState works.
// (the g_capVk/g_capCfg/g_capT0 slots live above keyBindWidget)
std::vector<Waypoint> g_ways;   // overlay.cfg: wp.<n>=<name>|<x>,<y>,<z>
// Waypoint / click-teleport request (the loop does the write, never the menu).
Vec3 g_tpTargetPos{};
DWORD g_tpPosTick = 0;
char g_tpPosName[48] = { 0 };
// ---- Visual tab (ESP v2 + radar). Colors are packed 0xRRGGBB ----
int cfgEspOn = 1;        // master switch for the whole ESP
int cfgEspBox = 1;
int cfgEspFill = 0;      // translucent fill (the "poor man's chams")
int cfgEspName = 1;
int cfgEspDist = 1;
int cfgEspHp = 1;
int cfgEspHead = 0;
int cfgEspLine = 0;      // snapline from the bottom of the screen
int cfgEspSkel = 0;      // skeleton (head + 4 limb anchors, already read)
int cfgEspOof = 0;       // off-screen arrows
int cfgEspNpc = 1;
int cfgEspMaxDist = 0;   // studs, 0 = no limit
int cfgEspColEnemy = 0xFF3C3C;
int cfgEspColTeam = 0x50FF50;
int cfgEspColNpc = 0xFFAA28;
int cfgRadarOn = 0;
int cfgRadarRange = 300; // studs shown from the centre to the edge
int cfgRadarSize = 130;  // px square
static int g_popupHits = 0;     // diag: ImGui popup windows marked this frame
const char* g_colHover = nullptr;  // diag: which colour bar ImGui hovers
static int g_espVmDiag = 0;               // diag: last frame had a valid view matrix
// Palette the Visual tab colour buttons cycle through (see the colour UI there).
const int kColorPal[11] = {
    0xFF3C3C, 0xFF8C28, 0xFFD23C, 0x8CFF3C, 0x3CFF6E, 0x3CFFF0,
    0x3C9CFF, 0x7A3CFF, 0xFF3CE0, 0xFFFFFF, 0x0A0A0A,
};
// (no separate hotkey checkbox anymore: the feature checkbox is the arm switch)
bool g_menuPosForce = false;  // re-place the panel next frame
static float g_menuW = 360.0f;       // last drawn panel size (screen bounds check)
static float g_menuH = 420.0f;
static float g_savedMenuW = 0.0f;    // size already stored in the cfg
static float g_savedMenuH = 0.0f;
float g_plyCurSpeed = 0.0f;  // last-read actual WalkSpeed (revert watch)
// Reference-tool behaviour: remember what the Humanoid had before we touched it
// and put it back when the toggle is switched off ("Restored when you turn it
// off."). The shadow copy is restored with the same value.
static uint64_t g_plyRestHum = 0;
static float g_plyRestWS = 0.0f;
static float g_plyRestWSShadow = 0.0f;
static float g_plyRestJP = 0.0f;
static float g_plyRestJH = 0.0f;
static bool g_plyWSValid = false;
static bool g_plyJPValid = false;
static bool g_plyJHValid = false;

// ---- World (Đợt 2): lighting / atmosphere / effects / terrain / camera -------
// Config flags live up here because loadAimCfg() parses them; the tick/apply
// machinery sits further down (after the cursor-confinement helpers).
int   cfgWorldFull = 0;          // world.full     fullbright
float cfgWorldBright = 2.0f;     // world.bright   Lighting.Brightness (1..3)
int   cfgWorldFog = 0;           // world.fog      anti-fog
int   cfgWorldShadows = 0;       // world.shadows  GlobalShadows = 0
int   cfgWorldExpo = 0;          // world.expo     exposure override on
float cfgWorldExpoVal = 0.0f;    // world.expoval  (-2..2)
int   cfgWorldClock = 0;         // world.clock    time-of-day on
float cfgWorldClockVal = 14.0f;  // world.clockval (hours 0..24)
int   cfgWorldFx = 0;            // world.fx       bloom/blur/CC off
int   cfgWorldAtmo = 0;          // world.atmo     Atmosphere off
int   cfgWorldWater = 0;         // world.water    clear + flat water
int   cfgWorldGrass = 0;         // world.grass    no grass
int   cfgWorldFov = 0;           // world.fovon    FOV override
float cfgWorldFovVal = 90.0f;    // world.fov      (70..120)
int   cfgWorldZoom = 0;          // world.zoom     far zoom

// ---- Server tab (Đợt 4): session / hop / watchdog / anti-AFK ---------------
// Same placement as the World flags: parsed by loadAimCfg(), read by srvTick()
// and the tab UI. Nothing here is ever written into the game (see srvTick).
int   cfgSrvAntiAfk = 0;         // srv.antiafk    0 = off
int   cfgSrvAfkMin = 12;         // srv.afkmin     5..15 minutes
int   cfgSrvAutoRejoin = 0;      // srv.autorejoin watchdog on unload
int   cfgSrvHopMin = 5;          // srv.hopmin     0..50 players
int   cfgSrvHopFresh = 1;        // srv.hopfresh   1 = drop full servers
int   cfgSrvGeo = 0;             // srv.geo        default OFF (HTTP out)
static std::string cfgSrvLastJob;       // srv.lastjob    last job we joined
static uint64_t cfgSrvLastPlace = 0;    // srv.lastplace  its placeId
static bool srvValidJob(const std::string& s); // defined with the Server module

static int cfgInt(const char* line, const char* key, int dflt) {
    size_t n = strlen(key);
    if (strncmp(line, key, n) == 0 && line[n] == '=') return atoi(line + n + 1);
    return dflt;
}

static float cfgFloat(const char* line, const char* key, float dflt) {
    size_t n = strlen(key);
    if (strncmp(line, key, n) == 0 && line[n] == '=') return (float)atof(line + n + 1);
    return dflt;
}

static void loadAimCfg() {
    FILE* f = fopen(exePath("overlay.cfg").c_str(), "r");
    if (!f) return;
    char line[128];
    bool legacySmooth = false; // old smoothing keys present -> one log line
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "aim.enabled=", 12) == 0)
            cfgAimEnabled = atoi(line + 12) ? true : false;
        if (strncmp(line, "aim.key=", 8) == 0) {
            int v = parseKey(line + 8);
            if (v) cfgAimKey = v;
        }
        cfgAimKeyMode = cfgInt(line, "aim.keymode", cfgAimKeyMode);
        cfgAimFov = cfgInt(line, "aim.fov", cfgAimFov);
        cfgAimSmooth = cfgInt(line, "aim.smooth", cfgAimSmooth);
        cfgAimMode = cfgInt(line, "aim.mode", cfgAimMode);
        cfgAimSticky = cfgInt(line, "aim.sticky", cfgAimSticky);
        cfgAimStickTime = cfgFloat(line, "aim.sticktime", cfgAimStickTime);
        cfgAimPredict = cfgInt(line, "aim.predict", cfgAimPredict);
        cfgAimPScale = cfgFloat(line, "aim.pscale", cfgAimPScale);
        cfgAimDynK = cfgFloat(line, "aim.dynk", cfgAimDynK);
        cfgAimNearest = cfgInt(line, "aim.nearest", cfgAimNearest);
        cfgAimOverextend = cfgFloat(line, "aim.overextend", cfgAimOverextend);
        cfgAimHeight = cfgFloat(line, "aim.height", cfgAimHeight);
        cfgAimMaxDist = cfgFloat(line, "aim.maxdist", cfgAimMaxDist);
        cfgAimDeadzone = cfgFloat(line, "aim.deadzone", cfgAimDeadzone);
        cfgAimIgnoreTeam = cfgInt(line, "aim.ignoreteam", cfgAimIgnoreTeam);
        cfgAimInterval = cfgInt(line, "aim.interval", cfgAimInterval);
        cfgAimBone = cfgInt(line, "aim.bone", cfgAimBone);
        cfgAimNPCs = cfgInt(line, "aim.npcs", cfgAimNPCs);
        cfgAimTSmooth = cfgFloat(line, "aim.tsmooth", cfgAimTSmooth);
        cfgAimSens = cfgFloat(line, "aim.sens", cfgAimSens);
        cfgAimGameSens = cfgFloat(line, "aim.gamesens", cfgAimGameSens);
        cfgAimPid = cfgInt(line, "aim.pid", cfgAimPid);
        cfgAimMemSmooth = cfgFloat(line, "aim.memsmooth", cfgAimMemSmooth);
        cfgAimRate = cfgInt(line, "aim.rate", cfgAimRate);
        cfgAimSmoothing = cfgFloat(line, "aim.smoothing", cfgAimSmoothing);
        // "aim.smooth=" is length 11 and does not collide with "aim.smoothing="
        if (!legacySmooth &&
            (strncmp(line, "aim.tsmooth=", 12) == 0 ||
             strncmp(line, "aim.memsmooth=", 14) == 0 ||
             strncmp(line, "aim.smooth=", 11) == 0))
            legacySmooth = true;
        if (strncmp(line, "trig.enabled=", 13) == 0)
            cfgTrigEnabled = atoi(line + 13) ? 1 : 0;
        if (strncmp(line, "trig.key=", 9) == 0) {
            int v = parseKey(line + 9);
            if (v) cfgTrigKey = v;
        }
        cfgTrigKeyMode = cfgInt(line, "trig.keymode", cfgTrigKeyMode);
        cfgTrigDelay = cfgInt(line, "trig.delay", cfgTrigDelay);
        cfgTrigCooldown = cfgInt(line, "trig.cooldown", cfgTrigCooldown);
        cfgTrigPress = cfgInt(line, "trig.press", cfgTrigPress);
        cfgTrigPad = cfgInt(line, "trig.pad", cfgTrigPad);
        if (strncmp(line, "autoclick.key=", 14) == 0) {
            int v = parseKey(line + 14);
            if (v) cfgACKey = v;
        }
        if (strncmp(line, "autoclick.enabled=", 18) == 0)
            cfgACEnabled = atoi(line + 18) ? 1 : 0;
        cfgACKeyMode = cfgInt(line, "autoclick.keymode", cfgACKeyMode);
        cfgACInterval = cfgInt(line, "autoclick.interval", cfgACInterval);
        cfgACDuration = cfgInt(line, "autoclick.duration", cfgACDuration);
        cfgACPress = cfgInt(line, "autoclick.press", cfgACPress);
        if (strncmp(line, "input.chatguard=", 16) == 0)
            cfgChatGuard = atoi(line + 16) ? 1 : 0;
        if (strncmp(line, "ply.speed=", 10) == 0)
            cfgPlySpeed = atoi(line + 10) ? 1 : 0;
        cfgPlySpeedVal = cfgFloat(line, "ply.speedval", cfgPlySpeedVal);
        cfgPlyMode = cfgInt(line, "ply.mode", cfgPlyMode);
        if (strncmp(line, "ply.jump=", 9) == 0)
            cfgPlyJump = atoi(line + 9) ? 1 : 0;
        cfgPlyJumpVal = cfgFloat(line, "ply.jumpval", cfgPlyJumpVal);
        cfgPlyJumpMode = cfgInt(line, "ply.jumpmode", cfgPlyJumpMode);
        if (strncmp(line, "ply.speedkey=", 13) == 0) {
            int v = parseKey(line + 13);
            if (v) cfgPlySpeedKey = v;
        }
        if (strncmp(line, "ply.jumpkey=", 12) == 0) {
            int v = parseKey(line + 12);
            if (v) cfgPlyJumpKey = v;
        }
        if (strncmp(line, "ply.sdnkey=", 11) == 0) {
            int v = parseKey(line + 11);
            if (v) cfgPlySdnKey = v;
        }
        if (strncmp(line, "ply.supkey=", 11) == 0) {
            int v = parseKey(line + 11);
            if (v) cfgPlySupKey = v;
        }
        if (strncmp(line, "ply.jdnkey=", 11) == 0) {
            int v = parseKey(line + 11);
            if (v) cfgPlyJdnKey = v;
        }
        if (strncmp(line, "ply.jupkey=", 11) == 0) {
            int v = parseKey(line + 11);
            if (v) cfgPlyJupKey = v;
        }
        if (strncmp(line, "fly.enabled=", 12) == 0)
            cfgFlyEnabled = atoi(line + 12) ? 1 : 0;
        if (strncmp(line, "fly.key=", 8) == 0) {
            // written by the menu's "hotkey" checkbox: NONE disables it on purpose
            cfgFlyKey = parseKey(line + 8);
        }
        cfgFlySpeed = cfgFloat(line, "fly.speed", cfgFlySpeed);
        // menu.show / menu.key (INSERT) are legacy since 2026-10-20: the
        // title-bar collapse triangle replaced them. Old cfg lines are ignored.
        if (strncmp(line, "menu.collapsed=", 15) == 0)
            cfgMenuCollapsed = atoi(line + 15) ? 1 : 0;
        if (strncmp(line, "menu.pos=", 9) == 0)
            sscanf(line + 9, "%f,%f", &cfgMenuX, &cfgMenuY);
        if (strncmp(line, "menu.size=", 10) == 0) {
            if (sscanf(line + 10, "%f,%f", &g_menuW, &g_menuH) == 2) {
                g_savedMenuW = g_menuW;
                g_savedMenuH = g_menuH;
            }
        }
        if (strncmp(line, "menu.tab=", 9) == 0) {
            int v = atoi(line + 9);
            cfgMenuTab = (v >= 0 && v < 8) ? v : 0;
        }
        if (strncmp(line, "menu.unlockmouse=", 17) == 0)
            cfgMenuUnlockMouse = atoi(line + 17) ? 1 : 0;
        if (strncmp(line, "tp.npc=", 7) == 0)
            cfgTpNpc = atoi(line + 7) ? 1 : 0;
        if (strncmp(line, "hud.on=", 7) == 0)
            cfgHudOn = atoi(line + 7) ? 1 : 0;
        if (strncmp(line, "hud.feats=", 10) == 0)
            cfgHudFeats = atoi(line + 10) ? 1 : 0;
        if (strncmp(line, "misc.wda=", 9) == 0)
            cfgWda = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "profile.auto=", 13) == 0)
            cfgAutoProfile = atoi(line + 13) ? 1 : 0;
        if (strncmp(line, "panic.key=", 10) == 0) {
            // parseKey returns 0 for "none": keep that meaning for panic
            cfgPanicKey = parseKey(line + 10);
        }
        if (strncmp(line, "esp.on=", 7) == 0)
            cfgEspOn = atoi(line + 7) ? 1 : 0;
        if (strncmp(line, "esp.box=", 8) == 0)
            cfgEspBox = atoi(line + 8) ? 1 : 0;
        if (strncmp(line, "esp.fill=", 9) == 0)
            cfgEspFill = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "esp.name=", 9) == 0)
            cfgEspName = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "esp.dist=", 9) == 0)
            cfgEspDist = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "esp.hp=", 7) == 0)
            cfgEspHp = atoi(line + 7) ? 1 : 0;
        if (strncmp(line, "esp.head=", 9) == 0)
            cfgEspHead = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "esp.line=", 9) == 0)
            cfgEspLine = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "esp.skel=", 9) == 0)
            cfgEspSkel = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "esp.oof=", 8) == 0)
            cfgEspOof = atoi(line + 8) ? 1 : 0;
        if (strncmp(line, "esp.npc=", 8) == 0)
            cfgEspNpc = atoi(line + 8) ? 1 : 0;
        if (strncmp(line, "esp.maxdist=", 12) == 0)
            cfgEspMaxDist = atoi(line + 12);
        if (strncmp(line, "esp.colenemy=", 13) == 0)
            cfgEspColEnemy = atoi(line + 13) & 0xFFFFFF;
        if (strncmp(line, "esp.colteam=", 12) == 0)
            cfgEspColTeam = atoi(line + 12) & 0xFFFFFF;
        if (strncmp(line, "esp.colnpc=", 11) == 0)
            cfgEspColNpc = atoi(line + 11) & 0xFFFFFF;
        if (strncmp(line, "radar.on=", 9) == 0)
            cfgRadarOn = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "radar.range=", 12) == 0)
            cfgRadarRange = atoi(line + 12);
        if (strncmp(line, "radar.size=", 11) == 0)
            cfgRadarSize = atoi(line + 11);
        // ---- World tab (Đợt 2) ----
        if (strncmp(line, "world.full=", 11) == 0)
            cfgWorldFull = atoi(line + 11) ? 1 : 0;
        cfgWorldBright = cfgFloat(line, "world.bright", cfgWorldBright);
        if (strncmp(line, "world.fog=", 10) == 0)
            cfgWorldFog = atoi(line + 10) ? 1 : 0;
        if (strncmp(line, "world.shadows=", 14) == 0)
            cfgWorldShadows = atoi(line + 14) ? 1 : 0;
        if (strncmp(line, "world.expo=", 11) == 0)
            cfgWorldExpo = atoi(line + 11) ? 1 : 0;
        cfgWorldExpoVal = cfgFloat(line, "world.expoval", cfgWorldExpoVal);
        if (strncmp(line, "world.clock=", 12) == 0)
            cfgWorldClock = atoi(line + 12) ? 1 : 0;
        cfgWorldClockVal = cfgFloat(line, "world.clockval", cfgWorldClockVal);
        if (strncmp(line, "world.fx=", 9) == 0)
            cfgWorldFx = atoi(line + 9) ? 1 : 0;
        if (strncmp(line, "world.atmo=", 11) == 0)
            cfgWorldAtmo = atoi(line + 11) ? 1 : 0;
        if (strncmp(line, "world.water=", 12) == 0)
            cfgWorldWater = atoi(line + 12) ? 1 : 0;
        if (strncmp(line, "world.grass=", 12) == 0)
            cfgWorldGrass = atoi(line + 12) ? 1 : 0;
        if (strncmp(line, "world.fovon=", 12) == 0)
            cfgWorldFov = atoi(line + 12) ? 1 : 0;
        cfgWorldFovVal = cfgFloat(line, "world.fov", cfgWorldFovVal);
        if (strncmp(line, "world.zoom=", 11) == 0)
            cfgWorldZoom = atoi(line + 11) ? 1 : 0;
        // ---- Server tab (Đợt 4) ----
        if (strncmp(line, "srv.antiafk=", 12) == 0)
            cfgSrvAntiAfk = atoi(line + 12) ? 1 : 0;
        cfgSrvAfkMin = cfgInt(line, "srv.afkmin", cfgSrvAfkMin);
        if (strncmp(line, "srv.autorejoin=", 15) == 0)
            cfgSrvAutoRejoin = atoi(line + 15) ? 1 : 0;
        cfgSrvHopMin = cfgInt(line, "srv.hopmin", cfgSrvHopMin);
        if (strncmp(line, "srv.hopfresh=", 13) == 0)
            cfgSrvHopFresh = atoi(line + 13) ? 1 : 0;
        if (strncmp(line, "srv.geo=", 8) == 0)
            cfgSrvGeo = atoi(line + 8) ? 1 : 0;
        if (strncmp(line, "srv.lastjob=", 12) == 0) {
            std::string j(line + 12);
            while (!j.empty() && (j.back() == '\n' || j.back() == '\r' ||
                                  j.back() == ' '))
                j.pop_back();
            if (srvValidJob(j)) cfgSrvLastJob = j;
        }
        if (strncmp(line, "srv.lastplace=", 14) == 0)
            cfgSrvLastPlace = strtoull(line + 14, nullptr, 10);
    }
    fclose(f);
    if (legacySmooth)
        printf("[aimcfg] legacy smoothing key ignored (aim.smooth/aim.tsmooth/"
               "aim.memsmooth); using aim.smoothing=%.2f\n", cfgAimSmoothing);
    if (cfgAimFov < 20) cfgAimFov = 20;
    if (cfgAimFov > 800) cfgAimFov = 800;
    if (cfgAimSmooth < 1) cfgAimSmooth = 1;
    if (cfgAimSmooth > 30) cfgAimSmooth = 30;
    if (cfgAimSmoothing < 1.0f) cfgAimSmoothing = 1.0f;
    if (cfgAimSmoothing > 20.0f) cfgAimSmoothing = 20.0f;
    if (cfgAimPScale < 0) cfgAimPScale = 0;
    if (cfgAimPScale > 10) cfgAimPScale = 10;
    if (cfgAimDynK < 0) cfgAimDynK = 0;
    if (cfgAimDynK > 5) cfgAimDynK = 5;
    if (cfgAimNearest != 0) cfgAimNearest = 1;
    if (cfgAimOverextend < 0) cfgAimOverextend = 0;
    if (cfgAimOverextend > 1) cfgAimOverextend = 1;
    if (cfgAimHeight < -3) cfgAimHeight = -3;
    if (cfgAimHeight > 6) cfgAimHeight = 6;
    if (cfgAimKeyMode != 0) cfgAimKeyMode = 1;
    if (cfgAimBone < 0 || cfgAimBone > 5) cfgAimBone = 0;
    if (cfgAimNPCs != 0) cfgAimNPCs = 1;
    if (cfgAimStickTime < 0) cfgAimStickTime = 0;
    if (cfgAimStickTime > 30) cfgAimStickTime = 30;
    if (cfgAimMode < 0 || cfgAimMode > 2) cfgAimMode = 0;
    if (cfgTrigKeyMode != 0) cfgTrigKeyMode = 1;
    if (cfgTrigDelay < 0) cfgTrigDelay = 0;
    if (cfgTrigDelay > 1000) cfgTrigDelay = 1000;
    if (cfgTrigCooldown < 0) cfgTrigCooldown = 0;
    if (cfgTrigPress < 5) cfgTrigPress = 5;
    if (cfgTrigPad < 0) cfgTrigPad = 0;
    if (cfgAimInterval < 0) cfgAimInterval = 0;
    if (cfgACInterval < 10) cfgACInterval = 10;
    if (cfgACInterval > 5000) cfgACInterval = 5000;
    if (cfgACDuration < 0) cfgACDuration = 0;
    if (cfgACDuration > 3600000) cfgACDuration = 3600000;
    if (cfgACPress < 5) cfgACPress = 5;
    if (cfgACPress > 500) cfgACPress = 500;
    if (cfgACPress >= cfgACInterval) cfgACPress = cfgACInterval / 2;
    if (cfgACKeyMode != 0) cfgACKeyMode = 1;
    if (cfgPlySpeedVal < 0) cfgPlySpeedVal = 0;
    if (cfgPlySpeedVal > 500) cfgPlySpeedVal = 500;
    if (cfgPlyJumpVal < 0) cfgPlyJumpVal = 0;
    if (cfgPlyJumpVal > 500) cfgPlyJumpVal = 500;
    if (cfgPlyMode < 0 || cfgPlyMode > 1) cfgPlyMode = 1;
    if (cfgFlySpeed < 5) cfgFlySpeed = 5;
    if (cfgFlySpeed > 500) cfgFlySpeed = 500;
    if (cfgFlyEnabled != 0) cfgFlyEnabled = 1;
    if (cfgAimTSmooth < 0) cfgAimTSmooth = 0;
    if (cfgAimTSmooth > 0.99f) cfgAimTSmooth = 0.99f;
    if (cfgAimSens <= 0) cfgAimSens = 1.0f;
    if (cfgAimGameSens < 0) cfgAimGameSens = 0;
    if (cfgAimPid != 0) cfgAimPid = 1;
    if (cfgAimMemSmooth < 0) cfgAimMemSmooth = 0;
    if (cfgAimMemSmooth > 0.99f) cfgAimMemSmooth = 0.99f;
    if (cfgAimRate < 30) cfgAimRate = 30;
    if (cfgAimRate > 1000) cfgAimRate = 1000;
    // World tab: keep every persisted value inside its slider range.
    if (cfgWorldBright < 1.0f) cfgWorldBright = 1.0f;
    if (cfgWorldBright > 3.0f) cfgWorldBright = 3.0f;
    if (cfgWorldExpoVal < -2.0f) cfgWorldExpoVal = -2.0f;
    if (cfgWorldExpoVal > 2.0f) cfgWorldExpoVal = 2.0f;
    if (cfgWorldClockVal < 0.0f) cfgWorldClockVal = 0.0f;
    if (cfgWorldClockVal > 24.0f) cfgWorldClockVal = 24.0f;
    if (cfgWorldFovVal < 70.0f) cfgWorldFovVal = 70.0f;
    if (cfgWorldFovVal > 120.0f) cfgWorldFovVal = 120.0f;
    // Server tab: same rule for the persisted hop/anti-AFK settings.
    if (cfgSrvAfkMin < 5) cfgSrvAfkMin = 5;
    if (cfgSrvAfkMin > 15) cfgSrvAfkMin = 15;
    if (cfgSrvHopMin < 0) cfgSrvHopMin = 0;
    if (cfgSrvHopMin > 50) cfgSrvHopMin = 50;
    if (cfgSrvAntiAfk != 0) cfgSrvAntiAfk = 1;
    if (cfgSrvAutoRejoin != 0) cfgSrvAutoRejoin = 1;
    if (cfgSrvHopFresh != 0) cfgSrvHopFresh = 1;
    if (cfgSrvGeo != 0) cfgSrvGeo = 1;
}

// ---- profiles ---------------------------------------------------------------
static void loadWaypoints(); // defined below, called by loadProfileSlot
// A profile is a byte copy of the live overlay.cfg, so it carries every setting
// without a hand-written serializer that could drift. Slots are fixed names
// (p1..p9) because the panel cannot host a text box - see the g_capVk note.
static bool copyFileRaw(const char* src, const char* dst) {
    FILE* a = fopen(src, "rb");
    if (!a) return false;
    FILE* b = fopen(dst, "wb");
    if (!b) {
        fclose(a);
        return false;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        if (fwrite(buf, 1, n, b) != n) {
            fclose(a);
            fclose(b);
            return false;
        }
    fclose(a);
    fclose(b);
    return true;
}
static std::string profileFile(const std::string& slot) {
    return g_exeDir + "\\profiles\\" + slot + ".cfg";
}
bool profileExists(const std::string& slot) {
    FILE* f = fopen(profileFile(slot).c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
bool saveProfileSlot(const std::string& slot) {
    CreateDirectoryA(exePath("profiles").c_str(), nullptr);
    return copyFileRaw(exePath("overlay.cfg").c_str(), profileFile(slot).c_str());
}
bool loadProfileSlot(const std::string& slot) {
    if (!copyFileRaw(profileFile(slot).c_str(), exePath("overlay.cfg").c_str())) return false;
    loadAimCfg();                 // re-parse the file we just swapped in
    g_toggleVk = loadToggleKey();
    loadWaypoints();
    return true;
}
// First value of a string key in overlay.cfg ("" when absent).
static std::string cfgStrValue(const std::string& key) {
    FILE* f = fopen(exePath("overlay.cfg").c_str(), "r");
    if (!f) return "";
    std::string pat = key + "=";
    std::string v;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, pat.c_str(), pat.size()) == 0) {
            v = line + pat.size();
            while (!v.empty() && (v.back() == '\n' || v.back() == '\r' ||
                                  v.back() == ' ' || v.back() == '\t'))
                v.pop_back();
        }
    }
    fclose(f);
    return v;
}
void setPlaceProfile(uint64_t place, const std::string& slot) {
    char key[48];
    snprintf(key, sizeof key, "profile.place.%llu", (unsigned long long)place);
    upsertCfgKey(key, slot.c_str());
}

// ---- waypoints (overlay.cfg: wp.<n>=<name>|<x>,<y>,<z>) ---------------------
void saveWaypoints() {
    FILE* f = fopen(exePath("overlay.cfg").c_str(), "r");
    std::string out;
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f))
            if (strncmp(line, "wp.", 3) != 0) out += line;
        fclose(f);
    }
    for (size_t i = 0; i < g_ways.size(); ++i) {
        char b[192];
        snprintf(b, sizeof b, "wp.%d=%s|%.1f,%.1f,%.1f\n", (int)i,
                 g_ways[i].name.c_str(), g_ways[i].p.x, g_ways[i].p.y,
                 g_ways[i].p.z);
        out += b;
    }
    f = fopen(exePath("overlay.cfg").c_str(), "w");
    if (!f) return;
    fputs(out.c_str(), f);
    fclose(f);
}
static void loadWaypoints() {
    g_ways.clear();
    FILE* f = fopen(exePath("overlay.cfg").c_str(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "wp.", 3) != 0) continue;
        const char* eq = strchr(line, '=');
        if (!eq) continue;
        char nm[64] = { 0 };
        float x = 0, y = 0, z = 0;
        if (sscanf(eq + 1, "%63[^|]|%f,%f,%f", nm, &x, &y, &z) == 4) {
            Waypoint w;
            w.name = nm;
            w.p = Vec3{ x, y, z };
            g_ways.push_back(w);
        }
    }
    fclose(f);
    if (g_ways.size() > 64) g_ways.resize(64);
}

// ---- hitbox expander (profile-driven, per PlaceId) ----
// profiles/<placeid>.cfg: hitbox.enabled/name/size/collide.
// Path/name rules only (addresses re-resolve every tick like players).
int cfgHitboxEnabled = 0;
static std::string cfgHitboxName = "Hitbox";
static Vec3 cfgHitboxSize = { 12.0f, 12.0f, 12.0f }; // cube or X,Y,Z
static int cfgHitboxCollide = -1; // -1 leave, 0 force false, 1 force true
static std::map<uint64_t, Vec3> hitboxSaved; // prim -> original size
static std::map<uint64_t, uint32_t> hitboxSavedFlags; // prim -> original flags
static uint64_t hitboxSelfModel = 0;

// ---- fly anchor: anchored parts ignore gravity so position writes stick.
// Saved per character prim, restored on fly-off / overlay exit.
static uint64_t flyAnchorPrim = 0;
static uint32_t flyAnchorSaved = 0;
static void flyAnchorRestore(Memory& m, const Offsets& o) {
    if (flyAnchorPrim && o.primitiveFlags)
        m.write(flyAnchorPrim + (uint64_t)o.primitiveFlags, flyAnchorSaved);
    flyAnchorPrim = 0;
}

static void loadHitboxProfile(uint64_t place) {
    cfgHitboxEnabled = 0;
    cfgHitboxName = "Hitbox";
    cfgHitboxSize = Vec3{ 12.0f, 12.0f, 12.0f };
    cfgHitboxCollide = -1;
    if (!place) return;
    std::string path = g_exeDir + "\\profiles\\" +
                       std::to_string((unsigned long long)place) + ".cfg";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "hitbox.enabled=", 15) == 0)
            cfgHitboxEnabled = atoi(line + 15) ? 1 : 0;
        if (strncmp(line, "hitbox.size=", 12) == 0) {
            float x = 0, y = 0, z = 0;
            if (sscanf(line + 12, "%f,%f,%f", &x, &y, &z) == 3)
                cfgHitboxSize = Vec3{ x, y, z };
            else {
                float s = (float)atof(line + 12);
                cfgHitboxSize = Vec3{ s, s, s };
            }
        }
        if (strncmp(line, "hitbox.name=", 12) == 0) {
            std::string v = line + 12;
            while (!v.empty() &&
                   (v.back() == '\n' || v.back() == '\r' || v.back() == ' '))
                v.pop_back();
            if (!v.empty() && v.size() < 64) cfgHitboxName = v;
        }
        if (strncmp(line, "hitbox.collide=", 15) == 0)
            cfgHitboxCollide = atoi(line + 15);
    }
    fclose(f);
    if (cfgHitboxSize.x > 200.0f) cfgHitboxSize.x = 200.0f;
    if (cfgHitboxSize.y > 200.0f) cfgHitboxSize.y = 200.0f;
    if (cfgHitboxSize.z > 200.0f) cfgHitboxSize.z = 200.0f;
    if (cfgHitboxCollide < -1) cfgHitboxCollide = -1;
    if (cfgHitboxCollide > 1) cfgHitboxCollide = 1;
}

static bool nameContainsCI(const std::string& name, const std::string& sub) {
    if (sub.empty()) return true;
    if (sub.size() > name.size()) return false;
    for (size_t i = 0; i + sub.size() <= name.size(); ++i) {
        bool ok = true;
        for (size_t k = 0; k < sub.size(); ++k)
            if (tolower((unsigned char)name[i + k]) !=
                tolower((unsigned char)sub[k])) {
                ok = false;
                break;
            }
        if (ok) return true;
    }
    return false;
}

static void hitboxApplyPart(Memory& m, const Offsets& o, uint64_t part) {
    uint64_t prim = 0;
    if (!o.basePartPrimitive || !o.primitiveSize ||
        !m.read(part + (uint64_t)o.basePartPrimitive, prim) ||
        !SDK::plausiblePtr(prim))
        return;
    if (!hitboxSaved.count(prim) && hitboxSaved.size() < 512) {
        float cur[3]{};
        if (m.read(prim + (uint64_t)o.primitiveSize, cur) && cur[0] > 0 &&
            cur[0] < 500 && cur[1] > 0 && cur[1] < 500 && cur[2] > 0 &&
            cur[2] < 500)
            hitboxSaved[prim] = Vec3{ cur[0], cur[1], cur[2] };
    }
    if (cfgHitboxSize.x > 0 && cfgHitboxSize.y > 0 && cfgHitboxSize.z > 0) {
        float want[3] = { cfgHitboxSize.x, cfgHitboxSize.y, cfgHitboxSize.z };
        m.write(prim + (uint64_t)o.primitiveSize, want);
    }
    if (cfgHitboxCollide != -1 && o.primitiveFlags) {
        uint32_t fl = 0;
        if (m.read(prim + (uint64_t)o.primitiveFlags, fl)) {
            if (!hitboxSavedFlags.count(prim) && hitboxSavedFlags.size() < 512)
                hitboxSavedFlags[prim] = fl;
            uint32_t bit = (uint32_t)o.primitiveCanCollideBit;
            uint32_t nw = (cfgHitboxCollide == 0) ? (fl & ~bit) : (fl | bit);
            if (nw != fl) m.write(prim + (uint64_t)o.primitiveFlags, nw);
        }
    }
}

static void hitboxRestoreAll(Memory& m, const Offsets& o) {
    if (o.primitiveSize) {
        for (auto& kv : hitboxSaved) {
            float s[3] = { kv.second.x, kv.second.y, kv.second.z };
            m.write(kv.first + (uint64_t)o.primitiveSize, s);
        }
    }
    if (o.primitiveFlags) {
        for (auto& kv : hitboxSavedFlags)
            m.write(kv.first + (uint64_t)o.primitiveFlags, kv.second);
    }
    hitboxSaved.clear();
    hitboxSavedFlags.clear();
}

static void hitboxTick(Memory& m, SDK& sdk, const Offsets& o, bool haveWrite,
                     DWORD nowTick) {
    if (!cfgHitboxEnabled || !haveWrite || !o.primitiveSize) return;
    uint64_t dm = 0, ws = 0;
    if (!sdk.dataModel(dm) || !sdk.workspace(dm, ws) || !ws) return;
    // Skip own character: expanding self helps your enemies, not you.
    if (true) { // re-resolve every tick: respawns mint new Models
        hitboxSelfModel = 0;
        uint64_t players = 0, local = 0, model = 0;
        if (sdk.findChildByClass(dm, "Players", players) && players &&
            o.playersLocalPlayer &&
            m.read(players + (uint64_t)o.playersLocalPlayer, local) && local &&
            o.playerModelInstance &&
            m.read(local + (uint64_t)o.playerModelInstance, model))
            hitboxSelfModel = model;
            { static uint64_t loggedSelf = 1; // 1 forces first log
              if (model != loggedSelf) {
                  loggedSelf = model;
                  FILE* lf = fopen(exePath("hitbox_self.log").c_str(), "a");
                  if (lf) {
                      fprintf(lf, "self=0x%llx players=0x%llx local=0x%llx\n",
                              (unsigned long long)model,
                              (unsigned long long)players,
                              (unsigned long long)local);
                      fclose(lf);
                  }
              } }
    }
    // Heal: a stale window may have expanded self's current parts before
    // the skip learned the new Model. Restore + drop them here.
    if (hitboxSelfModel && o.primitiveSize) {
        sdk.walkChildren(hitboxSelfModel, [&](uint64_t g) {
            std::string gn = sdk.readName(g);
            if (!gn.empty() && nameContainsCI(gn, cfgHitboxName)) {
                uint64_t prim = 0;
                if (o.basePartPrimitive &&
                    m.read(g + (uint64_t)o.basePartPrimitive, prim) &&
                    SDK::plausiblePtr(prim)) {
                    auto it = hitboxSaved.find(prim);
                    if (it != hitboxSaved.end()) {
                        float s[3] = { it->second.x, it->second.y, it->second.z };
                        m.write(prim + (uint64_t)o.primitiveSize, s);
                        hitboxSaved.erase(it);
                    }
                    auto jt = hitboxSavedFlags.find(prim);
                    if (jt != hitboxSavedFlags.end()) {
                        if (o.primitiveFlags)
                            m.write(prim + (uint64_t)o.primitiveFlags, jt->second);
                        hitboxSavedFlags.erase(jt);
                    }
                }
            }
            return true;
        });
    }
    uint64_t self = hitboxSelfModel;
    // depth<=2 scan: models -> parts (pilot layout + generic nesting).
    sdk.walkChildren(ws, [&](uint64_t ch) {
        if (self && ch == self) {
            static uint64_t loggedSkip = 0;
            if (ch != loggedSkip) {
                loggedSkip = ch;
                FILE* lf = fopen(exePath("hitbox_self.log").c_str(), "a");
                if (lf) {
                    fprintf(lf, "skip=0x%llx\n", (unsigned long long)ch);
                    fclose(lf);
                }
            }
            return true; // own model: skip subtree
        }
        std::string nm = sdk.readName(ch);
        if (!nm.empty() && nameContainsCI(nm, cfgHitboxName))
            hitboxApplyPart(m, o, ch);
        if (hitboxSaved.size() >= 512) return false;
        sdk.walkChildren(ch, [&](uint64_t g) {
            std::string gn = sdk.readName(g);
            if (!gn.empty() && nameContainsCI(gn, cfgHitboxName))
                hitboxApplyPart(m, o, g);
            return true;
        });
        return true;
    });
}

// ---- mode-1 high-rate writer: mem-rot at aim.rate Hz, decoupled from the
// vsync render loop. Render thread publishes the latest snapshot;
// this thread re-applies it so our write lands last before the game renders
// (the engine rebuilds rotation from its own state every tick, so one write
// per render frame only crawls).
struct AimSnap {
    bool ready = false;
    uint64_t epoch = 0;
    uint64_t seq = 0; // publish counter: velext model applies <=1 step per publish
    Vec3 tgt{};
    Vec3 vel{}; // world vel (studs/s) at publish time
    Vec3 cm{};
    uint64_t camAddr = 0;
    double qpc = 0;
};
static std::mutex g_snapMu;
static AimSnap g_snap;
static std::atomic<bool> g_writerRun{ true };
static std::atomic<long> g_writerCount{ 0 };
static std::atomic<uint64_t> g_snapEpoch{ 1 };
static std::atomic<uint64_t> g_snapSeq{ 0 };
static std::atomic<uint64_t> g_currentCam{ 0 };
static int g_writeRateHz = 500; // from aim.rate (main sets at startup)
static std::mutex g_diagMu;
static FILE* g_diagFile = nullptr;
static std::map<std::string, DWORD> g_diagLast;

static void diagOpen() {
    std::lock_guard<std::mutex> lk(g_diagMu);
    g_diagFile = fopen(exePath("aim_diag.log").c_str(), "a");
    if (g_diagFile) {
        fprintf(g_diagFile, "\n--- aim diagnostic session ---\n");
        fflush(g_diagFile);
    }
}

static void diagClose() {
    std::lock_guard<std::mutex> lk(g_diagMu);
    if (g_diagFile) {
        fprintf(g_diagFile, "--- aim diagnostic session end ---\n");
        fclose(g_diagFile);
        g_diagFile = nullptr;
    }
}

static void diagRate(const char* key, DWORD intervalMs, const char* fmt, ...) {
    DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lk(g_diagMu);
    if (!g_diagFile) return;
    auto it = g_diagLast.find(key);
    if (it != g_diagLast.end() && now - it->second < intervalMs) return;
    g_diagLast[key] = now;
    fprintf(g_diagFile, "[%lu] ", (unsigned long)now);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_diagFile, fmt, ap);
    va_end(ap);
    fputc('\n', g_diagFile);
    fflush(g_diagFile);
}

// ---- cursor confinement (Roblox locks the mouse to a 1px box while playing) --
// The clip is a desktop-wide resource: any process may lift it, and the game
// does *not* put it back on its own (verified: still free 4s later), so whoever
// lifts it must restore it. Lifting is what makes the panel clickable at all
// while the player has camera control.
static RECT g_gameClip{};
static bool g_gameClipHeld = false;

static void mouseClipLog(const char* what) {
    if (!g_diagState) return;
    RECT c{};
    GetClipCursor(&c);
    printf("clip %s: held=%d saved=%ld,%ld-%ld,%ld now=%ld,%ld-%ld,%ld\n", what,
           (int)g_gameClipHeld, g_gameClip.left, g_gameClip.top, g_gameClip.right,
           g_gameClip.bottom, c.left, c.top, c.right, c.bottom);
    fflush(stdout);
}

static void mouseClipRelease(int screenW, int screenH) {
    static int logged = -1;
    RECT cur{};
    if (GetClipCursor(&cur) &&
        (long long)(cur.right - cur.left) * (long long)(cur.bottom - cur.top) <
            (long long)screenW * (long long)screenH) {
        // smaller than the screen: a genuine lock. Remember the newest one - the
        // game may re-assert its box while the panel is up (any in-game input).
        if (!g_gameClipHeld || cur.left != g_gameClip.left ||
            cur.top != g_gameClip.top || cur.right != g_gameClip.right ||
            cur.bottom != g_gameClip.bottom) {
            g_gameClip = cur;
            g_gameClipHeld = true;
            if (logged != 1) {
                logged = 1;
                mouseClipLog("HOLD");
            }
        }
        g_gameClipHeld = true;
    }
    ClipCursor(nullptr);
}

static void mouseClipRestore() {
    static int logged = 1;
    if (g_gameClipHeld) {
        BOOL ok = ClipCursor(&g_gameClip);
        g_gameClipHeld = false;
        if (logged != 0) {
            logged = 0;
            mouseClipLog(ok ? "RESTORE" : "RESTORE-failed");
        }
    }
}

// ---- World (Đợt 2): lighting / atmosphere / effects / terrain / camera -------
// Every field below is client-local render state (Lighting, Atmosphere, the
// post effects, Terrain, Camera + Player zoom): a write changes this machine's
// picture only, nothing replicates. PLAN §0.2 discipline still applies: write
// only while the feature is ticked, remember the original before the first
// write, restore on untick / panic / exit, and after the one-shot apply re-read
// at most 4x/s and re-write only on drift. FOV is the deliberate exception:
// the game rebuilds Camera.FieldOfView every render, so it is written every
// loop while ticked, which is the only way the override sticks.
// (The cfg flags themselves are declared near loadAimCfg, which parses them.)

// Instance cache. Reset (with the saved originals) on place change, DataModel
// rebuild and camera/local-player swap; while something is missing the
// resolve pass retries at most once a second.
uint64_t wLighting = 0, wTerrain = 0, wAtmosphere = 0;
uint64_t wBloom = 0, wBlur = 0, wCC = 0, wLocalPlr = 0;
static DWORD wResolveTick = 0;  // last resolve attempt (<= 1/s)
static DWORD wVerifyTick = 0;   // last class re-validation (every 2 s)
static DWORD wDiffTick = 0;     // last drift read-back (<= 4/s)

// One float field / one bool (1 byte) field / one Color3 (3 floats) field.
// "have" = the original was captured before our first write; "wrote" = the
// wanted value has been written at least once.
struct WorldF { bool have = false; uint64_t addr = 0; float orig = 0.0f; float last = 0.0f; bool wrote = false; };
struct WorldB { bool have = false; uint64_t addr = 0; unsigned char orig = 0, last = 0; bool wrote = false; };
struct WorldV3 { bool have = false; uint64_t addr = 0; float orig[3]{}; float last[3]{}; bool wrote = false; };
// Every field of the tab, so "forget everything" is one assignment.
struct WorldState {
    WorldF bright, expo, clock, fogS, fogE, zoomMin, zoomMax, fov, fov2;
    WorldF dens, haze, glare, watTr, watWave, watRefl, grass;
    WorldB shadow, bloomB, blurB, ccB;
    WorldV3 amb, outAmb;
};
static WorldState worldSt;

// FOV raw values are radians on some builds (11f155f3: 70 deg == 1.22173) and
// degrees on others; anything at or below this is read as radians.
static constexpr float kFovRadMax = 3.2f;
// One place that formats a raw FOV for the diag lines: "70.0deg(raw 1.2217)"
// when the slot holds radians, plain "90.0" when it holds degrees.
static void fovFmt(char* b, size_t n, float raw) {
    if (raw > 0.0f && raw <= kFovRadMax)
        snprintf(b, n, "%.1fdeg(raw %.4f)", raw * 57.29578f, raw);
    else
        snprintf(b, n, "%.1f", raw);
}

static bool wfSeed(Memory& m, WorldF& f, uint64_t addr) {
    float cur = 0.0f;
    if (!addr || !m.read(addr, cur) || !std::isfinite(cur)) return false;
    f = WorldF{};
    f.have = true;
    f.addr = addr;
    f.orig = cur;
    return true;
}
// Seed on first use (or when the instance changed: the stale original is
// dropped, never written to a dead address), write on enable / on any value
// change. Drift repair is wfDiff at <= 4 Hz.
static void wfApply(Memory& m, WorldF& f, uint64_t addr, float want) {
    if (!addr) return;
    if ((!f.have || f.addr != addr) && !wfSeed(m, f, addr)) return;
    if (!f.wrote || f.last != want) {
        if (m.write(addr, want)) {
            f.last = want;
            f.wrote = true;
        }
    }
}
static void wfDiff(Memory& m, WorldF& f, float want) {
    if (!f.have || !f.addr) return;
    float cur = 0.0f;
    if (m.read(f.addr, cur) && cur != want && m.write(f.addr, want)) f.last = want;
}
static void wfRestore(Memory& m, WorldF& f) {
    if (f.have && f.addr) m.write(f.addr, f.orig);
    f = WorldF{};
}
static bool wbSeed(Memory& m, WorldB& f, uint64_t addr) {
    unsigned char cur = 0;
    if (!addr || !m.read(addr, cur)) return false;
    f = WorldB{};
    f.have = true;
    f.addr = addr;
    f.orig = cur;
    return true;
}
static void wbApply(Memory& m, WorldB& f, uint64_t addr, unsigned char want) {
    if (!addr) return;
    if ((!f.have || f.addr != addr) && !wbSeed(m, f, addr)) return;
    if (!f.wrote || f.last != want) {
        if (m.write(addr, want)) {
            f.last = want;
            f.wrote = true;
        }
    }
}
static void wbDiff(Memory& m, WorldB& f, unsigned char want) {
    if (!f.have || !f.addr) return;
    unsigned char cur = 0;
    if (m.read(f.addr, cur) && cur != want && m.write(f.addr, want)) f.last = want;
}
static void wbRestore(Memory& m, WorldB& f) {
    if (f.have && f.addr) m.write(f.addr, f.orig);
    f = WorldB{};
}
static bool wvSeed(Memory& m, WorldV3& f, uint64_t addr) {
    float cur[3]{};
    if (!addr || !m.read(addr, cur)) return false;
    f = WorldV3{};
    f.have = true;
    f.addr = addr;
    memcpy(f.orig, cur, sizeof cur);
    return true;
}
// Color3 writes go through writeBytes: the array would decay to a pointer in
// the generic write<T> template and send 8 bytes of pointer instead of the RGB.
static void wvApply(Memory& m, WorldV3& f, uint64_t addr, const float want[3]) {
    if (!addr) return;
    if ((!f.have || f.addr != addr) && !wvSeed(m, f, addr)) return;
    if (!f.wrote || memcmp(f.last, want, sizeof(float) * 3) != 0) {
        if (m.writeBytes(addr, want, sizeof(float) * 3)) {
            memcpy(f.last, want, sizeof(float) * 3);
            f.wrote = true;
        }
    }
}
static void wvDiff(Memory& m, WorldV3& f, const float want[3]) {
    if (!f.have || !f.addr) return;
    float cur[3]{};
    if (m.read(f.addr, cur) && memcmp(cur, want, sizeof cur) != 0 &&
        m.writeBytes(f.addr, want, sizeof(float) * 3))
        memcpy(f.last, want, sizeof(float) * 3);
}
static void wvRestore(Memory& m, WorldV3& f) {
    if (f.have && f.addr) m.writeBytes(f.addr, f.orig, sizeof(float) * 3);
    f = WorldV3{};
}

// Put back every original we saved, then drop the instance cache. This is also
// the place-change / DataModel-rebuild reset: writes to already-destroyed
// instances simply fail, and that is the point - a stale "original" from the
// old place must never be re-seeded as if it were the new place's value.
static void worldRestoreAll(Memory& m, const Offsets& o) {
    (void)o;
    bool any = worldSt.bright.have || worldSt.expo.have || worldSt.clock.have ||
               worldSt.fogS.have || worldSt.shadow.have || worldSt.bloomB.have ||
               worldSt.dens.have || worldSt.watTr.have || worldSt.grass.have ||
               worldSt.fov.have || worldSt.zoomMin.have;
    // Captured before the helpers clear the state: the diag line then shows
    // which real game values went back in.
    float rBright = worldSt.bright.orig, rFogS = worldSt.fogS.orig,
          rFov = worldSt.fov.orig;
    wfRestore(m, worldSt.bright);
    wvRestore(m, worldSt.amb);
    wvRestore(m, worldSt.outAmb);
    wfRestore(m, worldSt.fogS);
    wfRestore(m, worldSt.fogE);
    wbRestore(m, worldSt.shadow);
    wfRestore(m, worldSt.expo);
    wfRestore(m, worldSt.clock);
    wbRestore(m, worldSt.bloomB);
    wbRestore(m, worldSt.blurB);
    wbRestore(m, worldSt.ccB);
    wfRestore(m, worldSt.dens);
    wfRestore(m, worldSt.haze);
    wfRestore(m, worldSt.glare);
    wfRestore(m, worldSt.watTr);
    wfRestore(m, worldSt.watWave);
    wfRestore(m, worldSt.watRefl);
    wfRestore(m, worldSt.grass);
    wfRestore(m, worldSt.fov);
    wfRestore(m, worldSt.fov2);
    wfRestore(m, worldSt.zoomMin);
    wfRestore(m, worldSt.zoomMax);
    wLighting = wTerrain = wAtmosphere = 0;
    wBloom = wBlur = wCC = wLocalPlr = 0;
    wResolveTick = wVerifyTick = 0;
    if (any && g_diagState) {
        char fovTxt[48];
        fovFmt(fovTxt, sizeof fovTxt, rFov);
        printf("world restore all (orig bright=%.2f fogStart=%.0f fov=%s)\n",
               rBright, rFogS, fovTxt);
        fflush(stdout);
    }
}
// Lighting is a child of the DataModel, Terrain a child of Workspace.
// Atmosphere and the post effects are children of Lighting; some games parent
// them under the Camera instead, so both containers are scanned.
static void worldResolve(Memory& m, SDK& sdk, const Offsets& o, uint64_t dm,
                         uint64_t ws) {
    if (!wLighting) sdk.findChildByClass(dm, "Lighting", wLighting);
    if (wLighting) {
        uint64_t cam = 0;
        sdk.camera(ws, cam);
        auto scan = [&](uint64_t root) {
            if (!root) return;
            sdk.walkChildren(root, [&](uint64_t c) {
                std::string cls = sdk.readClass(c);
                if (cls == "Atmosphere") {
                    if (!wAtmosphere) wAtmosphere = c;
                } else if (cls == "BloomEffect") {
                    if (!wBloom) wBloom = c;
                } else if (cls == "BlurEffect") {
                    if (!wBlur) wBlur = c;
                } else if (cls == "ColorCorrectionEffect") {
                    if (!wCC) wCC = c;
                }
                return true;
            });
        };
        scan(wLighting);
        scan(cam);
    }
    if (!wTerrain && ws) sdk.findChildByClass(ws, "Terrain", wTerrain);
    if (!wLocalPlr) {
        uint64_t players = 0, local = 0;
        if (o.playersLocalPlayer && sdk.findChildByClass(dm, "Players", players) &&
            players && m.read(players + (uint64_t)o.playersLocalPlayer, local) &&
            SDK::plausiblePtr(local))
            wLocalPlr = local;
    }
}

static void worldTick(Memory& m, SDK& sdk, const Offsets& o, bool haveWrite,
                      DWORD nowLoop, uint64_t camAddr) {
    uint64_t dm = 0, ws = 0;
    if (!sdk.dataModel(dm)) {
        worldRestoreAll(m, o); // DataModel gone (leave/hop): nothing to keep
        return;
    }
    sdk.workspace(dm, ws);
    // Class re-validation: a same-place rejoin mints new instances without
    // tripping the place-change branch in refreshEnts.
    if (wLighting && nowLoop - wVerifyTick >= 2000) {
        wVerifyTick = nowLoop;
        if (sdk.readClass(wLighting) != "Lighting") {
            worldRestoreAll(m, o);
        } else if (wLocalPlr && sdk.readClass(wLocalPlr) != "Player") {
            wLocalPlr = 0;
            worldSt.zoomMin = WorldF{}; // stale player: drop, never write
            worldSt.zoomMax = WorldF{};
        }
    }
    if ((!wLighting || !wTerrain || !wAtmosphere || !wBloom || !wBlur || !wCC ||
         !wLocalPlr) &&
        nowLoop - wResolveTick >= 1000) {
        wResolveTick = nowLoop;
        worldResolve(m, sdk, o, dm, ws);
    }
    diagRate("world", 5000,
             "world lighting=%p terrain=%p atmo=%p fx=%d/%d/%d plr=%p",
             (void*)wLighting, (void*)wTerrain, (void*)wAtmosphere,
             wBloom ? 1 : 0, wBlur ? 1 : 0, wCC ? 1 : 0, (void*)wLocalPlr);
    if (g_diagState) {
        static DWORD lastWorldDiag = 0;
        if (nowLoop - lastWorldDiag >= 5000) {
            lastWorldDiag = nowLoop;
            // Read-only probe of the marquee slots: proves a write landed (or
            // that a restore did) without having to eyeball the picture.
            float vb = 0.0f, vfs = 0.0f, vfe = 0.0f, vfov = 0.0f, vclk = 0.0f;
            if (wLighting && o.lightingBrightness)
                m.read(wLighting + (uint64_t)o.lightingBrightness, vb);
            if (wLighting && o.lightingFogStart)
                m.read(wLighting + (uint64_t)o.lightingFogStart, vfs);
            if (wLighting && o.lightingFogEnd)
                m.read(wLighting + (uint64_t)o.lightingFogEnd, vfe);
            if (wLighting && o.lightingClockTime)
                m.read(wLighting + (uint64_t)o.lightingClockTime, vclk);
            if (camAddr && o.cameraFOV) m.read(camAddr + (uint64_t)o.cameraFOV, vfov);
            // The unit/format traps (radians slot, frozen-clock hacks) are
            // explained where the values come from; the diag just stays
            // readable, e.g. "fov=70.0deg(raw 1.2217) clock=-1.66e+15(=8.00 mod 24)".
            char fovTxt[48], clkTxt[48];
            fovFmt(fovTxt, sizeof fovTxt, vfov);
            if (std::isfinite(vclk) && fabsf(vclk) > 24.0f) {
                float m24 = fmodf(vclk, 24.0f);
                if (m24 < 0.0f) m24 += 24.0f;
                snprintf(clkTxt, sizeof clkTxt, "%.3g(=%.2f mod 24)", vclk, m24);
            } else {
                snprintf(clkTxt, sizeof clkTxt, "%.2f", vclk);
            }
            printf("world lighting=%p terrain=%p atmo=%p fx=%d/%d/%d plr=%p "
                   "vals bright=%.2f fogS=%.0f fogE=%.0f fov=%s clock=%s\n",
                   (void*)wLighting, (void*)wTerrain, (void*)wAtmosphere,
                   wBloom ? 1 : 0, wBlur ? 1 : 0, wCC ? 1 : 0, (void*)wLocalPlr,
                   vb, vfs, vfe, fovTxt, clkTxt);
            fflush(stdout);
        }
    }
    if (!haveWrite) return; // read-only handle: status only, no writes
    bool diffNow = (nowLoop - wDiffTick >= 250); // drift read-back <= 4/s
    if (diffNow) wDiffTick = nowLoop;
    const float white[3] = { 1.0f, 1.0f, 1.0f };

    // 1) Fullbright: Brightness slider + white Ambient/OutdoorAmbient.
    if (cfgWorldFull && wLighting && o.lightingBrightness && o.lightingAmbient &&
        o.lightingOutdoorAmbient) {
        wfApply(m, worldSt.bright, wLighting + (uint64_t)o.lightingBrightness,
                cfgWorldBright);
        wvApply(m, worldSt.amb, wLighting + (uint64_t)o.lightingAmbient, white);
        wvApply(m, worldSt.outAmb, wLighting + (uint64_t)o.lightingOutdoorAmbient,
                white);
        if (diffNow) {
            wfDiff(m, worldSt.bright, cfgWorldBright);
            wvDiff(m, worldSt.amb, white);
            wvDiff(m, worldSt.outAmb, white);
        }
    } else {
        wfRestore(m, worldSt.bright);
        wvRestore(m, worldSt.amb);
        wvRestore(m, worldSt.outAmb);
    }
    // 2) Anti-fog: push both planes out of sight (restore keeps both originals).
    if (cfgWorldFog && wLighting && o.lightingFogStart && o.lightingFogEnd) {
        wfApply(m, worldSt.fogS, wLighting + (uint64_t)o.lightingFogStart, 100000.0f);
        wfApply(m, worldSt.fogE, wLighting + (uint64_t)o.lightingFogEnd, 100000.0f);
        if (diffNow) {
            wfDiff(m, worldSt.fogS, 100000.0f);
            wfDiff(m, worldSt.fogE, 100000.0f);
        }
    } else {
        wfRestore(m, worldSt.fogS);
        wfRestore(m, worldSt.fogE);
    }
    // 3) No shadows (bool byte).
    if (cfgWorldShadows && wLighting && o.lightingGlobalShadows) {
        wbApply(m, worldSt.shadow, wLighting + (uint64_t)o.lightingGlobalShadows, 0);
        if (diffNow) wbDiff(m, worldSt.shadow, 0);
    } else {
        wbRestore(m, worldSt.shadow);
    }
    // 4) Exposure override.
    if (cfgWorldExpo && wLighting && o.lightingExposureCompensation) {
        wfApply(m, worldSt.expo,
                wLighting + (uint64_t)o.lightingExposureCompensation, cfgWorldExpoVal);
        if (diffNow) wfDiff(m, worldSt.expo, cfgWorldExpoVal);
    } else {
        wfRestore(m, worldSt.expo);
    }
    // 5) Time of day: written when the slider moves, kept frozen by the diff.
    if (cfgWorldClock && wLighting && o.lightingClockTime) {
        wfApply(m, worldSt.clock, wLighting + (uint64_t)o.lightingClockTime,
                cfgWorldClockVal);
        if (diffNow) wfDiff(m, worldSt.clock, cfgWorldClockVal);
    } else {
        wfRestore(m, worldSt.clock);
    }
    // 6) Bloom / Blur / ColorCorrection off (each may not exist in a game:
    // the missing ones are simply skipped).
    if (cfgWorldFx && o.effectEnabled) {
        if (wBloom)
            wbApply(m, worldSt.bloomB, wBloom + (uint64_t)o.effectEnabled, 0);
        else
            wbRestore(m, worldSt.bloomB);
        if (wBlur)
            wbApply(m, worldSt.blurB, wBlur + (uint64_t)o.effectEnabled, 0);
        else
            wbRestore(m, worldSt.blurB);
        if (wCC)
            wbApply(m, worldSt.ccB, wCC + (uint64_t)o.effectEnabled, 0);
        else
            wbRestore(m, worldSt.ccB);
        if (diffNow) {
            wbDiff(m, worldSt.bloomB, 0);
            wbDiff(m, worldSt.blurB, 0);
            wbDiff(m, worldSt.ccB, 0);
        }
    } else {
        wbRestore(m, worldSt.bloomB);
        wbRestore(m, worldSt.blurB);
        wbRestore(m, worldSt.ccB);
    }
    // 7) Atmosphere off.
    if (cfgWorldAtmo && wAtmosphere && o.atmosphereDensity && o.atmosphereHaze &&
        o.atmosphereGlare) {
        wfApply(m, worldSt.dens, wAtmosphere + (uint64_t)o.atmosphereDensity, 0.0f);
        wfApply(m, worldSt.haze, wAtmosphere + (uint64_t)o.atmosphereHaze, 0.0f);
        wfApply(m, worldSt.glare, wAtmosphere + (uint64_t)o.atmosphereGlare, 0.0f);
        if (diffNow) {
            wfDiff(m, worldSt.dens, 0.0f);
            wfDiff(m, worldSt.haze, 0.0f);
            wfDiff(m, worldSt.glare, 0.0f);
        }
    } else {
        wfRestore(m, worldSt.dens);
        wfRestore(m, worldSt.haze);
        wfRestore(m, worldSt.glare);
    }
    // 8) Clear + flat water (Terrain).
    if (cfgWorldWater && wTerrain && o.terrainWaterTransparency &&
        o.terrainWaterWaveSize && o.terrainWaterReflectance) {
        wfApply(m, worldSt.watTr,
                wTerrain + (uint64_t)o.terrainWaterTransparency, 1.0f);
        wfApply(m, worldSt.watWave,
                wTerrain + (uint64_t)o.terrainWaterWaveSize, 0.0f);
        wfApply(m, worldSt.watRefl,
                wTerrain + (uint64_t)o.terrainWaterReflectance, 0.0f);
        if (diffNow) {
            wfDiff(m, worldSt.watTr, 1.0f);
            wfDiff(m, worldSt.watWave, 0.0f);
            wfDiff(m, worldSt.watRefl, 0.0f);
        }
    } else {
        wfRestore(m, worldSt.watTr);
        wfRestore(m, worldSt.watWave);
        wfRestore(m, worldSt.watRefl);
    }
    // 9) No grass. [I] Terrain.WaterColor (464, Color3) is not touched this
    // round: it needs a colour UI and the panel cannot host popups (see the
    // colour-bar note in the Visual tab), so it waits for a palette button.
    if (cfgWorldGrass && wTerrain && o.terrainGrassLength) {
        wfApply(m, worldSt.grass, wTerrain + (uint64_t)o.terrainGrassLength, 0.0f);
        if (diffNow) wfDiff(m, worldSt.grass, 0.0f);
    } else {
        wfRestore(m, worldSt.grass);
    }
    // 10) FOV: rewritten every loop (the game re-derives FieldOfView each
    // render, so a one-shot or 4 Hz diff write would be visibly eaten).
    // Unit trap: on 11f155f3 the slot stores RADIANS (70 deg == 1.22173), so
    // the seed picks the unit once per camera (|orig| <= kFovRadMax ->
    // radians) and the slider stays in degrees. The same raw value also sits
    // in a second slot 12 bytes on (verified equal at seed on the live build,
    // independent of +304 afterwards); it is kept in sync while ticked so
    // whichever copy the renderer polls is covered - but only when it
    // started equal, which keeps the write off any unrelated field.
    if (cfgWorldFov && camAddr && o.cameraFOV) {
        uint64_t fa = camAddr + (uint64_t)o.cameraFOV;
        if (worldSt.fov.have && worldSt.fov.addr != fa) {
            worldSt.fov = WorldF{};  // camera swapped: stale originals dropped
            worldSt.fov2 = WorldF{};
        }
        if (!worldSt.fov.have) {
            if (wfSeed(m, worldSt.fov, fa)) {
                uint32_t a = 0, b = 0;
                if (worldSt.fov.orig > 0.0f && m.read(fa, a) &&
                    m.read(fa + 12, b) && a == b)
                    wfSeed(m, worldSt.fov2, fa + 12);
            }
        }
        if (worldSt.fov.have) {
            float want = (fabsf(worldSt.fov.orig) <= kFovRadMax)
                             ? cfgWorldFovVal * 0.0174532925f
                             : cfgWorldFovVal;
            if (m.write(fa, want)) {
                worldSt.fov.last = want;
                worldSt.fov.wrote = true;
            }
            if (worldSt.fov2.have && worldSt.fov2.addr == fa + 12 &&
                m.write(fa + 12, want)) {
                worldSt.fov2.last = want;
                worldSt.fov2.wrote = true;
            }
        }
    } else if (!cfgWorldFov) {
        wfRestore(m, worldSt.fov);
        wfRestore(m, worldSt.fov2);
    } else {
        worldSt.fov = WorldF{}; // no camera this tick: forget, never write stale
        worldSt.fov2 = WorldF{};
    }
    // 11) Far zoom: Player.MinZoomDistance / MaxZoomDistance.
    if (cfgWorldZoom && wLocalPlr && o.playerMinZoomDistance &&
        o.playerMaxZoomDistance) {
        wfApply(m, worldSt.zoomMin,
                wLocalPlr + (uint64_t)o.playerMinZoomDistance, 0.5f);
        wfApply(m, worldSt.zoomMax,
                wLocalPlr + (uint64_t)o.playerMaxZoomDistance, 2000.0f);
        if (diffNow) {
            wfDiff(m, worldSt.zoomMin, 0.5f);
            wfDiff(m, worldSt.zoomMax, 2000.0f);
        }
    } else {
        wfRestore(m, worldSt.zoomMin);
        wfRestore(m, worldSt.zoomMax);
    }
}

// ============================================================================
// Server tab (Đợt 4): session info, real ping, server list / hop / rejoin,
// auto-rejoin watchdog, anti-AFK.
//
// [P] READ-ONLY with respect to game memory: every value below is obtained
// through Memory/SDK reads. This module never writes a byte into the game.
// Data that leaves the process:
//   - HTTPS GETs to games.roblox.com / apis.roblox.com (public endpoints, no
//     auth) for the server list;
//   - HTTPS GET to ipwho.is ONLY when srv.geo is enabled (default off), and it
//     carries nothing but the server IP we already read;
//   - ShellExecuteW("roblox://...") for join / hop / rejoin;
//   - SendInput with a relative +1px / -1px mouse move for anti-AFK.
// No cookie, token or account data is ever read or sent.
// ============================================================================

static const int  kSrvHttpTimeoutMs = 4000;
static const wchar_t* kSrvUa = L"ext-overlay/1.0 (server tab)";
static const wchar_t* kSrvUaGeo = L"ext-overlay/1.0 (server tab geo)";
// [I] If roblox:// ever stops launching the client, flip this to true: the
// join/hop buttons then use the web "games/start" URL (same params).
static const bool kSrvWebUriFallback = false;


// ---- tiny JSON helpers -----------------------------------------------------
// The offsets fetcher already parses its JSON by hand; pulling in a library to
// read five scalars would be a new dependency. These helpers only understand
// objects/arrays of scalars, which is exactly the shape of the two endpoints.
static bool srvJsonStr(const char*& p, const char* e, std::string& out) {
    if (p >= e || *p != '"') return false;
    ++p;
    out.clear();
    while (p < e) {
        char c = *p++;
        if (c == '"') return true;
        if (c == '\\' && p < e) c = *p++; // enough for ids/names/city names
        out += c;
    }
    return false;
}

// Find "key":value at object depth 1. Strings are consumed whole, so braces
// inside a name cannot confuse the depth counter; nested objects/arrays are
// skipped. quote=true reads a JSON string, false returns the raw token.
static bool srvJsonField(const char* b, const char* e, const char* key,
                         std::string& out, bool quote) {
    int depth = 0;
    const char* p = b;
    while (p < e) {
        char c = *p;
        if (c == '"') {
            std::string s;
            const char* q = p;
            if (!srvJsonStr(q, e, s)) return false;
            const char* r = q;
            while (r < e && (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n'))
                ++r;
            if (depth == 1 && s == key && r < e && *r == ':') {
                ++r;
                while (r < e &&
                       (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n'))
                    ++r;
                if (quote) return srvJsonStr(r, e, out);
                const char* st = r;
                while (r < e && *r != ',' && *r != '}' && *r != ']') ++r;
                while (r > st && (r[-1] == ' ' || r[-1] == '\t' ||
                                  r[-1] == '\r' || r[-1] == '\n'))
                    --r;
                if (r == st) return false;
                out.assign(st, r);
                return true;
            }
            p = q;
            continue;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') {
            --depth;
            if (depth <= 0) break;
        }
        ++p;
    }
    return false;
}

// Count the strings of the array at "key" (used when a response omits
// "playing": public-server rows then only carry "playerTokens").
static int srvJsonCountArray(const char* b, const char* e, const char* key) {
    int depth = 0, n = 0;
    const char* p = b;
    while (p < e) {
        char c = *p;
        if (c == '"') {
            std::string s;
            const char* q = p;
            if (!srvJsonStr(q, e, s)) return n;
            const char* r = q;
            while (r < e && (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n'))
                ++r;
            if (depth == 1 && s == key && r < e && *r == ':') {
                ++r;
                while (r < e &&
                       (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n'))
                    ++r;
                if (r >= e || *r != '[') return 0;
                ++r;
                int d = 1;
                while (r < e && d > 0) {
                    char a = *r;
                    if (a == '"') {
                        if (d == 1) ++n;
                        std::string dummy;
                        const char* qq = r;
                        if (!srvJsonStr(qq, e, dummy)) return n;
                        r = qq;
                        continue;
                    }
                    if (a == '[' || a == '{') ++d;
                    else if (a == ']' || a == '}') --d;
                    ++r;
                }
                return n;
            }
            p = q;
            continue;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') {
            --depth;
            if (depth <= 0) break;
        }
        ++p;
    }
    return n;
}

// Walk {"data":[{...},{...}]}: returns true when the array was found (an empty
// array is a valid answer: the place simply has no public servers).
static bool srvParseList(const std::string& body, std::vector<SrvRow>& rows) {
    rows.clear();
    const char* b = body.c_str();
    const char* e = b + body.size();
    const char* arr = nullptr;
    const char* p = b;
    while (p < e) {
        if (*p == '"') {
            std::string s;
            const char* q = p;
            if (!srvJsonStr(q, e, s)) break;
            const char* r = q;
            while (r < e && (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n'))
                ++r;
            if (s == "data" && r < e && *r == ':') {
                ++r;
                while (r < e &&
                       (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n'))
                    ++r;
                if (r < e && *r == '[') {
                    arr = r + 1;
                    break;
                }
            }
            p = q;
            continue;
        }
        ++p;
    }
    if (!arr) return false;
    p = arr;
    while (p < e) {
        if (*p == ']') break;
        if (*p != '{') {
            ++p;
            continue;
        }
        const char* ob = p;
        int depth = 0;
        const char* q = p;
        while (q < e) {
            if (*q == '"') {
                std::string s;
                const char* r = q;
                if (!srvJsonStr(r, e, s)) {
                    q = e;
                    break;
                }
                q = r;
                continue;
            }
            if (*q == '{' || *q == '[') ++depth;
            else if (*q == '}' || *q == ']') {
                --depth;
                if (depth == 0) {
                    ++q;
                    break;
                }
            }
            ++q;
        }
        SrvRow row;
        std::string v;
        if (srvJsonField(ob, q, "id", v, true) && !v.empty()) row.id = v;
        if (srvJsonField(ob, q, "maxPlayers", v, false))
            row.maxPlayers = atoi(v.c_str());
        if (srvJsonField(ob, q, "ping", v, false)) row.ping = atoi(v.c_str());
        if (srvJsonField(ob, q, "playing", v, false))
            row.playing = atoi(v.c_str());
        else
            row.playing = srvJsonCountArray(ob, q, "playerTokens");
        if (!row.id.empty()) rows.push_back(row);
        p = q;
    }
    return true;
}

// ---- state (main thread owns all of it; the worker only touches g_srvWire) --
SrvState srv;

// Worker -> UI/main handoff (same shape as g_snapMu for the aim snapshot).
SrvWire g_srvWire;
static std::atomic<bool> g_srvRun{true};
static std::atomic<bool> g_srvWantList{false};
static std::atomic<unsigned long long> g_srvListPlace{0};
static std::atomic<bool> g_srvWantGeo{false};
static std::mutex g_srvGeoMu;
static std::string g_srvGeoIpReq;
static int g_srvListSeqSeen = 0;
static int g_srvGeoSeqSeen = 0;
int g_srvSel = -1; // selected row in the filtered list (UI only)

// ---- small helpers ---------------------------------------------------------
static std::string srvWideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    if (n <= 0) return "";
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr,
                        nullptr);
    return s;
}
static std::wstring srvUtf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// The panel cannot take keyboard focus (WS_EX_NOACTIVATE), so there is no
// InputText anywhere: clipboard is the only input path.
bool srvClipGet(std::string& out) {
    out.clear();
    if (!OpenClipboard(nullptr)) return false;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) {
            out = srvWideToUtf8(w);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return !out.empty();
}
bool srvClipSet(const std::string& s) {
    if (s.empty()) return false;
    std::wstring w = srvUtf8ToWide(s);
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    bool ok = false;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (mem) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            if (SetClipboardData(CF_UNICODETEXT, mem)) ok = true;
            else GlobalFree(mem); // ownership only transfers on success
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
    return ok;
}

// jobId = UUID with dashes (32..40 so both dashed and bare forms work; A-F
// accepted because Roblox has shipped uppercase ids before).
static bool srvValidJob(const std::string& s) {
    if (s.size() < 32 || s.size() > 40) return false;
    for (char c : s) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F') || c == '-')
            continue;
        return false;
    }
    return true;
}
std::string srvShort(const std::string& j) {
    return j.size() > 8 ? j.substr(0, 8) : j;
}

// Display form of DataModel.ServerIP: keep the raw string, but for the geo
// lookup strip an optional "|port" / ":port" (IPv6 colons are left alone).
static std::string srvIpOnly(const std::string& raw) {
    std::string s = raw;
    size_t bar = s.find('|');
    if (bar != std::string::npos) s = s.substr(0, bar);
    size_t c = s.rfind(':');
    if (c != std::string::npos && s.find(':') == c) {
        std::string port = s.substr(c + 1);
        bool digits = !port.empty() && port.size() <= 5;
        for (char ch : port)
            if (ch < '0' || ch > '9') digits = false;
        if (digits) s = s.substr(0, c);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    if (s.find(':') != std::string::npos) return ""; // IPv6: skip geo
    return s;
}

// Clipboard input: bare jobId, roblox:// URI or the web start link. Params may
// appear in any order; a missing placeId means "current place".
bool srvParseJoin(const std::string& raw, uint64_t curPlace,
                         SrvJoin& out) {
    out = SrvJoin{};
    size_t a = raw.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return false;
    size_t z = raw.find_last_not_of(" \t\r\n");
    std::string s = raw.substr(a, z - a + 1);
    size_t jp = s.find("gameInstanceId=");
    size_t pp = s.find("placeId=");
    if (jp != std::string::npos) {
        size_t vs = jp + 15;
        size_t ve = s.find_first_of("&#", vs);
        std::string job = s.substr(vs, ve == std::string::npos
                                           ? std::string::npos
                                           : ve - vs);
        if (!srvValidJob(job)) return false;
        out.job = job;
        if (pp != std::string::npos) {
            size_t ps = pp + 8;
            size_t pe = s.find_first_of("&#", ps);
            std::string pv = s.substr(ps, pe == std::string::npos
                                              ? std::string::npos
                                              : pe - ps);
            out.place = strtoull(pv.c_str(), nullptr, 10);
        }
        if (!out.place) out.place = curPlace;
        return true;
    }
    if (srvValidJob(s)) {
        out.job = s;
        out.place = curPlace;
        return true;
    }
    return false;
}

static std::string srvBuildUri(uint64_t place, const std::string& job) {
    char b[320];
    if (kSrvWebUriFallback)
        snprintf(b, sizeof b,
                 "https://www.roblox.com/games/start?placeId=%llu&gameInstanceId=%s",
                 (unsigned long long)place, job.c_str());
    else
        snprintf(b, sizeof b,
                 "roblox://experiences/start?placeId=%llu&gameInstanceId=%s",
                 (unsigned long long)place, job.c_str());
    return b;
}
std::string srvBuildLink(uint64_t place, const std::string& job) {
    char b[320];
    snprintf(b, sizeof b,
             "https://www.roblox.com/games/start?placeId=%llu&gameInstanceId=%s",
             (unsigned long long)place, job.c_str());
    return b;
}

// Open a join URI. srv.lastjob/srv.lastplace are written to overlay.cfg BEFORE
// the URI is opened: hopping starts a new client and this overlay is expected
// to exit with the old one (accepted v1 behaviour, see HANDOFF).
void srvOpenJoin(uint64_t place, const std::string& job, const char* why) {
    if (!place) {
        toast("join: no placeId yet");
        return;
    }
    if (!srvValidJob(job)) {
        toast("join: bad jobId");
        return;
    }
    cfgSrvLastJob = job;
    cfgSrvLastPlace = place;
    upsertCfgKey("srv.lastjob", job.c_str());
    char pb[32];
    snprintf(pb, sizeof pb, "%llu", (unsigned long long)place);
    upsertCfgKey("srv.lastplace", pb);
    srv.joinCd = GetTickCount();
    srv.arFired = false;
    srv.unloadTick = 0;
    std::string uri = srvBuildUri(place, job);
    toast("joining %s..", srvShort(job).c_str());
    diagRate("srv-join", 0, "join place=%llu job=%s why=%s uri=%s",
             (unsigned long long)place, job.c_str(), why, uri.c_str());
    std::wstring w = srvUtf8ToWide(uri);
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---- real ping (4.2) -------------------------------------------------------
// Stats -> "Data Ping" (StatsItem) -> Value. Value has been observed as double
// (1280 holds 8 bytes); float is tried as a fallback. Pointer cached, re-scan
// at most once per second, unknown => UI shows "--" and one diag line per 5 s.
static uint64_t g_srvPingItem = 0;
static DWORD g_srvPingFind = 0;
static int srvReadPing(Memory& m, SDK& sdk, const Offsets& o, uint64_t dm,
                       DWORD now) {
    if (!o.statsItemValue) return -1;
    if (!g_srvPingItem && now - g_srvPingFind >= 1000) {
        g_srvPingFind = now;
        uint64_t stats = 0;
        if (sdk.findChildByClass(dm, "Stats", stats)) {
            sdk.walkChildren(stats, [&](uint64_t it) {
                if (sdk.readName(it) == "Data Ping") {
                    g_srvPingItem = it;
                    return false;
                }
                return true;
            });
        }
        if (!g_srvPingItem)
            diagRate("srv-ping-miss", 5000,
                     "srv ping: Stats/Data Ping not found (showing --)");
    }
    if (!g_srvPingItem) return -1;
    int v = -1;
    double d = 0.0;
    if (m.read(g_srvPingItem + (uint64_t)o.statsItemValue, d)) {
        if (d >= 1.0 && d <= 100000.0) v = (int)d;
    }
    if (v < 0) {
        float f = 0.0f;
        if (m.read(g_srvPingItem + (uint64_t)o.statsItemValue, f)) {
            if (f >= 1.0f && f <= 100000.0f) v = (int)f;
        }
    }
    if (v < 0) {
        g_srvPingItem = 0; // instance changed under us; rescan next second
        diagRate("srv-ping-read", 5000,
                 "srv ping: value unreadable (showing --)");
    }
    return v;
}

// ---- filtering / requests --------------------------------------------------
std::vector<SrvRow> srvFilterRows(const std::vector<SrvRow>& in,
                                         const std::string& curJob) {
    std::vector<SrvRow> out;
    for (const SrvRow& r : in) {
        if (!srvValidJob(r.id)) continue;
        if (!curJob.empty() && _stricmp(r.id.c_str(), curJob.c_str()) == 0)
            continue; // never "hop" to the server we are already on
        if (r.playing < cfgSrvHopMin) continue;
        if (cfgSrvHopFresh && r.maxPlayers > 0 && r.playing >= r.maxPlayers)
            continue; // full
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const SrvRow& a, const SrvRow& b) {
        int pa = a.ping > 0 ? a.ping : 999999;
        int pb = b.ping > 0 ? b.ping : 999999;
        return pa < pb;
    });
    return out;
}

void srvRequestList(uint64_t place) {
    if (!place) {
        toast("no placeId yet");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_srvWire.mu);
        if (g_srvWire.listState == 1) return; // already loading
        g_srvWire.listState = 1;
    }
    g_srvListPlace.store((unsigned long long)place);
    g_srvWantList.store(true);
    diagRate("srv-list-req", 0, "srv list request place=%llu",
             (unsigned long long)place);
}

static void srvRequestGeo(const std::string& ip) {
    if (ip.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_srvWire.mu);
        if (g_srvWire.geoState == 1) return;
        g_srvWire.geoState = 1;
    }
    {
        std::lock_guard<std::mutex> lk(g_srvGeoMu);
        g_srvGeoIpReq = ip;
    }
    srv.geoTry = GetTickCount();
    g_srvWantGeo.store(true);
    diagRate("srv-geo-req", 0, "geo request ip=%s (srv.geo on)", ip.c_str());
}

// ---- HTTP fetch (worker thread) --------------------------------------------
// Primary endpoint per PLAN §4.3; on 401/403/odd JSON fall back to the
// apis.roblox.com universe lookup + the v2 server list.
static bool srvFetchList(uint64_t place, std::vector<SrvRow>& rows, int& code,
                         std::string& endpoint) {
    rows.clear();
    endpoint.clear();
    code = 0;
    if (!place) return false;
    char pth[320];
    snprintf(pth, sizeof pth,
             "/v1/games/%llu/servers/Public?sortOrder=Asc&excludeFullGames=true&limit=100",
             (unsigned long long)place);
    int st = 0;
    std::string body =
        httpGetEx(L"games.roblox.com", srvUtf8ToWide(pth), kSrvHttpTimeoutMs,
                  kSrvUa, &st);
    code = st;
    if (!g_srvRun.load(std::memory_order_relaxed)) return false; // shutting down
    if (st == 200 && srvParseList(body, rows)) {
        endpoint = "v1";
        return true;
    }
    diagRate("srv-list-v1", 0, "srv v1 list failed http=%d body=%d bytes -> v2",
             st, (int)body.size());

    snprintf(pth, sizeof pth, "/universes/v1/places/%llu/universe",
             (unsigned long long)place);
    st = 0;
    body = httpGetEx(L"apis.roblox.com", srvUtf8ToWide(pth), kSrvHttpTimeoutMs,
                     kSrvUa, &st);
    if (!g_srvRun.load(std::memory_order_relaxed)) return false;
    if (st != 200) {
        code = st;
        diagRate("srv-list-uni", 0, "srv universe lookup failed http=%d", st);
        return false;
    }
    std::string uni;
    if (!srvJsonField(body.c_str(), body.c_str() + body.size(), "universeId",
                      uni, false) ||
        uni.empty() || uni == "null") {
        code = st;
        diagRate("srv-list-uni", 0, "srv universe lookup: no universeId");
        return false;
    }
    snprintf(pth, sizeof pth,
             "/v2/games/%s/servers/Public?sortOrder=Asc&excludeFullGames=true&limit=100",
             uni.c_str());
    st = 0;
    body = httpGetEx(L"games.roblox.com", srvUtf8ToWide(pth), kSrvHttpTimeoutMs,
                     kSrvUa, &st);
    code = st;
    if (!g_srvRun.load(std::memory_order_relaxed)) return false;
    if (st == 200 && srvParseList(body, rows)) {
        endpoint = "v2";
        return true;
    }
    diagRate("srv-list-v2", 0, "srv v2 list failed http=%d", st);
    return false;
}

static bool srvFetchGeo(const std::string& ip, std::string& city,
                        std::string& country, int& code) {
    city.clear();
    country.clear();
    code = 0;
    if (ip.empty()) return false;
    std::string path = "/" + ip;
    int st = 0;
    std::string body =
        httpGetEx(L"ipwho.is", srvUtf8ToWide(path), kSrvHttpTimeoutMs,
                  kSrvUaGeo, &st);
    code = st;
    if (st != 200) return false;
    const char* b = body.c_str();
    const char* e = b + body.size();
    std::string okv;
    if (!srvJsonField(b, e, "success", okv, false) || okv != "true") return false;
    srvJsonField(b, e, "city", city, true);
    srvJsonField(b, e, "country", country, true);
    return !city.empty() || !country.empty();
}

// Worker: only fetches and publishes state. cfg/log writes stay on the main
// thread; the only thing it logs is diag (mutex-protected file).
static void srvWorkerThread() {
    while (g_srvRun.load(std::memory_order_relaxed)) {
        bool did = false;
        if (g_srvWantList.exchange(false)) {
            uint64_t place = (uint64_t)g_srvListPlace.load();
            std::vector<SrvRow> rows;
            int code = 0;
            std::string ep;
            bool ok = srvFetchList(place, rows, code, ep);
            std::lock_guard<std::mutex> lk(g_srvWire.mu);
            g_srvWire.listCode = code;
            g_srvWire.listEndpoint = ep;
            if (ok) {
                g_srvWire.rows.swap(rows);
                g_srvWire.listState = 2;
            } else {
                g_srvWire.listState = 3;
            }
            ++g_srvWire.listSeq;
            did = true;
        }
        if (g_srvWantGeo.exchange(false)) {
            std::string ip;
            {
                std::lock_guard<std::mutex> lk(g_srvGeoMu);
                ip = g_srvGeoIpReq;
            }
            std::string city, country;
            int code = 0;
            bool ok = srvFetchGeo(ip, city, country, code);
            std::lock_guard<std::mutex> lk(g_srvWire.mu);
            g_srvWire.geoIp = ip;
            g_srvWire.geoCity = city;
            g_srvWire.geoCountry = country;
            g_srvWire.geoState = ok ? 2 : 3;
            ++g_srvWire.geoSeq;
            did = true;
        }
        if (!did) Sleep(40);
    }
}

// ---- 4.10 roster diff (optional) -------------------------------------------
// Client-side approximation: names come from the entity list, which is name-
// only, so a player still streaming in can look like a join. To keep the
// toasts honest the first fill and any empty roster are skipped silently.
static void srvDiffRoster(const std::vector<std::string>& names) {
    std::set<std::string> cur;
    for (const std::string& n : names)
        if (!n.empty() && n.size() <= 32) cur.insert(n);
    if (srv.firstRoster || srv.roster.empty() || cur.empty()) {
        srv.firstRoster = false;
        srv.roster = cur;
        return;
    }
    int joined = 0, left = 0;
    for (const std::string& n : cur)
        if (!srv.roster.count(n)) {
            if (joined < 3) toast("%s joined the server", n.c_str());
            ++joined;
        }
    for (const std::string& n : srv.roster)
        if (!cur.count(n)) {
            if (left < 3) toast("%s left the server", n.c_str());
            ++left;
        }
    if (joined || left)
        diagRate("srv-roster", 0, "roster +%d -%d (%zu online)", joined, left,
                 cur.size());
    srv.roster.swap(cur);
}

static void srvDrainWire() {
    int lseq = 0, lstate = 0, lcode = 0, gseq = 0, gstate = 0;
    size_t n = 0;
    std::string ep, city, country, gip;
    {
        std::lock_guard<std::mutex> lk(g_srvWire.mu);
        lseq = g_srvWire.listSeq;
        lstate = g_srvWire.listState;
        lcode = g_srvWire.listCode;
        n = g_srvWire.rows.size();
        ep = g_srvWire.listEndpoint;
        gseq = g_srvWire.geoSeq;
        gstate = g_srvWire.geoState;
        city = g_srvWire.geoCity;
        country = g_srvWire.geoCountry;
        gip = g_srvWire.geoIp;
    }
    if (lseq && lseq != g_srvListSeqSeen) {
        g_srvListSeqSeen = lseq;
        if (lstate == 2) {
            diagRate("srv-list", 0, "srv list ok endpoint=%s http=%d servers=%zu",
                     ep.c_str(), lcode, n);
        } else {
            toast("server list failed (HTTP %d)", lcode);
            diagRate("srv-list", 0, "srv list FAIL endpoint=%s http=%d",
                     ep.c_str(), lcode);
        }
    }
    if (gseq && gseq != g_srvGeoSeqSeen) {
        g_srvGeoSeqSeen = gseq;
        if (gstate == 2) {
            srv.regionIp = gip;
            if (city.empty()) srv.region = country;
            else if (country.empty()) srv.region = city;
            else srv.region = city + ", " + country;
            diagRate("srv-geo", 0, "geo ip=%s -> %s", gip.c_str(),
                     srv.region.c_str());
        } else {
            srv.region = "?";
            diagRate("srv-geo", 0, "geo ip=%s FAIL", gip.c_str());
        }
    }
}

// ---- per-frame tick --------------------------------------------------------
// Called from the main loop next to worldTick(), i.e. BEFORE the "panel hidden"
// early-exit: the watchdog and anti-AFK must live outside the UI.
static void srvTick(Memory& m, SDK& sdk, const Offsets& o, DWORD now,
                    bool fgGame, bool typingNow, bool aiming,
                    const std::vector<std::string>& names, int entCount) {
    if (now - srv.lastInfo >= 1000) {
        srv.lastInfo = now;
        uint64_t dm = 0;
        bool haveDm = sdk.dataModel(dm);
        bool loadedNow = false;
        if (haveDm) {
            uint64_t v = 0;
            if (o.dataModelPlaceId && sdk.placeId(dm, v) && v) srv.place = v;
            if (o.dataModelGameId && sdk.gameId(dm, v) && v) srv.gameId = v;
            int pv = 0;
            if (o.dataModelPlaceVersion &&
                m.read(dm + (uint64_t)o.dataModelPlaceVersion, pv))
                srv.placeVersion = pv;
            std::string job;
            if (o.dataModelJobId)
                job = sdk.readTStringDirect(dm + (uint64_t)o.dataModelJobId);
            if (srvValidJob(job)) {
                if (job != srv.jobId) {
                    // jobId changed => new server: reset every per-server bit.
                    srv.jobId = job;
                    srv.jobSeen = now;
                    srv.region.clear();
                    srv.regionIp.clear();
                    srv.ping = -1;
                    srv.firstRoster = true;
                    srv.roster.clear();
                    srv.arFired = false;
                    srv.unloadTick = 0;
                    g_srvPingItem = 0;
                    if (srv.place) {
                        cfgSrvLastJob = job;
                        cfgSrvLastPlace = srv.place;
                        upsertCfgKey("srv.lastjob", job.c_str());
                        char pb[32];
                        snprintf(pb, sizeof pb, "%llu",
                                 (unsigned long long)srv.place);
                        upsertCfgKey("srv.lastplace", pb);
                    }
                    diagRate("srv-job", 0, "srv job=%s place=%llu",
                             job.c_str(), (unsigned long long)srv.place);
                }
            } else if (!job.empty()) {
                diagRate("srv-badjob", 10000,
                         "srv jobId rejected (len=%d)", (int)job.size());
            }
            std::string ip;
            if (o.dataModelServerIP)
                ip = sdk.readTStringDirect(dm + (uint64_t)o.dataModelServerIP);
            if (!ip.empty()) srv.serverIP = ip; // keep old value on read error
            srv.ping = srvReadPing(m, sdk, o, dm, now);
            srv.players = (int)names.size();
            int loaded = 0;
            if (o.dataModelGameLoaded &&
                m.read(dm + (uint64_t)o.dataModelGameLoaded, loaded)) {
                loadedNow = loaded != 0;
                if (loadedNow) {
                    srv.wasLoaded = true;
                    srv.loaded = true;
                    srv.arFired = false;
                    srv.unloadTick = 0;
                } else {
                    srv.loaded = false;
                }
            }
        }
        srv.loaded = loadedNow;
        // 4.10 roster diff runs on the same 1 s sample as the entity cache.
        srvDiffRoster(names);

        // Region lookup: explicit opt-in, one request per IP (retry 60 s).
        if (cfgSrvGeo) {
            std::string ip = srvIpOnly(srv.serverIP);
            if (!ip.empty() && ip != srv.regionIp &&
                (!srv.geoTry || now - srv.geoTry >= 60000))
                srvRequestGeo(ip);
        }

        // Auto-rejoin: was loaded once, now unloaded, no entities for 5 s.
        // The 5 s window must be continuous: any sample that breaks the
        // condition resets the timer.
        bool arCond = cfgSrvAutoRejoin && srv.wasLoaded && !srv.loaded &&
                      entCount == 0 && !cfgSrvLastJob.empty() && !srv.arFired &&
                      (srv.joinCd == 0 || now - srv.joinCd >= 60000);
        if (!arCond) {
            srv.unloadTick = 0;
        } else if (!srv.unloadTick) {
            srv.unloadTick = now;
        } else if (now - srv.unloadTick >= 5000) {
            srv.arFired = true;
            srv.unloadTick = 0;
            toast("auto-rejoin %s..", srvShort(cfgSrvLastJob).c_str());
            diagRate("srv-autorejoin", 0,
                     "auto-rejoin job=%s place=%llu loaded=0 ents=0 held=5s",
                     cfgSrvLastJob.c_str(),
                     (unsigned long long)cfgSrvLastPlace);
            srvOpenJoin(cfgSrvLastPlace, cfgSrvLastJob, "auto-rejoin");
        }

        srvDrainWire();
    }

    // Anti-AFK: relative +1/-1 px, no click. Foreground game only, never while
    // typing in chat or while aim is held.
    if (cfgSrvAntiAfk && fgGame && !typingNow && !aiming) {
        if (!srv.afkNext) srv.afkNext = now + (DWORD)cfgSrvAfkMin * 60000;
        if ((long)(now - srv.afkNext) >= 0) {
            INPUT n1{};
            n1.type = INPUT_MOUSE;
            n1.mi.dx = 1;
            n1.mi.dy = 0;
            n1.mi.dwFlags = MOUSEEVENTF_MOVE;
            INPUT n2 = n1;
            n2.mi.dx = -1;
            SendInput(1, &n1, sizeof n1);
            SendInput(1, &n2, sizeof n2);
            srv.afkNext = now + (DWORD)cfgSrvAfkMin * 60000;
            diagRate("srv-antiafk", 0,
                     "anti-afk nudge +1/-1 px (interval %d min)", cfgSrvAfkMin);
        }
    } else if (!cfgSrvAntiAfk) {
        srv.afkNext = 0;
    }
}

// ---- self-test (-srvtest <placeId>): no game, no UI --------------------------
static int runSrvSelfTest(uint64_t place) {
    printf("=== ext_overlay server tab self-test (no game) ===\n");
    printf("place=%llu\n\n", (unsigned long long)place);
    int fails = 0;

    printf("[1] jobId validation\n");
    {
        const char* good = "8b1a4e2c-3d4f-4a5b-9c6d-7e8f90123456";
        const char* good2 = "8B1A4E2C3D4F4A5B9C6D7E8F90123456";
        const char* bad1 = "";
        const char* bad2 = "abc";
        const char* bad3 = "8b1a4e2c-3d4f-4a5b-9c6d-7e8f9012345g";
        bool a = srvValidJob(good), b = srvValidJob(good2),
             c = !srvValidJob(bad1), d = !srvValidJob(bad2),
             f = !srvValidJob(bad3);
        printf("  uuid=%d bare=%d empty=%d short=%d badhex=%d %s\n", a, b, c, d,
               f, (a && b && c && d && f) ? "PASS" : "FAIL");
        if (!(a && b && c && d && f)) ++fails;
    }

    printf("[2] ServerIP -> IP for geo\n");
    {
        struct Case { const char* in; const char* out; };
        static const Case cs[] = {
            { "1.2.3.4|53640", "1.2.3.4" },
            { "1.2.3.4:53640", "1.2.3.4" },
            { "1.2.3.4", "1.2.3.4" },
            { "2001:db8::1", "" }, // IPv6: geo skipped
        };
        int ok = 0;
        for (const Case& c : cs) {
            std::string got = srvIpOnly(c.in);
            bool p = got == c.out;
            ok += p;
            printf("  %-18s -> %-10s %s\n", c.in, got.c_str(), p ? "PASS" : "FAIL");
        }
        if (ok != (int)(sizeof cs / sizeof cs[0])) ++fails;
    }

    printf("[3] clipboard join parser\n");
    {
        const char* job = "8b1a4e2c-3d4f-4a5b-9c6d-7e8f90123456";
        char uri[256], web[256];
        snprintf(uri, sizeof uri,
                 "roblox://experiences/start?gameInstanceId=%s&placeId=123", job);
        snprintf(web, sizeof web,
                 "https://www.roblox.com/games/start?placeId=123&gameInstanceId=%s",
                 job);
        SrvJoin j1, j2, j3, j4;
        bool r1 = srvParseJoin(job, 7, j1) && j1.job == job && j1.place == 7;
        bool r2 = srvParseJoin(uri, 7, j2) && j2.job == job && j2.place == 123;
        bool r3 = srvParseJoin(web, 7, j3) && j3.job == job && j3.place == 123;
        bool r4 = !srvParseJoin("hello world", 7, j4);
        printf("  bare=%d roblox=%d web=%d reject=%d %s\n", r1, r2, r3, r4,
               (r1 && r2 && r3 && r4) ? "PASS" : "FAIL");
        if (!(r1 && r2 && r3 && r4)) ++fails;
        std::string u = srvBuildUri(123, job);
        bool ru = u.find("roblox://experiences/start") == 0 &&
                  u.find(job) != std::string::npos;
        printf("  uri  %s %s\n", u.c_str(), ru ? "PASS" : "FAIL");
        if (!ru) ++fails;
    }

    printf("[4] server-list JSON parser\n");
    {
        const std::string body =
            "{\"previousPageCursor\":null,\"data\":["
            "{\"id\":\"8b1a4e2c-3d4f-4a5b-9c6d-7e8f90123456\",\"maxPlayers\":50,"
            "\"ping\":30,\"fps\":60,\"playerTokens\":[\"a\",\"b\",\"c\"]},"
            "{\"id\":\"9c2b5f3d-4e5a-4b6c-8d7e-0f1a2b3c4d5e\",\"maxPlayers\":50,"
            "\"playing\":12,\"ping\":90,\"fps\":59},"
            "{\"id\":\"not-a-job\",\"maxPlayers\":10,\"playing\":1,\"ping\":5}"
            "],\"nextPageCursor\":\"x\"}";
        std::vector<SrvRow> rows;
        bool ok = srvParseList(body, rows);
        bool shape = ok && rows.size() == 3 && rows[0].playing == 3 &&
                     rows[1].playing == 12 && rows[0].maxPlayers == 50 &&
                     rows[2].id == "not-a-job";
        printf("  parse=%d rows=%zu first_playing=%d second_playing=%d %s\n",
               ok ? 1 : 0, rows.size(),
               rows.empty() ? -1 : rows[0].playing,
               rows.size() < 2 ? -1 : rows[1].playing,
               shape ? "PASS" : "FAIL");
        if (!shape) ++fails;
    }

    printf("[5] hop filter\n");
    {
        int savedMin = cfgSrvHopMin, savedFresh = cfgSrvHopFresh;
        cfgSrvHopMin = 5;
        cfgSrvHopFresh = 1;
        std::vector<SrvRow> in;
        auto add = [&](const char* id, int pl, int mx, int pg) {
            SrvRow r;
            r.id = id;
            r.playing = pl;
            r.maxPlayers = mx;
            r.ping = pg;
            in.push_back(r);
        };
        add("8b1a4e2c-3d4f-4a5b-9c6d-7e8f90123456", 20, 50, 10); // current
        add("9c2b5f3d-4e5a-4b6c-8d7e-0f1a2b3c4d5e", 3, 50, 5);    // too empty
        add("0f1a2b3c-4d5e-4f60-8a7b-9c8d7e6f5a4b", 50, 50, 5);   // full
        add("11111111-2222-4333-8444-555555555555", 8, 50, 120);
        add("22222222-3333-4444-8555-666666666666", 30, 50, 40);
        std::vector<SrvRow> out =
            srvFilterRows(in, "8b1a4e2c-3d4f-4a5b-9c6d-7e8f90123456");
        bool ok = out.size() == 2 && out[0].ping == 40 && out[1].ping == 120;
        printf("  kept=%zu best_ping=%d %s\n", out.size(),
               out.empty() ? -1 : out[0].ping, ok ? "PASS" : "FAIL");
        cfgSrvHopMin = savedMin;
        cfgSrvHopFresh = savedFresh;
        if (!ok) ++fails;
    }

    printf("\n[6] network (WARN only - offline is not a failure)\n");
    {
        std::vector<SrvRow> rows;
        int code = 0;
        std::string ep;
        DWORD t0 = GetTickCount();
        bool ok = srvFetchList(place, rows, code, ep);
        DWORD dt = GetTickCount() - t0;
        printf("  list: %s endpoint=%s http=%d servers=%zu (%lu ms)\n",
               ok ? "OK" : "FAIL", ep.empty() ? "-" : ep.c_str(), code,
               rows.size(), (unsigned long)dt);
        for (size_t i = 0; i < rows.size() && i < 3; ++i)
            printf("    %s  %d/%d  ping %d\n", srvShort(rows[i].id).c_str(),
                   rows[i].playing, rows[i].maxPlayers, rows[i].ping);
        if (ok) {
            std::vector<SrvRow> filt = srvFilterRows(rows, "");
            printf("  filter(default hopmin=%d fresh=%d): %zu of %zu\n",
                   cfgSrvHopMin, cfgSrvHopFresh, filt.size(), rows.size());
        }
        std::string city, country;
        int gcode = 0;
        bool gok = srvFetchGeo("1.1.1.1", city, country, gcode);
        printf("  geo:  %s http=%d %s%s%s\n", gok ? "OK" : "WARN", gcode,
               city.c_str(), city.empty() ? "" : ", ", country.c_str());
    }

    printf("\n=== done (%s) ===\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

static bool buildAimMatrix(const Vec3& tgt, const Vec3& cm, float out[9]) {
    // Game CFrame layout: columns = Right, Up, -Look.
    float lx = tgt.x - cm.x, ly = tgt.y - cm.y, lz = tgt.z - cm.z;
    float ln = sqrtf(lx * lx + ly * ly + lz * lz);
    if (!(ln > 0.001f)) return false;
    lx /= ln;
    ly /= ln;
    lz /= ln;
    float bx = -lx, by = -ly, bz = -lz; // back = -look
    float ux = 0.0f, uy = 1.0f, uz = 0.0f;
    if (fabsf(ly) > 0.999f) {
        ux = 1.0f;
        uy = 0.0f;
        uz = 0.0f;
    }
    float rx = uy * bz - uz * by, ry = uz * bx - ux * bz,
          rz = ux * by - uy * bx;
    float rn = sqrtf(rx * rx + ry * ry + rz * rz);
    if (!(rn > 0.0001f)) return false;
    rx /= rn;
    ry /= rn;
    rz /= rn;
    float u2x = by * rz - bz * ry, u2y = bz * rx - bx * rz,
          u2z = bx * ry - by * rx;
    out[0] = rx;
    out[1] = u2x;
    out[2] = bx;
    out[3] = ry;
    out[4] = u2y;
    out[5] = by;
    out[6] = rz;
    out[7] = u2z;
    out[8] = bz;
    return true;
}

static Vec3 snapPos(const AimSnap& s, double nowQ) {
    // Extrapolate between 60Hz snapshots so 240Hz ticks pursue smoothly.
    Vec3 w = s.tgt;
    if (cfgAimPredict) {
        float dte = (float)(nowQ - s.qpc);
        if (dte < 0) dte = 0;
        if (dte > 0.25f) dte = 0.25f;
        float lead = dte * cfgAimPScale;
        w.x += s.vel.x * lead;
        w.y += s.vel.y * lead;
        w.z += s.vel.z * lead;
    }
    return w;
}

// ---- mode-1 writer thread ---------------------------------------------------
// velext model (aim.pid=0, default): at most ONE actuation per published
// snapshot (<=1 per main-loop iteration), 4 ms hard gate (<=250 Hz), stateless
// nlerp from the freshly re-read camera rotation, dot>0.99996 deadzone.
// aim.pid=1 keeps the legacy free-running slerp path for A/B comparison.
static void writerThread(Memory* pm, int rotOff) {
    timeBeginPeriod(1);
    double prevQ = qpcSeconds();
    uint64_t lastSeqApplied = 0;
    while (g_writerRun.load(std::memory_order_relaxed)) {
        AimSnap s;
        {
            std::lock_guard<std::mutex> lk(g_snapMu);
            s = g_snap;
        }
        double nowQ = qpcSeconds();
        double writeDt = nowQ - prevQ;
        prevQ = nowQ;
        if (writeDt < 0.0005) writeDt = 0.0005;
        if (writeDt > 0.05) writeDt = 0.05;
        uint64_t currentCam = g_currentCam.load(std::memory_order_acquire);
        uint64_t currentEpoch = g_snapEpoch.load(std::memory_order_acquire);
        if (s.ready) {
            double ageMs = (nowQ - s.qpc) * 1000.0;
            if (s.epoch != currentEpoch)
                diagRate("writer-epoch", 500, "writer skip: epoch snap=%llu current=%llu",
                         (unsigned long long)s.epoch,
                         (unsigned long long)currentEpoch);
            else if (s.camAddr != currentCam)
                diagRate("writer-camera", 500, "writer skip: camera snap=%p current=%p",
                         (void*)s.camAddr, (void*)currentCam);
            else if (ageMs >= 80.0)
                diagRate("writer-age", 500, "writer skip: snapshot age=%.1fms", ageMs);
        }
        bool fresh = s.ready && s.epoch == currentEpoch && s.camAddr == currentCam &&
                     s.camAddr && pm && pm->h && (nowQ - s.qpc) < 0.08;
        if (cfgAimPid == 1) {
            // ---- legacy path (unchanged): free-running re-apply at aim.rate Hz
            if (fresh) {
                float tar[9];
                if (buildAimMatrix(snapPos(s, nowQ), s.cm, tar)) {
                    float slow = cfgAimMemSmooth;
                    if (slow < 0) slow = 0;
                    if (slow > 0.99f) slow = 0.99f;
                    if (slow > 0.001f) {
                        float cur[9];
                        if (pm->read(s.camAddr + (uint64_t)rotOff, cur)) {
                            Quat a = matToQuat(cur);
                            Quat b = matToQuat(tar);
                            // Frame-rate independent exponential smoothing.
                            float tau = 0.012f + 0.10f * slow;
                            float t = 1.0f - expf((float)(-writeDt / tau));
                            quatToMat(slerpQ(a, b, t), tar);
                        } else {
                            diagRate("writer-read-rotation", 500,
                                     "writer read rotation failed cam=%p off=0x%X",
                                     (void*)s.camAddr, rotOff);
                        }
                    }
                    if (pm->write(s.camAddr + (uint64_t)rotOff, tar))
                        g_writerCount.fetch_add(1, std::memory_order_relaxed);
                    else {
                        diagRate("writer-write", 500,
                                 "writer write rotation failed cam=%p off=0x%X",
                                 (void*)s.camAddr, rotOff);
                        std::lock_guard<std::mutex> lk(g_snapMu);
                        g_snapEpoch.fetch_add(1, std::memory_order_acq_rel);
                        g_snap.ready = false;
                        g_snap.epoch = g_snapEpoch.load(std::memory_order_relaxed);
                    }
                }
            }
        } else if (fresh && s.seq != 0 && s.seq != lastSeqApplied) {
            // ---- velext model: <=1 actuation per published snapshot
            uint64_t lastNs = 0;
            float dt = 0.0f, alpha = 1.0f;
            if (!velextActGate(qpcNs(), &lastNs, &dt, &alpha, cfgAimSmoothing)) {
                // < 4 ms since the previous actuation: keep the snapshot and
                // retry next pass (it may get replaced by a newer one first).
            } else {
                lastSeqApplied = s.seq;
                float cur[9];
                if (!pm->read(s.camAddr + (uint64_t)rotOff, cur)) {
                    diagRate("writer-read-rotation", 500,
                             "writer read rotation failed cam=%p off=0x%X",
                             (void*)s.camAddr, rotOff);
                } else {
                    // look = -(m[2],m[5],m[8]) (calib-proven), dir = normalize(aim-cam)
                    float lx = -cur[2], ly = -cur[5], lz = -cur[8];
                    Vec3 t = snapPos(s, nowQ);
                    float dx = t.x - s.cm.x, dy = t.y - s.cm.y, dz = t.z - s.cm.z;
                    float dl = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (!(dl > 0.001f)) {
                        diagRate("writer-target", 500, "writer: degenerate target dir");
                    } else {
                        dx /= dl; dy /= dl; dz /= dl;
                        float dot = lx * dx + ly * dy + lz * dz;
                        if (dot > 1.0f) dot = 1.0f;
                        if (dot < -1.0f) dot = -1.0f;
                        if (dot > kVelextDotDeadzone) {
                            // [P] 0.99996 (~0.51 deg): no write, let it settle
                            diagRate("writer-deadzone", 500,
                                     "writer deadzone: dot=%.6f err=%.3f deg",
                                     dot, acosf(dot) * 57.2957795f);
                        } else {
                            // nlerp then normalize (never slerp / euler)
                            float nx = lx + (dx - lx) * alpha;
                            float ny = ly + (dy - ly) * alpha;
                            float nz = lz + (dz - lz) * alpha;
                            float nl = sqrtf(nx * nx + ny * ny + nz * nz);
                            if (nl < 1e-3f) { // velext fallback: use dir directly
                                nx = dx; ny = dy; nz = dz;
                                nl = 1.0f;
                            }
                            if (nl < 1e-6f) {
                                diagRate("writer-degenerate", 500, "writer: zero mix");
                            } else {
                                nx /= nl; ny /= nl; nz /= nl;
                                float tar[9];
                                Vec3 t2{ s.cm.x + nx, s.cm.y + ny, s.cm.z + nz };
                                if (buildAimMatrix(t2, s.cm, tar)) {
                                    if (pm->write(s.camAddr + (uint64_t)rotOff, tar))
                                        g_writerCount.fetch_add(1, std::memory_order_relaxed);
                                    else {
                                        diagRate("writer-write", 500,
                                                 "writer write rotation failed cam=%p off=0x%X",
                                                 (void*)s.camAddr, rotOff);
                                        std::lock_guard<std::mutex> lk(g_snapMu);
                                        g_snapEpoch.fetch_add(1, std::memory_order_acq_rel);
                                        g_snap.ready = false;
                                        g_snap.epoch = g_snapEpoch.load(std::memory_order_relaxed);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        int r = g_writeRateHz;
        if (r < 30) r = 30;
        if (r > 1000) r = 1000;
        Sleep(1000 / r);
    }
    timeEndPeriod(1);
}

// ---- velext aim-model self-test (-aimtest): pure math, no game needed -------
static int runAimSelfTest() {
    const float RAD2DEG = 57.2957795f;
    const float DEG2RAD = 0.0174532925f;
    printf("=== ext_overlay velext smoothing self-test (no game) ===\n");
    printf("model: ts=max(1,smoothing); tau=ts*0.008s; alpha=1-exp(-dt/tau)\n");
    printf("       dt clamp [0,0.1] (fallback 0.004); gate >=4ms; smoothing<=1 => snap\n\n");

    printf("[1] alpha table at 60 Hz (dt=1/60 s)\n");
    static const float sms[] = { 0.05f, 0.2f, 0.5f, 1.0f, 2.0f, 5.0f, 20.0f };
    for (float sm : sms) {
        float ts = sm > 1.0f ? sm : 1.0f;
        float a = velextAlphaFromDt(sm, 1.0f / 60.0f);
        if (a >= 1.0f)
            printf("  smoothing=%6.2f ts=%5.2f tau=   --    alpha=%.6f (SNAP)\n",
                   sm, ts, a);
        else
            printf("  smoothing=%6.2f ts=%5.2f tau=%5.1fms alpha=%.6f\n",
                   sm, ts, ts * 8.0f, a);
    }
    printf("  smoothing=5.00 alpha: 60Hz=%.4f 144Hz=%.4f 240Hz=%.4f (same tau=40ms)\n",
           velextAlphaFromDt(5.0f, 1.0f / 60.0f),
           velextAlphaFromDt(5.0f, 1.0f / 144.0f),
           velextAlphaFromDt(5.0f, 1.0f / 240.0f));
    printf("\n");

    printf("[2] convergence: err0=30.00 deg, smoothing=5.00 (tau=40 ms), 0.5 s\n");
    static const int rates[] = { 60, 144, 240, 500 };
    float residDz[4] = { 0, 0, 0, 0 };
    float residNo[4] = { 0, 0, 0, 0 };
    for (int ri = 0; ri < 4; ++ri) {
        int rate = rates[ri];
        float dtTick = 1.0f / (float)rate;
        int nSteps = (int)(0.5f * (float)rate);
        // --- with deadzone (runtime behaviour) ---
        g_velLastActNs.store(0, std::memory_order_relaxed);
        uint64_t simNs = 1000000000ULL; // != 0: no "missing stamp" sentinel
        float err = 30.0f * DEG2RAD;
        int nWrite = 0, nGate = 0, nDead = 0;
        float firstDeadT = -1.0f;
        printf("  --- %d Hz (dt=%.2f ms, %d ticks) ---\n", rate, dtTick * 1000.0f, nSteps);
        for (int k = 1; k <= nSteps; ++k) {
            uint64_t lastNs = 0;
            float dtv = 0.0f, alpha = 0.0f;
            bool pass = velextActGate(simNs, &lastNs, &dtv, &alpha, 5.0f);
            float errBefore = err;
            const char* act;
            if (!pass) {
                act = "GATE(4ms)";
                ++nGate;
            } else if (cosf(err) > kVelextDotDeadzone) {
                act = "DEADZONE";
                ++nDead;
                if (firstDeadT < 0.0f) firstDeadT = (float)k * dtTick * 1000.0f;
            } else {
                // direction-vector nlerp, then normalize (same math as runtime)
                float lx = cosf(err), ly = sinf(err);
                float mx = lx + (1.0f - lx) * alpha;
                float my = ly + (0.0f - ly) * alpha;
                float ml = sqrtf(mx * mx + my * my);
                if (ml < 1e-6f) { mx = 1.0f; my = 0.0f; ml = 1.0f; }
                err = atan2f(my / ml, mx / ml);
                act = "WRITE";
                ++nWrite;
            }
            if (pass)
                printf("    t=%6.1fms dt=%5.2fms ts=5.00 tau=40.0ms alpha=%.5f "
                       "err %7.4f -> %7.4f deg  %s\n",
                       (float)k * dtTick * 1000.0f, dtv * 1000.0f, alpha,
                       errBefore * RAD2DEG, err * RAD2DEG, act);
            else
                printf("    t=%6.1fms dt=  --   ts=5.00 tau=40.0ms alpha=  --    "
                       "err %7.4f -> %7.4f deg  %s\n",
                       (float)k * dtTick * 1000.0f, errBefore * RAD2DEG,
                       err * RAD2DEG, act);
            simNs += (uint64_t)((double)dtTick * 1e9);
        }
        residDz[ri] = err * RAD2DEG;
        // --- same run without the deadzone: pure-filter residual at 0.5 s ---
        {
            g_velLastActNs.store(0, std::memory_order_relaxed);
            uint64_t s2 = 1000000000ULL;
            float e2 = 30.0f * DEG2RAD;
            for (int k = 1; k <= nSteps; ++k) {
                uint64_t l2 = 0;
                float d2 = 0.0f, a2 = 0.0f;
                velextActGate(s2, &l2, &d2, &a2, 5.0f);
                float lx = cosf(e2), ly = sinf(e2);
                float mx = lx + (1.0f - lx) * a2;
                float my = ly + (0.0f - ly) * a2;
                float ml = sqrtf(mx * mx + my * my);
                e2 = atan2f(my / ml, mx / ml);
                s2 += (uint64_t)((double)dtTick * 1e9);
            }
            residNo[ri] = e2 * RAD2DEG;
        }
        printf("  %d Hz summary: writes=%d deadzone=%d gate=%d | residual(dz on)=%.4f deg"
               " (first deadzone @%.0f ms) | residual(dz off)=%.5f deg\n",
               rate, nWrite, nDead, nGate, residDz[ri], firstDeadT, residNo[ri]);
    }
    {
        float mn = residNo[0], mx2 = residNo[0];
        for (int i = 1; i < 4; ++i) {
            if (residNo[i] < mn) mn = residNo[i];
            if (residNo[i] > mx2) mx2 = residNo[i];
        }
        printf("  theory (dz off): 30.00 * exp(-0.5/0.04) = %.5f deg\n",
               30.0f * expf(-0.5f / 0.04f));
        printf("  spread across 60/144/240/500 Hz (dz off): max-min = %.5f deg\n",
               mx2 - mn);
    }
    printf("\n");

    printf("[3] 4 ms gate (real clock, shared stamp)\n");
    g_velLastActNs.store(0, std::memory_order_relaxed);
    uint64_t lA = 0;
    float dA = 0.0f, aA = 0.0f;
    bool pA = velextActGate(qpcNs(), &lA, &dA, &aA, 5.0f);
    printf("  attempt1 -> %s (dt fallback=%.4f s, alpha=%.4f)\n",
           pA ? "PASS" : "BLOCKED", dA, aA);
    uint64_t stamp = g_velLastActNs.load(std::memory_order_relaxed);
    uint64_t tB = qpcNs();
    uint64_t lB = 0;
    float dB = 0.0f, aB = 0.0f;
    bool pB = velextActGate(tB, &lB, &dB, &aB, 5.0f);
    printf("  attempt2 +%.3f ms -> %s (needs >= 4.000 ms)\n",
           (double)(tB - stamp) / 1e6, pB ? "PASS" : "BLOCKED");
    Sleep(4);
    uint64_t tC = qpcNs();
    uint64_t lC = 0;
    float dC = 0.0f, aC = 0.0f;
    bool pC = velextActGate(tC, &lC, &dC, &aC, 5.0f);
    printf("  attempt3 +%.3f ms -> %s (dt=%.4f s, alpha=%.4f)\n",
           (double)(tC - stamp) / 1e6, pC ? "PASS" : "BLOCKED", dC, aC);

    printf("\n[4] deadzone dot > 0.99996 (rotation path)\n");
    float dIn = cosf(0.50f * DEG2RAD), dOut = cosf(0.60f * DEG2RAD);
    printf("  err=0.50 deg dot=%.6f -> %s\n", dIn,
           dIn > kVelextDotDeadzone ? "NO WRITE (deadzone)" : "WRITE");
    printf("  err=0.60 deg dot=%.6f -> %s\n", dOut,
           dOut > kVelextDotDeadzone ? "NO WRITE (deadzone)" : "WRITE");
    printf("  boundary = acos(0.99996) = %.4f deg\n",
           acosf(kVelextDotDeadzone) * RAD2DEG);

    printf("\n[5] Phase C mapping: angle -> counts (velext 0x1400DAB52)\n");
    printf("    want = hand-computed indep. double of the §5 chain;\n");
    printf("    cap = clamp(14000*dt, 12, 400) applied only when alpha < 1\n");
    struct MapCase {
        float eX, eY, focal, p, out1, out2, alpha, dt;
        float exX, exY;
        const char* note;
    };
    static const MapCase maps[] = {
        { 100, 100, 1000, 0.00f, 1.00f, 1.00f, 1.00f, 1.0f / 60.0f,
          11.4212f, 14.8327f, "alpha=1 snap: no clamp" },
        { 100, 100, 1000, 0.50f, 1.00f, 1.00f, 1.00f, 1.0f / 60.0f,
          13.1880f, 14.8327f, "pitch cos on X only" },
        { 100, 100, 1000, 0.00f, 1.00f, 1.00f, 0.32f, 1.0f / 60.0f,
          3.6548f, 4.7465f, "alpha=0.32 (prompt example)" },
        { 100, 100, 1000, 0.99f, 1.00f, 1.00f, 0.50f, 1.0f / 60.0f,
          38.0706f, 7.4164f, "cosp floored at 0.15" },
        { 200, 200, 1000, 0.00f, 1.00f, 1.00f, 0.90f, 0.0001f,
          12.0000f, 12.0000f, "lim floor 12 clamps both" },
        { 1e6f, -1e6f, 1000, 0.00f, 0.36f, 0.36f, 0.90f, 1.0f,
          400.0000f, -400.0000f, "lim ceil 400 clamps both" },
        { 1e6f, -1e6f, 1000, 0.00f, 0.36f, 0.36f, 1.00f, 1.0f,
          499.6817f, -648.9373f, "alpha=1: never clamped" },
    };
    int nMapFail = 0;
    for (const MapCase& c : maps) {
        float gx = 0.0f, gy = 0.0f;
        velextMouseCounts(c.eX, c.eY, c.focal, c.p, c.out1, c.out2,
                          c.alpha, c.dt, &gx, &gy);
        float tolX = fabsf(c.exX) * 1e-3f;
        if (tolX < 1e-3f) tolX = 1e-3f;
        float tolY = fabsf(c.exY) * 1e-3f;
        if (tolY < 1e-3f) tolY = 1e-3f;
        bool ok = fabsf(gx - c.exX) <= tolX && fabsf(gy - c.exY) <= tolY;
        if (!ok) ++nMapFail;
        printf("  eX=%9.0f eY=%9.0f focal=%5.0f p=%5.2f out=%4.2f,%4.2f a=%4.2f"
               " dt=%.4f -> (%9.4f,%9.4f) want (%9.4f,%9.4f) %s  %s\n",
               c.eX, c.eY, c.focal, c.p, c.out1, c.out2, c.alpha, c.dt,
               gx, gy, c.exX, c.exY, ok ? "PASS" : "FAIL", c.note);
    }
    bool vLo = velextSensValidIn(kVelextSensInLo) &&
               velextSensValidIn(kVelextSensInHi);
    bool vBad = !velextSensValidIn(0.0f) &&
                !velextSensValidIn(kVelextSensInLo * 0.1f) &&
                !velextSensValidIn(kVelextSensInHi + 1.0f) &&
                !velextSensValidIn(NAN) && !velextSensValidIn(INFINITY);
    if (!vLo || !vBad) ++nMapFail;
    printf("  sens input bounds [1e-3,100]: edges accepted=%d, rejected"
           " (0, lo/10, hi+1, NaN, inf)=%d\n",
           vLo ? 1 : 0, vBad ? 1 : 0);
    if (nMapFail) {
        printf("  [5] FAILED: %d check(s)\n", nMapFail);
        printf("\n=== done (FAIL) ===\n");
        return 1;
    }
    printf("  [5] all mapping cases PASS\n");
    printf("\n=== done ===\n");
    return 0;
}

static LRESULT WINAPI WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) {
        // Only the menu's controls accept clicks; everything else falls through
        // to whatever is below (the game).
        if (g_clickthru) return HTTRANSPARENT;
        POINT pt{ (short)LOWORD(l), (short)HIWORD(l) };
        for (const auto& hr : g_menuHits)
            if (pt.x >= hr.l && pt.x < hr.r && pt.y >= hr.t && pt.y < hr.b)
                return DefWindowProcW(h, m, w, l);
        return HTTRANSPARENT;
    }
    // Color-keyed pixels remain transparent to the game; drawn ImGui pixels
    // still need the backend to receive mouse input.
    if (ImGui::GetCurrentContext() &&
        ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    initExeDir(); // side files resolve next to the exe, never the CWD
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-aimtest")) return runAimSelfTest();
        // -srvtest <placeId>: fetch + parse + filter without the game/UI. Runs
        // before the singleton mutex so it works while the overlay is open.
        if (!strcmp(argv[i], "-srvtest")) {
            uint64_t pid = (i + 1 < argc) ? strtoull(argv[i + 1], nullptr, 10) : 0;
            return runSrvSelfTest(pid);
        }
    }
    int autoQuit = 0;
    HANDLE hOne = CreateMutexW(nullptr, FALSE, L"ext_overlay_singleton");
    if (!hOne || GetLastError() == ERROR_ALREADY_EXISTS) {
        printf("another overlay is already running - quitting\n");
        fflush(stdout);
        Sleep(3000);
        return 1;
    }
    int toggleVk = loadToggleKey();
    g_toggleVk = toggleVk; // the keybind editor can change it at runtime
    loadAimCfg();
    loadWaypoints();
    printf("[cfg] %s\n", exePath("overlay.cfg").c_str());
    fflush(stdout);
    bool nohide = false; // test mode: always visible, ignore fg/toggle
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) autoQuit = atoi(argv[++i]);
        if (!strcmp(argv[i], "-key") && i + 1 < argc) {
            int v = parseKey(argv[++i]);
            if (v) {
                toggleVk = v;
                g_toggleVk = v;
            }
        }
        if (!strcmp(argv[i], "-nohide")) nohide = true;
        if (!strcmp(argv[i], "-clickthru")) g_clickthru = true;
        if (!strcmp(argv[i], "-diag")) g_diagState = true; // print arm/key state
    }

    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe")) {
        printf("FAIL: cannot find game process\n");
        return 1;
    }
    // Prefer a write handle (mods + mem aim need it); fall back to read-only.
    bool haveWrite = m.openWrite();
    if (!haveWrite) {
        if (!m.open()) {
            printf("FAIL: cannot open game process (is Roblox running?)\n");
            printf("closing in 5s...\n");
            fflush(stdout);
            Sleep(5000);
            return 1;
        }
        if (cfgAimMode != 0) {
            cfgAimMode = 0;
            printf("NOTE: write handle denied -> aim mode mouse, mods off\n");
        }
    }
    std::string running = m.exeVersion();
    Offsets o;
    std::string log, cache = exePath("offsets_cache.json");
    if (!OffsetManager::ensure(running, cache, o, log)) {
        printf("%sFAIL (offsets)\n", log.c_str());
        printf("closing in 5s...\n");
        fflush(stdout);
        Sleep(5000);
        return 1;
    }
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) {
        printf("FAIL: module base\n");
        return 1;
    }

    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0,
                    GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr,
                    L"ext_ovl", nullptr };
    RegisterClassExW(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    // velext recipe, verified live on this machine (VelocityOverlay window):
    // TOPMOST|LAYERED|TOOLWINDOW|NOACTIVATE + LWA_COLORKEY(black).
    // Black pixels are invisible AND click-through; drawn pixels are visible
    // AND clickable. Fullscreen is safe again.
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"ext_ovl", L"ext", WS_POPUP, 0, 0, sw, sh,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        printf("FAIL: CreateWindowExW (error=%lu)\n", GetLastError());
        UnregisterClassW(L"ext_ovl", wc.hInstance);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    printf("overlay running - %s toggles hide/show, \\ quits (or close the game)\n",
           keyName(toggleVk).c_str());
    fflush(stdout);
    // Console steals focus on launch, leaving the game unfocused (dead mouse).
    // Minimize it so focus returns to the game. END still quits globally.
    ShowWindow(GetConsoleWindow(), SW_MINIMIZE);
    g_writeRateHz = cfgAimRate;
    diagOpen();
    diagRate("startup", 0, "startup mode=%d key=%d keymode=%d rate=%d smoothing=%.2f pid=%d",
             cfgAimMode, cfgAimKey, cfgAimKeyMode, cfgAimRate, cfgAimSmoothing, cfgAimPid);
    std::thread writerTh(writerThread, &m, o.cameraRotation);
    // Server tab worker: server-list / geo HTTP only. Owns no cfg and writes no
    // log except diag; the main thread drains its results (g_srvWire).
    std::thread srvTh(srvWorkerThread);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = sw;
    sd.BufferDesc.Height = sh;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL lvl;
    auto cleanupD3D = [&]() {
        if (g_rtv) {
            g_rtv->Release();
            g_rtv = nullptr;
        }
        if (g_swap) {
            g_swap->Release();
            g_swap = nullptr;
        }
        if (g_ctx) {
            g_ctx->Release();
            g_ctx = nullptr;
        }
        if (g_dev) {
            g_dev->Release();
            g_dev = nullptr;
        }
    };
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &lvl, &g_ctx))) {
        printf("FAIL: d3d11\n");
        cleanupD3D();
        DestroyWindow(hwnd);
        UnregisterClassW(L"ext_ovl", wc.hInstance);
        return 1;
    }
    ID3D11Texture2D* bb = nullptr;
    HRESULT hrBB = g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (FAILED(hrBB) || !bb) {
        printf("FAIL: GetBuffer (hr=0x%08X)\n", (unsigned int)hrBB);
        if (bb) bb->Release();
        cleanupD3D();
        DestroyWindow(hwnd);
        UnregisterClassW(L"ext_ovl", wc.hInstance);
        return 1;
    }
    HRESULT hrRTV = g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
    if (FAILED(hrRTV) || !g_rtv) {
        printf("FAIL: CreateRenderTargetView (hr=0x%08X)\n",
               (unsigned int)hrRTV);
        cleanupD3D();
        DestroyWindow(hwnd);
        UnregisterClassW(L"ext_ovl", wc.hInstance);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // No imgui.ini: window placement lives in overlay.cfg (menu.pos), otherwise
    // a stale ini would override the config on every launch.
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    uint64_t place = 0;
    Vec3 cam{ 0, 0, 0 };
    uint64_t camAddr = 0;
    uint64_t mouseSvc = 0;     // [I] Phase C: MouseService (f at +0x21C)
    DWORD mouseSvcRetry = 0;   // [I] resolve retry stamp (<= 1/s)
    int frame = 0;
    DWORD t0 = GetTickCount();
    DWORD lastLoopTick = t0;
    float loopMs = 0;
    bool done = false;
    bool visible = true;
    bool aimOn = false;
    bool diagPrevAimOn = false;
    uint64_t diagPrevCam = 0;
    uint64_t stickPlayer = 0; // sticky target by Player ID (survives respawn)
    double lastAimQpc = 0;
    DWORD lastActTick = 0;  // pacing: min gap between actuations
    Vec3 lockPos{};         // ghost-lock detection anchor
    DWORD lockTick = 0;     // 0 = uninit
    bool trigOn = false;
    bool acOn = false; // autoclicker toggle/hold state
    int acPhase = 0;   // 0 idle, 1 waiting interval, 2 pressing
    DWORD acT0 = 0, acNext = 0, acPressT0 = 0;
    int trigPhase = 0; // 0 idle, 1 arming (delay), 2 pressing
    DWORD trigT0 = 0, lastFireTick = 0;
    float accX = 0, accY = 0; // fractional carry (mouse mode)
    bool bpInFlight = false;  // velext mouse back-pressure: step awaiting camera move
    uint64_t bpStampNs = 0;   // ns stamp of the injected step
    float bpYawDeg = 0, bpPitchDeg = 0; // camera angles at the injected step
    bool bpCursorHold = false;  // [I] Phase C: cursor visible => not mouse-locked
    int bpStaleCount = 0;       // [I] timeout releases with no camera movement
    bool bpStaleHold = false;   // [I] back-pressure says "no lock": hold injects
    uint64_t bpProbeNs = 0;     // [I] next probe stamp while stale-hold is on
    float emaTargetX = 0, emaTargetY = 0; // EMA-smoothed target (v5)
    uint64_t emaPrim = 0;                 // target the EMA tracks
    bool emaInit = false;
    struct MouseCtlState {
        bool active = false;
        float abVX = 0.0f, abVY = 0.0f;
        float iX = 0.0f, iY = 0.0f;
        float idleT = 0.0f;
        bool settled = false;
        float lastEmaX = 0.0f, lastEmaY = 0.0f;
        bool lastEmaInit = false;
    };
    MouseCtlState mouseCtl;
    DWORD stickStartTick = 0;
    bool vpShifted = false;   // viewport-shift active (mode 2)
    bool shiftedNow = false;  // set when mode 2 writes this frame
    int calibPhase = 0; // 0 idle, 1 wait after inject
    double calibT0 = 0;
    float calibYaw0 = 0;
    int calibCounts = 200;
    const int CALIB_VK = 0x77; // F8
    int dbgEnts = 0;          // remote-diagnostic: entity count
    char tpMsg[96] = "";        // teleport status line (expires)
    DWORD tpMsgTick = 0;
    std::string toggleName = keyName(toggleVk);
    std::string aimName = keyName(cfgAimKey);
    // Game client viewport (refreshed periodically; window moves/resizes).
    int vpx = 0, vpy = 0, vpw = 0, vph = 0;
    {
        RECT gr{};
        if (gameClientRect(m.pid, gr)) {
            vpx = gr.left;
            vpy = gr.top;
            vpw = gr.right - gr.left;
            vph = gr.bottom - gr.top;
        }
    }

    // ---- 3-tier entity cache: steady-state refresh ~= 30 RPM, strings only
    // for strangers. A full string re-walk every 0.5s was the visible hitch.
    std::map<uint64_t, Ent> ecache; // key: Player object addr
    std::map<uint64_t, std::string> classCache; // key: any instance addr
    std::vector<Ent> ents;
    uint64_t cDm = 0, cPlayers = 0, lastPlaceSeen = 0;
    size_t npcScanSkip = 0;
    uint64_t uisAddr = 0; // UserInputService (chat-focus guard)
    auto classOf = [&](uint64_t obj) -> std::string {
        auto it = classCache.find(obj);
        if (it != classCache.end()) return it->second;
        std::string c = sdk.readClass(obj);
        if (!c.empty()) {
            classCache[obj] = c;
            if (classCache.size() > 20000) classCache.clear();
        }
        return c;
    };
    // Full resolve (strings) — only for strangers and respawns.
    auto resolveParts = [&](Ent& e, uint64_t model) {
        e.model = model;
        e.prim = 0;
        e.headPrim = 0;
        for (uint64_t& limb : e.limbPrim) limb = 0;
        e.humanoid = 0;
        sdk.walkChildren(model, [&](uint64_t h) {
            if (classOf(h) != "Humanoid") return true;
            e.humanoid = h;
            sdk.walkChildren(model, [&](uint64_t part) {
                if (sdk.readName(part) != "HumanoidRootPart") return true;
                uint64_t prim = 0;
                if (m.read(part + (uint64_t)o.basePartPrimitive, prim) &&
                    SDK::plausiblePtr(prim))
                    e.prim = prim;
                return false;
            });
            return false;
        });
        sdk.walkChildren(model, [&](uint64_t part) {
            std::string name = sdk.readName(part);
            uint64_t* dst = nullptr;
            if (name == "Head") {
                dst = &e.headPrim;
            } else if (name == "LeftHand" || name == "LeftUpperArm" ||
                       name == "Left Arm") {
                dst = &e.limbPrim[0];
            } else if (name == "RightHand" || name == "RightUpperArm" ||
                       name == "Right Arm") {
                dst = &e.limbPrim[1];
            } else if (name == "LeftFoot" || name == "LeftLowerLeg" ||
                       name == "Left Leg") {
                dst = &e.limbPrim[2];
            } else if (name == "RightFoot" || name == "RightLowerLeg" ||
                       name == "Right Leg") {
                dst = &e.limbPrim[3];
            }
            if (dst && !*dst)
            {
                uint64_t prim = 0;
                if (m.read(part + (uint64_t)o.basePartPrimitive, prim) &&
                    SDK::plausiblePtr(prim))
                    *dst = prim;
            }
            return true;
        });
    };
    auto clearResolvedParts = [](Ent& e) {
        e.model = 0;
        e.prim = 0;
        e.headPrim = 0;
        e.humanoid = 0;
        for (uint64_t& limb : e.limbPrim) limb = 0;
    };
    auto invalidateAimSnapshot = [&]() {
        std::lock_guard<std::mutex> lk(g_snapMu);
        g_snapEpoch.fetch_add(1, std::memory_order_acq_rel);
        g_snap.ready = false;
        g_snap.epoch = g_snapEpoch.load(std::memory_order_relaxed);
        g_snap.camAddr = 0;
    };
    auto refreshEnts = [&]() {
        uint64_t dm = 0, players = 0, local = 0;
        if (!sdk.dataModel(dm)) {
            ents.clear();
            invalidateAimSnapshot();
            return;
        }
        if (o.dataModelGameLoaded) {
            int loaded = 0;
            if (!m.read(dm + (uint64_t)o.dataModelGameLoaded, loaded) || !loaded) {
                stickPlayer = 0;
                ents.clear();
                invalidateAimSnapshot();
                return;
            }
        }
        uint64_t placeNow = 0;
        sdk.placeId(dm, placeNow);
        if (placeNow && placeNow != lastPlaceSeen) {
            lastPlaceSeen = placeNow; // map hop: instant full rebuild below
            loadHitboxProfile(placeNow);
            hitboxRestoreAll(m, o);
            worldRestoreAll(m, o); // Đợt 2: the old place's instances are gone
            cDm = 0;
            cPlayers = 0;
            uisAddr = 0;
            ecache.clear();
            classCache.clear();
            npcScanSkip = 0;
        }
        if (!cDm || cDm != dm) {
            cDm = dm;
            cPlayers = 0;
            uisAddr = 0;
            ecache.clear();
            classCache.clear();
            npcScanSkip = 0;
            worldRestoreAll(m, o); // Đợt 2: new DataModel => all-new instances
        }
        if (!cPlayers) {
            if (!sdk.findChildByClass(dm, "Players", players)) {
                ents.clear();
                invalidateAimSnapshot();
                return;
            }
            cPlayers = players;
        } else {
            players = cPlayers; // validate cheaply: parent must still be dm
            uint64_t par = 0;
            if (!o.instanceParent || !m.read(players + (uint64_t)o.instanceParent, par) ||
                par != dm) {
                cPlayers = 0;
                if (!sdk.findChildByClass(dm, "Players", players)) {
                    ents.clear();
                    invalidateAimSnapshot();
                    return;
                }
                cPlayers = players;
            }
        }
        if (!m.read(players + (uint64_t)o.playersLocalPlayer, local) ||
            !SDK::plausiblePtr(local)) {
            ents.clear();
            invalidateAimSnapshot();
            return;
        }
        if (!uisAddr && o.userInputWS)
            sdk.findChildByClass(dm, "UserInputService", uisAddr);
        std::map<uint64_t, bool> seen;
        sdk.walkChildren(players, [&](uint64_t p) {
            if (classOf(p) != "Player") return true;
            seen[p] = true;
            auto it = ecache.find(p);
            if (it == ecache.end()) {
                Ent e;
                e.self = (p == local);
                e.player = p;
                m.read(p + (uint64_t)o.playerTeam, e.team);
                e.tag = sdk.readTStringDirect(p + (uint64_t)o.playerDisplayName);
                if (e.tag.empty()) e.tag = sdk.readName(p);
                uint64_t model = 0;
                if (m.read(p + (uint64_t)o.playerModelInstance, model) &&
                    SDK::plausiblePtr(model))
                    resolveParts(e, model);
                if (e.prim) ecache[p] = e;
            } else {
                Ent& e = it->second;
                e.self = (p == local);
                m.read(p + (uint64_t)o.playerTeam, e.team);
                uint64_t model = 0;
                if (!m.read(p + (uint64_t)o.playerModelInstance, model) ||
                    !SDK::plausiblePtr(model)) {
                    clearResolvedParts(e);
                    return true;
                }
                bool staleParts = !e.prim;
                if (!staleParts && o.primitivePosition) {
                    Vec3 probe{};
                    staleParts =
                        !m.read(e.prim + (uint64_t)o.primitivePosition, probe) ||
                        !std::isfinite(probe.x) || !std::isfinite(probe.y) ||
                        !std::isfinite(probe.z);
                }
                if (model != e.model || staleParts)
                {
                    diagRate("entity-reresolve", 250,
                             "entity re-resolve player=%p oldModel=%p newModel=%p oldPrim=%p reason=%s",
                             (void*)e.player, (void*)e.model, (void*)model,
                             (void*)e.prim, model != e.model ? "model-change" : "prim-read");
                    resolveParts(e, model); // respawned or streamed in
                }
            }
            return true;
        });
        for (auto it = ecache.begin(); it != ecache.end();) {
            if (!seen.count(it->first) && !it->second.isNPC)
                it = ecache.erase(it);
            else
                ++it;
        }
        // ---- NPCs/mobs: Model instances under Workspace with a Humanoid ----
        // (excludes the local player's own character). Budgeted so a crowded
        // map can't stall the render thread; the scan cursor advances between
        // refreshes without caching incomplete streamed-in models as props.
        bool npcSweepComplete = false;
        if (cfgAimNPCs) {
            uint64_t ws = 0, selfModel = 0;
            if (o.dataModelWorkspace && m.read(dm + (uint64_t)o.dataModelWorkspace, ws) && ws) {
                for (auto& kv : ecache) {
                    if (kv.second.self) {
                        selfModel = kv.second.model;
                        break;
                    }
                }
                if (!selfModel && local) {
                    uint64_t lm = 0;
                    if (m.read(local + (uint64_t)o.playerModelInstance, lm)) selfModel = lm;
                }
                // Player character Models are ALSO Model+Humanoid under
                // Workspace -> skip ALL of them, not just self, or every
                // enemy gets a duplicate orange NPC box over the red/green one.
                std::set<uint64_t> playerModels;
                for (auto& kv : ecache)
                    if (!kv.second.isNPC && kv.second.model)
                        playerModels.insert(kv.second.model);
                if (selfModel) playerModels.insert(selfModel);
                for (auto it = ecache.begin(); it != ecache.end();) {
                    if (it->second.isNPC &&
                        (playerModels.count(it->first) ||
                         playerModels.count(it->second.model)))
                        it = ecache.erase(it);
                    else
                        ++it;
                }
                int budget = 12;
                size_t candidateIndex = 0;
                size_t nextScanSkip = npcScanSkip;
                bool stoppedByBudget = false;
                sdk.walkChildren(ws, [&](uint64_t ch) {
                    if (classOf(ch) != "Model" || ch == selfModel ||
                        playerModels.count(ch))
                        return true;
                    size_t currentIndex = candidateIndex++;
                    if (currentIndex < npcScanSkip) {
                        if (ecache.count(ch))
                            seen[ch] = true;
                        return true;
                    }
                    if (budget <= 0) {
                        stoppedByBudget = true;
                        return false;
                    }
                    nextScanSkip = currentIndex + 1;
                    if (ecache.count(ch)) {
                        seen[ch] = true;
                        return true;
                    }
                    --budget;
                    bool hasHum = false;
                    sdk.walkChildren(ch, [&](uint64_t h) {
                        if (classOf(h) == "Humanoid") {
                            hasHum = true;
                            return false;
                        }
                        return true;
                    });
                    if (!hasHum) return true;
                    Ent e;
                    e.isNPC = true;
                    e.player = 0;
                    e.self = false;
                    e.tag = sdk.readName(ch);
                    if (e.tag.empty()) return true;
                    resolveParts(e, ch);
                    if (e.prim) {
                        ecache[ch] = e;
                        seen[ch] = true;
                    }
                    return true;
                });
                if (stoppedByBudget) {
                    npcScanSkip = nextScanSkip;
                } else {
                    npcScanSkip = 0;
                    npcSweepComplete = true;
                }
            }
        } else {
            npcScanSkip = 0;
            npcSweepComplete = true;
        }
        for (auto it = ecache.begin(); it != ecache.end();) {
            if (!seen.count(it->first) &&
                (!it->second.isNPC || npcSweepComplete))
                it = ecache.erase(it);
            else
                ++it;
        }
        ents.clear();
        for (auto& kv : ecache)
            if (kv.second.prim) ents.push_back(kv.second);
        diagRate("entity-refresh", 1000,
                 "entity refresh dm=%p players=%p local=%p cache=%zu ents=%zu",
                 (void*)dm, (void*)players, (void*)local,
                 ecache.size(), ents.size());
    };
    auto w2s = [&](const Vec3& w, const float* mt, int& sx, int& sy) {
        // Project into the GAME client rect (not fullscreen metrics:
        // windowed game => different origin/scale => systematic offset).
        int vw = vpw ? vpw : sw, vh = vph ? vph : sh;
        int ox = vpw ? vpx : 0, oy = vph ? vpy : 0;
        float x = mt[0] * w.x + mt[1] * w.y + mt[2] * w.z + mt[3];
        float y = mt[4] * w.x + mt[5] * w.y + mt[6] * w.z + mt[7];
        float ww = mt[12] * w.x + mt[13] * w.y + mt[14] * w.z + mt[15];
        if (ww < 0.1f) return false;
        sx = ox + (int)((vw / 2) * (1.0f + x / ww));
        sy = oy + (int)((vh / 2) * (1.0f - y / ww));
        return true;
    };
    auto releaseTriggerIfDown = [&]() {
        if (trigPhase == 2) {
            INPUT up{};
            up.type = INPUT_MOUSE;
            up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(1, &up, sizeof(up));
        }
        trigPhase = 0;
    };
    auto restoreViewportIfNeeded = [&](bool force) {
        if (!(haveWrite && vpShifted && camAddr && o.cameraViewport &&
              o.visualEnginePtr && o.visualEngineDimensions &&
              (force || !shiftedNow)))
            return;

        uint64_t ve3 = 0;
        float rw = 0.0f, rh = 0.0f;
        if (m.read(sdk.base + o.visualEnginePtr, ve3) && ve3 &&
            m.read(ve3 + (uint64_t)o.visualEngineDimensions, rw) &&
            m.read(ve3 + (uint64_t)o.visualEngineDimensions + 4, rh) &&
            rw > 0.0f && rh > 0.0f && rw < 10000.0f && rh < 10000.0f) {
            VP2 rs{ (short)rw, (short)rh };
            if (m.write(camAddr + (uint64_t)o.cameraViewport, rs))
                vpShifted = false;
        }
    };
    MSG msg{};
    DWORD lastEntRefreshTick = 0;
    while (!done) {
        DWORD nowLoop = GetTickCount();
        float instMs = (float)(nowLoop - lastLoopTick);
        if (instMs < 0) instMs = 0;
        if (instMs > 500) instMs = 500;
        loopMs = loopMs * 0.9f + instMs * 0.1f; // EMA: user-visible hitch meter
        lastLoopTick = nowLoop;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        // Exunys typing check: never steal keys/aim while chatting.
        // CurrentTextBox != 0 means an in-game textbox has focus.
        bool typingNow = false;
        if (cfgChatGuard && uisAddr && o.userInputWS && o.windowTextBox) {
            uint64_t wsi = 0, box = 0;
            if (m.read(uisAddr + (uint64_t)o.userInputWS, wsi) && wsi &&
                m.read(wsi + (uint64_t)o.windowTextBox, box) && box)
                typingNow = true;
        }
        if (!typingNow && (GetAsyncKeyState(QUIT_VK) & 1)) done = true;
        if (!typingNow && (GetAsyncKeyState(g_toggleVk) & 1)) visible = !visible;
        // ---- keybind capture: the next key/button press becomes the hotkey ---
        // (the panel cannot take keyboard focus, so ImGui can't do this)
        if (g_capVk) {
            if (GetAsyncKeyState(VK_ESCAPE) & 1) {
                *g_capVk = 0;
                if (g_capCfg) upsertCfgKey(g_capCfg, "NONE");
                toast("key unbound");
                g_capVk = nullptr;
                g_capCfg = nullptr;
            } else {
                bool anyDown = false;
                for (int vk = 1; vk <= 0xFE; ++vk)
                    if (GetAsyncKeyState(vk) & 0x8000) anyDown = true;
                if (anyDown) {
                    // The click that opened capture (and anything else still held)
                    // must not become the new key: stay armed until every key is up.
                    g_capDrained = false;
                } else if (GetTickCount() - g_capT0 > 150) {
                    if (!g_capDrained) {
                        // All keys are up, but the stale "pressed since last call"
                        // bit of the opening click is still unread - clear it once,
                        // then start listening on the next frame.
                        for (int vk = 1; vk <= 0xFE; ++vk) GetAsyncKeyState(vk);
                        g_capDrained = true;
                    } else {
                        for (int vk = 1; vk <= 0xFE && g_capVk; ++vk) {
                            if (!(GetAsyncKeyState(vk) & 1)) continue;
                            const char* ck = g_capCfg ? g_capCfg : "?";
                            *g_capVk = vk;
                            if (g_capCfg) upsertCfgKey(g_capCfg, keyCfgName(vk).c_str());
                            toast("key = %s", keyName(vk).c_str());
                            if (g_diagState) {
                                printf("bind %s = 0x%02X (%s)\n", ck, vk,
                                       keyName(vk).c_str());
                                fflush(stdout);
                            }
                            g_capVk = nullptr;
                            g_capCfg = nullptr;
                            break;
                        }
                    }
                }
            }
        }
        // While capturing, every other hotkey stays asleep; aim/trigger/autoclick
        // all gate on typingNow, so this freezes them too.
        if (g_capVk) typingNow = true;
        // ---- panic: everything off + panel away, nothing written to the cfg
        // (so the next start is exactly what you had before the panic) --------
        if (cfgPanicKey && !typingNow && (GetAsyncKeyState(cfgPanicKey) & 1)) {
            cfgAimEnabled = false;
            cfgTrigEnabled = 0;
            cfgACEnabled = 0;
            cfgHitboxEnabled = 0;
            cfgPlySpeed = 0;
            cfgPlyJump = 0;
            cfgFlyEnabled = 0;
            cfgWorldFull = 0;
            cfgWorldFog = 0;
            cfgWorldShadows = 0;
            cfgWorldExpo = 0;
            cfgWorldClock = 0;
            cfgWorldFx = 0;
            cfgWorldAtmo = 0;
            cfgWorldWater = 0;
            cfgWorldGrass = 0;
            cfgWorldFov = 0;
            cfgWorldZoom = 0;
            cfgSrvAntiAfk = 0;
            cfgSrvAutoRejoin = 0;
            cfgSrvGeo = 0;
            if (haveWrite) worldRestoreAll(m, o); // lighting/terrain back to normal
            g_flyActive = 0;
            g_speedActive = 0;
            g_jumpActive = 0;
            aimOn = false;
            trigOn = false;
            acOn = false;
            cfgMenuCollapsed = 1; // panel out of the way (title bar only)
            toast("PANIC: all features off");
        }
        if (cfgAimKeyMode == 0) {
            bool held = !typingNow && (GetAsyncKeyState(cfgAimKey) & 0x8000) != 0;
            if (!held) stickPlayer = 0; // release drops sticky lock
            aimOn = held;
        } else {
            if (!typingNow && (GetAsyncKeyState(cfgAimKey) & 1)) aimOn = !aimOn;
        }
        if (aimOn != diagPrevAimOn) {
            diagRate("aim-state", 0, "aim state=%s typing=%d fgPending=1",
                     aimOn ? "ON" : "OFF", typingNow ? 1 : 0);
            diagPrevAimOn = aimOn;
        }
        if (!aimOn) {
            stickPlayer = 0;
            stickStartTick = 0;
            lockTick = 0;
            lockPos = Vec3{};
            lastAimQpc = 0;
            lastActTick = 0;
            accX = 0.0f;
            accY = 0.0f;
            emaTargetX = 0.0f;
            emaTargetY = 0.0f;
            emaPrim = 0;
            emaInit = false;
            mouseCtl = MouseCtlState{};
            invalidateAimSnapshot();
        }
        if (cfgTrigKeyMode == 0) {
            trigOn = !typingNow && (GetAsyncKeyState(cfgTrigKey) & 0x8000) != 0;
        } else {
            if (!typingNow && (GetAsyncKeyState(cfgTrigKey) & 1)) trigOn = !trigOn;
        }
        if (!trigOn) releaseTriggerIfDown();
        if (cfgACKeyMode == 0) {
            bool heldAC = !typingNow && (GetAsyncKeyState(cfgACKey) & 0x8000) != 0;
            if (heldAC && !acOn) {
                acT0 = GetTickCount();
                acNext = acT0;
            }
            acOn = heldAC;
        } else {
            if (!typingNow && (GetAsyncKeyState(cfgACKey) & 1)) {
                acOn = !acOn;
                if (acOn) {
                    acT0 = GetTickCount();
                    acNext = acT0;
                }
            }
        }
        // ---- Player-mod hotkeys: tick in the menu ARMS the feature, the key
        // ACTIVATES it. Not armed => the key is completely ignored (and the
        // feature is forced off), which is exactly the "no accidental F" rule.
        {
            // own rising-edge detection (see g_prevSpeedKey comment)
            bool dSpeed = cfgPlySpeedKey &&
                          (GetAsyncKeyState(cfgPlySpeedKey) & 0x8000) != 0;
            if (dSpeed && !g_prevSpeedKey && !typingNow && cfgPlySpeed)
                g_speedActive = !g_speedActive;
            g_prevSpeedKey = dSpeed;

            bool dJump = cfgPlyJumpKey &&
                         (GetAsyncKeyState(cfgPlyJumpKey) & 0x8000) != 0;
            if (dJump && !g_prevJumpKey && !typingNow && cfgPlyJump)
                g_jumpActive = !g_jumpActive;
            g_prevJumpKey = dJump;

            bool dFly = cfgFlyKey && (GetAsyncKeyState(cfgFlyKey) & 0x8000) != 0;
            if (dFly && !g_prevFlyKey && !typingNow && cfgFlyEnabled)
                g_flyActive = !g_flyActive;
            g_prevFlyKey = dFly;
        }
        if (!typingNow && cfgPlySdnKey && (GetAsyncKeyState(cfgPlySdnKey) & 1)) {
            cfgPlySpeedVal -= 2.0f;
            if (cfgPlySpeedVal < 0) cfgPlySpeedVal = 0;
            char b[32]; snprintf(b, sizeof b, "%.0f", cfgPlySpeedVal);
            upsertCfgKey("ply.speedval", b);
        }
        if (!typingNow && cfgPlySupKey && (GetAsyncKeyState(cfgPlySupKey) & 1)) {
            cfgPlySpeedVal += 2.0f;
            if (cfgPlySpeedVal > 500) cfgPlySpeedVal = 500;
            char b[32]; snprintf(b, sizeof b, "%.0f", cfgPlySpeedVal);
            upsertCfgKey("ply.speedval", b);
        }
        if (!typingNow && cfgPlyJdnKey && (GetAsyncKeyState(cfgPlyJdnKey) & 1)) {
            cfgPlyJumpVal -= 5.0f;
            if (cfgPlyJumpVal < 0) cfgPlyJumpVal = 0;
            char b[32]; snprintf(b, sizeof b, "%.0f", cfgPlyJumpVal);
            upsertCfgKey("ply.jumpval", b);
        }
        if (!typingNow && cfgPlyJupKey && (GetAsyncKeyState(cfgPlyJupKey) & 1)) {
            cfgPlyJumpVal += 5.0f;
            if (cfgPlyJumpVal > 500) cfgPlyJumpVal = 500;
            char b[32]; snprintf(b, sizeof b, "%.0f", cfgPlyJumpVal);
            upsertCfgKey("ply.jumpval", b);
        }
        if (autoQuit > 0 && (int)(GetTickCount() - t0) / 1000 >= autoQuit) done = true;
        if (done) {
            if (haveWrite) flyAnchorRestore(m, o);
            if (haveWrite) worldRestoreAll(m, o); // Đợt 2: quit = nothing left on
            mouseClipRestore();
            break;
        }

        // Game closed -> quit with it (no orphan overlay, no key needed).
        if (WaitForSingleObject(m.h, 0) == WAIT_OBJECT_0) {
            releaseTriggerIfDown();
            restoreViewportIfNeeded(true);
            mouseClipRestore();
            if (haveWrite) flyAnchorRestore(m, o);
            worldRestoreAll(m, o); // Đợt 2: best-effort, the process is going away
            break;
        }

        // Keep gameplay state current even while the HUD is hidden or the
        // game is not foreground, so player mods never use an old entity.
        if (lastEntRefreshTick == 0 ||
            nowLoop - lastEntRefreshTick >= 250) {
            refreshEnts();
            lastEntRefreshTick = nowLoop;
        }

        // Hidden by toggle, or game not foreground: HIDE the window entirely.
        // A hidden window cannot hit-test, period (stronger than transparency).
        DWORD fgPid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
                { static DWORD lastHbTickBg = 0; // hitbox runs even while HUD hidden
          if (nowLoop - lastHbTickBg >= 500) {
              lastHbTickBg = nowLoop;
              hitboxTick(m, sdk, o, haveWrite, nowLoop);
          } }
        // ---- World (Đợt 2): lighting / atmosphere / terrain / FOV. Same
        // placement as the hitbox tick: it runs even while the HUD is hidden,
        // so a ticked feature stays live until it is unticked.
        worldTick(m, sdk, o, haveWrite, nowLoop, camAddr);
        // ---- Server (Đợt 4): info / ping / watchdog / anti-AFK. Same placement
        // as worldTick: it keeps running while the panel is hidden, so the
        // auto-rejoin watchdog and anti-AFK never depend on the UI being open.
        // The roster snapshot is rebuilt at 1 Hz (srvTick samples at 1 Hz too).
        {
            static DWORD srvRosterTick = 0;
            static std::vector<std::string> srvNames;
            static int srvEntCount = 0;
            if (nowLoop - srvRosterTick >= 1000) {
                srvRosterTick = nowLoop;
                srvNames.clear();
                for (const Ent& e : ents)
                    if (!e.self && !e.isNPC) srvNames.push_back(e.tag);
                srvEntCount = (int)ents.size();
            }
            srvTick(m, sdk, o, nowLoop, fgPid == m.pid, typingNow, aimOn,
                    srvNames, srvEntCount);
        }
// ---- Player mods: persistent writes, run even while HUD hidden ----
        // Effective state = menu checkbox (arm) AND hotkey (activate). Everything
        // below drives off these three instead of the raw config flags, so an
        // unticked feature is off *and* its key does nothing.
        const bool speedApply = cfgPlySpeed && g_speedActive;
        const bool jumpApply = cfgPlyJump && g_jumpActive;
        const bool flyApply = cfgFlyEnabled && g_flyActive;
        if (g_diagState) {
            static DWORD lastSt = 0;
            static int lastF = -1, lastS = -1, lastJ = -1;
            DWORD nowSt = GetTickCount();
            bool changed = (g_flyActive != lastF || g_speedActive != lastS ||
                            g_jumpActive != lastJ);
            if (changed || nowSt - lastSt >= 2000) {
                lastSt = nowSt;
                lastF = g_flyActive;
                lastS = g_speedActive;
                lastJ = g_jumpActive;
                printf("state fly[arm=%d key=%s active=%d apply=%d] "
                       "speed[arm=%d key=%s active=%d apply=%d] "
                       "jump[arm=%d key=%s active=%d apply=%d]%s\n",
                       cfgFlyEnabled, keyName(cfgFlyKey).c_str(), g_flyActive,
                       (int)flyApply, cfgPlySpeed, keyName(cfgPlySpeedKey).c_str(),
                       g_speedActive, (int)speedApply, cfgPlyJump,
                       keyName(cfgPlyJumpKey).c_str(), g_jumpActive,
                       (int)jumpApply, changed ? "  <-- CHANGE" : "");
                fflush(stdout);
            }
        }
        // Reference-tool behaviour: when a toggle goes off (or the property path
        // is left), write back the values the Humanoid had before we touched it.
        if (haveWrite && g_plyRestHum) {
            bool jpTouched = (cfgPlyMode == 0 && jumpApply && cfgPlyJumpMode != 2);
            if (g_plyJPValid && !jpTouched) {
                if (o.humanoidJumpPower)
                    m.write(g_plyRestHum + (uint64_t)o.humanoidJumpPower, g_plyRestJP);
                g_plyJPValid = false;
            }
            if (g_plyJHValid &&
                !(cfgPlyMode == 0 && jumpApply && cfgPlyJumpMode == 0)) {
                if (o.humanoidJumpHeight)
                    m.write(g_plyRestHum + (uint64_t)o.humanoidJumpHeight, g_plyRestJH);
                g_plyJHValid = false;
            }
            if (g_plyWSValid && !(cfgPlyMode == 0 && speedApply)) {
                if (o.humanoidWalkSpeedShadow)
                    m.write(g_plyRestHum + (uint64_t)o.humanoidWalkSpeedShadow,
                            g_plyRestWSShadow);
                if (o.humanoidWalkSpeed)
                    m.write(g_plyRestHum + (uint64_t)o.humanoidWalkSpeed, g_plyRestWS);
                if (o.humanoidWalkSpeedShadow)
                    m.write(g_plyRestHum + (uint64_t)o.humanoidWalkSpeedShadow,
                            g_plyRestWSShadow);
                g_plyWSValid = false;
            }
            if (!g_plyWSValid && !g_plyJPValid && !g_plyJHValid)
                g_plyRestHum = 0;
        }
        // mode 0 = Humanoid WalkSpeed/JumpPower property. WalkSpeed is paired with
        // a shadow copy on the Humanoid (Humanoid.WalkSpeedShadow; theo:
        // Humanoid.WalkspeedCheck, 0x3A4): writing WalkSpeed alone leaves the pair
        // inconsistent and Hyperion kicks with Error 268. The value is bracketed
        // shadow -> WalkSpeed -> shadow with identical bytes, every tick, exactly
        // like the reference external.
        if (cfgPlyMode == 0 && (speedApply || jumpApply) && haveWrite) {
            for (auto& e : ents) {
                if (!e.self || !e.player || !e.model || !e.humanoid ||
                    !o.playerModelInstance || !o.humanoidHealth ||
                    !SDK::plausiblePtr(e.humanoid))
                    continue;
                uint64_t currentModel = 0;
                if (!m.read(e.player + (uint64_t)o.playerModelInstance,
                            currentModel) ||
                    currentModel != e.model)
                    continue;
                float hpProbe = 0.0f;
                if (!m.read(e.humanoid + (uint64_t)o.humanoidHealth,
                            hpProbe) ||
                    !std::isfinite(hpProbe) || hpProbe < 0.0f ||
                    hpProbe > 1000000.0f)
                    continue;
                // respawn / model swap => the saved originals belong to a dead
                // Humanoid, forget them (nothing to restore).
                if (e.humanoid != g_plyRestHum) {
                    g_plyRestHum = e.humanoid;
                    g_plyWSValid = false;
                    g_plyJPValid = false;
                }
                if (speedApply && o.humanoidWalkSpeed) {
                    float cur = 0.0f;
                    if (m.read(e.humanoid + (uint64_t)o.humanoidWalkSpeed, cur) &&
                        std::isfinite(cur)) {
                        g_plyCurSpeed = cur;
                        if (!g_plyWSValid) { // remember the originals on enable
                            g_plyRestWS = cur;
                            g_plyRestWSShadow = cur;
                            if (o.humanoidWalkSpeedShadow)
                                m.read(e.humanoid + (uint64_t)o.humanoidWalkSpeedShadow,
                                       g_plyRestWSShadow);
                            g_plyWSValid = true;
                        }
                        { // every tick while enabled, like the reference external
                            if (o.humanoidWalkSpeedShadow)
                                m.write(e.humanoid + (uint64_t)o.humanoidWalkSpeedShadow,
                                        cfgPlySpeedVal);
                            m.write(e.humanoid + (uint64_t)o.humanoidWalkSpeed,
                                    cfgPlySpeedVal);
                            if (o.humanoidWalkSpeedShadow)
                                m.write(e.humanoid + (uint64_t)o.humanoidWalkSpeedShadow,
                                        cfgPlySpeedVal);
                        }
                    }
                }
                if (jumpApply && cfgPlyJumpMode != 2) {
                    // Roblox derives the take-off velocity from JumpPower when
                    // UseJumpPower is true, otherwise from JumpHeight:
                    //     v0 = JumpPower
                    //     v0 = sqrt(2 * Gravity * JumpHeight)
                    // One slider (ply.jumpval, same meaning as the reference
                    // external's JumpPower) is converted to the equivalent
                    // JumpHeight so both game flavours jump the same.
                    // JumpHeight has no shadow copy -> nothing else to keep in
                    // sync (checked by scanning the whole Humanoid).
                    float jp = 0.0f, jh = 0.0f;
                    if (o.humanoidJumpPower &&
                        m.read(e.humanoid + (uint64_t)o.humanoidJumpPower, jp) &&
                        std::isfinite(jp)) {
                        if (!g_plyJPValid) {
                            g_plyRestJP = jp;
                            g_plyJPValid = true;
                        }
                    }
                    if (o.humanoidJumpHeight &&
                        m.read(e.humanoid + (uint64_t)o.humanoidJumpHeight, jh) &&
                        std::isfinite(jh)) {
                        if (!g_plyJHValid) {
                            g_plyRestJH = jh;
                            g_plyJHValid = true;
                        }
                    }
                    float wantH = (cfgPlyJumpVal * cfgPlyJumpVal) / (2.0f * 196.2f);
                    if (cfgPlyJumpMode == 1) { // exactly like the reference tool
                        if (o.humanoidJumpPower)
                            m.write(e.humanoid + (uint64_t)o.humanoidJumpPower,
                                    cfgPlyJumpVal);
                    } else { // auto: drive both, whichever the game reads
                        if (o.humanoidJumpPower)
                            m.write(e.humanoid + (uint64_t)o.humanoidJumpPower,
                                    cfgPlyJumpVal);
                        if (o.humanoidJumpHeight)
                            m.write(e.humanoid + (uint64_t)o.humanoidJumpHeight, wantH);
                    }
                }
                // jumpmode 2: pure velocity impulse on Space (no property write,
                // so no check can ever see a changed jump field). Also works
                // mid-air -> free infinite jump.
                if (jumpApply && cfgPlyJumpMode == 2 && e.prim &&
                    o.primitiveVelocity && !typingNow &&
                    (GetAsyncKeyState(VK_SPACE) & 1)) {
                    Vec3 v{};
                    if (m.read(e.prim + (uint64_t)o.primitiveVelocity, v) &&
                        std::isfinite(v.y)) {
                        v.y = cfgPlyJumpVal;
                        m.write(e.prim + (uint64_t)o.primitiveVelocity, v);
                    }
                }
                break;
            }
        }
        // ---- Velocity speed (mode 1): WalkSpeed property stays legit, so
        // property checks see nothing. Horizontal velocity is driven toward
        // the WASD wish direction every tick; Y is untouched (jump/gravity
        // intact). No input -> touch nothing, natural friction applies.
        if (cfgPlyMode == 1 && speedApply && haveWrite && !typingNow &&
            fgPid == m.pid && !flyApply && o.primitivePosition &&
            o.primitiveVelocity) {
            uint64_t selfPrim = 0;
            for (auto& e : ents) {
                if (e.self && e.prim) {
                    selfPrim = e.prim;
                    break;
                }
            }
            if (selfPrim) {
                float fwdX = 0.0f, fwdZ = 1.0f, rgtX = 1.0f, rgtZ = 0.0f;
                if (camAddr && o.cameraRotation) {
                    float r[9]{};
                    if (m.read(camAddr + (uint64_t)o.cameraRotation, r)) {
                        float lx = -r[2], lz = -r[8];
                        float ll = sqrtf(lx * lx + lz * lz);
                        if (ll > 1e-4f) {
                            fwdX = lx / ll;
                            fwdZ = lz / ll;
                        }
                        float rx = r[0], rz = r[6];
                        float rl = sqrtf(rx * rx + rz * rz);
                        if (rl > 1e-4f) {
                            rgtX = rx / rl;
                            rgtZ = rz / rl;
                        }
                    }
                }
                float mx = 0.0f, mz = 0.0f;
                if (GetAsyncKeyState('W') & 0x8000) { mx += fwdX; mz += fwdZ; }
                if (GetAsyncKeyState('S') & 0x8000) { mx -= fwdX; mz -= fwdZ; }
                if (GetAsyncKeyState('D') & 0x8000) { mx += rgtX; mz += rgtZ; }
                if (GetAsyncKeyState('A') & 0x8000) { mx -= rgtX; mz -= rgtZ; }
                float ml = sqrtf(mx * mx + mz * mz);
                if (ml > 1e-6f) {
                    Vec3 v{};
                    if (m.read(selfPrim + (uint64_t)o.primitiveVelocity, v) &&
                        std::isfinite(v.x) && std::isfinite(v.y) &&
                        std::isfinite(v.z)) {
                        Vec3 nv{ mx / ml * cfgPlySpeedVal, v.y,
                                 mz / ml * cfgPlySpeedVal };
                        m.write(selfPrim + (uint64_t)o.primitiveVelocity, nv);
                    }
                }
                // Velocity jump: Space edge sets takeoff velocity directly
                // (~1:1 with JumpPower). Works mid-air = doubles as inf-jump.
                if (jumpApply && (GetAsyncKeyState(VK_SPACE) & 1)) {
                    Vec3 v{};
                    if (m.read(selfPrim + (uint64_t)o.primitiveVelocity, v) &&
                        std::isfinite(v.y)) {
                        v.y = cfgPlyJumpVal;
                        m.write(selfPrim + (uint64_t)o.primitiveVelocity, v);
                    }
                }
            }
        }
        // ---- Teleport (driven by the Teleport tab; the old H-to-nearest hotkey
        // is gone). The button stores the target HRP + tick; the write happens
        // here so the menu only ever sets a request.
        if (g_tpTargetPrim && haveWrite && !typingNow && o.primitivePosition &&
            nowLoop - g_tpRequestTick < 2000) {
            uint64_t want = g_tpTargetPrim;
            g_tpTargetPrim = 0;
            uint64_t selfPrim = 0;
            Vec3 selfPos{};
            bool selfOk = false;
            for (auto& e : ents) {
                if (!e.self || !e.prim) continue;
                uint64_t cur = 0;
                if (e.player && o.playerModelInstance &&
                    (!m.read(e.player + (uint64_t)o.playerModelInstance, cur) ||
                     cur != e.model))
                    continue;
                if (m.read(e.prim + (uint64_t)o.primitivePosition, selfPos) &&
                    std::isfinite(selfPos.x) && std::isfinite(selfPos.y) &&
                    std::isfinite(selfPos.z)) {
                    selfPrim = e.prim;
                    selfOk = true;
                }
                break;
            }
            int best = -1;
            for (size_t i = 0; i < ents.size(); ++i)
                if (ents[i].prim == want) {
                    best = (int)i;
                    break;
                }
            Vec3 dst{};
            if (!selfOk) {
                snprintf(tpMsg, sizeof tpMsg, "tp: self not found");
                tpMsgTick = nowLoop;
            } else if (best < 0) {
                snprintf(tpMsg, sizeof tpMsg, "tp: target is gone");
                tpMsgTick = nowLoop;
            } else if (!m.read(ents[best].prim + (uint64_t)o.primitivePosition,
                               dst) ||
                       !std::isfinite(dst.x) || !std::isfinite(dst.y) ||
                       !std::isfinite(dst.z)) {
                snprintf(tpMsg, sizeof tpMsg, "tp: read failed");
                tpMsgTick = nowLoop;
            } else {
                float dx = dst.x - selfPos.x, dy = dst.y - selfPos.y,
                      dz = dst.z - selfPos.z;
                float d = sqrtf(dx * dx + dy * dy + dz * dz);
                dst.y += 3.0f; // land above their head, avoid overlap
                Vec3 zv{ 0, 0, 0 };
                if (o.primitiveVelocity)
                    m.write(selfPrim + (uint64_t)o.primitiveVelocity, zv);
                if (m.write(selfPrim + (uint64_t)o.primitivePosition, dst)) {
                    if (o.primitiveVelocity)
                        m.write(selfPrim + (uint64_t)o.primitiveVelocity, zv);
                    snprintf(tpMsg, sizeof tpMsg, "tp -> %s (%.0fm)",
                             ents[best].tag.c_str(), d);
                    tpMsgTick = nowLoop;
                    diagRate("teleport", 0, "teleport to %s dist=%.1f",
                             ents[best].tag.c_str(), d);
                } else {
                    snprintf(tpMsg, sizeof tpMsg, "tp: write failed");
                    tpMsgTick = nowLoop;
                }
            }
        }
        // ---- Teleport to a saved waypoint (Teleport tab, "wp: TP") ----
        if (g_tpPosTick && haveWrite && !typingNow && o.primitivePosition &&
            nowLoop - g_tpPosTick < 2000) {
            g_tpPosTick = 0;
            Vec3 dst = g_tpTargetPos;
            uint64_t selfPrim = 0;
            Vec3 selfPos{};
            for (auto& e : ents) {
                if (!e.self || !e.prim) continue;
                if (m.read(e.prim + (uint64_t)o.primitivePosition, selfPos) &&
                    std::isfinite(selfPos.x) && std::isfinite(selfPos.y) &&
                    std::isfinite(selfPos.z))
                    selfPrim = e.prim;
                break;
            }
            if (!selfPrim) {
                snprintf(tpMsg, sizeof tpMsg, "wp: self not found");
                tpMsgTick = nowLoop;
            } else {
                float dx = dst.x - selfPos.x, dy = dst.y - selfPos.y,
                      dz = dst.z - selfPos.z;
                float d = sqrtf(dx * dx + dy * dy + dz * dz);
                Vec3 zv{ 0, 0, 0 };
                if (o.primitiveVelocity)
                    m.write(selfPrim + (uint64_t)o.primitiveVelocity, zv);
                if (m.write(selfPrim + (uint64_t)o.primitivePosition, dst)) {
                    if (o.primitiveVelocity)
                        m.write(selfPrim + (uint64_t)o.primitiveVelocity, zv);
                    snprintf(tpMsg, sizeof tpMsg, "wp %s (%.0fm)", g_tpPosName, d);
                    tpMsgTick = nowLoop;
                    diagRate("wptp", 0, "waypoint tp %s dist=%.1f", g_tpPosName, d);
                } else {
                    snprintf(tpMsg, sizeof tpMsg, "wp: write failed");
                    tpMsgTick = nowLoop;
                }
            }
        }
        // ---- Fly off: un-anchor once (also covers respawn churn) ----
        if (!flyApply && flyAnchorPrim) {
            if (haveWrite) flyAnchorRestore(m, o);
            else flyAnchorPrim = 0;
        }
        // ---- Fly: anchored HRP + WASD/Space/C-Ctrl, camera-relative ----
        // Without Anchored the physics sim re-integrates gravity every step
        // and drags you down between our position writes.
        if (flyApply && haveWrite && !typingNow && fgPid == m.pid &&
            o.primitivePosition) {
            uint64_t selfPrim = 0;
            Vec3 selfPos{};
            for (auto& e : ents) {
                if (!e.self || !e.prim) continue;
                if (m.read(e.prim + (uint64_t)o.primitivePosition, selfPos) &&
                    std::isfinite(selfPos.x) && std::isfinite(selfPos.y) &&
                    std::isfinite(selfPos.z))
                    selfPrim = e.prim;
                break;
            }
            if (selfPrim && o.primitiveFlags) {
                if (flyAnchorPrim != selfPrim) flyAnchorPrim = 0; // respawned
                uint32_t fl = 0;
                if (m.read(selfPrim + (uint64_t)o.primitiveFlags, fl)) {
                    if (!flyAnchorPrim) {
                        flyAnchorPrim = selfPrim;
                        flyAnchorSaved = fl;
                    }
                    uint32_t want = fl | (uint32_t)o.primitiveAnchoredBit;
                    if (want != fl)
                        m.write(selfPrim + (uint64_t)o.primitiveFlags, want);
                }
            }
            if (selfPrim) {
                float fwdX = 0.0f, fwdZ = 1.0f, rgtX = 1.0f, rgtZ = 0.0f;
                if (camAddr && o.cameraRotation) {
                    float r[9]{};
                    if (m.read(camAddr + (uint64_t)o.cameraRotation, r)) {
                        float lx = -r[2], lz = -r[8];
                        float ll = sqrtf(lx * lx + lz * lz);
                        if (ll > 1e-4f) {
                            fwdX = lx / ll;
                            fwdZ = lz / ll;
                        }
                        float rx = r[0], rz = r[6];
                        float rl = sqrtf(rx * rx + rz * rz);
                        if (rl > 1e-4f) {
                            rgtX = rx / rl;
                            rgtZ = rz / rl;
                        }
                    }
                }
                float mx = 0.0f, mz = 0.0f, my = 0.0f;
                if (GetAsyncKeyState('W') & 0x8000) { mx += fwdX; mz += fwdZ; }
                if (GetAsyncKeyState('S') & 0x8000) { mx -= fwdX; mz -= fwdZ; }
                if (GetAsyncKeyState('D') & 0x8000) { mx += rgtX; mz += rgtZ; }
                if (GetAsyncKeyState('A') & 0x8000) { mx -= rgtX; mz -= rgtZ; }
                if (GetAsyncKeyState(VK_SPACE) & 0x8000) my += 1.0f;
                if ((GetAsyncKeyState('C') & 0x8000) ||
                    (GetAsyncKeyState(VK_CONTROL) & 0x8000))
                    my -= 1.0f;
                float ml = sqrtf(mx * mx + mz * mz);
                if (ml > 1e-6f) {
                    mx /= ml;
                    mz /= ml;
                }
                float dt = instMs / 1000.0f;
                if (dt < 0) dt = 0;
                if (dt > 0.05f) dt = 0.05f;
                Vec3 np{ selfPos.x + mx * cfgFlySpeed * dt,
                         selfPos.y + my * cfgFlySpeed * dt,
                         selfPos.z + mz * cfgFlySpeed * dt };
                Vec3 zv{ 0, 0, 0 };
                if (o.primitiveVelocity)
                    m.write(selfPrim + (uint64_t)o.primitiveVelocity, zv);
                m.write(selfPrim + (uint64_t)o.primitivePosition, np);
            }
        }
        bool wantShow = nohide ? true : (visible && (fgPid == m.pid));
        // ---- Mouse-lock handling for the panel --------------------------------
        // While the panel is EXPANDED the cursor must be free to reach it; the
        // game's own confinement box comes back when it is collapsed.
        auto mouseLockUpdate = [&](bool panelUp) {
            if (!cfgMenuUnlockMouse) return;
            if (panelUp)
                mouseClipRelease(sw, sh);
            else
                mouseClipRestore();
        };
        bool isShown = !!IsWindowVisible(hwnd);
        if (wantShow && !isShown)
            ShowWindow(hwnd, SW_SHOWNA); // show without activating
        else if (!wantShow && isShown)
            ShowWindow(hwnd, SW_HIDE);
        if (!wantShow) {
            mouseLockUpdate(false); // give the game its cursor lock back
            g_menuHits.clear(); // hidden overlay takes no clicks at all
            releaseTriggerIfDown();
            restoreViewportIfNeeded(true);
            invalidateAimSnapshot();
            Sleep(50);
            continue;
        }

        // Refresh the camera chain every loop. Camera instances can be
        // replaced during respawn/map transitions; retaining the old address
        // lets the writer continue targeting stale memory.
        {
            uint64_t dm = 0, ws = 0, ch = 0;
            bool cameraValid = false;
            if (sdk.dataModel(dm)) {
                sdk.placeId(dm, place);
                if (sdk.workspace(dm, ws) && sdk.camera(ws, ch)) {
                    Vec3 nextCam{};
                    if (SDK::plausiblePtr(ch) && sdk.cameraPos(ch, nextCam) &&
                        std::isfinite(nextCam.x) && std::isfinite(nextCam.y) &&
                        std::isfinite(nextCam.z)) {
                        camAddr = ch;
                        cam = nextCam;
                        g_currentCam.store(ch, std::memory_order_release);
                        if (diagPrevCam != ch) {
                            diagRate("camera-change", 0,
                                     "camera changed old=%p new=%p pos=%.2f,%.2f,%.2f",
                                     (void*)diagPrevCam, (void*)ch,
                                     cam.x, cam.y, cam.z);
                            diagPrevCam = ch;
                        }
                        cameraValid = true;
                    }
                }
            }
            if (!cameraValid) {
                camAddr = 0;
                g_currentCam.store(0, std::memory_order_release);
                if (diagPrevCam != 0) {
                    diagRate("camera-invalid", 0,
                             "camera invalid old=%p", (void*)diagPrevCam);
                    diagPrevCam = 0;
                }
                invalidateAimSnapshot();
            }
        }

        // ---- Đợt 1: stream-proof window affinity -----------------------------
        {
            static int wdaApplied = -1;
            if (cfgWda != wdaApplied) {
                // 0x11 = WDA_EXCLUDEFROMCAPTURE (Win10 2004+): the panel vanishes
                // from screenshots/OBS/window capture but stays on your screen.
                SetWindowDisplayAffinity(hwnd, cfgWda ? 0x00000011u : 0u);
                wdaApplied = cfgWda;
            }
        }
        // ---- Đợt 1: auto-load the profile bound to this place ----------------
        {
            static uint64_t lastAutoPlace = 0;
            if (place && place != lastAutoPlace) {
                lastAutoPlace = place;
                if (cfgAutoProfile) {
                    char pkey[64];
                    snprintf(pkey, sizeof pkey, "profile.place.%llu",
                             (unsigned long long)place);
                    std::string slot = cfgStrValue(pkey);
                    if (!slot.empty() && slot != "-" && profileExists(slot) &&
                        loadProfileSlot(slot))
                        toast("auto profile %s", slot.c_str());
                }
            }
        }

        // ---- F8 sens calib: fixed N counts -> yaw delta -> px/count.
        // Game must be foreground; absorbs Windows speed/precision + DPI.
        if (!typingNow && (GetAsyncKeyState(CALIB_VK) & 1) && calibPhase == 0) {
            if (fgPid == m.pid && camAddr && o.cameraRotation) {
                float r0[9]{};
                if (m.read(camAddr + (uint64_t)o.cameraRotation, r0)) {
                    calibYaw0 = lookYaw(r0);
                    sendRelMouse(calibCounts, 0); // +X inject, NOCOALESCE
                    calibT0 = qpcSeconds();       // stamp after inject
                    calibPhase = 1;
                }
            }
        }
        if (calibPhase == 1 && qpcSeconds() - calibT0 > 0.35) {
            float r1[9]{};
            if (camAddr && o.cameraRotation &&
                m.read(camAddr + (uint64_t)o.cameraRotation, r1)) {
                float dyaw = wrapPi(lookYaw(r1) - calibYaw0);
                float adyaw = fabsf(dyaw);
                if (adyaw > 0.0005f) {
                    int vw = vpw ? vpw : sw, vh = vph ? vph : sh;
                    float vfovDeg = 70.0f, vf = 0;
                    if (o.cameraFOV &&
                        m.read(camAddr + (uint64_t)o.cameraFOV, vf) &&
                        std::isfinite(vf) && vf > 10.0f && vf < 150.0f)
                        vfovDeg = vf;
                    float aspect = vh > 0 ? (float)vw / (float)vh : 16.0f / 9.0f;
                    float vfovR = vfovDeg * 3.14159265f / 180.0f;
                    float hfovR = 2.0f * atanf(tanf(vfovR / 2.0f) * aspect);
                    float pxPerRad = (float)vw / (2.0f * tanf(hfovR / 2.0f));
                    float pxPerCount =
                        adyaw * pxPerRad / (float)calibCounts;
                    if (std::isfinite(pxPerCount) && pxPerCount > 0.01f &&
                        pxPerCount < 50.0f) {
                        cfgAimGameSens = pxPerCount;
                        char buf[32];
                        snprintf(buf, sizeof buf, "%.4f", pxPerCount);
                        upsertCfgKey("aim.gamesens", buf);
                    }
                }
            }
            calibPhase = 0;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        // Our window is never focused, and the game's cursor clipping makes the
        // backend fire WM_MOUSELEAVE all the time, after which it parks the mouse
        // at (-FLT_MAX) until the next WM_MOUSEMOVE. With no position ImGui has no
        // HoveredWindow, so a click that comes without a preceding mouse move is
        // attributed to nothing and silently does nothing. Feed the real position
        // ourselves every frame (client coords, same space as the backend's).
        if (!g_clickthru) {
            POINT mp{};
            if (GetCursorPos(&mp) && ScreenToClient(hwnd, &mp))
                ImGui::GetIO().AddMousePosEvent((float)mp.x, (float)mp.y);
        }
        // The old "ext P2.1" diagnostics window is gone: the clickable menu is
        // the only on-screen window now (backup of the HUD build:
        // backup\overlay_HUD_version_20261017.cpp / .exe).
        ImGui::NewFrame();

        if (g_diagState) {
            ImGuiIO& dio2 = ImGui::GetIO();
            if (dio2.MouseClicked[0] || dio2.MouseReleased[0])
                printf("mouse %s at %.0f,%.0f hoveredId=%u win=%s\n",
                       dio2.MouseClicked[0] ? "DOWN" : "UP", dio2.MousePos.x,
                       dio2.MousePos.y, ImGui::GetHoveredID(),
                       ImGui::GetCurrentContext()->HoveredWindow
                           ? ImGui::GetCurrentContext()->HoveredWindow->Name
                           : "(none)");
        }

        // ---- watermark / status HUD -------------------------------------------
        // Drawn with the draw list (not an ImGui window), so it is never part of
        // g_menuHits and every click over it still reaches the game.
        if (cfgHudOn) {
            ImDrawList* hdl = ImGui::GetForegroundDrawList();
            char l1[224];
            snprintf(l1, sizeof l1,
                     "ext  |  fps %.0f  loop %.1fms  |  place %llu  |  %s",
                     ImGui::GetIO().Framerate, loopMs, (unsigned long long)place,
                     haveWrite ? "mem ON" : "mem OFF");
            ImU32 shadow = IM_COL32(0, 0, 0, 220);
            ImU32 col = IM_COL32(235, 235, 235, 240);
            hdl->AddText(ImVec2(13.0f, 11.0f), shadow, l1);
            hdl->AddText(ImVec2(12.0f, 10.0f), col, l1);
            if (cfgHudFeats) {
                std::string f;
                auto addf = [&](const char* n, bool on) {
                    if (!on) return;
                    if (!f.empty()) f += "  ";
                    f += n;
                };
                addf("aim", aimOn && cfgAimEnabled);
                addf("trig", trigOn && cfgTrigEnabled);
                addf("ac", acOn && cfgACEnabled);
                addf("hitbox", cfgHitboxEnabled != 0);
                addf("fly", cfgFlyEnabled && g_flyActive);
                addf("speed", cfgPlySpeed && g_speedActive);
                addf("jump", cfgPlyJump && g_jumpActive);
                addf("aim(mem)", cfgAimMode == 1);
                char l2[256];
                snprintf(l2, sizeof l2, "on: %s",
                         f.empty() ? "(nothing)" : f.c_str());
                hdl->AddText(ImVec2(13.0f, 27.0f), shadow, l2);
                hdl->AddText(ImVec2(12.0f, 26.0f),
                             f.empty() ? IM_COL32(170, 170, 170, 230)
                                       : IM_COL32(120, 255, 140, 240),
                             l2);
            }
        }

        // ---- Clickable + draggable menu panel --------------------------------
        // Only the widgets inside this panel take mouse input (see WndProc):
        // every other pixel - HUD, ESP, aim, even the gaps between the controls -
        // passes the click on to the game, and WS_EX_NOACTIVATE keeps the game
        // focused the whole time. Drag by the title bar; the position is written
        // to overlay.cfg the moment a drag ends.
        g_menuHits.clear();
        mouseLockUpdate(cfgMenuCollapsed == 0); // expanded = free the cursor
        { // panel always exists; the title-bar triangle collapses it
            if (cfgMenuX < 0.0f) cfgMenuX = 0.0f;
            if (cfgMenuY < 0.0f) cfgMenuY = 0.0f;
            // Stale/saved position must not place the panel out of reach on the
            // first frame (screen size or scale may have changed since).
            float orgX = cfgMenuX, orgY = cfgMenuY;
            if (cfgMenuX > (float)sw - g_menuW - 8.0f)
                cfgMenuX = (float)sw - g_menuW - 8.0f;
            if (cfgMenuY > (float)sh - g_menuH - 8.0f)
                cfgMenuY = (float)sh - g_menuH - 8.0f;
            if (cfgMenuX != orgX || cfgMenuY != orgY) {
                char pb[64];
                snprintf(pb, sizeof pb, "%.0f,%.0f", cfgMenuX, cfgMenuY);
                upsertCfgKey("menu.pos", pb);
            }
            ImGui::SetNextWindowPos(ImVec2(cfgMenuX, cfgMenuY),
                                    g_menuPosForce ? ImGuiCond_Always
                                                   : ImGuiCond_Once);
            g_menuPosForce = false;
            ImGui::SetNextWindowBgAlpha(0.93f);
            // Keep the 250px floor in both states: expanded uses the content
            // width; collapsed still auto-fits to its title, but the floor
            // keeps the pill readable and an easy click target instead of a
            // ~16px sliver.
            ImGui::SetNextWindowSizeConstraints(ImVec2(250.0f, 0.0f),
                                                ImVec2(380.0f, 1.0e9f));
            // Collapse (title-bar triangle / double-click on the title) is a
            // feature now: menu.collapsed is applied once at start, ImGui owns
            // the state after that and changes are written back below. The
            // menu.size save guard (g_menuH > 60) keeps the ~25px collapsed
            // height out of the saved geometry.
            static bool collapseApplied = false;
            if (!collapseApplied) {
                ImGui::SetNextWindowCollapsed(cfgMenuCollapsed != 0,
                                              ImGuiCond_Always);
                collapseApplied = true;
            }
            ImGui::Begin("ext menu", nullptr,
                         ImGuiWindowFlags_AlwaysAutoResize);
            {
                ImVec2 wp = ImGui::GetWindowPos();
                ImVec2 wsz = ImGui::GetWindowSize();
                g_menuW = wsz.x;
                g_menuH = wsz.y;
                // Triangle / title double-click collapsed or expanded it: keep
                // the cfg in sync (one write on the change frame).
                if (ImGui::IsWindowCollapsed() != (cfgMenuCollapsed != 0)) {
                    cfgMenuCollapsed = ImGui::IsWindowCollapsed() ? 1 : 0;
                    upsertCfgKey("menu.collapsed",
                                 cfgMenuCollapsed ? "1" : "0");
                }
                // remember the panel size so the next start can place the saved
                // position exactly (and still keep it on screen). Only after the
                // size has held still for ~20 frames: the first frames of an
                // auto-resizing window report a transient 250x35 (or 19px
                // collapsed) size and saving that would poison the cfg.
                static int sizeStable = 0;
                static float szW = -1.0f, szH = -1.0f;
                if (fabsf(g_menuW - szW) > 0.5f || fabsf(g_menuH - szH) > 0.5f) {
                    szW = g_menuW;
                    szH = g_menuH;
                    sizeStable = 0;
                } else if (sizeStable < 1000) {
                    ++sizeStable;
                }
                if (sizeStable == 20 && g_menuH > 60.0f &&
                    (fabsf(g_menuW - g_savedMenuW) > 1.0f ||
                     fabsf(g_menuH - g_savedMenuH) > 1.0f)) {
                    char sb[64];
                    snprintf(sb, sizeof sb, "%.0f,%.0f", g_menuW, g_menuH);
                    upsertCfgKey("menu.size", sb);
                    g_savedMenuW = g_menuW;
                    g_savedMenuH = g_menuH;
                }
                // title bar = drag handle (also holds collapse arrow + close)
                g_menuHits.push_back({ wp.x, wp.y, wp.x + wsz.x,
                                       wp.y + ImGui::GetFrameHeight() + 4.0f });
                // drag ended -> remember where the user put it
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                    (fabsf(wp.x - cfgMenuX) > 0.5f ||
                     fabsf(wp.y - cfgMenuY) > 0.5f)) {
                    cfgMenuX = wp.x;
                    cfgMenuY = wp.y;
                    char pb[64];
                    snprintf(pb, sizeof pb, "%.0f,%.0f", cfgMenuX, cfgMenuY);
                    upsertCfgKey("menu.pos", pb);
                }
                // ImGui drags windows past the screen edge - if that just
                // happened, pull the panel back so its title bar stays grabbable
                // (never fight the mouse while the button is still down).
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    float cx = wp.x, cy = wp.y;
                    float limY = (float)sh - ImGui::GetFrameHeight() - 6.0f;
                    if (cx < 0.0f) cx = 0.0f;
                    if (cy < 0.0f) cy = 0.0f;
                    if (cx > (float)sw - 120.0f) cx = (float)sw - 120.0f;
                    if (cy > limY) cy = limY;
                    if (fabsf(cx - wp.x) > 0.5f || fabsf(cy - wp.y) > 0.5f) {
                        cfgMenuX = cx;
                        cfgMenuY = cy;
                        g_menuPosForce = true;
                        char pb[64];
                        snprintf(pb, sizeof pb, "%.0f,%.0f", cx, cy);
                        upsertCfgKey("menu.pos", pb);
                    }
                }
            }
            // Collapsed = title bar only: skip the entire content. ImGui would
            // draw nothing anyway, but markChildArea/hitMark must not run (a
            // phantom rect below the title would swallow game clicks).
            if (!ImGui::IsWindowCollapsed()) {
            // ---- tabs: plain buttons, so the hit rects are exactly the tab
            // buttons (same reason the impl pickers are buttons, not combos).
            // 8 tabs, two rows of four: badge/tools order follows the roadmap
            // (Body, Combat, Visual, Teleport | World, Server, Tools, Misc) ----
            {
                static const char* tabNames[8] = { "Body", "Combat", "Visual",
                                                   "Teleport", "World", "Server",
                                                   "Tools", "Misc" };
                for (int t = 0; t < 8; ++t) {
                    if (t == 4)
                        ImGui::NewLine();
                    else if (t)
                        ImGui::SameLine();
                    bool act = (cfgMenuTab == t);
                    if (act)
                        ImGui::PushStyleColor(ImGuiCol_Button,
                                              ImVec4(0.26f, 0.59f, 0.98f, 1.0f));
                    if (ImGui::Button(tabNames[t], ImVec2(76.0f, 0.0f))) {
                        cfgMenuTab = t;
                        char b[8];
                        snprintf(b, sizeof b, "%d", t);
                        upsertCfgKey("menu.tab", b);
                        if (g_diagState) {
                            printf("tab click -> %d (%s)\n", t, tabNames[t]);
                            fflush(stdout);
                        }
                    }
                    if (act) ImGui::PopStyleColor();
                    hitMark();
                }
            }
            ImGui::Separator();

            MenuCtx tabCtx{ m, o, ents, place, haveWrite,
                            aimOn, trigOn, acOn,
                            flyApply, speedApply, jumpApply,
                            dbgEnts, loopMs, nowLoop };
            if      (cfgMenuTab == 0) drawTabBody(tabCtx);
            else if (cfgMenuTab == 1) drawTabCombat(tabCtx);
            else if (cfgMenuTab == 2) drawTabVisual(tabCtx);
            else if (cfgMenuTab == 3) drawTabTeleport(tabCtx);
            else if (cfgMenuTab == 4) drawTabWorld(tabCtx);
            else if (cfgMenuTab == 5) drawTabServer(tabCtx);
            else if (cfgMenuTab == 6) drawTabTools(tabCtx);
            else                      drawTabMisc(tabCtx);

            // ---- status footer (always visible, outside the tabs) ----
            ImGui::Separator();
            if (typingNow)
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f),
                                   "typing: aim/keys paused");
            if (g_toast[0] && nowLoop - g_toastTick < 3000)
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 1.0f, 1.0f), "%s", g_toast);
            if (tpMsg[0] && nowLoop - tpMsgTick < 4000)
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", tpMsg);
            ImGui::TextDisabled("%s: hide   \\: quit", keyName(g_toggleVk).c_str());
            } // !collapsed
            if (g_diagState) {
                static DWORD lastMenuDiag = 0;
                DWORD nd = GetTickCount();
                if (nd - lastMenuDiag >= 2000) {
                    lastMenuDiag = nd;
                    ImVec2 wp2 = ImGui::GetWindowPos();
                    ImVec2 ws2 = ImGui::GetWindowSize();
                    ImGuiIO& dio = ImGui::GetIO();
                    printf("menu tab=%d pos=%.0f,%.0f size=%.0f,%.0f hits=%d "
                           "collapsed=%d mouse=%.0f,%.0f down=%d fg=%lu pop=%d/%d "
                           "col=%s ents=%d vm=%d vp=%.0f,%.0f,%.0f,%.0f\n",
                           cfgMenuTab, wp2.x, wp2.y, ws2.x, ws2.y,
                           (int)g_menuHits.size(),
                           ImGui::IsWindowCollapsed() ? 1 : 0, dio.MousePos.x,
                           dio.MousePos.y, (int)dio.MouseDown[0],
                           (unsigned long)fgPid, g_popupHits,
                           ImGui::GetCurrentContext()
                               ? ImGui::GetCurrentContext()->OpenPopupStack.Size
                               : -1,
                           g_colHover ? g_colHover : "-", dbgEnts, g_espVmDiag,
                           (float)vpx, (float)vpy, (float)vpw, (float)vph);
                    for (size_t i = 0; i < g_menuHits.size() && i < 40; ++i)
                        printf("  hit[%d] = %.0f,%.0f - %.0f,%.0f\n", (int)i,
                               g_menuHits[i].l, g_menuHits[i].t, g_menuHits[i].r,
                               g_menuHits[i].b);
                    fflush(stdout);
                }
            }
            ImGui::End();
        }
        // Popups (the colour pickers) live in their own ImGui windows, so hitMark
        // never sees them: without this a click inside an open popup would fall
        // through to the game.
        if (!g_clickthru) {
            ImGuiContext* gctx = ImGui::GetCurrentContext();
            bool anyPopup = false;
            g_popupHits = 0;
            if (gctx) {
                for (int i = 0; i < gctx->Windows.Size; ++i) {
                    ImGuiWindow* w = gctx->Windows[i];
                    if (!w || !(w->Flags & ImGuiWindowFlags_Popup)) continue;
                    if (!w->Active && !w->WasActive) continue;
                    anyPopup = true;
                    ++g_popupHits;
                    g_menuHits.push_back({ w->Pos.x, w->Pos.y,
                                           w->Pos.x + w->Size.x,
                                           w->Pos.y + w->Size.y });
                }
            }
            if (anyPopup) {
                // While a popup is up, the whole window takes clicks: that is the
                // only way ImGui ever sees the "clicked outside" that closes it,
                // and we have no keyboard focus so Escape cannot arrive either.
                g_menuHits.push_back({ 0.0f, 0.0f, (float)sw, (float)sh });
            }
        }

        // ---- ESP: cached list refreshes independently of visibility; entity
        // positions are read every visible frame.
        if ((frame % 60) == 0) {
            RECT gr{};
            if (gameClientRect(m.pid, gr)) {
                vpx = gr.left;
                vpy = gr.top;
                vpw = gr.right - gr.left;
                vph = gr.bottom - gr.top;
            }
        }
        float vm[16]{};
        uint64_t ve = 0;
        bool haveVm = m.read(sdk.base + o.visualEnginePtr, ve) && ve &&
                      m.read(ve + (uint64_t)o.visualEngineViewMatrix, vm);
        if (haveVm) {
            for (int k = 0; k < 16; ++k)
                if (!std::isfinite(vm[k])) {
                    haveVm = false;
                    break;
                }
        }
        g_espVmDiag = haveVm ? 1 : 0;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (haveVm) {
            uint64_t espLocalTeam = 0;
            Vec3 espSelf{};
            bool espSelfOk = false;
            for (auto& e : ents) {
                if (!e.self) continue;
                espLocalTeam = e.team;
                if (m.read(e.prim + (uint64_t)o.primitivePosition, espSelf) &&
                    std::isfinite(espSelf.x) && std::isfinite(espSelf.y) &&
                    std::isfinite(espSelf.z))
                    espSelfOk = true;
                break;
            }
            auto rgb = [](int c) {
                return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255);
            };
            // A primitive's position, projected (skeleton + head dot).
            auto w2sPrim = [&](uint64_t prim, int& px, int& py) {
                if (!prim) return false;
                Vec3 p{};
                if (!m.read(prim + (uint64_t)o.primitivePosition, p)) return false;
                if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
                    !std::isfinite(p.z))
                    return false;
                return w2s(p, vm, px, py);
            };
            // Projection that stays usable when the point is off screen or behind
            // the camera: the off-screen arrows need a direction, not a pixel.
            auto w2sRay = [&](const Vec3& wp, float& nx, float& ny) {
                float x = vm[0] * wp.x + vm[1] * wp.y + vm[2] * wp.z + vm[3];
                float y = vm[4] * wp.x + vm[5] * wp.y + vm[6] * wp.z + vm[7];
                float ww = vm[12] * wp.x + vm[13] * wp.y + vm[14] * wp.z + vm[15];
                bool front = (ww > 0.1f);
                float wc = front ? ww : 0.1f;
                nx = x / wc;
                ny = y / wc;
                if (!front) {
                    nx = -nx;
                    ny = -ny;
                }
            };
            if (cfgEspOn)
            for (auto& e : ents) {
                // Skip self: camera hugs the local character, tiny view depth
                // amplifies noise into jitter (velext defaults showSelf=0 too).
                if (e.self) continue;
                if (e.isNPC && !cfgEspNpc) continue;
                Vec3 root{};
                if (!m.read(e.prim + (uint64_t)o.primitivePosition, root))
                    continue;
                if (!std::isfinite(root.x) || !std::isfinite(root.y) ||
                    !std::isfinite(root.z))
                    continue;
                float dist = -1.0f;
                if (espSelfOk) {
                    float dx = root.x - espSelf.x, dy = root.y - espSelf.y,
                          dz = root.z - espSelf.z;
                    dist = sqrtf(dx * dx + dy * dy + dz * dz);
                }
                if (cfgEspMaxDist > 0 && dist > (float)cfgEspMaxDist) continue;
                Vec3 head{ root.x, root.y + 2.8f, root.z };
                Vec3 feet{ root.x, root.y - 2.5f, root.z };
                int hx = 0, hy = 0, fx = 0, fy = 0;
                bool onScreen = w2s(head, vm, hx, hy) && w2s(feet, vm, fx, fy);
                int h = fy - hy;
                if (onScreen && (h < 4 || h > 2000)) onScreen = false;
                ImU32 col = e.isNPC ? rgb(cfgEspColNpc)
                          : (e.team && espLocalTeam && e.team == espLocalTeam)
                                ? rgb(cfgEspColTeam)
                                : rgb(cfgEspColEnemy);
                ImU32 edge = IM_COL32(0, 0, 0, 220);
                if (!onScreen) {
                    if (cfgEspOof) {
                        float nx = 0, ny = 0;
                        w2sRay(head, nx, ny);
                        float m2 = fmaxf(fabsf(nx), fabsf(ny));
                        if (m2 > 1.0f) {
                            float ax = nx / m2, ay = ny / m2;
                            float bw = (float)sw * 0.42f, bh = (float)sh * 0.42f;
                            float px = (float)sw * 0.5f + ax * bw;
                            float py = (float)sh * 0.5f + ay * bh;
                            float p2x = -ay, p2y = ax;
                            ImVec2 tip(px + ax * 10.0f, py + ay * 10.0f);
                            dl->AddTriangleFilled(
                                tip, ImVec2(px + p2x * 7.0f, py + p2y * 7.0f),
                                ImVec2(px - p2x * 7.0f, py - p2y * 7.0f), col);
                            if (dist >= 0.0f) {
                                char db[24];
                                snprintf(db, sizeof db, "%.0fm", dist);
                                dl->AddText(ImVec2(tip.x - 10.0f, tip.y - 16.0f),
                                            edge, db);
                                dl->AddText(ImVec2(tip.x - 11.0f, tip.y - 17.0f),
                                            col, db);
                            }
                        }
                    }
                    continue;
                }
                int w = h / 2;
                int cx = (hx + fx) / 2;
                float bx0 = (float)(cx - w / 2), bx1 = (float)(cx + w / 2);
                float by0 = (float)hy, by1 = (float)fy;
                if (cfgEspFill)
                    dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1),
                                      (col & 0x00FFFFFFu) | 0x38000000u);
                if (cfgEspBox) {
                    // bold box: black underlay then colour on top
                    dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), edge, 0.0f, 0,
                                4.0f);
                    dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), col, 0.0f, 0,
                                2.0f);
                }
                if (cfgEspHp && e.humanoid && o.humanoidHealth &&
                    o.humanoidMaxHealth) {
                    float hp = 0.0f, mhp = 0.0f;
                    if (m.read(e.humanoid + (uint64_t)o.humanoidHealth, hp) &&
                        m.read(e.humanoid + (uint64_t)o.humanoidMaxHealth, mhp) &&
                        std::isfinite(hp) && mhp > 0.0f) {
                        float f = hp / mhp;
                        if (f < 0.0f) f = 0.0f;
                        if (f > 1.0f) f = 1.0f;
                        float hbx0 = bx0 - 6.0f, hbx1 = bx0 - 3.0f;
                        dl->AddRectFilled(ImVec2(hbx0, by0), ImVec2(hbx1, by1),
                                          IM_COL32(0, 0, 0, 190));
                        ImU32 hcol = f > 0.5f
                                         ? IM_COL32(80, 255, 80, 255)
                                         : (f > 0.25f ? IM_COL32(255, 220, 60, 255)
                                                      : IM_COL32(255, 70, 70, 255));
                        dl->AddRectFilled(ImVec2(hbx0, by1 - (by1 - by0) * f),
                                          ImVec2(hbx1, by1), hcol);
                    }
                }
                if (cfgEspName || cfgEspDist) {
                    char nb[112];
                    const char* label = e.tag.c_str();
                    if (cfgEspDist && dist >= 0.0f) {
                        snprintf(nb, sizeof nb, "%s [%.0fm]", e.tag.c_str(), dist);
                        label = nb;
                    }
                    ImVec2 ts = ImGui::CalcTextSize(label);
                    ImVec2 tp((float)cx - ts.x * 0.5f, by0 - ts.y - 4.0f);
                    dl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), edge, label);
                    dl->AddText(tp, col, label);
                }
                if (cfgEspHead && e.headPrim) {
                    int px = 0, py = 0;
                    if (w2sPrim(e.headPrim, px, py))
                        dl->AddCircleFilled(ImVec2((float)px, (float)py), 3.5f, col);
                }
                if (cfgEspLine)
                    dl->AddLine(ImVec2((float)sw * 0.5f, (float)sh),
                                ImVec2((float)cx, by1),
                                (col & 0x00FFFFFFu) | 0x90000000u, 1.5f);
                if (cfgEspSkel && e.headPrim) {
                    int rpx = 0, rpy = 0, hpx = 0, hpy = 0;
                    if (w2sPrim(e.prim, rpx, rpy) &&
                        w2sPrim(e.headPrim, hpx, hpy)) {
                        dl->AddLine(ImVec2((float)hpx, (float)hpy),
                                    ImVec2((float)rpx, (float)rpy), col, 1.5f);
                        for (int i = 0; i < 4; ++i) {
                            int lx = 0, ly = 0;
                            if (w2sPrim(e.limbPrim[i], lx, ly))
                                dl->AddLine(ImVec2((float)rpx, (float)rpy),
                                            ImVec2((float)lx, (float)ly), col,
                                            1.5f);
                        }
                    }
                }
            }
            // ---- radar: top-right square, self at the centre, camera-up = up ----
            if (cfgRadarOn) {
                float r9[9]{};
                bool haveRot =
                    camAddr && m.read(camAddr + (uint64_t)o.cameraRotation, r9);
                int ew = vpw ? vpw : sw;
                int eox = vpw ? vpx : 0, eoy = vph ? vpy : 0;
                float sz = (float)cfgRadarSize;
                float rx = (float)(eox + ew) - sz - 14.0f;
                float ry = (float)eoy + 14.0f + (cfgHudOn ? 34.0f : 0.0f);
                ImVec2 c0(rx + sz * 0.5f, ry + sz * 0.5f);
                // NB: a black translucent fill would blend down to pure black,
                // which is the window colour key -> the panel would vanish.
                dl->AddRectFilled(ImVec2(rx, ry), ImVec2(rx + sz, ry + sz),
                                  IM_COL32(10, 12, 16, 150), 6.0f);
                dl->AddRect(ImVec2(rx, ry), ImVec2(rx + sz, ry + sz),
                            IM_COL32(200, 200, 200, 90), 6.0f);
                dl->AddLine(ImVec2(c0.x - 5.0f, c0.y), ImVec2(c0.x + 5.0f, c0.y),
                            IM_COL32(255, 255, 255, 150));
                dl->AddLine(ImVec2(c0.x, c0.y - 5.0f), ImVec2(c0.x, c0.y + 5.0f),
                            IM_COL32(255, 255, 255, 150));
                float range = (float)(cfgRadarRange > 10 ? cfgRadarRange : 10);
                float scale = (sz * 0.5f - 8.0f) / range;
                // Camera CFrame layout (see lookYaw): column 0 = Right,
                // column 2 = -Look, so look = -(m[2],m[5],m[8]).
                float fwdX = -r9[2], fwdZ = -r9[8];
                float rgtX = r9[0], rgtZ = r9[6];
                float fl = sqrtf(fwdX * fwdX + fwdZ * fwdZ);
                float rl = sqrtf(rgtX * rgtX + rgtZ * rgtZ);
                if (haveRot && fl > 1e-4f && rl > 1e-4f && espSelfOk) {
                    fwdX /= fl;
                    fwdZ /= fl;
                    rgtX /= rl;
                    rgtZ /= rl;
                    for (auto& e : ents) {
                        if (e.self) continue;
                        if (e.isNPC && !cfgEspNpc) continue;
                        Vec3 p{};
                        if (!m.read(e.prim + (uint64_t)o.primitivePosition, p))
                            continue;
                        if (!std::isfinite(p.x) || !std::isfinite(p.z)) continue;
                        float dx = p.x - espSelf.x, dz = p.z - espSelf.z;
                        float px = c0.x + (dx * rgtX + dz * rgtZ) * scale;
                        float py = c0.y - (dx * fwdX + dz * fwdZ) * scale;
                        if (px < rx + 3.0f || px > rx + sz - 3.0f ||
                            py < ry + 3.0f || py > ry + sz - 3.0f)
                            continue;
                        ImU32 c = e.isNPC ? rgb(cfgEspColNpc)
                                : (e.team && espLocalTeam &&
                                   e.team == espLocalTeam)
                                      ? rgb(cfgEspColTeam)
                                      : rgb(cfgEspColEnemy);
                        dl->AddCircleFilled(ImVec2(px, py), 3.0f, c);
                    }
                }
            }

            // ---- Aimbot v2 (velext knobs): sticky + dead/team/dist filters +
            // velocity prediction + deadzone, then mouse or memory actuation.
            // Foulz: FOV + selection centered on the REAL cursor, not the
            // viewport center (identical when locked, correct in menus).
            // The game's true crosshair is ALWAYS the viewport center when
            // using raw input (Windows cursor may sit in a corner).
            int vw = vpw ? vpw : sw, vh = vph ? vph : sh;
            int ccx = (vpw ? vpx : 0) + vw / 2;
            int ccy = (vph ? vpy : 0) + vh / 2;

            // Draw FOV at the true crosshair (not Windows cursor)
            dl->AddCircle(ImVec2((float)ccx, (float)ccy), (float)cfgAimFov,
                          IM_COL32(255, 255, 255, 90), 48);
            bool wantAim = aimOn && cfgAimEnabled && fgPid == m.pid && !typingNow;
            bool wantTrig = trigOn && cfgTrigEnabled && fgPid == m.pid && !typingNow;
            shiftedNow = false;
            if (!wantAim) {
                emaInit = false;
                emaPrim = 0;
                mouseCtl = MouseCtlState{};
                accX = 0.0f;
                accY = 0.0f;
                bpInFlight = false;
                bpStampNs = 0;
                bpCursorHold = false;
                bpStaleCount = 0;
                bpStaleHold = false;
                bpProbeNs = 0;
                mouseSvc = 0;      // re-resolve on next enable (<=1/s)
                mouseSvcRetry = 0;
            }
            if ((wantAim || wantTrig) && haveVm) {
                dbgEnts = (int)ents.size();
                DWORD nowTick = GetTickCount();
                double nowQ = qpcSeconds();
                float dt = lastAimQpc > 0.0 ? (float)(nowQ - lastAimQpc) : 0.016f;
                if (dt < 0) dt = 0;
                if (dt > 0.1f) dt = 0.1f;
                lastAimQpc = nowQ;

                uint64_t localTeam = 0;
                for (auto& e : ents) {
                    if (e.self) {
                        localTeam = e.team;
                        break;
                    }
                }

                // Best-target record (v3: score-based, sticky by Player ID).
                struct AimTarget {
                    int sx = 0, sy = 0;
                    Vec3 world{}, root{}, vel{};
                    uint64_t prim = 0, player = 0;
                    float distPx = 0, radiusPx = 0, score = 0;
                };

                AimTarget bestTarget{};
                bool haveTarget = false;

                // evaluateEntity: filters + bone + prediction + project + score.
                auto evaluateEntity = [&](Ent& e, AimTarget& outT) -> bool {
                    if (!e.prim) return false;
                    if (e.player && o.playerModelInstance) {
                        uint64_t currentModel = 0;
                        if (!m.read(e.player + (uint64_t)o.playerModelInstance,
                                    currentModel) ||
                            !SDK::plausiblePtr(currentModel) ||
                            currentModel != e.model)
                            return false;
                    }
                    // Health is a validity check, not just an optional filter:
                    // retaining a dead Humanoid's last primitive causes stale
                    // locks until the slow entity refresh removes it.
                    if (e.humanoid && o.humanoidHealth) {
                        float hp = 0.0f;
                        if (!m.read(e.humanoid + (uint64_t)o.humanoidHealth, hp) ||
                            !std::isfinite(hp) || hp <= 0.0f ||
                            hp > 1000000.0f)
                            return false;
                    }
                    Vec3 root{};
                    if (!m.read(e.prim + (uint64_t)o.primitivePosition, root))
                        return false;
                    if (!std::isfinite(root.x) || !std::isfinite(root.y) ||
                        !std::isfinite(root.z))
                        return false;
                    if (cfgAimIgnoreTeam && e.team && localTeam && e.team == localTeam)
                        return false;
                    if (cfgAimMaxDist > 0) {
                        float ddx = root.x - cam.x, ddy = root.y - cam.y,
                              ddz = root.z - cam.z;
                        if (sqrtf(ddx * ddx + ddy * ddy + ddz * ddz) > cfgAimMaxDist)
                            return false;
                    }
                    Vec3 base = root;
                    bool haveBase = false;
                    uint64_t anchorPrim = 0;
                    if (cfgAimBone == 0) {
                        anchorPrim = e.headPrim;
                    } else if (cfgAimBone >= 2 && cfgAimBone <= 5) {
                        anchorPrim = e.limbPrim[cfgAimBone - 2];
                    }
                    if (anchorPrim) {
                        Vec3 hp{};
                        if (m.read(anchorPrim + (uint64_t)o.primitivePosition, hp) &&
                            std::isfinite(hp.x) && std::isfinite(hp.y) &&
                            std::isfinite(hp.z)) {
                            base = hp;
                            haveBase = true;
                        }
                    }
                    if (!haveBase)
                        base = Vec3{ root.x, root.y + cfgAimHeight, root.z };
                    Vec3 head = base;
                    if (cfgAimPredict && o.primitiveVelocity) {
                        Vec3 vel{};
                        if (m.read(e.prim + (uint64_t)o.primitiveVelocity, vel) &&
                            std::isfinite(vel.x) && std::isfinite(vel.y) &&
                            std::isfinite(vel.z)) {
                            head.x += vel.x * dt * cfgAimPScale;
                            outT.vel = vel;
                            head.y += vel.y * dt * cfgAimPScale;
                            head.z += vel.z * dt * cfgAimPScale;
                        }
                    }
                    Vec3 feet{ root.x, root.y - 2.5f, root.z };
                    int sx, sy, fx, fy;
                    if (!w2s(head, vm, sx, sy) || !w2s(feet, vm, fx, fy))
                        return false;
                    outT.sx = sx;
                    outT.sy = sy;
                    outT.world = head;
                    outT.root = root;
                    outT.prim = e.prim;
                    outT.player = e.player;
                    float bodyPx = sqrtf((float)((sx - fx) * (sx - fx) +
                                                (sy - fy) * (sy - fy)));
                    if (!std::isfinite(bodyPx) || bodyPx < 1.0f) bodyPx = 1.0f;
                    float k = (cfgAimBone == 0) ? cfgAimDynK : 0.7f;
                    float rawRadius = k * bodyPx + 2.0f;
                    // Expand the gate for fast targets so prediction has room
                    // to acquire them before they cross the center.
                    float velocityGate = 0.0f;
                    if (cfgAimPredict && o.primitiveVelocity) {
                        Vec3 vel{};
                        if (m.read(e.prim + (uint64_t)o.primitiveVelocity, vel) &&
                            std::isfinite(vel.x) && std::isfinite(vel.y) &&
                            std::isfinite(vel.z)) {
                            float speed = sqrtf(vel.x * vel.x +
                                                vel.y * vel.y +
                                                vel.z * vel.z);
                            float screenSpeedPxPerS = speed * 20.0f;
                            if (std::isfinite(screenSpeedPxPerS))
                                velocityGate = screenSpeedPxPerS * 0.100f;
                        }
                    }
                    const float baseGate = 50.0f;
                    float minGate = baseGate + velocityGate;
                    float acquireRadius =
                        (std::isfinite(rawRadius) && rawRadius > minGate)
                            ? rawRadius
                            : minGate;
                    if (acquireRadius > (float)cfgAimFov)
                        acquireRadius = (float)cfgAimFov;
                    float dx = (float)(sx - ccx), dy = (float)(sy - ccy);
                    float distPx = sqrtf(dx * dx + dy * dy);
                    outT.distPx = distPx;
                    outT.radiusPx = acquireRadius;
                    // FIX 2: score prefers near-center AND big-on-screen.
                    outT.score = distPx / (bodyPx * 0.5f > 8.0f ? bodyPx * 0.5f : 8.0f);
                    return true;
                };

                // (entAim folded into evaluateEntity above.)


                // 1. Sticky by Player ID (players) or prim (NPCs).
                // Player objects persist across respawns; prims don't.
                if (cfgAimSticky && stickPlayer != 0) {
                    for (auto& e : ents) {
                        bool match = e.self ? false
                                     : (e.player ? (e.player == stickPlayer)
                                                 : (e.prim == stickPlayer));
                        if (!match) continue;
                        AimTarget t;
                        if (evaluateEntity(e, t)) {
                            // FIX 3 (TrackGain v4): widen gate x2.5 while locked.
                            float trackRadius = t.radiusPx * 2.5f;
                            if (trackRadius > (float)cfgAimFov * 1.5f)
                                trackRadius = (float)cfgAimFov * 1.5f;
                            if (t.distPx <= trackRadius) {
                                bestTarget = t;
                                haveTarget = true;
                                bool drop = false;
                                if (lockTick != 0) {
                                    float ox = t.world.x - lockPos.x;
                                    float oy = t.world.y - lockPos.y;
                                    float oz = t.world.z - lockPos.z;
                                    if (sqrtf(ox * ox + oy * oy + oz * oz) > 40.0f)
                                        drop = true;
                                    if (stickStartTick == 0)
                                        stickStartTick = nowTick;
                                    if (!drop && cfgAimStickTime > 0 &&
                                        nowTick - stickStartTick >
                                            (DWORD)(cfgAimStickTime * 1000.0f))
                                        drop = true;
                                }
                                if (drop) {
                                    haveTarget = false;
                                    stickPlayer = 0;
                                    stickStartTick = 0;
                                    lockTick = 0;
                                } else {
                                    lockPos = t.world;
                                    lockTick = nowTick;
                                }
                            } else {
                                stickPlayer = 0;
                                stickStartTick = 0;
                                lockTick = 0;
                            }
                        } else {
                            stickPlayer = 0;
                            stickStartTick = 0;
                            lockTick = 0;
                        }
                        break;
                    }
                }
                // 2. Acquire: score-based by default (distance/size), nearest
                // when cfgAimNearest=1. Ghidra first-match dropped: it locked
                // far targets while ignoring near-center ones.
                if (!haveTarget) {
                    float bestScore = 1e9f;
                    for (auto& e : ents) {
                        if (e.self) continue;
                        AimTarget t;
                        if (!evaluateEntity(e, t)) continue;
                        if (t.distPx > t.radiusPx || t.distPx > (float)cfgAimFov)
                            continue;
                        if (cfgAimNearest) {
                            if (t.distPx < bestTarget.distPx || !haveTarget) {
                                bestTarget = t;
                                haveTarget = true;
                            }
                        } else {
                            if (t.score < bestScore) {
                                bestScore = t.score;
                                bestTarget = t;
                                haveTarget = true;
                            }
                        }
                    }
                    if (haveTarget && cfgAimSticky) {
                        stickPlayer = bestTarget.player ? bestTarget.player
                                                        : bestTarget.prim;
                        lockPos = bestTarget.world;
                        lockTick = nowTick;
                        stickStartTick = nowTick;
                    } else if (haveTarget) {
                    }
                }
                // Branch B overextend: aim past the head away from the root.
                if (haveTarget && cfgAimOverextend > 0) {
                    Vec3 ext{ bestTarget.world.x + cfgAimOverextend * (bestTarget.world.x - bestTarget.root.x),
                              bestTarget.world.y + cfgAimOverextend * (bestTarget.world.y - bestTarget.root.y),
                              bestTarget.world.z + cfgAimOverextend * (bestTarget.world.z - bestTarget.root.z) };
                    int ex, ey;
                    if (w2s(ext, vm, ex, ey)) {
                        bestTarget.sx = ex;
                        bestTarget.sy = ey;
                        bestTarget.world = ext;
                        float dx = (float)(ex - ccx);
                        float dy = (float)(ey - ccy);
                        bestTarget.distPx = sqrtf(dx * dx + dy * dy);
                        if (bestTarget.distPx > (float)cfgAimFov)
                            haveTarget = false;
                    } else {
                        haveTarget = false;
                    }
                }
                if (wantAim && !haveTarget) {
                    emaInit = false;
                    emaPrim = 0;
                    mouseCtl = MouseCtlState{};
                    accX = 0.0f;
                    accY = 0.0f;
                    invalidateAimSnapshot();
                }
                bool acted = (wantAim && haveTarget && bestTarget.distPx <= (float)cfgAimFov);
                if (!acted || (cfgAimMode == 1 &&
                               bestTarget.distPx < cfgAimDeadzone)) {
                    // Do not let the high-rate writer keep pursuing a target
                    // that is no longer an actionable, current selection.
                    invalidateAimSnapshot();
                }
                if (acted) {
                    float gdx = (float)(bestTarget.sx - ccx), gdy = (float)(bestTarget.sy - ccy);
                    float gdist = sqrtf(gdx * gdx + gdy * gdy);
                    if (gdist >= cfgAimDeadzone) {
                        bool paced = cfgAimInterval <= 0 ||
                                     (nowTick - lastActTick >= (DWORD)cfgAimInterval);
                        if (paced) lastActTick = nowTick;
                        if (paced && cfgAimMode == 2 && haveWrite && camAddr &&
                            o.cameraViewport && o.visualEngineDimensions) {
                            // Viewport-shift silent aim (Foulz): move the
                            // render viewport so the target lands on center.
                            // Neither cursor nor camera rotation is touched.
                            float resW = 0, resH = 0;
                            uint64_t ve2 = 0;
                            if (m.read(sdk.base + o.visualEnginePtr, ve2) && ve2 &&
                                m.read(ve2 + (uint64_t)o.visualEngineDimensions, resW) &&
                                m.read(ve2 + (uint64_t)o.visualEngineDimensions + 4, resH) &&
                                resW > 0 && resH > 0 && resW < 10000 && resH < 10000) {
                                int rx = vpw ? bestTarget.sx - vpx : bestTarget.sx;
                                int ry = vph ? bestTarget.sy - vpy : bestTarget.sy;
                                VP2 vp{ (short)((resW - rx) * 2),
                                        (short)((resH - ry) * 2) };
                                if (m.write(camAddr + (uint64_t)o.cameraViewport, vp)) {
                                    vpShifted = true;
                                    shiftedNow = true;
                                }
                            }
                        } else                         if (paced && cfgAimMode == 1 && haveWrite && camAddr) {
                            // Publish the target for the dedicated writer thread.
                            // Keeping one rotation writer avoids read/modify/write
                            // races with the render loop and the game's camera tick.
                            { std::lock_guard<std::mutex> lk(g_snapMu);
                              g_snap.ready = true;
                              g_snap.epoch = g_snapEpoch.load(std::memory_order_relaxed);
                              g_snap.seq = g_snapSeq.fetch_add(1, std::memory_order_relaxed) + 1;
                              g_snap.tgt = bestTarget.world;
                              g_snap.vel = bestTarget.vel;
                              g_snap.cm = cam;
                              g_snap.camAddr = camAddr;
                              g_snap.qpc = qpcSeconds(); }
                            diagRate("snapshot-publish", 100,
                                     "snapshot publish epoch=%llu cam=%p player=%p prim=%p target=%.2f,%.2f,%.2f",
                                     (unsigned long long)g_snapEpoch.load(),
                                     (void*)camAddr, (void*)bestTarget.player,
                                     (void*)bestTarget.prim,
                                     bestTarget.world.x, bestTarget.world.y,
                                     bestTarget.world.z);
                        } else if (paced) {
                            uint64_t tgtKey = bestTarget.player
                                                  ? bestTarget.player
                                                  : bestTarget.prim;
                            float tsmooth = cfgAimTSmooth;
                            if (tsmooth < 0.0f) tsmooth = 0.0f;
                            if (tsmooth > 0.99f) tsmooth = 0.99f;
                            float gamesens = cfgAimGameSens > 0.05f
                                                 ? cfgAimGameSens
                                                 : 0.05f;
                            float outGain = cfgAimSens / gamesens;
                            if (outGain < 0.1f) outGain = 0.1f;
                            if (outGain > 4.0f) outGain = 4.0f;
                            if (cfgAimPid == 0) {
                                // ---- velext mouse model (aim.pid=1 keeps the
                                // legacy filters): velext alpha, camera
                                // back-pressure, real angle->counts mapping
                                // (Phase C), per-step clamp, 1.5 px deadzone.
                                // No smoothed state is kept. ----
                                uint64_t nowNsM = qpcNs();
                                uint64_t lastNsM = 0;
                                float dtV = 0.0f, alphaV = 1.0f;
                                // [I] MouseService (f at +0x21C): resolve via
                                // DataModel children like velsample does;
                                // retried at most once per second.
                                if (!mouseSvc && nowTick - mouseSvcRetry >= 1000) {
                                    mouseSvcRetry = nowTick;
                                    uint64_t dmS = 0;
                                    if (sdk.dataModel(dmS)) {
                                        sdk.walkChildren(dmS, [&](uint64_t c) {
                                            if (sdk.readName(c) == "MouseService") {
                                                mouseSvc = c;
                                                return false;
                                            }
                                            return true;
                                        });
                                    }
                                    diagRate("aim-mouse-svc", 2000,
                                             "MouseService resolve: %s (%p)",
                                             mouseSvc ? "ok" : "not found",
                                             (void*)mouseSvc);
                                }
                                if (velextActGate(nowNsM, &lastNsM, &dtV, &alphaV,
                                                  cfgAimSmoothing)) {
                                    float eX = (float)bestTarget.sx - (float)ccx;
                                    float eY = (float)bestTarget.sy - (float)ccy;
                                    float distPx = sqrtf(eX * eX + eY * eY);
                                    float yawDeg = 0, pitchDeg = 0;
                                    bool haveAng = false;
                                    float cre[9]{};
                                    if (camAddr &&
                                        m.read(camAddr + (uint64_t)o.cameraRotation, cre))
                                        haveAng = camYawPitchDeg(cre, &yawDeg, &pitchDeg);
                                    bool bpBlock = false;
                                    float bpDY = 0, bpDP = 0, bpAgeMs = 0;
                                    if (bpInFlight && haveAng) {
                                        // [P] velext back-pressure: hold the next
                                        // step until the camera moved >0.02 deg
                                        // on either axis, or 60 ms passed.
                                        bpDY = fabsf(wrap180f(yawDeg - bpYawDeg));
                                        bpDP = fabsf(wrap180f(pitchDeg - bpPitchDeg));
                                        bpAgeMs = (float)((nowNsM - bpStampNs) * 1e-6);
                                        if (bpDY <= kVelextBpMoveDeg &&
                                            bpDP <= kVelextBpMoveDeg &&
                                            (nowNsM - bpStampNs) < kVelextBpTimeoutNs)
                                            bpBlock = true;
                                    }
                                    // [I] Phase C mouse-lock guard (primary):
                                    // the game hides the cursor while it holds
                                    // mouse lock; a visible cursor means
                                    // third-person free-look - skip inject.
                                    bpCursorHold = !mouseCursorHidden();
                                    if (bpCursorHold) {
                                        bpStaleCount = 0;
                                        bpStaleHold = false;
                                    }
                                    // [I] Phase C mouse-lock guard (secondary):
                                    // a back-pressure release with no camera
                                    // movement means the counts did nothing.
                                    // N in a row => injects are held; a single
                                    // probe every 750 ms checks for relock.
                                    if (bpInFlight && haveAng && !bpBlock &&
                                        !bpCursorHold) {
                                        if (bpDY > kVelextBpMoveDeg ||
                                            bpDP > kVelextBpMoveDeg) {
                                            bpStaleCount = 0;
                                            if (bpStaleHold) {
                                                bpStaleHold = false;
                                                diagRate("aim-lock-regained", 1000,
                                                         "counts move the camera again "
                                                         "(dYaw=%.3f dPitch=%.3f) - "
                                                         "resuming injects", bpDY, bpDP);
                                            }
                                        } else if (!bpStaleHold) {
                                            if (bpStaleCount < 1000000)
                                                ++bpStaleCount;
                                            if (bpStaleCount >= kVelextBpStaleMax) {
                                                bpStaleHold = true;
                                                bpProbeNs = nowNsM;
                                                diagRate("aim-stale-hold", 1000,
                                                         "no mouse lock: %d releases "
                                                         "without camera move "
                                                         "(dYaw=%.3f dPitch=%.3f)",
                                                         bpStaleCount, bpDY, bpDP);
                                            }
                                        }
                                    }
                                    bool probeDue = bpStaleHold &&
                                        (nowNsM - bpProbeNs >= kVelextBpProbeNs);
                                    if (bpCursorHold) {
                                        bpInFlight = false; // nothing in flight
                                        diagRate("aim-cursor-hold", 2000,
                                                 "mouse not locked (cursor visible) - "
                                                 "inject skipped, err=%.1f px", distPx);
                                    } else if (bpStaleHold && !probeDue) {
                                        bpInFlight = false;
                                        diagRate("aim-stale-hold", 2000,
                                                 "no mouse lock (no camera move) - "
                                                 "inject held, probe in %.0f ms",
                                                 (float)((kVelextBpProbeNs -
                                                          (nowNsM - bpProbeNs)) * 1e-6));
                                    } else if (bpBlock) {
                                        diagRate("aim-bp", 500,
                                                 "mouse back-pressure hold: dYaw=%.3f dPitch=%.3f age=%.1fms",
                                                 bpDY, bpDP, bpAgeMs);
                                    } else if (alphaV <= kVelextAlphaEps) {
                                        // [P] 0x1402B7FAC: too small to matter
                                    } else if (distPx <= kVelextMouseDzPx) {
                                        // [P] 0x1402B8144: 1.5 px deadzone
                                    } else {
                                        if (probeDue) bpProbeNs = nowNsM;
                                        bpInFlight = false; // step released
                                        float stepX = 0.0f, stepY = 0.0f;
                                        bool mapped = false;
                                        // [P] Phase C: real velext mapping.
                                        // sens read is the fn_14010d580
                                        // chain; focal needs a sane FOV and
                                        // the fresh camera read (for p).
                                        VelextSens vs = readVelextSens(m, sdk.base, mouseSvc);
                                        float focal = 0.0f;
                                        if (vs.ok && camAddr && o.cameraFOV) {
                                            float fovV = 0.0f;
                                            if (m.read(camAddr + (uint64_t)o.cameraFOV, fovV) &&
                                                std::isfinite(fovV) &&
                                                fovV > 10.0f && fovV < 150.0f) {
                                                float tanHalf = tanf(fovV * 0.5f * kDeg2Rad);
                                                if (tanHalf > 1e-6f)
                                                    focal = ((float)vh * 0.5f) / tanHalf;
                                            }
                                        }
                                        if (vs.ok && focal > 0.0f && haveAng) {
                                            // p = pitch component (cre[5]);
                                            // its sign cancels in 1-p*p.
                                            velextMouseCounts(eX, eY, focal, cre[5],
                                                              vs.out1, vs.out2,
                                                              alphaV, dtV,
                                                              &stepX, &stepY);
                                            mapped = true;
                                        }
                                        if (!mapped) {
                                            // [I] fallback: legacy linearized
                                            // aim.sens/aim.gamesens gain.
                                            diagRate("aim-map-fallback", 3000,
                                                     "velext mapping unusable (sens=%d "
                                                     "focal=%.1f cre=%d) - legacy gain %.3f",
                                                     vs.ok ? 1 : 0, focal,
                                                     haveAng ? 1 : 0, outGain);
                                            float lim = kVelextStepCapPxS * alphaV;
                                            if (lim < kVelextStepCapLo) lim = kVelextStepCapLo;
                                            if (lim > kVelextStepCapHi) lim = kVelextStepCapHi;
                                            stepX = eX * alphaV * outGain;
                                            stepY = eY * alphaV * outGain;
                                            if (stepX > lim) stepX = lim;
                                            if (stepX < -lim) stepX = -lim;
                                            if (stepY > lim) stepY = lim;
                                            if (stepY < -lim) stepY = -lim;
                                        }
                                        accX += stepX;
                                        accY += stepY;
                                        int mx = (int)accX;
                                        int my = (int)accY;
                                        accX -= (float)mx;
                                        accY -= (float)my;
                                        if (mx || my) {
                                            sendRelMouse(mx, my);
                                            if (haveAng) {
                                                bpInFlight = true;
                                                bpStampNs = nowNsM;
                                                bpYawDeg = yawDeg;
                                                bpPitchDeg = pitchDeg;
                                            }
                                        }
                                    }
                                }
                            } else {
                            bool resetState = (!emaInit || emaPrim != tgtKey ||
                                                !mouseCtl.active);
                            if (resetState) {
                                emaTargetX = (float)bestTarget.sx;
                                emaTargetY = (float)bestTarget.sy;
                                emaPrim = tgtKey;
                                emaInit = true;
                                mouseCtl = MouseCtlState{};
                                mouseCtl.active = true;
                                mouseCtl.lastEmaX = emaTargetX;
                                mouseCtl.lastEmaY = emaTargetY;
                                mouseCtl.lastEmaInit = true;
                                accX = 0.0f;
                                accY = 0.0f;
                            }
                                if (tsmooth < 0.0f) tsmooth = 0.0f;
                                if (tsmooth > 0.99f) tsmooth = 0.99f;
                                float tauPos = 0.025f + 0.080f * tsmooth;
                                float smoothDiv =
                                    (float)(cfgAimSmooth < 1 ? 1 : cfgAimSmooth);
                                float kp = 6.0f / sqrtf(smoothDiv);
                                float ki = 0.0f;
                                const float iMax = 120.0f;
                                const float vMax = 4500.0f;
                                float outGain = cfgAimSens /
                                    (cfgAimGameSens > 0.05f
                                         ? cfgAimGameSens
                                         : 0.05f);
                                if (outGain < 0.1f) outGain = 0.1f;
                                if (outGain > 4.0f) outGain = 4.0f;

                                float a = dt / (tauPos + dt);
                                float b = 0.8f * a * a;
                                float predX = emaTargetX + mouseCtl.abVX * dt;
                                float predY = emaTargetY + mouseCtl.abVY * dt;
                                float inX = (float)bestTarget.sx - predX;
                                float inY = (float)bestTarget.sy - predY;
                                emaTargetX = predX + a * inX;
                                emaTargetY = predY + a * inY;
                                if (dt > 1e-5f) {
                                    mouseCtl.abVX += (b / dt) * inX;
                                    mouseCtl.abVY += (b / dt) * inY;
                                }

                                // A/B branch: same viewport-center error as
                                // 1-pole (crosshair == viewport center, NOT
                                // OS cursor). Keeps EMA/alpha-beta filter so
                                // aim.pid=0 vs 1 compares filter only.
                                float eX = emaTargetX - (float)ccx;
                                float eY = emaTargetY - (float)ccy;
                                float errMag = sqrtf(eX * eX + eY * eY);
                                float velMag = sqrtf(mouseCtl.abVX * mouseCtl.abVX +
                                                     mouseCtl.abVY * mouseCtl.abVY);

                                float targetMoved = 0.0f;
                                if (mouseCtl.lastEmaInit) {
                                    float targetMovedX = emaTargetX -
                                                         mouseCtl.lastEmaX;
                                    float targetMovedY = emaTargetY -
                                                         mouseCtl.lastEmaY;
                                    targetMoved = sqrtf(targetMovedX * targetMovedX +
                                                        targetMovedY * targetMovedY);
                                }
                                mouseCtl.lastEmaX = emaTargetX;
                                mouseCtl.lastEmaY = emaTargetY;
                                mouseCtl.lastEmaInit = true;

                                if (!mouseCtl.settled) {
                                    if (errMag < 0.45f && velMag < 5.0f &&
                                        fabsf(inX) < 0.8f &&
                                        fabsf(inY) < 0.8f &&
                                        targetMoved < 1.0f) {
                                        mouseCtl.idleT += dt;
                                        if (mouseCtl.idleT > 0.05f)
                                            mouseCtl.settled = true;
                                    } else {
                                        mouseCtl.idleT = 0.0f;
                                    }
                                } else if (errMag > 1.10f || velMag > 14.0f ||
                                           fabsf(inX) > 1.6f ||
                                           fabsf(inY) > 1.6f ||
                                           targetMoved > 2.0f) {
                                    mouseCtl.settled = false;
                                    mouseCtl.idleT = 0.0f;
                                }

                                if (!mouseCtl.settled) {
                                    float nextIX = mouseCtl.iX + eX * dt;
                                    float nextIY = mouseCtl.iY + eY * dt;
                                    mouseCtl.iX = (nextIX > iMax)
                                                      ? iMax
                                                      : ((nextIX < -iMax)
                                                             ? -iMax
                                                             : nextIX);
                                    mouseCtl.iY = (nextIY > iMax)
                                                      ? iMax
                                                      : ((nextIY < -iMax)
                                                             ? -iMax
                                                             : nextIY);
                                    float uX = kp * eX + ki * mouseCtl.iX;
                                    float uY = kp * eY + ki * mouseCtl.iY;
                                    float mag = sqrtf(uX * uX + uY * uY);
                                    if (mag > vMax && mag > 1e-4f) {
                                        float scale = vMax / mag;
                                        uX *= scale;
                                        uY *= scale;
                                        mouseCtl.iX *= 0.98f;
                                        mouseCtl.iY *= 0.98f;
                                    }

                                    accX += uX * dt * outGain;
                                    accY += uY * dt * outGain;
                                    int mx = (int)accX;
                                    int my = (int)accY;
                                    accX -= (float)mx;
                                    accY -= (float)my;
                                    sendRelMouse(mx, my);
                                } else {
                                    accX = 0.0f;
                                    accY = 0.0f;
                                    mouseCtl.iX = 0.0f;
                                    mouseCtl.iY = 0.0f;
                                }
                            }
                        }
                    dl->AddLine(ImVec2((float)ccx, (float)ccy),
                                ImVec2((float)bestTarget.sx, (float)bestTarget.sy),
                                IM_COL32(0, 255, 0, 200), 2.0f);
                    dl->AddCircle(ImVec2((float)bestTarget.sx, (float)bestTarget.sy), 8.0f,
                                  IM_COL32(0, 255, 0, 255), 24);
                    if (bestTarget.radiusPx > 0)
                        dl->AddCircle(ImVec2((float)bestTarget.sx, (float)bestTarget.sy),
                                      bestTarget.radiusPx,
                                      IM_COL32(0, 255, 0, 120), 32);
                }
                // ---- Triggerbot (velext recipe): auto-click when the selected
                // target sits within pad px of center. Non-blocking phases.
                if (!wantTrig && trigPhase) {
                    releaseTriggerIfDown();
                }
                if (wantTrig) {
                    DWORD now2 = GetTickCount();
                    bool centered = haveTarget && bestTarget.distPx <= (float)cfgTrigPad;
                    if (trigPhase == 0) {
                        if (centered &&
                            (now2 - lastFireTick >= (DWORD)cfgTrigCooldown)) {
                            trigPhase = 1;
                            trigT0 = now2;
                        }
                    } else if (trigPhase == 1) {
                        if (!centered) {
                            trigPhase = 0;
                        } else if (now2 - trigT0 >= (DWORD)cfgTrigDelay) {
                            INPUT dn{};
                            dn.type = INPUT_MOUSE;
                            dn.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                            SendInput(1, &dn, sizeof(dn));
                            trigPhase = 2;
                            trigT0 = now2;
                        }
                    } else if (trigPhase == 2) {
                        if (now2 - trigT0 >= (DWORD)cfgTrigPress) {
                            INPUT up{};
                            up.type = INPUT_MOUSE;
                            up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                            SendInput(1, &up, sizeof(up));
                            trigPhase = 0;
                            lastFireTick = now2;
                        }
                    }
                    if (trigPhase) {
                        dl->AddCircle(ImVec2((float)ccx, (float)ccy), 10.0f,
                                      IM_COL32(255, 220, 0, 255), 16);
                    }
                }
            }
        }
        }
        // Viewport-shift reset (mode 2, Foulz reset_vp): restore identity
        // whenever we didn't shift this frame.
                { static DWORD lastHbTick = 0; // hitbox expander: 2Hz re-resolve
          if (nowLoop - lastHbTick >= 500) {
              lastHbTick = nowLoop;
              hitboxTick(m, sdk, o, haveWrite, nowLoop);
          } }
// ---- Autoclicker: fixed-interval clicks for a set burst duration.
        // Independent from triggerbot (don't enable both at once).
        bool wantAC = acOn && cfgACEnabled && fgPid == m.pid && !typingNow;
        DWORD nowAC = GetTickCount();
        if (!wantAC && acPhase) {
            if (acPhase == 2) {
                INPUT up{};
                up.type = INPUT_MOUSE;
                up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                SendInput(1, &up, sizeof(up));
            }
            acPhase = 0;
        }
        if (wantAC) {
            if (cfgACDuration > 0 && nowAC - acT0 >= (DWORD)cfgACDuration) {
                acOn = false; // burst done (also ends toggle)
                if (acPhase == 2) {
                    INPUT up{};
                    up.type = INPUT_MOUSE;
                    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                    SendInput(1, &up, sizeof(up));
                }
                acPhase = 0;
            } else if (acPhase == 2) {
                if (nowAC - acPressT0 >= (DWORD)cfgACPress) {
                    INPUT up{};
                    up.type = INPUT_MOUSE;
                    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                    SendInput(1, &up, sizeof(up));
                    acPhase = 1;
                }
            } else if (nowAC >= acNext) {
                INPUT dn{};
                dn.type = INPUT_MOUSE;
                dn.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                SendInput(1, &dn, sizeof(dn));
                acPhase = 2;
                acPressT0 = nowAC;
                acNext = nowAC + (DWORD)cfgACInterval;
            } else {
                acPhase = 1;
            }
        }
        restoreViewportIfNeeded(false);
        ImGui::Render();
        ImDrawData* dd = ImGui::GetDrawData();
        // Clear to the colorkey: pure black == invisible + click-through.
        const float clear[4] = { 0, 0, 0, 1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        if (dd)
            ImGui_ImplDX11_RenderDrawData(dd);
        g_swap->Present(0, 0);
        { double nq = qpcSeconds(); // pace render/snapshot loop ~144Hz
          while (qpcSeconds() - nq < 0.0059) Sleep(1);
          while (qpcSeconds() - nq < 0.0069) {} }
    }

    releaseTriggerIfDown();
    restoreViewportIfNeeded(true);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(L"ext_ovl", wc.hInstance);
    hitboxRestoreAll(m, o);
    worldRestoreAll(m, o);
    g_writerRun = false; if (writerTh.joinable()) writerTh.join();
    g_srvRun.store(false);
    if (srvTh.joinable()) srvTh.join();
    diagClose();
    printf("overlay exit OK\n");
    return 0;
}
