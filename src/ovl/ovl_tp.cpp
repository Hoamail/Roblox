// Teleport writer (Teleport tab). See ovl_tp.h for the mechanism.
//
// This is the Stunt "Piggyback" mechanism, not the Fly mechanism: the writer
// owns an absolute target position and re-writes Primitive.Position every pass
// at the same Sleep(0) cadence as Fly. Steering with AssemblyLinearVelocity
// (the previous implementation) could not cross a wall - the solver stopped
// the assembly at the first collider. A continuously re-applied absolute
// position has no collision response to lose against: the character is placed
// there, the physics step moves it, the next pass places it again. Stunt's
// Piggyback does exactly this to ride a target through geometry.
//
// "Arrived" is not derived from the virtual position (that only proves the
// writer advanced its own bookkeeping). It requires reading the live
// Primitive.Position back and seeing the written point hold for several
// samples; otherwise the result is -2 (rejected), never a false "arrived".
#include "ovl_tp.h"
#include "ovl_core.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

const int kMaxParts = Tp::kTpMaxGhostParts + 1;

// Distance at which the virtual position is considered on target, and the
// tolerance for the read-back proof (the solver may nudge the assembly by a
// fraction of a stud while the write is re-applied).
const float kReach = 1.0f;
const float kReadbackTol = 2.5f;

// Instant-snap proof. The engine reverts a position write 10-50 ms after the
// fact, so a read-back taken a few microseconds after the write only proves
// that the bytes landed, not that the character stayed. Sequence:
//   1. keep writing the exact target for kHoldSec (render + physics step see
//      it; the revert is overwritten before it can be visible)
//   2. stop writing and wait kReleaseSec with the engine in control
//   3. read the live field once: if it still sits near the point we held, the
//      teleport is real; otherwise the engine (or the server) pushed the
//      character back and the result is -2 "blocked", never a false "arrived".
const double kHoldSec = 0.20;
const double kReleaseSec = 0.15;
const float kHoldTol = 6.0f;

std::atomic<bool> g_run{ true };
std::atomic<int> g_on{ 0 };
std::atomic<int> g_cancel{ 0 };
std::atomic<int> g_result{ 0 };       // 1 arrived, -1 timeout, -2 write rejected

std::atomic<uint64_t> g_prim{ 0 };
std::atomic<uint64_t> g_track{ 0 };
std::atomic<float> g_tx{ 0 }, g_ty{ 0 }, g_tz{ 0 };
// Virtual position: the absolute point this writer keeps placing the root at.
// Starts from the live position on request and advances at `tp.speed`.
std::atomic<float> g_cx{ 0 }, g_cy{ 0 }, g_cz{ 0 };
std::atomic<float> g_speed{ 300.0f };
std::atomic<int> g_instant{ 1 };
std::atomic<double> g_deadline{ 0.0 };
std::atomic<uint64_t> g_seq{ 0 };
std::atomic<float> g_rate{ 0.0f };

// CanCollide pinning (pass through walls instead of letting the physics step
// resolve the character out of geometry between two writes). Position writes
// already bypass collision; this just reduces the correction work.
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

