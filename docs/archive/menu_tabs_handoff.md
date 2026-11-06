# Handoff: menu chia tab + tab Teleport (2026-10-17)

Tiếp theo `menu_v2_handoff.md` (bỏ HUD, arm/key). File này ghi lại phần **chia tab**,
**tab Teleport danh sách người chơi**, **bỏ hotkey H**, và các bug phát hiện khi test.

## 1. Việc đã làm

### 1.1 Chia menu thành 4 tab
- Tab dựng bằng **nút thường** (`ImGui::Button`) chứ không dùng `ImGui::BeginTabBar`:
  hệ thống hit-test của overlay (`g_menuHits` + `hitMark()`) chỉ đúng khi rect widget
  biết chắc chắn. Tab bar của ImGui + auto-resize cho rect lệch, còn nút thì y như
  các nút `impl:` sẵn có.
- 4 tab: **Body** (Fly / Speed / Jump + impl + trạng thái), **Combat** (Aim / Trigger /
  AutoClick / Hitbox / mem), **Teleport** (danh sách người chơi), **Misc** (hint, build,
  place, fps, loop, ents, reset panel position, unlock mouse).
- Tab đang chọn được tô xanh (`ImGuiCol_Button` sáng) và **lưu vào cfg**: `menu.tab=0..3`
  → mở lại đúng tab cũ.
- Nhãn tab bấm được kiểm chứng: click `Combat→1`, `Teleport→2`, `Misc→3`, `Body→0`
  (đọc `menu.tab` trong `overlay.cfg` làm bằng chứng, xem `t_*.png`).

### 1.2 Tab Teleport = danh sách người chơi + nút TP từng người
- Liệt kê mọi `Ent` là player (`e.player`) — thêm checkbox **NPCs** (`tp.npc=0/1`) để
  hiện cả mob/NPC.
- Mỗi dòng: nút `TP` (36px) + tên + khoảng cách (`m`) + `hp` (hoặc `dead` khi hp<=0).
- **Sắp xếp gần → xa**, dòng của mình luôn đầu (nút `you`, bị disable, không TP được).
- Bấm `TP` chỉ **đặt yêu cầu** (`g_tpTargetPrim = e.prim`, `g_tpRequestTick = nowLoop`);
  việc ghi bộ nhớ vẫn nằm ở khối teleport trong loop (chỗ hotkey H cũ), nên menu không
  bao giờ ghi bộ nhớ trực tiếp khi vẽ. Yêu cầu hết hạn sau 2000ms (entity despawn).
- Kết quả hiện ở footer menu: `tp -> <tên> (12m)` / `tp: target is gone` / `tp: write failed`.
- **Lưu ý khi test**: nếu game đang ở màn loading thì mọi toạ độ bằng nhau → cột khoảng
  cách hiện `0m` cho tất cả (đã gặp, không phải bug). Vào map rồi thì ra số thật:
  `self=-234,8,-597`, `pLaYeR671 99m`, `wyatt 341m`, `Pole 386m`.
- Danh sách nằm trong child 324x200 có **mark nguyên vùng** vào `g_menuHits`: nếu không
  mark, con lăn chuột ở vùng trống sẽ xuyên xuống game (zoom camera) thay vì scroll list.

### 1.3 Bỏ hotkey H (teleport tới người gần nhất)
- Xoá khối `if (GetAsyncKeyState(cfgTpKey) & 1)` + biến `cfgTpKey` + key cfg `tp.key`.
  Parser bỏ qua dòng `tp.key=H` cũ trong cfg (vô hại).
- Giữ nguyên phần logic ghi vị trí (dịch `+3.0` Y, zero velocity) — chỉ đổi nguồn target
  từ "gần nhất" sang `g_tpTargetPrim`.

### 1.4 Bug: panel tự collapse + `menu.size` bị ghi rác
- **Hiện tượng**: panel tự nhiên co lại còn mỗi title bar (`size=344,19`, `collapsed=1`),
  và `menu.size` bị ghi thành `344,19`; lần sau mở panel có thể bị clamp sai.
- **Nguyên nhân**: ImGui **double-click vào title bar = collapse** (imgui.cpp dòng 7887).
  Panel nằm giữa màn hình game, người chơi double-click trong game rất dễ trúng title bar.
  Cộng thêm frame đầu tiên của cửa sổ auto-resize báo size tạm `250x35` → code cũ ghi
  ngay size đó vào cfg.
- **Fix**:
  1. `ImGuiWindowFlags_NoCollapse` cho panel (không thể collapse nữa, kể cả double-click).
  2. Chỉ ghi `menu.size` khi size đã **đứng yên 20 frame** và `h > 60`
     (bỏ qua size tạm của frame đầu). Kết quả: `size=344,230`/`344,308`/`344,173` ổn định
     theo tab, không còn 19/35.
- Còn 1 file rác `imgui.ini` trong project (section `[Window][ext menu] Collapsed=1` do
  build cũ ghi). Overlay đã đặt `io.IniFilename = nullptr` ngay sau `CreateContext()`
  nên **không đọc/ghi** file này nữa; cứ để nguyên cũng được, xoá cũng được.

