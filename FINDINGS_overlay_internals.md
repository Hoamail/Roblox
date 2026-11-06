# FINDINGS — overlay internals (hợp nhất & đã xác minh)

- Hợp nhất: 2026-10-22
- Đối chiếu trên: `ext_overlay.exe` sha256 `7C643B63E24EA923…` (build 2026-10-22 09:33) + `src/` hiện tại
- Offsets tham chiếu: `offsets_fixtures/version-9ac8595712b447a7.json` (theo RbxDumperV2, dump 20/10/2026)
- Thay thế 5 file: `velext_aim_findings.md`, `velext_P0_findings.md`, `velext_P1b_findings.md`,
  `HANDOFF_aimsmooth.md`, `HANDOFF_speed_jump.md`
- Vẫn giữ riêng: `HANDOFF_fly_velocity.md` (số đo fly chi tiết), `HANDOFF_roblox_update_playbook.md` (quy trình update)

Quy ước bằng chứng: **[P]** = chứng minh được (instruction/địa chỉ/đọc code hiện tại),
**[I]** = suy luận nhất quán, **[?]** = chưa rõ.

Nguyên tắc của file này: **số offset lấy từ dump hiện hành, không lấy từ velext.** Số của
velext là của build Roblox ngày 17/10, đã lệch (xem §6) — chúng chỉ dùng để hiểu *cơ chế*,
không dùng để code.

---

## 0. TL;DR

1. Aim của overlay hiện tại là mô hình velext: **bộ lọc thời gian một cực**
   `alpha = 1 - exp(-dt/tau)`, `tau = 0.008 * max(1, smoothing)`, tối đa **1 actuation / 4 ms**
   (250 Hz), deadzone `dot > 0.99996` (~0.51°). [P] `src/ovl/ovl_aim.h:47..58`,
   `src/overlay.cpp:193..200`
2. Actuation mode 0 (Mouse) **không** dùng SendInput/SetCursorPos; nó ghi **mouse counts**
   theo công thức `atan(...)/cosp/(0.5*out1)` với `out1/out2` đọc từ 3 biến float trong module
   Roblox (`base+0x8D857D8/0x8D857E0/0x8D85820`). [P] `src/overlay.cpp:246..320`
3. Mode 1 (Camera/memory) là nlerp trên look-direction, **đọc camera tươi ở mỗi lần ghi**,
   không giữ state → không bị trôi khi game tự xoay camera. [P]
4. Speed/Jump dùng cặp **property + shadow copy** (`Humanoid.Walkspeed` + `WalkspeedCheck`);
   ghi lệch cặp = **Error 268 / kick**, không phải do "bị phát hiện API". [P] `overlay.cpp:1978..2030`
5. Fly **phải** là velocity writer tốc độ cao (~1e6 writes/s). Position-step 628 writes/s = **0 studs**.
   Anchored bị solver bỏ qua → không dùng. [P] `src/ovl/ovl_fly.cpp:1..26`
6. Điểm **hardcode theo build Roblox** duy nhất còn lại: 3 RVA độ nhạy ở §2. Sai build → đọc fail
   → **âm thầm fallback** về gain tuyến tính (không crash, nhưng cảm giác aim khác). [P]
7. Ba findings cũ đã bị chứng minh là **sai/lỗi thời** — xem §7. Đừng đọc lại 5 file gốc.

---

## 1. Aim model (đang chạy)

### 1.1 Bộ lọc thời gian (dùng chung cho cả 2 mode)

| tham số | giá trị | nơi | ghi chú |
|---|---|---|---|
| `alpha` | `1 - exp(-dt/tau)` | `overlay.cpp:194..200` | `smoothing <= 1` → snap (`alpha = 1`) |
| `tau` | `0.008 * max(1, smoothing)` s | `ovl_aim.h:47` | `aim.smoothing` default **5.00**, clamp **1..20** |
| `dt` fallback | `0.004` s | `ovl_aim.h:48` | khi thiếu timestamp publish |
| `dt` max | `0.1` s | `ovl_aim.h:49` | chống spike sau khi alt-tab |
| gate actuation | `4e6 ns` (250 Hz) | `ovl_aim.h:50` | giữa 2 lần ghi liên tiếp |
| deadzone góc | `dot > 0.99996` | `ovl_aim.h:52` | ~0.51° → coi như đã trúng |
| deadzone chuột | `1.5` px | `ovl_aim.h:53` | mode 0 |
| alpha eps | `0.002` | `ovl_aim.h:51` | dưới ngưỡng thì bỏ qua actuation |
| step cap | `clamp(14000*dt, 12, 400)` px | `ovl_aim.h:54..56` | chỉ áp khi `alpha < 1` |

