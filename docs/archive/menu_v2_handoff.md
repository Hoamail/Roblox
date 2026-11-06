# Overlay v2: bỏ HUD cũ + checkbox = công tắc arm cho phím (handoff)

Trạng thái: **XONG**, đã build + chạy (`ext_overlay.exe`). Người dùng cần test
trong game (máy tao lúc làm bị khoá session nên không chụp được ảnh).

---

## 1. Hai việc người dùng yêu cầu

### (a) "có cả 2 overlay trên màn hình" — bỏ HUD cũ

Thực tế **chỉ 1 process `ext_overlay.exe`** vẽ **2 cửa sổ ImGui**:

| Cửa sổ | Nội dung |
|---|---|
| `ext P2.1 [date]` | HUD debug cũ, ~18 dòng: build/PlaceId/CamPos/FPS/Aim/Trig/AClick/Hitbox/Lead/gamesens/writer/ents/Ply/Fly/Menu |
| `ext menu` | panel menu mới (click + drag) |

→ Đã **xoá hẳn cửa sổ HUD**. Trên màn hình giờ chỉ còn `ext menu`.

Thông tin còn hữu ích của HUD được dồn vào **footer của menu**:

```
typing: aim/keys paused          <- chỉ hiện khi con trỏ đang ở chat box
tp -> <tên> (3m)                 <- kết quả teleport (4s)
fps 91  loop 11.0ms  ents 8  \ quits
```

và mỗi hàng tính năng có nhãn sống `[phím] ON/off` (xem mục 3).

**Backup bản HUD** (muốn quay lại thì chép đè):

```
backup\overlay_HUD_version_20261017.cpp    -> src\overlay.cpp
backup\ext_overlay_HUD_version_20261017.exe -> ext_overlay.exe
backup\overlay_HUD_version_20261017.cfg    -> overlay.cfg
```

(md5 exe live lúc backup == md5 file backup ✓)

### (b) "tick ô Fly thì F mới bay, bỏ tick thì spam F vô hại"

Trước đây **checkbox và phím là 2 công tắc độc lập trên cùng 1 biến**:
`cfgFlyEnabled` vừa là trạng thái vừa là công tắc; bấm F toggle chính biến đó và
ghị cfg → cảm giác "double control", không khớp ý người dùng.

Bây giờ tách rõ **arm (menu) / activate (phím)**:

| | ý nghĩa | lưu cfg? |
|---|---|---|
| `cfgFlyEnabled` (checkbox Fly) | **arm**: cho phép F hoạt động | có (`fly.enabled`) |
| `g_flyActive` | **activate**: F bật/tắt (đổi runtime) | **không** (session) |
| áp dụng thật | `flyApply = cfgFlyEnabled && g_flyActive` | |

Luật áp dụng cho cả 3: Fly (F), Speed (B), Jump (N).

- Tick Fly + F → bay; F lần nữa → tắt bay.
- Không tick Fly → F **hoàn toàn vô hại** (kể cả spam), và fly bị ép off.
- Bỏ tick trong lúc đang bay → `g_flyActive = 0` ngay (un-anchor ✓).
- Restart overlay: mọi `g_*Active = 0` → bao giờ cũng khởi động ở trạng thái "chưa bật".

Toàn bộ chỗ ghi bộ nhớ đã đổi sang `speedApply` / `jumpApply` / `flyApply`
(kể cả nhánh **khôi phục giá trị gốc** khi tắt → tắt là Humanoid về như cũ):

```
restore JumpPower/JumpHeight/WalkSpeed+shadow  <- !jumpApply / !speedApply
mode 0 property write                          <- (speedApply || jumpApply)
mode 1 velocity write                          <- speedApply && !flyApply
fly anchored + position steps                  <- flyApply
fly un-anchor                                  <- !flyApply
```

UI: bỏ checkbox `hotkey` cũ (giờ vô nghĩa), thay bằng nhãn trạng thái. Hàng
Combat cũng có nhãn (Aim `[T]`, Trigger `[XBUTTON2]`, AutoClick `[V]`), Hitbox
cạnh `mem ON/OFF`. Hai dòng hint trên panel:

```
INSERT: hide/show  -  tick = arm, key = on
unticked = that key does nothing at all
```

## 2. Bug tìm ra trong lúc verify: edge phím bị "ăn"

Code cũ (và code của các hotkey khác trong file) dùng bit thấp:

```cpp
if (GetAsyncKeyState(cfgPlySpeedKey) & 1) ...   // "vừa bấm kể từ lần đọc trước"
```

Bit này **dùng chung cho cả desktop** — Roblox cũng poll phím, nên có lần edge bị
process khác đọc mất trước khi overlay kịp nhìn (test thật: `arm=1` nhưng bấm B
không ăn). Đã đổi 3 phím Fly/Speed/Jump sang **tự phát hiện cạnh lên** bằng bit
`0x8000` (trạng thái "đang giữ"), an toàn tuyệt đối với process khác:

