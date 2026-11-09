#pragma once
// Teleport (Teleport tab): one-click travel to a player/waypoint that ignores
// geometry.
//
// Mechanism = Stunt "Piggyback" (ovl_stunt.cpp, preset 8), not Fly. The writer
// owns an absolute point and re-writes Primitive.Position with it every pass at
// the Fly cadence (Sleep(0)). A velocity dash is what this module used before:
// the solver stops the assembly at the first collider, so it could not cross a
// wall. A continuously re-applied absolute position is placed again after every
// physics step, so collision has nothing to stop - the same reason Piggyback can
// ride a target through geometry.
//
// Arrival is proven, never assumed: the writer reads Primitive.Position back and
// needs the written point to hold for several samples. A write the engine
// rejects returns -2 instead of reporting "arrived".
#include "sdk.h"

namespace Tp {

// Spawn the writer thread. Call once at startup, after openWrite().
void start(Memory* m, int posOff, int velOff, int flagsOff,
           uint32_t canCollideBit);

// Stop the thread, zero the residual velocity and put the collision bits back.
void stop(Memory& m, const Offsets& o);

// Begin (or retarget) a run for `selfPrim`. `trackPrim` may be the target
// player's HRP primitive: its position is re-read during the run so a moving
// player is still caught. Pass 0 for a static waypoint.
//
// `instant` = Stunt Piggyback behaviour: the target point is written as-is on
// every pass (no interpolation). false = advance a virtual point at `speed`
// studs/s, still writing absolute positions (nice for a visible glide, same
// collision bypass).
//
// `ghostParts` are the character's other collidable primitives (head/limbs):
// clearing CanCollide on the root alone still lets the torso/limb collision
// fight the position write. Up to `kTpMaxGhostParts` extra primitives.
// Returns false when Primitive.Position is missing or the self position cannot
// be read.
const int kTpMaxGhostParts = 8;
bool request(Memory& m, const Offsets& o, uint64_t selfPrim,
             uint64_t trackPrim, const Vec3& dst, float speed,
             const uint64_t* ghostParts = nullptr, int ghostCount = 0,
             bool instant = true);

// Falling edge request from the UI (does not block).
void cancel();

// UI/diag.
bool active();
float writesPerSec();

// 1 = arrived (read-back confirmed), -1 = timeout/cancelled,
// -2 = position write rejected (engine kept the old position), 0 = nothing.
int takeResult();

} // namespace Tp
