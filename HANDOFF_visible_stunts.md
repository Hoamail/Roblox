# HANDOFF — Visible Stunts (root-only choreography)

Trạng thái: **PLAN — chưa code**. Tạo 2026-11-02, để làm tiếp ngày mai.
Mục tiêu: một tab **Stunt** (index 8) biến nhân vật thành một "diễn viên" mà **người khác thật sự nhìn thấy** — lộn, xoay, bay, lết, cưỡi đầu người khác, xác bay — dựa **duy nhất** trên kênh replicate được: transform + velocity của assembly gốc.

Đọc §1 trước. Nếu bỏ qua §1 sẽ code ra thứ chỉ mình thấy và tưởng là hỏng.

---

## 0. TL;DR — quyết định đã chốt

1. **Chỉ root là nhìn thấy được.** `Motor6D.Transform` là render-only, không replicate. Mọi "animation" trong tài liệu này là **choreography trên 6 DOF của assembly gốc** (position + rotation + linear velocity + angular velocity).
2. **Hai chế độ điều khiển**, không trộn lẫn trong cùng một preset:
   - **Dynamic** — chỉ ghi `AssemblyLinearVelocity` / `AssemblyAngularVelocity`, để solver tích phân. Mượt, "physics thật", rẻ (6 float/tick). Dùng cho xoay, lộn, bắn, bay.
   - **Kinematic** — ghi thẳng `Primitive.Rotation` (9 float) và/hoặc `Primitive.Position` (3 float). Chính xác tuyệt đối nhưng phải đo lại tốc độ ghi (xem §2.2 và E1), và chỉ đứng yên được khi humanoid không kéo upright (`PlatformStand=1`, `AutoRotate=0`).
3. **`PlatformStand = 1` là công tắc quan trọng nhất.** Nó tắt torque giữ thăng bằng của humanoid → pose xoay giữ nguyên, người đổ như bao cát, và chính cú đổ đó replicate (người khác thấy bạn ngã).
4. **Không có `Motor6D` offset trong dump** → không pose được tay chân. Tab này cố tình thiết kế quanh giới hạn đó. Nếu sau này dò được `Motor6D` (C0/C1/Transform) thì mở khoá thêm tầng "limb flavor" (§9 bước 12, optional).
5. Ưu tiên làm: **E1/E2 (đo rotation) → preset flip/cart/spin/tube → ragdoll cannon/heli → piggy/orbit → strobe/statue**.

---

## 1. Mô hình replication — cái gì người khác thấy

### 1.1 Bảng kênh

| Kênh | Ghi từ client | Người khác thấy | Độ tin cậy |
|---|---|---|---|
| `Primitive.Position` (212) | có | **có** — đây là nền của teleport hiện tại | đã chứng minh trong project |
| `Primitive.Rotation` (176, 9 float) | có | **có** (là một phần CFrame của assembly) | suy luận chắc + cần E2 xác nhận bằng mắt |
| `Primitive.AssemblyLinearVelocity` (224) | có | **có** — nền của fly | đã chứng minh (fly, speed mode velocity) |
| `Primitive.AssemblyAngularVelocity` (236) | có | **có** (solver tích phân → rotation) | suy luận chắc + E3 |
| `Primitive.Size` (452) | có | **không** (property, server sở hữu) | đã biết từ hitbox: chỉ local |
| `Primitive.Flags` Anchored/CanCollide (446) | có | **không** trực tiếp; **gián tiếp** qua vị trí (xuyên tường thì người khác thấy bạn trong tường) | đã đo: Anchored bị solver bỏ qua |
| `Motor6D.Transform` | có | **không** (render-only) | đã xác nhận |
| `Humanoid.PlatformStand` (473) | có | **gián tiếp** — humanoid hết giữ thăng bằng → root đổ → người khác thấy cái đổ | suy luận + E4 |
| `Humanoid.WalkSpeed` (460) | có | **gián tiếp** — bạn chạy nhanh hơn, họ thấy vị trí | đã biết (speed mode property) |
| `Humanoid.HipHeight` (400) | có | **gián tiếp** — thân cao/thấp hơn, họ thấy vị trí | suy luận + E5 |
| `Humanoid.Health = 0` (396) | có | **có** — chết, khớp bị phá ở server, ai cũng thấy ragdoll | đã biết (suicide script) |
| `Humanoid.Sit` (474) | có | **gián tiếp** — tư thế ngồi là local, nhưng root hạ xuống → thấy | cần E7 |
| Chat / remote / tool | có | **có** (engine gửi đi) | đã biết |

Ghi chú: "gián tiếp" nghĩa là property không đi lên server, nhưng **hệ quả vật lý** của nó (vị trí/rotation của assembly) đi lên, vì client sở hữu network ownership của nhân vật mình. Đây là toàn bộ lý do fly/teleport hoạt động.

### 1.2 Vì sao limb pose không thấy được

`Motor6D` là khớp **motorized weld**: physics coi cả nhân vật là **một rigid body**, còn `Motor6D.Transform` chỉ là offset được áp **lúc render** trên máy đang mô phỏng. Nó không nằm trong state replication (replication gửi assembly CFrame + velocity, không gửi từng joint).

Hệ quả: mọi thứ "trông như animation" mà bạn thấy trên màn mình (tay vung, chân bước) là do **animator local** vẽ ra. Người khác nhận được đúng **một khối hộp** đang di chuyển/xoay.

### 1.3 Hệ quả thiết kế

