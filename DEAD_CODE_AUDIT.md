# DEAD-CODE AUDIT — overlay.cpp + phụ trợ
Ngày: 2026-10-20 · Bản hiện tại: `src\overlay.cpp` 7.217 dòng · **CHƯA SỬA GÌ** (chỉ báo cáo)

## 0. Cách kiểm (bằng chứng)

| # | Phương pháp | Kết quả |
|---|---|---|
| 1 | Compile `g++ -O2 -Wall -Wextra -Wunused-const-variable=2` (chỉ `-c`, không ghi đè exe) | 20 warning / 19 thật; `offsets.cpp` sạch (0 warning) |
| 2 | Script cross-ref toàn bộ 236 biến file-scope: phân loại refs theo load (945–1244) / save (`upsertCfgKey`) / logic | 233 biến có dùng thật; 3 nghi chết |
| 3 | Script "genuine read" cho mọi biến file-scope (loại dòng gán thuần + `m.read(...)` địa chỉ) | 10 biến 0-read (bảng G2) |
| 4 | Struct-field scan (14 struct) — đếm read/write ngoài khối def | 6 field chết (bảng G3) |
| 5 | Offsets consumer scan toàn repo (`.field` trong mọi `src\*.cpp` + `sdk.h` + `memory.h`, trừ `offsets.cpp`) | 13 field 0 consumer (bảng G4) |
| 6 | Đếm callsite 141 `static` function + GCC `-Wunused-function` | 0 function chết |
| 7 | Link test thật (ra exe tạm, KHÔNG đụng `ext_overlay.exe`) để kiểm `-ldwmapi` | **`-ldwmapi` là BẮT BUỘC** — libimgui.a cần `DwmIsCompositionEnabled`/`DwmGetColorizationColor`/`DwmEnableBlurBehindWindow` |

Giới hạn: không có `#if 0`/`if(false)` trong file; không có macro sinh symbol; các kết luận "chết" cho biến/field đều là 0-tham-chiếu (không phải suy đoán).

---

## G1. Dead chắc chắn — compiler chứng minh (xoá an toàn)

Toàn bộ nằm trong khối aim cũ / radar. Gỡ **cả câu gán** (dead store), không chỉ decl.

| Biến | Def | Ghi tại | Ghi chú |
|---|---|---|---|
| `lastAimTick` | 3818 | 4371, 6446 | aim cũ |
| `vAimInit` | 3823 | 4374 | virtual-aim cũ |
| `vAimX`, `vAimY` | 3824 | 4375, 4376 | virtual-aim cũ |
| `vAimPlayer` | 3825 | 4377 | virtual-aim cũ |
| `drawnGate` | 3826 | 6603, 6670, 6672 | "DEBUG: massive track gate" |
| `smAim` | 3833 | — (chỉ def) | smoothed aim cũ |
| `smPrim` | 3834 | — | smoothed aim cũ |
| `smInit` | 3835 | 4378 | smoothed aim cũ |
| `smSX`, `smSY` | 3836 | — | smoothed aim cũ |
| `smSPrim` | 3837 | — | smoothed aim cũ |
| `smSInit` | 3838 | 4379 | smoothed aim cũ |
| `shownLeadMs` | 3863 | 6711 | "readout" cho menu cũ |
| `shownErrPx` | 3864 | 6805, 7005 | readout cũ |
| `shownPxCount` | 3865 | 5053 | readout cũ |
| `dbgTgt` | 3872 | 6439, 6703 | remote-diagnostic cũ |
| `eh` (cục bộ) | 6354 | — | `int ew = ..., eh = ...;` → bỏ vế `eh` |

Ước lượng: ~20 dòng. Riêng block reset 4371–4379 sẽ ngắn đi 9 dòng.

---

## G2. Global "chỉ ghi, không ai đọc" (script chứng minh)

