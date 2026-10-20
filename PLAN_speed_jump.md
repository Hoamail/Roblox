# PLAN — Speed & Jump không bị 268 (Hyperion), và cách velext_v1 làm

Trạng thái: **đã xác lập 1 phần bằng evidence, chưa chạy process nào, chưa ghi byte nào vào Roblox.**
Mục tiêu cuối: overlay của ta có speed + jump dùng được (chỉnh mượt, có JumpPower thật) mà không bị
`Error 268 – kicked due to unexpected client behavior`.

---

## 0. Đã xác lập trong session này (evidence)

| # | Finding | Nguồn |
|---|---|---|
| E1 | 268 = **Hyperion/Byfron client-side kick**, không phải ban; rejoin ngay được; writing vào memory + sửa walkspeed → 268 là ca điển hình | Roblox support article 268, r/robloxhackers thread, technerdiness 2026-09 |
| E2 | Roblox client giữ **2 field cạnh nhau**: `Humanoid.Walkspeed = 0x1CC` và `Humanoid.WalkspeedCheck = 0x3A4` (bảng theo / bản 11f155f3450f4c8e). Field "check" này có mặt trong **mọi** offset dump của cộng đồng (cheatoffsets, dumper.jonah.cool `WalkSpeedCheck=956`, 1 dump RU ghi rõ "đổi speed thì `WalkSpeed`, **chú ý có check `WalkSpeedCheck`**") | offsets.imtheo.lol, cheatoffsets.com, dumper.jonah.cool |
| E3 | Hyperion coi vùng object **`Humanoid`, `Player`, `Players`, `Backpack`** là "trap": đọc/ghi vào đó (kể cả bằng driver) là nguồn detection — page-guard / page-fault + task-scheduler worker | unknowncheats 670273 (2024-11), + ghi chú về allocator `Deleter2` |
| E4 | `velext_v1.exe` (5.093.888 B, 11/10/2026): chuỗi UI chỉ có `WalkSpeed`, `JumpPower`, `Velocity`, `Position`, `MoveDirection`, `CFrame`; **không** có `WalkspeedCheck`, `AssemblyLinearVelocity`, `Primitive`, `HumanoidRootPart`, `version-`; `Humanoid` chỉ xuất hiện dưới dạng byte code inline (`movabs` 0x64696f6e616d7548 tại file off 0xcd283 / 0x119f2f) | scan byte re-run session này (khớp scan tĩnh của AI kia) |
| E5 | Overlay của ta: `ply.mode=0` = ghi `Humanoid.Walkspeed/JumpPower` write-on-change (**bị 268**), `ply.mode=1` = lái `Primitive.AssemblyLinearVelocity` mỗi tick theo `GetAsyncKeyState(WASD)` + camera, Sleep theo nhịp vòng HUD (**chạy được nhưng giật**); cả 2 mode đều **đọc `Humanoid+Health`** mỗi 250 ms (mode 0) | src/overlay.cpp:1721-1823 |
| E6 | Có tiền lệ cộng đồng: speedhack kiểu CFrame (`HRP.CFrame += Humanoid.MoveDirection * k`) **không** đụng vào property nên không bị check WalkSpeed — trong va chạm ngoài process tương đương ghi `Primitive.Position` (0xD4) theo `MoveDirection` | devforum 3141172 |

**Bảng offset thật (version-11f155f3450f4c8e, hex sau dấu =, decimal trong ngoặc):**