Ý nghĩa thực tế: `smoothing=f` ⇒ hằng số thời gian `8·max(1,f)` ms. `f=5` → 40 ms → trễ ~1 frame
ở 60 fps, ~3 frame ở 144 fps. Chỉnh `f` là chỉnh "độ mượt" duy nhất; `aim.smooth/aim.tsmooth/
aim.memsmooth` là key cũ, **parser vẫn đọc nhưng execution path bỏ qua** (1 dòng log cảnh báo).

`aim.pid` : `0` = mô hình velext (default) · `1` = đường slerp/PID cũ, chỉ để A/B so sánh. [P]

### 1.2 Mode 0 — Mouse counts

```
cosp = max(0.15, sqrt(max(0, 1 - p*p)))              ; p = thành phần pitch của look dir
cx   = atan(eX/focal) * RAD2DEG / cosp / (0.5 * out1) * alpha
cy   = atan(eY/focal) * RAD2DEG / (0.385 * out2)     * alpha
if (alpha < 1)  clamp cx, cy vào ±clamp(14000*dt, 12, 400)
```

- `focal` = `0.5 * viewportHeight / tan(fov/2)` (từ `Camera.FieldOfView` + `Viewport`). [P]
- `out1 = |f| * A.x`, `out2 = A.y * f`, với `f` = float tại `MouseService+0x21C` (**soft**,
  không đọc được thì dùng `1.0`). `A`, `B`, `C` là **hard**: fail là hỏng cả đường này. [P]
- Validation giống hệt velext: mọi giá trị phải hữu hạn và `1e-3 ≤ |v| ≤ 100`;
  `out1/out2` phải nằm trong `[1e-4, 1000]`. [P] `overlay.cpp:251..282`
- Cap dùng **dt**, không dùng alpha — đã chốt bằng register trace tại site clamp
  (findings cũ còn hedge điểm này, xem §7.2).
- Đường dự phòng: `aim.sens` (gain tuyến tính) + kernel SendInput. Đây là đường **cũ**, vẫn giữ
  để không chết aim khi build Roblox đổi. [P]

### 1.3 Mode 1 — Camera / memory writer

- Mỗi publish: đọc camera CFrame tươi → tính look dir hiện tại → nlerp về target với `alpha` →
  ghi lại 1 lần. **Stateless**, nên game tự xoay (cutscene, knockback, shake) không bị chống lại. [P]
- Tối đa 1 actuation / 4 ms; main loop publish theo nhịp refresh view (~250 Hz khi cửa sổ active).
- Không dùng InputObject injection, không gọi remote. [P]

### 1.4 Back-pressure + cursor guard (chống "aim kẹt")

| cơ chế | ngưỡng | hành vi |
|---|---|---|
| back-pressure | camera nhích `< 0.02°` trong `60 ms` | không ghi tiếp, chờ (tránh bão hoà) |
| stale-hold | 6 lần liên tiếp không nhích | bỏ target, chờ relock |
| probe | mỗi `750 ms` | thử 1 actuation nhỏ để phát hiện relock |
| cursor guard | `GetCursorInfo` thấy cursor đang hiện | không inject counts (third-person free-look sẽ văng cursor) |

Nguồn: `ovl_aim.cpp:400..500`, `kVelextBp*` trong `ovl_aim.h:57..58`. Log tương ứng:
`aim-lock-regained`, `aim-stale-hold`, `aim-cursor-hold`.

### 1.5 Xác minh

```
ext_overlay.exe -aimtest      # 522 dòng, 0 FAIL, exit 0
```
Unit test dùng **chung** hàm thuần (`velextAlphaFromDt`, `velextMouseCounts`) với code ship,
nên không thể lệch giữa test và runtime. [P]

### 1.6 Bất biến của writer mode 1 (đừng phá)