### 1.5 Mới: mở khoá chuột khi menu hiện (`menu.unlockmouse=1`)
- **Vấn đề gốc**: khi đang chơi, Roblox ghim con trỏ vào **1x1 px giữa màn hình**
  (`ClipCursor`) ⇒ `SetCursorPos` cũng không nhúc nhích ⇒ chuột **không bao giờ tới được
  panel** ⇒ click vào panel không ăn (đây là lý do nhiều lần test click của tôi thất bại
  dù toạ độ đúng).
- **Giải pháp**: `ClipCursor(NULL)` từ process mình giải phóng được (clip là tài nguyên
  dùng chung). Trong lúc panel đang hiện, overlay gọi `ClipCursor(NULL)` mỗi frame; khi
  panel ẩn (INSERT, X, hoặc overlay tự ẩn vì mất focus) thì **trả lại đúng box cũ** của game.
- Game **không tự giành lại** clip (đã đo: 4s sau khi nhả vẫn tự do) nên phải tự restore,
  kể cả lúc thoát overlay (đã thêm `mouseClipRestore()` ở cả 2 nhánh thoát + nhánh ẩn).
- Có checkbox ở tab **Misc**: `[x] unlock mouse (game pins it while playing)`.
  Tắt đi thì quay về hành vi cũ: chỉ click được khi game đang không khoá chuột (ví dụ lúc
  ở màn loading / mở menu Roblox).
- **Đánh đổi cần biết**: lúc panel hiện, chuột tự do nên **camera vẫn quay theo chuột**
  (Roblox đọc raw input). Muốn hết thì tắt checkbox và chấp nhận phải Esc/unlock trước
  khi click panel.

## 2. Trạng thái / cách chạy
- Đang chạy `ext_overlay.exe` chế độ thường (không `-nohide`, không `-diag`).
- Build (PowerShell, phải kill process trước):
  ```powershell
  Stop-Process -Name ext_overlay -Force
  g++ -std=c++17 -O2 -g -static -o ext_overlay.exe src\overlay.cpp src\offsets.cpp `
      lib\libimgui.a -Ithirdparty/imgui-master -Ithirdparty/imgui-master/backends -Isrc `
      -lwinhttp -lshell32 -lpsapi -ld3d11 -ld3dcompiler -ldwmapi -lgdi32 -lwinmm
  ```
  (g++ tại `C:\Users\adminn\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe`)
- Build exit 0, exe 6.141.790 bytes.

## 3. Key cfg mới
| key | ý nghĩa |
| --- | --- |
| `menu.tab` | tab đang mở (0 Body, 1 Combat, 2 Teleport, 3 Misc) |
| `menu.unlockmouse` | 1 = nhả khoá chuột khi panel hiện (mặc định 1) |
| `tp.npc` | 1 = liệt kê cả NPC/mob trong tab Teleport |
| `tp.key` | **bỏ** (dòng cũ còn trong cfg vô hại) |

## 4. Cách test (dành cho user)
1. Vào game (đang chơi bình thường, chuột bị khoá) → bấm `INSERT`.
2. Đưa chuột vào panel: **giờ phải đi được** (nhờ unlock mouse). Bấm 4 tab, kéo slider,
   tick/bỏ tick như cũ.
3. Tab **Teleport**: `TP` cạnh tên người chơi → nhân vật nhảy tới người đó, footer hiện
   `tp -> <tên> (xx m)`. (Tôi **không** bấm thử TP để khỏi dịch chuyển nhân vật của bạn —
   bạn tự test bước này.)
4. Bấm `INSERT` lần nữa: panel ẩn, chuột bị ghim lại như game muốn.
5. Nếu thấy camera quay khi đang mở panel mà không thích → Misc → bỏ tick `unlock mouse`.

## 5. Chứng cứ test
- Click tab: `Combat→1, Teleport→2, Misc→3, Body→0` (đọc `menu.tab` sau từng click).
- `clip HOLD: saved=749,347-750,348` (khoá chuột của game bị giữ tạm) và
  `clip RESTORE: now=749,347-750,348` (trả lại đúng box khi ẩn panel).
- Khoảng cách thật trong tab Teleport: `pLaYeR671 99m`, `wyatt_worldhd 341m`, `Pole 386m`.
- Ảnh: `t_body.png`, `t_combat.png`, `t_tp.png`, `t_misc.png`.
- Log: `overlay_out.txt` (chạy với `-diag`, gồm `menu tab=...`, `clip ...`, `tpdiag ...`).

## 6. Backup
- `backup/overlay_TABS_version_20261017.cpp` / `ext_overlay_TABS_version_20261017.exe` /
  `.cfg` — bản có tab (bản này).
- `backup/overlay_HUD_version_20261017.*` — bản còn cửa sổ HUD cũ (chép đè `src\overlay.cpp`
  để quay lại).