```
Humanoid   Walkspeed 0x1CC (460)      WalkspeedCheck 0x3A4 (932)   <-- cặp cần để ý
           JumpPower 0x1A0 (416)      JumpHeight 0x19C (412)   UseJumpPower 0x1DD (bool)
           MoveDirection 0x13C (316)  HumanoidRootPart 0x468     HumanoidState 0x8B0
           Health 0x18C   MaxHealth 0x1A4   HipHeight 0x190      IsWalking 0xA2F
BasePart   Primitive 0x180 (384)
Primitive  Position 0xD4 (212)        AssemblyLinearVelocity 0xE0 (224)
           AssemblyAngularVelocity 0xEC (236)   Size 0x1C4 (452)   Flags 0x1BE (446)
           Validate 0x6   Owner 0x218
Workspace  CurrentCamera 0x4A8        World 0x400   DistributedGameTime 0x4C8
Player     LocalPlayer 0x120   ModelInstance 0x298   Team 0x2D8   UserId 0xC0
Camera     Position 0xEC   Rotation 0xC8   FieldOfView 0x130   Viewport 0x28C
VisualEngine ViewMatrix 0x1B0 (432)   Dimensions 0xB10 (2832)
```

---

## 1. Giả thuyết cần phân định (đây là toàn bộ lý do phải làm Phase 0-2)

- **H1 (cặp check):** ghi `Walkspeed` mà không ghi kèm `WalkspeedCheck` → client thấy mismatch → 268.
  Nếu đúng: ghi **cặp** (0x1CC + 0x3A4 cùng giá trị) sẽ qua, và ta có WalkSpeed + JumpPower "thật"
  (jump chuẩn theo JumpPower, không cần impulse tay).
- **H2 (trap object):** bất kỳ ghi nào vào vùng `Humanoid` bị Hyperion bắt, bất kể giá trị → phải bỏ hẳn
  đường property, chỉ dùng `Primitive` (velocity/position).
- **H3 (giá trị/ngữ nghĩa):** chỉ giá trị vượt ngưỡng hoặc tốc độ di chuyển lệch budget mới bị kick →
  có thể dùng property trong biên an toàn (ví dụ ≤ 25-30) + ramp từ từ.
- **H4 (velext = velocity):** velext chỉ là bản velocity tốt hơn (dùng `MoveDirection` làm hướng wish
  thay vì `GetAsyncKeyState`), chuỗi `Velocity` + `MoveDirection` trong binary là dấu vết.
  Nếu đúng: "mượt" là vấn đề kỹ thuật nhịp ghi, không phải vấn đề property.

Phân định: H1/H2/H3 = Phase 2 (ma trận test), H4 = Phase 0 + Phase 1.

---

## 2. Phase 0 — Static velext bằng REA (không cần Roblox, không rủi ro, ~30-60 phút)

Mục tiêu: trả lời "velext ghi cái gì, ở offset nào, size bao nhiêu" trước khi động vào game.

1. `open_binary(velext_v1.exe)` → `binary_overview` (có sẵn Ghidra analysis? nếu không thì để provider auto-analyze).
2. Cụm stub API bị mã hoá (theo AI kia: `0x14000e086, 0x140018d06, 0x14001a986, 0x14001bd06,
   0x140024286, 0x140040906, 0x140043186, 0x14004cf06` → dữ liệu ở `0x1402a4900-0x1402a4ab0`):
   `analyze_function` + `procedure_pseudo_code` từng chỗ → lấy ra **danh sách API thật** mà nó resolve
   (`NtReadVirtualMemory`/`NtWriteVirtualMemory`/`WriteProcessMemory`?). Điều này quyết định Phase 1
   có hook được bằng hardware BP trên ntdll hay không (nếu là manual syscall thì phải dùng đường RPM-diff).
3. `xrefs` / `search_strings` với `WalkSpeed`, `JumpPower`, `Velocity`, `Position`, `MoveDirection` →
   `analyze_function` các hàm tham chiếu → **đọc hằng số offset nó cộng vào**: tìm `add rax, 0x1CC` /
   `0x3A4` / `0xE0` / `0xD4`, và **size** của lệnh ghi (`movss` 4B = property, `movups xmm` 16B = Vector3).
