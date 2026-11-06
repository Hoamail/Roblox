# HANDOFF S2 — tách cfg IO / Server / World khỏi `overlay.cpp`

> Ngày: 2026-10-20. Tiếp nối `HANDOFF_S1_split_menu.md`.
> Kế hoạch: `PLAN_S2_cfg_server_world.md` (đã duyệt, có bổ sung gate full build + diag world).
>
> | | Dòng | Byte | SHA-256 |
> |---|---:|---:|---|
> | Trước S2 | 6.123 | 280.220 | `6B86DB0FD56DF073E791658FE4674CB802A2319A3B5B09D2B2239237F16ED1CF` |
> | Sau S2 | **4.085** | **196.206** | `5DAA89C05A605D9100636F1006B45EF7675145F0DCC6DD761AAC1CC1103CA9E4` |
>
> Giảm **2.038 dòng / 84.014 B (33,3%)**. Không đổi hành vi aim/ESP/menu/tab.

---

## 1. Kết quả

Đã tách 3 khối có ranh giới rõ, move **verbatim** (chỉ bỏ `static` ở symbol cần export):

| Khối | Nguồn (S2PRE) | Số dòng | Đích |
|---|---|---:|---|
| cfg IO (exe dir, toggle key, upsert, parser, profiles, waypoints) | 52–175, 484–515, 736–737, 740–1161 | 580 | `src\ovl\ovl_cfg.cpp` |
| Server (JSON, HTTP worker, join, ping, watchdog, anti-AFK, self-test) | 1382–2364 | 983 | `src\ovl\ovl_server.cpp` |
| World (Lighting/Atmosphere/Terrain/effects/FOV/zoom) | 905–1381 | 477 | `src\ovl\ovl_world.cpp` |
| NT write syscall | `memory.h` static per-TU | — | `src\ntwrite.cpp` |

`overlay.cpp` còn lại: khung menu, main loop, aim, ESP, entity cache (S3), hitbox, teleport, input, dispatch tab.

---

## 2. File map sau S2

| File | Dòng | Nội dung |
|---|---:|---|
| `src\overlay.cpp` | 4.085 | menu, main loop, aim, ESP, entity cache, hitbox, teleport, input |
| `src\ntwrite.cpp` | 23 | định nghĩa duy nhất `g_ntWriteId`, `ntWriteSyscall` |
| `src\ovl\ovl_cfg.h` | 42 | API cfg + extern 43 biến (41 biến de-static + `cfgSrvLastJob/Place`) |
| `src\ovl\ovl_cfg.cpp` | 593 | exe dir, toggle, upsert, parser, profiles, waypoints |
| `src\ovl\ovl_server.h` | 16 | `g_srvRun`, `srvWorkerThread`, `srvTick`, `runSrvSelfTest`, `srvValidJob` |
| `src\ovl\ovl_server.cpp` | 1.001 | server module (983 dòng block + include) |
| `src\ovl\ovl_world.h` | 8 | `worldRestoreAll`, `worldTick` |
| `src\ovl\ovl_world.cpp` | 485 | world module (477 dòng block + include) |
| `src\ovl\ovl_core.h` | 5 | `diagRate` (dùng bởi server + world) |
| `src\ovl\ovl_state.h` | — | extern state cũ; không đổi tên key |
| `src\ovl\ovl_ui.h` | 29 | thêm `#include "ovl_cfg.h"`, bỏ prototype `upsertCfgKey` cũ |

Xuất symbol chính:
- cfg: `initExeDir`, `exePath`, `loadToggleKey`, `loadAimCfg`, `upsertCfgKey`, `cfgStrValue`, `profileExists/save/load`, `setPlaceProfile`, `loadWaypoints`.
- server: `g_srvRun` (atomic), `srvWorkerThread`, `srvTick`, `runSrvSelfTest`, `srvValidJob`; state `srv`, `g_srvWire`, `g_srvSel` vẫn định nghĩa trong `ovl_server.cpp`.
- world: `worldRestoreAll`, `worldTick`.
- core: `diagRate` (bỏ `static` tại định nghĩa trong `overlay.cpp`).

---

## 3. Build

`build_fast.bat` và `build.bat` (danh sách tường minh, **không glob**):

