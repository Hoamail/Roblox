# velext P1b — cơ chế speed/jump ĐÃ CHỐT (đọc runtime bằng admin) — 2026-10-17

Bước lớn này: lấy được **bảng offset runtime** của `velext_v1.exe` (chạy `ext_dumpdata.exe`
as admin, UAC), rồi decompile **đúng hàm ghi speed/jump**. Kết quả: H1 được xác nhận
ở mức mã nguồn, và fix đã được áp vào `ext_overlay.exe`.

---

## 1. Cách vào được

- `velext_v1.exe` (PID 26120) chạy **elevated** → `OpenProcess(VMR|QUERY_INFORMATION)` từ
  tiến trình thường trả `error 5`. Chạy tool qua UAC thì đọc được:

```
Start-Process cmd.exe -ArgumentList '/c','cd /d C:\Users\adminn\ext_project && ext_dumpdata.exe velext_v1.exe velext_dump > velext_dump_report.txt 2>&1' -Verb RunAs -Wait
```

Kết quả (đã lưu):
- `velext_dump_text.bin` (2.45 MB), `velext_dump_rdata.bin` (866 KB), `velext_dump_data.bin` (76 KB)
- `velext_dump_report.txt`, `velext_names.txt` (bảng name→offset do `resolve_names.py` giải ra)
- `syscall id a50(NtReadVirtualMemory)=63 (0x3f)`, `a20(NtWriteVirtualMemory)=58 (0x3a)`

## 2. velext là tool gì

- Tên: **VELOCITY** (`DAT_140331cd8 -> "VELOCITY"`), `https://realvelocity.xyz/`
- Nguồn offset: `https://realvelocity.xyz/offsets/roblox/external/`
- Vá bản build: `"Velocity - Update Available"`.

## 3. Bảng property→offset (velext resolve theo tên)

Cặp `(tên, slot)` trong `FUN_14010abc0`. Các entry liên quan nhân vật:

| Property (velext) | Slot | Offset | theo feed |
|---|---|---|---|
| `Humanoid.WalkSpeed` | `DAT_14032d218` | **0x1CC (460)** | `Humanoid.Walkspeed` = 460 |
| `Humanoid.WalkSpeedShadow` | `DAT_14032d200` | **0x3A4 (932)** | `Humanoid.WalkspeedCheck` = 932 |
| `Humanoid.JumpPower` | `DAT_14032d228` | 0x1A0 (416) | 416 |
| `Humanoid.Health` | `DAT_14032d220` | 0x18C (396) | 396 |
| `Humanoid.MaxHealth` | `DAT_14032d210` | 0x1A4 (420) | 420 |
| `Humanoid.MoveDirection` | `DAT_14032d418` | 0x13C (316) | 316 |
| `Humanoid.HipHeight` | `DAT_14032d208` | 0x190 (400) | 400 |
| `Humanoid.PlatformStand` | `DAT_14032d258` | 0xC0 (192) | PlatformStand = 473 (bit index?) |
| `BasePart.Primitive` | `DAT_14032d1f8` | 0x180 (384) | |
| `Primitive.Velocity` | `DAT_14032d1e8` | 0xE0 (224) | |
| `Primitive.CFrame` | `DAT_14032d1e0` | 0xB0 (176) | |
| `Player.Character` | `DAT_14032d268` | 0x298 (664) | ModelInstance |
| `Players.LocalPlayer` | `DAT_14032d298` | 0x120 (288) | 288 |
| `Workspace.CurrentCamera` | `DAT_14032d288` | 0x4A8 (1192) | 1192 |

**Chỉ có duy nhất một property dạng "Shadow": `Humanoid.WalkSpeedShadow`.** Không có
shadow cho JumpPower / Velocity / CFrame.

## 4. Đường ghi SPEED — `FUN_1400db390` (chốt H1)

```c
void FUN_1400db390(longlong *ctx, longlong hum, float value)
{
  // 1) Humanoid + 0x3A4  (WalkSpeedShadow)
  NtWriteVirtualMemory(*ctx, hum + DAT_14032d200 /*0x3A4*/, &value, 4, &r);
  // 2) Humanoid + 0x1CC  (WalkSpeed)
  NtWriteVirtualMemory(*ctx, hum + DAT_14032d218 /*0x1CC*/, &value, 4, &r);
  // 3) Humanoid + 0x3A4  (WalkSpeedShadow) — ghi lại lần nữa
  NtWriteVirtualMemory(*ctx, hum + DAT_14032d200 /*0x3A4*/, &value, 4, &r);
}
```