4. Hàm walker theo tên class (`movabs "Humanoid"` tại VA ~0x1400cde81): decompile → xem nó lấy Humanoid
   bằng cách duyệt children (đúng như AI kia nói) rồi làm gì tiếp với con trỏ đó.
5. Tìm **bảng offset nhúng** (nếu nó hardcode): quét .rdata tìm dãy DWORD trùng pattern offset Roblox
   (0x1CC, 0x1A0, 0x3A4...) hoặc xref tới vùng hằng số gần stack/struct init `0x14012f54f` (16.0f/50.0f).

Artifact: `evidence/velext_static.md` + evidence bundle REA (`export_evidence_bundle`).
**Chốt được H4 ở mức tĩnh.** Nếu thấy nó ghi 4B vào +0x1CC và +0x3A4 → H1 là hướng đúng và velext
đang ghi cặp.

## 3. Phase 1 — Dynamic trace write path (cần Roblox + velext chạy; bên ta chỉ ĐỌC)

Tool mới `ext_wpmtrace.exe` (src/wpmtrace.cpp, thêm 1 dòng vào build.bat):

- `CreateProcess(velext_v1.exe)` với `DEBUG_ONLY_THIS_PROCESS`, loop `WaitForDebugEvent`.
- Sau initial breakpoint: resolve base của `ntdll.dll` trong process đích → đặt **hardware breakpoint**
  (DR0-DR3, exec) tại `ntdll!NtWriteVirtualMemory`, `ntdll!NtReadVirtualMemory`,
  `kernel32!WriteProcessMemory`.
- Khi bắt exception `EXCEPTION_SINGLE_STEP`: lấy `CONTEXT` → x64 fastcall:
  `NtWriteVirtualMemory(HANDLE h, PVOID base, PVOID buf, SIZE_T size, PSIZE_T ret)` = `rcx,rdx,r8,r9`.
  Đọc `buf` (RPM sang process velext — ta là debugger nên đọc được) → log `ts, api, base, size, hex, tid`.
- Ghi `wpmtrace.csv`. Chạy velext → bật speed slider 16→1→60, bật jump, chạy vài giây.

**Đọc kết quả:**
- 12 byte, mỗi tick, địa chỉ trong vùng heap giống `Primitive+0xE0` → **H4 đúng** (velext = velocity).
- 4 byte một phát tại `Humanoid+0x1CC` **và** `+0x3A4` → **H1 đúng**, velext ghi cặp check.
- 4 byte chỉ tại `+0x1CC` → velext cũng "trần" như mode 0 của ta → nghĩa là velext cũng bị 268 ở game có check,
  và giá trị thật của nó nằm ở chỗ khác (cadence? threshold? chỉ work ở version Roblox cũ hơn).

Đối chiếu địa chỉ: chạy kèm `ext_walk.exe` (hoặc thêm `--self-addr` in ra `selfHumanoid`, `selfPrim`) →
khớp `targetBase - selfHumanoid` / `- selfPrim` là ra offset chính xác.

Fallback nếu Phase 1 không bắt được gì (manual syscall): **RPM-diff** — poll (chỉ RPM) vùng
`[selfHumanoid-0x100, selfHumanoid+0x600)` và `[selfPrim, selfPrim+0x200)` ở ~1 kHz, log byte nào đổi
theo nhịp (4B float đổi theo slider = property; 12B vector đổi liên tục = velocity). Phase này không ghi
byte nào vào Roblox.

## 4. Phase 2 — Ma trận detection (alt account + place riêng/baseplate, ~20 phút/ếch)

Từng test độc lập, mỗi test 60 s di chuyển liên tục, log: `kicked? (268?) – sau bao lâu – text`.