- Stunt phải **đọc được bằng root**: lộn, xoay, nghiêng, bay, trượt, teleport-snap, rơi.
- Chi tiết "tay chân" chỉ để trang trí cho màn hình mình → đừng đặt nặng.
- Thứ tạo cảm giác "pro" không phải số khớp, mà là **timeline + easing + timing**. Một cú lộn 0.85 s có ease-in-out trông "thật" hơn 10 khớp xoay tuyến tính.

---

## 2. Nền kỹ thuật

### 2.1 `Primitive.Rotation` — layout và công thức

Roblox lưu rotation matrix **column-major, 9 float**:

```
[0..2] = right  (cột 0)
[3..5] = up     (cột 1)
[6..8] = back   (cột 2)   // LookVector = -back
```

Bằng chứng trong code hiện tại: `overlay.cpp` đọc `Camera.Rotation` và lấy forward bằng `lx = -r[2], lz = -r[8]` (tức `-cột 2`).

Công thức Euler → basis (order Y→X→Z, dùng cho toàn bộ preset):

```cpp
// yaw = quay quanh Y, pitch = quanh X, roll = quanh Z (radian)
// out = 9 float đúng thứ tự bộ nhớ: right, up, back
static void eulerToBasis(float yaw, float pitch, float roll, float out[9]) {
    const float cy = cosf(yaw),   sy = sinf(yaw);
    const float cx = cosf(pitch), sx = sinf(pitch);
    const float cz = cosf(roll),  sz = sinf(roll);
    // R = Ry * Rx * Rz (row-major, chỉ để suy ra cột)
    const float r00 =  cy*cz + sy*sx*sz;
    const float r01 = -cy*sz + sy*sx*cz;
    const float r02 =  sy*cx;
    const float r10 =  cx*sz;
    const float r11 =  cx*cz;
    const float r12 = -sx;
    const float r20 = -sy*cz + cy*sx*sz;
    const float r21 =  sy*sz + cy*sx*cz;
    const float r22 =  cy*cx;
    out[0]=r00; out[1]=r10; out[2]=r20;   // right
    out[3]=r01; out[4]=r11; out[5]=r21;   // up
    out[6]=r02; out[7]=r12; out[8]=r22;   // back
}
```

Kiểm tra nhanh (dùng khi unit test):
- yaw=0, pitch=0, roll=0 → right=(1,0,0), up=(0,1,0), back=(0,0,1) ✓
- yaw=90° → back=(1,0,0) (nhìn về +X), right=(0,0,-1) ✓
- pitch=+90° → up=(0,0,1), back=(0,-1,0) → mặt hướng lên trời (ngửa ra sau)

Quy ước dấu cho preset:
- **backflip** = pitch **âm** dần (−360°) → ngửa ra sau rồi lộn.
- **frontflip** = pitch **dương** (+360°).
- **cartwheel** = roll ±360°.
- **spin** = yaw tăng đều.

### 2.2 Position vs Velocity — số đo có sẵn (đừng đo lại)

Từ header `src/ovl/ovl_fly.cpp` (đo trên `version-11f155f3450f4c8e`):

| Lever | Tốc độ ghi | Kết quả |
|---|---|---|
| `AssemblyLinearVelocity` | 628 w/s (`Sleep(1)`) | **0.0 studs** |
| `AssemblyLinearVelocity` | ~1.0M w/s (`Sleep(0)`) | leo đủ, +40..46 studs/s ở target 40–45 |
| `Primitive.Position` | 140k w/s | chỉ jitter vài studs, bị revert 10–50 ms |
| `Primitive.Flags` Anchored | bất kỳ | dính trong memory nhưng solver bỏ qua |

Suy ra:
- **Dynamic mode bắt buộc chạy vòng `Sleep(0)`** giống fly. Ngân sách: 1 core trong lúc stunt chạy. Chấp nhận được vì stunt là burst 1–3 s.
- **Kinematic position = đắt và rung.** Chỉ dùng cho snap ngắn (strobe), không dùng để hover.
- **Hover** = ghi `velocity.y = 0` ở tốc độ cao (giống fly), không phải ghi position.
- **Rotation chưa đo** → E1 là việc đầu tiên ngày mai (§6).

### 2.3 Humanoid — các công tắc

| Field | Offset | Kiểu | Tác dụng |
|---|---|---|---|
| `PlatformStand` | 473 | bool (1 byte) | tắt giữ thăng bằng + tắt state machine đi bộ → pose xoay giữ nguyên, người đổ |
| `Sit` | 474 | bool | tư thế ngồi (local), root hạ xuống |
| `EvaluateStateMachine` | 468 | bool | 0 = humanoid không tự đổi state (đứng yên tuyệt đối) |
| `AutoRotate` | 465 | bool | 0 = không tự quay mặt theo hướng đi (cần cho moonwalk/piggy) |
| `HipHeight` | 400 | float | độ cao thân so với chân; đổi → thân nhô lên/hạ xuống |
| `HumanoidStateID` | 32 | int | ép state (0 = FallingDown, 10 = Freefall, 11 = Seated, 13 = Dead). Thí nghiệm E8 |
| `BreakJointsOnDeath` | 213 | bool | true (mặc định) → chết là rời khớp |
| `Health` | 396 | float | =0 → chết, ragdoll thật, ai cũng thấy |
| `HumanoidRootPart` | 1128 | Instance* | lấy HRP trực tiếp, khỏi `walkChildren` |

**Thứ tự bật/tắt chuẩn của một stunt tĩnh (statue/pose):**
```
PlatformStand = 1
AutoRotate    = 0
EvaluateStateMachine = 0     // optional, chặn state machine ghi đè
velocity      = 0            // mỗi tick
rotation      = pose         // 1 lần, hoặc 60/s nếu E1 nói không giữ
```

