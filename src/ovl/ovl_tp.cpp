// Guided velocity dash (Teleport tab). See ovl_tp.h for the rationale.
//
// The writer holds AssemblyLinearVelocity toward the destination at the same
// Sleep(0) cadence as Fly (~1e6 writes/s). At 628 writes/s the solver simply
// overwrites the value and the character does not move (measured), so a slow
// writer would be just another version of the old "spam TP" bug.
#include "ovl_tp.h"
#include "ovl_core.h"
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

namespace {

const int kMaxParts = Tp::kTpMaxGhostParts + 1;

std::atomic<bool> g_run{ true };
std::atomic<int> g_on{ 0 };
std::atomic<int> g_cancel{ 0 };
std::atomic<int> g_result{ 0 };       // 1 arrived, -1 timeout/cancel

std::atomic<uint64_t> g_prim{ 0 };
std::atomic<uint64_t> g_track{ 0 };
std::atomic<float> g_tx{ 0 }, g_ty{ 0 }, g_tz{ 0 };
std::atomic<float> g_speed{ 300.0f };
std::atomic<double> g_deadline{ 0.0 };
std::atomic<uint64_t> g_seq{ 0 };
std::atomic<float> g_rate{ 0.0f };

// CanCollide pinning (pass through walls instead of grinding along them).
// Plain arrays guarded by g_flagsMu: only request() and the writer's finish
// touch them, and both need the whole set consistent.
std::mutex g_flagsMu;
uint64_t g_parts[kMaxParts]{};
uint32_t g_partFlags[kMaxParts]{};
int g_partCount = 0;

Memory* g_mem = nullptr;
int g_posOff = 0;
int g_velOff = 0;
int g_flagsOff = 0;
uint32_t g_ccBit = 0;
std::thread g_th;
bool g_started = false;

bool finite3(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

void zeroVelocity(Memory& m, uint64_t prim) {
    if (prim && g_velOff) {
        Vec3 z{ 0, 0, 0 };
        m.write(prim + (uint64_t)g_velOff, z);
    }
}

// Caller holds g_flagsMu (or the writer is the only user during shutdown).
void restoreFlags(Memory& m) {
    for (int i = 0; i < g_partCount; ++i) {
        uint64_t prim = g_parts[i];
        uint32_t orig = g_partFlags[i];
        if (!prim || !g_flagsOff) continue;
        // Re-read so another feature (or the game) can own the other bits; only
        // CanCollide is put back to its pre-dash value.
        uint32_t cur = 0;
        if (m.read(prim + (uint64_t)g_flagsOff, cur)) {
            if (orig & g_ccBit)
                cur |= g_ccBit;
            else
                cur &= ~g_ccBit;
            m.write(prim + (uint64_t)g_flagsOff, cur);
        } else {
            m.write(prim + (uint64_t)g_flagsOff, orig);
        }
    }
    g_partCount = 0;
}

// `seq` is the dash generation the writer was working on. Under g_flagsMu, a
// newer request() wins: without the check a writer that finished sampling just
// before a retarget could restore the *new* dash's collision bits and clear its
// g_on (the retarget would silently do nothing).
void finishDash(Memory& m, int result, uint64_t seq) {
    std::lock_guard<std::mutex> lk(g_flagsMu);
    if (seq != g_seq.load(std::memory_order_acquire)) return;
    zeroVelocity(m, g_prim.load(std::memory_order_acquire));
    restoreFlags(m);
    g_on.store(0, std::memory_order_release);
    g_result.store(result, std::memory_order_release);
    g_rate.store(0.0f, std::memory_order_release);
}

void threadBody() {
    timeBeginPeriod(1);
    Vec3 pos{};
    bool havePos = false;
    unsigned tick = 0;
    long writes = 0;
    double sliceStart = qpcSeconds();

    while (g_run.load(std::memory_order_relaxed)) {
        if (g_cancel.exchange(0, std::memory_order_acq_rel)) {
            if (g_on.load(std::memory_order_acquire) && g_mem)
                finishDash(*g_mem, -1, g_seq.load(std::memory_order_acquire));
            continue;
        }
        if (!g_on.load(std::memory_order_acquire)) {
            Sleep(5);
            continue;
        }
        if (!g_mem || !g_mem->h) {
            g_on.store(0, std::memory_order_release);
            continue;
        }
        uint64_t seq = g_seq.load(std::memory_order_acquire);
        uint64_t prim = g_prim.load(std::memory_order_acquire);
        if (!prim) {
            finishDash(*g_mem, -1, seq);
            continue;
        }

        const bool sample = !havePos || (tick & 1023u) == 0;
        if (sample) {
            Vec3 p{};
            if (g_mem->read(prim + (uint64_t)g_posOff, p) && finite3(p)) {
                pos = p;
                havePos = true;
            } else {
                havePos = false;
            }
            // Re-aim at a moving player (target HRP) once per millisecond.
            uint64_t tr = g_track.load(std::memory_order_acquire);
            if (tr) {
                Vec3 tp{};
                if (g_mem->read(tr + (uint64_t)g_posOff, tp) && finite3(tp)) {
                    g_tx.store(tp.x, std::memory_order_release);
                    g_ty.store(tp.y + 3.0f, std::memory_order_release);
                    g_tz.store(tp.z, std::memory_order_release);
                }
            }
            double now = qpcSeconds();
            double dt = now - sliceStart;
            if (dt > 0.0)
                g_rate.store((float)(writes / dt), std::memory_order_release);
            sliceStart = now;
            writes = 0;
        }
        ++tick;

        if (!havePos) {
            Sleep(1);
            continue;
        }

        float tx = g_tx.load(std::memory_order_acquire);
        float ty = g_ty.load(std::memory_order_acquire);
        float tz = g_tz.load(std::memory_order_acquire);
        float dx = tx - pos.x, dy = ty - pos.y, dz = tz - pos.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);

        if (!std::isfinite(dist) || dist <= 2.0f) {
            if (g_seq.load(std::memory_order_acquire) == seq)
                finishDash(*g_mem, (std::isfinite(dist) && dist <= 2.0f) ? 1 : -1,
                           seq);
            continue;
        }
        double now = qpcSeconds();
        if (now > g_deadline.load(std::memory_order_acquire)) {
            if (g_seq.load(std::memory_order_acquire) == seq)
                finishDash(*g_mem, -1, seq);
            continue;
        }

        float spd = g_speed.load(std::memory_order_acquire);
        if (!std::isfinite(spd) || spd < 20.0f) spd = 20.0f;
        if (spd > 2000.0f) spd = 2000.0f;
        // Ease off inside the last ~15 studs so 300+ studs/s cannot overshoot.
        if (dist < 15.0f) {
            // NB: `near` is a legacy windows.h macro; do not name it that.
            float cap = 30.0f + dist * 6.0f;
            if (cap < spd) spd = cap;
        }
        float k = spd / dist;
        Vec3 v{ dx * k, dy * k, dz * k };
        if (g_mem->write(prim + (uint64_t)g_velOff, v)) ++writes;
        Sleep(0);
    }
    timeEndPeriod(1);
}

} // namespace

