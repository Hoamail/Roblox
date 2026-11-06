# HANDOFF — velext aim smoothing model (Phase A)

- Date: 2026-10-18
- Binary: `ext_overlay.exe` · Source: `src/overlay.cpp`
- Backup: `backup/overlay_DOT2_AIMSMOOTH_20261018.{cpp,exe,cfg}`
- Full self-test output: `aim_work/aimtest_output.txt` (504 lines, real run)
- Reference: `velext_aim_findings.md` (spec) — this file is the implementation handoff.

Scope: replace the smoothing model of BOTH actuation paths with the velext-proven
time-constant filter. No InputObject injection. No in-game test in this phase.

## What changed

1. New cfg key `aim.smoothing` (float, default **5.00**, clamped 1..20). One knob.
2. `aim.smooth` / `aim.tsmooth` / `aim.memsmooth` are still parsed but ignored by the
   execution path; one log line is printed when any of them is present:
   `[aimcfg] legacy smoothing key ignored (aim.smooth/aim.tsmooth/aim.memsmooth); using aim.smoothing=5.00`
   (verified in a real run against the old cfg; live cfg has been cleaned since).
3. **Mode 1 (memory writer)**: stateless nlerp on the look direction, re-read fresh
   from the game at every actuation; at most **one actuation per published snapshot**
   (per main-loop iteration) + 4 ms hard gate (≤250 Hz). The old 500 Hz slerp
   re-apply path is only reachable via `aim.pid=1` (legacy).
4. **Mode 0 (SendInput)**: velext mouse model — same alpha, 1.5 px deadzone,
   alpha ≤ 0.002 skip, per-step clamp `clamp(14000*alpha, 12, 400)`, camera
   back-pressure (move > 0.02° or 60 ms). `aim.pid=1` keeps the legacy PID/AB filter.
5. **Menu (Combat tab)**: float slider 1..20 "Smoothing" + live readout of tau and
   alpha@60 Hz; persisted as `aim.smoothing=<%.2f>`.
6. **`-aimtest`**: pure-math self-test (no game, no Roblox, no singleton mutex),
   4 sections, exit code 0.

## 1. Final formula (exact, as implemented)

```
ts    = max(1.0, aim.smoothing)          // <= 1.0 -> snap
alpha = 1.0                              if ts <= 1.0
      = clamp(1 - expf(-dt/tau), 0, 1)   otherwise, tau = ts * 0.008 s
dt    = (now - lastActuate) seconds, clamped [0, 0.1]
        first-ever actuation: dt = 0.004 s (velext fallback)
gate  = actuation skipped unless now - lastAct >= 4 ms (stamp consumed on pass)
filter (mode 1) dir = normalize( look + (targetDir - look) * alpha )    // nlerp, not slerp
        look = -(m[2],m[5],m[8]) of the freshly re-read 36-byte camera rotation
        write skipped when dot(look, targetDir) > 0.99996 (~0.5124 deg)
```

Notes:
- Stateless: current rotation is re-read every actuation; no carried filtered
  orientation, no accumulated error (self-correcting, no drift).
- Mode 1 consumes ≤ 1 snapshot publish per actuation ⇒ actuation rate =
  main-loop publish rate, capped at 250 Hz by the 4 ms gate.
- The deadzone check happens after the stamp is consumed (mirrors velext
  stamp-then-deadzone order).
- Mode 0 additionally: `distPx <= 1.5` skip; `alpha <= 0.002` skip; per-step
  count clamp; back-pressure hold (only stamps in-flight when a step was sent).

## 2. Alpha table — real `-aimtest` output (dt = 1/60 s)

```
smoothing=  0.05 ts= 1.00 tau=   --    alpha=1.000000 (SNAP)
smoothing=  0.20 ts= 1.00 tau=   --    alpha=1.000000 (SNAP)
smoothing=  0.50 ts= 1.00 tau=   --    alpha=1.000000 (SNAP)
smoothing=  1.00 ts= 1.00 tau=   --    alpha=1.000000 (SNAP)
smoothing=  2.00 ts= 2.00 tau= 16.0ms alpha=0.647134
smoothing=  5.00 ts= 5.00 tau= 40.0ms alpha=0.340759
smoothing= 20.00 ts=20.00 tau=160.0ms alpha=0.098925
smoothing=5.00 alpha: 60Hz=0.3408 144Hz=0.1594 240Hz=0.0989 (same tau=40ms)
```