**Tắt (restore):** ghi lại `PlatformStand/AutoRotate/EvaluateStateMachine` = giá trị seed, zero velocity, ghi lại rotation seed.

### 2.4 Ngưỡng an toàn (server-side)

- Velocity đột ngột > ~200 studs/s dễ bị coi là teleport/fly → Error 268 hoặc kick tuỳ game. Giữ preset mặc định ≤ 120, cho slider tới 200.
- Rơi dưới `World.FallenPartsDestroyHeight` (532) → chết. Đừng làm preset bay xuống.
- Sau `Health=0` network ownership thường bị thu → **hết lái được**. Ragdoll "lái được" phải dùng `PlatformStand`, không dùng chết.
- Đổi `HipHeight` quá lớn (>10) làm humanoid mất đất → rơi tự do.

---

## 3. Kiến trúc module

### 3.1 File mới

```
src/ovl/ovl_stunt.h      API + struct StuntParams/PoseOut
src/ovl/ovl_stunt.cpp    state machine + writer thread + 14 preset
src/ovl/tab_stunt.cpp    drawTabStunt(MenuCtx&)
```

Phải thêm vào **cả** `build_fast.bat` và `build.bat` (danh sách nguồn overlay là tường minh — xem AGENTS.md).

### 3.2 API

```cpp
#pragma once
#include "sdk.h"

struct StuntParams {
    int   preset   = 0;      // index vào bảng preset
    float timeScale= 1.0f;   // 0.25 .. 2.0
    float amp      = 1.0f;   // biên độ (studs / độ)
    int   ragdoll  = 0;      // 1 = PlatformStand trong lúc chạy
    int   loop     = 1;
    int   hover    = 0;      // 1 = giữ độ cao hiện tại
    uint64_t targetPrim = 0; // HRP prim của player đích (piggy/orbit)
};

namespace Stunt {

// Gọi 1 lần sau openWrite(). Spawn writer thread (park Sleep(10) khi idle).
void start(Memory* m, const Offsets* o);

// Gọi mỗi frame từ main loop. `selfPrim` = HRP primitive của mình.
// `camAddr` để lấy forward cho preset bắn/né.
void publish(Memory& m, const Offsets& o, uint64_t selfPrim, uint64_t camAddr,
             const StuntParams& p, bool on);

// Tắt khẩn cấp: restore pose + zero velocity + trả humanoid flags.
void stop(Memory& m, const Offsets& o);

bool  active();
float writesPerSec();
const char* presetName(int i);
int   presetCount();
}
```

### 3.3 State machine (trong writer thread)

```
IDLE ──publish(on=1)──▶ SEED ──▶ RUN ──publish(on=0)──▶ RESTORE ──▶ IDLE
```

- **SEED**: đọc `position(3)`, `rotation(9)`, `velocity(3)`, `angular(3)`, các flag humanoid. Lưu lại. Ghi `PlatformStand/AutoRotate` nếu preset cần. Bỏ qua nếu seed fail (prim chết/place đổi).
- **RUN**: mỗi pass tính `PoseOut` từ `t = (qpcSeconds() - t0) * timeScale` rồi ghi.
- **RESTORE**: ghi lại seed (velocity 0, rotation seed, position seed, flags seed). Nếu prim đã đổi (respawn/place đổi) → bỏ restore, chỉ reset state.

Guard bắt buộc: nếu `selfPrim` đổi so với seed → về IDLE (nhân vật respawn, restore lên xác cũ là vô nghĩa và có thể ghi rác).

### 3.4 Vòng ghi (một pass)

```cpp
// dynamic: 2 write/tick (vel 3 float + ang 3 float)  -> rẻ
// kinematic: + rotation 9 float (36 B), + position 3 float (12 B)
Vec3 v = computeVelocity(t, p, seed);
Vec3 w = computeAngular(t, p, seed);
m.write(prim + o.primitiveVelocity,        v);   // 224
m.write(prim + o.primitiveAngularVelocity, w);   // 236  (offset mới)
if (needRot) m.write(prim + o.primitiveRotation, rot, 36);      // 176
if (needPos) m.write(prim + o.primitivePosition, pos);          // 212
```

Nhịp: `Sleep(0)` mỗi pass (giống fly). Đo `writesPerSec` bằng đếm pass / 1 ms, y như `Fly::writesPerSec`.

Ghi rotation **mỗi pass** cũng được (36 B thêm, không đáng kể so với 1M pass/s), nhưng nếu E1 cho thấy pose giữ được thì hạ xuống 1/64 pass để đỡ cache thrash.

### 3.5 Arbitration với module khác

| Module | Xung đột | Luật |
|---|---|---|
| Fly | cùng ghi velocity | **Stunt thắng**: nếu `Stunt::active()` thì `Fly::publish(..., false)` (falling edge zero 1 lần) và bỏ qua block fly |
| Speed (mode velocity) | cùng ghi velocity | tắt speed khi stunt chạy (block đã có `!flyApply`; thêm `&& !Stunt::active()`) |
| Teleport | ghi position 1 lần | teleport chạy trước stunt trong loop; stunt ghi đè ngay sau → **cấm teleport khi stunt bật** (UI disable) |
| Aim | chỉ camera/mouse | không xung đột |
| Hitbox | `Primitive.Size` + flags | không xung đột (stunt không đụng Size) |
| Ghost preset | `Flags` CanCollide bit | **cùng field với hitbox** → ghost phải lưu/khôi phục bit, và hitbox restore phải chạy sau stunt |

### 3.6 cfg keys (theo recipe AGENTS.md §Thêm cfg key)

```
stunt.enabled=0
stunt.preset=0
stunt.speed=1.0
stunt.amp=1.0
stunt.ragdoll=0
stunt.loop=1
stunt.hover=0
stunt.key=0
```

