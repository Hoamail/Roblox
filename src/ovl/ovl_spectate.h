#pragma once
// Spectate (Teleport tab): make the game's own camera follow another Humanoid.
//
// Mechanism: `Camera.CameraSubject` is the Instance the default camera module
// looks at every RenderStep. Writing the target's Humanoid pointer into that
// field is the same thing an executor does with
// `workspace.CurrentCamera.CameraSubject = other.Humanoid`, so the camera
// orbits/zooms with the normal mouse controls while looking at that character.
// The writer keeps the field pinned (a game that re-assigns CameraSubject every
// frame would otherwise snap the view back) and restores the local Humanoid on
// stop.
#include "sdk.h"

namespace Spectate {

// Spawn the writer thread. Call once at startup, after openWrite().
void start(Memory* m, int subjectOff);

// Stop the writer and write the local Humanoid back into CameraSubject.
void stop(Memory& m);

// Called once per overlay frame. `on` holds `subject` (a Humanoid instance) in
// `camAddr + CameraSubject`; `selfSubject` is restored on the falling edge.
// Safe to call every frame; a no-op when `on` is false and nothing was held.
void publish(uint64_t camAddr, uint64_t subject, uint64_t selfSubject, bool on);

// UI/diag.
bool active();
uint64_t subject();

} // namespace Spectate
