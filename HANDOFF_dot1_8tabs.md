# HANDOFF — Đợt 1 (8 tab + ESP v2 + radar + profiles + waypoints)

Ngày: 2026-10-18 · File: `src/overlay.cpp` · exe: `ext_overlay.exe` (6.607.108 bytes, build 00:09)
Backup: `backup/overlay_DOT1_8TAB_20261018.{cpp,exe,cfg}` (bản 4 tab trước đó: `backup/overlay_TABS_version_20261017.*`)

---

## 1. Đợt 1 làm được gì

### A. Layout 8 tab (2 hàng × 4 ô) + khung tính năng mới
- Tab: `Body, Combat, Visual, Teleport | World, Server, Tools, Misc`. Dựng bằng `ImGui::Button` thường
  (không `BeginTabBar` vì hit-test `g_menuHits` chỉ đúng với rect widget); tab đang chọn tô xanh; persist `menu.tab` (clamp 0..7).
- `World/Server/Tools` hiện là placeholder có text mô tả (Đợt 2 / 4 / 6).
- Mỗi tab dài bọc trong `BeginChild` + `markChildArea()` để lăn chuột không xuyên xuống game.
- Gỡ hotkey `H` teleport (nút `TP` trong tab Teleport thay thế); `tp.key=H` còn trong cfg nhưng vô hại.
- Thêm: keybind editor (panel không có keyboard focus → capture bằng `GetAsyncKeyState` trong main loop, ESC = unbind),
  panic key (`DELETE`: tắt arm/hotkey + ẩn panel, **không ghi cfg**), stream proof `misc.wda` (WDA_EXCLUDEFROMCAPTURE),
  watermark HUD `hud.on/hud.feats`, auto-profile theo place, profiles `p1..p5` (copy byte `overlay.cfg`),
  waypoints `wp.<n>` trong cfg.

### B. Visual: ESP v2 + radar (đọc-only, không ghi memory)
- Master `esp.on` + từng element: `box, fill, name, dist, hp, head, skel, line, oof, npc` + `esp.maxdist` + 3 màu `esp.colenemy/colteam/colnpc` (int 0xRRGGBB).
- Skeleton (head + `limbPrim[4]`), HP bar, head dot, snapline, off-screen arrow, NPC riêng màu, team = màu team.
- Radar góc trên-phải: `radar.on/range/size`, "self" ở tâm, dùng cột ma trận camera (right=`m[0],m[6]`, look=`-(m[2],m[8])`) nên không cần yaw.
- HUD/ESP/radar vẽ bằng draw list (không phải ImGui window) → click vẫn xuyên xuống game.
- Nút màu: **click để cycle palette 11 màu** (không dùng ColorEdit3 popup — xem §4).

### C. Teleport: waypoints
- `save waypoint` (lấy vị trí self từ memory) · `clear all` · list `TP / X` (mỗi dòng tên + toạ độ), tối đa 64.
- Cfg: `wp.<n>=<name>|<x>,<y>,<z>`; `saveWaypoints()` ghi lại file, bỏ mọi dòng `wp.`.
- Nút `TP` chỉ set request (`g_tpTargetPos`, `g_tpPosTick`, `g_tpPosName`, hết hạn 2000ms); write nằm trong loop (khối "Teleport to a saved waypoint").

---

## 2. Build

```
Stop-Process -Name ext_overlay -Force
g++ -std=c++17 -O2 -g -static -o ext_overlay.exe src\overlay.cpp src\offsets.cpp lib\libimgui.a ^
  -Ithirdparty/imgui-master -Ithirdparty/imgui-master/backends -Isrc ^
  -lwinhttp -lshell32 -lpsapi -ld3d11 -ld3dcompiler -ldwmapi -lgdi32 -lwinmm
```
(g++: `C:\Users\adminn\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe`)

Chạy: `ext_overlay.exe` (thêm `-nohide -diag` khi test). `INSERT` mở/ẩn panel · `DELETE` panic.

---

## 3. Đã verify (bằng click thật + ảnh + log)

| Hạng mục | Kết quả |
|---|---|
| 8 tab click | `tab click -> 0 (Body)` … `-> 7 (Misc)` ✔ |
| Keybind capture | `bind start aim.key` → F10 → `bind aim.key = 0x79 (F10)` + `overlay.cfg aim.key=F10` ✔ |
| ESC trên nút bind | `panic.key=NONE` ✔ |
| Panic | ẩn panel (`menu tab=` ngừng in), `menu.show=1` **không** bị ghi; INSERT mở lại ✔ |
| Profiles | save `profiles\p1.cfg` (1312 bytes) → toggle `misc.wda` 1→0 → load về 1 ✔ |
| Hit map Visual tab | [10] ESP on, [11] box, [12] fill, [13] name, [14] distance, [15] hp, [16] head, [17] skeleton, [18] snapline, [19] offscr, [20] NPCs, [21] max dist, [22..24] 3 màu, [25] radar, [26] range, [27] size — click ghi cfg đúng ✔ |
| Nút màu | cycle đủ 11 màu, mỗi click 1 bước, ghi cfg đúng ✔ |
| Waypoints | save 3 → `wp.0=wp01|0,5,0 …`; `X` xoá + đánh số lại; `clear all` → 0 dòng; TP → log `waypoint tp wp03 dist=0.0` ✔ |
| Radar vẽ thật | đo pixel: crosshair `(131,131,132)`, border `(65,66,67)`, fill `(6,7,9)`, control `(20,20,20)` ✔ |
| Watermark | `on: aim(mem)` + `place 905X7556345396 | mem ON` hiện trên màn hình ✔ |
| Diag ESP | `vm=1` (view matrix hợp lệ) → nhánh ESP/radar chạy ✔ |