Default phải thêm vào chuỗi default trong `loadToggleKey()` (`ovl_cfg.cpp`) và parse trong `loadAimCfg()`. Clamp ngay sau parse: `preset ∈ [0, presetCount)`, `speed ∈ [0.25,2]`, `amp ∈ [0.1,3]`.

Lưu ý: **preset list thay đổi thì clamp cũng phải đổi** — đừng để `preset=13` trỏ ra ngoài mảng.

### 3.7 Wiring vào main loop (đúng thứ tự)

Chèn sau block fly (~`overlay.cpp:2278`), trước `wantShow`:

```cpp
// ---- Stunt: hand the program to ovl_stunt (writer thread) -------------
bool stuntApply = cfgStuntEnabled && g_stuntActive && haveWrite &&
                  !typingNow && fgPid == m.pid && !flyApply;
if (stuntApply) {
    uint64_t selfPrim = 0;
    for (auto& e : ents) { if (e.self && e.prim) { selfPrim = e.prim; break; } }
    Stunt::publish(m, o, selfPrim, camAddr, g_stuntParams, selfPrim != 0);
} else {
    Stunt::publish(m, o, 0, 0, g_stuntParams, false);
}
```

- `g_stuntActive` = trạng thái hotkey (rising-edge trên `0x8000`, giống speed/jump/fly — **không** dùng bit `&1`).
- `g_stuntParams` là biến global (extern trong `ovl_state.h`) do tab ghi; main chỉ đọc.
- Trong block fly: thêm `&& !Stunt::active()` vào điều kiện.
- Panic (`overlay.cpp` ~1875/1886) phải gọi `Stunt::stop(m, o)` cạnh `Fly::killVelocity`.
- Exit (~2793) gọi `Stunt::stop` trước `Fly::stop`.

### 3.8 Tab UI (index 8)

Theo recipe AGENTS.md §Thêm tab mới:

1. `tabNames` thêm `"Stunt"`, mảng `[9]`, vòng `< 9`.
2. Nhánh `else if (cfgMenuTab == 8) drawTabStunt(tabCtx);`
3. `ovl_cfg.cpp`: clamp `cfgMenuTab` `< 9`.
4. `MenuCtx` **không cần thêm field** (tab chỉ đọc `c.ents`, `c.haveWrite`, `c.nowTick`).
5. Nội dung tab:

```
[Stunt]                                    (hàng chọn preset, 2 cột)
( ) Backflip     ( ) Cartwheel   ( ) Beyblade
( ) Tube Man     ( ) Strobe      ( ) Statue
( ) Ragdoll Cannon ( ) Heli       ( ) Piggyback
( ) Orbit        ( ) Moonwalk    ( ) Ghost
( ) Spin Dash    ( ) Ground Pound

speed  [---o----] 1.00x     amp [---o---] 1.00
[x] ragdoll   [x] loop   [ ] hover    key: [G]
target: <dropdown player>            (chỉ piggy/orbit)
status: RUN 0.42M w/s   |  idle
[Run]  (hold-to-run nếu key=0)
```

- Preset chọn bằng `ImGui::Button` + highlight (`hitMark()` sau mỗi widget — bắt buộc, xem tab khác).
- `target` lấy từ `c.ents` (player rows), lưu prim.
- Status đọc `Stunt::active()` + `writesPerSec()`.

---

## 4. Thư viện preset (14 cái, công thức cụ thể)

Ký hiệu: `t` = giây (đã nhân timeScale), `φ = t / dur`, `ease(x) = x*x*(3-2x)`.
`P0` = position seed, `R0` = rotation seed, `fwd` = camera forward (ngang), `up = (0,1,0)`.

### 4.1 `flip` — Backflip (dynamic)
```
dur  = 0.85
ωx   = -2π / dur                  (rad/s, âm = ngửa ra sau)
vy   = +18                        (bật lên; hoặc để game tự jump rồi chỉ xoay)
hover= 0
```
Ghi `angular = (ωx, 0, 0)`, `linear = (0, vy, 0)`. Sau `dur` → kết thúc, restore.
Cảm giác: lộn một vòng rưỡi nếu để `loop=1` (mỗi vòng lại bật).

### 4.2 `cart` — Cartwheel (dynamic)
```
dur = 1.00
ωz  = +2π / dur
v   = right * 8                    (right lấy từ camera rotation, cột 0)
```

### 4.3 `spin` — Beyblade (dynamic)
```
ωy   = 14 rad/s
vy   = +2.0                       (nhích lên rồi hover)
hover= 1                          → sau 0.4 s ghi vy = 0
```
Người khác thấy một khối người xoay tít, lơ lửng 1–2 studs.

### 4.4 `tube` — Wacky Inflatable Tube Man (kinematic)
```
pitch = 18° * sin(2π * 1.6t)
roll  = 12° * sin(2π * 1.1t + 1.0)
yaw   = 0.5t                      (xoay chậm)
pos   = P0 + (0, 0.35*|sin(2π*1.6t)|, 0)
```
Cần `PlatformStand=1`, `AutoRotate=0`, `velocity=0`. Đây là preset "cháy" nhất về mặt hài — người khác thấy đúng một anh tube man.

### 4.5 `strobe` — Strobe Dance (kinematic position)
```
f    = 8 Hz                       (nhịp 125 ms)
k    = floor(t * f) & 3           (4 góc)
pos  = P0 + (±1.5, 0, ±1.5) theo k
yaw  = 90° * k
```
Snap cứng, không nội suy. Cần ghi position **mỗi pass** (position bị revert 10–50 ms → phải giữ nhịp 1M/s).
Rủi ro: trông như teleport spam → chỉ dùng ở place không có anti-cheat chặt.

