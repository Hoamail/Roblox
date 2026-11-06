# Menu click được + draggable cho ext_overlay (handoff)

Trạng thái: **XONG** — menu ImGui trong overlay, click được (checkbox/slider/nút),
kéo được bằng title bar, lưu vị trí/kích thước vào `overlay.cfg`, và **không ăn
click của game** ngoài đúng vùng widget.

Người dùng xác nhận: *"drag title và click và kéo thanh trượt hoạt động rồi"*.

---

## 1. Yêu cầu

- Trong game muốn tắt Fly thì **click vào Fly** trong menu (thay vì chỉ dùng hotkey
  F, dễ bấm nhầm).
- Menu phải **draggable**.

## 2. Cách dùng

| Thao tác | Kết quả |
|---|---|
| `INSERT` | ẩn/hiện menu (đổi được bằng `menu.key=`) |
| Click checkbox | bật/tắt feature, ghi ngay vào `overlay.cfg` |
| Kéo slider | đổi giá trị; ghi cfg khi **nhả chuột** |
| Kéo **title bar** (`ext menu`) | di chuyển menu; ghi `menu.pos` khi nhả |
| Nút `X` trên title bar | ẩn menu (lưu `menu.show=0`, mở lại bằng `INSERT`) |
| Checkbox `hotkey` cạnh Fly | bật/tắt hotkey `F` cho Fly (tắt ⇒ chỉ điều khiển bằng menu) |
| Nút `impl: ...` | click để xoay vòng chế độ (property/velocity, auto/power/velocity) |

Menu hiện có: **Fly**(+tốc độ bay, hotkey), **Speed**(+giá trị, impl), **Jump**
(+giá trị, impl, đọc live power/height/UseJumpPower), **Aim**(+fov), **Trigger**,
**AutoClick**, **Hitbox**, và `mem ON/OFF`.

## 3. Cơ chế (phần khó nhất)

### 3.1 Vì sao trước đây menu không click được