// `seq` is the generation the writer was working on. Under g_flagsMu, a newer
// request() wins: without the check a writer that finished sampling just
// before a retarget could restore the *new* dash's collision bits and clear
// its g_on (the retarget would silently do nothing).
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
    unsigned tick = 0;
    long writes = 0;
    double sliceStart = qpcSeconds();
    double lastStep = sliceStart;
    double reachedAt = 0.0;
    int confirm = 0;
    // Instant-snap hold/release state. `hold*` is the point that was actually
    // held, not the target: with a tracked player the target keeps moving and
    // comparing against it after release would report a false miss.
    bool inRelease = false;
    double releaseAt = 0.0;
    float holdX = 0.0f, holdY = 0.0f, holdZ = 0.0f;
    uint64_t seenSeq = 0;

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
        // A retarget is a new run: the previous hold/release bookkeeping must
        // not make the fresh request finish on the first pass.
        if (seq != seenSeq) {
            seenSeq = seq;
            reachedAt = 0.0;
            confirm = 0;
            inRelease = false;
            releaseAt = 0.0;
        }

        // Re-aim at a moving player (target HRP) once per millisecond, and
        // refresh the rate counter on the same cadence.
        if ((tick & 1023u) == 0) {
            uint64_t tr = g_track.load(std::memory_order_acquire);
            if (tr) {
                Vec3 tp{};
                if (g_mem->read(tr + (uint64_t)g_posOff, tp) && finite3(tp)) {
                    g_tx.store(tp.x, std::memory_order_release);
                    g_ty.store(tp.y + 3.0f, std::memory_order_release);
                    g_tz.store(tp.z, std::memory_order_release);
                }
            }
            double nowR = qpcSeconds();
            double dtR = nowR - sliceStart;
            if (dtR > 0.0)
                g_rate.store((float)(writes / dtR), std::memory_order_release);
            sliceStart = nowR;
            writes = 0;
        }
        ++tick;

        double now = qpcSeconds();
        double dt = now - lastStep;
        lastStep = now;
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.1) dt = 0.1; // a stalled scheduler must not overshoot

        float tx = g_tx.load(std::memory_order_acquire);
        float ty = g_ty.load(std::memory_order_acquire);
        float tz = g_tz.load(std::memory_order_acquire);
        Vec3 cur{ g_cx.load(std::memory_order_acquire),
                  g_cy.load(std::memory_order_acquire),
                  g_cz.load(std::memory_order_acquire) };
        if (!finite3(cur)) {
            finishDash(*g_mem, -1, seq);
            continue;
        }
        float dx = tx - cur.x, dy = ty - cur.y, dz = tz - cur.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(dist)) {
            finishDash(*g_mem, -1, seq);
            continue;
        }

        // Sample the LIVE field before this pass's write: whatever the engine
        // did with the previous write is now visible. This is what separates a
        // real arrival from the old "virtual bookkeeping reached the target"
        // false positive, where the engine had rejected every write and the
        // character never moved.
        bool liveOk = false;
        float liveDist = 1.0e9f;
        if ((tick & 31u) == 0) {
            Vec3 live{};
            liveOk = g_mem->read(prim + (uint64_t)g_posOff, live) &&
                     finite3(live);
            if (liveOk) {
                float lx = live.x - tx, ly = live.y - ty, lz = live.z - tz;
                liveDist = sqrtf(lx * lx + ly * ly + lz * lz);
            }
        }

        const bool instant = g_instant.load(std::memory_order_acquire) != 0;
        bool onTarget = false;
        if (instant) {
            // Stunt Piggyback: place the root AT the target right now, every
            // pass. No interpolation, no path, no obstacle interaction - the
            // next engine step starts from the point we wrote.
            cur.x = tx;
            cur.y = ty;
            cur.z = tz;
            onTarget = true;
        } else {
            float spd = g_speed.load(std::memory_order_acquire);
            if (!std::isfinite(spd) || spd < 60.0f) spd = 60.0f;
            if (spd > 1200.0f) spd = 1200.0f;
            if (dist > kReach) {
                // Gradual mode: advance the virtual point at tp.speed and place
                // the root there. The write is absolute, so the engine's
                // reversion of the previous sample costs nothing.
                float step = spd * (float)dt;
                if (step < 1.0e-5f) step = 1.0e-5f;
                if (step > dist) step = dist;
                float k = step / dist;
                cur.x += dx * k;
                cur.y += dy * k;
                cur.z += dz * k;
                reachedAt = 0.0;
                confirm = 0;
            } else {
                cur.x = tx;
                cur.y = ty;
                cur.z = tz;
                onTarget = true;
            }
        }
        // Instant snap: hold the written point for kHoldSec, then let go and
        // prove it stuck. During the release the writer must not touch the
        // position any more, otherwise the sample below proves nothing.
        bool holdPos = !(instant && inRelease);
        if (onTarget && instant) {
            if (reachedAt == 0.0) {
                reachedAt = now;
                holdX = cur.x;
                holdY = cur.y;
                holdZ = cur.z;
            } else if (!inRelease) {
                holdX = cur.x;
                holdY = cur.y;
                holdZ = cur.z;
                if (now - reachedAt >= kHoldSec) {
                    inRelease = true;
                    releaseAt = now;
                    holdPos = false;
                }
            }
        }

        g_cx.store(cur.x, std::memory_order_release);
        g_cy.store(cur.y, std::memory_order_release);
        g_cz.store(cur.z, std::memory_order_release);

        if (holdPos) {
            Vec3 out{ cur.x, cur.y, cur.z };
            if (g_mem->write(prim + (uint64_t)g_posOff, out)) ++writes;
        }
        zeroVelocity(*g_mem, prim); // no residual momentum on release

        if (onTarget) {
            if (!instant) {
                // Gradual mode: the virtual point is on target, sample the live
                // field until the engine has carried the character there.
                if ((tick & 31u) == 0) {
                    if (liveOk && liveDist <= kReadbackTol)
                        ++confirm;
                    else
                        confirm = 0;
                    if (confirm >= 3) {
                        if (g_seq.load(std::memory_order_acquire) == seq)
                            finishDash(*g_mem, 1, seq);
                        continue;
                    }
                    if ((!liveOk || liveDist > kReadbackTol) &&
                        now - reachedAt > 0.75) {
                        if (g_seq.load(std::memory_order_acquire) == seq)
                            finishDash(*g_mem, -2, seq);
                        continue;
                    }
                }
            } else if (inRelease && now - releaseAt >= kReleaseSec) {
                // The engine has had the position for kReleaseSec with the
                // writer silent: whatever it did is the truth now.
                Vec3 live{};
                bool ok = g_mem->read(prim + (uint64_t)g_posOff, live) &&
                          finite3(live);
                float d = 1.0e9f;
                if (ok) {
                    float lx = live.x - holdX, ly = live.y - holdY,
                          lz = live.z - holdZ;
                    d = sqrtf(lx * lx + ly * ly + lz * lz);
                }
                if (g_seq.load(std::memory_order_acquire) == seq)
                    finishDash(*g_mem, (ok && d <= kHoldTol) ? 1 : -2, seq);
                continue;
            }
        }

        if (now > g_deadline.load(std::memory_order_acquire)) {
            if (g_seq.load(std::memory_order_acquire) == seq)
                finishDash(*g_mem, -1, seq);
            continue;
        }
        Sleep(0);
    }
    timeEndPeriod(1);
}

} // namespace

