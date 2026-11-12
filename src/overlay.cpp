// P2.1 overlay: transparent click-through fullscreen HUD (ImGui + D3D11).
// Shows live PlaceId / CamPos / FPS + crosshair. Read-only vs the game.
// Usage: ext_overlay.exe [-t seconds] (auto-quit, for testing). END key quits.
#include "offsets.h"
#include "sdk.h"
#include "ovl/ovl_types.h"
#include "ovl/ovl_ui.h"
#include "ovl/ovl_state.h"
#include "ovl/ovl_tabs.h"
#include "ovl/ovl_cfg.h"
#include "ovl/ovl_server.h"
#include "ovl/ovl_tools.h"
#include "ovl/ovl_core.h"
#include "ovl/ovl_world.h"
#include "ovl/ovl_cache.h"
#include "ovl/ovl_aim.h"
#include "ovl/ovl_view.h"
#include "ovl/ovl_camview.h"
#include "ovl/ovl_esp.h"
#include "ovl/ovl_fly.h"
#include "ovl/ovl_stunt.h"
#include "ovl/ovl_spectate.h"
#include "ovl/ovl_tp.h"
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
// Red footer line when the session is degraded (version mismatch, dead chain).
// Set at startup, never cleared: the user must know writes are off.
char g_cfgWarn[192] = "";




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

double qpcSeconds() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