Cùng **một giá trị 4 byte**, ghi shadow → walkspeed → shadow. Hàm này được lưu trong
`.data` tại `DAT_14034d968` → nó là **setter callback** của property `Humanoid.WalkSpeed`.

## 5. Đường ghi JUMP — `FUN_1400db4a0`

```c
NtWriteVirtualMemory(*ctx, hum + DAT_14032d228 /*0x1A0 JumpPower*/, &value, 4, &r);
```

**Một lệnh duy nhất**, không có field cặp. (Khớp runtime: JumpPower giữ 50.000 vì slider
của user đang đặt đúng 50 = default → không thấy thay đổi.)

## 6. Vòng lặp mỗi frame — `FUN_1400dba30` + `FUN_1400dbc70`

- Duyệt danh sách entity nội bộ của velext (`stride 0x440`); mỗi entry có
  `+0x1d0` = Humanoid trong Roblox, `+0x420`/`+0x424` = float lưu speed/jump.
- `FUN_1400dbc70` (chạy mỗi frame):
  - nếu `DAT_140334430` (speed enable) và `DAT_140334431` → `FUN_1400db390(ctx, hum, DAT_140334434)`
  - nếu `DAT_140334420` (jump enable) và `DAT_140334421` → ghi `DAT_140334424` vào `hum+0x1A0`
- Manual mode (`DAT_140335690 == 0`): ghi thẳng giá trị UI `DAT_140334434` mỗi frame.
- Tooltip trong tool: *"How fast you walk. **Restored when you turn it off**."* /
  *"How high you jump. Restored when you turn it off."* → nó lưu giá trị gốc để restore.
- Config keys: `ply.walkSpeed`, `ply.speedOn`, `ply.jumpPower`, `ply.jumpOn`, `ply.flySpeed`,
  `ply.noclip`, `ply.antiFling`.

### Giải luôn bí ẩn 49.967701
Slider của VELOCITY là float liên tục, label hiển thị `%.0f` ⇒ **49.967701 hiện ra thành
"50"**. Vậy giá trị velext ghi thật là **49.967701**, không phải 50.0 — khớp chính xác
dữ liệu sampler (`49.967701` ↔ `23.000`). Sawtooth = velext ghi mỗi frame vs game
(client-side từ server stat) set lại 23 qua engine setter (nên shadow luôn khớp).

Và `ext_wsfast --sec 3` thấy đứng yên 23.000 = lúc đó **game window không foreground**
(cả hai tool đều gate theo foreground).

## 7. Vì sao overlay mình bị 268 — kết luận

Overlay (`src/overlay.cpp` mode 0) ghi **chỉ** `Humanoid+0x1CC` bằng `WriteProcessMemory`.
Cặp `0x3A4` (WalkSpeedShadow) giữ nguyên giá trị game (23) ⇒ **lệch cặp**
(WalkSpeed=50, Shadow=23).

Bằng chứng WPM không phải nguyên nhân: overlay đã WPM cho hitbox / camera / velocity mà
không bị 268; và mode 1 (velocity, WPM vào `Primitive.Velocity`) chạy được. ⇒ **268 do
field bị kiểm tra (cặp WalkSpeed/WalkSpeedShadow), không phải do API.**

## 8. Fix đã áp vào overlay

- `src/offsets.h`: thêm `int humanoidWalkSpeedShadow`.
- `src/offsets.cpp`: bind `Humanoid.WalkspeedCheck` (theo) + fallback `0x3A4`.
- `src/overlay.cpp` (mode 0): khi ghi WalkSpeed thì ghi bộ 3 y như velext
  **shadow → walkspeed → shadow** cùng giá trị.
- Rebuild: `ext_overlay.exe` (6010101 bytes, 2026-10-17 19:32).
- Jump giữ nguyên (ghi `Humanoid+0x1A0`), đúng như velext.

## 9. Test plan (cần account phụ + place riêng)

1. Vào baseplate riêng bằng **account phụ**, mở `ext_overlay.exe`.
2. Bật `ply.mode = 0` (Humanoid property), `ply.speedval = 50`, `ply.jumpval = 100`.
3. Verify bằng `ext_watch.exe --sec 8`: phải thấy `ws` = 50 và **`0x3A4` = 50** (0 mismatch),
   giữ được ~50 khi đang đi.