namespace Tp {

void start(Memory* m, int posOff, int velOff, int flagsOff,
           uint32_t canCollideBit) {
    if (g_started || !m || !posOff) return;
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
             const uint64_t* ghostParts, int ghostCount, bool instant) {
    if (!g_started || !selfPrim || !o.primitivePosition) return false;
    if (!finite3(dst)) return false;
    Vec3 self{};
    if (!m.read(selfPrim + (uint64_t)o.primitivePosition, self) ||
        !finite3(self))
        return false;

    std::lock_guard<std::mutex> lk(g_flagsMu);
    // A retarget while a run is active keeps the existing snapshots so the
    // original (pre-run) CanCollide value is not lost. If the self primitive
    // changed (respawn), restore the old one first.
    bool sameRun = g_on.load(std::memory_order_acquire) != 0 &&
                   g_partCount > 0 && g_parts[0] == selfPrim;
    if (g_on.load(std::memory_order_acquire) && !sameRun && g_partCount > 0) {
        restoreFlags(m);
    }
    if (!sameRun) {
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
    // Start the virtual point from where the character actually is, so a
    // retarget right after a previous run does not jump the intermediate path.
    g_cx.store(self.x, std::memory_order_release);
    g_cy.store(self.y, std::memory_order_release);
    g_cz.store(self.z, std::memory_order_release);
    g_speed.store(speed, std::memory_order_release);
    g_instant.store(instant ? 1 : 0, std::memory_order_release);

    float dx = dst.x - self.x, dy = dst.y - self.y, dz = dst.z - self.z;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    // Instant mode is one placement, so it either sticks immediately or the
    // engine rejects it; gradual mode gets a budget proportional to distance.
    float sp = speed > 60.0f ? speed : 60.0f;
    double budget = instant ? 3.0 : 4.0 + (double)(dist / sp) * 2.0;
    if (!(budget >= 3.0)) budget = 3.0;
    if (budget > 60.0) budget = 60.0;
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
