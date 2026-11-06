#pragma once
// Teleport (Teleport tab): one-click, no-spam travel to a player/waypoint.
//
// Why not a single Primitive.Position write: the engine re-derives
// Primitive.Position from its physics assembly every step, so a raw position
// write is reverted in 10-50 ms (measured in this project; see
// FINDINGS_overlay_internals.md §4). Clicking repeatedly occasionally catches
// the replication window, which is the jerk-then-snap-back the old button
// produced. AssemblyLinearVelocity, held at high rate, is the lever the solver
// actually respects (same mechanism as Fly), so this module turns a click into
// a guided dash and stops when it reaches the target.
#include "sdk.h"

namespace Tp {

// Spawn the writer thread. Call once at startup, after openWrite().
void start(Memory* m, int posOff, int velOff, int flagsOff,
           uint32_t canCollideBit);

// Stop the thread, drop the leftover velocity and put the collision bits back.
void stop(Memory& m, const Offsets& o);

// Begin (or retarget) a dash for `selfPrim`. `trackPrim` may be the target
// player's HRP primitive: its position is re-read during the dash so a moving
// player is still caught. Pass 0 for a static waypoint.
//
// `ghostParts` are the character's other collidable primitives (head/limbs):
// clearing CanCollide on the root alone still lets the torso/limb collision
// stop the assembly at a wall. Up to `kTpMaxGhostParts` extra primitives.
// Returns false when the offsets needed for a dash are missing.
const int kTpMaxGhostParts = 8;
bool request(Memory& m, const Offsets& o, uint64_t selfPrim,
             uint64_t trackPrim, const Vec3& dst, float speed,
             const uint64_t* ghostParts = nullptr, int ghostCount = 0);

// Falling edge request from the UI (does not block).
void cancel();

// UI/diag.
bool active();
float writesPerSec();

// 1 = arrived, -1 = timeout/cancelled, 0 = nothing. Clears the slot.
int takeResult();

} // namespace Tp