namespace Tp {

void start(Memory* m, int posOff, int velOff, int flagsOff,
           uint32_t canCollideBit) {
    if (g_started || !m || !posOff || !velOff) return;
    g_mem = m;
    g_posOff = posOff;
    g_velOff = velOff;
    g_flagsOff = flagsOff;
    g_ccBit = canCollideBit;
    g_run.store(true, std::memory_order_relaxed);
    g_th = std::thread(threadBody);
    g_started = true;
}

void stop(Memory& m, const Offsets& o) {
    if (!g_started) return;
    g_on.store(0, std::memory_order_release);
    g_run.store(false, std::memory_order_release);
    if (g_th.joinable()) g_th.join();
    uint64_t prim = g_prim.load(std::memory_order_acquire);
    zeroVelocity(m, prim);
    {
        std::lock_guard<std::mutex> lk(g_flagsMu);
        restoreFlags(m);
    }
    (void)o;
    g_started = false;
    g_result.store(0, std::memory_order_release);
    g_rate.store(0.0f, std::memory_order_release);
}

bool request(Memory& m, const Offsets& o, uint64_t selfPrim,
             uint64_t trackPrim, const Vec3& dst, float speed,
             const uint64_t* ghostParts, int ghostCount) {
    if (!g_started || !selfPrim || !o.primitivePosition || !o.primitiveVelocity)
        return false;
    if (!finite3(dst)) return false;

    std::lock_guard<std::mutex> lk(g_flagsMu);
    // A retarget while a dash is running keeps the existing snapshots so the
    // original (pre-dash) CanCollide value is not lost. If the self primitive
    // changed (respawn), restore the old one first.
    bool sameDash = g_on.load(std::memory_order_acquire) != 0 &&
                    g_partCount > 0 && g_parts[0] == selfPrim;
    if (g_on.load(std::memory_order_acquire) && !sameDash && g_partCount > 0) {
        restoreFlags(m);
    }
    if (!sameDash) {
        g_partCount = 0;
        uint32_t fl = 0;
        if (o.primitiveFlags && g_ccBit &&
            m.read(selfPrim + (uint64_t)o.primitiveFlags, fl)) {
            g_parts[g_partCount] = selfPrim;
            g_partFlags[g_partCount] = fl;
            ++g_partCount;
            m.write(selfPrim + (uint64_t)o.primitiveFlags, fl & ~g_ccBit);
        }
        if (o.primitiveFlags && g_ccBit && ghostParts) {
            for (int i = 0; i < ghostCount && g_partCount < kMaxParts; ++i) {
                uint64_t p = ghostParts[i];
                if (!p || p == selfPrim) continue;
                bool dup = false;
                for (int k = 0; k < g_partCount; ++k)
                    if (g_parts[k] == p) { dup = true; break; }
                if (dup) continue;
                uint32_t f2 = 0;
                if (!m.read(p + (uint64_t)o.primitiveFlags, f2)) continue;
                g_parts[g_partCount] = p;
                g_partFlags[g_partCount] = f2;
                ++g_partCount;
                m.write(p + (uint64_t)o.primitiveFlags, f2 & ~g_ccBit);
            }
        }
    }

    g_seq.fetch_add(1, std::memory_order_acq_rel);
    g_cancel.store(0, std::memory_order_release);
    g_prim.store(selfPrim, std::memory_order_release);
    g_track.store(trackPrim, std::memory_order_release);
    g_tx.store(dst.x, std::memory_order_release);
    g_ty.store(dst.y, std::memory_order_release);
    g_tz.store(dst.z, std::memory_order_release);
    g_speed.store(speed, std::memory_order_release);
    // Timeout scales with the distance so a long cross-map dash is not clipped
    // at a fixed 15 s (300 u/s default would cap the useful range at ~4.5k).
    float dist = 0.0f;
    Vec3 p{};
    if (o.primitivePosition &&
        m.read(selfPrim + (uint64_t)o.primitivePosition, p) && finite3(p)) {
        float dx = dst.x - p.x, dy = dst.y - p.y, dz = dst.z - p.z;
        dist = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    float sp = speed > 60.0f ? speed : 60.0f;
    double budget = 8.0 + (double)(dist / sp) * 3.0;
    if (!(budget >= 8.0)) budget = 8.0;
    if (budget > 30.0) budget = 30.0;
    g_deadline.store(qpcSeconds() + budget, std::memory_order_release);
    g_result.store(0, std::memory_order_release);
    zeroVelocity(m, selfPrim);
    g_on.store(1, std::memory_order_release);
    return true;
}

void cancel() { g_cancel.store(1, std::memory_order_release); }

bool active() { return g_on.load(std::memory_order_acquire) != 0; }

float writesPerSec() { return g_rate.load(std::memory_order_acquire); }

int takeResult() { return g_result.exchange(0, std::memory_order_acq_rel); }

} // namespace Tp
