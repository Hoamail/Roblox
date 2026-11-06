# velext_v1.exe — AIMBOT internals (smoothing / actuation / sensitivity) — RE findings

Target: `C:\Users\adminn\ext_project\velext_v1.exe` (VELOCITY, realvelocity.xyz)
Method: static RE of the on-disk image (`velext_dump_text/data/rdata.bin` = runtime dumps, base `0x7FF6C78F0000`, module VA base `0x140000000`), read-only. No velext process was launched for these findings. **Phase B (§14, 2026-10-18/19) extends this with live, read-only measurements** taken while velext *and* Roblox were running (no writes to either process).

Evidence tags used below: **[P]** = proven (exact instruction/address), **[I]** = inferred (consistent evidence, not 1:1 provable), **[?]** = unknown.

---

## 0. TL;DR

* Aim actuation has exactly three methods, selected by `aim.method` (0=Mouse, 1=Camera, 2=Silent):
  * **Mouse (0)** — writes a synthetic mouse record (20 bytes: `{x, y, dx, dy, 0,0,0}`) into a `std::vector`-like queue inside the game process (`MouseService`-ish object) + one 2-byte flag; **it does NOT use SendInput / SetCursorPos / mouse_event** [P].
  * **Camera (1)** — reads the Camera CFrame (48 B) and writes back **only the 36-byte rotation** (position untouched) [P].
  * **Silent (2)** — writes nothing to the game; only publishes the aim plan to its own globals (`0x1403343B0/3C0/3D0`) [P].
* The smoothing core is **not** an angle lerp and **not** a fixed "smoothing factor": it is a **time-constant exponential filter on the aim *direction vector*, using `dt` in seconds**:

  ```
  ts    = max(1.0f, aim.smoothing)              // 0x14032D460
  snap  = (ts == 1.0f)                          // i.e. smoothing <= 1.0  -> alpha = 1.0
  tau   = ts * 0.008f                           // 8 ms per unit of smoothing
  alpha = 1.0f - expf(-dt / tau)                // dt in s, clamped to [0, 0.1]
  alpha = clamp(alpha, 0, 1)
  ```
  with `dt = (now_ns - last_apply_ns) / 1e9`, `last_apply_ns` at `0x140334408`, `now_ns` = `FUN_140059570()` (QPC→ns), fallback `dt = 0.004 s` when there is no previous timestamp.
  **Consequence: changing render FPS does not change the aim feel** (the filter is time-based, not tick-based). The single knob that reproduces vede's feel is `smoothing` (τ = 8 ms · smoothing, snap at ≤ 1).
* Hard gates around it: **4 ms minimum between applications (≤250 Hz)**, mouse path: **screen deadzone 1.5 px**, **`alpha ≤ 0.002` → no move**, **per-step clamp `clamp(14000·alpha, 12, 400)` counts/pixels**, camera path deadzone **`dot > 0.99996` (±0.51°) → no write** [P].
* Mouse method is additionally **back-pressured**: after an injection, the next injection is skipped until the camera actually moved by **> 0.02°** (or **60 ms** elapsed) — this is what makes it look "1 step per camera update" instead of spamming [P].
* Angle→mouse-count conversion (Mouse method) is derived from the game's own coefficients: `counts = angleDeg / (0.5·out1)` (X, plus `1/max(0.15, cos-pitch)` correction) and `angleDeg / (0.385·out2)` (Y), where `out1/out2` come from a per-instance float (`MouseService+0x21C`) and two float pairs inside the Roblox image (`base+0x8D857D8`, `base+0x8D857E0`) [P for the code chain, **[I]** for the semantic label of those fields].
* Config: ship defaults are read from `%LOCALAPPDATA%\velext\profiles.ini`. Default `aim.smoothing=5` ⇒ **τ = 40 ms** (63 % in 40 ms, 95 % in 120 ms).
* **Phase B (live, read-only — §14):** the formula / τ = 8 ms·smoothing and the 36-byte camera write are now **verified in-game** (smoothing 1.0 / 1.92 / 4.98 / 20, ≈2 % agreement), and the "tick = overlay frame rate" guess is **disproven**: the tick is a fixed ~15.5 ms (64.5 Hz) cadence, unchanged by `ovl.renderFps` / `ovl.renderUncap` and by the game's own fps.

---

## 1. Function / address table (all [P] unless noted)

| VA | size | role |
|---|---|---|
| `0x1400D9CC0` | 4538 B | **aim tick** (target pick → aim point → smoothing → method dispatch → plan publish) |
| `0x1400D96B0` | — | Camera method actuator: read CFrame, nlerp dir, write 36 B rotation (only caller = aim tick @ `0x1400DA81C`) |
| `0x1400D9160` | — | lookAt/rotation-matrix builder used by `0x1400D96B0` |
| `0x1400D9450` | — | CFrame/rotation validator used by `0x1400D96B0` |
| `0x1400D9C00` | — | FOV radius in px: `(h/2)·tan(min(89, fov/2)·deg2rad)/tan(camFov/2·deg2rad)` |
| `0x1400DB130` | — | hitbox/part lookup (12 part indices, parent fallback chain, limb extension) |
| `0x1400D9B40` | — | hold-key test (aim key pressed?) |
| `0x1400E6040` | — | life-state lookup (FNV-1a hash map) for "Hide dead" |
| `0x140059570` | — | clock: ns timestamp (QPC, `_Query_perf_counter/frequency`) |
| `0x1400DAF00` | 462 B | **aim worker thread job** (waits for state, copies viewport snapshot, calls tick) |
| `0x1400DB0D0` | — | `_beginthreadex` trampoline: `proc(param->f15(param->p8, param->p0))` |
| `0x14009A2A0` | 160 B | lock-free cell acquire/take (`__std_atomic_wait_direct`, `MSGWAIT`-style) — the aim thread blocks here |
| `0x14010D9C0` | 1521 B | **mouse injector**: pushes 20-byte record into game-side vector + writes 2-byte flag |
| `0x14010D250` | — | resolves the injector target object (cached at `0x140334888`) |
| `0x14010D4D0` | — | mode probe (cursor mode vs delta mode) |
| `0x14010D580` | 1082 B | **sensitivity reader**: reads `|f|·A.x`, `A.y·f` (+validates B, C), 2 float outputs |
| `0x1400FC490` | 1178 B | generic "find child instance by name" (used for `MouseService`) |
| `0x140096140` | 13135 B | main/overlay loop: creates worker threads, publishes viewport snapshot, frame limiter |
| `0x140093510` | — | precision sleep until absolute ns deadline (`SetWaitableTimer`+`WaitForSingleObject`) |
| `0x14012FDC0` | — | settings apply (`profiles.ini` → globals) |
| `0x140134730` | — | single value parser (called from `0x14013AD52`) |
| `0x14013A650` | — | `profiles.ini` loader |
| `0x1401453A0` | — | AIMBOT tab UI (widget ↔ global ↔ label/tooltip) |

Syscalls used by actuation: `NtReadVirtualMemory` id `0x3F` @ `0x140332A50`, `NtWriteVirtualMemory` id `0x3A` @ `0x140332A20`, stubs `0x14022FD20` (load id) / `0x14022FD2A` (execute). Math helpers are ucrt imports: `_fdclass 0x14024DA4F`, `asinf 0x14024DA67`, `atan2f 0x14024DA79`, `atanf 0x14024DA7F`, `expf 0x14024DAA9`, `sqrtf 0x14024DB03`, `tanf 0x14024DB15` [P via import table `aim_work/imp.py`].

---

## 2. (A) Actuation path — all three methods