| Biến | Def | Ghi tại | Ghi chú |
|---|---|---|---|
| `g_plyCurJumpH` | 879 | — (chỉ def) | comment "JumpHeight (mode 0 auto)" nhưng không còn code đọc/ghi |
| `g_plyCurJump` | 888 | 4641 | `jp` vẫn dùng cho `g_plyRestJP` — chỉ xoá 1 câu gán |
| `g_hwnd` | 822 | 3714 | comment "stream-proof affinity" nhưng không đọc; WDA dùng `hwnd` local |
| `g_menuWinX`, `g_menuWinY` | 883, 884 | 5184, 5185 | "HUD debug" — không còn HUD nào đọc |
| `hitboxShown` | 1369 | 1487 | không hiển thị ở đâu |
| `hitboxSelfTick` | 1371 | 1493 | không có consumer |
| `cfgHitboxPlace` | 1366 | 1384 | trong `loadHitboxProfile()`; tham số `place` vẫn dùng cho phần còn lại của hàm |

Ước lượng: ~12 dòng.

---

## G3. cfg key chết + field struct chết

### cfg key chết (load nhưng biến không ai đọc)
| Var | Def | Load | Kết luận |
|---|---|---|---|
| `cfgAimIgnoreDead` | 741 | 972 | key `aim.ignoredead` — xoá var + dòng load. Key cũ còn trong overlay.cfg của user **vô hại** (loader bỏ qua key lạ) |
| `cfgAimPark` | 752 | 982 + clamp 1225 | comment ghi "retained for config compatibility" — không có logic nào dùng. Xoá nếu không cần compat (khuyến nghị: xoá) |

### Struct fields chết
| Field | Src | Bằng chứng | Dọn kèm |
|---|---|---|---|
| `Ent.uid` (3898) | overlay.cpp | chỉ có 1 ref = dòng ghi 4059 `m.read(p + o.playerUserId, e.uid)` — không đọc | `o.playerUserId` **GIỮ** (espcheck.cpp dùng 58 chỗ) |
| `AimTarget.fx`/`fy` (6458) | overlay.cpp | ghi 6539/6540, không đọc | — |
| `AimTarget.score` (6461) | overlay.cpp | ghi 6579, không đọc | — |
| `SrvState.creatorId` (2395) | overlay.cpp | ghi 2940 (+local `cid` 2937–2940), không đọc | kéo theo **`o.dataModelCreatorId`** (offsets.h:101, offsets.cpp:98) hết consumer toàn repo → xoá |
| `SrvRow.fps` (2191) | overlay.cpp | parse 2380, không hiển thị (fps ở HUD/footer là fps render của overlay) | — |

Ước lượng: ~12 dòng overlay.cpp + 2 dòng offsets.h/cpp.

---

## G4. Offsets payload không consumer (repo-wide) — 13 field

`.field` = 0 trên **toàn bộ** source (overlay + sdk + memory + 25 tool; trừ chính mapping trong offsets.cpp). Chỉ tốn chỗ trong struct + JSON→struct copy.

```
cameraViewUnused                 (còn không được fill — placeholder P1, offsets.h:32)
lightingEnvironmentDiffuseScale
lightingEnvironmentSpecularScale
lightingFogColor
atmosphereColor
atmosphereDecay
effectIntensity
effectSize
effectBrightness
effectContrast
effectTintColor
terrainWaterWaveSpeed
terrainWaterColor
```

**KHÔNG đụng** các field World đang dùng: `atmosphereDensity/Haze/Glare`, `bloom*`, `blurSize`, `terrainGrass*`, `terrainWaterTransparency/Reflectance`, `effectEnabled` (worldFX dùng `o.effectEnabled` tại 2037/2041/2045; các `effect*` khác mới chết) — nhóm còn lại có consumer trong tool khác.

Ước lượng: ~13 dòng offsets.h + ~12 dòng mapping offsets.cpp.

---

## G5. Hạ tầng (đã test thật)

