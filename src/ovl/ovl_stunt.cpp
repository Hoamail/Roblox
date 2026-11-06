// Stunt module: state machine + writer thread + preset library.
//
// Design notes (measured facts live in HANDOFF_visible_stunts.md §2):
//   * The game refreshes Primitive.Position / velocity / rotation from its own
//     simulation every frame, so a write done once per overlay frame is gone
//     before the next physics step. Like ovl_fly.cpp, everything here is held
//     by a Sleep(0) writer thread (~1e6 writes/s) and parks on Sleep(10) when
//     idle.
//   * Only the root assembly replicates: presets move the box, they never pose
//     limbs (Motor6D.Transform is render-only).
//   * PlatformStand=1 + AutoRotate=0 (+ EvaluateStateMachine=0) stop the
//     humanoid from standing itself back up, which is what makes a rotated
//     pose survive long enough to be replicated.
//   * Rotation matrix on Primitive is 9 float, column-major: right, up, back.
#include "ovl_stunt.h"
#include "ovl_core.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr float kPi = 3.14159265358979f;

inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
// Smoothstep: 0 at 0, 1 at 1, zero slope at both ends. One flip with easing
// reads as "real" where a linear ramp reads as a robot.
inline float ease(float x) { x = clamp01(x); return x * x * (3.0f - 2.0f * x); }

inline bool finitef(float v) { return std::isfinite(v); }
inline bool finite3(const Vec3& v) {
    return finitef(v.x) && finitef(v.y) && finitef(v.z);
}
inline bool finite9(const float m[9]) {
    for (int i = 0; i < 9; ++i)
        if (!finitef(m[i])) return false;
    return true;
}

// Euler -> basis, order Y->X->Z (HANDOFF §2.1). out = right, up, back columns.
void eulerToBasis(float yaw, float pitch, float roll, float out[9]) {
    const float cy = cosf(yaw), sy = sinf(yaw);
    const float cx = cosf(pitch), sx = sinf(pitch);
    const float cz = cosf(roll), sz = sinf(roll);
    const float r00 = cy * cz + sy * sx * sz;
    const float r01 = -cy * sz + sy * sx * cz;
    const float r02 = sy * cx;
    const float r10 = cx * sz;
    const float r11 = cx * cz;
    const float r12 = -sx;
    const float r20 = -sy * cz + cy * sx * sz;
    const float r21 = sy * sz + cy * sx * cz;
    const float r22 = cy * cx;
    out[0] = r00; out[1] = r10; out[2] = r20; // right
    out[3] = r01; out[4] = r11; out[5] = r21; // up
    out[6] = r02; out[7] = r12; out[8] = r22; // back
}

// Inverse of eulerToBasis (same order). Used to start a kinematic integration
// from whatever orientation the character happened to have.
bool basisToEuler(const float m[9], float& yaw, float& pitch, float& roll) {
    float sy = -m[7];
    if (sy > 1.0f) sy = 1.0f;
    if (sy < -1.0f) sy = -1.0f;
    pitch = asinf(sy);
    yaw = atan2f(m[6], m[8]);
    roll = atan2f(m[1], m[4]);
    return finitef(yaw) && finitef(pitch) && finitef(roll);
}

// ---------------------------------------------------------------- data model
struct StuntSeed {
    uint64_t prim = 0;
    uint64_t humanoid = 0;
    Vec3 pos{}, vel{}, ang{};
    float rot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    float health = 100.0f;
    float hipHeight = 0.0f; // Humanoid.HipHeight at seed (Float preset ramps it)
    uint32_t flags = 0;
    bool haveFlags = false;
    unsigned char platformStand = 0, sit = 0, eval = 0, autoRotate = 0;
    bool valid = false;
};

struct StuntEnv {
    Vec3 fwd{ 0, 0, 1 }, right{ 1, 0, 0 };
    Vec3 tpos{};
    float trot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    bool haveTarget = false;
};

enum {
    PF_PLATFORM = 1, // humanoid.PlatformStand = 1 (no upright torque)
    PF_NOROT    = 2, // humanoid.AutoRotate = 0
    PF_NOEVAL   = 4, // humanoid.EvaluateStateMachine = 0
    PF_SIT      = 8, // humanoid.Sit = 1
    PF_GHOST    = 16 // Primitive.Flags CanCollide bit cleared
};

struct StuntPose {
    bool dyn = false;
    Vec3 vel{}, ang{};
    bool useRot = false;
    float rot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    bool usePos = false;
    Vec3 pos{};
    unsigned flags = 0;
    bool done = false;
    int sub = -1;
    // Altitude hold: target = seed.y + alt. The writer keeps it with a P
    // controller on velocity.y, which is the only way to keep a ragdoll body
    // out of the floor (PlatformStand collapses it under the root).
    float alt = 0.0f;
    float hip = 0.0f;
    bool useHip = false;
};

struct StuntInteg { float yaw = 0, pitch = 0, roll = 0; bool init = false; };

struct PresetDef {
    const char* name;
    float dur;    // 0 = endless until toggled off
    unsigned flags;
    float alt;    // altitude hold: target = seed.y + alt (0 = no hold)
};

// UI order (HANDOFF §3.8). Indices are part of overlay.cfg (stunt.preset), so
// append only - never reorder.
const PresetDef kPresets[] = {
    { "Backflip",      0.85f, 0, 0.0f },
    { "Cartwheel",     1.00f, 0, 0.0f },
    { "Beyblade",      2.00f, 0, 2.0f },
    { "Tube Man",      0.00f, PF_PLATFORM | PF_NOROT, 1.2f },
    { "Strobe",        2.00f, PF_PLATFORM | PF_NOROT, 1.6f },
    { "Statue",        0.00f, PF_PLATFORM | PF_NOROT | PF_NOEVAL, 2.6f },
    { "Ragdoll Cannon",1.60f, PF_PLATFORM, 0.0f },
    { "Heli",          2.00f, PF_PLATFORM, 0.0f },
    { "Piggyback",     0.00f, PF_SIT | PF_NOEVAL, 0.0f },
    { "Orbit",         0.00f, PF_PLATFORM | PF_NOEVAL, 0.0f },
    { "Moonwalk",      1.50f, PF_NOROT, 0.0f },
    { "Ghost",         0.00f, PF_GHOST, 0.2f },
    { "Spin Dash",     0.60f, 0, 0.0f },
    { "Ground Pound",  0.90f, 0, 0.0f },
    { "Missile",       2.60f, 0, 0.0f },   // combos, durations below
    { "Tornado",       8.881f, 0, 0.0f },
    { "Phantom",       3.95f, 0, 0.0f },
    // ---- batch 2: the ones that actually look different from each other ----
    { "Plank",         0.00f, PF_PLATFORM | PF_NOROT | PF_NOEVAL, 3.0f },
    { "Headspin",      0.00f, PF_PLATFORM | PF_NOROT | PF_NOEVAL, 3.2f },
    { "Float",         0.00f, PF_NOROT, 1.0f },
    { "Figure 8",      4.00f, PF_PLATFORM | PF_NOEVAL, 2.4f },
    { "Helix",         6.00f, PF_PLATFORM | PF_NOEVAL, 2.4f },
    { "Pendulum",      3.00f, PF_PLATFORM | PF_NOEVAL, 2.0f },
    { "Skipper",       3.00f, 0, 0.0f },
    { "Zero-G",        0.00f, PF_PLATFORM, 0.0f },
    { "Sink",          0.00f, PF_PLATFORM | PF_NOROT, 0.0f },
};
const int kPresetCount = (int)(sizeof(kPresets) / sizeof(kPresets[0]));