- overlay: thêm `src\ntwrite.cpp`, `src\ovl\ovl_cfg.cpp`, `src\ovl\ovl_server.cpp`, `src\ovl\ovl_world.cpp`.
- `ext_ntcheck` + `ext_wpmtest`: thêm `src\ntwrite.cpp`.

Thứ tự file trong 2 script giống nhau; nhớ sửa **cả hai** khi thêm file.

---

## 4. Đường ghi memory (NtWriteVirtualMemory)

`src\memory.h` giờ chỉ còn:

```cpp
extern uint32_t g_ntWriteId;
long ntWriteSyscall(void* handle, void* addr, const void* buf, size_t size,
                    size_t* written);
```

Định nghĩa duy nhất ở `src\ntwrite.cpp`. Hệ quả:

- Mọi TU link `ntwrite.cpp` dùng chung id đã resolve bởi `openWrite()`.
- Không còn cảnh "TU chưa gọi `openWrite()` thấy `g_ntWriteId=0` → fallback `WriteProcessMemory`".
- Bất kỳ tool nào gọi `m.write`/`openWrite` **bắt buộc** link `src\ntwrite.cpp`, nếu không sẽ lỗi link `g_ntWriteId`/`ntWriteSyscall`.

Bằng chứng:
- `ext_ntcheck.exe`: `resolved NtWriteVirtualMemory id = 58 (0x3a) directWrite=1`, `ntdll stub ... MATCH direct-call ABI OK`, exit 0.
- Diag tạm trong `ovl_world.cpp` (đã gỡ): `world write path direct=1 id=58` mỗi giây → `worldTick` chạy trong TU riêng vẫn đi qua syscall.
- Sau khi gỡ, `b'world write path' in ext_overlay.exe == False`.

---

## 5. Gate đã chạy