4. Nếu **vẫn 268**: nghi vấn còn lại là API-level (Hyperion bắt `WriteProcessMemory`).
   Bước tiếp: chuyển `Memory::write` sang **direct syscall NtWriteVirtualMemory**
   (`mov r10,rcx; mov eax,id; syscall`) như velext — id resolve từ ntdll stub.
   (Nhưng nhiều khả năng không cần, vì các WPM khác đang chạy êm.)

## 10. Câu hỏi còn mở

| # | Câu hỏi | Trạng thái |
|---|---|---|
| 1 | Vì sao velext ghi shadow **3 lần** (trước + sau)? | Chắc để chống engine ghi đè ở giữa; chưa chứng minh bắt buộc |
| 2 | `DAT_140335690` (auto/manual mode) dùng khi nào? | `!=0` → lấy speed từ entity `+0x420` |
| 3 | Game hiện tại? (WalkSpeed là stat động 13/15/23/35) | cần placeId từ wssample.log |
| 4 | Hyperion có bắt direct syscall không? | velext sống sót ⇒ không (ít nhất ở thời điểm này) |

## 11. Tool & file mới của bước này

| File | Việc |
|---|---|
| `ext_dumpdata.exe` (admin) | dump `.text/.rdata/.data` + syscall id của process đích |
| `velext_dump_*.bin`, `velext_dump_report.txt` | dump thô của velext |
| `resolve_names.py`, `velext_names.txt` | giải 135 cặp name→offset từ `.data` |
| `src/wsfast.cpp` / `ext_wsfast.exe` | sampler ~229kHz bắt transient + mismatch |
| `src/watch.cpp` / `ext_watch.exe` | sampler ~15–21kHz + CSV + bin 250ms |

Đã thêm hết vào `build.bat`.

---

# PHẦN 2 — "làm hết mọi thứ như velext" + VERIFY (2026-10-17 19:40)

## 12. Đã implement đủ bộ như velext

### 12.1 Direct syscall write (`src/memory.h`)
`Memory::openWrite()` gọi `resolveWriteSyscall()`: đọc id thẳng từ stub ntdll
(`NtWriteVirtualMemory`) theo pattern `4C 8B D1 B8 <id> 0F 05 C3`, fallback quét
`B8 <id> 0F 05` trong 64 byte đầu (Hell's/Halo's Gate). Stub asm của mình:

```asm
ntWriteSyscall:
    movl g_ntWriteId(%rip), %eax
    movq %rcx, %r10
    syscall
    ret
```

`write()` → `writeBytes()`: ưu tiên syscall, fallback `WriteProcessMemory`.
⇒ Mọi write của overlay (speed/jump/hitbox/camera/velocity) **không đi qua ntdll**,
không để lại dấu cho hook của anti-cheat — đúng thiết kế của velext.

### 12.2 Ghi mỗi frame, không phải write-on-change
Velext (manual mode) ghi mỗi frame; overlay giờ cũng vậy (bỏ điều kiện
`fabsf(cur - target) > 0.01`).

### 12.3 Restore-on-disable
Velext: *"Restored when you turn it off."* Overlay giờ lưu giá trị gốc
(WalkSpeed + shadow + JumpPower) khi bật và **ghi trả lại khi tắt** (hoặc khi rời
mode 0). Respawn/model-swap ⇒ quên giá trị cũ.

### 12.4 Config `overlay.cfg` đã bật
```
ply.speed=1     ply.speedval=50
ply.jump=1      ply.jumpval=90
ply.mode=0
```

## 13. Kết quả verify

### 13.1 Verify đường syscall — `ext_ntcheck.exe` (không đổi gì trong game)
```
resolved NtWriteVirtualMemory id = 58 (0x3a)   directWrite=1
direct syscall -> addr 0x1 : NTSTATUS 0x8000000d
ntdll stub     -> addr 0x1 : NTSTATUS 0x8000000d   MATCH direct-call ABI OK
```
⇒ id **58** khớp đúng dump của velext (`a20 = 58`), và **ABI khớp tuyệt đối** với
stub ntdll. Ghi thử 3 lệnh với giá trị đang có: status `0/0/0`, giá trị không đổi,
cặp `0x1CC`/`0x3A4` vẫn in-sync.

### 13.2 Verify overlay thật — `ext_watch.exe --sec 8` (499.455 mẫu @62kHz)
```
distinct WalkSpeed:  0.000000 x2412 | 23.548388 x189722 | 50.000000 x307320
mismatches(ws!=chk) = 172
  (50.0, 23.5484) x90   (23.5484, 50.0) x78
  (0.0, 50.0) x2  (12.5, 50.0) x1  (50.0, 0.0) x1
```
Đọc:
- Overlay **ghi 50.000000**, game revert **23.548388** ⇒ sawtooth giống hệt velext.
- **23.548388 = `0x41bc6319`** — đúng hằng số `DAT_14032d47c` của velext, tức
  **base WalkSpeed thật của game** (không phải 23.0 như mẫu cũ).