struct Seg { int preset; float dur; };
const Seg kMissile[] = { { 6, 0.9f }, { 7, 1.2f }, { 13, 0.5f } };
const Seg kTornado[] = { { 2, 0.5f }, { 9, 8.3806f } }; // orbit two turns at 1.5 rad/s
const Seg kPhantom[] = { { 5, 0.6f }, { 4, 1.0f }, { 3, 1.5f }, { 0, 0.85f } };

const Seg* comboTable(int preset, int& n) {
    if (preset == 14) { n = (int)(sizeof(kMissile) / sizeof(kMissile[0])); return kMissile; }
    if (preset == 15) { n = (int)(sizeof(kTornado) / sizeof(kTornado[0])); return kTornado; }
    if (preset == 16) { n = (int)(sizeof(kPhantom) / sizeof(kPhantom[0])); return kPhantom; }
    n = 0;
    return nullptr;
}

// ---------------------------------------------------------- pose computation
// Pure function of (preset, time, seed, params, env): the writer thread and the
// offline self-test run the exact same code.
StuntPose computePose(int preset, float t, const StuntSeed& s, const StuntParams& p,
                      const StuntEnv& env, float dt, StuntInteg* ig) {
    StuntPose po;
    if (preset < 0 || preset >= kPresetCount) preset = 0;

    const float dur = kPresets[preset].dur;
    float tt = t;
    if (dur > 0.0f && p.loop) {
        tt = fmodf(t, dur);
        if (tt < 0.0f) tt += dur;
    }
    po.done = !p.loop && dur > 0.0f && t >= dur;

    int sub = preset;
    float lt = tt;
    int nseg = 0;
    const Seg* segs = comboTable(preset, nseg);
    if (segs && nseg > 0) {
        float acc = 0.0f;
        for (int i = 0; i < nseg; ++i) {
            if (lt < acc + segs[i].dur || i == nseg - 1) {
                sub = segs[i].preset;
                lt -= acc;
                if (lt < 0.0f) lt = 0.0f;
                break;
            }
            acc += segs[i].dur;
        }
        // Combo segments carry the flags of the preset they play.
        po.flags = kPresets[sub].flags;
    } else {
        po.flags = kPresets[preset].flags;
    }
    po.sub = sub;
    po.alt = kPresets[sub].alt;

    const float amp = p.amp > 0.0f ? p.amp : 1.0f;

    switch (sub) {
    case 0: { // Backflip: pitch negative -> head goes back (HANDOFF §2.1)
        po.dyn = true;
        po.ang.x = -2.0f * kPi / 0.85f;
        po.vel.y = 18.0f * amp;
        po.sub = 0;
        break;
    }
    case 1: { // Cartwheel: roll + full turn, drift to the right
        po.dyn = true;
        po.ang.z = 2.0f * kPi / 1.0f;
        po.vel.x = env.right.x * 8.0f * amp;
        po.vel.z = env.right.z * 8.0f * amp;
        break;
    }
    case 2: { // Beyblade: spin + slow forward tumble, reads as a wobbling top
        po.dyn = true;
        po.ang.y = 14.0f;
        po.ang.x = 2.2f * amp;
        po.vel.y = (p.hover || lt > 0.4f) ? 0.0f : 2.0f * amp;
        break;
    }
    case 3: { // Tube Man (kinematic rotation, tiny bob)
        po.flags = PF_PLATFORM | PF_NOROT;
        const float w1 = 2.0f * kPi * 1.6f;
        const float w2 = 2.0f * kPi * 1.1f;
        const float pitch = 0.3142f * amp * sinf(w1 * lt);      // 18 deg
        const float roll = 0.2094f * amp * sinf(w2 * lt + 1.0f); // 12 deg
        const float yaw = 0.5f * lt;
        eulerToBasis(yaw, pitch, roll, po.rot);
        po.useRot = true;
        po.pos = s.pos;
        po.pos.y += 0.35f * amp * fabsf(sinf(w1 * lt));
        po.usePos = true;
        po.dyn = true; // velocity pinned to 0 every pass
        break;
    }
    case 4: { // Strobe: 4 hard snaps, 8 Hz
        po.flags = PF_PLATFORM | PF_NOROT;
        static const float ox[4] = { 1.5f, -1.5f, 1.5f, -1.5f };
        static const float oz[4] = { 1.5f, 1.5f, -1.5f, -1.5f };
        int k = ((int)floorf(lt * 8.0f)) & 3;
        po.pos = s.pos;
        po.pos.x += ox[k] * amp;
        po.pos.z += oz[k] * amp;
        po.usePos = true;
        eulerToBasis((kPi * 0.5f) * (float)k, 0.0f, 0.0f, po.rot);
        po.useRot = true;
        po.dyn = true;
        break;
    }
    case 5: { // Statue: frozen at a lean, everything pinned
        po.flags = PF_PLATFORM | PF_NOROT | PF_NOEVAL;
        eulerToBasis(-0.6109f * amp, 0.0f, 0.0f, po.rot); // -35 deg yaw
        po.useRot = true;
        po.dyn = true;
        break;
    }
    case 6: { // Ragdoll Cannon: platform stand + launch, then fall
        po.flags = PF_PLATFORM;
        po.dyn = true;
        if (lt < 1.6f) {
            po.vel.x = env.fwd.x * 95.0f * amp;
            po.vel.z = env.fwd.z * 95.0f * amp;
            po.vel.y = 12.0f * amp;
            po.ang = Vec3{ 5.0f, 7.0f, 3.0f };
        }
        break; // after dur: zero velocity, gravity does the rest
    }
    case 7: { // Heli: spinning corpse that slowly climbs
        po.flags = PF_PLATFORM;
        po.dyn = true;
        po.ang.y = 18.0f;
        po.vel.y = 1.2f * amp;
        break;
    }
    case 8: { // Piggyback: ride the target, Sit=1
        po.flags = PF_SIT | PF_NOEVAL;
        po.dyn = true;
        if (env.haveTarget) {
            po.pos = env.tpos;
            po.pos.y += 2.4f;
            po.pos.x -= env.trot[6] * 0.8f; // back column of the target basis
            po.pos.z -= env.trot[8] * 0.8f;
            po.usePos = true;
            memcpy(po.rot, env.trot, sizeof po.rot);
            po.useRot = true;
        }
        break;
    }
    case 9: { // Orbit: circle the target, always facing it
        po.flags = PF_PLATFORM | PF_NOEVAL;
        po.dyn = true;
        if (env.haveTarget) {
            const float r = 4.0f + amp;
            const float w = 1.5f;
            po.pos = env.tpos;
            po.pos.x += cosf(w * lt) * r;
            po.pos.z += sinf(w * lt) * r;
            po.pos.y += 0.5f;
            po.usePos = true;
            float yaw = atan2f(env.tpos.x - po.pos.x, env.tpos.z - po.pos.z);
            if (preset == 15) yaw += 14.0f * lt; // TORNADO: spin while orbiting
            eulerToBasis(yaw, preset == 15 ? 0.28f : 0.0f, 0.0f, po.rot);
            po.useRot = true;
        }
        break;
    }
    case 10: { // Moonwalk: slide backwards, face stays on the camera forward
        po.flags = PF_NOROT;
        po.dyn = true;
        po.vel.x = -env.fwd.x * 14.0f * amp;
        po.vel.z = -env.fwd.z * 14.0f * amp;
        eulerToBasis(atan2f(env.fwd.x, env.fwd.z), 0.0f, 0.0f, po.rot);
        po.useRot = true;
        break;
    }
    case 11: { // Ghost: clear CanCollide locally; position keeps replicating
        po.flags = PF_GHOST;
        break;
    }
    case 12: { // Spin Dash
        po.dyn = true;
        po.ang.y = 30.0f;
        po.vel.x = env.fwd.x * 70.0f * amp;
        po.vel.z = env.fwd.z * 70.0f * amp;
        break;
    }
    case 13: { // Ground Pound
        po.dyn = true;
        if (lt < 0.15f) {
            po.vel.y = 12.0f * amp;
        } else if (lt < 0.45f) {
            po.vel.y = -140.0f * amp;
            po.ang.x = 2.0f;
        }
        break; // after 0.45: velocity zero
    }
    // ---- batch 2 ----------------------------------------------------------
    case 17: { // Plank: horizontal, slow yaw = body sweeping like a clock hand
        po.flags = PF_PLATFORM | PF_NOROT | PF_NOEVAL;
        eulerToBasis(1.2f * lt, 1.5707964f, 0.0f, po.rot);
        po.useRot = true;
        po.dyn = true;
        break;
    }
    case 18: { // Headspin: inverted, fast yaw (breakdance head spin)
        po.flags = PF_PLATFORM | PF_NOROT | PF_NOEVAL;
        eulerToBasis(8.0f * lt, 3.1415927f, 0.0f, po.rot);
        po.useRot = true;
        po.dyn = true;
        break;
    }
    case 19: { // Float: ramp Humanoid.HipHeight -> body lifts off the feet
        po.flags = PF_NOROT;
        const float ph = 3.0f;
        const float k = 0.5f - 0.5f * cosf(2.0f * kPi * lt / ph);
        po.hip = s.hipHeight + 3.0f * amp * k;
        if (po.hip < 0.0f) po.hip = 0.0f;
        if (po.hip > 20.0f) po.hip = 20.0f;
        po.useHip = true;
        po.dyn = true;
        break;
    }
    case 20: { // Figure 8: lemniscate around the seed, facing the tangent
        po.flags = PF_PLATFORM | PF_NOEVAL;
        const float w = 0.7f;
        const float ox = 4.0f * amp * sinf(w * lt);
        const float oz = 2.0f * amp * sinf(2.0f * w * lt);
        po.pos = s.pos;
        po.pos.x += env.right.x * ox + env.fwd.x * oz;
        po.pos.z += env.right.z * ox + env.fwd.z * oz;
        po.usePos = true;
        const float dx = 4.0f * amp * w * cosf(w * lt);
        const float dz = 4.0f * amp * w * cosf(2.0f * w * lt);
        const float dirx = env.right.x * dx + env.fwd.x * dz;
        const float dirz = env.right.z * dx + env.fwd.z * dz;
        eulerToBasis(atan2f(dirx, dirz), 0.0f, 0.0f, po.rot);
        po.useRot = true;
        po.dyn = true;
        break;
    }
    case 21: { // Helix: circle while climbing, like a drill coming out of the floor
        po.flags = PF_PLATFORM | PF_NOEVAL;
        const float w = 1.6f;
        const float r = 3.0f * amp;
        po.pos = s.pos;
        po.pos.x += cosf(w * lt) * r;
        po.pos.z += sinf(w * lt) * r;
        po.pos.y += 0.8f * amp * lt;
        po.usePos = true;
        float yaw = atan2f(-sinf(w * lt), cosf(w * lt));
        eulerToBasis(yaw, 0.0f, 0.0f, po.rot);
        po.useRot = true;
        po.dyn = true;
        break;
    }
    case 22: { // Pendulum: swings on an invisible arm, feet trailing the arc
        po.flags = PF_PLATFORM | PF_NOEVAL;
        const float w = 2.2f;
        const float sw = 0.85f * sinf(w * lt);
        const float L = 5.0f * amp;
        po.pos = s.pos;
        po.pos.y += L * (1.0f - cosf(sw));
        po.pos.x += env.right.x * L * sinf(sw);
        po.pos.z += env.right.z * L * sinf(sw);
        po.usePos = true;
        eulerToBasis(atan2f(env.fwd.x, env.fwd.z), 0.0f, 0.0f, po.rot);
        po.useRot = true;
        po.dyn = true;
        break;
    }
    case 23: { // Skipper: hopping forward with a small forward tumble
        const float ph = 0.7f;
        float q = fmodf(lt, ph);
        if (q < 0.0f) q += ph;
        po.vel.y = 14.0f * amp * sinf(kPi * q / ph);
        po.vel.x = env.fwd.x * 8.0f * amp;
        po.vel.z = env.fwd.z * 8.0f * amp;
        po.ang.x = -3.0f;
        po.dyn = true;
        break;
    }
    case 24: { // Zero-G: constant drift + slow multi-axis tumble
        po.flags = PF_PLATFORM;
        po.vel.x = env.fwd.x * 4.0f * amp;
        po.vel.z = env.fwd.z * 4.0f * amp;
        po.vel.y = 1.2f * amp;
        po.ang = Vec3{ 2.0f, 1.0f, 3.0f };
        po.dyn = true;
        break;
    }
    case 25: { // Sink: deliberate slow descent through the floor
        po.flags = PF_PLATFORM | PF_NOROT;
        po.vel.y = -2.5f * amp;
        po.dyn = true;
        break;
    }
    default:
        po.dyn = true;
        break;
    }

    if (p.hover) po.vel.y = 0.0f;

    // Kinematic override (stunt.mode=1): integrate the angular velocity here and
    // write the matrix instead of letting the solver do it. This is the A/B
    // switch for E1/E2 - dynamic presets only, never the kinematic presets.
    if (ig && p.mode == 1 && po.dyn &&
        (fabsf(po.ang.x) + fabsf(po.ang.y) + fabsf(po.ang.z)) > 1e-6f) {
        if (!ig->init) {
            if (!basisToEuler(s.rot, ig->yaw, ig->pitch, ig->roll)) {
                ig->yaw = ig->pitch = ig->roll = 0.0f;
            }
            ig->init = true;
        }
        ig->yaw += po.ang.y * dt;
        ig->pitch += po.ang.x * dt;
        ig->roll += po.ang.z * dt;
        eulerToBasis(ig->yaw, ig->pitch, ig->roll, po.rot);
        po.useRot = true;
        po.ang = Vec3{ 0, 0, 0 };
    }

    // Never hand NaN to a write: a single bad frame otherwise sticks in the
    // assembly until the next physics step overwrites it.
    if (!finite3(po.vel)) po.vel = Vec3{ 0, 0, 0 };
    if (!finite3(po.ang)) po.ang = Vec3{ 0, 0, 0 };
    if (!finite3(po.pos)) { po.pos = s.pos; }
    if (!finite9(po.rot)) memcpy(po.rot, s.rot, sizeof po.rot);
    return po;
}