### S2.0 — baseline trước khi sửa
- Snapshot `backup\S2PRE_20261020\` (source + exe + cfg + sha256).
- `build_fast`: exit 0, 0 warning.
- `-aimtest`: exit 0, 0 `FAIL`.
- `-srvtest 0`: PASS (network list FAIL là WARN khi offline).
- Hits 8 tab khớp `backup\s1_baseline_hits.txt`; cfg hash `34B76AE8...` không đổi.
- Lưu ý: lần chạy đầu tab3 có thể ra `hits=16` do frame cuối; chạy lại ổn định 17.

### S2.1 — `ntwrite.cpp`
- `build_fast`: 0/0.
- **Full `build.bat`**: exit 0, 0 error, 0 tool fail (bắt buộc vì `memory.h` ảnh hưởng ntcheck/wpmtest).
- `ext_ntcheck`: id=58, MATCH, exit 0.
- `-aimtest`: 0/0. Hits 8/8. Snapshot `backup\S2.1_ntwrite\`.

### S2.2 — `ovl_cfg`
- Move verbatim: 580 dòng block + 4 dòng separator; verifier so từng dòng với S2PRE, chỉ khác 10 chỗ bỏ `static` (8 symbol cfg + `cfgSrvLastJob/Place`).
- Key-set parser: **112 literal key**, không mất/không thêm.
- Default template: chạy S2PRE và bản mới trong thư mục trống → `overlay.cfg` **byte-identical**, 1.846 B. Hash phụ thuộc `srv.lastjob` của server đang chơi (S2.2 `D508…`; S2.5 `FF4E…`); gate là old==new tại cùng thời điểm.
- Round-trip: 110 dòng trước/sau; chỉ `menu.size` đổi `344,455 → 344,253` (auto-write sau 20 frame ổn định). `srv.lastjob` được pin để tránh server state ghi đè.
- Sentinel `toggle=H, aim.key=Q, aim.keymode=0, aim.mode=2, aim.rate=777, aim.smoothing=13, aim.pid=1, menu.tab=7, menu.pos=200,150`:
  - stdout: `H toggles hide/show`, `menu tab=7 pos=200,150 ... hits=32`.
  - `aim_diag.log`: `startup mode=2 key=81 keymode=0 rate=777 smoothing=13.00 pid=1`.
- `-aimtest` 0/0; hits 8/8. Snapshot `backup\S2.2_cfg\`.

### S2.3 — `ovl_server`
- Move verbatim 983 dòng (chỉ 4 chỗ bỏ `static`: `g_srvRun`, `srvWorkerThread`, `srvTick`, `runSrvSelfTest`).
- `-srvtest 0`: PASS cả 5 mục parser; mục `[6] network` list FAIL/geo OK là WARN phụ thuộc mạng.
- Tab 5 chạy `-diag -nohide -t 6`: exit 0, elapsed 8,27 s, không còn process, hits=23 size=344,455.
- `-aimtest` 0/0; hits 8/8. Snapshot `backup\S2.3_server\`.

### S2.4 — `ovl_world`
- Move verbatim 477 dòng (chỉ 2 chỗ bỏ `static`: `worldRestoreAll`, `worldTick`).
- Diag tạm: `world write path direct=1 id=58` (đã gỡ; string không còn trong exe).
- Tab 4: hits=25 size=344,455. `ext_ntcheck` id=58 MATCH.
- `-aimtest` 0/0; hits 8/8. Snapshot `backup\S2.4_world\`.

### S2.5 — final
- `build_fast`: exit 0, 0 warning, 0 error.
- Full `build.bat`: exit 0, 0 error, 0 tool fail.
- `-aimtest`: exit 0, 0 `FAIL`; `-srvtest 0`: PASS.
- Cfg re-check: key-set **112 same**; default old==new 1.846 B; round-trip 110 dòng chỉ `menu.size`; sentinel `H / Q / mode2 / 777 / 13.00 / pid1 / tab7 / pos200,150` đúng.
- Hits 8/8 (bảng dưới), cfg hash không đổi `34B76AE8…`.
- `.text`: `0x1C1800` (S2PRE) → `0x1C28D0` (S2POST), +4.304 B (chỉ tham khảo, không phải gate).

| Tab | Hits | Size |
|---|---:|---|
| 0 | 17 | 344,253 |
| 1 | 15 | 380,236 |
| 2 | 28 | 344,525 |
| 3 | 17 | 344,500 |
| 4 | 25 | 344,455 |
| 5 | 23 | 344,455 |
| 6 | 10 | 344,355 |
| 7 | 32 | 344,525 |

Tất cả `pos=928,173`, `exit=0`. Log final: `backup\s2_hits_final.txt`; các mốc trước: `s2_hits_after_s21/s22/s23/s24.txt`.

---

## 6. Full build S2.5

`build_log_s25_full.txt`: exit **0**, **0 error**, **0 tool fail** (25 tool standalone + overlay). `build_log_s25_fast.txt`: exit 0, 0 warning.

---

## 7. Gotchas / build debt

1. **Orphan tools**: `clockhold.cpp`, `fovpair.cpp`, `hbcheck.cpp`, `worldprobe.cpp` không nằm trong `build.bat`. Sau S2, build tay các tool này **phải thêm `src\ntwrite.cpp`**, nếu không link fail:
   ```
   "%GCC%" -std=c++17 -O2 -static -o ext_hbcheck.exe src\hbcheck.cpp src\offsets.cpp src\ntwrite.cpp -lwinhttp -lshell32 -lpsapi
   ```
   (libs tùy tool; clockhold/fovpair/hbcheck thường chỉ cần winhttp/shell32/psapi). Đây là debt đã biết, không chặn build chính.
2. **`ovl_ui.h` include `ovl_cfg.h`** thay vì sửa 8 file tab: giữ nguyên include của tab, `upsertCfgKey` vẫn nhìn thấy. Không tạo vòng include (`ovl_cfg.h` không include `ovl_ui.h`).
3. **`menu.size` auto-write** sau 20 frame ổn định là hành vi cũ, không phải cfg round-trip lỗi. Khi test, copy `overlay.cfg` mới cho từng tab: app có thể ghi `menu.pos` đã clamp (tab1 rộng 380 px → `892,173` nếu tái dùng cfg cũ).
4. **Tab3 first-run flake**: thỉnh thoảng `hits=16` ở lần chạy đầu; chạy lại 17. Không phải regression.
5. **`diagRate` không được để `static` lại**: server + world gọi qua `ovl_core.h`.
6. **`g_srvRun` là atomic**; `main` store false trước khi join `srvTh`. Đừng đổi sang bool thường.
7. **`srvValidJob`** de-static sớm ở S2.2 (vì `loadAimCfg` gọi), definition chuyển sang `ovl_server.cpp` ở S2.3; header `ovl_server.h` giữ prototype.
8. **Server self-test** phụ thuộc mạng: `[6] list` có thể FAIL khi offline, chỉ là WARN; `geo` cần HTTP.
9. **LF-only, no BOM** cho `src\overlay.cpp` và `src\ovl\*`. Các script Python ghi `newline="\n"`; đừng dùng editor tự CRLF.
10. **Git**: repo đã có baseline commit `6d2b161` (S1 final) + `.gitignore` (`*.exe`, `*.log`, `backup/`). Không dùng `git checkout` để khôi phục `overlay.cpp` (autocrlf có thể đổi LF→CRLF); rollback chính vẫn là `backup\`.

---

## 8. Sai lệch so với plan

1. `ovl_ui.h` **include `ovl_cfg.h`** thay vì xoá prototype rồi thêm include vào 8 file tab. Cùng kết quả, ít sửa hơn; không vòng include.
2. Move contiguous `52–175` và `740–1161` gồm cả dòng trống phân cách, nên tổng move là **580 dòng** (plan liệt kê 577 + 2 dòng `cfgSrvLastJob/Place` = 579; thêm 1 dòng trống line 66). Nội dung vẫn verbatim.
3. `ovl_core.h` chỉ tạo ở **S2.3** (plan ghi S2.2) để tránh de-static `diagRate` sớm khi cfg chưa cần.
4. `ovl_server.h` stub được tạo từ S2.2 (chỉ `srvValidJob`) vì `loadAimCfg` gọi hàm này; bản đầy đủ ở S2.3.
5. Diag `world-direct` là tạm theo yêu cầu, đã gỡ trước gate cuối.
6. Full `build.bat` chạy ở **S2.1** (yêu cầu bổ sung), không chỉ `build_fast`.

## 9. Checklist in-game (chưa tự động hoá)

- [ ] Tab 0: menu kéo/thu gọn, toggle ẩn/hiện, panic restore.
- [ ] Tab 1: aim key/RMB, FOV circle, smoothing, trigger, autoclick.
- [ ] Tab 2: ESP box/name/HP/skeleton/OOF, team/NPC filters.
- [ ] Tab 3: teleport, waypoint save/load/delete.
- [ ] Tab 4: lighting/fog/clock/FOV/zoom; untick và exit phải restore.
- [ ] Tab 5: server info, list (online), hop/join, watchdog, anti-AFK; `srv.geo` mặc định off.
- [ ] Tab 6/7: tools + misc, không crash khi bật/tắt.
- [ ] Thoát game/overlay: không còn process, cfg lưu đúng, lần sau load lại.

## 10. Rollback

| Snapshot | Nội dung |
|---|---|
| `backup\S2PRE_20261020\` | toàn bộ source + exe + cfg trước S2 |
| `backup\S2.1_ntwrite\` | memory.h, ntwrite.cpp, 2 build script |
| `backup\S2.2_cfg\` | overlay.cpp, ovl_cfg.*, ovl_server.h (stub), ovl_ui.*, build script |
| `backup\S2.3_server\` | overlay.cpp, ovl_server.*, ovl_core.h, build script |
| `backup\S2.4_world\` | overlay.cpp, ovl_world.*, build script |
| `backup\S2POST_20261020\` | toàn bộ source + exe + cfg sau S2 |

Khôi phục: copy file từ snapshot tương ứng đè lên `src\...`, build lại. Không cần `git checkout`.

---

## 11. Việc tiếp theo (S3)

- **Entity cache** (~306 dòng, các lambda `[&]` trong `refreshEnts` chia sẻ state snapshot với aim) — đã defer từ S2. Ranh giới phức tạp hơn 3 khối này; cần plan riêng.
- Sau S3, `overlay.cpp` mục tiêu còn ~3.700 dòng.
- Kiểm tra lại orphan-tool debt nếu muốn build `clockhold/fovpair/hbcheck/worldprobe`.