| Item | Kết luận |
|---|---|
| `#include <dwmapi.h>` (overlay.cpp:7) | Nguồn overlay **không** dùng symbol `Dwm*` → include thừa, bỏ được (cosmetic) |
| `-ldwmapi` (build_fast.bat:10, build.bat:25) | **BẮT BUỘC GIỮ** — link test thiếu flag fail với `imgui_impl_win32.o` (3 symbol Dwm*) |

---

## G6. KHÔNG chết — đừng xoá vội (cần biết)

1. **Legacy aim filter `aim.pid=1`** (vẫn reachable khi `aim.mode=0`):
   `cfgAimTSmooth` (6762), `cfgAimMemSmooth` (3290, writer thread), `cfgAimSmooth` (6973),
   `matToQuat`/`slerpQ`/`quatToMat` (3296–3301). → user quyết định; đề xuất **giữ** đến khi chốt bỏ `aim.pid`.
2. **Setting file-only** (không UI, không có callsite save — chỉ chỉnh tay trong `overlay.cfg`):
   `aim.maxdist`, `aim.deadzone`, `aim.interval`, `aim.rate`, `aim.sens`,
   `trig.delay`, `trig.pad`, `trig.cooldown`, `trig.press`, `trig.keymode`,
   `autoclick.duration`, `autoclick.interval`, `autoclick.keymode`, `autoclick.press`,
   `input.chatguard`. → functional, KHÔNG phải dead code (đây là kiểm kê, không phải lỗi).
3. **`if (true) {` scaffold** ở 1492 (hitboxTick) — dedent được, hành vi y nguyên.
4. **Warning misleading-indentation 1496–1502**: block log `hitbox_self.log` không được `if` bọc (thiếu braces). Hiện chỉ là logging chạy mọi tick; "sửa" sẽ đổi log → để nguyên hoặc fix có chủ ý.

---

## G7. Đã quét — SẠCH (không có gì để xoá)

- **0 function chết**: GCC `-Wunused-function` không báo cái nào; 3 hàm bị regex đếm "0 callsite" (WndProc, writerThread, srvWorkerThread) thực ra được truyền dạng callback (`lpfnWndProc = WndProc`, `std::thread(writerThread)`) — GCC xác nhận có dùng.
- **0 unused parameter** (`-Wextra`); `offsets.cpp` compile sạch.
- **SDK 14/14 method** và **Memory 8/8 method** đều có consumer.
- Các struct còn lại (Waypoint, `SrvWire` 11 field, phần còn lại `SrvState`, MouseCtlState, WorldState, AimSnap, VelextSens, MapCase, Quat): mọi field đều đọc.
- Các include khác (`d3d11`, `timeapi`, `shellapi`, `cctype`, `thread/mutex/atomic`, ...) đều có token sử dụng.

---

## G8. Đề xuất thứ tự làm sạch + verify

| Bước | Nội dung | Verify |
|---|---|---|
| **C1** | overlay.cpp: G1 + G2 + G3-phần-overlay (~45 dòng) | `build_fast.bat` exit 0 → `-aimtest` exit 0 → hits 8 tab == baseline (S1.0) |
| **C2** | offsets.h/cpp: `dataModelCreatorId` + 13 field G4 (~27 dòng) | `build.bat` full (đảm bảo tool khác link được) + build_fast |
| **C3** | overlay.cpp:7 bỏ `#include <dwmapi.h>` (tuỳ) | build_fast |

**Thứ tự với S1:** chạy sau **S1.0** (backup + baseline) và **trước S1.1** — để S1 move code đã sạch (các tab cũng bớt rác). Cũng có thể làm sau S1 nếu muốn S1 thuần verbatim 100% — nhưng khi đó phải sửa thêm các file tab mới.

Tổng ước lượng gỡ: **~45 dòng overlay.cpp + ~27 dòng offsets.h/cpp + 1 include**. Không đổi hành vi, không đổi tên cfg key nào còn dùng.