// ------------------------------------------------------------------- restore
struct RestorePlan {
    Vec3 pos{}, vel{}, ang{};
    float rot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    unsigned char platformStand = 0, sit = 0, eval = 0, autoRotate = 0;
    float hipHeight = 0.0f;
    uint32_t flags = 0;
    bool haveFlags = false;
};

// Everything the restore writes, derived from the seed only. Pulled out so the
// offline self-test can check "restore hands the seed back unchanged".
RestorePlan makeRestorePlan(const StuntSeed& s) {
    RestorePlan r;
    r.pos = s.pos;
    r.vel = Vec3{ 0, 0, 0 };
    r.ang = Vec3{ 0, 0, 0 };
    memcpy(r.rot, s.rot, sizeof r.rot);
    r.platformStand = s.platformStand;
    r.sit = s.sit;
    r.eval = s.eval;
    r.autoRotate = s.autoRotate;
    r.hipHeight = s.hipHeight;
    r.flags = s.flags;
    r.haveFlags = s.haveFlags;
    return r;
}

// Alive check: after Health hits 0 the server takes ownership, so restoring a
// pose would only fight the ragdoll and write into a corpse.
bool seedAlive(Memory& m, const Offsets& o, const StuntSeed& s) {
    if (!s.humanoid || !o.humanoidHealth) return true;
    float hp = s.health;
    if (!m.read(s.humanoid + (uint64_t)o.humanoidHealth, hp)) return true;
    return hp > 0.0f;
}

