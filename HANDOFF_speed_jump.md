# HANDOFF — speed/jump RE (velext_v1 = "VELOCITY") — 2026-10-17

Trạng thái: **P0 xong**, **P1a (runtime) đang chạy**, đã dựng xong bộ instrument.
Người đọc tiếp: đọc mục 5 trước (việc cần làm ngay), rồi mục 2–4 để có ngữ cảnh.

---

## 1. Mục tiêu

Đưa speed + jump vào `ext_overlay.exe` **không bị Roblox kick Error 268**
("You have been kicked due to unexpected client behavior").
Nguồn tham chiếu: `velext_v1.exe` (tên sản phẩm trong UI: **VELOCITY**) đang chạy
speed/jump hack mà không bị kick.

---

## 2. P0 — Static RE `velext_v1.exe` (đã xong)

Chi tiết đầy đủ: `velext_P0_findings.md`. Tóm tắt:

| # | Phát hiện | Ý nghĩa |
|---|-----------|---------|
| 1 | Không import API memory nào (no RPM/WPM/OpenProcess/VirtualAlloc/Nt*) | Không dùng đường kernel32→ntdll bị Hyperion hook |
| 2 | Đọc/ghi memory Roblox bằng **direct syscall** cắm trong `.text`: `FUN_14022fd20` (`MOV GS:[0x16B4],RCX;RET`) + `FUN_14022fd2a` (`MOV EAX,GS:[0x16B4];MOV R10,RCX;SYSCALL;RET`) | 561 call-site |
| 3 | Syscall id **resolve runtime** từ ntdll: PEB Ldr walk (`FUN_1400636e0`) + parse export + đọc `mov eax,imm32` tại stub+4, fallback quét stub lân cận ±0x20 (Hell's/Halo's Gate, `FUN_140093320`) | Không hardcode, sống qua update |
| 4 | `DAT_140332a50` = **NtReadVirtualMemory** (147 call-site); `DAT_140332a20` = **NtWriteVirtualMemory** (60 call-site) | 2 nguyên hàm I/O duy nhất cần |
| 5 | Offset Roblox **không** hardcode; bảng `.data` `0x14032D000–0x14032D900` khởi tạo `0xDEADBEEF`, điền runtime | Muốn biết velext ghi offset nào phải đọc runtime — **thử P1a bị chặn** (xem 4.1) |
| 6 | Truy cập property **theo tên** class/property (`"Player"`,`"Camera"`,`"Humanoid"`, `"WalkSpeed"`,`"JumpPower"`,`"Velocity"`,`"Position"`,`"MoveDirection"`,`"CFrame"`) trong `FUN_1400cd7d0` & `FUN_14011a670` | Name-driven, không phải bảng offset cứng |
| 7 | **H1 (velext ghi cặp `WalkSpeed`+`WalkspeedCheck`) KHÔNG được xác nhận bằng static** — các hit 0x1CC/0x3A4 trong file là false positive (bảng hằng float/matrix trong `FUN_140176c10`) | Xem mục 4.3 (runtime lại cho bằng chứng mạnh) |
| 8 | Có HTTP client (WININET) nhưng không có URL/AOB-signature plaintext | Nguồn offset vẫn mở |

---

## 3. Runtime — điều đã quan sát được (trong game thật)

Game hiện tại (self `khoibsalt`, UserId 10858856827, HP 450) là game có **WalkSpeed như một stat động**.

### 3.1 WalkSpeed và `Humanoid+0x3A4` luôn khớp nhau
- `ext_wssample` 200 mẫu @31ms: `0x3A4 != 0x1CC` → **0/200**
- `ext_wsfast` 691.532 mẫu @229kHz: → **0** mismatch
- `ext_watch` ~890k mẫu @15–21kHz: → **0** mismatch

⇒ Slot `Humanoid+0x3A4` (theo dữ liệu công khai: `WalkSpeedCheck`) **bám sát** `+0x1CC`
ở mọi thời điểm quan sát, kể cả khi giá trị nhảy 23 ↔ 49.9677 liên tục (~12 lần/giây).