**Chưa verify được:** box / fill / hp / name / skeleton / snapline / off-screen / chấm radar **theo từng player** —
game hiện chỉ có 1 entity (self, bị skip theo thiết kế, giống velext `showSelf=0`) nên không có gì để vẽ.
Cần test ở place có ≥2 người (account phụ + place riêng như đã thống nhất).

---

## 4. Gotcha quan trọng (đừng để mất)

1. **Color key = RGB(0,0,0).** Overlay là layered window `LWA_COLORKEY` màu đen tuyền: mọi pixel **đúng (0,0,0)**
   vừa *trong suốt* vừa **xuyên click ở tầng OS** (click không tới cửa sổ mình, không tới ImGui).
   → Trong menu **tuyệt đối không vẽ đen tuyền** (chữ/icon/ký hiệu). Chữ trên nền sáng phải dùng xám `(0.06,0.06,0.06)`.
   Triệu chứng đã gặp: click vào đúng hàng pixel chữ đen của nút màu thì "không ăn" (y=500), các y khác vẫn ăn.
   Theme dark mặc định của ImGui không vẽ đen tuyền (WindowBg 0.06→15, TitleBg 0.04→10) nên vẫn OK.
   Hệ quả phụ (chấp nhận được): bóng đen alpha của HUD/ESP bị key → ESP không bao giờ nuốt click.
2. **Panel không bao giờ được focus** → backend `imgui_impl_win32` bắn `WM_MOUSELEAVE` liên tục (do game clip cursor)
   rồi để `MousePos = -FLT_MAX`. Không có MousePos thì ImGui không có `HoveredWindow` ⇒ click **không kèm mousemove**
   bị nuốt. Đã fix: tự feed `io.AddMousePosEvent(GetCursorPos + ScreenToClient)` mỗi frame trước `NewFrame()`.
   (Test tổng hợp bằng `SetCursorPos` cùng toạ độ là dính bug này — click thật thì ít gặp nhưng vẫn fix.)
3. `hitMark()` **phải clip theo `GetClipRectMin/Max`**: widget bị cuộn ra ngoài child vẫn có rect; không clip thì
   click vào vùng không thấy gì sẽ bị overlay nuốt.
4. `hitMark()` sau `ColorEdit3` trả về **bounding box của group** (ô màu + label), không phải rect ô màu (ô màu chỉ rộng `GetFrameHeight()`, 19px).
5. **Popup (color picker / combo / context menu) không tự đóng** trong cửa sổ này: click ra ngoài rơi xuống game.
   Đã có sẵn code mark rect popup + rect fullscreen nuốt click sau `ImGui::End()`, nhưng chưa verify được đường "click ngoài để đóng"
   → **đừng dựng feature nào phụ thuộc popup** cho tới khi test lại. (Vì vậy màu dùng nút cycle thay vì picker.)
6. Test click bằng script: **không** gọi `SetProcessDPIAware` (overlay là DPI-unaware, hệ logical 1280x800);
   ảnh chụp thì ngược lại — dùng session riêng **có** DPI-aware. Toạ độ lấy từ `-diag` hit rects, không tính từ ảnh.
7. `misc.wda=1` (stream proof) làm ảnh chụp **không** thấy overlay → set 0 trước khi chụp để kiểm chứng.

---

## 5. Wart / TODO đã biết

- Checkbox `Hitbox` (tab Combat) ghi `hitbox.enabled` vào `overlay.cfg` nhưng loader hitbox đọc `profiles/<placeId>.cfg`
  ⇒ tick trong menu **không** có tác dụng và không được load lại. `hitbox.enabled=1` đang nằm trong cfg là khoá chết
  (không có file `profiles/<placeId>.cfg` ⇒ hitbox thực tế **đang tắt**). Cần sửa cho ghi đúng file profile khi bạn muốn dùng hitbox.
- `profiles/p1.cfg` là rác từ test profile — xoá được.
- `g_popupHits` + code mark popup hiện là dead code (giữ lại cho tương lai).
- Diag hiện in khá nhiều (`menu tab=… col=… ents=… vm=… vp=…` + `mouse DOWN/UP`), chỉ bật khi có `-diag`.

---

## 6. Bước tiếp theo

1. **Test ESP có người**: account phụ + place riêng, `INSERT` → Visual → tick `ESP on` (các element đã bật sẵn: box, name, dist, hp, head dot, skeleton, snapline, off-screen, NPCs, max dist 600, radar bật).
   Riêng lẻ từng element để chắc chắn từng cái vẽ đúng; kiểm luôn màu team vs enemy.
2. Đợt 2 (World): fullbright, anti-fog, no shadows, water transparency, FOV/zoom.
3. Đợt 4 (Server): jobId/server IP/region/ping, server hop, rejoin, anti-AFK.
4. Đợt 6 (Tools): instance explorer, saveinstance-lite, memory scanner, macro recorder.
5. Cân nhắc: popup dismissal (nếu cần combo/context menu), hitbox checkbox → profile đúng file.
