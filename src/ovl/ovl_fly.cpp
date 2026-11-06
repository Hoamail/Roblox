// Fly module (Body tab): high-rate AssemblyLinearVelocity writer.
//
// Why this exists at all (measured on version-11f155f3450f4c8e, place
// 85011874799856, character walk=16 jp=50):
//
//   * The game re-derives Primitive.Position / Primitive.AssemblyLinearVelocity
//     from its own simulation state every rendered frame. A write done once per
//     overlay frame is therefore gone before the next physics step: the
//     character never leaves the ground, and the game's own jump (Space) is
//     snapped straight back down. That was the old "Fly" bug.
//   * Primitive.Flags Anchored bit: the write sticks in memory (25/25 samples)
//     but the solver ignores it - the character keeps falling.
//   * Primitive.Position write: reverted in ~10-50 ms, even with the anchored
//     bit set. A burst of 140k writes/s only nets a few studs of jitter.
//   * Primitive.AssemblyLinearVelocity at 628 writes/s (Sleep(1)): 0.0 studs.
//   * Primitive.AssemblyLinearVelocity at ~1.0M writes/s: full climb,
//     +40..46 studs/s at a 40-45 u/s target, vy reads back exactly the target.
//
// So the only reliable external fly is a writer that stays far ahead of the
// per-frame refresh. This is also what the reference external does: a capture
// of velext_v1.exe flying shows Flags=0x230 (not anchored) and
// AssemblyLinearVelocity.y pinned at 129.516 (its fly speed) while y climbs
// smoothly - i.e. velocity, held, not position stepping.
//
// Cost: while fly is active this thread burns one core (Sleep(0) each pass,
// ~1e6 writes/s). When fly is off it parks on Sleep(10).
#include "ovl_fly.h"
#include "ovl_core.h"
#include <atomic>
#include <thread>
#include <cmath>

namespace {

std::atomic<bool> g_run{ true };
std::atomic<int> g_on{ 0 };              // 1 = hold the target velocity
std::atomic<uint64_t> g_prim{ 0 };       // self HumanoidRootPart primitive
std::atomic<uint64_t> g_cam{ 0 };        // camera instance
std::atomic<float> g_spd{ 60.0f };       // studs/s
std::atomic<float> g_rate{ 0.0f };       // writes/s (UI + diag)
std::atomic<long> g_slices{ 0 };         // sampling slices, diag only

Memory* g_mem = nullptr;
int g_camRotOff = 0;
int g_velOff = 0;
std::thread g_th;
bool g_started = false;
bool g_wasOn = false;

void holdZeroVelocity(Memory& m, const Offsets& o) {
    uint64_t prim = g_prim.load(std::memory_order_relaxed);
    if (prim && o.primitiveVelocity) {
        Vec3 zv{ 0, 0, 0 };
        m.write(prim + (uint64_t)o.primitiveVelocity, zv);
    }
}

// Camera basis + WASD/Space/Ctrl -> the velocity vector to hold.
Vec3 targetVelocity(float fwdX, float fwdZ, float rgtX, float rgtZ) {
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
    } else {
        mx = 0.0f;
        mz = 0.0f;
    }
    float spd = g_spd.load(std::memory_order_relaxed);
    // No key held -> (0,0,0): gravity never accumulates because the value is
    // rewritten before every physics step, so the character hovers in place.
    return Vec3{ mx * spd, my * spd, mz * spd };
}

void threadBody() {
    Vec3 v{};
    unsigned tick = 0;
    long writes = 0;
    double sliceStart = qpcSeconds();
    float fwdX = 0.0f, fwdZ = 1.0f, rgtX = 1.0f, rgtZ = 0.0f;

    while (g_run.load(std::memory_order_relaxed)) {
        if (!g_on.load(std::memory_order_relaxed)) {
            Sleep(10);
            continue;
        }
        uint64_t prim = g_prim.load(std::memory_order_relaxed);
        if (!prim) {
            Sleep(1);
            continue;
        }
        // Camera basis and key state are sampled about once per millisecond
        // (~1024 passes); the rest of the loop is a single 12-byte write,
        // which is what wins the race with the engine refresh.
        if ((++tick & 1023u) == 0) {
            uint64_t cam = g_cam.load(std::memory_order_relaxed);
            if (cam && g_camRotOff) {
                float r[9]{};
                if (g_mem->read(cam + (uint64_t)g_camRotOff, r)) {
                    float lx = -r[2], lz = -r[8];
                    float ll = sqrtf(lx * lx + lz * lz);
                    fwdX = 0.0f;
                    fwdZ = 0.0f;
                    if (ll > 1e-4f) {
                        fwdX = lx / ll;
                        fwdZ = lz / ll;
                    }
                    float rx = r[0], rz = r[6];
                    float rl = sqrtf(rx * rx + rz * rz);
                    rgtX = 0.0f;
                    rgtZ = 0.0f;
                    if (rl > 1e-4f) {
                        rgtX = rx / rl;
                        rgtZ = rz / rl;
                    }
                }
            }
            v = targetVelocity(fwdX, fwdZ, rgtX, rgtZ);

            double now = qpcSeconds();
            double dt = now - sliceStart;
            if (dt > 0.0) g_rate.store((float)(writes / dt), std::memory_order_relaxed);
            sliceStart = now;
            writes = 0;
            g_slices.fetch_add(1, std::memory_order_relaxed);
        }
        if (g_mem->write(prim + (uint64_t)g_velOff, v)) writes++;
        Sleep(0);
    }
}

} // namespace

namespace Fly {

void start(Memory* m, int camRotOff, int velOff) {
    if (g_started) return;
    g_mem = m;
    g_camRotOff = camRotOff;
    g_velOff = velOff;
    g_run.store(true, std::memory_order_relaxed);
    g_th = std::thread(threadBody);
    g_started = true;
}

void stop(Memory& m, const Offsets& o) {
    if (!g_started) return;
    g_on.store(0, std::memory_order_relaxed);
    g_run.store(false, std::memory_order_relaxed);
    if (g_th.joinable()) g_th.join();
    holdZeroVelocity(m, o);
    g_started = false;
    g_wasOn = false;
    g_rate.store(0.0f, std::memory_order_relaxed);
}

void publish(Memory& m, const Offsets& o, uint64_t selfPrim, uint64_t camAddr,
             float speed, bool on) {
    if (on) {
        if (selfPrim) g_prim.store(selfPrim, std::memory_order_relaxed);
        if (camAddr) g_cam.store(camAddr, std::memory_order_relaxed);
        g_spd.store(speed, std::memory_order_relaxed);
        g_on.store(1, std::memory_order_relaxed);
        g_wasOn = true;
    } else if (g_wasOn) {
        // Falling edge: stop holding and drop the leftover momentum once.
        g_on.store(0, std::memory_order_relaxed);
        holdZeroVelocity(m, o);
        g_wasOn = false;
        g_rate.store(0.0f, std::memory_order_relaxed);
    }
}

void killVelocity(Memory& m, const Offsets& o) {
    g_on.store(0, std::memory_order_relaxed);
    g_wasOn = false;
    holdZeroVelocity(m, o);
    g_rate.store(0.0f, std::memory_order_relaxed);
}

bool active() { return g_on.load(std::memory_order_relaxed) != 0; }

float writesPerSec() { return g_rate.load(std::memory_order_relaxed); }

} // namespace Fly