Writer ghi đúng **36 byte** (9 float) tại `Camera + Camera.Rotation`. Vì vậy:

- **`Camera.Rotation` không được có fallback.** Bản cũ để `= 200` (offset của build cũ). Trong
  layout hiện tại `Camera.Rotation=192`, `Camera.Position=228`: ghi 9 float tại 200 sẽ đè
  `Rotation[2..8]` **và** `Position.x/y` (228/232) → camera đứng im, mouse look chết, màn hình
  "cứng đờ" đúng như triệu chứng aim không đi theo mục tiêu. Giờ: `cameraRotation` mặc định `0`;
  `0` → `writerThread` không chạy (`writer-disabled`), startup ép `aim.mode=0` + banner đỏ.
  Key này cũng nằm trong danh sách `opt()` nên thiếu key là thấy ngay ở footer.
- **Mode 1 không được rơi về đường chuột.** Trước đây `else if (paced)` bắt cả trường hợp
  `camAddr == 0`, tức là mode memory bỗng inject mouse counts. Giờ nhánh chuột chỉ chạy khi
  `cfgAimMode == 0`; mode 1 thiếu camera/rotation chỉ log `aim-no-actuator`.
- **Mục tiêu phải còn sống.** `Ent` lưu thêm `rootPart/headPart/limbPart`; mỗi refresh (250 ms)
  kiểm `Instance.Parent(part) == model`. Part bị destroy/reparent để lại `Primitive` đọc được
  nhưng `Position` **đóng băng** → aim bám điểm chết và camera đứng yên. Log:
  `entity-reresolve ... reason=part-gone|bone-gone`, `npc-reresolve`.
- **Bone xa root > 30 studs = bỏ bone.** Chặn trường hợp `headPrim`/`limbPrim` trỏ vào part cũ
  trong khi root vẫn chạy (aim bám 1 điểm cố định). Log: `aim-bone-stale`.

Chẩn đoán khi triệu chứng quay lại: `aim_diag.log` có `aim tick ... tgt=yes|no prim=... world=...`
mỗi 1 s. `world` không đổi qua nhiều dòng = target ghost (cache/part). `tgt=no` = mất target
(FOV/khoảng cách/health). `aim-no-actuator` = mode 1 không ghi được. [P]

---

## 2. Độ nhạy chuột — điểm hardcode duy nhất theo build Roblox

| gì | địa chỉ | tính chất |
|---|---|---|
| `A` (float2) | `robloxBase + 0x8D857D8` | hard — fail = fallback |
| `B` (float2) | `robloxBase + 0x8D857E0` | hard |
| `C` (float) | `robloxBase + 0x8D85820` | hard |
| `f` (float) | `MouseService + 0x21C` | soft — fail = 1.0 |

Nguồn: `kVelextSensRva*` `ovl_aim.h:59..62`; logic đọc `overlay.cpp:246..290`.

**Rủi ro sau mỗi lần Roblox update:** 3 RVA này là offset trong `.data`/`.rdata` của build Roblox,
không nằm trong `offsets.json` của theo. Khi lệch:
- không crash, không log đỏ, không gate nào bắt;
- `readVelextSens` fail → mode 0 tự chuyển sang gain tuyến tính (`aim.sens`), aim vẫn trúng nhưng
  tốc độ/độ nhạy khác đi.

Cách phát hiện nhanh: bật aim mode 0, nếu "cảm giác nhạy" đổi rõ rệt so với trước update → nghi RVA.
Cách sửa: scan 3 float liên tiếp trong `.data` quanh vùng cũ (giá trị 1e-3..100, quan hệ
`out1/out2` hợp lệ) hoặc chờ cộng đồng. **Chưa có test offline** cho phần này — đây là mục P2 còn mở.

---

## 3. Speed / Jump

### 3.1 Bảng mode

| key | giá trị | ý nghĩa |
|---|---|---|
| `ply.mode` | `0` property · `1` velocity | cách đặt tốc độ chạy |
| `ply.jumpmode` | `0` auto · `1` JumpPower-only · `2` velocity impulse | cách nhảy |

### 3.2 Property mode (mặc định)