### 3.2 Game tự ghi WalkSpeed
Các giá trị bắt gặp: **13.000, 15.000, 23.000, 35.000, 49.967701** (số nguyên cho
mọi player khác — Zzz0823zzZ=23, OvO333x=23, to58d36=13, THERoRoTAE11=23, koikaze9=35).
⇒ Đây là game mà WalkSpeed là stat; **game tự set**, nên test "ai ghi WalkSpeed" trong
game này rất nhiễu. Giá trị `49.967701` (=0x4247DEED, KHÔNG phải 50.0f=0x42480000) nổi bật
vì không giống số nguyên của game → nghi là giá trị sau khi bị game clamp/biến đổi.

### 3.3 Jump không đổi qua property
`JumpPower = 50.000` và `JumpHeight = 7.200`, `UseJumpPower = 0` cho **mọi** player
(giống hệt self), dù jump hack đang bật với JumpPower slider = 50 (trùng default).
⇒ Chưa quan sát được jump hack ghi field nào (cần test slider khác default — xem 5.2).

### 3.4 Lúc "không hoạt động": WalkSpeed đứng yên
`ext_wsfast --sec 3` (691k mẫu): WalkSpeed = 23.000 không đổi suốt 3s, 0 mismatch.
⇒ Có lúc velext **không ghi gì** (nghi: chỉ ghi khi cửa sổ game focus / khi di chuyển).

### 3.5 Đo tốc độ mặt đất
`ext_watch` (bin 250ms) bắt được di chuyển: khi WalkSpeed=15.00 thì
`maxMeas ≈ 31–39` studs/s, `max|v| ≈ 20`. (Cần đo lại có kiểm soát — meas đơn lẻ
có thể dính teleport/snap.)

---

## 4. P1a — thử đọc bảng offset runtime của velext

### 4.1 Kết quả: BỊ CHẶN
`ext_dumpdata.exe velext_v1.exe` → `OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_INFORMATION)`
trả **error 5 (Access denied)**, kể cả `PROCESS_QUERY_LIMITED_INFORMATION`.
⇒ velext (PID 26120) chạy elevated/protected. Muốn đọc phải chạy tool **as admin**
(`Start-Process -Verb RunAs`), hoặc đọc từ driver. Chưa làm (cần user quyết định).

### 4.2 Hướng thay thế đang dùng
Thay vì đọc process velext, đọc **phía Roblox**: so giá trị game set (baseline) với
giá trị khi hack bật → suy ra velext ghi gì. Đây là hướng 5.2.

### 4.3 Trạng thái giả thuyết
- **H1 (phải ghi kèm `+0x3A4` khi ghi `+0x1CC`)**: runtime cho thấy 2 field luôn khớp.
  Chưa phân biệt được (a) engine tự mirror, hay (b) writer ghi cả cặp. Cần test 5.2:
  nếu tự tay ghi mỗi `0x1CC` mà `0x3A4` lệch → H1 đúng.
- **H4 (velext dùng velocity)**: chưa loại trừ. `meas` > WalkSpeed một lần (31–39 vs 15)
  là gợi ý yếu, cần đo có kiểm soát.

---

## 5. VIỆC CẦN LÀM NGAY

### 5.1 Thí nghiệm A (1 phút, cần user) — JumpPower là kênh sạch
`JumpPower` hiện = 50.000 cho mọi player và **không bị game đổi**.
1. Trong VELOCITY: đặt **JumpPower = 37.25** (giữ Jump hack bật), và **WalkSpeed = 37.25**
   (giữ Speed hack bật).
2. Click vào cửa sổ game (để focus), đứng yên.
3. Báo "xong" → chạy:
   ```
   ext_watch.exe --sec 10 --csv watch_jp.csv
   ```
4. Đọc: nếu `Humanoid+0x1A0` (JumpPower) = 37.25 → velext ghi **property**.
   Nếu vẫn 50.0 → velext **không** dùng JumpPower (nghi velocity/impulse/macro).
   Với WalkSpeed: nếu 37.25 xuất hiện (dù chỉ 1 mẫu) → velext ghi raw slider value
   vào `+0x1CC`; nếu chỉ 23/49.9677 → có biến đổi/clamp.

### 5.2 Thí nghiệm B — bật/tắt hack để có baseline
1. Bỏ tick cả 2 hack, click game, báo "off" → chạy `ext_watch --sec 8 --csv watch_off.csv`.
2. Bật lại cả 2, giữ W đi bộ 8s, báo "on" → chạy `ext_watch --sec 8 --csv watch_on.csv`.
So sánh: WalkSpeed, `meas` (tốc độ mặt đất thật) và `|AssemblyLinearVelocity|`
⇒ quyết định **property vs velocity** (nếu meas ≈ 50 khi WalkSpeed = 23 → velocity).