Cửa sổ overlay tạo bằng `WS_EX_TOPMOST|LAYERED|TOOLWINDOW|NOACTIVATE` +
`SetLayeredWindowAttributes(LWA_COLORKEY, đen)`: pixel **đen** thì vô hình *và*
xuyên click, pixel được vẽ thì nhận click. Nhưng `WndProc` cũ trả
`HTTRANSPARENT` cho **mọi** `WM_NCHITTEST` ("play mode: overlay never takes
clicks") ⇒ overlay xuyên click 100% ⇒ chỉ hotkey là dùng được.

### 3.2 Thiết kế mới: hit-test theo **đúng vùng widget**

```cpp
struct HitRect { float l, t, r, b; };
static std::vector<HitRect> g_menuHits;      // rebuild mỗi frame
static void hitMark() {                       // gọi ngay sau mỗi widget
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    g_menuHits.push_back({ a.x, a.y, b.x, b.y });
}
```

`WndProc`:

```cpp
if (m == WM_NCHITTEST) {
    if (g_clickthru) return HTTRANSPARENT;            // -clickthru: tắt hẳn
    POINT pt{ (short)LOWORD(l), (short)HIWORD(l) };
    for (const auto& hr : g_menuHits)
        if (pt.x >= hr.l && pt.x < hr.r && pt.y >= hr.t && pt.y < hr.b)
            return DefWindowProcW(h, m, w, l);        // nhận click
    return HTTRANSPARENT;                             // còn lại về game
}
```

- Rect được thêm cho: **title bar** (`GetFrameHeight()+4`) + từng widget
  (checkbox, slider, nút) qua `hitMark()`.
- Khe hở giữa các widget **không** nhận click ⇒ game vẫn nhận (an toàn hơn).
- `g_menuHits.clear()` mỗi frame và khi overlay ẩn (`wantShow == false`).
- `ImGuiCond_Once` cho lần đặt vị trí đầu; drag do ImGui tự xử lý.

**Bài học (bug đã gặp)**: bản build đầu dùng **cả hình chữ nhật panel** làm vùng
nhận click. Hậu quả: click trong game rơi vào panel (panel mặc định nằm chỗ hay
bấm) và **lật Speed/Jump** (thấy rõ trong `overlay.cfg` bị đổi 1→0). Phải chuyển
sang hit-test chính xác theo widget.

### 3.3 Lưu / sửa vị trí

- `menu.pos=x,y` — ghi khi kết thúc drag.
- `menu.size=w,h` — ghi khi kích thước panel đổi (>1px).
- Khi khởi động: clamp bằng size đã lưu để panel **không** ra ngoài màn hình.
- **Pull-back**: ImGui cho kéo window ra ngoài mép; nếu title bar ra khỏi màn hình
  thì panel được kéo lại (`g_menuPosForce` + `ImGuiCond_Always` cho 1 frame) để
  luôn còn chỗ nắm lại. Không đụng vào khi chuột còn đang giữ (tránh giành với drag).
- `ImGui::GetIO().IniFilename = nullptr`: **phải** tắt `imgui.ini`, nếu không nó
  đè vị trí `menu.pos` (đã dính lỗi này: menu về (16,360) dù cfg ghi (900,90)).

### 3.4 Ghi cfg

Mọi thay đổi từ menu đi qua `upsertCfgKey()` (đã có sẵn cho hotkey):
`fly.enabled`, `fly.speed`, `fly.key`, `ply.speed`, `ply.speedval`, `ply.mode`,
`ply.jump`, `ply.jumpval`, `ply.jumpmode`, `aim.enabled`, `aim.fov`,
`trig.enabled`, `autoclick.enabled`, `hitbox.enabled`, `menu.show`, `menu.pos`,
`menu.size`.

Helper mới (đầu file):
- `chkInt(const char*, int*)` + overload `bool*` — checkbox cho cờ config kiểu int.
- `sliderFloatCfg()/sliderIntCfg()` — slider + ghi cfg khi `IsItemDeactivatedAfterEdit()`.

### 3.5 Không dùng được Combo/popup

Popup của `ImGui::Combo` vẽ **ngoài** panel ⇒ nằm ngoài vùng `g_menuHits` ⇒ click
vào list sẽ xuyên xuống game. Vì vậy chỗ chọn chế độ dùng **nút click để xoay vòng**
(label `impl: property##pm` — phần sau `##` giữ ID ổn định).

### 3.6 Fix hotkey tắt được

`parseKey("NONE") == 0`, mà code cũ là `if (v) cfgFlyKey = v;` ⇒ `fly.key=NONE`
bị bỏ qua, restart là F bật lại. Đã sửa thành:

```cpp
if (strncmp(line, "fly.key=", 8) == 0) cfgFlyKey = parseKey(line + 8); // NONE -> 0
```

## 4. Kiểm chứng

| Test | Cách làm | Kết quả |
|---|---|---|
| Click widget | `SetCursorPos` + `mouse_event` vào checkbox Fly (775,234) | `fly.enabled` 0→1→0 trong cfg ✔ |
| Drag title bar | kéo từ title bar | `menu.pos` đổi: 336,543 → 271,405 → 76,466 (user kéo + test tao) ✔ |
| Kéo slider | user kéo slider Jump | `ply.jumpval` 90→95 ✔ |
| Clamp vị trí xấu | nhét `menu.pos=600,900` rồi restart | tự sửa thành `600,472` ✔ |
| Round-trip hotkey | `fly.key=NONE` + restart | HUD hiện `[0x0]`, cfg vẫn NONE ✔ |
| Hệ toạ độ | in ra stdout: `dpi=96, scr=1280,800, disp=1280x800, fb=1.00, client=1280x800` | ImGui coords == pixel vật lý ⇒ clamp/hit-test đúng hệ ✔ |
| Ẩn/hiện | `INSERT`, nút `X` | panel ẩn/hiện, trạng thái lưu cfg ✔ |

## 5. Gotcha khi làm tiếp

1. **Đừng đo toạ độ từ ảnh chụp PowerShell.** Script capture là DPI-unaware nên
   ảnh có thể lệch tỉ lệ so với toạ độ ImGui (đã mất thời gian vì chuyện này). Muốn
   số chuẩn: in ra stdout (redirect ra file) hoặc đọc dòng `Menu:` trên HUD.
2. Menu **ăn click trong vùng widget**. Nếu đặt panel vào chỗ hay bấm trong game thì
   click đó sẽ bị nuốt (và có thể lật toggle). Cách tránh: `INSERT` ẩn khi chơi, hoặc
   kéo panel ra chỗ trống. (Đây cũng là lý do phải hit-test theo widget, không theo panel.)
3. Popup/Combo: xem 3.5.
4. `g_menuHits` phải được `clear()` khi overlay bị ẩn (`!wantShow`), không thì vùng
   click cũ vẫn còn hiệu lực.
5. Hit-test dùng rect của frame trước (rebuild mỗi frame) — trễ 1 frame, không thấy được.
6. `ImGui::GetItemRectMin/Max` phải gọi ngay sau widget tương ứng (sau `SameLine()`
   thì "last item" không đổi nên vẫn đúng).
7. Build overlay: phải `Stop-Process -Name ext_overlay -Force` trước (file bị khoá),
   build xong `Start-Process` lại.

## 6. Cấu hình hiện tại (overlay.cfg sau khi chốt)

```
ply.speed=0        # user tắt bằng menu
ply.jump=1
ply.jumpval=95
ply.mode=0
fly.enabled=1
fly.key=NONE       # user tắt hotkey F -> chỉ dùng menu
fly.speed=71
aim.enabled=1
autoclick.enabled=1
hitbox.enabled=1
menu.show=1
menu.key=INSERT
menu.pos=76,466
menu.size=338,275
```

## 7. Việc có thể làm tiếp (chưa yêu cầu)

- Bỏ/thu gọn dòng debug `Menu:` trên HUD (hiện in `pos/win/size/scr/disp`).
- Đổi hotkey của từng feature ngay trong menu (hiện chỉ bật/tắt hotkey F của Fly).
- Nút `Hide HUD` / `Hide menu` trong panel.
- Tab/scroll (`ImGuiWindowFlags_` + collapsed mặc định) nếu thêm nhiều tính năng.
- Hiện trạng thái key hiện tại của Aim/Trigger/AutoClick trong panel.