### A.1 Camera method (`aim.method = 1`) — `FUN_1400D96B0`
1. `NtReadVirtualMemory(handle, Camera + *(qword)0x14032D270, buf, 0x30, &n)` — `0x14032D270` holds **`0xC8`** (verified in the runtime `.data` dump) ⇒ reads the 48-byte CFrame at `Camera + 0xC8` [P].
2. `FUN_1400D9450` validates the matrix; look vector = **negated third column** `-(m[2], m[5], m[8])` (sign mask `0x1402B8B20`) [P].
3. `dir = normalize(aimPoint - camPos)`; `dot = clamp(dot(look, dir), -1.0f, 1.0f)` (`0x1402B8548` = −1, `0x1402B80FC` = +1) [P].
4. **Deadzone:** `if (dot > 0.99996f) return;` (`0x1402B80F8`) → ≤ 0.51° error does nothing [P].
5. `mix = look + (dir - look)·alpha` (per component), `mix = (len(mix) < 1e-3) ? dir : mix`, then `newDir = normalize(mix)`, `target = camPos + newDir` [P].
6. `FUN_1400D9160` builds the rotation matrix; **`NtWriteVirtualMemory(handle, Camera + 0xC8, rot, 0x24, &n)` — writes 36 bytes, position untouched** [P].
7. Called only from the aim tick ⇒ the tick is the sole writer of camera rotation [P].

### A.2 Mouse method (`aim.method = 0`) — `FUN_14010D9C0`
* **No SendInput / SetCursorPos / mouse_event anywhere in the binary** (import table contains none of them) [P].
* `FUN_14010D9C0(ctx, x, y, dx, dy)` performs, via `NtReadVirtualMemory`/`NtWriteVirtualMemory` on the game:
  1. Read `begin/end/cap` from the target object: globals `0x14032D6E8` = **`0x20`**, `0x14032D6E0` = **`0x28`**, `0x14032D6D0` = **`0x30`** (runtime `.data` dump) ⇒ `std::vector` at object `+0x20/+0x28/+0x30` [P].
  2. Validate `begin ≥ 0x10000`, `begin ≤ end ≤ cap`, `(end-begin) % 20 == 0`, `(cap-begin) % 20 == 0`, `(cap-begin) < 0x1400014` [P].
  3. **`NtWriteVirtualMemory(handle, end, &rec, 0x14, &n)`** — pushes one 20-byte record `{ float x, float y, float dx, float dy, uint16 0, uint8 0, uint8 0 }` [P].
  4. Write the new `end` (`end+0x14`) back to `object+0x28` (=`0x14032D6E0`) [P].
  5. Write **`uint16 0x0101`** to `object + 0x48` (`0x14032D6D8` = `0x48`) [P].
  6. Target object resolved by `FUN_14010D250` and cached in `0x140334888`; on any failure the cache is zeroed (`0x14010DF72`) [P].
* Sanity: `dx==dy==0` ⇒ return (no injection) [P].
* Sub-mode chosen by `FUN_14010D4D0(ctx)` (probe returns non-zero → delta mode):
  * **non-zero = delta/mouse-lock mode** (0x1400DAA27…): `x/y` fields = reference point, `dx/dy` = mouse counts (see §4).
  * **zero = cursor mode** (0x1400DA96B…0x1400DAA22): reads the real cursor (`GetCursorPos` @`0x140259690`, `ScreenToClient` @`0x140259650`), `x/y` fields = cursor position, `dx/dy` = pixels of movement toward the target [P].

### A.3 Silent method (`aim.method = 2`)
* `0x1400DADA4`: clears `0x1403343A0`, `0x140334390`, `0x140334408`, `0x140332C5A`, then jumps to plan publishing (`0x1400DADF8` → `0x1403343B0/3C0/3D0`). No game memory write [P].
* Plan readers: `FUN_1400DF3A0`, `FUN_1400DAE80`, `FUN_1400EE9E0`, `0x1401459EC` [P].

---

## 3. (B) Tick loop / thread / frequency

* The aim job is a `_beginthreadex` thread created inside `FUN_140096140` at `0x140097768` (param struct built at `0x14009772A`; proc = `FUN_1400DAF00`, trampoline = `FUN_1400DB0D0`). The thread is (re)armed only if `xchg byte 0x140332C5B` was previously 0 [P].
* `FUN_1400DAF00` loop (`0x1400DAF50…0x1400DB07B`), gated by byte `0x140332C5B`:
  1. `active = aim.enabled(0x1403354EF) && FUN_1400D9B40()` (hold key) [P]
  2. `world = FUN_14009A2A0(&cell)` — **blocks** on `__std_atomic_wait_direct` until the producer publishes new state [P]
  3. lock mutex `0x14032E0C0`, copy the 40-byte viewport/camera snapshot `0x1403343E0/0x1403343F0/0x140334400`, unlock [P]
  4. `FUN_1400D9CC0(ctx, world, &snapshot)` [P]
  5. `FUN_140099D10(active ? 0x140261F50 : 0x140261F60)` (publish state), release cell [P]
* Producer of the snapshot: `FUN_140096140` (only writers of `0x1403343E0/3F0/400`, at `0x140097F24` and `0x140098A96`; second copy to `0x140334570/580/590`), i.e. the overlay's main loop. It also owns the frame limiter (`0x14009871C`: config-change flag `0x1403330FB` + 0.75 s stall timeout `0x2CB41780` ns; wait via `FUN_140093510` at `0x14009876D`) [P].
* **Effective tick rate = min(main-loop publish rate, 250 Hz)** [P for the 250 Hz bound; **[I]** for "publish rate = overlay frame rate" — **Phase B (§14.5): measured FALSE; the live tick is a fixed 15.50 ms (64.5 Hz) cadence, independent of `ovl.renderFps`/`ovl.renderUncap` and of the game fps**].
* Per-tick gates inside the tick:
  * **4 ms gate**: `if (now_ns - [0x140334408] < 0x3D0900) → no actuation` (applies to Camera **and** Mouse paths) ⇒ ≤ 250 Hz [P].
  * `alpha ≤ 0.002` → abort (Mouse path) (`0x1402B7FAC`) [P].
  * Mouse path only: abort if `[world] == [0x140334390]` (same value as last mouse actuation) — per-frame dedup [P; semantics **[I]**].
* Timestamps/state written by the tick: `0x140334408` (last apply, both paths), `0x140334248` (last yaw, deg), `0x1403343AC` (last pitch, deg), `0x140334418` (last mouse apply, ns), `0x140334390` (dedup id), `0x140332C5A` (mouse step in flight), `0x1403343B0/3C0/3D0` (published plan) [P].

---

## 4. (C) The smoothing formula — exact form, slider mapping, dt, clamps

### C.1 Camera path — `0x1400DA6E…0x1400DA821` (identical logic mirrored in the Mouse path `0x1400DA8D2…0x1400DA925`)
```
; 0x1400DA71E  xmm7 = 1.0f                       (const 0x1402B80FC)
; 0x1400DA726  xmm6 = xmm7
; 0x1400DA729  maxss xmm6, [0x14032D460]         ; xmm6 = ts = max(1.0, aim.smoothing)
; method branch: 2 -> silent(plan only), 1 -> camera, 0 -> mouse
; 4 ms gate (0x1400DA73A): (now-[0x140334408]) >= 0x3D0900 ns else return
; 0x1400DA775  last = [0x140334408]; [0x140334408] = now = FUN_140059570()
; 0x1400DA78F  dt = (last == 0) ? 0.004f (0x1402B7FB8)
;              : (float)(now-last) / 1e9f (0x1402B84D0)
; 0x1400DA7AF  dt = max(0.0f, dt)                (xmm12 = 0.0)
; 0x1400DA7B7  dt = min(0.1f, dt)                (0x1402B7FF0)
; 0x1400DA7C3  if (1.0f >= ts) goto APPLY        ; alpha stays 1.0  <=> smoothing <= 1.0  -> SNAP
; 0x1400DA7C8  tau = ts * 0.008f                 (0x1402B7FBC)
; 0x1400DA7D8  if (1e-4f >= tau) goto APPLY      ; alpha stays 1.0
; 0x1400DA7DD  xmm1 = -dt; xmm1 /= tau
; 0x1400DA7EB  xmm0 = expf(xmm1)
; 0x1400DA7F3  alpha = 1.0f - xmm0
; 0x1400DA7F7  alpha = max(0.0f, alpha)
; 0x1400DA7FC  alpha = min(1.0f, alpha)
; APPLY 0x1400DA801: FUN_1400D96B0(ctx, [cam+0x10], &aimPoint(+[rbp-0x60]), alpha)
```
* **Which quantity is smoothed:** the smoothed state is not stored; each application *re-reads the camera rotation from the game* and mixes it toward the desired direction: `newDir = normalize( lerp(lookFromGame, dirToTarget, alpha) )` [P]. So the "time constant" behaviour holds even though the game itself may also move the camera.
* The Mouse path adds one extra guard before the filter is computed: `if (alpha <= 0.002f) → no move` (`0x1400DA92A…0x1400DA936`) [P].
* Slider: `smoothing` min `1.0`, span `19.0` (`0x1402B834C`) ⇒ UI range **1 … 20**; the config value is a float and is *not* clamped by the binary (`max(1.0, ·)` only) [P for min/span and for the missing clamp].