- **WalkSpeed phải ghi kèm shadow** `Humanoid.WalkspeedCheck` (dump hiện tại: `Walkspeed=452`,
  `WalkspeedCheck=924`). Roblox so sánh 2 field → lệch = **Error 268 / kick**. Đây là nguyên nhân
  gốc của lỗi "kick khi chỉnh speed" thời kỳ đầu, **không phải** do dùng `WriteProcessMemory`. [P]
  `overlay.cpp:1978..2030`
- Lưu/khôi phục cả 2 field khi tắt (state `g_plyRestWS`).
- **JumpHeight không có shadow** → không phải sync gì thêm (đã scan toàn bộ Humanoid). [P]
- `ply.jumpmode=0`: ghi **cả** `JumpPower = slider` và `JumpHeight = slider²/(2·196.2)` để đúng
  cho cả game đọc `UseJumpPower` và game đọc `JumpHeight`. [P] `overlay.cpp:2059..2072`
- `ply.jumpmode=1`: chỉ ghi `JumpPower` (giống tool tham chiếu).
- **Gravity 196.2 hardcode** tại `overlay.cpp:2060` dù dump có `World.Gravity=544`. Game đổi gravity
  → quy đổi JumpHeight lệch theo `sqrt(g/196.2)`. Chưa parse field này; nếu cần thì đọc
  `World.Gravity` và thay hằng số.

### 3.3 Velocity impulse (jumpmode 2)

Ghi thẳng `Primitive.AssemblyLinearVelocity.y = ply.jumpval` khi nhấn Space (`GetAsyncKeyState`
edge). Không đổi property nào ⇒ **không check nào thấy field đổi**; hoạt động cả khi đang ở trên
không → nhảy vô hạn. [P] `overlay.cpp:2074..2085`

### 3.4 Velocity speed (ply.mode 1)

Mỗi tick: hướng đi mong muốn từ WASD → kéo vận tốc ngang về hướng đó, **không đụng trục Y**
(jump/gravity nguyên vẹn). Không giữ phím → không ghi gì, friction tự nhiên. [P]

---

## 4. Fly — bất biến của engine (quan trọng nhất trong file này)

Engine ghi lại `Primitive.Position` **và** `AssemblyLinearVelocity` mỗi frame (solver của
`World`/`worldStepsPerSec`). Hệ quả đo được:

| cách ghi | kết quả |
|---|---|
| `Primitive.Position` 1 lần/frame | **0 studs** (bị solver revert sau 10–50 ms) |
| `AssemblyLinearVelocity` 628 writes/s (`Sleep(1)`) | **0 studs** |
| `AssemblyLinearVelocity` ~1.0e6 writes/s (`Sleep(0)`) | bay đúng, leo đủ |
| set bit `Anchored` | **không có tác dụng** — cờ dính trong memory nhưng solver bỏ qua |

⇒ `src/ovl/ovl_fly.cpp` là writer thread riêng, `Sleep(0)` mỗi vòng (~1e6 writes/s);
main loop chỉ `Fly::publish` hướng/keys. Bật fly = ăn **1 core**; tắt → thread về `Sleep(10)`.
Tuyệt đối không bật `Anchored` cho fly. Số đo chi tiết + log: `HANDOFF_fly_velocity.md`.

---

## 5. Hitbox

- Bit cờ đọc từ dump (`PrimitiveFlags`): `Anchored=2`, `CanCollide=8`, `CanTouch=16`, `CanQuery=32`.
  Code dùng `o.primitiveAnchoredBit` / `o.primitiveCanCollideBit`, **không hardcode**. [P]
- Bật: read `Primitive.Flags` (446) → set/clear bit → lưu `(prim, flags gốc)` vào map → tắt: ghi lại
  giá trị gốc. Không có restore = part ở lại trạng thái sai sau khi tắt. [P] `overlay.cpp:640..720`
- Đổi build mà `validateOffsets` vẫn pass nhưng `PrimitiveFlags` đổi nghĩa ⇒ hitbox ghi sai bit.
  Gate hiện tại chỉ check range/align, **không** check semantic của bit.

---

## 6. Velext-era vs hiện tại (đừng dùng số cũ)

