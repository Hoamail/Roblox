# Fly (Body tab) — vì sao bản cũ không bay, và bản mới khác gì

Ngày: 2026-10-20. Game test: `version-11f155f3450f4c8e`, place `85011874799856`
(nhân vật `walk=16 jp=50 jh=7.2 ujp=1`), đối chứng: `velext_v1.exe`.

---

## 1. Triệu chứng báo cáo

Bật Fly trong tab Body, bấm Space → nhân vật **giật về đất**, không bay được;
kéo slider `fly.speed` không có tác dụng gì.

---

## 2. Bản cũ làm gì (và tại sao hỏng)

Mỗi vòng của main loop (~60 Hz), khối fly trong `overlay.cpp` làm 3 việc:

```cpp
// (1) bật bit Anchored trên HumanoidRootPart
want = flags | o.primitiveAnchoredBit;      // 0x2
m.write(prim + o.primitiveFlags, want);

// (2) zero velocity
m.write(prim + o.primitiveVelocity, {0,0,0});

// (3) dịch vị trí một bước
np = pos + dir * cfgFlySpeed * dt;          // dt ~ 16 ms
m.write(prim + o.primitivePosition, np);
```

Giả định của bản cũ: "anchored thì physics không kéo xuống, position write thì dính".
Giả định đó **sai với engine hiện tại**. Đo bằng `src/flyprobe.cpp` (`ext_flyprobe.exe`):

| Thử nghiệm | Kết quả đo |
|---|---|
| Ghi bit `Anchored` vào `Primitive.Flags` (+446) | Bit **dính trong memory 25/25 mẫu**, physics **vẫn rơi** → solver bỏ qua anchored |
| Ghi `Primitive.Position` (+212) **1 lần** | Đọc lại thấy đúng ngay sau write, nhưng **bị ghi đè về giá trị gốc sau ~10–50 ms** (kể cả khi đã anchored) |
| Ghi `AssemblyLinearVelocity` (+224) **1 lần/frame** (≈628 writes/s) | **0.000 studs** dịch chuyển; `vel.y` đọc lại = 0 |
| Bão position ~140k writes/s trong 200 ms | chỉ nhích ~6 studs rồi giật, không kiểm soát được |
| Ghi velocity ~1.17M writes/s (burst 150 ms) | +4.3 studs (đúng hướng), sau đó còn trớn |
| Ghi velocity ~1.05M writes/s (Sleep(0), 1 s) | **+40.4 studs/s** với target 45 u/s |
| Ghi velocity ~1.29M writes/s (`_mm_pause`, 1 s) | **+46.5 studs/s**, `vel.y` đọc lại đúng bằng target |

Kết luận cơ chế: game **tái tạo** `Primitive.Position` / `AssemblyLinearVelocity`
từ state mô phỏng của nó **mỗi frame render**. Vì vậy:

* write 1 lần/frame luôn bị ghi đè **trước** physics step kế tiếp → vị trí không đổi;
* cú nhảy hợp lệ của game (Space) bị chính write kế tiếp kéo về → "giật về đất";
* `anchored` không được solver tôn trọng nên không có "phao" nào giữ nhân vật.

Đó chính là toàn bộ lỗi. Không phải sai offset (`offsets_cache.json`
`version-11f155f3450f4c8e` khớp: `Anchored=2`, `Position=212`, `Velocity=224`,
`Flags=446`), không phải sai tốc độ — mà là **sai cơ chế**.

---

## 3. Bản mới làm gì

`src/ovl/ovl_fly.cpp` (+ `ovl_fly.h`) — một writer thread riêng:

* main loop **chỉ publish** mỗi frame: `Fly::publish(m, o, selfPrim, camAddr, cfgFlySpeed, on)`.
* thread giữ `AssemblyLinearVelocity` ở **~1e6 writes/s** (`Sleep(0)` mỗi vòng,
  mỗi vòng đúng 1 write 12 byte).