void restoreSeed(Memory& m, const Offsets& o, const StuntSeed& s) {
    if (!s.valid || !s.prim) return;
    if (!seedAlive(m, o, s)) return;
    const RestorePlan r = makeRestorePlan(s);
    if (o.primitiveVelocity) {
        m.write(s.prim + (uint64_t)o.primitiveVelocity, r.vel);
        m.write(s.prim + (uint64_t)o.primitiveVelocity, r.vel);
    }
    if (o.primitiveAngularVelocity)
        m.write(s.prim + (uint64_t)o.primitiveAngularVelocity, r.ang);
    if (o.primitiveRotation) m.write(s.prim + (uint64_t)o.primitiveRotation, r.rot);
    if (o.primitivePosition) m.write(s.prim + (uint64_t)o.primitivePosition, r.pos);
    if (o.primitiveFlags && r.haveFlags)
        m.write(s.prim + (uint64_t)o.primitiveFlags, r.flags);
    if (s.humanoid) {
        if (o.humanoidPlatformStand)
            m.write<unsigned char>(
                s.humanoid + (uint64_t)o.humanoidPlatformStand, r.platformStand);
        if (o.humanoidSit)
            m.write<unsigned char>(s.humanoid + (uint64_t)o.humanoidSit, r.sit);
        if (o.humanoidEvaluateStateMachine)
            m.write<unsigned char>(
                s.humanoid + (uint64_t)o.humanoidEvaluateStateMachine, r.eval);
        if (o.humanoidAutoRotate)
            m.write<unsigned char>(
                s.humanoid + (uint64_t)o.humanoidAutoRotate, r.autoRotate);
        if (o.humanoidHipHeight)
            m.write(s.humanoid + (uint64_t)o.humanoidHipHeight, r.hipHeight);
    }
}