- 2412 mẫu `ws==0`: trong đó **2410 mẫu `chk==0`** ⇒ **game** set cả cặp về 0
  (state chết/respawn); lúc đó overlay bỏ qua (guard model/Health) nên không ghi.
- 172 mismatch (0.03%) chia **cả hai chiều** ⇒ đó là **cửa sổ ghi 1–2 µs**:
  (a) cửa sổ giữa 2 lệnh trong bộ 3 của chính overlay, và (b) engine setter của game
  cũng ghi 2 field tuần tự. Velext có y hệt (chỉ là sampler 31ms trước đây không bắt được).
  ⇒ Không phải lỗi, mà là bản chất của cặp field.

### 13.3 Trạng thái
`ext_overlay.exe` (PID 15640) đang chạy, mode 0, speed 50, jump 90. **Chờ kết quả
in-game của user (có bị 268 hay không).**

### 13.4 KẾT QUẢ USER (2026-10-17 20:0x)
> "ngon quá rồi mày ơi không bị kick nữa, dù vẫn bị revert nhưng mà speed vẫn ổn.
> jump thì không đổi thật"

⇒ **Fix `WalkSpeedShadow` THÀNH CÔNG: 268 biến mất, speed chạy.** Còn **jump không đổi**
(đúng như dự đoán: game `UseJumpPower=0`).

---

# PHẦN 3 — FIX JUMP (2026-10-17 20:1x)

## 14. Vì sao jump của velext không có tác dụng ở game này
- `Humanoid.UseJumpPower` (theo: 477 = **0x1DD**) = **0** cho mọi player.
- velext **chỉ** ghi `JumpPower` (**0x1A0**) — mà game này bỏ qua JumpPower, dùng
  `JumpHeight` (theo: 412 = **0x19C**): `v0 = sqrt(2 * Gravity * JumpHeight)`.
- ⇒ velext ghi JumpPower=50 = **đúng bằng default** của game ⇒ vô hình. Đó là lý do
  "jump không đổi".

## 15. Kiểm tra JumpHeight có field "shadow" không (tránh 268 quay lại)
Tool `ext_scanfields.exe` (read-only) quét toàn bộ Humanoid 0x0..0x1000 tìm mọi slot
4-byte giữ bit-exact các giá trị quan tâm:

| giá trị | các offset tìm thấy |
|---|---|
| WalkSpeed (15.0) | `0x16C`, **`0x1CC`**, **`0x3A4`** (shadow), `0xF6C` |
| JumpPower (50.0) | **`0x1A0`**, `0xFA0` |
| JumpHeight (7.2) | **`0x19C`**, `0xF9C` |

Nghi vấn block `+0xE00`. Kiểm tra dứt điểm: `class(hum+0xE00) = Humanoid`,
`name(hum+0xE00) = Humanoid`, và **không trùng Humanoid của player nào** trong
Players (liệt kê đủ 9 player) ⇒ đó là **Humanoid của NPC** nằm liền kề trong
allocator, **KHÔNG phải mirror**.

⇒ **Kết luận: chỉ WalkSpeed mới có field check (`0x3A4`). JumpHeight/JumpPower
KHÔNG có shadow** ⇒ ghi thẳng là an toàn (đúng cơ chế engine setter).

## 16. Đã implement
- `src/offsets.h/.cpp`: thêm `humanoidJumpHeight` (theo `JumpHeight`, fallback
  `0x19C`) + `humanoidUseJumpPower` (theo `UseJumpPower`, fallback `0x1DD`).
- `src/overlay.cpp`: thêm `ply.jumpmode`:
  | giá trị | hành vi |
  |---|---|
  | **0 (default)** | auto: ghi **cả** `JumpPower` (= `ply.jumpval`) và `JumpHeight` = `jumpval² / (2·196.2)` |
  | 1 | chỉ `JumpPower` (đúng y velext) |
  | 2 | **velocity impulse** khi bấm Space (không đụng property nào; có inf-jump) |
- Một slider `ply.jumpval` giữ nguyên nghĩa "JumpPower" như velext, tự quy đổi sang
  JumpHeight tương đương (vd 90 → 20.6422) để game nào cũng nhảy như nhau.