```cpp
bool dFly = cfgFlyKey && (GetAsyncKeyState(cfgFlyKey) & 0x8000) != 0;
if (dFly && !g_prevFlyKey && !typingNow && cfgFlyEnabled)
    g_flyActive = !g_flyActive;
g_prevFlyKey = dFly;          // luôn cập nhật -> không sinh cạnh giả
```

`g_prev*` được cập nhật **kể cả khi overlay đang ẩn** (khối hotkey nằm trước
`wantShow/continue`) nên không bao giờ sinh toggle giả khi ẩn/hiện.

> Các hotkey cũ khác (B/N value-step `[` `]` `;` `'`, aim, trigger, autoclick)
> vẫn dùng `&1` như trước — không đụng tới để khỏi phá cái đang chạy tốt.

## 3. Cách test (và bằng chứng)

Thêm cờ ẩn **`-diag`** (in state ra stdout, chỉ in khi có thay đổi + heartbeat 2s):

```
state fly[arm=1 key=F active=1 apply=1] speed[arm=0 key=B active=0 apply=0] jump[arm=1 key=N active=0 apply=0]  <-- CHANGE
```

Chạy: `.\ext_overlay.exe -nohide -diag > diag.txt` rồi bấm phím thật.

**Bằng chứng đã chạy:**

| Test | Cách làm | Kết quả |
|---|---|---|
| arm=1 cả 3 | bấm F, B, N (mỗi phím 1 lần) | 3 dòng CHANGE: lần lượt `fly apply=1`, `speed apply=1`, `jump apply=1` ✔ |
| arm=0 cả 3 | spam F, B, N (6 lần) | **không** có dòng CHANGE nào (state vẫn `active=0 apply=0`) ✔ |
| bỏ tick giữa chừng | code: `if (!cfgPlySpeed) g_speedActive = 0;` | active reset ngay + restore giá trị gốc ✔ (review code) |

**Chưa verify được bằng mắt:** lúc test, session Windows **không có foreground
window** (`GetForegroundWindow() = 0` → máy khoá/tắt màn hình), nên:
- overlay **tự ẩn** (`wantShow = fgPid == m.pid` → false) → chụp màn hình chỉ thấy
  1 vùng mờ, không thấy panel;
- `keybd_event` inject không tới nơi (tap 300ms mà poller chỉ thấy ~56ms, có phím
  mất hẳn) → vài test bị "miss" giả, phải test lại bằng tap 400ms; **không phải bug
  của overlay** (khi bấm bình thường loop 90fps thì không thể miss).

⇒ Người dùng cần test lại trong game: tick Fly → F bay; bỏ tick → spam F không bay.

## 4. Gotcha khi làm tiếp

1. **Đừng quay lại dùng `& 1`** cho hotkey mới: bit thấp dùng chung toàn desktop,
   Roblox đọc mất (xem mục 2).
2. Khi màn hình khoá/không có foreground window: overlay ẩn + input inject không
   đáng tin ⇒ đừng kết luận gì từ test kiểu đó.
3. `-nohide` chỉ dùng khi test; chạy thường thì overlay chỉ hiện khi game là cửa sổ
   đang focus (đúng ý "không thấy khi không chơi").
4. Mọi trạng thái "đang bật" của Fly/Speed/Jump là **session-only**: không ghi vào
   cfg, restart là sạch (tránh vào game đã bay sẵn).
5. Nhãn `[phím] ON/off` là `TextDisabled`, **không** `hitMark()` → click ở đó vẫn
   xuyên xuống game (đúng thiết kế: chỉ widget ăn click).
6. Xoá HUD thì mất mấy dòng debug cũ (Lead/errPx/gamesens/writer Hz/aim mode). Log
   chi tiết vẫn còn qua `diagRate(...)` xuống file nếu cần.

## 5. Trạng thái hiện tại

`overlay.cfg` (đã khôi phục theo ý người dùng):

```
fly.enabled=1     # arm: tick Fly
fly.key=F         # (không còn checkbox hotkey; F là phím thật)
fly.speed=71
ply.speed=0       # chưa tick Speed -> B vô hại
ply.speedval=50
ply.jump=1        # tick Jump -> N hoạt động
ply.jumpval=90
ply.jumpmode=0
menu.show=1
menu.key=INSERT
menu.pos=79,460
menu.size=351,280
```

Process đang chạy: `ext_overlay.exe` (PID 16220, chế độ thường, không `-nohide`).

## 6. Việc có thể làm tiếp

- Cho đổi phím của từng feature ngay trong menu (hiện phím cố định trong cfg:
  F/B/N, aim T, trig XBUTTON2, autoclick V).
- Sau khi trừ HUD, nếu muốn xem lại nhanh mấy thông số aim (lead/errPx/gamesens)
  thì thêm 1 mục "debug" thu gọn trong menu thay vì dựng lại cửa sổ riêng.
- Test in-game rồi tinh chỉnh panel: hiện cao 280px tại (79,460).