// -------------------------------------------------------------- writer state
std::atomic<bool> g_run{ true };
std::atomic<int> g_on{ 0 };
std::atomic<uint64_t> g_prim{ 0 };
std::atomic<uint64_t> g_hum{ 0 };
std::atomic<uint64_t> g_cam{ 0 };
std::atomic<uint64_t> g_target{ 0 };
std::atomic<int> g_preset{ 0 };
std::atomic<int> g_ragdoll{ 0 };
std::atomic<int> g_loop{ 1 };
std::atomic<int> g_hover{ 0 };
std::atomic<int> g_mode{ 0 };
std::atomic<float> g_tscale{ 1.0f };
std::atomic<float> g_amp{ 1.0f };
std::atomic<double> g_t0{ 0.0 };
std::atomic<int> g_state{ 0 };  // 0 idle, 1 running, 2 restoring
std::atomic<int> g_sub{ -1 };
std::atomic<float> g_rate{ 0.0f };
std::atomic<bool> g_rearm{ false };

Memory* g_mem = nullptr;
const Offsets* g_off = nullptr;
std::thread g_th;
bool g_started = false;

std::mutex g_seedMu;
StuntSeed g_shared;      // last seed, for stop()/panic on the main thread
bool g_sharedValid = false;

bool readSeed(Memory& m, const Offsets& o, uint64_t prim, uint64_t hum,
              StuntSeed& s) {
    StuntSeed n;
    if (!prim || !o.primitivePosition) return false;
    if (!m.read(prim + (uint64_t)o.primitivePosition, n.pos)) return false;
    if (!finite3(n.pos)) return false;
    n.prim = prim;
    n.humanoid = hum;
    if (o.primitiveRotation) {
        if (!m.read(prim + (uint64_t)o.primitiveRotation, n.rot)) return false;
        if (!finite9(n.rot)) return false;
    }
    if (o.primitiveVelocity) m.read(prim + (uint64_t)o.primitiveVelocity, n.vel);
    if (o.primitiveAngularVelocity)
        m.read(prim + (uint64_t)o.primitiveAngularVelocity, n.ang);
    if (o.primitiveFlags) {
        uint32_t fl = 0;
        if (m.read(prim + (uint64_t)o.primitiveFlags, fl)) {
            n.flags = fl;
            n.haveFlags = true;
        }
    }
    if (hum) {
        if (o.humanoidPlatformStand)
            m.read(hum + (uint64_t)o.humanoidPlatformStand, n.platformStand);
        if (o.humanoidSit) m.read(hum + (uint64_t)o.humanoidSit, n.sit);
        if (o.humanoidEvaluateStateMachine)
            m.read(hum + (uint64_t)o.humanoidEvaluateStateMachine, n.eval);
        if (o.humanoidAutoRotate)
            m.read(hum + (uint64_t)o.humanoidAutoRotate, n.autoRotate);
        if (o.humanoidHealth) m.read(hum + (uint64_t)o.humanoidHealth, n.health);
        if (o.humanoidHipHeight)
            m.read(hum + (uint64_t)o.humanoidHipHeight, n.hipHeight);
    }
    n.valid = true;
    s = n;
    return true;
}

void applyHumanoidFlags(Memory& m, const Offsets& o, const StuntSeed& s,
                        const StuntPose& po, int ragdoll, unsigned char& lastPlat,
                        unsigned char& lastSit, unsigned char& lastEval,
                        unsigned char& lastRot) {
    if (!s.humanoid) return;
    const unsigned char plat =
        ((po.flags & PF_PLATFORM) || ragdoll) ? 1 : s.platformStand;
    const unsigned char sit = (po.flags & PF_SIT) ? 1 : s.sit;
    const unsigned char eval = (po.flags & PF_NOEVAL) ? 0 : s.eval;
    const unsigned char rot = (po.flags & PF_NOROT) ? 0 : s.autoRotate;
    if (o.humanoidPlatformStand && plat != lastPlat) {
        m.write<unsigned char>(s.humanoid + (uint64_t)o.humanoidPlatformStand, plat);
        lastPlat = plat;
    }
    if (o.humanoidSit && sit != lastSit) {
        m.write<unsigned char>(s.humanoid + (uint64_t)o.humanoidSit, sit);
        lastSit = sit;
    }
    if (o.humanoidEvaluateStateMachine && eval != lastEval) {
        m.write<unsigned char>(s.humanoid + (uint64_t)o.humanoidEvaluateStateMachine,
                               eval);
        lastEval = eval;
    }
    if (o.humanoidAutoRotate && rot != lastRot) {
        m.write<unsigned char>(s.humanoid + (uint64_t)o.humanoidAutoRotate, rot);
        lastRot = rot;
    }
}