### 4.6 `cannon` — Ragdoll Cannon (dynamic + PlatformStand)
```
PlatformStand = 1
v  = fwd * 95 + up * 12
ω  = (5, 7, 3)
dur = 1.6                          (sau đó zero velocity, để rơi)
```
Người khác thấy một cái xác bay tumbling qua map. Đây là "ragdoll thật" mà vẫn lái được (khác `Health=0`).

### 4.7 `heli` — Ragdoll Helicopter (dynamic + PlatformStand)
```
PlatformStand = 1
ωy = 18
vy = +1.2                          (giữ, đừng hover=1)
```
Xác xoay như cánh quạt, từ từ bay lên.

### 4.8 `piggy` — Piggyback (follow + Sit)
```
target = player đích (HRP prim)
pos    = targetPos + (0, 2.4, 0) + back(target) * -0.8
yaw    = yaw của target
Sit    = 1
```
Cần đọc rotation của target (9 float) để lấy `back`. Ghi position mỗi pass (target di chuyển).
Nếu target nhảy/rơi, bám theo. Đây là trò "nghịch" mạnh nhất về mặt xã hội.

### 4.9 `orbit` — Satellite (follow)
```
r   = 4 + amp
ω   = 1.5 rad/s
pos = targetPos + (cos(ωt)*r, 0.5, sin(ωt)*r)
yaw = atan2(targetPos.x - pos.x, targetPos.z - pos.z)   // luôn nhìn vào target
```

### 4.10 `statue` — Frozen Statue (kinematic)
```
PlatformStand = 1; AutoRotate = 0; EvaluateStateMachine = 0
rotation = euler(0, -35°, 0)        (hoặc pose bất kỳ)
velocity = 0
```
Treo lơ lửng, nghiêng, không nhúc nhích. Kết hợp `hover` để giữ độ cao.

### 4.11 `moon` — Moonwalk (dynamic)
```
WalkSpeed = 0                       (chân không bước → không animation đi bộ)
v = -fwd * 14                       (trượt lùi)
yaw = yaw(fwd)                      (mặt vẫn hướng tới)
AutoRotate = 0
```
Người khác thấy bạn trượt lùi mà mặt vẫn nhìn họ.

### 4.12 `ghost` — Ghost Walk (flags)
```
flags &= ~CanCollideBit (8)         (local)
```
Rồi đi bộ bình thường. Người khác thấy bạn xuyên qua tường/vật thể (vì vị trí vẫn replicate).
Restore: `flags |= bit` (hoặc trả seed). **Đụng field với hitbox** → xem §3.5.

### 4.13 `dash` — Spin Dash (dynamic)
```
ωy = 30
v  = fwd * 70
dur= 0.6
```
Cuộn tròn lao đi như Sonic. Có thể nối `cannon` để thành "spin dash → ragdoll launch".

### 4.14 `slam` — Ground Pound (dynamic)
```
0.0–0.15 s: vy = +12 (nhảy lên)
0.15–0.45 s: vy = -140, ω = (2,0,0)
0.45 s: vy = 0, position giữ (dừng đột ngột)
```

---

## 5. Combo "siêu cấp" (3 cái để demo)

Mỗi combo = một chuỗi preset, dùng chung state machine, mỗi bước có `dur` riêng.

### 5.1 `MISSILE` — Ragdoll Missile
```
cannon(0.9s) → heli(1.2s) → slam(0.5s)
```
Bắn xác qua map, xoay như chong chóng, rồi cắm đầu xuống đất.

### 5.2 `TORNADO` — Beyblade Tornado
```
spin(0.5s) → orbit(target, 2 vòng) + spin
```
Vừa xoay tít vừa bay vòng quanh nạn nhân.

### 5.3 `PHANTOM` — Phantom Dance
```
statue(0.6s) → strobe(1.0s) → tube(1.5s) → flip(0.85s)
```
Tượng bất động → nhảy teleport → tube man → lộn. Đây là cái "trailer" để quay video.

### 5.4 (optional) `KIDNAP` — bắt cóc
```
piggy(target) 3s → orbit(target) 2s
```
Cưỡi lên đầu rồi bay vòng quanh.

---

## 6. Kịch bản verify — làm TRƯỚC khi code preset

Không có 2 client thì không chứng minh được "người khác thấy". Nhưng vẫn phải làm E1/E3 để biết engine chấp nhận gì.

| # | Thí nghiệm | Cách làm | Kết quả cần ghi |
|---|---|---|---|
| **E1** | Rotation có giữ không? | `PlatformStand=1`, `AutoRotate=0`, ghi `Rotation` = pitch 45°, đọc lại sau 50 ms / 500 ms / 2 s | giữ / bị revert sau bao lâu |
| **E2** | Người khác có thấy rotation? | 2 client (alt trên điện thoại), chạy E1 | thấy nghiêng / không |
| **E3** | ω có tích phân mượt? | ghi `AssemblyAngularVelocity=(0,14,0)` ở 1M/s, nhìn màn mình + witness | xoay đều / giật / bị kick |
| **E4** | PlatformStand có replicate? | bật `PlatformStand=1` khi đứng yên, witness nhìn | thấy đổ / không |
| **E5** | HipHeight | ghi +2.0, witness nhìn | thấy nhô lên / không |
| **E6** | Velocity trần | tăng dần 40→200, ghi mốc bị kick/Error 268 | ngưỡng an toàn thực tế |
| **E7** | Sit | `Sit=1` khi không có ghế | ngồi không / không đổi |
| **E8** | HumanoidStateID | `EvaluateStateMachine=0` + `HumanoidStateID=0` (FallingDown) | nằm sấp / bị override |
| **E9** | `worldStepsPerSec` | đọc giá trị thật (offset 2176) | số bước/s (để chọn tần số ghi) |