Vs the reference table (smoothing 0.05/0.2/0.5/1.0 → snap; 2.0 → 0.648;
5.0 → 0.341; 20.0 → 0.099):
- snap rows, 5.0 and 20.0 match at display precision;
- 2.0 is **0.647134 vs 0.648** (Δ 0.0009). Cause: the reference value used
  dt = 16.7 ms; the exact 1/60 s dt gives 0.6471. No model difference.

## 3. Convergence / gate / deadzone — real output

Convergence from 30°, smoothing 5 (tau = 40 ms), 0.5 s:

```
60 Hz  summary: writes=11 deadzone=19 gate=0   | residual(dz on)=0.4255 deg (first deadzone @200 ms) | residual(dz off)=0.00016 deg
144 Hz summary: writes=24 deadzone=48 gate=0   | residual(dz on)=0.5096 deg (first deadzone @174 ms) | residual(dz off)=0.00012 deg
240 Hz summary: writes=40 deadzone=80 gate=0   | residual(dz on)=0.4763 deg (first deadzone @171 ms) | residual(dz off)=0.00011 deg
500 Hz summary: writes=41 deadzone=84 gate=125 | residual(dz on)=0.5071 deg (first deadzone @166 ms) | residual(dz off)=0.00011 deg
theory (dz off): 30.00 * exp(-0.5/0.04) = 0.00011 deg
spread across 60/144/240/500 Hz (dz off): max-min = 0.00004 deg
```

- FPS invariance proven: pure-filter residual 0.00011–0.00016° vs theory 0.00011°
  (spread 4e-5° — float/interpolation noise).
- With the deadzone the filter parks within 0.43–0.51° at every rate (boundary
  `acos(0.99996) = 0.5124°`), i.e. "sai số còn lại xấp xỉ nhau".
- 500 Hz row: 125 GATE blocks over 250 ticks ⇒ the gate caps actuation at 250 Hz.

Trace sample (60 Hz / 500 Hz):

```
t=  16.7ms dt= 4.00ms ts=5.00 tau=40.0ms alpha=0.09516 err 30.0000 -> 27.2407 deg  WRITE
t=  33.3ms dt=16.67ms ts=5.00 tau=40.0ms alpha=0.34076 err 27.2407 -> 18.0331 deg  WRITE
...
t= 466.7ms dt=16.67ms ts=5.00 tau=40.0ms alpha=0.34076 err  0.4255 ->  0.4255 deg  DEADZONE

t=   2.0ms dt= 4.00ms ts=5.00 tau=40.0ms alpha=0.09516 err 30.0000 -> 27.2407 deg  WRITE
t=   4.0ms dt=  --   ts=5.00 tau=40.0ms alpha=  --    err 27.2407 -> 27.2407 deg  GATE(4ms)
t=   6.0ms dt= 4.00ms ts=5.00 tau=40.0ms alpha=0.09516 err 27.2407 -> 24.7200 deg  WRITE
```

Real-clock gate test and deadzone predicate:

```
attempt1 -> PASS (dt fallback=0.0040 s, alpha=0.0952)
attempt2 +0.004 ms -> BLOCKED (needs >= 4.000 ms)
attempt3 +7.524 ms -> PASS (dt=0.0075 s, alpha=0.1715)

err=0.50 deg dot=0.999962 -> NO WRITE (deadzone)
err=0.60 deg dot=0.999945 -> WRITE
boundary = acos(0.99996) = 0.5124 deg
```

## 4. Constants

[P] = value proven from velext disasm (address in `velext_v1.exe`).
[I] = inferred; semantics/tuning may change after Phase B measurement.

| Constant | Value | Tag | Address / note |
|---|---|---|---|
| tau per smoothing unit | 0.008 s | [P] | 0x1402B7FBC |
| dt fallback (no prior stamp) | 0.004 s | [P] | 0x1402B7FB8 |
| dt clamp hi | 0.1 s | [P] | 0x1402B7FF0 |
| tau epsilon (=> alpha 1) | 1e-4 | [P] | 0x1402B7FA4 |
| min actuation interval | 4 ms (`0x3D0900` ns) | [P] | gate + stamp `0x140334408` |
| snap threshold | smoothing <= 1.0 | [P] | 0x1402B80FC |
| rotation deadzone | dot > 0.99996 | [P] | 0x1402B80F8 (~0.5124 deg) |
| mouse deadzone | 1.5 px | [P] | 0x1402B8144 |
| mouse alpha epsilon | 0.002 | [P] | 0x1402B7FAC |
| per-step clamp lo / hi | 12 / 400 counts | [P] | 0x1402B831C / 0x1402B8470 |
| degenerate mix length | 1e-3 | [P] | 0x1402B7FA8 (fallback to target dir) |
| per-step cap base | 14000 | [I] | velext float 0x1402B84B0; our px->counts mapping is inferred |
| back-pressure move | 0.02 deg | [I] | velext 0x1402B7FCC; "camera must move" semantics inferred |
| back-pressure timeout | 60 ms | [I] | velext timer `0x140334418`; semantics inferred |