void threadBody() {
    StuntSeed seed;
    StuntInteg ig;
    StuntEnv env;
    StuntParams p;
    unsigned tick = 0;
    long writes = 0;
    double sliceStart = qpcSeconds();
    double lastNow = qpcSeconds();
    unsigned char lastPlat = 0xFF, lastSit = 0xFF, lastEval = 0xFF, lastRot = 0xFF;
    bool ghostOn = false;
    float selfY = 0.0f;

    while (g_run.load(std::memory_order_relaxed)) {
        if (!g_on.load(std::memory_order_relaxed)) {
            if (g_state.load(std::memory_order_relaxed) != 0) {
                if (g_mem && g_off) restoreSeed(*g_mem, *g_off, seed);
                seed.valid = false;
                {
                    std::lock_guard<std::mutex> lk(g_seedMu);
                    g_sharedValid = false;
                }
                g_state.store(0, std::memory_order_relaxed);
                g_sub.store(-1, std::memory_order_relaxed);
                g_rate.store(0.0f, std::memory_order_relaxed);
                ghostOn = false;
                lastPlat = lastSit = lastEval = lastRot = 0xFF;
            }
            Sleep(10);
            continue;
        }
        if (!g_mem || !g_off) { Sleep(10); continue; }
        Memory& m = *g_mem;
        const Offsets& o = *g_off;
        uint64_t prim = g_prim.load(std::memory_order_relaxed);
        if (!prim) { Sleep(1); continue; }

        // Respawn / place change: the seed belongs to a body that no longer
        // exists. Drop it without restoring (spec §3.3).
        if (g_rearm.load(std::memory_order_relaxed)) { Sleep(1); continue; }
        if (seed.valid && prim != seed.prim) {
            seed.valid = false;
            {
                std::lock_guard<std::mutex> lk(g_seedMu);
                g_sharedValid = false;
            }
            g_state.store(0, std::memory_order_relaxed);
            g_sub.store(-1, std::memory_order_relaxed);
            g_rate.store(0.0f, std::memory_order_relaxed);
            g_rearm.store(true, std::memory_order_relaxed);
            continue;
        }

        if (g_state.load(std::memory_order_relaxed) == 0) {
            if (!readSeed(m, o, prim, g_hum.load(std::memory_order_relaxed), seed)) {
                Sleep(5);
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(g_seedMu);
                g_shared = seed;
                g_sharedValid = true;
            }
            g_t0.store(qpcSeconds(), std::memory_order_relaxed);
            ig = StuntInteg{};
            ghostOn = false;
            selfY = seed.pos.y;
            lastPlat = lastSit = lastEval = lastRot = 0xFF;
            tick = 0;
            writes = 0;
            sliceStart = qpcSeconds();
            lastNow = sliceStart;
            g_state.store(1, std::memory_order_relaxed);
        }

        const double now = qpcSeconds();
        float dt = (float)(now - lastNow);
        lastNow = now;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.05f) dt = 0.05f; // a stalled pass must not spin the pose

        // Parameters are steady enough to sample once per millisecond.
        if ((++tick & 63u) == 0 || tick == 1) {
            p.preset = g_preset.load(std::memory_order_relaxed);
            p.timeScale = g_tscale.load(std::memory_order_relaxed);
            p.amp = g_amp.load(std::memory_order_relaxed);
            p.ragdoll = g_ragdoll.load(std::memory_order_relaxed);
            p.loop = g_loop.load(std::memory_order_relaxed);
            p.hover = g_hover.load(std::memory_order_relaxed);
            p.mode = g_mode.load(std::memory_order_relaxed);
            p.targetPrim = g_target.load(std::memory_order_relaxed);
        }
        if ((tick & 1023u) == 0) {
            uint64_t cam = g_cam.load(std::memory_order_relaxed);
            if (cam && o.cameraRotation) {
                float r[9]{};
                if (m.read(cam + (uint64_t)o.cameraRotation, r)) {
                    float lx = -r[2], lz = -r[8];
                    float ll = sqrtf(lx * lx + lz * lz);
                    if (ll > 1e-4f) {
                        env.fwd.x = lx / ll;
                        env.fwd.z = lz / ll;
                    }
                    float rx = r[0], rz = r[6];
                    float rl = sqrtf(rx * rx + rz * rz);
                    if (rl > 1e-4f) {
                        env.right.x = rx / rl;
                        env.right.z = rz / rl;
                    }
                }
            }
            env.haveTarget = false;
            uint64_t tp = g_target.load(std::memory_order_relaxed);
            if (tp && o.primitivePosition) {
                Vec3 tpos{};
                if (m.read(tp + (uint64_t)o.primitivePosition, tpos) &&
                    finite3(tpos)) {
                    env.tpos = tpos;
                    env.haveTarget = true;
                    if (o.primitiveRotation) {
                        float tr[9]{};
                        if (m.read(tp + (uint64_t)o.primitiveRotation, tr) &&
                            finite9(tr))
                            memcpy(env.trot, tr, sizeof env.trot);
                    }
                }
            }
            double slice = now - sliceStart;
            if (slice > 0.0)
                g_rate.store((float)(writes / slice), std::memory_order_relaxed);
            sliceStart = now;
            writes = 0;
        }

        const float t =
            (float)((now - g_t0.load(std::memory_order_relaxed)) *
                    (double)p.timeScale);
        StuntPose po = computePose(p.preset, t, seed, p, env, dt, &ig);
        g_sub.store(po.sub, std::memory_order_relaxed);

        // Altitude hold. ragdoll/PlatformStand collapses the body under the
        // root, so every "in the air" preset pins the root at seed.y + alt:
        // position presets get the offset added to the y they already write,
        // everything else gets a P controller on velocity.y (position writes
        // alone are reverted by the solver, velocity is not).
        if (po.alt != 0.0f) {
            po.dyn = true;
            if (po.usePos) {
                po.pos.y += po.alt;
            } else {
                if ((tick & 1023u) == 0 && o.primitivePosition) {
                    Vec3 cur{};
                    if (m.read(prim + (uint64_t)o.primitivePosition, cur) &&
                        finite3(cur))
                        selfY = cur.y;
                }
                const float target = seed.pos.y + po.alt;
                float vy = (target - selfY) * 6.0f;
                if (vy > 40.0f) vy = 40.0f;
                if (vy < -40.0f) vy = -40.0f;
                po.vel.y = vy;
            }
        }

        applyHumanoidFlags(m, o, seed, po, p.ragdoll, lastPlat, lastSit, lastEval,
                           lastRot);

        // Ghost shares Primitive.Flags with the hitbox: save at seed, put the
        // bit back on the way out (and as soon as the preset changes).
        const bool wantGhost = (po.flags & PF_GHOST) != 0;
        if (o.primitiveFlags && seed.haveFlags && wantGhost != ghostOn) {
            const uint32_t bit = (uint32_t)o.primitiveCanCollideBit;
            const uint32_t f = wantGhost ? (seed.flags & ~bit) : seed.flags;
            m.write(prim + (uint64_t)o.primitiveFlags, f);
            ghostOn = wantGhost;
            writes++;
        }

        if (po.useRot && o.primitiveRotation) {
            if (m.write(prim + (uint64_t)o.primitiveRotation, po.rot)) writes++;
        }
        if (po.usePos && o.primitivePosition) {
            if (m.write(prim + (uint64_t)o.primitivePosition, po.pos)) writes++;
        }
        if (po.useHip && o.humanoidHipHeight && seed.humanoid) {
            if (m.write(seed.humanoid + (uint64_t)o.humanoidHipHeight, po.hip))
                writes++;
        }
        if (po.dyn) {
            if (o.primitiveVelocity) {
                if (m.write(prim + (uint64_t)o.primitiveVelocity, po.vel)) writes++;
            }
            if (o.primitiveAngularVelocity) {
                if (m.write(prim + (uint64_t)o.primitiveAngularVelocity, po.ang))
                    writes++;
            }
        }

        if (po.done) {
            // Non-loop preset finished: hand the seed back once, then wait for
            // the toggle to go off before arming again.
            restoreSeed(m, o, seed);
            seed.valid = false;
            {
                std::lock_guard<std::mutex> lk(g_seedMu);
                g_sharedValid = false;
            }
            g_state.store(0, std::memory_order_relaxed);
            g_sub.store(-1, std::memory_order_relaxed);
            g_rate.store(0.0f, std::memory_order_relaxed);
            g_rearm.store(true, std::memory_order_relaxed);
            continue;
        }
        Sleep(0);
    }
}