### 5.3 Thí nghiệm C (quyết định H1) — tự ghi thử
Dùng `ext_wpmtest.exe` / viết tool riêng: ghi **mỗi** `Humanoid+0x1CC = X` (không đụng 0x3A4),
rồi `ext_wsfast` xem `0x3A4` có lệch không và **có bị 268 không**.
⚠️ Chỉ trên **account phụ + place baseplate riêng** (game hiện tại có server check, dễ nhiễu).

### 5.4 (tuỳ chọn) P1a-as-admin
Nếu muốn đọc thẳng bảng offset velext: chạy `ext_dumpdata.exe` bằng quyền admin
(`Start-Process -Verb RunAs`) → dump `.data`/`.rdata` + in bảng slot ≠ 0xDEADBEEF.
Cần user đồng ý vì là đọc memory process khác (read-only).

---

## 6. Tool đã dựng trong repo

| Tool | Nguồn | Việc |
|------|-------|------|
| `ext_dumpdata.exe` | `src/dumpdata.cpp` | Đọc `.data/.rdata` + syscall id của process đích (read-only). Hiện bị chặn với velext (elevated) |
| `ext_velprobe.exe` | `src/velprobe.cpp` | Liệt kê Humanoid của self + mọi player (WalkSpeed/JumpPower/JumpHeight/HP/|HRP vel|), dump vùng `+0x380..0x3D0`, `--dump` in cửa sổ 0x900 |
| `ext_wssample.exe` | `src/wssample.cpp` | Lấy mẫu @~31ms, in tỉ lệ `0x3A4 != 0x1CC`, `--ctrl` so player khác |
| `ext_wsfast.exe` | `src/wsfast.cpp` | Tight-loop @~229kHz: histogram giá trị `(0x1CC,0x3A4)`, bắt transient + mismatch |
| `ext_watch.exe` | `src/watch.cpp` | Tight-loop @~15–21kHz → CSV ~500Hz: `ws,chk,jp,jh,hp,v,|v|,meas_speed,pos` + bảng bin 250ms |

Build (mingw như `build.bat`):
```
g++ -std=c++17 -O2 -static -o ext_watch.exe src\watch.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi
```
(`ext_dumpdata` cần thêm `-municode`.)

---

## 7. Dữ kiện kỹ thuật cần nhớ

- Offset (version `11f155f3450f4c8e`): `Humanoid.Walkspeed=0x1CC`, `Humanoid+0x3A4` (mirror/check),
  `JumpPower=0x1A0`, `JumpHeight=0x19C`, `UseJumpPower=0x1DD` (bool, hiện =0),
  `Health=0x18C`, `MaxHealth=0x1A4`, `HRP=0x468`, `Primitive.Position=0xD4`,
  `Primitive.AssemblyLinearVelocity=0xE0`, `BasePart.Primitive=0x180`.
- Vùng `+0x3A0`: `0x3A0=0.0f`, `0x3A4=WalkSpeed copy`, `0x3A8=1.0f`, `0x3B0`=cặp float nhỏ
  (0x35dd2080, 0x35dd2080), sau đó là inline string (`,Studio.` tại `0x3C0`).
- Velext: sha256 `0a78225f…d29f94c`, PID 26120, elevated (RPM bị chặn).
- Roblox: PID 25884, base `0x7ff742430000`, chạy qua ExploitStrap path.

---

## 8. Bảng chưa quyết (cần dữ liệu)

| Câu hỏi | Trạng thái | Cách trả lời |
|---------|-----------|--------------|
| velext ghi WalkSpeed bằng property hay velocity? | mở | TN B (5.2) |
| `+0x3A4` là engine mirror hay do writer ghi kèm? | mở | TN C (5.3) |
| Jump hack của velext dùng field nào? | mở | TN A (5.1) với JumpPower ≠ 50 |
| 49.967701 là clamp của game hay giá trị velext? | mở | TN A với slider 37.25 |
| Vì sao 268 khi ghi `0x1CC` (overlay mình) mà velext không bị? | mở | TN C + so cadence/giá trị |
