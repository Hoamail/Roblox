#pragma once
// Fly module (Body tab): high-rate AssemblyLinearVelocity writer.
// The tab only publishes the target; the writes happen in a dedicated thread
// because the engine refreshes the primitive velocity every rendered frame
// (see the long comment in ovl_fly.cpp for the measurements).
#include "sdk.h"

namespace Fly {

// Spawn the writer thread. Call once at startup, after openWrite().
void start(Memory* m, int camRotOff, int velOff);

// Stop the thread and drop the leftover momentum (call before join/exit).
void stop(Memory& m, const Offsets& o);

// Called once per overlay frame. `on` = fly armed AND toggled AND game focused.
// Falling edge (on -> off) zeroes the velocity once, so unticking fly never
// leaves the character drifting. Safe to call every frame.
void publish(Memory& m, const Offsets& o, uint64_t selfPrim, uint64_t camAddr,
             float speed, bool on);

// Zero the held velocity once (quit paths / panic).
void killVelocity(Memory& m, const Offsets& o);

// UI + diag: is the writer currently holding a velocity, and how fast is it
// writing (writes/s, updated about once per millisecond).
bool active();
float writesPerSec();

} // namespace Fly
