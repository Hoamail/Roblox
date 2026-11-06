#pragma once
// Aim snapshot / writer handoff + aim tick, trigger and autoclick.
// Snapshot scaffolding landed in S3.1; the tick moved here in S3.3a.
#include "ovl_types.h"
#include <atomic>
#include <cstdint>
#include <mutex>

struct ImDrawList;
struct Memory;
struct Offsets;

// ---- snapshot (move tu overlay.cpp 797-806) ----
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
extern std::mutex g_snapMu;
extern AimSnap g_snap;
extern std::atomic<uint64_t> g_snapEpoch;
extern std::atomic<uint64_t> g_snapSeq;
void invalidateAimSnapshot();

// ---- Phase C types ----
struct MouseCtlState {
    bool active = false;
    float abVX = 0.0f, abVY = 0.0f;
    float iX = 0.0f, iY = 0.0f;
    float idleT = 0.0f;
    bool settled = false;
    float lastEmaX = 0.0f, lastEmaY = 0.0f;
    bool lastEmaInit = false;
};
struct VelextSens {
    bool ok = false;
    float out1 = 1.0f;
    float out2 = 1.0f;
};

// Block hang so 179-212, gia tri nguyen ven; static const -> inline constexpr.
inline constexpr float kVelextTauPerUnit = 0.008f;
inline constexpr float kVelextDtFallback = 0.004f;
inline constexpr float kVelextDtMax = 0.1f;
inline constexpr uint64_t kVelextGateNs = 4000000ULL;
inline constexpr float kVelextAlphaEps = 0.002f;
inline constexpr float kVelextDotDeadzone = 0.99996f;
inline constexpr float kVelextMouseDzPx = 1.5f;
inline constexpr float kVelextStepCapPxS = 14000.0f;
inline constexpr float kVelextStepCapLo = 12.0f;
inline constexpr float kVelextStepCapHi = 400.0f;
inline constexpr float kVelextBpMoveDeg = 0.02f;
inline constexpr uint64_t kVelextBpTimeoutNs = 60000000ULL;
inline constexpr uint64_t kVelextSensRvaA = 0x8D857D8ULL;
inline constexpr uint64_t kVelextSensRvaB = 0x8D857E0ULL;
inline constexpr uint64_t kVelextSensRvaC = 0x8D85820ULL;
inline constexpr uint64_t kVelextMouseSvcSensRva = 0x21C;
inline constexpr float kVelextSensInLo = 1e-3f;
inline constexpr float kVelextSensInHi = 100.0f;
inline constexpr float kVelextSensOutLo = 1e-4f;
inline constexpr float kVelextSensOutHi = 1000.0f;
inline constexpr float kVelextSensXFac = 0.5f;
inline constexpr float kVelextSensYFac = 0.385f;
inline constexpr float kVelextMinCosP = 0.15f;
inline constexpr float kDeg2Rad = 0.0174532925f;
inline constexpr float kRad2Deg = 57.2957795f;
inline constexpr int kVelextBpStaleMax = 6;
inline constexpr uint64_t kVelextBpProbeNs = 750000000ULL;

// Helper definitions stay in overlay.cpp until S3.3b.
bool velextActGate(uint64_t nowNs, uint64_t* lastNsOut, float* dtOut,
                   float* alphaOut, float smoothing);
bool camYawPitchDeg(const float* m9, float* yawDeg, float* pitchDeg);
VelextSens readVelextSens(Memory& mem, uint64_t base, uint64_t msvc);
void velextMouseCounts(float eX, float eY, float focal, float p,
                       float out1, float out2, float alpha, float dt,
                       float* cxOut, float* cyOut);
bool mouseCursorHidden();
float wrap180f(float d);
void sendRelMouse(int mx, int my);

struct AimCtx {
    AimCtx(Memory& m_, SDK& sdk_, const Offsets& o_, std::vector<Ent>& ents_,
           bool& haveWrite_, uint64_t& camAddr_, Vec3& cam_,
           int& vpx_, int& vpy_, int& vpw_, int& vph_, int& sw_, int& sh_,
           bool& aimOn_, bool& trigOn_, bool& acOn_, uint64_t& stickPlayer_);

    // ---- refs (17) ----
    Memory& m;
    SDK& sdk;
    const Offsets& o;
    std::vector<Ent>& ents;
    bool& haveWrite;
    uint64_t& camAddr;
    Vec3& cam;
    int& vpx;
    int& vpy;
    int& vpw;
    int& vph;
    int& sw;
    int& sh;
    bool& aimOn;
    bool& trigOn;
    bool& acOn;
    uint64_t& stickPlayer;

    // ---- owned state (32; ten y het local cu) ----
    int dbgEnts = 0;
    double lastAimQpc = 0;
    DWORD lastActTick = 0;
    Vec3 lockPos{};
    DWORD lockTick = 0;
    DWORD stickStartTick = 0;
    int trigPhase = 0;
    DWORD trigT0 = 0, lastFireTick = 0;
    int acPhase = 0;
    DWORD acT0 = 0, acNext = 0, acPressT0 = 0;
    float accX = 0, accY = 0;
    float emaTargetX = 0, emaTargetY = 0;
    uint64_t emaPrim = 0;
    bool emaInit = false;
    MouseCtlState mouseCtl;
    uint64_t mouseSvc = 0;
    DWORD mouseSvcRetry = 0;
    bool bpInFlight = false;
    uint64_t bpStampNs = 0;
    float bpYawDeg = 0, bpPitchDeg = 0;
    bool bpCursorHold = false;
    int bpStaleCount = 0;
    bool bpStaleHold = false;
    uint64_t bpProbeNs = 0;
    bool vpShifted = false;
    bool shiftedNow = false;

    void tick(ImDrawList* dl, const float* vm, bool haveVm,
              DWORD fgPid, bool typingNow);
    void autoclickTick(DWORD fgPid, bool typingNow);
    void releaseTriggerIfDown();
    void restoreViewportIfNeeded(bool force);
    void onAimDisabled();
    void onAutoclickEnabled();

private:
    bool w2s(const Vec3& w, const float* mt, int& sx, int& sy);
};