### C.2 Slider → coefficient table (τ = 8 ms × max(1, smoothing); alpha for dt = 16.7 ms / 4 ms)

| `aim.smoothing` | ts | tau (ms) | alpha @16.7 ms | alpha @4 ms | behaviour |
|---|---|---|---|---|---|
| 0.05 | 1.0 | — | 1.0 | 1.0 | **snap** (ts==1 ⇒ no filter) |
| 0.2 | 1.0 | — | 1.0 | 1.0 | **snap** |
| 0.5 | 1.0 | — | 1.0 | 1.0 | **snap** |
| 1.0 | 1.0 | — | 1.0 | 1.0 | **snap** (UI minimum) |
| 2.0 | 2.0 | 16 | 0.648 | 0.221 | very fast |
| 5.0 | 5.0 | 40 | 0.341 | 0.0952 | **ship default** (`profiles.ini`) |
| 20.0 | 20.0 | 160 | 0.0993 | 0.0247 | slowest (UI max) |

Values below 1.0 are physically unreachable through the UI but the formula still snaps for any value ≤ 1.0 (e.g. a hand-edited ini) [P].

### C.3 Sticky / residual storage (vel-ext's own `.data`)
| VA | type | meaning |
|---|---|---|
| `0x14032D460` | float | `aim.smoothing` (UI min 1.0 / span 19.0) |
| `0x14032D45C` | float | `aim.predictScale` (0…3, `0x1402B8224`=3.0) |
| `0x14032D2A0` | float | `aim.maxDistance` (step 2000, `0x1402B84A4`) |
| `0x14032D464` | float | `aim.fov` (cone half-angle for `useFov`) |
| `0x140334408` | u64 | last apply timestamp (ns) — shared by Camera & Mouse |
| `0x140334418` | u64 | last successful mouse injection (ns) |
| `0x140334248` | float | last applied yaw (deg, Mouse delta mode) |
| `0x1403343AC` | float | last applied pitch (deg, Mouse delta mode) |
| `0x140334390` | u64 | last mouse-actuation world/frame id (dedup) |
| `0x140332C5A` | u8 | "mouse step in flight" (back-pressure flag) |
| `0x140334888` | ptr | cached mouse-injector object (0 ⇒ re-resolve) |
| `0x1403343B0/3C0/3D0` | 40 B | published aim plan (silent/ESP readers) |
| `0x1403343E0/3F0/400` | 40 B | viewport/camera snapshot (produced by main loop) |
| `0x140332C5B` | u8 | "aim job thread armed" |

---

## 5. (D) Sensitivity / angle → mouse-count conversion (Mouse method only)

Camera and Silent methods **do not use sensitivity at all** (rotation is written directly / nothing is written) [P].

Delta mode (`FUN_14010D4D0() != 0`, `0x1400DAA27…0x1400DAD55`):
```
yaw     = atan2f(-cam.lookX, -cam.lookZ) / DEG2RAD          ; 0x1400DAA27..0x1400DAA51 (0x1402B7FC8)
pitch   = asinf(clamp(-cam.lookY, -1.0f, 1.0f)) / DEG2RAD   ; 0x1400DAA57..0x1400DAA7E (0x1402B8548)
focal   = (h * 0.5f) / tanf(cam.fovY * 0.5f * DEG2RAD)      ; 0x1400DAC85..0x1400DAC95 (0x1402B8074, guard 1e-6 @0x1402B7F94)
; sensitivity chain (FUN_14010D580):
;   A = *(float2*)(robloxBase + 0x8D857D8)   ; base @0x140334890, RVA @0x14032D738   (A.x used, A.y used)
;   B = *(float2*)(robloxBase + 0x8D857E0)   ; RVA @0x14032D740   (validated only)
;   C = *(float )(robloxBase + 0x8D85820)    ; RVA @0x14032D718   (validated only)
;   f = *(float )(MouseService + 0x21C)      ; RVA @0x14032D760 = 0x21C, default 1.0f
;   out1 = fabsf(f) * A.x ; out2 = A.y * f   ; both validated: 1e-4 <= |v| <= 1000 (0x1402B7FA4/0x1402B849C)
countsX = (atanf( (aimScreenX - refX) / focal ) / DEG2RAD)
          / max(0.15f, sqrtf(1 - p*p))                      ; 0x1402B8008 = 0.15, p = cam+0x2C
          / (0.5f   * out1) * alpha                        ; 0x1402B8074 = 0.5
countsY = (atanf( (aimScreenY - refY) / focal ) / DEG2RAD)
          / (0.385f * out2) * alpha                        ; 0x1402B8050 = 0.385
if (alpha < 1.0f):  lim = clamp(14000.0f * alpha, 12.0f, 400.0f)   ; 0x1402B84B0/0x1402B831C/0x1402B8470
                    countsX = clamp(countsX, -lim, lim); countsY = clamp(countsY, -lim, lim)
record = { refX, refY, countsX, countsY }; FUN_14010D9C0(...)      ; then back-pressure bookkeeping
```
Followed by the **back-pressure** block (`0x1400DAA84…0x1400DAB44`):
```
if (mouseStepInFlight /*0x140332C5A*/):
    dYaw   = wrap180(yaw   - lastYaw   /*0x140334248*/)   ; wrap with ±180/±360 consts 0x1402B8418/0x1402B85A4/0x1402B85A0/0x1402B8460
    dPitch = wrap180(pitch - lastPitch /*0x1403343AC*/)
    if (fabs(dYaw) > 0.02f /*0x1402B7FCC*/) goto reset
    if (fabs(dPitch) <= 0.02f && now - lastInject(0x140334418) < 0x3938700 ns) return   ; 60 ms
reset: mouseStepInFlight = 0
on success: lastYaw/lastPitch = yaw/pitch ; lastInject = now ; mouseStepInFlight = 1
```
Cursor mode (`FUN_14010D4D0() == 0`): pure screen-space pursuit — `dx = (aimScreenX - cursorX)·alpha`, `dy = (aimScreenY - cursorY)·alpha`, clamped to `clamp(14000·dt, 12, 400)` px, record `{cursorX, cursorY, dx, dy}` [P].

**[I]** semantic labels: `MouseService+0x21C` = per-instance mouse-sensitivity multiplier (default 1.0); `A = {A.x, A.y}` = per-build mouse→rotation coefficients stored in the Roblox image; the literals `0.5` / `0.385` = velext-side X/Y model constants. What is provable: the code chain, the addresses, the ranges, and that `B`, `C` are read+validated but unused in this path. **Phase B (§14.5, live diff while dragging the slider):** the floats at `base+0x8D857D8 / +0x8D857DC / +0x8D857E0 / +0x8D857E4 / +0x8D85820` all track the Roblox *Mouse Sensitivity* slider exactly (1 → 2.8 → 3.4 → 4 → 0.36), while `MouseService+0x21C` stayed **1.0** the whole time → `out1/out2` scale with the slider through `A`, and `+0x21C` is **not** the live sensitivity value (constant scale in this setup).

---

## 6. (E) Target selection & aim point

