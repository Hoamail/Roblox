# HANDOFF — Thu gọn panel bằng tam giác (bỏ INSERT) — 2026-10-20

Trạng thái: **code xong, build sạch (1:22:29), self-test offline PASS, cold-start verify PASS**.
Việc cần **user click thật trong game** (môi trường test không bấm được nút — xem mục 4).

Rollback / mốc:
- `backup\overlay_COLLAPSE_final_20261020.cpp` + `backup\ext_overlay_COLLAPSE_final_20261020.exe`
  (7,774,956 bytes — bản final: collapse + Server tab + mọi thứ, build 1:22:29).
- `backup\overlay_MENUCOLLAPSE_20261020.cpp` + `backup\ext_overlay_MENUCOLLAPSE_20261020.exe`
  (6,884,497 bytes — **trước** khi làm menu-collapse; = bản World era, chưa có Server tab).

---

## 1. Yêu cầu → kết quả

| Yêu cầu | Kết quả |
|---|---|
| Bỏ hẳn INSERT (toggle + hint + keybind) | ✅ xoá code toggle; parser bỏ qua `menu.show`/`menu.key` (di sản, không lỗi); dòng hint ở tab Body xoá |
| Tam giác collapse/uncollapse bấm được trên title bar | ✅ dùng collapse built-in của ImGui (`NoCollapse` đã bỏ, `p_open=nullptr` — không còn nút X); ▼ góc trái title bar, bấm đúp title bar cũng được |
| Giữ K (ẩn/hiện toàn bộ overlay) | ✅ `toggle=K` không đổi |

## 2. Hành vi mới (chi tiết)

- Panel **luôn tồn tại** (không còn trạng thái "đóng hoàn toàn").
- **Thu gọn** = title bar thuần (pill `▼ ext menu`), min-width **250px** (dễ thấy/dễ bấm —
  không bị teo còn ~16px). Thu gọn xong: không còn hit-rect nào ngoài title (không chặn
  click vào game — verify `hits=1`).
- Nhớ trạng thái: key mới **`menu.collapsed`** (0/1) trong `overlay.cfg`, ghi đúng frame
  khi đổi. Lần đầu trên cfg cũ (chưa có key) → mặc định **0 = mở rộng**.
- **Panic** (`panic.key` = DELETE) giờ thu gọn panel thay vì ẩn kiểu cũ; trạng thái đó
  được ghi vào `menu.collapsed` luôn (qua cơ chế sync).
- `mouseLockUpdate` gọi với `cfgMenuCollapsed == 0` (mở = thả chuột như trước).
- Guard: `menu.size` chỉ lưu khi `g_menuH > 60` → pill ~19px không bao giờ ghi đè size đã lưu.
- Khi thu gọn, phần nội dung (tabs + hit-mark) bị skip (`IsWindowCollapsed()` guard) →
  không sinh rect "ma" chặn click game.

## 3. Điểm code (src\overlay.cpp, bản 1:22)

| Vị trí | Nội dung |
|---|---|
| ~809 | `static int cfgMenuCollapsed = 0;` |
| ~1055 | parse `menu.collapsed` (comment di sản `menu.show`/`menu.key` bỏ qua) |
| ~4346 | panic: `cfgMenuCollapsed = 1;` (chỉ đổi biến; sync ghi cfg frame sau) |
| ~5134 | `mouseLockUpdate(cfgMenuCollapsed == 0);` |
| ~5155 | size constraints **luôn áp** (min 250×0, max 380×∞) — cả lúc thu gọn để pill ≥250px |
| ~5165 | `collapseApplied` — `SetNextWindowCollapsed(cfgMenuCollapsed, Always)` **một lần** mỗi process; sau đó ImGui tự quản |
| ~5171 | `Begin("ext menu", nullptr, AlwaysAutoResize)` — `p_open=nullptr` (không nút X), không còn `NoCollapse` |
| ~5182 | sync: `IsWindowCollapsed() != cfgMenuCollapsed` → cập nhật biến + `upsertCfgKey("menu.collapsed", …)` |
| ~5246 | `if (!ImGui::IsWindowCollapsed())` — toàn bộ nội dung nằm trong guard |

## 4. Verification đã chạy

- `build_fast.bat` → `build overlay exit: 0`; exe `7,774,956` bytes @ 1:22:29.
- `ext_overlay.exe -aimtest` → exit 0, `[5] all mapping cases PASS` (không regression logic aim).
- Sandbox cold-start (`%TEMP%\opencode\worldtest`, `-nohide -diag`, không cần game):
  - `menu.collapsed=1` → `size=250,19 hits=1 collapsed=1`, cfg `menu.size=344,253` **giữ nguyên** ✅
  - `menu.collapsed=0` → `size=344,253 hits=17 collapsed=0` ✅
- **Không verify được bằng click inject**: phiên này click chuột tiêm từ PowerShell không
  kích được nút do **DPI-virtualization** (toạ độ set ≠ toạ độ process thấy). Đây là hạn chế
  của *môi trường test*, không phải lỗi code: feed chuột của overlay dùng
  `GetCursorPos + ScreenToClient` y như mọi widget khác đang hoạt động hằng ngày
  (user đã drag panel + bấm tab bình thường). **Việc cần user: bấm thử ▼ trong game.**

## 5. Checklist user (trong game, foreground)

1. Mở `ext_overlay.exe` (bản mới nhất). Panel hiện **mở rộng** (vì cfg cũ chưa có
   `menu.collapsed`), đúng vị trí đã lưu (`menu.pos` cũ vẫn dùng).
2. Bấm **▼** bên trái chữ "ext menu" → panel thu còn pill `▼ ext menu` (rộng ~250px).
3. Bấm **▼** trên pill → mở rộng lại. (Bấm đúp vào title bar cũng đổi được.)
4. Thoát overlay (`\`) rồi mở lại → trạng thái thu/mở **giữ nguyên** như lúc thoát.
5. **K** vẫn ẩn/hiện toàn bộ overlay như trước.
6. **DELETE** (panic) → panel thu gọn.
7. Nếu có gì sai (bấm ▼ không ăn / panel không hiện / click xuyên lung tung) → báo lại
   kèm tình trạng panel + `overlay.cfg`.

## 6. Ghi chú phiên (quan trọng cho mốc sau)

- Trong lúc làm menu-collapse, **một session khác đang build tab Server song song** vào
  cùng `src\overlay.cpp` (nguồn +~51KB, exe họ build 1:19). Bản final 1:22:29 **bao gồm cả
  Server tab lẫn menu-collapse** — không nhầm lẫn khi rollback.
- `menu.show`/`menu.key` trong `overlay.cfg` của user: giữ nguyên dòng cũng **vô hại**
  (bị bỏ qua); không cần xoá tay.
- Mọi comment/khảo sát về "NoCollapse + X button" (nếu còn trong doc khác) đã lỗi thời —
  bản này bỏ hẳn nút X, dùng triangle thu gọn.
