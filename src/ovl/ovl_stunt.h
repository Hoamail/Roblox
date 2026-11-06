#pragma once
// Stunt tab (index 8): visible choreography on the root assembly.
//
// Only the root replicates (position + rotation + velocity of the assembly);
// Motor6D.Transform is render-only, so every preset here moves/xorients the
// whole character. See HANDOFF_visible_stunts.md §1 for the channel table.
//
// Two control modes, per preset:
//   dynamic   - write AssemblyLinearVelocity / AssemblyAngularVelocity at a
//               high rate (Sleep(0)); the solver integrates. Same mechanism
//               fly uses, which is why it is the cheap/default path.
//   kinematic - write Primitive.Rotation (9 float) and/or Position directly.
//               Exact, but needs a high write rate as well (position is
//               reverted in 10-50 ms). Selected at runtime with stunt.mode=1
//               so the two can be compared in game (E1/E2).
//
// PlatformStand=1 is the switch that stops the humanoid from standing itself
// back up; without it a rotated pose is snapped upright on the next step.
#include "sdk.h"

struct StuntParams {
    int   preset    = 0;     // index into the preset table (see Stunt::presetName)
    float timeScale = 1.0f;  // 0.25 .. 2.0 (stunt.speed)
    float amp       = 1.0f;  // 0.1 .. 3.0  (stunt.amp)
    int   ragdoll   = 0;     // 1 = force PlatformStand for every preset
    int   loop      = 1;
    int   hover     = 0;     // 1 = hold altitude (velocity.y pinned to 0)
    int   mode      = 0;     // 0 = dynamic, 1 = kinematic (stunt.mode)
    uint64_t targetPrim = 0; // target HRP primitive (piggy/orbit)
};

namespace Stunt {

// Spawn the writer thread (parks on Sleep(10) while idle). Safe to call once
// after openWrite(); publish() will spawn it lazily if this was skipped.
void start(Memory* m, const Offsets* o);

// Called once per overlay frame. `selfPrim` = own HRP primitive, `selfHum` =
// own Humanoid instance, `camAddr` = Camera instance (forward/right basis).
// on == false is the falling edge: restore seed pose + flags once.
void publish(Memory& m, const Offsets& o, uint64_t selfPrim, uint64_t selfHum,
             uint64_t camAddr, const StuntParams& p, bool on);

// Emergency/unload: restore pose, zero velocity, hand the humanoid flags back,
// then join the thread. Used by panic and by the exit path.
void stop(Memory& m, const Offsets& o);

bool  active();          // 1 while the writer is holding a pose
float writesPerSec();
int   stateId();         // 0 idle, 1 running, 2 restoring (UI status only)
int   presetRuntime();   // sub-preset currently executing (combos), else preset
const char* presetName(int i);
int   presetCount();

} // namespace Stunt

// Offline self-test (-stunttest): orthonormality of eulerToBasis, easing
// boundaries, every preset sampled for NaN, kinematic integration and the
// restore plan. No game, no offsets needed. 0 = PASS.
int runStuntSelfTest();