| field | velext 17/10 | dump 20/10 (đang dùng) |
|---|---|---|
| `Humanoid.Walkspeed` | `0x1CC` (460) | **452** |
| `WalkspeedCheck` (shadow) | `0x3A4` (932) | **924** |
| `Humanoid.JumpPower` | `0x1A0` (416) | **408** |
| `Humanoid.Health` | `0x18C` (396) | **388** |
| `Camera.Position` | `0xEC` (236) | **228** |
| `Camera.Rotation` (CFrame) | `0xC8` (200) | **192** |
| `Primitive.Position` | — | **212** |
| `Primitive.AssemblyLinearVelocity` | — | **224** |
| `Humanoid.HumanoidRootPart` | — | **1120** |

Tất cả lệch **−8 byte** so với build 17/10 (Roblox chèn 1 field). Đây là lý do tuyệt đối không
hardcode: mỗi update có thể dịch cả một khối.

### 6.1 Projection — nguồn thật và bẫy đơn vị `FieldOfView` (đo 06/11/2026, `f0ea5da9412f4e35`)

ESP/aim chỉ có hai nguồn projection; **cả hai đều từng chết im lặng**, và triệu chứng giống hệt
"cache rỗng": box/name không hiện, FOV gate không khớp, deadzone không bao giờ bắn.

**1) `VisualEngine.ViewMatrix` (dump: 432) — build này SỐNG.** Đọc live:

```
ve[0] =  0.8673  0        0        252.5858
ve[1] =  0        1.4281   0       -433.5385
ve[2] =  0        0        0        0.1007
ve[3] =  0        0       -1.0000  -739.5240
```

Giải mã (camera pos `-291.23,303.57,-739.52`, client `1280x777`):
- `0.8673 = 1/(aspect·tan(fov/2)) = 1/(1.6474·0.7002)`; `1.4281 = 1/tan(35°)` → FOV thật **70°**.
- hàng w `(0,0,-1)` = forward; `m15 = -739.524 = -eye.z` → `w(eye) = 0`, eye chiếu đúng tâm client.
- aspect khớp đúng `1280/777`, không phải aspect màn hình.

Trên `5a21b87bbc2641cf` cùng key này là blob chết (`row2 = (0,0,0,0.0998)`, 16 float không đổi
qua 12 mẫu) — nên **không được tin key, chỉ được tin phép kiểm**.

**2) `Camera.Rotation` (192) + `Position` (228) + `FieldOfView` (296)** — model velext
(`FUN_7ff7ddc3a120`), dùng làm fallback. Basis vẫn orthonormal, đúng layout cột
Right/Up/−Look trên build này.

**Bẫy đơn vị:** `Camera+296` trên build mới lưu **RADIANS**. Đo thật: `fovRaw=1.2217` ⇔ `70.00°`.
Trước khi sửa, `camViewBuild()` nhận 1.2217 như *độ* → `tanHalf = 0.0106` → phóng ~66×: nhân vật
cách camera 16.1 studs chiếu ra `y≈4434` trên client cao 777 (đo bằng `-diag`, dòng `proj-check`),
tức mọi box/name nằm ngoài màn hình. Cùng giá trị đó làm aim focal (`10 < fov < 150`) fail mỗi
frame → aim rơi về gain legacy. Chuẩn hoá: `camFovDegrees()` trong `src/ovl/ovl_core.h`
(`raw < 3.2 → rad`). `ovl_world.cpp` đã tự xử lý từ trước (`kFovRadMax`); projection + aim thì chưa.

**Thứ tự chọn (`overlay.cpp`):** VE matrix nếu `camViewMatrixSane()` **và**
`camViewMatrixAgrees()` (`|dot(fwd_matrix, fwd_camera)| ≥ 0.98`); nếu không thì camera-derived.
`-diag` in `vm=`: `1` = VE, `2` = camera, `0` = không có.
- `camViewMatrixSane()` (chống blob chết): `|row0|·aspect ≈ |row1|`, `w(eye) ≈ 0`
  (≤ 2·|w row|), điểm 20 studs phía trước → tâm client ±5%.
- `camViewMatrixAgrees()` khoá ma trận vào camera **cùng frame**, nên blob "trông hợp lệ" nhưng
  không xoay theo camera vẫn bị loại.