* Method = `aim.method` @ `0x140335674` (items `Mouse/Camera/Silent`; branch at `0x1400DA70F`/`0x1400DA731`) [P].
* Filters (all flags in `.data`, bound to the UI page `FUN_1401453A0`): `Enabled 0x1403354EF`, `Visible Only 0x140335670`, `Hide teammates 0x140335678` (gates `[e+0x2E1]/[e+0x2E2]`), `Hide dead 0x14033567A` (via `FUN_1400E6040`), `Use FOV cone 0x140335680`, `Sticky aim 0x140335679`, `Prediction 0x14033567B` [P for the mapping; Sticky aim's use-site = **[?]**].
* Range gate: `dist·0.28f > aim.maxDistance (0x14032D2A0)` ⇒ reject (`0x1402B8024` = 0.28) [P].
* Prediction (`0x14033567B`): `pred = ([cam+0x78]/1000.0f) * aim.predictScale (0x14032D45C)`; target part position `+0x28C/0x290/0x294` (Head) or root `+0x280/0x284/0x288`; velocity `+0x2C8/0x2CC/0x2D0` [P].
* Priority = `0x14033567C` (`Crosshair/Distance/Health`): metric = screen distance / 3-D distance `[rbp-0x78]` / (`[e+0x3FC]/[e+0x400]` if `[e+0x404]!=0` else `1e9f` @`0x1402B84D0`), lower wins, compare at `0x1400DA5B1` [P].
* Target part = 12 items (`Head` index 0 → `[e+0x28C..0x294]`; `Torso` 1 → `[e+0x280..0x288]`; `Nearest` 2 → 15-entry table `0x140261F80` = `{01,02,03,06,09,0C,0F,04,05,07,08,0A,0B,0D,0E}` via `FUN_1400DB130`; 3…11 via `0x140261F70`) [P]. The dropdown index global itself = **[?]** (not isolated).
* Screen centre = `(w·0.5, h·0.5)` (`0x1402B8074` = 0.5); viewport struct (tick arg 3) = `{ptr, +0x10, +0x14, +0x18 = w (int), +0x1C = h (int)}` [P].

---

## 7. (F) Constant table (float/other literals, verified with `cst.py` / dumps)

| VA | value | use |
|---|---|---|
| `0x1402B7FBC` | 0.008 | **tau per smoothing unit** |
| `0x1402B7FB8` | 0.004 | dt fallback (s) |
| `0x1402B7FF0` | 0.1 | dt max (s) |
| `0x1402B84D0` | 1e9 | ns→s |
| `0x1402B7FA4` | 1e-4 | dt/tau epsilon, sensitivity min |
| `0x1402B80FC` | 1.0 | 1, clamp hi, snap reference |
| `0x1402B8548` | −1.0 | clamp lo (dot/asin) |
| `0x1402B80F8` | 0.99996 | camera deadzone (cos) |
| `0x1402B7FCC` | 0.02 | mouse back-pressure angle (deg) |
| `0x3938700` | 60 000 000 ns | mouse back-pressure timeout (immediate) |
| `0x3D0900` | 4 000 000 ns | min apply interval (250 Hz) |
| `0x1402B7FAC` | 0.002 | alpha epsilon (mouse) |
| `0x1402B8144` | 1.5 | screen deadzone (px) |
| `0x1402B831C` | 12.0 | per-step clamp min |
| `0x1402B8470` | 400.0 | per-step clamp max |
| `0x1402B84B0` | 14000.0 | px/s (×dt or ×alpha) cap |
| `0x1402B8074` | 0.5 | half (`h/2`, `sens·0.5`) |
| `0x1402B8050` | 0.385 | Y sensitivity model constant |
| `0x1402B8008` | 0.15 | min cos-pitch correction |
| `0x1402B7F94` | 1e-6 | tan(fov/2) guard |
| `0x1402B7FC8` | 0.017453292 | DEG2RAD |
| `0x1402B8418/0x1402B85A4` | +180 / −360 | yaw wrap |
| `0x1402B85A0/0x1402B8460` | −180 / +360 | yaw wrap |
| `0x1402B8B20` | 0x80000000 ×4 | sign flip mask |
| `0x1402B8AF0` | 0x7FFFFFFF ×4 | abs mask |
| `0x1402B8024` | 0.28 | range gate factor |
| `0x1402B83D0` | 89.0 | FOV clamp |
| `0x1402B834C` | 19.0 | smoothing slider span |
| `0x1402B8224` | 3.0 | predictScale max |
| `0x1402B84A4` | 2000.0 | maxDistance step |
| `0x1402B83DC` | 100.0 | sensitivity read bound |
| `0x1402B849C` | 1000.0 | sensitivity result bound |

Roblox-side offsets used by the Mouse method: camera CFrame `Camera+0xC8` (`0x14032D270` = 0xC8); `MouseService+0x21C` (`0x14032D760` = 0x21C); injector vector `+0x20/+0x28/+0x30` (`0x14032D6E8/6E0/6D0`) and flag `+0x48` (`0x14032D6D8`); sensitivity RVAs `0x8D857D8`, `0x8D857E0`, `0x8D85820` (`0x14032D738/740/718`), Roblox base cached at `0x140334890`.

---

## 8. (G) Do `ovl.renderFps` / `ovl.renderUncap` / "Smooth visuals" change the aim rate?

* **[P]** The aim thread cannot free-run: each iteration it *blocks* in `FUN_14009A2A0` (`__std_atomic_wait_direct`, only wait site in the binary) until the producer publishes new state, and the viewport snapshot it consumes is produced exclusively by the main overlay loop `FUN_140096140` (`0x140097F24`, `0x140098A96`). ⇒ **aim ticks ≤ main-loop iterations**.
* **[P]** The main loop has an explicit frame limiter: a precision wait to an absolute deadline (`FUN_140093510`, `SetWaitableTimer`+`WaitForSingleObject`) at `0x14009876D`, with a re-evaluation flag `0x1403330FB` (set by the settings-apply path `0x140139AC7`) and a 0.75 s stall timeout (`0x2CB41780` ns @ `0x14009873B`). Config keys `ovl.renderFps` (60 default) and `ovl.renderUncap` sit in the same settings table (`ovl.renderFps` @ `0x140339F48`, `ovl.renderUncap` @ `0x14033B480`) — **[I]** that this limiter's period is the one derived from them. **Phase B (§14.5): measured FALSE as the source of the aim tick** — with `ovl.renderUncap=1` (overlay ~140–150 fps) the tick stayed 15.5 ms. (A live read of `0x140339F48` / `0x14033B480` returned unrelated values — 778860143 / 111 — so the config storage at those VAs remains [I].)
* **[P]** Independent of the above, actuation is hard-limited to one step per **4 ms** (≤250 Hz) and, for the Mouse method, to one step per camera update (0.02°/60 ms back-pressure).
* **Effect on feel:** because the filter is `1 - exp(-dt/tau)` with real `dt`, **`renderFps` does not change the convergence curve** — only the step size (and therefore how "stair-steppy" the motion looks) and the number of mouse injections per second. At `renderFps=60` (default) a step is 16.7 ms; at uncapped (e.g. 300 Hz) steps become ~3.3 ms and 5× more injections of 1/5 the size. Both give the same τ = 8 ms·smoothing **[I, from the formula]** — **Phase B (§14.5): the "steps become ~3.3 ms / 5× more injections at uncapped" half is measured FALSE** — with the overlay uncapped (~140–150 fps) the tick stayed 15.5 ms, so the step size and injection count do **not** change with `renderFps`; only the (already tick-agnostic) filter math applies.
* **"Smooth visuals"** is a Misc/render-side setting (its tooltip string @ `0x140332550`: "Needs Smooth visuals on in Misc to have any effect.", referenced only from `FUN_14014BE92`/`0x14002ADFE`) and has **no** code path into the aim tick **[P]** (no reference from `0x1400D9CC0`, `0x1400DAF00`, `0x14010D9C0`).

---

## 9. Reconstruction spec — drop-in replacement for your overlay's aim step

Goal: identical *feel* as velext's Camera/Mouse smoothing with one knob (`smoothing`), plus their gates.

```cpp
// ---- velext-equivalent aim step (C++17) -------------------------------
// Knobs (put in overlay.cfg):
//   aim.smoothing : float, UI-equivalent range 1..20 (<=1 => instant snap)
//   aim.deadzone  : bool  (camera path dot > 0.99996 => skip write)
struct VelextAim {
    uint64_t lastApplyNs = 0;      // velext: 0x140334408
    uint64_t lastInjectNs = 0;     // velext: 0x140334418
    bool     stepInFlight = false; // velext: 0x140332C5A
    float    lastYawDeg = 0.f, lastPitchDeg = 0.f; // velext: 0x140334248 / 0x1403343AC
    float    smoothing = 5.f;      // velext default (profiles.ini)

    // Call once per render/aim tick, after you have (a) the camera CFrame and
    // (b) the world aim point. `nowNs` = your QPC-based ns clock.
    // Returns true if you should actuate this tick.
    bool step(float* camRot9, /* read fresh from the game every tick */
              const Vec3& camPos, const Vec3& aimPoint, uint64_t nowNs, float alpha)
    { /* see below */ }
};

// 1) minimum interval: 4 ms (velext 0x3D0900 ns)  -> <=250 Hz
// 2) dt (seconds): fallback 0.004, clamp [0, 0.1]
// 3) alpha: snapped when smoothing <= 1.0
static inline float velextAlpha(uint64_t nowNs, uint64_t& lastNs, float smoothing) {
    float ts  = std::max(1.0f, smoothing);
    uint64_t prev = lastNs;
    lastNs = nowNs;
    if (prev == 0) return 1.0f;                       // no previous stamp
    float dt = (float)((double)(nowNs - prev) * 1e-9);
    dt = std::clamp(dt, 0.0f, 0.1f);
    if (1.0f >= ts) return 1.0f;                      // <=> smoothing <= 1.0
    float tau = ts * 0.008f;
    if (1e-4f >= tau) return 1.0f;
    return std::clamp(1.0f - std::exp(-dt / tau), 0.0f, 1.0f);
}

// 4) camera path actuation (== FUN_1400D96B0 semantics)
//    look  = -(m[2], m[5], m[8]) from the *fresh* read of the game CFrame
//    dir   = normalize(aimPoint - camPos)
//    if (dot(clamp(look,dir,-1,1)) > 0.99996f) return;         // deadzone
//    mix   = look + (dir - look) * alpha; if (len(mix) < 1e-3f) mix = dir;
//    newDir = normalize(mix); target = camPos + newDir;
//    write ONLY the 3x3 rotation at Camera + 0xC8 (36 bytes), never position.
```

Key points to port 1:1:

1. **Re-read the camera rotation from the game every tick** and mix *from it* (do not keep a private smoothed orientation). That is what makes velext's convergence self-correcting when the player moves the mouse and prevents "two writers fighting".
2. **Mix on the direction vector, then normalize** (nlerp), not quaternion slerp, not euler interpolation.
3. **`alpha = 1 - exp(-dt/tau)`, `tau = 0.008·max(1, smoothing)`**, snap when `smoothing ≤ 1`.
4. Enforce the **4 ms** floor between actuations and the `alpha ≤ 0.002` bail-out.
5. If you actuate with **SendInput** (your current mouse mode) instead of a memory write, add velext's back-pressure so you don't out-pace the game's camera: after each injection, skip the next injection until the *game's* yaw/pitch moved by > 0.02°, with a 60 ms timeout; then recompute from the freshly read camera the same way.
6. Per-step clamp for the mouse path: `lim = clamp(14000·alpha, 12, 400)` counts (64-bit-safe ints).

If you want a single "make it feel like velext" number: `smoothing = 5` (default, τ = 40 ms) is what a fresh velext install runs (`%LOCALAPPDATA%\velext\profiles.ini`, `aim.smoothing=5`, `aim.method=0`).

---

## 10. Diff vs your current `src/overlay.cpp`

| aspect | velext_v1 | your overlay (as of this session) | action |
|---|---|---|---|
| filter domain | direction vector, nlerp + renormalize (per-tick re-read of camera) | writer thread: quaternion **slerp** on the *matrix* using `Quat a = matToQuat(cur)` (line ~1442) | change to vector nlerp to match exactly; slerp is close but not identical (different angular rate at large errors) |
| tau mapping | `tau = 0.008 * max(1, smoothing)`; snap ≤ 1.0 (`0x14032D460`) | `tau = 0.012 + 0.10 * slow` (`aim.memsmooth`, line ~1447) | expose a single `aim.smoothing` and compute `tau = 0.008*max(1,smoothing)`; set default 5 |
| snap | `smoothing ≤ 1.0` ⇒ `alpha = 1` | `memSmooth ≤ 0.001` ⇒ skip filtering (equivalent for 0, but not at 0.01..1) | align the threshold semantics or document the difference |
| dt source | QPC ns, fallback 0.004 s, clamp [0, 0.1] (0x140334408) | `writeDt` clamped [0.0005, 0.05] with `g_writeRateHz` loop (line ~1416) | widen clamp to [0, 0.1]; keep 4 ms min like velext |
| rate limit | **4 ms** hard floor per apply (≤250 Hz) + tick ≤ main-loop rate | writer loop `Sleep(1000/rate)` with `aim.rate = 500` (2 ms) | set 4 ms (`aim.rate = 250`) for exact parity; ≥250 Hz changes nothing in feel but increases injections |
| deadzone | camera: `dot > 0.99996` skip; mouse: 1.5 px screen; `alpha ≤ 0.002` skip | no equivalent constant found in the writer path | add both to kill micro-jitter at the target |
| mouse actuation | **memory injection** of `{x,y,dx,dy}` into the game's input vector (+0x20 vec, +0x48 flag) | `SendInput` relative move (`sendRelMouse`, line ~370) | SendInput is fine for external use, but you must add the 0.02°/60 ms back-pressure, otherwise the feel is "spammy" compared to velext |
| mouse scaling | counts from the game's own coefficients (`MouseService+0x21C`, `base+0x8D857D8` pair, `0.5`/`0.385` factors, cos-pitch correction on X) | `aim.sens`, `aim.gamesens`, empirical `sendRelMouse` px | either read the same game coefficients, or calibrate one `degPerCount` and plug it into velext's formulas (`counts = angleDeg / degPerCount`) |
| cursor mode | when the game is not mouse-locked: move the *cursor* toward the target's screen position at `clamp(14000·dt,12,400)` px | `aim.mode` variants, screen-space EMA | optional |
| prediction | `pred = (cam.pingMs/1000) * predictScale` (0..3) | `snapPos()` extrapolation `dte*cfgAimPScale`, clamp 0.25 s | velext scales by ping/1000 (≈0.05 s default) — clamp your `dte` to your ping, not 0.25 s |

**Phase A update (2026-10-18, applied — backup `backup\overlay_DOT2_AIMSMOOTH_20261018.*`, details in `HANDOFF_aimsmooth.md`):** the overlay now implements the velext model: one `aim.smoothing` knob (1…20, clamp at load, snap ≤ 1), `velextActGate` = QPC-ns dt → `alpha = 1 - exp(-dt/tau)` with `tau = 0.008·max(1,smoothing)` and the 4 ms gate, direction-vector **nlerp from the freshly re-read camera** + `dot > 0.99996` deadzone, mouse-side 1.5 px deadzone and 0.02° back-pressure (legacy free-running slerp stays behind `aim.pid=1` for A/B). Rows 1–6 above are therefore implemented.

Honest caveat for the mouse branch: **SendInput + back-pressure is *not* the same as velext's 20-byte record push.** Measured (Phase B §14.4): velext writes into the game's own input vector and the game consumes it within ≤ 1 ms of the rotation shift; the game then applies it at *its own* input cadence (≈30 ms per consumed step in the D4 trace, vs the 15.5 ms camera tick), so the mouse branch cannot be made 100 % timing-identical; it also only works while the game is mouse-locked (first-person / shift-lock), and it cannot publish the record's `{x,y}` reference fields. The camera branch (direct 36-byte rotation write) has none of these constraints.

---

## 11. Verify plan (no writes to the game, read-only instrumentation)

1. **Smoothing table check (offline math):** compute `alpha = 1-exp(-dt/(0.008*max(1,smoothing)))` for `smoothing ∈ {1,2,5,20}`, `dt = 1/240, 1/200, 1/144, 1/60` and compare with the table in §C.2 (all values are in this document, no exe needed).
2. **Camera-path timing check (needs velext running + the game, ask user first):** attach a *read-only* debugger or memory scanner to velext, watch `0x140334408` (as `0x7FF6C78F0000 + (0x140334408 - 0x140000000)`) — the deltas must be ≥ 4 ms and equal to the overlay's frame period; watch `0x140334248`/`0x1403343AC` in Mouse mode: they only change when a step was actually injected (and the `0x140332C5A` flag returns to 0 before the next one).
3. **Effectivity of the deadzone:** set `aim.smoothing=20`, stand still on a static target; camera rotation must stop changing (writer writes stop) once the angle error ≤ 0.51°; `0x140334408` stops advancing.
4. **FPS independence:** compare crosshair convergence time with `ovl.renderFps=60` vs `ovl.renderUncap=1`; the 63 % time must be ≈ τ (40 ms at smoothing 5) in both cases, only step granularity differs.
5. **Mouse back-pressure:** in Mouse mode, hold the aim key and move the real mouse by hand — velext must stop injecting while you fight it (|Δyaw| > 0.02° each frame), and resume after 60 ms of no change.
6. **Port validation:** run your overlay with `aim.smoothing=5` and time the crosshair→target convergence; expected 63 % at 40 ms, 95 % at 120 ms (independent of your tick rate).

Ask the user before launching velext + Roblox: this session did not run them.

---

## 12. Confidence levels

*Phase B (§14.6) revisits several rows below with live, read-only measurements — check the delta table there before trusting a "medium"/"low" level.*

| claim | level | basis |
|---|---|---|
| smoothing formula/`tau = 0.008·max(1,smooth)`/snap ≤ 1.0 | **high** | exact instructions `0x1400DA71E…0x1400DA7FC`, constants read from the image and from the runtime `.data` dump |
| `dt` source (QPC ns) + fallback/clamps + 4 ms gate | **high** | `0x1400DA775`, `0x1400DA78F`, `0x1400B4D0`, `0x1400DA761`, `0x1400DA84F` |
| camera actuation = 36-byte rotation write at `Camera+0xC8` | **high** | `FUN_1400D96B0`, single caller, CFrame offset value 0xC8 in `.data` |
| nlerp + normalize (not slerp) | **high** | `0x1400D990A…0x1400D99EF` |
| deadzones/clamps constants | **high** | constant addresses + compare sites |
| mouse injection mechanism (vector push + 0x101 flag, no SendInput) | **high** (mechanism) | `FUN_14010D9C0` + import table (no mouse APIs) |
| mouse sub-mode semantics (cursor vs delta) | medium | both code paths mapped; label attribution from geometry (`GetCursorPos`, `atan2f`, `tanf`) |
| identity of `MouseService+0x21C` and `A/B/C` Roblox RVAs | **low/medium** | code chain [P], semantics **[I]** — would need a live read of `robloxBase+0x8D857D8` / `MouseService+0x21C` to confirm values |
| aim tick rate = overlay frame rate (renderFps link) | medium | [P] tick ≤ main-loop iterations + [P] limiter exists; the fps *value* feeding it = **[I]** |
| target selection/priority/part mapping | high (from UI + code) | `FUN_1401453A0`, tables `0x140261F70/80`, tick 0x1400DA240… |
| `aim.targetPart` dropdown global address; `aim.key`/`keyMode` globals | **unknown** | not isolated (UI page mapped, globals not) |
| ships defaults | high | `%LOCALAPPDATA%\velext\profiles.ini` (`aim.smoothing=5`, `aim.method=0`, `aim.fov=15`, `aim.maxDistance=500`, `aim.priority=0`, `aim.targetPart=0`) |

---

## 13. Raw files (evidence)

* `aim_work/disasm_u8.txt` — UTF-8 linear disassembly (656 068 lines), source for all address quotes.
* `aim_work/fn_1400d9cc0.txt` — full aim tick; `aim_work/digest_tick.txt` / `..\digest_tick.txt` — tick globals/calls digest.
* `aim_work/fn_1400d96b0.txt`, `fn_1400d9160.txt`, `fn_1400d9450.txt` — camera read/mix/write.
* `aim_work/fn_1400daf00.txt`, `fn_1400db0d0.txt`, `fn_14009a2a0.txt` — aim thread job / trampoline / cell wait.
* `aim_work/fn_14010d9c0.txt`, `fn_14010d250.txt`, `fn_14010d4d0.txt`, `fn_14010d580.txt` — mouse injector, target resolver, mode probe, sensitivity reader.
* `aim_work/fn_1400fc490.txt` — child-instance-by-name (`MouseService`).
* `aim_work/fn_1401453a0.txt` — AIMBOT UI page (global ↔ widget ↔ label/tooltip).
* `aim_work/fn_1400db130.txt`, `fn_1400d9c00.txt`, `fn_1400e6040.txt`, `fn_140059570.txt` — part lookup, FOV radius, life state, clock.
* `aim_work/imp.py` — IAT resolver (used for all import identifications); `calls.py` — per-function call lister; `ref.py`, `xr.py`, `rt.py`, `st.py`, `cst.py`, `ar.py`, `pe.py`, `dd.py`, `cst.py` — analysis toolbox.
* `velext_dump_data.bin` / `velext_dump_rdata.bin` / `velext_dump_text.bin` — runtime dumps (`.data` base `0x14032D000`).
* `C:\Users\adminn\AppData\Local\velext\profiles.ini` — shipped defaults + real key/section layout (`[Default]`, `[Profile N]`, `active=1`).
* `src/overlay.cpp` — user overlay (style/diff reference only).

Note: `aim.smooth`, `aim.tsmooth`, `aim.memsmooth`, `aim.rate`, `aim.pscale`, `aim.dynk`, `aim.pid`, `aim.sens`, `aim.gamesens` are **invented by this overlay** — they are not velext keys and were not used as evidence about velext anywhere above.

---

## 14. Phase B — live, read-only evidence (2026-10-18/19)

**Instrumentation.** `aim_work/velsample.cpp` → `velsample2.exe` (mingw `g++ -std=c++17 -O2 -static`), run elevated: Roblox via `OpenProcess` + `ReadProcessMemory`; velext via direct-syscall `NtReadVirtualMemory` (id `0x3F`; `rpm_ok=0 syscall_ok=1` in the logs) — **no writes to either process, no remote threads, no input injection**. `ext_overlay.exe` not running; secondary Roblox account in a private place; 2000 Hz sampling, QPC `t_ns`; beep markers for the operator. Launchers `aim_work/run*.cmd` (started via `Start-Process -Verb RunAs -Wait`). Analysis: `brest2.py` (transient α/τ, settled-direction method — see 14.8), `q21/q22.py` (actuation coincidence), `q27.py` (tick dt histogram), `q28.py`, `q29.py` (sensitivity diff).

### 14.1 Live layout verification (write-through)

* Active-profile index `u32 @ 0x140334E80` = 1 (0-based) → active block = `vecBegin + 1·0x208`; **aim base = block + 0x20**. Example: `vecBegin = 0x21ebb641f10`, block1 = `0x21ebb642138`, stride `0x208`, 2 blocks. **Corrected formula (supersedes the session note `vecEnd − 0x1E8`): `base = vecBegin + active·0x208 + 0x20`; `vecEnd − 0x1E8` only coincides when the active block is the last one.**
* UI smoothing 5.00 → live `+0xBC` = **4.983871** (slider quantisation). Turning the aim master ON flipped `+0x9C` 0→1.
* Live values matched `profiles.ini` `[Profile 2]`: `+0x9C`=1, `+0xA0`=method (0 and 1 both observed), `+0xA4`=88, `+0xA8`=1, `+0xB0`=15, `+0xB8`=483.871, `+0xC1`=1, `+0xC4`=0.822581, `+0xC8`=0, `+0xCC`=0, `+0xD0/+0xD1`=0.
* `+0x89` stayed **0 in every sample of every run**, including locked transients → its label stays `[?]` (not a working "visible" flag).
* **Camera position is read at `+0xEC`**; `+0xC4` cannot be the position (`0xC4+0x30` overlaps the `0xC8…0xEB` rotation block). During B8 transients (third-person, standing still) position drift was **0.0 mm** while rotation stepped.

### 14.2 (B) smoothing model — Camera method (method=1)

Protocol: third-person, stand still, ~20–30° off a target, tap **X** (key=88, toggle), do not touch the mouse; targets were moving (prediction ON, live config).

* Run `aim_work/traceB8_cam.csv` (+`.log`): 35 s, 69 808 samples, 0 read failures, live smoothing 4.983871.
* Tick: `v_stamp` (0x140334408) advanced with **dt median 15.50 ms** (1440 intervals; p5 15.00, p95 16.50, min 13.52). Rotation deltas > 0.02° coincided with stamp advances at **median offset 0.00 ms** → **one camera write per tick**. No dt below 13.5 ms in any Phase-B run → **the 4 ms gate never binds at this tick rate**.
* Transient α (settled-direction method, first steps only, trivially-1.0 last step excluded): clean transients give **α = 0.319–0.326 ⇒ τ = 39.1–41.5 ms** vs theory **39.87 ms** (≈2–4 %; all-transient median α ≈ 0.30). Largest transient: 15.2° → 0.46° over 13 steps, step ratio ≈ 0.70.
* Verdict: **MATCH** — `α = 1 − exp(−dt/(0.008·max(1,smoothing)))`, one write per tick, rotation-only.

### 14.3 (C) smoothing sweep

| live smoothing | trace | theory (τ; α @15.5 ms) | measured | verdict |
|---|---|---|---|---|
| 1.0 | `traceC1_s1_cam.csv` | snap | single-write snap: 0.50° → 0.01–0.10° in one tick, no multi-step decay | MATCH (snap) |
| 1.919355 | `traceC2_s19_cam.csv` | 15.35 ms; 0.636 | α 0.628 / 0.659 / 0.628 / 0.633 (4 clean transients) ⇒ τ 14.7–15.7 ms | MATCH (~2 %) |
| 4.983871 | `traceB8_cam.csv` | 39.87 ms; 0.322 | α 0.319–0.326 ⇒ τ 39.1–41.5 ms | MATCH (~2 %) |
| 20.0 | `traceC3_s20_cam.csv` | 160 ms; 0.092 | α 0.073 / 0.088 / 0.075 ⇒ τ 205 / 168 / 192 ms | MATCH within 5–28 % (biased high: target moves during the ~1 s transients) |

* `traceC2_s2_cam.csv` is **void** — the log shows `--smooth=auto: live value 1` (the slider edit had not applied); kept only as a record.
* Bursts with α ≤ 0.01 on huge residuals are hand-mouse tracking, not aim transients (excluded). The τ ∝ smoothing scaling (8 ms/unit) is confirmed across 1.92 → 4.98 → 20 (≈16× step for ≈10.4× smoothing).

### 14.4 (D4) Actuation paths — Camera vs Mouse, live

Mouse run `aim_work/traceD4_mouse.csv` was done in **first-person** (the operator must be mouse-locked for the mouse method to do anything — see below), smoothing 4.983871, method=0.

| metric (rotation Δ > 0.02°) | Camera (B8) | Mouse (D4) |
|---|---|---|
| nearest stamp advance | **median 0.00 ms** | median −7.50 ms, spread ±2 s (unrelated) |
| nearest input-vector size change (mouse queue) | p90 3076 ms (unrelated) | **median 0.50 ms, p10 0.50, p90 1.00** |
| cadence | 15.50 ms (per tick) | ~16–32 ms (per game input consumption) |
| transient α @ smoothing 4.98 | 0.319–0.326 → τ 39.1–41.5 ms | 0.31–0.40 → τ 38.7–47.3 ms |

* The injected 20-byte record (read live at the injection instant): `rec0..rec4 ≈ {640, 388.5, dx, dy, …}` — `x/y` = the locked cursor position (screen centre), `dx/dy` = the mouse counts. **The cursor is not moved onto the enemy; the rotation comes from the deltas.** (Operator question answered.)
* Flag bytes: `u32 @ obj+0x48` reads `0x00010100` (idle) / `0x00010101` (record in flight) → byte `+0x48` toggles 0/1 and the constant `0x0101` sits at `+0x49…+0x4A`. The static note "`u16 0x0101` at `+0x48`" has a 1-byte discrepancy with these reads → keep `[?]`.
* **Mouse method needs mouse-lock**: in third-person free-look, pressing X produces no camera motion (operator-verified in-game, repeatable); in first-person the same injection rotates the camera (D4). Camera method works in third-person (all B/C runs above were third-person). This matches the A.2 sub-modes (delta/mouse-lock mode vs cursor mode).
* Presence of `in_size > 0` alone means nothing — the operator's own mouse fills the same queue in the camera runs too (max 80–100 B in B4/B8); only the ≤1 ms coincidence discriminates.

### 14.5 (D2) Tick source & (D1) sensitivity globals

**(D2) — the aim tick is not the overlay frame rate and not the game fps.**

| configuration | overlay fps | game fps | tick dt (median of `v_stamp` intervals) |
|---|---|---|---|
| B8 / C3 (baseline; ini `renderFps=46`, `gameFpsCap=65`, `uncap=0`) | ~45–60 *(operator)* | ~45–60 *(operator)* | **15.50 ms** (n=1440, min 13.52) |
| `traceD2_fps120.csv` (run while `renderFps` had not actually been changed) | ~45–60 | ~45–60 | 15.50 ms (n=263) |
| `traceD2_ovluncap.csv` (`ovl.renderUncap=1`) | **~140–150** *(operator)* | **~130** *(operator)* | **15.50 ms** (n=234; 96.2 % within 14–17 ms; min 14.45) |

* Rotation deltas still land on stamp ticks (median offset 0.00 ms) → the actuation cadence is unchanged, and the tick is even *faster* than the game's frames when the game ran at 45–60 fps.
* Conclusion: the tick is an **internal ~64.5 Hz cadence** (jitter ±~1 ms, scheduler-like), independent of `ovl.renderFps`/`ovl.renderUncap` and of the game fps. What clocks 64.5 Hz is **[?]** (plausible: the game's fixed-rate scheduler/heartbeat ≈60 Hz reached through the main-loop producer — not the render loop; `[I]`).
* `v_bpstamp` (0x140334418) read as 0 / never advancing in these runs → unusable as a frame counter.
* Live read of `0x140339F48` / `0x14033B480` returned unrelated values (778860143 / 111) → the "config storage" claim for those VAs stays `[I]`; behavior was measured via dt instead.

**(D1) — the live sensitivity is the `A/B/C` float set, not `MouseService+0x21C`.** `traceD1_sens.csv` (30 s, operator dragging Roblox *Mouse Sensitivity* in the Esc menu):

* `base+0x8D857D8`, `+0x8D857DC`, `+0x8D857E0`, `+0x8D857E4`, `+0x8D85820` changed **in lockstep**: 1.0 → 2.8 → 3.4 → **4.0** (held 14 s) → **0.36** (held 10.6 s); the drag motion itself was captured in flight.
* `MouseService+0x21C` = **1.0 constant**; `base+0x8D85824` = **5.7 constant** → neither is the slider value.
* ⇒ velext's `out1 = |f|·A.x`, `out2 = A.y·f` scale with the slider through `A.x/A.y` (`f` = 1.0 constant in this setup) — so the Mouse-method angle→count conversion does follow the game sensitivity. Range seen: 0.36 … 4.0.

### 14.6 Corrections & confidence deltas (nothing above was silently changed)

| # | earlier claim | Phase B result | evidence |
|---|---|---|---|
| 1 | tick = min(main-loop publish rate, 250 Hz), "publish rate = overlay frame rate" `[I]` (§3) | **disproven** — fixed ~15.5 ms (64.5 Hz) regardless of overlay fps (45→150) and game fps (45→130) | 14.5 |
| 2 | uncapping → ~3.3 ms steps / 5× injections `[I]` (§8) | **disproven** — steps stayed 15.5 ms | 14.5 |
| 3 | `MouseService+0x21C` = sensitivity multiplier `[I]` (§5/**low** in §12) | live: constant **1.0** while the slider moved 1→4→0.36; the slider value lives in the `A/B/C` floats | 14.5 |
| 4 | identity of the `A/B/C` Roblox RVAs (**low/medium**, §12) | **proven** — they are the live Mouse-Sensitivity value (5 mirrors move in lockstep) | 14.5 |
| 5 | smoothing formula / τ / snap (static high) | **confirmed live** (1.0 / 1.92 / 4.98 / 20; ≈2 % at 1.92 & 4.98) | 14.2–14.3 |
| 6 | camera actuation = 36-byte rotation write at `+0xC8`, position untouched (static high) | confirmed live (steps exactly at ticks; 0.0 mm position drift during transients) | 14.2 |
| 7 | mouse actuation = 20-byte record push + flag, no SendInput (static high) | confirmed live at the queue level (≤1 ms coincidence; record = centre x/y + deltas) | 14.4 |
| 8 | mouse sub-modes (cursor vs delta) `[medium]` | delta/mouse-lock mode observed directly; **mouse method requires mouse-lock in-game** | 14.4 |
| 9 | flag `u16 0x0101 @ +0x48` (static high) | live bytes: `+0x48` toggles 0/1, constant `0x0101` at `+0x49…4A` → 1-byte discrepancy, unresolved `[?]` | 14.4 |
| 10 | `aim.targetPart` / `aim.key` / `keyMode` globals `[unknown]` (§12) | live values at `+0xC8 / +0xA4 / +0xA8` matched the ini in every run — **D3 (§14.10) proved all four live**: targetPart `+0xC8`, priority `+0xCC`, key `+0xA4`, keyMode `+0xA8` → `[P]` | 14.1, 14.10 |
| 11 | session note: "`+0x89` vis flag" | 0 in every sample incl. locked transients → not a working flag, keep `[?]` | 14.1 |
| 12 | session note: "`0x140334390 == 0` ⇒ no target" | **wrong** — clean transients ran while it stayed 0; it is the per-frame id of §3 | 14.2 |
| 13 | session note: camera position candidate `0xC4` | **wrong** — `0xC4+0x30` overlaps the rotation block; the position is at `0xEC` | 14.1 |

Not measured (still open): `renderFps`/`uncap` config storage VAs, the 64.5 Hz clock source, the `+0x48`/`+0x49` flag byte. No writes were made to the game or to velext during Phase B.

### 14.7 Trace inventory

| file | what it is |
|---|---|
| `aim_work/traceB8_cam.csv` + `.log` | B evidence (camera, smoothing 4.983871, third-person) — primary B/C dataset |
| `aim_work/traceC1_s1_cam.csv` | C1 smoothing 1.0 (snap) |
| `aim_work/traceC2_s19_cam.csv` | C2 smoothing 1.919355 |
| `aim_work/traceC2_s2_cam.csv` | C2 first attempt — **void** (slider not applied; live value 1) |
| `aim_work/traceC3_s20_cam.csv` | C3 smoothing 20.0 |
| `aim_work/traceD4_mouse.csv` | D4 mouse method (first-person), smoothing 4.983871 |
| `aim_work/traceD2_ovluncap.csv` | D2 overlay uncapped (~140–150 fps), game ~130 fps |
| `aim_work/traceD2_fps120.csv` | D2 run before the fps change took effect (baseline repeat) |
| `aim_work/traceD1_sens.csv` | D1 sensitivity drag |
| `aim_work/traceD3_ui.csv` | D3 run 1 — targetPart + priority UI diff |
| `aim_work/traceD3b_keys.csv` | D3 run 2 — aim key + keyMode UI diff |
| `aim_work/traceB3.csv`, `traceB5..B7` | early/negative runs — not used (mixed provenance / frozen / no aim activity) |

### 14.8 How α was measured (settled-direction method)

The game's aim point (pb) is offset ~0.45° from where the camera settles (moving target, prediction), so per-transient: take the camera direction **after settling** as the reference target `T`, then for each step of the transient `α = angle(step) / angle(remaining before the step)`, using the first ≤3 steps and **excluding the last step** (trivially α=1). Median per transient; `τ = −dt̄/ln(1−α)`. Hand-mouse bursts and target-switch transients are excluded by residual/step sanity checks.

### 14.9 Repro

```bat
:: build the sampler (mingw g++)
g++ -std=c++17 -O2 -static aim_work\velsample.cpp -o velsample2.exe

:: example run (elevated; exact args per run*.cmd)
::   runB8.cmd : velsample2.exe --sec=35 --rate=2000 --delay=10 --smooth=auto --csv=aim_work\traceB8_cam.csv --log=aim_work\traceB8_cam.log
::   runC1.cmd : ... --sec=20 --rate=2000 --delay=8  --csv=aim_work\traceC1_s1_cam.csv ...
Start-Process cmd.exe -ArgumentList "/c","aim_work\runB8.cmd" -Verb RunAs -Wait

:: analysis
python aim_work\brest2.py aim_work\traceB8_cam.csv aim_work\traceC2_s19_cam.csv aim_work\traceC3_s20_cam.csv aim_work\traceD4_mouse.csv
python aim_work\q22.py    aim_work\traceD4_mouse.csv aim_work\traceB8_cam.csv     :: rot vs stamp vs input-size coincidence
python aim_work\q27.py    aim_work\traceD2_ovluncap.csv aim_work\traceB8_cam.csv  :: tick dt histogram
python aim_work\q29.py                                                            :: sensitivity transitions (traceD1_sens.csv, path hardcoded)
```

### 14.10 (D3) UI → memory diff (2026-10-19)

Two runs, UI changes only (no aiming); transitions = value runs stable ≥ 40 samples.

Run 1 — `aim_work/traceD3_ui.csv` (40 s):

| control changed | trace column (offset) | observed | verdict |
|---|---|---|---|
| targetPart: Head → 1 → 3 → Head | `v_tpart` (`+0xC8`) | 0→1 @ t=2.20 s, 1→3 @ 6.28 s, 3→0 @ 9.36 s | **`+0xC8` = targetPart [P]** |
| Priority: 0 → 1 → 0 | `v_prio` (`+0xCC`) | 0→1 @ 21.60 s, 1→0 @ 22.89 s (operator-confirmed: Priority dropdown) | **`+0xCC` = priority [P]** |

Run 2 — `aim_work/traceD3b_keys.csv` (25 s):

| control changed | trace column (offset) | observed | verdict |
|---|---|---|---|
| aim key: X → V → X, then → Y → Z | `v_key` (`+0xA4`) | 88→86 @ 3.66 s, 86→88 @ 4.97 s, 88→89 @ 14.34 s, 89→90 @ 15.72 s | **`+0xA4` = aim key [P]** (Windows VK codes) |
| keyMode: Toggle → Hold → Toggle, then → Hold | `v_keymode` (`+0xA8`) | 1→0 @ 9.01 s, 0→1 @ 10.95 s, 1→0 @ 22.78 s | **`+0xA8` = keyMode [P]** (1 = Toggle, 0 = Hold) |

Baseline row (run 2 start): `key=88, keyMode=1, tpart=0, prio=0, method=1, enabled=1, pred=1, sticky=0, igteam=1, igdown=0`. No other settings column transitioned in either run (noise-filtered: > 15 transitions ignored). Note: run 2 left the velext settings at **key=Z (90), keyMode=Hold** — restore in the velext UI if not intended.