void sendRelMouse(int mx, int my) {
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


uint64_t qpcNs() { return (uint64_t)(qpcSeconds() * 1e9); }

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
bool velextActGate(uint64_t nowNs, uint64_t* lastNsOut, float* dtOut,
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

float wrap180f(float d) {
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

// Camera yaw/pitch in degrees, velext mouse-path shape (yaw=atan2(-Lx,-Lz),
// pitch=asin(-Ly)) applied to this overlay's calib-proven look extraction
// look = -(m[2],m[5],m[8]). Only used for back-pressure deltas, so the sign
// convention does not matter.
bool camYawPitchDeg(const float* m9, float* yawDeg, float* pitchDeg) {
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
// (VelextSens lives in ovl_aim.h since S3.3a.)

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
VelextSens readVelextSens(Memory& mem, uint64_t base, uint64_t msvc) {
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
void velextMouseCounts(float eX, float eY, float focal, float p,
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
bool mouseCursorHidden() {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return false;
    return (ci.flags & CURSOR_SHOWING) == 0;
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
 int cfgAimKeyMode = 1;   // 0 = hold key, 1 = toggle
int cfgAimFov = 150;     // px radius around viewport center
 int cfgAimSmooth = 1;    // snap divisor (velext menu: 1.0)
 int cfgAimMode = 0;      // 0 = mouse move, 1 = memory write
 int cfgAimSticky = 0;    // menu: off (first-match is stable alone)
 float cfgAimStickTime = 4.0f; // s: max lock age, 0 = unlimited
 int cfgAimPredict = 1;   // lead moving targets by velocity
 float cfgAimPScale = 1.02f; // menu value (lead = vel * dt * scale)
 float cfgAimDynK = 0.42f;  // velext: dynamic FOV radius = max(2, k*d+2)
 int cfgAimNearest = 0;     // 0 = first-match (velext), 1 = nearest
 float cfgAimOverextend = 0.0f; // branch B: aim past head, 0.45 to mirror
 float cfgAimHeight = 1.5f; // aim point above HRP (studs; head~2.8)
 float cfgAimMaxDist = 2000.0f; // studs, 0 = unlimited
 float cfgAimDeadzone = 3.0f;   // px: don't actuate inside this
 int cfgAimIgnoreTeam = 0;  // menu: off
 int cfgAimInterval = 0;      // ms: min gap between actuations (0 = every frame)
 int cfgAimBone = 0;          // 0 = Head, 1 = HRP+height, 2-5 limbs
 int cfgAimNPCs = 1;          // also target NPC/mob Models (orange)
 float cfgAimTSmooth = 0.12f; // LEGACY (aim.pid=1): 1-pole t = 1-this
 float cfgAimSens = 1.0f;     // extra gain
 float cfgAimGameSens = 1.0f; // px per mouse count (calib F8)
 int cfgAimPid = 0;           // 0 = velext filter (default), 1 = legacy PID/AB filters
 float cfgAimMemSmooth = 0.18f; // LEGACY (aim.pid=1): rotation slerp smoothing
float cfgAimSmoothing = 5.0f;  // velext model: tau = 0.008*max(1,s) s; <=1 snaps (1..20)
 int cfgAimRate = 500; // mode-1 writer thread poll Hz (legacy path writes at this rate)
// ---- triggerbot (velext trig.* recipe: auto-click when target centered)
int cfgTrigEnabled = 0;
int cfgTrigKey = 0x06;       // XBUTTON2 like velext default
int cfgTrigKeyMode = 1;      // 0 = hold, 1 = toggle
 int cfgTrigDelay = 45;       // ms: arm delay before DOWN
 int cfgTrigCooldown = 90;    // ms: gap between shots
 int cfgTrigPress = 35;       // ms: DOWN-hold duration
 int cfgTrigPad = 2;
// ---- autoclicker: fixed-interval clicks for a set burst duration ----
int cfgACEnabled = 0;
int cfgACKey = 'V';
 int cfgACKeyMode = 1;    // 0 = hold, 1 = toggle
 int cfgACInterval = 100; // ms between click starts
 int cfgACDuration = 2000; // ms per activation, 0 = until key off
 int cfgACPress = 35;     // ms: DOWN-hold duration          // px: fire when target within this
 int cfgACMode = 0;       // 0 = mouse LEFT click, 1 = keyboard key
 int cfgACKbKey = 0x20;   // vk sent in keyboard mode (default Space)
 int cfgChatGuard = 1;      // Exunys typing check: no keys/aim in chat
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
 int cfgPlySdnKey = 0xDB;     // '[' : speed -step
 int cfgPlySupKey = 0xDD;     // ']' : speed +step
 int cfgPlyJdnKey = 0xBA;     // ';' : jump -step
 int cfgPlyJupKey = 0xDE;     // '\'' : jump +step
int cfgFlyEnabled = 0;       // fly (high-rate velocity writer, see ovl_fly.cpp)
int cfgFlyKey = 'F';         // toggle fly

// ---- Stunt tab (index 8): visible root choreography, see ovl_stunt.cpp ----
// cfg is the only source of truth; the main loop copies it into g_stuntParams
// and hands that to the writer thread (the tab itself never writes memory).
int cfgStuntEnabled = 0;
int cfgStuntPreset = 0;      // index into Stunt::presetName(); 14..16 = combos
float cfgStuntSpeed = 1.0f;  // timeScale 0.25..2
float cfgStuntAmp = 1.0f;    // 0.1..3
int cfgStuntRagdoll = 0;     // 1 = PlatformStand for every preset
int cfgStuntLoop = 1;
int cfgStuntHover = 0;       // 1 = pin velocity.y to 0
int cfgStuntKey = 0;         // 0 = hold-to-run via the tab's Run button
int cfgStuntMode = 0;        // 0 = dynamic, 1 = kinematic (stunt.mode)
int g_stuntActive = 0;       // hotkey state, session only (like g_flyActive)
int g_stuntHold = 0;         // tab "Run (hold)" button, only used when key=0
uint64_t g_stuntTargetPrim = 0; // piggy/orbit/tornado target HRP primitive
StuntParams g_stuntParams;
static bool g_prevStuntKey = false;

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
 int cfgMenuCollapsed = 0;     // 1 = collapsed to the title bar
int cfgMenuTab = 0;           // active tab (0 Body 1 Combat 2 Visual 3 TP 4 World 5 Server 6 Tools 7 Misc 8 Stunt)
uint64_t g_tpTargetPrim = 0;  // Teleport tab: target HRP, consumed by loop
DWORD g_tpRequestTick = 0;    // when that click happened
int cfgTpNpc = 0;             // Teleport tab: also list NPCs/mobs
int cfgTpSpeed = 300;         // Teleport: gradual-mode speed (studs/s)
int cfgTpInstant = 1;         // Teleport: 1 = Piggyback snap (default)
// Spectate (Teleport tab): the tab only sets a request; the main loop resolves
// the target and publishes it to ovl_spectate. Session-only, never in cfg.
int g_specWant = 0;           // 1 = follow the key below
int g_specIsPlayer = 1;       // 1 = key is Player, 0 = NPC Model
uint64_t g_specKey = 0;
char g_specName[48] = { 0 };
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
// Waypoint highlighter (Teleport tab). Drawn by ovl_esp.cpp from the same
// view matrix as the ESP, but independent of the ESP master switch.
int cfgWpHl = 1;
int cfgWpHlColor = 0x35D8FF; // cyan, distinct from the ESP red/green/orange
int cfgWpHlMax = 0;          // studs, 0 = no limit
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
 float g_menuW = 360.0f;       // last drawn panel size (screen bounds check)
 float g_menuH = 420.0f;
 float g_savedMenuW = 0.0f;    // size already stored in the cfg
 float g_savedMenuH = 0.0f;
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

// ---- fly: the writer thread lives in ovl/ovl_fly.cpp (Body tab) ------------
// The main loop only calls Fly::publish() once per frame; the actual velocity
// writes happen in Fly::threadBody() because the engine re-derives the
// primitive velocity every rendered frame (measurements + rationale there).

void loadHitboxProfile(uint64_t place) {
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

void hitboxRestoreAll(Memory& m, const Offsets& o) {
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
std::mutex g_snapMu;
AimSnap g_snap;
static std::atomic<bool> g_writerRun{ true };
static std::atomic<long> g_writerCount{ 0 };
std::atomic<uint64_t> g_snapEpoch{ 1 };
std::atomic<uint64_t> g_snapSeq{ 0 };
static std::atomic<uint64_t> g_currentCam{ 0 };
static int g_writeRateHz = 500; // from aim.rate (main sets at startup)

void invalidateAimSnapshot() {
    std::lock_guard<std::mutex> lk(g_snapMu);
    g_snapEpoch.fetch_add(1, std::memory_order_acq_rel);
    g_snap.ready = false;
    g_snap.epoch = g_snapEpoch.load(std::memory_order_relaxed);
    g_snap.camAddr = 0;
}
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

void diagRate(const char* key, DWORD intervalMs, const char* fmt, ...) {
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
    // 0 = the dump had no Camera.Rotation. Never write 9 floats at a guessed
    // offset: 36 bytes at the wrong place overwrite Camera.Position and the
    // game camera stops responding (frozen view, dead mouse look).
    if (rotOff <= 0) {
        diagRate("writer-disabled", 0,
                 "writer disabled: Camera.Rotation unresolved (off=%d)", rotOff);
        timeEndPeriod(1);
        return;
    }
    double prevQ = qpcSeconds();
    uint64_t lastSeqApplied = 0;
    int revertStreak = 0;
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
                                    if (pm->write(s.camAddr + (uint64_t)rotOff, tar)) {
                                        g_writerCount.fetch_add(1, std::memory_order_relaxed);
                                        // Did the write actually land? A stale
                                        // offset (or a protected page) leaves
                                        // memory bit-identical to what we read
                                        // before the write: the camera then
                                        // never moves while the aim "writes".
                                        float back[9]{};
                                        bool same = pm->read(s.camAddr + (uint64_t)rotOff, back);
                                        for (int bi = 0; bi < 9 && same; ++bi)
                                            if (back[bi] != cur[bi]) same = false;
                                        if (same) {
                                            if (++revertStreak >= 60)
                                                diagRate("writer-reverted", 2000,
                                                         "writer write does not land: "
                                                         "cam=%p off=0x%X (%d writes reverted)",
                                                         (void*)s.camAddr, rotOff,
                                                         revertStreak);
                                        } else {
                                            revertStreak = 0;
                                        }
                                    } else {
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

    printf("\n[6] camera-derived projection (velext v1.0.2 FUN_7ff7ddc3a120)\n");
    {
        int nProjFail = 0;
        const float D2R = 0.0174532925f;
        // CFrame basis, columns Right / Up / Back(-look): identity look = -Z.
        const float rotId[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        CamView cv;
        Vec3 eye{ 0, 0, 0 };
        bool built = camViewBuild(cv, rotId, eye, 70.0f);
        float pm[16]{};
        bool madeM = built && camViewMatrix(cv, 1600.0f / 900.0f, pm);
        if (!built || !madeM) {
            printf("  build/matrix FAILED (built=%d matrix=%d)\n", built ? 1 : 0,
                   madeM ? 1 : 0);
            ++nProjFail;
        } else {
            const float tanHalf = tanf(70.0f * 0.5f * D2R);
            const float aspect = 1600.0f / 900.0f;
            int sx = 0, sy = 0;
            // straight ahead -> centre
            bool c1 = screenW2S(Vec3{ 0, 0, -10 }, pm, sx, sy, 0, 0, 1600, 900,
                                1600, 900);
            bool ok1 = c1 && abs(sx - 800) <= 2 && abs(sy - 450) <= 2;
            // right edge: x = tanHalf*aspect*depth
            bool c2 = screenW2S(Vec3{ tanHalf * aspect * 10, 0, -10 }, pm, sx, sy,
                                0, 0, 1600, 900, 1600, 900);
            bool ok2 = c2 && abs(sx - 1600) <= 3 && abs(sy - 450) <= 3;
            // top edge: y = tanHalf*depth
            bool c3 = screenW2S(Vec3{ 0, tanHalf * 10, -10 }, pm, sx, sy, 0, 0,
                                1600, 900, 1600, 900);
            bool ok3 = c3 && abs(sy) <= 3 && abs(sx - 800) <= 3;
            // behind the camera -> rejected
            bool ok4 = !screenW2S(Vec3{ 0, 0, 10 }, pm, sx, sy, 0, 0, 1600, 900,
                                  1600, 900);
            // client-rect origin is honoured (windowed game)
            bool c5 = screenW2S(Vec3{ 0, 0, -10 }, pm, sx, sy, 100, 50, 1600,
                                900, 1600, 900);
            bool ok5 = c5 && abs(sx - 900) <= 2 && abs(sy - 500) <= 2;
            // a stale constant blob (measured VE+432) must fail the gate
            float dead[16]{};
            dead[3] = 0.0f;
            dead[7] = 0.0f;
            dead[11] = 0.0f;
            dead[15] = 0.0998f;
            bool ok6 = !camViewMatrixSane(dead, Vec3{ 1, 2, 3 }, 1600, 900);
            // a matrix that is orthonormal but not a projection is also refused
            // (4x4 identity: no forward row at all)
            const float ident4[16] = { 1, 0, 0, 0, 0, 1, 0, 0,
                                       0, 0, 1, 0, 0, 0, 0, 1 };
            bool ok7 = !camViewMatrixSane(ident4, Vec3{ 0, 0, 0 }, 1600, 900);
            // FieldOfView unit trap: 1.22173 is 70 deg in RADIANS (f0ea5da9 /
            // 11f155f3 store radians). The projection must be identical to the
            // degrees build; without the conversion everything lands ~66x off
            // screen, which is the "ESP dead" failure this test locks down.
            CamView cvRad;
            float rm[16]{};
            bool ok8 = camViewBuild(cvRad, rotId, eye, camFovDegrees(1.22173f)) &&
                       camViewMatrix(cvRad, aspect, rm);
            for (int i = 0; i < 16 && ok8; ++i)
                if (fabsf(rm[i] - pm[i]) > 1e-5f) ok8 = false;
            // A real view*projection (the live f0ea5da9 shape) must pass the
            // gate, must be refused once the camera has moved, must agree with
            // the camera basis, and must disagree when the camera turns.
            bool ok9 = camViewMatrixSane(pm, eye, 1600, 900);
            bool ok10 = !camViewMatrixSane(pm, Vec3{ 0, 30, 0 }, 1600, 900);
            bool ok11 = camViewMatrixAgrees(pm, cv);
            CamView cvTurn = cv;
            cvTurn.fwd = Vec3{ 1, 0, 0 };
            bool ok12 = !camViewMatrixAgrees(pm, cvTurn);
            printf("  centre=%d edgeX=%d edgeY=%d behind-rejected=%d "
                   "client-origin=%d dead-matrix-rejected=%d\n",
                   ok1 ? 1 : 0, ok2 ? 1 : 0, ok3 ? 1 : 0, ok4 ? 1 : 0,
                   ok5 ? 1 : 0, ok6 ? 1 : 0);
            printf("  fov-radians=%d vp-accepted=%d vp-moved-rejected=%d "
                   "vp-agrees=%d vp-turned-rejected=%d (stale-identity=%d)\n",
                   ok8 ? 1 : 0, ok9 ? 1 : 0, ok10 ? 1 : 0, ok11 ? 1 : 0,
                   ok12 ? 1 : 0, ok7 ? 1 : 0);
            if (!ok1 || !ok2 || !ok3 || !ok4 || !ok5 || !ok6 || !ok7 || !ok8 ||
                !ok9 || !ok10 || !ok11 || !ok12) {
                printf("  [6] FAILED (sanity matrix accepted=%d)\n", ok7 ? 1 : 0);
                ++nProjFail;
            } else {
                printf("  [6] projection PASS (fov70 aspect16:9 tanHalf=%.6f)\n",
                       (double)tanHalf);
            }
        }
        if (nProjFail) {
            printf("\n=== done (FAIL) ===\n");
            return 1;
        }
    }

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

// ---- startup self-check ----------------------------------------------------
// Walks exactly the chains every tab depends on and reports the first broken
// step. Runs once at launch (and via -verify) so "overlay opens but every tab is
// empty" becomes a one-line diagnosis instead of a guessing session.
static bool chainSelfCheck(Memory& m, SDK& sdk, const Offsets& o, bool verbose) {
    uint64_t dm = 0, ws = 0, cam = 0, players = 0, local = 0;
    uint64_t model = 0, hum = 0, root = 0, prim = 0, place = 0;
    bool okDm  = sdk.dataModel(dm);
    bool okWs  = okDm && sdk.workspace(dm, ws);
    bool okCam = okWs && sdk.camera(ws, cam);
    bool okPl  = okDm && sdk.findChildByClass(dm, "Players", players);
    bool okLoc = okPl && m.read(players + (uint64_t)o.playersLocalPlayer, local) &&
                 SDK::plausiblePtr(local);
    if (okDm) sdk.placeId(dm, place);
    bool okModel = okLoc && m.read(local + (uint64_t)o.playerModelInstance, model) &&
                   SDK::plausiblePtr(model);
    bool okHum = false, okRoot = false, okPrim = false;
    if (okModel) {
        sdk.walkChildren(model, [&](uint64_t c) {
            if (sdk.readClass(c) == "Humanoid") { hum = c; okHum = true; }
            if (sdk.readName(c) == "HumanoidRootPart") { root = c; okRoot = true; }
            return true;
        });
        if (okRoot)
            okPrim = m.read(root + (uint64_t)o.basePartPrimitive, prim) &&
                     SDK::plausiblePtr(prim);
    }
    // Required: without these the overlay is an empty shell. model/hrp/prim can
    // legitimately be absent for a second after a join or respawn -> warned only.
    bool required = okDm && okWs && okPl && okLoc && place != 0;
    if (!verbose) return required;
    auto st = [](bool b) { return b ? "OK" : "FAIL"; };
    printf("chain dm=%s ws=%s cam=%s players=%s local=%s model=%s humanoid=%s "
           "hrp=%s prim=%s place=%llu\n",
           st(okDm), st(okWs), st(okCam), st(okPl), st(okLoc), st(okModel),
           st(okHum), st(okRoot), st(okPrim), (unsigned long long)place);
    if (!required) {
        const char* at = !okDm ? "DataModel" : !okWs ? "Workspace"
                       : !okPl ? "Players" : !okLoc ? "LocalPlayer"
                       : "PlaceId";
        printf("chain FAIL at %s (dm=0x%llx ws=0x%llx players=0x%llx local=0x%llx "
               "placeId@0x%x)\n",
               at, (unsigned long long)dm, (unsigned long long)ws,
               (unsigned long long)players, (unsigned long long)local,
               o.dataModelPlaceId);
    } else if (!(okCam && okModel && okHum && okRoot && okPrim)) {
        printf("chain NOTE: entity parts not ready yet (loading/respawn) - "
               "ESP/aim fill in after the character streams in\n");
    }
    if (sdk.dmMismatch)
        printf("chain NOTE: the two DataModel routes disagree - dump/offset drift\n");
    return required;
}

// -verify: same checks without the UI, usable while the overlay is running.
static int runVerify() {
    SetConsoleOutputCP(CP_UTF8);
    initExeDir();
    Memory m;
    if (!m.findProcess(L"RobloxPlayerBeta.exe")) {
        printf("FAIL: cannot find game process\n");
        return 1;
    }
    if (!m.open()) {
        printf("FAIL: cannot open game process (err=%lu)\n", GetLastError());
        return 1;
    }
    std::string running = m.exeVersion();
    Offsets o;
    std::string log, cache = exePath("offsets_cache.json"),
                good = exePath("offsets_good.json");
    if (!OffsetManager::ensure(running, cache, o, log, good)) {
        printf("%sFAIL (offsets)\n", log.c_str());
        return 2;
    }
    printf("%s", log.c_str());
    if (!running.empty() && !o.robloxVersion.empty() && running != o.robloxVersion)
        printf("WARN: offsets %s != running %s (write features would be blocked)\n",
               o.robloxVersion.c_str(), running.c_str());
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) {
        printf("FAIL: module base\n");
        return 2;
    }
    bool ok = chainSelfCheck(m, sdk, o, true);
    printf("verify: %s (running=%s offsets=%s src=%s hash=%016llx)\n",
           ok ? "PASS" : "FAIL", running.c_str(), o.robloxVersion.c_str(),
           o.source.c_str(), (unsigned long long)o.srcHash);
    return ok ? 0 : 2;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    initExeDir(); // side files resolve next to the exe, never the CWD
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-aimtest")) return runAimSelfTest();
        // -bindtest: keybind capture state machine (stuck-key regression),
        // no game, no offsets, no window.
        if (!strcmp(argv[i], "-bindtest")) return runBindSelfTest();
        // -stunttest: preset math + restore plan, no game, no offsets needed.
        if (!strcmp(argv[i], "-stunttest")) return runStuntSelfTest();
        // -verify: offsets + live pointer chain, no UI, no singleton. The gate
        // for "did this Roblox update break anything?" - exit 0 = PASS, 2 = FAIL.
        if (!strcmp(argv[i], "-verify")) return runVerify();
        // -offdump <file.json>: print every parsed offset (used to bless a new
        // fixture). -offtest [dir]: parse all fixtures, compare with .expect.
        if (!strcmp(argv[i], "-offdump")) {
            if (i + 1 >= argc) {
                printf("usage: -offdump <offsets.json>\n");
                return 1;
            }
            return runOffDump(argv[++i]);
        }
        if (!strcmp(argv[i], "-offtest")) {
            std::string dir = (i + 1 < argc && argv[i + 1][0] != '-')
                                  ? argv[++i] : "offsets_fixtures";
            std::string log;
            int rc = runOffTest(dir, log);
            printf("%s", log.c_str());
            return rc;
        }
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
    // Read-only first: the running build id decides whether writing is allowed
    // at all. Never take a write handle before the offsets version is known.
    if (!m.open()) {
        printf("FAIL: cannot open game process (is Roblox running?)\n");
        printf("closing in 5s...\n");
        fflush(stdout);
        Sleep(5000);
        return 1;
    }
    std::string running = m.exeVersion();
    Offsets o;
    std::string log, cache = exePath("offsets_cache.json"),
                good = exePath("offsets_good.json");
    if (!OffsetManager::ensure(running, cache, o, log, good)) {
        printf("%sFAIL (offsets)\n", log.c_str());
        printf("closing in 5s...\n");
        fflush(stdout);
        Sleep(5000);
        return 1;
    }
    printf("%s", log.c_str());
    printf("build %s %s\n", __DATE__, __TIME__);
    printf("offsets ready: running=%s loaded=%s src=%s hash=%016llx\n",
           running.empty() ? "<unknown>" : running.c_str(),
           o.robloxVersion.empty() ? "<unknown>" : o.robloxVersion.c_str(),
           o.source.c_str(), (unsigned long long)o.srcHash);
    // A dump for another build reads plausible-looking garbage: write features
    // must stay off (Hyperion kicks on the wrong field) while reads can warn.
    bool verMismatch = !running.empty() && !o.robloxVersion.empty() &&
                       running != o.robloxVersion;
    if (verMismatch) {
        printf("WARN: offsets %s != running %s -> READ-ONLY "
               "(no aim-write/mods/fly/teleport/world writes)\n",
               o.robloxVersion.c_str(), running.c_str());
        snprintf(g_cfgWarn, sizeof g_cfgWarn,
                 "offsets %s != game %s: read-only", o.robloxVersion.c_str(),
                 running.c_str());
    }
    // Prefer a write handle (mods + mem aim need it); fall back to read-only.
    bool haveWrite = false;
    if (!verMismatch) {
        m.close();
        haveWrite = m.openWrite();
        if (!haveWrite && !m.open()) {
            printf("FAIL: cannot open game process (is Roblox running?)\n");
            printf("closing in 5s...\n");
            fflush(stdout);
            Sleep(5000);
            return 1;
        }
        if (!haveWrite) {
            printf("NOTE: write handle denied -> aim mode mouse, mods off\n");
            snprintf(g_cfgWarn, sizeof g_cfgWarn, "no write handle: read-only");
        }
    }
    if (!haveWrite && cfgAimMode != 0) cfgAimMode = 0;
    // aim.mode=1 writes 9 rotation floats at Camera.Rotation. If the dump has
    // no such key we must not guess an offset: writing blind at a stale value
    // lands inside Camera.Position and freezes the view. Force the safe mode
    // and make it visible in the footer instead of silently mis-writing.
    if (cfgAimMode == 1 && !o.cameraRotation) {
        cfgAimMode = 0;
        printf("WARN: Camera.Rotation unresolved in this dump -> aim mode mouse "
               "(rotation writes disabled)\n");
        if (!g_cfgWarn[0])
            snprintf(g_cfgWarn, sizeof g_cfgWarn,
                     "Camera.Rotation missing: aim mode forced to mouse");
    }
    fflush(stdout);
    SDK sdk(m, o);
    if (!sdk.moduleBase(L"RobloxPlayerBeta.exe")) {
        printf("FAIL: module base\n");
        return 1;
    }
    // One-shot chain check: prints PASS/FAIL per step, sets the footer warning.
    bool chainOk = chainSelfCheck(m, sdk, o, true);
    if (!chainOk && !g_cfgWarn[0])
        snprintf(g_cfgWarn, sizeof g_cfgWarn, "pointer chain broken: see console");

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
    // Environment stamp: after a bad session this line alone says which build,
    // which dump (hash), whether writes were allowed and whether the chain was
    // alive at launch.
    diagRate("startup-env", 0,
             "build %s %s running=%s offsets=%s src=%s hash=%016llx chain=%s write=%d warn='%s'",
             __DATE__, __TIME__, running.c_str(), o.robloxVersion.c_str(),
             o.source.c_str(), (unsigned long long)o.srcHash,
             chainOk ? "ok" : "FAIL", haveWrite ? 1 : 0, g_cfgWarn);
    std::thread writerTh(writerThread, &m, o.cameraRotation);
    // Body tab fly: dedicated writer thread (see ovl/ovl_fly.cpp).
    if (haveWrite) Fly::start(&m, o.cameraRotation, o.primitiveVelocity);
    // Stunt tab: same writer-thread pattern, parked on Sleep(10) until armed.
    if (haveWrite) Stunt::start(&m, &o);
    // Teleport tab: guided velocity dash (one click, no spam) + spectate
    // (CameraSubject pin). Both park themselves when idle.
    if (haveWrite)
        Tp::start(&m, o.primitivePosition, o.primitiveVelocity,
                  o.primitiveFlags, (uint32_t)o.primitiveCanCollideBit);
    if (haveWrite) Spectate::start(&m, o.cameraSubject);
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
    bool trigOn = false;
    bool acOn = false; // autoclicker toggle/hold state
    int calibPhase = 0; // 0 idle, 1 wait after inject
    double calibT0 = 0;
    float calibYaw0 = 0;
    int calibCounts = 200;
    const int CALIB_VK = 0x77; // F8
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
    std::vector<Ent> ents;
    uint64_t uisAddr = 0; // UserInputService (chat-focus guard)
    EntityCtx entCtx{ m, sdk, o, ents, uisAddr, stickPlayer };
    EspCtx espCtx{ m, o, ents, vpx, vpy, vpw, vph, sw, sh, camAddr };
    AimCtx aimCtx{ m, sdk, o, ents, haveWrite, camAddr, cam,
                   vpx, vpy, vpw, vph, sw, sh,
                   aimOn, trigOn, acOn, stickPlayer };
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
        // A capture in progress owns the keyboard: no hotkey (hide/show, panic,
        // aim, trigger, autoclick, fly/speed/jump) may fire until it ends, or
        // binding the panel key would hide the panel mid-capture.
        if (g_capVk) typingNow = true;
        // Rising edge of the DOWN bit, not the "pressed since last read" low
        // bit. The low bit is shared with the rest of the desktop (the game
        // polls keys too, and the macro recorder sweeps all 256 VKs while it
        // records), so an H press was eaten roughly every other time and the
        // panel stopped hiding/showing. Same pattern as speed/jump/fly above.
        static int prevToggle = -1; // -1 = first frame, never a rising edge
        int toggleDown =
            (g_toggleVk && (GetAsyncKeyState(g_toggleVk) & 0x8000)) ? 1 : 0;
        if (!typingNow && toggleDown && prevToggle == 0) visible = !visible;
        prevToggle = toggleDown;
        // ---- keybind capture: the next key/button press becomes the hotkey ---
        // (the panel cannot take keyboard focus, so ImGui can't do this)
        if (g_capVk) {
            bool down[0x100] = {};
            for (int vk = 1; vk <= 0xFE; ++vk)
                down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
            int nv = 0;
            if (keyBindPoll(down, GetTickCount(), &nv)) {
                const char* ck = g_capCfg ? g_capCfg : "?";
                if (g_capVk) *g_capVk = nv;
                if (g_capCfg) upsertCfgKey(g_capCfg, nv ? keyCfgName(nv).c_str()
                                                        : "NONE");
                if (nv) {
                    toast("key = %s", keyName(nv).c_str());
                    if (g_diagState) {
                        printf("bind %s = 0x%02X (%s)\n", ck, nv,
                               keyName(nv).c_str());
                        fflush(stdout);
                    }
                } else {
                    toast("key unbound");
                    if (g_diagState) {
                        printf("bind %s = NONE\n", ck);
                        fflush(stdout);
                    }
                }
                keyBindStop();
            }
        }
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
            cfgStuntEnabled = 0;
            if (haveWrite) worldRestoreAll(m, o); // lighting/terrain back to normal
            // Stunt owns the pose, the humanoid flags and (ghost) the collision
            // bit: restore + join before anything else touches the character.
            if (haveWrite) Stunt::stop(m, o);
            g_flyActive = 0;
            g_speedActive = 0;
            g_jumpActive = 0;
            g_stuntActive = 0;
            g_stuntHold = 0;
            g_specWant = 0;      // camera back to the local character
            Tp::cancel();        // stop a running dash
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
            aimCtx.onAimDisabled();
            invalidateAimSnapshot();
        }
        if (cfgTrigKeyMode == 0) {
            trigOn = !typingNow && (GetAsyncKeyState(cfgTrigKey) & 0x8000) != 0;
        } else {
            if (!typingNow && (GetAsyncKeyState(cfgTrigKey) & 1)) trigOn = !trigOn;
        }
        if (!trigOn) aimCtx.releaseTriggerIfDown();
        if (cfgACKeyMode == 0) {
            bool heldAC = !typingNow && (GetAsyncKeyState(cfgACKey) & 0x8000) != 0;
            if (heldAC && !acOn) {
                aimCtx.onAutoclickEnabled();
            }
            acOn = heldAC;
        } else {
            if (!typingNow && (GetAsyncKeyState(cfgACKey) & 1)) {
                acOn = !acOn;
                if (acOn) {
                    aimCtx.onAutoclickEnabled();
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

            // Stunt: same rising-edge rule (never the shared &1 low bit). When
            // cfgStuntKey == 0 the tab's Run button holds it instead.
            bool dStunt = cfgStuntKey && (GetAsyncKeyState(cfgStuntKey) & 0x8000) != 0;
            if (dStunt && !g_prevStuntKey && !typingNow && cfgStuntEnabled) {
                g_stuntActive = !g_stuntActive;
                // Stunt wins the velocity channel: drop fly/speed instead of
                // letting three writers fight over the same field.
                if (g_stuntActive) {
                    g_flyActive = 0;
                    g_speedActive = 0;
                }
            }
            g_prevStuntKey = dStunt;
            if (!cfgStuntEnabled) g_stuntActive = 0;
            if (!cfgStuntKey) g_stuntActive = g_stuntHold ? 1 : 0;
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
            if (haveWrite) Fly::killVelocity(m, o);
            if (haveWrite) Stunt::stop(m, o); // pose/flags back before we exit
            if (haveWrite) worldRestoreAll(m, o); // Đợt 2: quit = nothing left on
            mouseClipRestore();
            break;
        }

        // Game closed -> quit with it (no orphan overlay, no key needed).
        if (WaitForSingleObject(m.h, 0) == WAIT_OBJECT_0) {
            aimCtx.releaseTriggerIfDown();
            aimCtx.restoreViewportIfNeeded(true);
            mouseClipRestore();
            if (haveWrite) Fly::killVelocity(m, o);
            if (haveWrite) Stunt::stop(m, o);
            worldRestoreAll(m, o); // Đợt 2: best-effort, the process is going away
            break;
        }

        // Keep gameplay state current even while the HUD is hidden or the
        // game is not foreground, so player mods never use an old entity.
        if (lastEntRefreshTick == 0 ||
            nowLoop - lastEntRefreshTick >= 250) {
            entCtx.refreshEnts();
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
        toolsTick(m, sdk, o, place, srv.jobId, nowLoop);
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
            fgPid == m.pid && !flyApply && !Stunt::active() &&
            o.primitivePosition && o.primitiveVelocity) {
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
        // is gone). One click now starts a guided velocity dash (ovl_tp.cpp):
        // writing Primitive.Position was reverted by the solver in 10-50 ms,
        // which is why the old button needed spam-clicking and jerked back.
        // Fly/stunt own the same velocity field, so they cancel a dash.
        if ((flyApply || Stunt::active()) && Tp::active()) Tp::cancel();
        if (g_tpTargetPrim && haveWrite && !typingNow && !flyApply &&
            !Stunt::active() && o.primitivePosition &&
            nowLoop - g_tpRequestTick < 2000) {
            uint64_t want = g_tpTargetPrim;
            g_tpTargetPrim = 0;
            uint64_t selfPrim = 0;
            Vec3 selfPos{};
            bool selfOk = false;
            Ent* selfEnt = nullptr;
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
                    selfEnt = &e;
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
                uint64_t ghost[1 + 1 + 4];
                int gn = 0;
                if (selfEnt) {
                    if (selfEnt->headPrim) ghost[gn++] = selfEnt->headPrim;
                    for (uint64_t p : selfEnt->limbPrim)
                        if (p) ghost[gn++] = p;
                }
                if (Tp::request(m, o, selfPrim, want, dst,
                                (float)cfgTpSpeed, ghost, gn,
                                cfgTpInstant != 0)) {
                    snprintf(tpMsg, sizeof tpMsg, "tp -> %s (%.0fm)",
                             ents[best].tag.c_str(), d);
                    tpMsgTick = nowLoop;
                    diagRate("teleport", 0,
                             "tp to %s dist=%.1f speed=%d",
                             ents[best].tag.c_str(), d, cfgTpSpeed);
                } else {
                    snprintf(tpMsg, sizeof tpMsg, "tp: writer unavailable");
                    tpMsgTick = nowLoop;
                }
            }
        }
        // ---- Teleport to a saved waypoint (Teleport tab, "wp: TP") ----
        if (g_tpPosTick && haveWrite && !typingNow && !flyApply &&
            !Stunt::active() && o.primitivePosition &&
            nowLoop - g_tpPosTick < 2000) {
            g_tpPosTick = 0;
            Vec3 dst = g_tpTargetPos;
            uint64_t selfPrim = 0;
            Vec3 selfPos{};
            Ent* selfEnt = nullptr;
            for (auto& e : ents) {
                if (!e.self || !e.prim) continue;
                if (m.read(e.prim + (uint64_t)o.primitivePosition, selfPos) &&
                    std::isfinite(selfPos.x) && std::isfinite(selfPos.y) &&
                    std::isfinite(selfPos.z)) {
                    selfPrim = e.prim;
                    selfEnt = &e;
                }
                break;
            }
            if (!selfPrim) {
                snprintf(tpMsg, sizeof tpMsg, "wp: self not found");
                tpMsgTick = nowLoop;
            } else {
                float dx = dst.x - selfPos.x, dy = dst.y - selfPos.y,
                      dz = dst.z - selfPos.z;
                float d = sqrtf(dx * dx + dy * dy + dz * dz);
                uint64_t ghost[1 + 1 + 4];
                int gn = 0;
                if (selfEnt) {
                    if (selfEnt->headPrim) ghost[gn++] = selfEnt->headPrim;
                    for (uint64_t p : selfEnt->limbPrim)
                        if (p) ghost[gn++] = p;
                }
                if (Tp::request(m, o, selfPrim, 0, dst, (float)cfgTpSpeed,
                                ghost, gn, cfgTpInstant != 0)) {
                    snprintf(tpMsg, sizeof tpMsg, "wp %s (%.0fm)", g_tpPosName, d);
                    tpMsgTick = nowLoop;
                    diagRate("wptp", 0,
                             "waypoint tp %s dist=%.1f speed=%d",
                             g_tpPosName, d, cfgTpSpeed);
                } else {
                    snprintf(tpMsg, sizeof tpMsg, "wp: writer unavailable");
                    tpMsgTick = nowLoop;
                }
            }
        }
        // Report the writer's verdict once (one line in the footer, not a
        // per-frame poll).
        {
            int tr = Tp::takeResult();
            if (tr > 0) {
                snprintf(tpMsg, sizeof tpMsg, "tp: arrived");
                tpMsgTick = nowLoop;
            } else if (tr == -2) {
                // The engine kept the pre-write position: report it instead of
                // the old false "arrived" ("position write rejected").
                snprintf(tpMsg, sizeof tpMsg,
                         "tp: blocked - position write was rejected");
                tpMsgTick = nowLoop;
            } else if (tr < 0) {
                snprintf(tpMsg, sizeof tpMsg, "tp: timeout");
                tpMsgTick = nowLoop;
            }
        }
        // ---- Fly: hand the target to ovl_fly (writer thread) ----------------
        // Fly::publish() is the only thing the main loop does for fly; the
        // per-frame writes that used to live here always lost the race with
        // the engine's velocity refresh (see ovl/ovl_fly.cpp).
        if (flyApply && haveWrite && !typingNow && fgPid == m.pid &&
            !Stunt::active() && o.primitiveVelocity) {
            uint64_t selfPrim = 0;
            for (auto& e : ents) {
                if (!e.self || !e.prim) continue;
                selfPrim = e.prim;
                break;
            }
            Fly::publish(m, o, selfPrim, camAddr, cfgFlySpeed, selfPrim != 0);
        } else {
            Fly::publish(m, o, 0, 0, cfgFlySpeed, false); // falling edge: stop once
        }
        // ---- Stunt: hand the character to ovl_stunt (writer thread) ----------
        // Same contract as fly: the loop only publishes; the writes that can
        // actually beat the engine refresh live in ovl/ovl_stunt.cpp.
        {
            g_stuntParams.preset = cfgStuntPreset;
            g_stuntParams.timeScale = cfgStuntSpeed;
            g_stuntParams.amp = cfgStuntAmp;
            g_stuntParams.ragdoll = cfgStuntRagdoll;
            g_stuntParams.loop = cfgStuntLoop;
            g_stuntParams.hover = cfgStuntHover;
            g_stuntParams.mode = cfgStuntMode;
            g_stuntParams.targetPrim = g_stuntTargetPrim;
            const bool stuntOn = cfgStuntEnabled && g_stuntActive && haveWrite &&
                                 !typingNow && fgPid == m.pid;
            uint64_t selfPrim = 0, selfHum = 0;
            if (stuntOn) {
                for (auto& e : ents) {
                    if (!e.self || !e.prim) continue;
                    selfPrim = e.prim;
                    selfHum = e.humanoid;
                    break;
                }
            }
            Stunt::publish(m, o, selfPrim, selfHum, camAddr, g_stuntParams,
                           stuntOn && selfPrim != 0);
        }
        // Refresh the camera chain every loop, before the hidden-overlay early
        // exit: the spectate writer and Fly keep using camAddr while the panel
        // is hidden, so a stale address after a respawn/map transition would let
        // them write into freed memory.
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

        // ---- Spectate (Teleport tab): pin CameraSubject to another Humanoid ---
        // The game's own camera module reads CameraSubject every RenderStep, so
        // the normal mouse orbit/zoom now centres on the watched character.
        // Kept outside the !wantShow branch: hiding the panel must not drop the
        // camera back to us.
        {
            uint64_t selfHum = 0;
            for (auto& e : ents)
                if (e.self && e.humanoid) { selfHum = e.humanoid; break; }
            if (g_specWant) {
                Ent* tgt = nullptr;
                for (auto& e : ents) {
                    if (g_specIsPlayer) {
                        if (e.player && e.player == g_specKey) { tgt = &e; break; }
                    } else if (e.isNPC && e.model == g_specKey) {
                        tgt = &e;
                        break;
                    }
                }
                if (!haveWrite || !o.cameraSubject) {
                    g_specWant = 0;
                    Spectate::publish(0, 0, selfHum, false);
                    toast("spectate: offset missing (read-only?)");
                } else if (!tgt) {
                    g_specWant = 0;
                    Spectate::publish(camAddr, 0, selfHum, false);
                    toast("spectate: target gone");
                } else if (tgt->self) {
                    g_specWant = 0;
                    Spectate::publish(camAddr, 0, selfHum, false);
                } else if (tgt->humanoid) {
                    Spectate::publish(camAddr, tgt->humanoid, selfHum, true);
                } else {
                    // Mid-respawn: the previous Humanoid address may already be
                    // freed, so fall back to our own subject instead of pinning
                    // a dangling pointer. g_specWant stays set, so the next
                    // frame that resolves a Humanoid re-pins the camera.
                    Spectate::publish(camAddr, 0, selfHum, false);
                }
            } else {
                Spectate::publish(camAddr, 0, selfHum, false);
            }
            if (g_diagState && g_specWant) {
                uint64_t cur = 0;
                if (camAddr && o.cameraSubject)
                    m.read(camAddr + (uint64_t)o.cameraSubject, cur);
                diagRate("spectate", 2000,
                         "spectate name=%s active=%d cam=%p subject=%p now=%p",
                         g_specName, (int)Spectate::active(), (void*)camAddr,
                         (void*)Spectate::subject(), (void*)cur);
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
            aimCtx.releaseTriggerIfDown();
            aimCtx.restoreViewportIfNeeded(true);
            invalidateAimSnapshot();
            Sleep(50);
            continue;
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
                        std::isfinite(vf)) {
                        // radians on recent builds (see camFovDegrees)
                        float d = camFovDegrees(vf);
                        if (d > 10.0f && d < 150.0f) vfovDeg = d;
                    }
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
            // 9 tabs: badge/tools order follows the roadmap
            // (Body, Combat, Visual, Teleport | World, Server, Tools, Misc, Stunt) ----
            {
                static const char* tabNames[9] = { "Body", "Combat", "Visual",
                                                   "Teleport", "World", "Server",
                                                   "Tools", "Misc", "Stunt" };
                for (int t = 0; t < 9; ++t) {
                    if (t == 4 || t == 8)
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
                            aimCtx.dbgEnts, loopMs, nowLoop };
            if      (cfgMenuTab == 0) drawTabBody(tabCtx);
            else if (cfgMenuTab == 1) drawTabCombat(tabCtx);
            else if (cfgMenuTab == 2) drawTabVisual(tabCtx);
            else if (cfgMenuTab == 3) drawTabTeleport(tabCtx);
            else if (cfgMenuTab == 4) drawTabWorld(tabCtx);
            else if (cfgMenuTab == 5) drawTabServer(tabCtx);
            else if (cfgMenuTab == 6) drawTabTools(tabCtx);
            else if (cfgMenuTab == 7) drawTabMisc(tabCtx);
            else                      drawTabStunt(tabCtx);

            // ---- status footer (always visible, outside the tabs) ----
            ImGui::Separator();
            if (g_cfgWarn[0])
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", g_cfgWarn);
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
                           g_colHover ? g_colHover : "-", aimCtx.dbgEnts, g_espVmDiag,
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
        int vmSrc = 0; // 0 none / 1 VisualEngine matrix / 2 camera-derived
        CamView camView;  // diag: the basis the projection was built from
        float camFov = 0.0f;
        {
            // Two candidate sources, in order of exactness:
            //  1) VisualEngine.ViewMatrix - the matrix the renderer itself
            //     uses. Live on f0ea5da9 (verified live: row0 =
            //     1/(aspect*tanHalf) = 0.8673, y row = 1.4281 = 1/tan35,
            //     w row = (0,0,-1) = forward, eye maps to the client centre),
            //     a constant dead blob on 5a21b87b. camViewMatrixSane() plus
            //     camViewMatrixAgrees() decide per build and per frame.
            //  2) Camera.Rotation/Position/FieldOfView, the velext v1.0.2
            //     model (FUN_7ff7ddc3a120).
            // Unit trap: FieldOfView is RADIANS on recent builds (70 deg reads
            // 1.22173). Feeding 1.22 to tanf() as degrees collapses the
            // projection ~66x and throws every box off screen - that is the
            // "ESP does nothing" failure, not the cache.
            int vw = vpw ? vpw : sw, vh = vph ? vph : sh;
            float rot[9]{};
            float fov = 0.0f;
            if (camAddr && o.cameraRotation && o.cameraFOV &&
                m.read(camAddr + (uint64_t)o.cameraRotation, rot) &&
                m.read(camAddr + (uint64_t)o.cameraFOV, fov)) {
                camFov = camFovDegrees(fov);
                camViewBuild(camView, rot, cam, camFov);
            }
            if (m.read(sdk.base + o.visualEnginePtr, ve) && ve &&
                m.read(ve + (uint64_t)o.visualEngineViewMatrix, vm) &&
                camViewMatrixSane(vm, cam, vw, vh) &&
                (!camView.ok || camViewMatrixAgrees(vm, camView))) {
                vmSrc = 1;
            } else if (camView.ok &&
                       camViewMatrix(camView, (float)vw / (float)vh, vm)) {
                vmSrc = 2;
            }
            if (!vmSrc) {
                for (int k = 0; k < 16; ++k) vm[k] = 0.0f;
                diagRate("view-no-projection", 5000,
                         "no usable projection: cam=%p rotOff=%d fovOff=%d "
                         "ve=%p vmOff=%d",
                         (void*)camAddr, o.cameraRotation, o.cameraFOV,
                         (void*)ve, o.visualEngineViewMatrix);
            }
        }
        bool haveVm = vmSrc != 0;
        g_espVmDiag = vmSrc;
        // Projection self-check (diag only): how far the local character is
        // from the camera tells us first/third person, and where the cache
        // projects it tells us whether the basis/fov/rect agree with what is
        // rendered. Third person => self must land near the client centre.
        if (g_diagState && vmSrc && !ents.empty()) {
            int nOn = 0, nAll = 0;
            int selfX = -1, selfY = -1;
            float selfDist = -1.0f;
            Vec3 selfP{};
            for (auto& e : ents) {
                Vec3 p{};
                if (!e.prim ||
                    !m.read(e.prim + (uint64_t)o.primitivePosition, p))
                    continue;
                int px = 0, py = 0;
                bool on = screenW2S(p, vm, px, py, vpx, vpy, vpw, vph, sw, sh);
                if (e.self) {
                    selfX = on ? px : -1;
                    selfY = on ? py : -1;
                    selfP = p;
                    float dx = p.x - cam.x, dy = p.y - cam.y, dz = p.z - cam.z;
                    selfDist = sqrtf(dx * dx + dy * dy + dz * dz);
                    continue;
                }
                ++nAll;
                if (on) ++nOn;
            }
            Vec3 d{ selfP.x - cam.x, selfP.y - cam.y, selfP.z - cam.z };
            diagRate("proj-check", 2000,
                     "proj src=%d client=%d,%d %dx%d center=%d,%d "
                     "self=%.1fstuds screen=%d,%d others=%d onscreen=%d "
                     "espOn=%d box=%d dist=%d name=%d",
                     vmSrc, vpx, vpy, vpw, vph, (vpw ? vpw : sw) / 2,
                     (vph ? vph : sh) / 2, selfDist, selfX, selfY, nAll, nOn,
                     cfgEspOn, cfgEspBox, cfgEspDist, cfgEspName);
            diagRate("proj-raw", 2000,
                     "proj raw fov=%.2f cam=%.2f,%.2f,%.2f self=%.2f,%.2f,%.2f "
                     "d=(%.2f,%.2f,%.2f) dotR=%.2f dotU=%.2f depth=%.2f "
                     "r=%.2f,%.2f,%.2f u=%.2f,%.2f,%.2f f=%.2f,%.2f,%.2f",
                     camFov, cam.x, cam.y, cam.z, selfP.x, selfP.y, selfP.z,
                     d.x, d.y, d.z, camViewDot(d, camView.right),
                     camViewDot(d, camView.up), camViewDot(d, camView.fwd),
                     camView.right.x, camView.right.y, camView.right.z,
                     camView.up.x, camView.up.y, camView.up.z, camView.fwd.x,
                     camView.fwd.y, camView.fwd.z);
        }
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (haveVm) {
            espCtx.draw(dl, vm);

            // ---- Aimbot v2 (velext knobs): sticky + dead/team/dist filters +
            // velocity prediction + deadzone, then mouse or memory actuation.
            // Foulz: FOV + selection centered on the REAL cursor, not the
            // viewport center (identical when locked, correct in menus).
            // The game's true crosshair is ALWAYS the viewport center when
            // using raw input (Windows cursor may sit in a corner).
            aimCtx.tick(dl, vm, true, fgPid, typingNow);
        }
        // Viewport-shift reset (mode 2, Foulz reset_vp): restore identity
        // whenever we didn't shift this frame.
                { static DWORD lastHbTick = 0; // hitbox expander: 2Hz re-resolve
          if (nowLoop - lastHbTick >= 500) {
              lastHbTick = nowLoop;
              hitboxTick(m, sdk, o, haveWrite, nowLoop);
          } }
        // ---- Autoclicker: fixed-interval clicks for a set burst duration.
        aimCtx.autoclickTick(fgPid, typingNow);
        aimCtx.restoreViewportIfNeeded(false);
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

    aimCtx.releaseTriggerIfDown();
    aimCtx.restoreViewportIfNeeded(true);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(L"ext_ovl", wc.hInstance);
    // Stunt first: it restores the root pose, the humanoid flags and (ghost)
    // the CanCollide bit, and it must do that before hitboxRestoreAll writes
    // its own saved Flags snapshot back (HANDOFF §3.5).
    Stunt::stop(m, o);
    Tp::stop(m, o);          // drop dash velocity + put collision bits back
    Spectate::stop(m);       // camera subject back to the local Humanoid
    hitboxRestoreAll(m, o);
    worldRestoreAll(m, o);
    g_writerRun = false; if (writerTh.joinable()) writerTh.join();
    Fly::stop(m, o);
    toolsShutdown();
    g_srvRun.store(false);
    if (srvTh.joinable()) srvTh.join();
    diagClose();
    printf("overlay exit OK\n");
    return 0;
}