**Gate:** `-aimtest` mục `[6]` kiểm `fov-radians`, `vp-accepted`, `vp-moved-rejected`,
`vp-agrees`, `vp-turned-rejected`. `ext_projcheck.exe` in số live (camera pos/rot/fov, cả hai ma
trận, `dotR/dotU/depth` từng player) — dùng khi nghi box lệch.

---

## 7. Corrections — findings cũ SAI hoặc đã lỗi thời

1. **`velext_P0_findings.md` §1.7 / §7**: "không có bằng chứng velext ghi `Humanoid+0x1CC` /
   `+0x3A4`". → **SAI**. Bản runtime P1b chứng minh `FUN_1400db390` ghi đúng 2 field đó (và đó là
   nguồn gốc Error 268 khi lệch cặp). P1b thay thế kết luận này.
2. **`velext_aim_findings.md` §14.5**: cap `14000·x` còn hedge "x = dt hay alpha". → **chốt là dt**
   (register trace tại site clamp trùng giá trị feed `exp(-dt/tau)`); code hiện tại dùng `dt`.
3. **`HANDOFF_aimsmooth.md` §8(c)**: "UI-diff `+0xC8` / `+0xA4` / `+0xA8` còn mở". → **đã đóng**:
   §14.10 chứng minh `targetPart=+0xC8`, `priority=+0xCC`, `aimKey=+0xA4`, `keyMode=+0xA8`.
4. **Bản Phase A của `velext_aim_findings.md`**: "sensitivity đọc ở `MouseService+0x21C`". →
   **không đủ**: nguồn thật là `A/B/C` tại `base+0x8D857D8/0x8D857E0/0x8D85820`; `+0x21C` chỉ là
   scale phụ (`f`, soft).
5. **Tick 64.5 Hz** (đo 1 mẫu ở Phase A) → Phase B đo **250 Hz (4 ms)** trên 3 lần chạy; code dùng
   gate 4 ms.
6. **`src/overlay.cpp:428`** comment cũ: "fly (anchored HRP + position steps)" → **SAI**, fly là
   velocity writer. Đã sửa 2026-10-22.
7. `velext_v1.exe` sha256 `0a78225ff73d6c93…` là **binary đích của RE**. Findings trong file này đủ
   để hiểu cơ chế; nhưng nếu xóa binary thì **không tái lập được thí nghiệm** (trace UI, dump .data).

---

## 8. Checklist xác minh (chạy khi nghi ngờ)

```
ext_overlay.exe -aimtest                 # 522 dòng, 0 FAIL, exit 0
ext_overlay.exe -srvtest 0               # === done
ext_overlay.exe -offtest offsets_fixtures  # fails=0, keys=50
ext_overlay.exe -verify                  # verify: PASS (cần Roblox đang mở, đang ở trong place)
ext_projcheck.exe                        # fovRaw/fovDeg + VE matrix + px/py từng player
```
- Aim mode 0 sai cảm giác sau update → §2 (RVA độ nhạy).
- Speed bị kick → §3.2 (cặp WalkSpeed/shadow, không phải API).
- Fly 0 studs → §4 (phải là velocity writer tốc độ cao, không position-step, không anchored).
- Box/name không hiện dù `entity refresh ... ents=N` (N>0) → §6.1: `ext_projcheck` phải cho
  `fovDeg ≈ 70` (không phải `1.22`) và `-diag` phải in `vm=1` hoặc `vm=2` (không phải `vm=0`).
- Ping Server `--` → `StatsItem.Value` trong dump là offset rác (52434, unaligned) và place hiện tại
  không có node `Data Ping`. Không phải lỗi parse.

## 9. Rủi ro còn lại (sau update Roblox)

| # | rủi ro | hậu quả | phát hiện |
|---|---|---|---|
| 1 | 3 RVA độ nhạy (§2) lệch | aim vẫn chạy nhưng sai cảm giác, **im lặng** | so cảm giác / scan `.data` |
| 2 | `World.Gravity` đổi | JumpHeight quy đổi lệch | hiếm, chỉ game custom gravity |
| 3 | `PrimitiveFlags` đổi nghĩa bit | hitbox ghi sai bit | test trong game |
| 4 | `StatsItem.Value` rác | ping hiển thị `--` | đã biết |
| 5 | `Instance.NameContainer/Name` đổi | `ents` rỗng, mọi tab trắng | `-verify` chain FAIL (đã có gate) |