- Restore-on-disable giờ phủ cả `JumpHeight` (`g_plyRestJH`).
- `overlay.cfg`: `ply.speed=1`, `ply.jump=1`, `ply.jumpmode=0`.
- Rebuild `ext_overlay.exe` (PID 17972).

## 17. Verify jump (read-only)
`ext_jumpchk.exe --sec 4` (236kHz) khi overlay chạy:
```
JumpPower(0x1A0) distinct values (1):  90.000000   x945505
JumpHeight(0x19C) distinct values (1): 20.642202   x945505
velocity.y range: min=0.00 max=0.00
```
⇒ Ghi đúng, **ổn định tuyệt đối, KHÔNG bị game revert** (khác WalkSpeed) ⇒ không có
sawtooth, không có cửa sổ mismatch nào để Hyperion bắt.
- Baseline game: JumpHeight 7.2, g=196.2 ⇒ `v0 = 53.15`; với 20.6422 ⇒ `v0 = 90`.

## 18. KẾT QUẢ CUỐI (2026-10-17 20:2x) — XONG
> "tuyệt vời, không bị kick cũng không bị văng, jump và speed đều hoạt động"

⇒ **Cả speed lẫn jump chạy, KHÔNG kick, KHÔNG văng.** Mục tiêu ban đầu đạt được.

---

# TỔNG KẾT — overlay giờ "làm hết mọi thứ như velext"

| Hạng mục | Velext | Overlay mình | Trạng thái |
|---|---|---|---|
| Cơ chế ghi memory | direct syscall `NtWriteVirtualMemory` (tự giải id, Hell's/Halo's Gate) | y hệt: asm `mov eax,id; mov r10,rcx; syscall; ret`, id **58** resolve từ stub ntdll, fallback WPM | ✅ ABI khớp tuyệt đối stub ntdll |
| Speed | `shadow(0x3A4) → WalkSpeed(0x1CC) → shadow(0x3A4)`, mỗi frame | y hệt | ✅ không 268 |
| Jump | chỉ `JumpPower(0x1A0)` | auto: `JumpPower` + `JumpHeight(0x19C)` = `v²/2g` (hoặc mode 1 = y velext, mode 2 = velocity) | ✅ **hơn velext** (velext vô dụng ở game `UseJumpPower=0`) |
| Restore khi tắt | có ("Restored when you turn it off") | có, phủ cả WalkSpeed+shadow+JumpPower+JumpHeight | ✅ |
| Ghi mỗi frame | có | có (bỏ write-on-change) | ✅ |
| Offsets | không hardcode, feed động | feed `realvelocity.xyz` + `offsets_cache.json` + fallback | ✅ |

### Tool phụ trợ đã thêm (đều read-only trừ ntcheck `--write`)
| tool | việc |
|---|---|
| `ext_ntcheck.exe` | verify đường syscall (id + so NTSTATUS với stub ntdll), `--write` = test ghi giá trị-không-đổi |
| `ext_scanfields.exe` | quét Humanoid tìm field "shadow" của mọi giá trị quan tâm + nhận diện neighbor |
| `ext_jumpchk.exe` | sampler JumpPower/JumpHeight/WalkSpeed/shadow + peak `velocity.y` (v0) mỗi cú nhảy |
| `ext_watch.exe` | sampler cặp ws/shadow tốc độ cao (phát hiện mismatch) |

Đã thêm hết vào `build.bat`.

### Cấu hình chốt (`overlay.cfg`)
```
ply.mode=0        # 0 = property (đã fix 268), 1 = velocity
ply.speed=1  ply.speedval=50
ply.jump=1   ply.jumpval=90   ply.jumpmode=0   # 0 auto, 1 JumpPower-only, 2 velocity impulse
```
Hotkey: **B** bật/tắt speed, **N** bật/tắt jump, `[`/`]` chỉnh speed, `;`/`'` chỉnh jump, **K** ẩn/hiện HUD.

### Nếu muốn làm tiếp (chưa làm)
1. Đưa `ply.jumpmode` / các giá trị lên HUD dạng text cho dễ nhìn.
2. Restore-on-disable cho **tất cả** feature khác (hitbox, camera...), không chỉ speed/jump.
3. Auto-pick `ply.jumpval` theo default của game (đọc `JumpHeight` gốc rồi nhân hệ số).
4. Đổi tên `WalkspeedCheck` → `WalkSpeedShadow` trong `src/dumpdata.cpp` cho khỏi lẫn.