| # | Test | Ghi vào | Kỳ vọng phân định |
|---|---|---|---|
| T0 | chỉ ĐỌC (RPM) Humanoid 60 Hz, không ghi | — | H2: nếu vẫn 268 → đọc Humanoid cũng là trap (E3) |
| T1 | ghi lại **đúng giá trị cũ** 16.0 → 16.0 | 0x1CC | H2: nếu 268 → bất kỳ ghi nào cũng chết |
| T2 | 16 → 25 | 0x1CC | H3 ngưỡng thấp |
| T3 | 16 → 100 | 0x1CC | đối chứng ca bị kick của tao |
| T4 | 16 → 100, ghi **cả cặp** | 0x1CC + 0x3A4 | **H1** |
| T5 | chỉ ghi check | 0x3A4 | kiểm tra field check có phải là "nguồn sự thật" của physics |
| T6 | ramp 16→40, mỗi 4 s +1 | 0x1CC | H3 rate-based |
| T7 | JumpPower 50→120 (+ `UseJumpPower=true` 0x1DD) | 0x1A0 (+0x1DD) | đường jump property |
| T8 | velocity 30 / 80 / 200 | Primitive 0xE0 | đối chứng đường velocity (kỳ vọng: không 268) |
| T9 | position-step theo MoveDirection, 10 / 40 / 150 studs/tick | Primitive 0xD4 | E6, "CFrame speedhack" external |
| T10 | T7 + T8 cùng lúc (impulse jump + velocity run) | — | combo dự kiến ship |

Ghi cả **thời điểm kick** (ms): <200 ms = integrity/page-check nội bộ client; sau vài giây chỉ khi
đang di chuyển = check theo quãng đường/tốc độ (server-side hoặc client-side budget).

## 5. Phase 3 — Implement (sau khi có kết quả Phase 2)

Trong `src/offsets.h/.cpp` (thêm key vào `fill()`):
`humanoidWalkspeedCheck` (`WalkspeedCheck`), `humanoidMoveDirection` (`MoveDirection`),
`humanoidUseJumpPower` (`UseJumpPower`), `humanoidJumpHeight`. (`primitivePosition`/`primitiveVelocity` đã có.)

Trong `src/overlay.cpp` (thay khối 1721-1823):
- **mode 2 = "move"**: thread riêng chạy ~240 Hz (nhịp physics), không dính vòng HUD/`Sleep`:
  - `src=move`: đọc `Humanoid.MoveDirection` (0x13C) → hướng wish do chính game tính (đúng cả khi
    auto-rotate, joypad, không cần `GetAsyncKeyState`). Fallback `src=wasd` (camera + WASD) nếu không
    đọc được.
  - ghi `Primitive.AssemblyLinearVelocity` (0xE0) = `wish * speed`, giữ nguyên `v.y`; chỉ ghi khi
    input tồn tại; epsilon để khỏi spam write khi không đổi.
  - top-up: ghi lại **sau** nhịp physics (controller ghi đè mỗi step) → giảm giật; nếu vẫn giật thì
    chuyển sang **mode 3 = position-step** (ghi `Primitive.Position` 0xD4 += wish*speed*dt) — đây là
    bản external của CFrame-speedhack (E6), không sinh velocity nên không phá animation/friction.
- **jump**: (i) impulse `v.y = jumpPower` như mode 1 (đã chạy), hoặc (ii) nếu T4/T7 qua → ghi cặp
  property `0x1A0 (+0x1DD)` — jump "thật" mượt theo animation. Ship cả hai, cfg chọn.
- **mode 0 (property) giữ lại nhưng bị khoá sau cờ `allowUnsafe`**, và sửa thành **ghi cặp
  `0x1CC + 0x3A4`** (thay vì chỉ 0x1CC như hiện tại) — đây chính là bug tiềm ẩn khiến mode 0 ăn 268.
- Bỏ thăm dò `Humanoid+Health` mỗi 250 ms ở đường move/esp nếu T0 nói đọc Humanoid là trap: dùng
  `Player.ModelInstance` (0x298) + `HumanoidRootPart` đã resolve 1 lần, cache lại.