* Cứ ~1024 vòng (≈1 ms) mới đọc camera matrix + `GetAsyncKeyState`
  (W/A/S/D, Space, C/Ctrl) và tính lại vector đích:

```
v = ( mx * speed, my * speed, mz * speed )
mx,mz = hướng camera (forward/right) đã normalize, my = ±1 theo Space/Ctrl
không bấm phím nào  -> v = (0,0,0)  -> hover tại chỗ
```

* `on = false` (tắt F, untick Fly, mất focus game, panic, thoát overlay) →
  ghi 0 **một lần** rồi thread về `Sleep(10)`.

Vì sao hover được: gravity không tích luỹ nổi vì velocity bị ghi đè trước mỗi
physics step; vector 0 nghĩa là nhân vật đứng yên giữa không trung.

Kiểm chứng trên bản chạy thật sau khi tách module
(`ext_overlay.exe` + `ext_flywatch --hz 20`, `fly_fix_evidence.csv`,
`diag_fly_module.txt`, `fly.speed=40`):

```
t=123281  F bật        diag: fly[arm=1 key=F active=1 apply=1]
t=124844  Space giữ    y 9.60 -> 25.44     vy = 40.00  (đúng target)
t=128828  Space giữ    y 31.83 -> 46.19    vy = 40.00   leo đều
t=131203  Space giữ    y 75.92 -> 132.57   vy = 40.00
t=132000  W giữ        vx 35.98 vz 17.48   |v| = 40.00  (hướng camera)
t=135203  xoay camera  vx 13.08 vz 37.80   |v| = 40.00  (đổi hướng theo cam)
t=136828  W giữ        vx -17.48 vz 35.98  |v| = 40.00
t=140406  thả phím     vy 0.00 vx 0.00 vz 0.00  -> hover tại y=114.7
t=149xxx  F tắt        rơi tự do, không còn write
```

Điểm quan trọng: các giá trị **đúng bằng** `fly.speed` (40.00), không phải
"xấp xỉ" — chứng tỏ velocity được ghi đè đúng trước mỗi physics step. Trước khi
fix, cùng phép đo này cho `vy = 0.00` và `y` không đổi.

---

## 4. Bắt chước velext ở đâu

Capture velext bay (`flywatch.csv`, 20 Hz, chính tay user bay):

| Quan sát trên velext | Kết luận | Ta làm giống? |
|---|---|---|
| `AssemblyLinearVelocity.y = 129.516` **hằng số** suốt lúc leo (4 mẫu liên tiếp, 47 ms/mẫu) | Bay bằng **velocity được giữ liên tục**, không phải position step | ✅ giữ velocity bằng writer tốc độ cao |
| `Primitive.Flags = 0x230` suốt lúc bay (không có bit `0x2`) | **Không dùng Anchored** | ✅ bỏ hẳn anchored |
| Trong 1.3 s nhân vật đi ngang ~96 studs (≈74 u/s) trong khi `WalkSpeed=16` | Nó cũng **lái cả vận tốc ngang**, không chỉ trục Y | ✅ vector 3 chiều theo camera + WASD |
| y leo mượt 26.3 → 45.8 → 61.7 rồi mới rơi; vy chuyển +66 → −45 đột ngột khi ngừng | Velocity bị *ghi đè* liên tục, không phải impulse một lần | ✅ ghi liên tục, ngắt là thả |
| `HipHeight`, `PlatformStand`, `JumpPower` **không đổi** trong suốt lúc bay | Không dùng mấy field đó | ✅ không đụng |

Khác biệt có chủ ý:

* velext có vẻ đặt `vy = flySpeed` **cố định** (129.516 bất kể hướng camera),
  còn ta normalize hướng ngang rồi nhân `speed`, nên bay chéo không bị nhanh hơn.
* Ta thêm `my = -1` cho C/Ctrl (hạ xuống) — velext không thấy dùng trong capture.
* Ta zero velocity khi tắt (velext để rơi tự do ngay).