## 5. Remaining warts

- Mode 0 px→count mapping is the linear small-angle form `step = px * alpha * outGain`
  (`outGain = aim.sens/aim.gamesens`, clamp [0.1,4]); velext uses
  `atan(px/focal)` + per-axis game coefficients. Feel-equivalent for typical
  errors (< ~15°), exact math deferred to Phase B.
- Mode 1 keeps `aim.pid=1` reachable (legacy slerp + `aim.memsmooth`), and mode 0
  `aim.pid=1` keeps the old PID/AB filter — both intentionally untouched for A/B.
- Leftover unused legacy state in `overlay.cpp`: `smAim/smPrim/smInit/smSX/smSY`
  (declared, never read) — pre-existing, harmless.
- Mode-0 dt stamp can be stale after long idle below the outer gates
  (`aim.deadzone`, `aim.interval`); the first re-engagement step then uses
  `dt=0.1` clamped → alpha ≈ 0.92 at smoothing 5. Velext has no equivalent
  idle state to compare (its stamp also only advances while ticks pass the gate).
- Self-test noise: 60 Hz residual 0.00016° vs 0.00011° at 144/240 (4e-5°).
- Not yet verified in game by design; all numbers are math-sim + real-clock
  gate/deadzone checks.

## 6. DIFF WITH VELEXT (still standing)

1. **Injection mechanism**: we use `SendInput` (mode 0) or a 36-byte rotation
   write (mode 1). Velext's Mouse method injects a 20-byte record into the game
   input vector (`obj+0x20/0x28/0x30`, flag `0x0101` at `obj+0x48`). Not
   implemented by constraint — the rollout difference is the input-stack path,
   not the smoothing math.
2. **Counts mapping** (mode 0): linearized, see wart #1. Needs Roblox RVAs
   `+0x8D857D8 / +0x8D857E0 / +0x8D85820` and `MouseService+0x21C` to be exact.
   Phase B (§14.5): the RVAs are confirmed to track the Roblox sensitivity slider;
   `+0x21C` stayed 1.0 (constant scale) — the exact mapping can now be built from
   the measured floats.
3. **Stamp consumption**: ours is only consumed when the outer gates pass
   (`aim.deadzone`, `aim.interval`, actionable target); velext stamps on every
   tick that clears the 4 ms gate. Observable only in edge cases (long idle).
4. **Back-pressure read**: ours re-reads the camera rotation each frame when the
   aim is active; velext uses its own cached camera state. Equivalent modulo
   read timing; ours silently proceeds if the read fails.
5. **Prediction / sticky**: our lead (`aim.predict/aim.pscale`) and sticky
   (`aim.sticky/sticktime`) remain overlay-side extras; velext's
   `prediction/predictScale/sticky` formulas were not part of this phase.
6. **Silent method**: not implemented (we have no publish-only plan channel).
7. **Alpha epsilon**: implemented only in the mode-0 path — mirrors velext,
   whose camera path has no 0.002 check.

## 7. How to use / A/B / revert

- Goal state: change ONE parameter — `aim.smoothing` in `overlay.cfg` (or the
  Combat-tab slider). 1 → snap/fastest, 5 → velext default feel (tau 40 ms),
  20 → very slow (tau 160 ms).
- Full legacy behaviour: `aim.pid=1` (mode 0: PID/AB; mode 1: slerp + `aim.memsmooth`).
- Revert binary: `backup/overlay_DOT2_AIMSMOOTH_20261018.*`; prior milestone:
  `backup/overlay_DOT1_8TAB_20261018.*`.
- Re-run self-test: `ext_overlay.exe -aimtest` (no game needed, exits immediately).

## 8. Status of Phase B