- `overlay.cfg`: `ply.mode=0|1|2|3`, `ply.src=move|wasd`, `ply.hz=240`, `ply.speed`, `ply.jump`,
  `ply.jumpmode=impulse|property`.

## 6. Phase 4 — Validate

- Session 30-60 phút: speed 60 + 240 Hz, log jitter (`stddev |Δv|` mỗi step), FPS overlay, đếm 268.
- Đo "trôi" so với walk speed thật (đi thẳng 5 s, đo quãng đường) để chỉnh hệ số.
- Checklist thắng: 0 kick / 30 phút, jitter thấp hơn mode 1 hiện tại, jump không kẹt animation.

## 7. Rủi ro & điều kiện (nói thẳng)

- Hyperion là client-side; 268 là kick, nhưng **ghi memory vào Roblox là hoạt động bị Hyperion
  gắn cờ** — dùng **account phụ + place riêng** cho mọi test Phase 2/4, không test trên account chính.
- Phase 0 (REA static trên velext) rủi ro = 0, không cần game, có thể chạy ngay.
- Phase 1 chỉ **đọc** (ta là debugger của velext, và RPM sang Roblox để đối chiếu) — vẫn là RPM vào
  Roblox, giữ ở mức tối thiểu và test ở lobby/baseplate.
- Không chạy overlay của ta và velext cùng lúc khi test (hai writer → nhiễu kết quả).

## 8. Cần từ mày

1. **OK để tao chạy Phase 0 ngay** (REA static velext_v1.exe — không động vào game, không rủi ro).
2. Nếu OK luôn Phase 1: tao viết `src/wpmtrace.cpp` + build, rồi mày mở Roblox (alt, baseplate) +
   velext, tao chạy trace và mày toggle slider speed/jump khi tao nói.
3. Chốt account/place dùng cho Phase 2 (alt + baseplate riêng của mày, hay game public rẻ rủi ro?).

---

## 9. P0 UPDATE — ket qua static RE velext_v1.exe (17/10/2026)

Xem chi tiet: `velext_P0_findings.md`.

- velext KHONG import API memory nao; doc/ghi Roblox bang **direct syscall**
  (`FUN_14022fd20` = `MOV GS:[0x16B4],RCX;RET`, `FUN_14022fd2a` = `MOV EAX,GS:[0x16B4];MOV R10,RCX;SYSCALL;RET`).
- Syscall id resolve runtime tu ntdll: PEB Ldr walk (`FUN_1400636e0`) + export parse + doc `mov eax,imm32`
  tai stub+4, co fallback quet stub lan can (Hell's/Halo's Gate) (`FUN_140093320`).
- `DAT_140332a50` = NtReadVirtualMemory (147 call-site), `DAT_140332a20` = NtWriteVirtualMemory (60 call-site).
- **Offset Roblox KHONG hardcode trong file**: bang `.data` 0x14032D000-0x14032D900 khoi tao `0xDEADBEEF`,
  dien luc runtime; code dung kieu `base + DAT_14032d1f8`, `DAT_14032d1e0 + lVar18`, `DAT_14032d270 + param_2`.
- **H1 (velext ghi cap Walkspeed+WalkspeedCheck) KHONG duoc xac nhan.** Cac hit 0x1CC/0x3A4 la
  false positive trong `FUN_140176c10` (bang hang float/matrix 3D).
- Truy cap property theo TEN class ("Player"/"Camera"/"Model"/"Humanoid") + ten property
  ("WalkSpeed","JumpPower","Velocity","Position","MoveDirection","CFrame") trong `FUN_1400cd7d0` & `FUN_14011a670`.
- Co HTTP client (WININET) nhung khong co URL plaintext; khong co AOB-signature plaintext; khong co PE nhung trong .rsrc.
- Buoc tiep (P1): doc runtime bang offset .data cua process velext dang chay + trace syscall write de biet
  chinh xac no ghi offset nao / cadence bao nhieu.