---

## 5. File đã thay đổi

| File | Thay đổi |
|---|---|
| `src/ovl/ovl_fly.h` | **mới** — API `Fly::start/stop/publish/killVelocity/active/writesPerSec` |
| `src/ovl/ovl_fly.cpp` | **mới** — writer thread + toàn bộ số đo/giải thích cơ chế ở đầu file |
| `src/overlay.cpp` | bỏ `flyAnchorPrim/flyAnchorSaved/flyAnchorRestore` + khối anchored/position-step; thêm `#include "ovl/ovl_fly.h"`; thread: `Fly::start(...)`, exit: `Fly::stop(m, o)`, quit path: `Fly::killVelocity(m, o)`; mỗi frame: `Fly::publish(...)` |
| `src/ovl/tab_body.cpp` | cạnh hàng Fly thêm telemetry `x.xx M w/s` (màu xanh) khi writer đang chạy — thấy ngay writer có sống hay không |
| `build_fast.bat`, `build.bat` | thêm `src\ovl\ovl_fly.cpp` vào danh sách nguồn (cả 2 file, đúng quy ước AGENTS) |
| `AGENTS.md` | thêm gotcha: fly phải là velocity writer tốc độ cao, không anchor |

Ghi chú kiến trúc: **không** đặt writer trong `tab_body.cpp`. Tab chỉ là UI
(đúng luật "Tab chỉ đọc state; mọi ghi memory do main loop thực hiện" trong
AGENTS.md) và `drawTabBody` chỉ chạy khi tab Body đang mở — nhân vật sẽ rơi ngay
khi user chuyển sang tab khác. Vì vậy code nằm ở `ovl_fly.cpp`, còn tab chỉ hiển
thị trạng thái.

---

## 6. Cách build / test lại

```bat
build_fast.bat                      :: ~40 s, build ext_overlay.exe
ext_overlay.exe -aimtest            :: exit 0, không dòng FAIL
```

Test tay: tick **Fly**, bấm **F** (arm = tick, activate = F), rồi:

| Phím | Kỳ vọng |
|---|---|
| Space | bay lên đúng `fly.speed` u/s |
| C / Ctrl | hạ xuống |
| W/A/S/D | bay ngang theo hướng camera, đúng `fly.speed` u/s |
| không phím | hover tại chỗ |
| F lần nữa | tắt, rơi tự do |

Đo tự động (khi cần số liệu): `ext_flywatch.exe --ms 30000 --hz 20 --csv out.csv`
rồi xem cột `y`, `vy` — giá trị `vy` phải **đúng bằng** `fly.speed`.

---

## 7. Giới hạn / lưu ý

* Lúc Fly bật, writer ăn **100% 1 core** (Sleep(0) ~1e6 writes/s). Đây là cái giá
  của cơ chế; tắt Fly là thread về `Sleep(10)`. Nếu muốn hạ, phải đo lại ngưỡng:
  628 writes/s đã là **0 studs**, nên không thể giảm nhiều.
* Không dùng `Anchored` nữa: game nào **có** tôn trọng anchored sẽ làm part đứng
  im và velocity fly vô hiệu — nếu gặp, phải xử lý riêng.
* Nhân vật respawn: `publish` lấy `self.prim` mỗi frame nên tự chuyển sang prim
  mới; nếu prim cũ còn sót, writer chỉ ghi nhầm tối đa 1 frame.
* Rollback: `backup\ext_overlay_before_flyfix.exe`, cfg cũ
  `backup\overlay.cfg.before_flytest`.
* Test tự động bằng `SendInput` từ PowerShell **không đáng tin**: nó trả về 1
  nhưng `GetAsyncKeyState` vẫn 0 (bị chặn kiểu UIPI khi target chạy elevated),
  nên nhân vật không nhận phím → kết quả âm tính giả. Test fly phải bấm phím
  thật (hoặc chạy bộ injector cùng integrity level với game).