- **DONE (2026-10-18/19)** — live, read-only measurements. Full evidence, trace files and
  corrections: `velext_aim_findings.md` **§14 (Phase B — live evidence)**.
- Answers to the open questions:
  (a) The `A/B/C` floats (`base+0x8D857D8 / +0x8D857DC / +0x8D857E0 / +0x8D857E4 / +0x8D85820`)
      track the Roblox *Mouse Sensitivity* slider exactly (1 → 2.8 → 3.4 → 4 → 0.36 in
      `traceD1_sens.csv`); `MouseService+0x21C` stayed constant **1.0** → it is a scale, not the
      live sensitivity. The linearized px→count mapping (wart #1) is still not ported.
  (b) **No.** The aim tick is a fixed ~15.5 ms (64.5 Hz) cadence: with `ovl.renderUncap=1`
      (overlay ~140–150 fps, game ~130 fps) the tick stayed 15.50 ms (`traceD2_ovluncap.csv`).
      The "limiter deadline derives from `ovl.renderFps`" assumption is disproven as the tick
      source (findings §14.5).
  (c) **Not UI-diffed** (D3 not performed) — live values at the block offsets matched the ini in
      every run (`+0xC8`=0, `+0xA4`=88, `+0xA8`=1), single-point consistency only → still [I].
  (d) **Done** at smoothing 1.0 / 1.919355 / 4.983871 / 20.0: MATCH within ~2 % at 1.92 & 4.98,
      5–28 % at 20 (moving target), clean snap at 1.0 (`traceC1/C2/C3`, findings §14.3).
- New in-game facts that affect the port:
  * Mouse mode requires **mouse-lock** (first-person / shift-lock); third-person free-look =
    pressing X does nothing (operator-verified). The overlay's SendInput branch has the same
    constraint; the camera branch does not.
  * The camera write is rotation-only — 0.0 mm position drift during B8 transients.
  * The 4 ms gate never binds at ~64.5 Hz (min observed dt 13.5 ms).
  * Flag byte detail: live reads show `+0x48` toggling 0/1 with the constant `0x0101` at
    `+0x49…+0x4A` (findings §14.4) — do not hard-code the static `+0x48` layout.

## 9. Status of Phase C (2026-10-19)

- **DONE (offline build, no game run)** — real velext Mouse-method mapping ported into
  mode 0 (`aim.pid=0`), mouse-lock guard added, self-test extended. `velext_aim_findings.md`
  was not touched. Backup: `backup\overlay_DOT3_20261019.{cpp,exe,cfg}`.

### What changed (`src\overlay.cpp` only)

1. **Mode 0 mapping (wart #1 closed).** Per inject step the mouse branch now runs the real
   `fn_1400d9cc0` chain (`velextMouseCounts`, shared with the self-test):
   ```
   cosp = max(0.15, sqrt(max(0, 1-p*p)))         ; p = cre[5] (pitch component)
   cx = atan(eX/focal)*RAD2DEG/cosp/(0.5*out1)*alpha
   cy = atan(eY/focal)*RAD2DEG/(0.385*out2)*alpha
   if (alpha < 1): lim = clamp(14000*dt, 12, 400); clamp cx/cy to +-lim
   ```
   - `eX/eY` = target screen pos − viewport centre (same ref as the live B-traces).
   - `focal = (vh*0.5)/tan(fov*0.5*DEG2RAD)`, FOV from `cam+o.cameraFOV` (sane bound
     10..150 °, `tanHalf > 1e-6` guard), `vh` = current viewport height.
   - Sens chain faithful to `fn_14010d580`: A=`base+0x8D857D8`, B=`+0x8D857E0`,
     C=`+0x8D85820` (all must be finite and `1e-3 ≤ |v| ≤ 100`); `out1=|f|*A.x`,
     `out2=A.y*f` must land in `[1e-4, 1000]`; `f` = `MouseService+0x21C` is **soft**
     (substitutes 1.0 on failure, exactly like velext).
   - **Correction to §5/prompt:** the per-step cap base is **dt**, not alpha. Register
     trace of the clamp site (`xmm8`, 0x1400DACF9 / 0x1400DA9DB) ties it to the same
     value that feeds `exp(-dt/tau)` at 0x1400DA901 — findings §5 itself hedged
     "xdt or xalpha". Applied only when `alpha < 1` (0x1400DACF3), as documented.
   - Fallback (sens/FOV/camera-read unusable): legacy `aim.sens/aim.gamesens` linear gain
     + its old alpha-based cap, one rate-limited log `aim-map-fallback`. Legacy keys and
     the `aim.pid=1` path untouched.
2. **Mouse-lock guard (§3, new).** Two nets, only for the SendInput branch:
   - *Primary:* `GetCursorInfo()` — cursor visible in the foreground game → skip inject
     (`aim-cursor-hold` log). Relock resets the stale counter.
   - *Secondary:* back-pressure releases with **no camera movement** (>0.02°) count up;
     N=6 in a row (`kVelextBpStaleMax`) ⇒ injects held (`aim-stale-hold`), with a single
     probe inject every 750 ms (`kVelextBpProbeNs`); a probe that moves the camera
     clears the hold (`aim-lock-regained`).
3. **MouseService resolve**: DataModel child walk by name (velsample pattern), cached,
   retry ≤ 1/s, reset when aim turns off. Log `aim-mouse-svc`.
4. **Self-test**: section `[5]` — 7 hand-computed mapping cases (incl. prompt example
   α=0.32, cosp floor, lim floor/ceil, α=1 no-clamp) + input-bound validation. `-aimtest`
   still exits 0.
5. **Prediction (§2)**: overlay has **no ping source** → kept the current `snapPos` clamp
   behaviour and per-frame dt fallback, as the prompt allows; not faked.

### Build & verify (offline)

```
> build_fast.bat
build overlay exit: 0
> .\ext_overlay.exe -aimtest    # exit 0, full log: aim_work\aimtest_output.txt
[5] Phase C mapping: angle -> counts (velext 0x1400DAB52)
  eX=      100 eY=      100 focal= 1000 p= 0.00 out=1.00,1.00 a=1.00 dt=0.0167 -> (  11.4212,  14.8327) want (  11.4212,  14.8327) PASS
  eX=      100 eY=      100 focal= 1000 p= 0.50 out=1.00,1.00 a=1.00 dt=0.0167 -> (  13.1881,  14.8327) want (  13.1880,  14.8327) PASS
  eX=      100 eY=      100 focal= 1000 p= 0.00 out=1.00,1.00 a=0.32 dt=0.0167 -> (   3.6548,   4.7465) want (   3.6548,   4.7465) PASS
  eX=      100 eY=      100 focal= 1000 p= 0.99 out=1.00,1.00 a=0.50 dt=0.0167 -> (  38.0706,   7.4164) want (  38.0706,   7.4164) PASS
  eX=      200 eY=      200 focal= 1000 p= 0.00 out=1.00,1.00 a=0.90 dt=0.0001 -> (  12.0000,  12.0000) want (  12.0000,  12.0000) PASS
  eX=  1000000 eY= -1000000 focal= 1000 p= 0.00 out=0.36,0.36 a=0.90 dt=1.0000 -> ( 400.0000,-400.0000) want ( 400.0000,-400.0000) PASS
  eX=  1000000 eY= -1000000 focal= 1000 p= 0.00 out=0.36,0.36 a=1.00 dt=1.0000 -> ( 499.6817,-648.9372) want ( 499.6817,-648.9373) PASS
  sens input bounds [1e-3,100]: edges accepted=1, rejected (0, lo/10, hi+1, NaN, inf)=1
  [5] all mapping cases PASS
```

### Remaining [I] / fallback points (mode 0)

- Sens/FOV/camera-read failure → legacy linear gain (rate-limited log) — by design.
- FOV sanity filter 10..150 ° is ours; velext trusts its own read.
- `f` soft-default 1.0 matches velext, but a *stale* `MouseService` pointer still yields
  the default 1.0 silently (only A/B/C failure logs).
- Cap base correction (dt) is register-evidence-based; no live A/B measured yet.
- Cursor-hidden heuristic may misfire if another app hides the cursor; the stale-release
  net covers it.
- Prediction still `snapPos` clamp (no ping source — documented).
- Phase B item (c) UI-diff of `+0xC8/+0xA4/+0xA8` still open (unchanged).

## Disclosure

During verification, one 1-second run `ext_overlay.exe -t 1` occurred while
`RobloxPlayerBeta.exe` was already open (user session). Aim was disabled
(`aim.enabled=0`), no memory writes were performed, and `velext_v1.exe` was not
running. The `-aimtest` runs and the final rebuild touched neither process.
