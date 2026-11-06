// Spectate writer (Teleport tab). See ovl_spectate.h for the mechanism.
//
// The writer is deliberately cheap: it only writes when the field no longer
// holds the requested Humanoid, so a game that leaves CameraSubject alone costs
// one 8-byte read per millisecond. A game that re-assigns it every frame gets
// the target written back within ~1 ms, which is enough to win the camera
// module's next update.
#include "ovl_spectate.h"
#include "ovl_core.h"
#include <atomic>
#include <thread>

namespace {

std::atomic<bool> g_run{ true };
std::atomic<int> g_on{ 0 };
std::atomic<int> g_restore{ 0 };   // 1 = falling edge, write g_self once
std::atomic<uint64_t> g_cam{ 0 };
std::atomic<uint64_t> g_subj{ 0 };
std::atomic<uint64_t> g_self{ 0 };

Memory* g_mem = nullptr;
int g_off = 0;
std::thread g_th;
bool g_started = false;
bool g_wasOn = false;

void writeSubject(uint64_t cam, uint64_t subj) {
    if (!g_mem || !g_mem->h || !g_off || !cam) return;
    // A 0 here is a valid "nil CameraSubject" (the default camera module
    // guards it). It only happens when the local Humanoid could not be
    // resolved at stop time.
    if (!g_mem->write(cam + (uint64_t)g_off, subj))
        diagRate("spectate-write", 1000,
                 "spectate write failed cam=%p subject=%p", (void*)cam,
                 (void*)subj);
}

void threadBody() {
    timeBeginPeriod(1);
    while (g_run.load(std::memory_order_relaxed)) {
        if (!g_on.load(std::memory_order_relaxed)) {
            if (g_restore.exchange(0, std::memory_order_acq_rel)) {
                writeSubject(g_cam.load(std::memory_order_acquire),
                             g_self.load(std::memory_order_acquire));
            }
            Sleep(5);
            continue;
        }
        uint64_t cam = g_cam.load(std::memory_order_acquire);
        uint64_t subj = g_subj.load(std::memory_order_acquire);
        if (!cam || !subj) {
            Sleep(1);
            continue;
        }
        uint64_t cur = 0;
        if (g_mem->read(cam + (uint64_t)g_off, cur) && cur == subj) {
            Sleep(1);
            continue;
        }
        // Only touch the field when the game moved it: this keeps the thread
        // idle unless something is actually fighting us.
        if (!g_mem->write(cam + (uint64_t)g_off, subj))
            diagRate("spectate-write", 1000,
                     "spectate write failed cam=%p subject=%p", (void*)cam,
                     (void*)subj);
        Sleep(1);
    }
    timeEndPeriod(1);
}

} // namespace

namespace Spectate {

void start(Memory* m, int subjectOff) {
    if (g_started || !m || !subjectOff) return;
    g_mem = m;
    g_off = subjectOff;
    g_run.store(true, std::memory_order_relaxed);
    g_th = std::thread(threadBody);
    g_started = true;
}

void stop(Memory& m) {
    if (!g_started) {
        // No thread (missing offset / read-only session): a direct restore is
        // still better than leaving the camera on someone else.
        uint64_t cam = g_cam.load(std::memory_order_acquire);
        uint64_t self = g_self.load(std::memory_order_acquire);
        if (cam && g_off && m.h) m.write(cam + (uint64_t)g_off, self);
        return;
    }
    g_on.store(0, std::memory_order_release);
    g_restore.store(0, std::memory_order_release);
    g_run.store(false, std::memory_order_release);
    if (g_th.joinable()) g_th.join();
    uint64_t cam = g_cam.load(std::memory_order_acquire);
    uint64_t self = g_self.load(std::memory_order_acquire);
    if (cam && g_off && m.h) m.write(cam + (uint64_t)g_off, self);
    g_started = false;
    g_wasOn = false;
}

void publish(uint64_t camAddr, uint64_t subject, uint64_t selfSubject, bool on) {
    if (selfSubject) g_self.store(selfSubject, std::memory_order_release);
    if (on && camAddr && subject) {
        g_cam.store(camAddr, std::memory_order_release);
        g_subj.store(subject, std::memory_order_release);
        g_restore.store(0, std::memory_order_release);
        g_on.store(1, std::memory_order_release);
        g_wasOn = true;
    } else if (g_wasOn) {
        // Falling edge (user pressed stop, target vanished, panic, ...):
        // put the local character back into the camera subject slot.
        g_on.store(0, std::memory_order_release);
        g_restore.store(1, std::memory_order_release);
        g_wasOn = false;
        if (!g_started) writeSubject(g_cam.load(std::memory_order_acquire),
                                     g_self.load(std::memory_order_acquire));
    }
}

bool active() { return g_on.load(std::memory_order_acquire) != 0; }

uint64_t subject() { return g_subj.load(std::memory_order_acquire); }

} // namespace Spectate
