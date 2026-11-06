# HANDOFF S3 — tách entity cache / ESP / aim khỏi `overlay.cpp`

> Ngày: 2026-10-20. Tiếp nối `HANDOFF_S2_cfg_server_world.md`.
> Kế hoạch: `PLAN_S3_aim_esp_cache.md` (duyệt full core S3.1 → S3.3a).
>
> | | Dòng | Byte | SHA-256 |
> |---|---:|---:|---|
> | Trước S3 (S2POST) | 4.085 | 196.206 | `5DAA89C05A605D9100636F1006B45EF7675145F0DCC6DD761AAC1CC1103CA9E4` |
> | Sau S3 (core) | **2.691** | **122.334** | `FD8F98EA34DBA05F87CD7EE7745C65AE8A7328F35B15F8B41A58BA1F894A825C` |
>
> Giảm **1.394 dòng / 73.872 B (37,7%)**. Không đổi key cfg, default hay thứ tự tab.
> S3.3b (writer + velext math + `-aimtest`) **chưa làm** — xem §9.

---

## 1. Kết quả

Move **verbatim** (chỉ đổi signature, `static` → external, thêm include):

| Khối | Nguồn (S3PRE) | Dòng | Đích |
|---|---|---:|---|
| Entity cache (`classOf`, `resolveParts`, `clearResolvedParts`, `refreshEnts`) | 1.582–1.887 | 306 | `src\ovl\ovl_cache.cpp` |
| ESP + radar | 3.060–3.283 | 224 | `src\ovl\ovl_esp.cpp` |
| Aim tick + trigger (gồm cả `AimTarget`, evaluate, memory/mouse actuation) | 3.285–3.999 | 715 | `src\ovl\ovl_aim.cpp` (`AimCtx::tick`) |
| Autoclick | 4.008–4.050 | 43 | `src\ovl\ovl_aim.cpp` (`AimCtx::autoclickTick`) |
| `releaseTriggerIfDown` / `restoreViewportIfNeeded` | 1.901–1.926 | 26 | `AimCtx` methods |
| `w2s` projection | 1.888–1.900 | 13 | `src\ovl\ovl_view.h` (`screenW2S`) |
| `AimSnap` + 4 snapshot globals | 797–813 | — | `src\ovl\ovl_aim.h` |

`overlay.cpp` còn: khung menu, main loop, hitbox, teleport, input, player mods, world/server tick, writer thread, `-aimtest`.

---

## 2. File map sau S3

| File | Dòng | Nội dung |
|---|---:|---|
| `src\overlay.cpp` | 2.691 | menu, main loop, hitbox, teleport, input, writer, self-test |
| `src\ovl\ovl_cache.h` | 35 | `EntityCtx` (6 ref + 5 owned) |
| `src\ovl\ovl_cache.cpp` | 301 | cache 3 tầng + NPC sweep |
| `src\ovl\ovl_view.h` | 19 | `screenW2S` inline |
| `src\ovl\ovl_esp.h` | 30 | `EspCtx` (10 ref) |
| `src\ovl\ovl_esp.cpp` | 238 | ESP + radar |
| `src\ovl\ovl_aim.h` | 150 | `AimSnap`, `MouseCtlState`, `VelextSens`, constants, `AimCtx` |
| `src\ovl\ovl_aim.cpp` | 827 | constructor, tick, trigger, autoclick, reset methods, `w2s` |
| `src\ovl\ovl_core.h` | 9 | `diagRate`, `qpcSeconds`, `qpcNs` |
| `src\ovl\ovl_state.h` | 124 | thêm `loadHitboxProfile`, `hitboxRestoreAll` |

Symbol external mới:
- `loadHitboxProfile`, `hitboxRestoreAll` (bỏ `static` trong `overlay.cpp`).
- `qpcSeconds`, `qpcNs`, `sendRelMouse`, `velextActGate`, `camYawPitchDeg`, `readVelextSens`, `velextMouseCounts`, `mouseCursorHidden`, `wrap180f`.
- `g_snapMu`, `g_snap`, `g_snapEpoch`, `g_snapSeq`; `invalidateAimSnapshot()` free function.
- `EntityCtx::refreshEnts`, `EspCtx::draw`, `AimCtx::{tick,autoclickTick,releaseTriggerIfDown,restoreViewportIfNeeded,onAimDisabled,onAutoclickEnabled}`.

---

## 3. Build

`build_fast.bat` + `build.bat` (danh sách tường minh, **không glob**), thêm sau `ovl_world.cpp`:

```
src\ovl\ovl_cache.cpp src\ovl\ovl_esp.cpp src\ovl\ovl_aim.cpp
```

Không đổi danh sách tool standalone.

---

## 4. Gate tự động

| Gate | S3.1 | S3.2 | S3.3a |
|---|---|---|---|
| `build_fast.bat` | exit 0, 0 warning | exit 0, 0 warning | exit 0, 0 warning |
| `build.bat` | exit 0, 22 tool exit 0 | exit 0, 22 tool exit 0 | exit 0, 22 tool exit 0 |
| `-aimtest` | 522 dòng, 0 FAIL | 522 dòng, 0 FAIL | 522 dòng, 0 FAIL |
| `-srvtest 0` | PASS | PASS | PASS |
| 8 tab hits | n/a | n/a | **PASS** (so S3PRE) |
| LF / BOM | 0 CR, no BOM | 0 CR, no BOM | 0 CR, no BOM |
| `overlay.cfg` | không đổi hash | không đổi | không đổi |