Ghi kết quả vào chính file này (§6 bảng "Kết quả đo") — lần sau khỏi đo lại.

**Cách dựng witness nhanh:** điện thoại/PC thứ 2 vào cùng place, đứng ở khoảng cách 20–30 studs. Hoặc nhờ bạn. Không cần tool gì.

---

## 7. Gate checklist (theo AGENTS.md)

1. `build_fast.bat` → exit 0.
2. `ext_overlay.exe -aimtest` → exit 0, 0 dòng `FAIL`.
3. `ext_overlay.exe -srvtest 0` → `=== done (PASS) ===`.
4. `ext_overlay.exe -offtest offsets_fixtures` → `fails=0`; **chú ý**: thêm field vào `OFF_FIELDS` làm `keys` tăng (50 → 50+N) → phải bless lại `.expect` bằng `-offdump` (xem AGENTS.md §Update Roblox).
5. `ext_overlay.exe -offtest offsets_fixtures\negative` → phải `FAIL`, exit 1.
6. `build.bat` → toàn bộ tool exit 0.
7. `-diag` sandbox với `menu.tab=8` → tab Stunt render, hit counts 8 tab cũ **không đổi**.
8. `src/overlay.cpp`, `src/ovl/*`: 0 CR, không BOM.
9. Key set `overlay.cfg` trước/sau = identical nếu chỉ refactor; có 8 key `stunt.*` nếu là feature.
10. In-game: bật/tắt từng preset, panic restore sạch (không drift, không kẹt PlatformStand).

---

## 8. Rủi ro kỹ thuật + cách giảm

| Rủi ro | Triệu chứng | Giảm |
|---|---|---|
| Humanoid kéo upright | pose xoay bị bật về đứng | `PlatformStand=1` + `AutoRotate=0` + `EvaluateStateMachine=0` |
| Solver ghi đè rotation | pose giật/nhảy | E1 trước; nếu cần, ghi rotation mỗi pass |
| Position revert | hover rung, snap không dính | dùng velocity thay position; position chỉ cho strobe |
| Kick vì velocity lớn | Error 268 / disconnect | giữ ≤120 mặc định, slider max 200, test E6 |
| Stunt + fly cùng bật | bay loạn, velocity đánh nhau | `!Stunt::active()` trong block fly + `!flyApply` trong stunt |
| Hitbox + ghost cùng đụng Flags | mất collision vĩnh viễn | ghost lưu/khôi phục bit; hitbox restore chạy sau |
| Respawn giữa stunt | ghi vào xác cũ | prim đổi → về IDLE, không restore |
| Chết giữa stunt | ownership mất, restore vô nghĩa | check `Health` seed; nếu chết → bỏ restore |
| Place đổi | prim cũ là rác | `refreshEnts` đổi `e.self` → so sánh mỗi frame |
| Bật `PlatformStand` rồi crash | nhân vật nằm mãi | `stop()` trong panic + exit + `-diag` không bật stunt |
| Tab index lệch | click sai tab | đổi `[8]`→`[9]`, clamp `<9`, comment index |

---

## 9. Việc cần làm, theo thứ tự (ngày mai)

1. **E1 + E3** (§6) — 30 phút. Quyết định kinematic có khả thi không. Ghi kết quả vào §6.
2. Thêm 8 offset mới vào `offsets.h` + `fill()` + `OFF_FIELDS`:
   `primitiveRotation`, `primitiveAngularVelocity`, `humanoidPlatformStand`,
   `humanoidSit`, `humanoidEvaluateStateMachine`, `humanoidAutoRotate`,
   `humanoidHipHeight`, `worldGravity` (+ `humanoidRootPart` nếu muốn bỏ `walkChildren`).
   **Đánh dấu optional trong `validateOffsets`** — nếu không, cache cũ (không có key) sẽ bị reject.
3. `ovl_stunt.h/.cpp` khung: state machine + writer thread + `flip` + `spin` + `statue`.
4. `tab_stunt.cpp`: grid preset + speed/amp/ragdoll/key + status.
5. Wiring main loop + arbitration fly + panic/exit.
6. `build_fast` + gate 1–3, chạy thử 3 preset trong game (nhìn màn mình).
7. Thêm `cart`, `tube`, `strobe`, `moon`.
8. Thêm `cannon`, `heli`, `dash`, `slam`.
9. Thêm `piggy`, `orbit` (cần đọc rotation target).
10. Thêm `ghost` (flags + arbitration hitbox).
11. Combo `MISSILE` / `TORNADO` / `PHANTOM`.
12. (Optional, mở khoá limb) dò `Motor6D` (C0/C1/Transform) → thêm "limb flavor" local.
13. Gate đầy đủ §7 + witness test E2/E4/E5 với 2 client.

---

## 10. References

### 10.1 Offsets (từ `offsets_cache.json`, version-11f155f3450f4c8e)

Cột "struct" = đã có field trong `Offsets` chưa.