void spawnThread() {
    if (g_started) return;
    g_run.store(true, std::memory_order_relaxed);
    g_th = std::thread(threadBody);
    g_started = true;
}

} // namespace

namespace Stunt {

void start(Memory* m, const Offsets* o) {
    if (m) g_mem = m;
    if (o) g_off = o;
    spawnThread();
}

void publish(Memory& m, const Offsets& o, uint64_t selfPrim, uint64_t selfHum,
             uint64_t camAddr, const StuntParams& p, bool on) {
    g_mem = &m;
    g_off = &o;
    if (!on) {
        g_rearm.store(false, std::memory_order_relaxed);
        g_on.store(0, std::memory_order_relaxed);
        return;
    }
    spawnThread();
    if (selfPrim) g_prim.store(selfPrim, std::memory_order_relaxed);
    if (selfHum) g_hum.store(selfHum, std::memory_order_relaxed);
    if (camAddr) g_cam.store(camAddr, std::memory_order_relaxed);
    g_target.store(p.targetPrim, std::memory_order_relaxed);
    g_preset.store(p.preset, std::memory_order_relaxed);
    g_tscale.store(p.timeScale, std::memory_order_relaxed);
    g_amp.store(p.amp, std::memory_order_relaxed);
    g_ragdoll.store(p.ragdoll, std::memory_order_relaxed);
    g_loop.store(p.loop, std::memory_order_relaxed);
    g_hover.store(p.hover, std::memory_order_relaxed);
    g_mode.store(p.mode, std::memory_order_relaxed);
    g_on.store(1, std::memory_order_relaxed);
}

void stop(Memory& m, const Offsets& o) {
    g_on.store(0, std::memory_order_relaxed);
    g_rearm.store(false, std::memory_order_relaxed);
    if (g_started) {
        g_run.store(false, std::memory_order_relaxed);
        if (g_th.joinable()) g_th.join();
        g_started = false;
    }
    StuntSeed s;
    {
        std::lock_guard<std::mutex> lk(g_seedMu);
        s = g_shared;
        s.valid = s.valid && g_sharedValid;
        g_sharedValid = false;
    }
    if (s.valid) restoreSeed(m, o, s);
    g_state.store(0, std::memory_order_relaxed);
    g_sub.store(-1, std::memory_order_relaxed);
    g_rate.store(0.0f, std::memory_order_relaxed);
    g_run.store(true, std::memory_order_relaxed); // a later start() still works
}

bool active() { return g_on.load(std::memory_order_relaxed) != 0; }
float writesPerSec() { return g_rate.load(std::memory_order_relaxed); }
int stateId() { return g_state.load(std::memory_order_relaxed); }
int presetRuntime() { return g_sub.load(std::memory_order_relaxed); }
const char* presetName(int i) {
    if (i < 0 || i >= kPresetCount) return "?";
    return kPresets[i].name;
}
int presetCount() { return kPresetCount; }

} // namespace Stunt

// -------------------------------------------------------------- self-test
namespace {

int g_testFail = 0;

void check(bool ok, const char* what, float detail = 0.0f) {
    if (!ok) {
        printf("FAIL: %s (%.6f)\n", what, (double)detail);
        ++g_testFail;
    }
}

// Column dot products + determinant of the stored 9-float basis.
void basisError(const float m[9], float& maxDot, float& det) {
    const float rx = m[0], ry = m[1], rz = m[2];
    const float ux = m[3], uy = m[4], uz = m[5];
    const float bx = m[6], by = m[7], bz = m[8];
    const float nr = sqrtf(rx * rx + ry * ry + rz * rz);
    const float nu = sqrtf(ux * ux + uy * uy + uz * uz);
    const float nb = sqrtf(bx * bx + by * by + bz * bz);
    const float d1 = (rx * ux + ry * uy + rz * uz) / (nr * nu);
    const float d2 = (rx * bx + ry * by + rz * bz) / (nr * nb);
    const float d3 = (ux * bx + uy * by + uz * bz) / (nu * nb);
    maxDot = fmaxf(fabsf(d1), fmaxf(fabsf(d2), fabsf(d3)));
    maxDot = fmaxf(maxDot, fmaxf(fabsf(nr - 1.0f),
                                 fmaxf(fabsf(nu - 1.0f), fabsf(nb - 1.0f))));
    det = rx * (uy * bz - uz * by) - ux * (ry * bz - rz * by) +
          bx * (ry * uz - rz * uy);
}

} // namespace