`-aimtest` **không byte-identical** giữa các lần chạy: output chứa timing thật
(`attemptN +x.xxx ms`, `dt=`, `alpha=`), lệch 2–4 dòng mỗi lần. Nội dung còn lại
khớp 100%, 0 `FAIL`. Đây là sai lệch đã biết, không phải regression.

8 tab hits: lần đầu bị chặn vì chưa mở game; đã chạy lại đầy đủ sau khi mở
Roblox — kết quả ở §5.

---

## 5. Gate in-game — **PASS**

Checklist in-game đã chạy ngày 2026-10-20 (Roblox đang mở, PID 25400):

- **Cache**: session thật 18 phút (19:36–19:54) — `entity refresh ... cache=8 ents=8`
  đều mỗi giây, **0** `entity-reresolve`, process không abort. Sandbox: 60 refresh,
  `cache=7 ents=7`, 0 reresolve.
- **Aim**: log có chuỗi `aim state=ON/OFF` khi user bấm hotkey — tick chạy, không crash.
- **Menu/layout**: 8 tab chạy lại; hit count khớp **S3PRE** trên cùng trạng thái game
  (tab0 17, tab1 15, tab2 28, tab3 23–24, tab4 25, tab5 23, tab6 10, tab7 32).
  Khác `s2_hits_final.txt` ở tab2/3/4/5 là do lúc baseline server rỗng (tab Teleport
  ít row) và một dòng toast transient +17px — matrix S3PRE/S3.1/S3.2/S3.3a chứng minh
  không phải regression.
- **ESP**: `EspCtx::draw` chạy mỗi frame với `vm=1`, không crash trong 18 phút.
  Kiểm tra cảm giác box/radar vẫn cần mắt người; code move verbatim + matrix layout pass.
- Assert tạm đã gỡ sau session này; build final không còn `assert`.

---

## 6. Sai lệch / tạm thời

- **Assert ref**: đã chạy 18 phút in-game với assert bật, không fire; sau đó gỡ khỏi
  `overlay.cpp` và build lại. Không còn `assert`/`<cassert>` trong source.
- `AimCtx` giữ **ref** cho `aimOn/trigOn/acOn/stickPlayer` và 13 biến main khác;
  state aim-only nằm trong `AimCtx`. Không copy state sang main.
- `frame` vẫn không tăng (giữ 0) nên viewport refresh chạy mỗi frame; khối này
  **không** move.
- `uisAddr` và `stickPlayer` vẫn thuộc main, `EntityCtx` giữ ref.
- `restoreViewportIfNeeded(false)` vẫn gọi mỗi frame ngoài `if (haveVm)`, đúng vị trí cũ.
- Writer thread + `-aimtest` + velext math vẫn ở `overlay.cpp` (S3.3b).

---

## 7. Rollback

Toàn bộ S3:

```
copy backup\S3PRE_20261020\src\overlay.cpp      src\overlay.cpp
copy backup\S3PRE_20261020\src\ovl\ovl_state.h  src\ovl\ovl_state.h
copy backup\S3PRE_20261020\build_fast.bat       build_fast.bat
copy backup\S3PRE_20261020\build.bat            build.bat
del  src\ovl\ovl_cache.* src\ovl\ovl_esp.* src\ovl\ovl_view.h src\ovl\ovl_aim.*
```

Từng bước: `backup\S3.1_cache\`, `backup\S3.2_esp\`, `backup\S3.3a_aim\`
(mỗi thư mục có `src\`, exe, build script, log, `sha256.txt`).

Không dùng `git checkout src\overlay.cpp` — autocrlf có thể đổi LF → CRLF.

---

## 8. Snapshot

- `backup\S3PRE_20261020\` — trước S3, hash `5DAA…`, baseline aimtest/srvtest/build log.
- `backup\S3.1_cache\` — sau cache.
- `backup\S3.2_esp\` — sau ESP.
- `backup\S3.3a_aim\` — sau aim, còn assert tạm (dùng để so).
- `backup\S3POST_20261020\` — **final**: đã gỡ assert, build lại, kèm aimtest/srvtest/
  build log, 8-tab hits, `AGENTS.md` + handoff này. Exe hash `82498C71…`.

---

## 9. Việc còn lại

1. (Optional) S3.3b: move writer + velext math + `-aimtest` sang `ovl_aim.cpp`,
   thêm API `aimWriterStart/Stop/SetRate/SetCam`; `overlay.cpp` còn ~2.130 dòng.
   Gate: `-aimtest` byte-identical (so file, không chỉ exit code).
2. Kiểm tra cảm giác ESP/aim bằng mắt trong phiên chơi thật. Gate code/crash/log
   đã pass; phần "nhìn có đúng ý" không tự động hoá được.