| Path | Offset | Struct | Ghi chú |
|---|---|---|---|
| `Primitive.Position` | 212 | `primitivePosition` ✓ | 3 float |
| `Primitive.Rotation` | 176 | ✗ **thêm** | 9 float, column-major |
| `Primitive.AssemblyLinearVelocity` | 224 | `primitiveVelocity` ✓ | 3 float |
| `Primitive.AssemblyAngularVelocity` | 236 | ✗ **thêm** | 3 float, rad/s |
| `Primitive.Size` | 452 | `primitiveSize` ✓ | 3 float |
| `Primitive.Flags` | 446 | `primitiveFlags` ✓ | bitfield |
| `PrimitiveFlags.Anchored` | 2 | `primitiveAnchoredBit` ✓ | solver bỏ qua |
| `PrimitiveFlags.CanCollide` | 8 | `primitiveCanCollideBit` ✓ | ghost dùng |
| `BasePart.Primitive` | 384 | `basePartPrimitive` ✓ | |
| `Humanoid.HumanoidRootPart` | 1128 | `humanoidHRP` ✓ | Instance* |
| `Humanoid.PlatformStand` | 473 | ✗ **thêm** | bool |
| `Humanoid.Sit` | 474 | ✗ **thêm** | bool |
| `Humanoid.EvaluateStateMachine` | 468 | ✗ **thêm** | bool |
| `Humanoid.AutoRotate` | 465 | ✗ **thêm** | bool |
| `Humanoid.HipHeight` | 400 | ✗ **thêm** | float |
| `Humanoid.HumanoidStateID` | 32 | ✗ | int, cho E8 |
| `Humanoid.BreakJointsOnDeath` | 213 | ✗ | bool |
| `Humanoid.Health` | 396 | `humanoidHealth` ✓ | float |
| `Humanoid.MoveDirection` | 316 | ✗ | Vector3, có thể dùng cho moonwalk |
| `Humanoid.FloorMaterial` | 384 | ✗ | int, biết đang chạm đất |
| `Model.PrimaryPart` | 584 | ✗ | Instance* |
| `Seat.Occupant` | 520 | ✗ | Instance* |
| `World.Gravity` | 544 | ✗ | float |
| `World.worldStepsPerSec` | 2176 | ✗ | int, E9 |
| `World.FallenPartsDestroyHeight` | 532 | ✗ | float |
| `Camera.Position` | 236 | `cameraPosition` ✓ | |
| `Camera.Rotation` | 200 | `cameraRotation` ✓ | 9 float, để lấy fwd/right |
| `Camera.CameraType` | 296 | ✗ | int |
| `Camera.CameraSubject` | 184 | ✗ | Instance* |
| `TaskScheduler.MaxFPS` | 176 | ✗ | int (uncap FPS) |
| `Animator.ActiveAnimations` | 2688 | ✗ | array, để tắt animator (nếu làm limb) |
| `AnimationTrack.Speed` | 196 | ✗ | float |
| `AnimationTrack.TimePosition` | 200 | ✗ | float |
| `AnimationTrack.IsPlaying` | 2632 | ✗ | bool |
| `AnimationTrack.Looped` | 213 | ✗ | bool |

### 10.2 Code refs trong repo

| Cần gì | Xem |
|---|---|
| Writer thread pattern (Sleep(0), rate, atomic handoff) | `src/ovl/ovl_fly.cpp` (header giải thích số đo) |
| Ghi velocity cho speed mode | `src/overlay.cpp:2103..2159` |
| Teleport: zero velocity + write position | `src/overlay.cpp:2162..2235` |
| Fly publish + falling edge | `src/overlay.cpp:2265..2279` |
| Hitbox: flags save/restore | `src/overlay.cpp:555..730` |
| `eulerToBasis` / pose | **mới**, `ovl_stunt.cpp` |
| Tab recipe | AGENTS.md §Thêm tab mới |
| cfg recipe | AGENTS.md §Thêm cfg key |
| Offset parse + `OFF_FIELDS` | `src/offsets.cpp:205..260` (dump), `:307..370` (fill) |
| `validateOffsets` (đừng làm key mới thành required) | `src/offsets.cpp` (cuối file) |
| Timing helpers | `src/ovl/ovl_core.h` (`qpcSeconds`, `qpcNs`) |
| `MenuCtx` | `src/ovl/ovl_tabs.h` |
| UI helpers (`chkInt`, `sliderFloatCfg`, `hitMark`, `keyBindWidget`) | `src/ovl/ovl_ui.h` |

### 10.3 Docs ngoài

- CFrame math / rotation matrix: https://create.roblox.com/docs/reference/engine/datatypes/CFrame
- Motor6D (vì sao Transform không replicate): https://create.roblox.com/docs/reference/engine/classes/Motor6D
- Humanoid (PlatformStand, HipHeight, states): https://create.roblox.com/docs/reference/engine/classes/Humanoid
- Network ownership (vì sao client sở hữu physics của nhân vật mình): https://create.roblox.com/docs/projects/network-ownership
- BasePart (Anchored/CanCollide là property, không phải physics state): https://create.roblox.com/docs/reference/engine/classes/BasePart
- HumanoidStateType enum (cho E8): https://create.roblox.com/docs/reference/engine/enums/HumanoidStateType
- RunService / physics stepping (để hiểu `worldStepsPerSec`): https://create.roblox.com/docs/reference/engine/classes/RunService

---

## Appendix A — Animation asset format (đã thử 2026-11-01, đừng làm lại)

Dùng khi muốn quay lại ý tưởng "tải animation về rồi replay".

```
GET https://assetdelivery.roblox.com/v1/asset/?id=<id>      (không cần auth với asset public)

Header : "<roblox!" + 6 byte signature (89 FF 0D 0A 1A 0A)   -> 14 byte
         uint16 version (0)
         uint32 classCount
         uint32 instanceCount
         uint8[8] reserved                                     -> chunk đầu ở offset 32
Chunk  : char[4] name ("INST","PROP","PRNT","META","END\0")
         uint32 compLen
         uint32 rawLen
         uint32 reserved
         data[compLen]   // LZ4 block thuần (không frame header, không zlib)
INST   : uint32 classId, uint32 nameLen, char[nameLen] name, uint8 isService, uint32 referent
PROP   : uint32 classId, uint32 nameLen, char[nameLen] propName, uint8 typeId, values[]
PRNT   : (uint32 referent, uint32 parent) * instanceCount     // parent 0xFFFFFFFF = root
```