int runStuntSelfTest() {
    g_testFail = 0;

    // 1) eulerToBasis is a proper rotation for a spread of angles.
    {
        const float angles[][3] = {
            { 0.0f, 0.0f, 0.0f },     { 1.5707964f, 0.0f, 0.0f },
            { 0.0f, 1.5707964f, 0.0f }, { 0.0f, 0.0f, 1.5707964f },
            { -0.6109f, 0.3f, -1.2f }, { 2.7f, -1.1f, 0.55f },
            { -3.1f, 1.4f, 2.9f },
        };
        float worst = 0.0f, worstDet = 0.0f;
        int n = (int)(sizeof(angles) / sizeof(angles[0]));
        for (int i = 0; i < n; ++i) {
            float m[9];
            eulerToBasis(angles[i][0], angles[i][1], angles[i][2], m);
            float dot = 0.0f, det = 0.0f;
            basisError(m, dot, det);
            worst = fmaxf(worst, dot);
            worstDet = fmaxf(worstDet, fabsf(det - 1.0f));
            check(finite9(m), "euler basis finite");
        }
        printf("stunttest: basis orthonormal maxErr=%.6f |det-1|max=%.6f\n",
               (double)worst, (double)worstDet);
        check(worst < 1e-4f, "euler basis columns not orthonormal", worst);
        check(worstDet < 1e-4f, "euler basis determinant != +1", worstDet);
    }

    // 2) basisToEuler is the inverse (round trip).
    {
        float worst = 0.0f;
        const float angles[][3] = {
            { 0.2f, -0.4f, 0.9f }, { -2.2f, 0.7f, -1.5f }, { 1.0f, 0.0f, 0.0f },
        };
        for (const auto& a : angles) {
            float m[9], m2[9];
            eulerToBasis(a[0], a[1], a[2], m);
            float y = 0, p = 0, r = 0;
            check(basisToEuler(m, y, p, r), "basisToEuler failed");
            eulerToBasis(y, p, r, m2);
            for (int i = 0; i < 9; ++i)
                worst = fmaxf(worst, fabsf(m[i] - m2[i]));
        }
        printf("stunttest: euler roundtrip maxErr=%.6f\n", (double)worst);
        check(worst < 1e-4f, "basisToEuler roundtrip mismatch", worst);
    }

    // 3) easing boundaries.
    {
        printf("stunttest: ease(0)=%.3f ease(0.5)=%.3f ease(1)=%.3f\n",
               (double)ease(0.0f), (double)ease(0.5f), (double)ease(1.0f));
        check(fabsf(ease(0.0f) - 0.0f) < 1e-6f, "ease(0) != 0", ease(0.0f));
        check(fabsf(ease(1.0f) - 1.0f) < 1e-6f, "ease(1) != 1", ease(1.0f));
        check(fabsf(ease(0.5f) - 0.5f) < 1e-6f, "ease(0.5) != 0.5", ease(0.5f));
        float prev = -1.0f;
        for (int i = 0; i <= 10; ++i) {
            float v = ease((float)i / 10.0f);
            check(v >= prev, "ease not monotonic", v);
            prev = v;
        }
        check(ease(-2.0f) == 0.0f, "ease clamp low", ease(-2.0f));
        check(ease(3.0f) == 1.0f, "ease clamp high", ease(3.0f));
    }

    // 4) every preset samples without NaN and with a usable pose.
    {
        StuntSeed s;
        s.valid = true;
        s.prim = 0x1000;
        s.humanoid = 0x2000;
        s.pos = Vec3{ 10.0f, 5.0f, -3.0f };
        eulerToBasis(0.4f, -0.2f, 0.1f, s.rot);
        s.vel = Vec3{ 1, 2, 3 };
        StuntEnv env;
        env.fwd = Vec3{ 0, 0, 1 };
        env.right = Vec3{ 1, 0, 0 };
        env.tpos = Vec3{ 20.0f, 6.0f, 4.0f };
        eulerToBasis(1.1f, 0.0f, 0.0f, env.trot);
        env.haveTarget = true;
        StuntParams p;
        int samples = 0;
        for (int i = 0; i < Stunt::presetCount(); ++i) {
            p.preset = i;
            const float dur = kPresets[i].dur > 0.0f ? kPresets[i].dur : 1.0f;
            const float ts[5] = { 0.0f, 0.05f, dur * 0.33f, dur * 0.5f, dur * 1.5f };
            for (float t : ts) {
                StuntInteg ig;
                StuntPose po = computePose(i, t, s, p, env, 1.0f / 60.0f, &ig);
                ++samples;
                check(finite3(po.vel), "preset vel NaN");
                check(finite3(po.ang), "preset ang NaN");
                check(finite3(po.pos), "preset pos NaN");
                check(finite9(po.rot), "preset rot NaN");
                if (po.useRot) {
                    float dot = 0.0f, det = 0.0f;
                    basisError(po.rot, dot, det);
                    check(dot < 1e-3f, Stunt::presetName(i), dot);
                    check(fabsf(det - 1.0f) < 1e-3f, "preset rot det", det);
                }
                const bool usable = po.dyn || po.useRot || po.usePos || po.flags != 0;
                check(usable, "preset does nothing");
                if (i == 8 || i == 9) check(po.usePos || !env.haveTarget,
                                            "follow preset ignored target");
                check(po.alt == po.alt && fabsf(po.alt) <= 20.0f, "alt out of range",
                      po.alt);
                if (po.useHip)
                    check(po.hip == po.hip && po.hip >= 0.0f && po.hip <= 20.0f,
                          "hip out of range", po.hip);
            }
        }
        printf("stunttest: presets=%d samples=%d\n", Stunt::presetCount(), samples);
    }

    // 5) kinematic integration (stunt.mode=1) stays a proper rotation.
    {
        StuntSeed s;
        s.valid = true;
        s.pos = Vec3{ 0, 0, 0 };
        eulerToBasis(0.0f, 0.0f, 0.0f, s.rot);
        StuntParams p;
        p.preset = 0; // backflip
        p.mode = 1;
        StuntEnv env;
        StuntInteg ig;
        float worst = 0.0f;
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 120; ++i) {
            StuntPose po = computePose(0, (float)i * dt, s, p, env, dt, &ig);
            float dot = 0.0f, det = 0.0f;
            basisError(po.rot, dot, det);
            worst = fmaxf(worst, fmaxf(dot, fabsf(det - 1.0f)));
            check(po.useRot, "kinematic mode did not write rotation");
            check(fabsf(po.ang.x) < 1e-6f, "kinematic mode kept angular velocity");
        }
        printf("stunttest: kinematic integration 120 steps maxErr=%.6f\n",
               (double)worst);
        check(worst < 1e-3f, "kinematic integration drifted", worst);
    }

    // 6) restore plan hands the seed back unchanged, with zero motion.
    {
        StuntSeed s;
        s.valid = true;
        s.prim = 0xABC000;
        s.humanoid = 0xDEF000;
        s.pos = Vec3{ 1.5f, 2.5f, 3.5f };
        s.vel = Vec3{ 9, 8, 7 };
        s.ang = Vec3{ 6, 5, 4 };
        eulerToBasis(0.7f, -0.3f, 1.2f, s.rot);
        s.flags = 0x230;
        s.haveFlags = true;
        s.platformStand = 0;
        s.sit = 0;
        s.eval = 1;
        s.autoRotate = 1;
        s.hipHeight = 2.0f;
        RestorePlan r = makeRestorePlan(s);
        bool same = true;
        for (int i = 0; i < 9; ++i) same = same && (r.rot[i] == s.rot[i]);
        same = same && r.pos.x == s.pos.x && r.pos.y == s.pos.y &&
               r.pos.z == s.pos.z;
        same = same && r.vel.x == 0 && r.vel.y == 0 && r.vel.z == 0;
        same = same && r.ang.x == 0 && r.ang.y == 0 && r.ang.z == 0;
        same = same && r.flags == s.flags && r.haveFlags;
        same = same && r.platformStand == s.platformStand && r.sit == s.sit &&
               r.eval == s.eval && r.autoRotate == s.autoRotate;
        same = same && r.hipHeight == s.hipHeight;
        printf("stunttest: restore plan seed match=%s\n", same ? "yes" : "no");
        check(same, "restore plan did not reproduce the seed");
    }

    if (g_testFail) {
        printf("stunttest: FAIL (%d)\n", g_testFail);
        return 1;
    }
    printf("stunttest: PASS\n");
    return 0;
}