Kết quả với `id=507771019` (R15 dance, 20.107 byte):
- classes: `KeyframeSequence`, `Keyframe`, `Pose`, `Motor6D`
- props: `Time` (Keyframe, float), `transform` / `preTransform` / `postTransform` (Motor6D, CFrame 12 float)
- joints (tên trong asset): `torso lhip lknee lankle lsole rhip rknee rankle rsole waist neck head lshoulder lelbow lwrist lpalm rwrist rpalm`
- `transform` chính là giá trị sẽ ghi vào `Motor6D.Transform` tại thời điểm keyframe.

File thử + script parse dở: `%TEMP%\rbxanim\507771019.bin`, `%TEMP%\rbxanim\parse.ps1`.
Chưa xong: LZ4 decoder đúng (offset trong stream vượt quá output hiện tại → cần state liên tục giữa các chunk, hoặc chunk data có prefix 4 byte chưa xác định). **Không cần** cho hướng stunt hiện tại.

## Appendix B — Đã SAI / đừng thử lại

| Ý tưởng | Vì sao sai |
|---|---|
| Ghi `Motor6D.Transform` để người khác thấy pose | render-only, không replicate |
| Ghi `Primitive.Size` để người khác thấy to hơn | Size là property, server sở hữu |
| Bật `Anchored` để đứng giữa không trung | bit dính nhưng solver bỏ qua (đo 25/25) |
| Ghi `Position` 1 lần/frame để hover | revert 10–50 ms, chỉ jitter |
| Ghi velocity ở `Sleep(1)` (~628/s) | 0 studs — phải `Sleep(0)` |
| `Health=0` để "ragdoll mà vẫn lái" | chết → mất network ownership → hết lái |
| Dùng bit `&1` cho hotkey | bị macro sweep ăn mất (xem checkpoint Tools tab) |
| Trộn kinematic và dynamic trong 1 preset | solver và writer đánh nhau → giật |

## Appendix C — Batch 2 (2026-11-03): preset 17..25 + altitude hold

Thực tế sau lần test đầu: 14 preset cũ nhìn **giống nhau** (yaw-only gần như vô hình
trên body đối xứng, không animation; mọi preset `PlatformStand` đều đổ thành "đống
thịt chìm nửa người dưới sàn"; ghost clear CanCollide → rơi xuyên sàn). Ba sửa đổi
gốc, áp dụng cho cả preset cũ:

1. **Altitude hold** (`PresetDef.alt`). Thread đọc `Primitive.Position.y` mỗi 1024
   pass, ghi `velocity.y = clamp((seed.y + alt − y) · 6, ±40)`. Chỉ dùng velocity vì
   position bị revert 10–50 ms (§2.2). Nhờ vậy preset "trên không" lơ lửng thật thay
   vì chui qua sàn. `alt = 0` = không giữ độ cao.
2. **Wobble cho preset xoay tròn**: `Beyblade` thêm `ang.x = 2.2·amp` (con quay
   nghiêng), `TORNADO` thêm `pitch = 0.28` vào basis đã ghi → khác hẳn `Orbit`.
3. **`Humanoid.HipHeight`** (offset optional 400): `Float` ramp `hip = seed.hip + 3·amp·k`
   → thân nhô lên khỏi chân. Restore trả lại `hipHeight` seed (đã test trong `-stunttest`).

| # | Tên | Cơ chế |
|---|-----|--------|
| 17 | Plank | kinematic `pitch = 90°` + yaw 1.2 rad/s, alt 3.0 → nằm ngang xoay như kim đồng hồ |
| 18 | Headspin | kinematic `pitch = 180°` + yaw 8 rad/s, alt 3.2 → lộn ngược xoay |
| 19 | Float | không xoay, ramp `HipHeight` 0→3·amp→0 trong 3 s, alt 1.0 |
| 20 | Figure 8 | position lemniscate (`x = 4·sin`, `z = 2·sin 2t`), yaw theo tangent |
| 21 | Helix | position vòng tròn bán kính `3·amp` + leo `0.8·amp·t`, yaw theo tiếp tuyến |
| 22 | Pendulum | position cung lắc (`L = 5·amp`, biên độ 0.85 rad), mặt hướng camera |
| 23 | Skipper | velocity nhảy `14·amp·sin(π·q/0.7)` + drift 8·amp + tumble `ang.x = −3` |
| 24 | Zero-G | drift `fwd·4·amp + up·1.2·amp` + tumble `ang = (2,1,3)` |
| 25 | Sink | `PlatformStand` + `velocity.y = −2.5·amp` → chìm từ từ có chủ đích |

`stunt.ragdoll`/`hover` vẫn giữ nguyên nghĩa (ép PlatformStand toàn bộ / ghim `vel.y = 0`);
altitude hold chạy **sau** `hover` nên preset có `alt` thắng khi `hover` bật.

Quy ước: index preset là hợp đồng của `stunt.preset` → **chỉ thêm cuối bảng**. Combo
được nhận diện bằng `comboTable(preset, n) != nullptr`, không còn mốc `kFirstCombo`,
nên thêm preset mới không cần sửa logic combo.

`-stunttest` hiện in `presets=26 samples=130` và kiểm thêm `alt` ∈ [−20, 20], `hip`
∈ [0, 20], restore trả `hipHeight`.

